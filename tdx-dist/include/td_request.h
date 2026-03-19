#ifndef TD_REQUEST_H
#define TD_REQUEST_H

#include "td_transport.h"

typedef enum {
    TD_REQ_STATE_FREE = 0,
    TD_REQ_STATE_READY = 1,
    TD_REQ_STATE_DONE = 2,
} td_request_state_t;

typedef enum {
    TD_REQ_COMMIT_SLOT = 1,
    TD_REQ_EVICT_CACHE = 2,
} td_request_op_t;

typedef struct {
    uint64_t seq;
    uint32_t op;
    uint32_t state;
    uint32_t region_kind;
    uint32_t status;
    uint64_t compare_epoch;
    uint64_t observed_epoch;
    uint64_t observed_tie_breaker;
    uint64_t slot_index;
    td_slot_t slot;
} td_request_entry_t;

typedef struct {
    int status;
    uint64_t observed_epoch;
    uint64_t observed_tie_breaker;
} td_request_result_t;

size_t td_request_ring_bytes_for_slots(size_t request_slots);
size_t td_request_ring_entry_offset(const td_region_header_t *header, uint64_t seq);
int td_remote_commit_request(
    td_session_t *session,
    td_region_kind_t kind,
    size_t slot_index,
    const td_slot_t *slot,
    uint64_t compare_epoch,
    td_request_result_t *result,
    char *err,
    size_t err_len);
int td_remote_evict_request(td_session_t *session, td_request_result_t *result, char *err, size_t err_len);
int td_request_consume_once(td_local_region_t *region, size_t eviction_threshold_pct, char *err, size_t err_len);

#endif
