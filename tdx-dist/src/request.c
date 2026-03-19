#include "td_request.h"

#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static td_request_entry_t *td_local_request_entry(td_local_region_t *region, uint64_t seq) {
    return (td_request_entry_t *)((unsigned char *)region->base + td_request_ring_entry_offset(region->header, seq));
}

static int td_remote_read_u64(td_session_t *session, size_t offset, uint64_t *value, char *err, size_t err_len) {
    return session->read_region(session, offset, value, sizeof(*value), err, err_len);
}

static int td_remote_reserve_seq(td_session_t *session, uint64_t *seq_out, char *err, size_t err_len) {
    const size_t reserve_offset = (size_t)session->header.request_ring_offset + offsetof(td_request_ring_t, reserve_head);
    const size_t tail_offset = (size_t)session->header.request_ring_offset + offsetof(td_request_ring_t, tail);

    for (;;) {
        uint64_t reserve_head = 0;
        uint64_t tail = 0;
        uint64_t observed = 0;

        if (td_remote_read_u64(session, reserve_offset, &reserve_head, err, err_len) != 0 ||
            td_remote_read_u64(session, tail_offset, &tail, err, err_len) != 0) {
            return -1;
        }
        if ((reserve_head - tail) >= session->header.request_capacity) {
            if (session->transport_profile != NULL) {
                ++session->transport_profile->rdma_request_ring_full;
            }
            sched_yield();
            continue;
        }
        if (session->cas64(session, reserve_offset, reserve_head, reserve_head + 1, &observed, err, err_len) != 0) {
            return -1;
        }
        if (observed == reserve_head) {
            *seq_out = reserve_head;
            return 0;
        }
    }
}

static int td_remote_publish_seq(td_session_t *session, uint64_t seq, char *err, size_t err_len) {
    const size_t head_offset = (size_t)session->header.request_ring_offset + offsetof(td_request_ring_t, head);

    for (;;) {
        uint64_t observed = 0;

        if (session->cas64(session, head_offset, seq, seq + 1, &observed, err, err_len) != 0) {
            return -1;
        }
        if (observed == seq) {
            return 0;
        }
        if (session->transport_profile != NULL) {
            ++session->transport_profile->rdma_request_publish_spins;
        }
        sched_yield();
    }
}

static int td_remote_submit_request(td_session_t *session, td_request_entry_t *request, td_request_result_t *result, char *err, size_t err_len) {
    td_request_entry_t observed;
    size_t entry_offset;
    uint64_t seq;
    uint64_t start_ns;

    start_ns = session->transport_profile != NULL ? td_now_ns() : 0;
    if (td_remote_reserve_seq(session, &seq, err, err_len) != 0) {
        return -1;
    }
    if (session->transport_profile != NULL && start_ns != 0) {
        session->transport_profile->rdma_request_reserve_ns += td_now_ns() - start_ns;
    }

    request->seq = seq;
    request->state = TD_REQ_STATE_READY;
    entry_offset = td_request_ring_entry_offset(&session->header, seq);

    start_ns = session->transport_profile != NULL ? td_now_ns() : 0;
    if (session->write_region(session, entry_offset, request, sizeof(*request), err, err_len) != 0) {
        return -1;
    }
    if (session->transport_profile != NULL && start_ns != 0) {
        session->transport_profile->rdma_request_payload_write_ns += td_now_ns() - start_ns;
    }

    atomic_thread_fence(memory_order_release);

    start_ns = session->transport_profile != NULL ? td_now_ns() : 0;
    if (td_remote_publish_seq(session, seq, err, err_len) != 0) {
        return -1;
    }
    if (session->transport_profile != NULL && start_ns != 0) {
        session->transport_profile->rdma_request_publish_ns += td_now_ns() - start_ns;
    }

    start_ns = session->transport_profile != NULL ? td_now_ns() : 0;
    for (;;) {
        if (session->read_region(session, entry_offset, &observed, sizeof(observed), err, err_len) != 0) {
            return -1;
        }
        if (observed.seq != seq) {
            td_format_error(err, err_len, "request ring slot reused before completion");
            return -1;
        }
        if (observed.state == TD_REQ_STATE_DONE) {
            break;
        }
        sched_yield();
    }
    if (session->transport_profile != NULL && start_ns != 0) {
        session->transport_profile->rdma_request_completion_wait_ns += td_now_ns() - start_ns;
    }

    if (result != NULL) {
        result->status = (int)observed.status;
        result->observed_epoch = observed.observed_epoch;
        result->observed_tie_breaker = observed.observed_tie_breaker;
    }
    return 0;
}

static int td_local_commit_slot(
    td_local_region_t *region,
    td_region_kind_t kind,
    size_t slot_index,
    const td_slot_t *proposal,
    uint64_t compare_epoch,
    uint64_t *observed_epoch,
    uint64_t *observed_tie_breaker) {
    td_slot_t *slot;
    int rc = 1;

    if (slot_index >= td_region_kind_slot_count(region->header, kind)) {
        return -1;
    }

    pthread_mutex_lock(&region->lock);
    slot = td_region_slot_ptr(region, kind, slot_index);
    *observed_epoch = slot->guard_epoch;
    *observed_tie_breaker = slot->tie_breaker;
    if (slot->guard_epoch == compare_epoch) {
        memcpy(
            (unsigned char *)slot + offsetof(td_slot_t, visible_epoch),
            (const unsigned char *)proposal + offsetof(td_slot_t, visible_epoch),
            sizeof(td_slot_t) - offsetof(td_slot_t, visible_epoch));
        atomic_thread_fence(memory_order_release);
        slot->guard_epoch = proposal->visible_epoch;
        *observed_epoch = compare_epoch;
        *observed_tie_breaker = proposal->tie_breaker;
        rc = 0;
    }
    pthread_mutex_unlock(&region->lock);
    return rc;
}

size_t td_request_ring_bytes_for_slots(size_t request_slots) {
    return sizeof(td_request_ring_t) + (request_slots * sizeof(td_request_entry_t));
}

size_t td_request_ring_entry_offset(const td_region_header_t *header, uint64_t seq) {
    size_t index = (size_t)(seq % header->request_capacity);

    return (size_t)header->request_ring_offset + sizeof(td_request_ring_t) + (index * header->request_entry_size);
}

int td_remote_commit_request(
    td_session_t *session,
    td_region_kind_t kind,
    size_t slot_index,
    const td_slot_t *slot,
    uint64_t compare_epoch,
    td_request_result_t *result,
    char *err,
    size_t err_len) {
    td_request_entry_t request;

    memset(&request, 0, sizeof(request));
    request.op = TD_REQ_COMMIT_SLOT;
    request.region_kind = (uint32_t)kind;
    request.compare_epoch = compare_epoch;
    request.slot_index = slot_index;
    request.slot = *slot;
    return td_remote_submit_request(session, &request, result, err, err_len);
}

int td_remote_evict_request(td_session_t *session, td_request_result_t *result, char *err, size_t err_len) {
    td_request_entry_t request;

    memset(&request, 0, sizeof(request));
    request.op = TD_REQ_EVICT_CACHE;
    return td_remote_submit_request(session, &request, result, err, err_len);
}

int td_request_consume_once(td_local_region_t *region, size_t eviction_threshold_pct, char *err, size_t err_len) {
    td_request_ring_t *ring = td_region_request_ring_ptr(region);
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    td_request_entry_t *entry;
    int rc = 0;

    if (tail >= head) {
        return 0;
    }

    entry = td_local_request_entry(region, tail);
    if (__atomic_load_n(&entry->state, __ATOMIC_ACQUIRE) != TD_REQ_STATE_READY || entry->seq != tail) {
        return 0;
    }

    if (entry->op == TD_REQ_COMMIT_SLOT) {
        rc = td_local_commit_slot(
            region,
            (td_region_kind_t)entry->region_kind,
            (size_t)entry->slot_index,
            &entry->slot,
            entry->compare_epoch,
            &entry->observed_epoch,
            &entry->observed_tie_breaker);
        if (rc < 0) {
            td_format_error(err, err_len, "invalid commit request seq=%llu", (unsigned long long)tail);
            return -1;
        }
    } else if (entry->op == TD_REQ_EVICT_CACHE) {
        td_region_evict_if_needed(region, eviction_threshold_pct);
        rc = 0;
    } else {
        td_format_error(err, err_len, "unknown request op %u", entry->op);
        return -1;
    }

    entry->status = (uint32_t)rc;
    atomic_thread_fence(memory_order_release);
    __atomic_store_n(&entry->state, TD_REQ_STATE_DONE, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}
