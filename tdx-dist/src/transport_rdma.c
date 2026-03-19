#include "td_transport.h"
#include "td_request.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *op_mr;
    struct ibv_mr *recv_mr;
    struct ibv_mr *cas_mr;
    void *workbuf_base;
    size_t workbuf_len;
    int workbuf_fd;
    int use_tdx_shm;
    unsigned char *op_buf;
    unsigned char *recv_buf;
    uint64_t *cas_buf;
    size_t op_buf_len;
    size_t recv_buf_len;
} td_rdma_impl_t;

typedef struct td_rdma_host_conn {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *region_mr;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    void *workbuf_base;
    size_t workbuf_len;
    int workbuf_fd;
    int use_tdx_shm;
    unsigned char *send_buf;
    unsigned char *recv_buf;
    size_t msg_buf_len;
    td_local_region_t *region;
    size_t eviction_threshold_pct;
    int msg_mode;
    struct td_rdma_host_conn *next;
} td_rdma_host_conn_t;

typedef struct {
    uint64_t *poll_cq_ns;
    uint64_t *backoff_ns;
    size_t *empty_polls;
    size_t *backoff_count;
} td_rdma_wait_profile_t;

#define TD_RDMA_MSG_BUF_BYTES 4096u
#define TD_RDMA_WRID_SEND 1ULL
#define TD_RDMA_WRID_RECV 2ULL

int td_tcp_client_connect(td_session_t *session, const td_endpoint_t *endpoint, char *err, size_t err_len);

static int td_rdma_use_tdx_shm(td_tdx_mode_t tdx_mode) {
    return tdx_mode == TD_TDX_ON && access(TD_TDX_SHM_DEVICE, R_OK | W_OK) == 0;
}

static int td_rdma_mr_access(int base_access, td_tdx_mode_t tdx_mode) {
    (void)tdx_mode;
    return base_access;
}

static size_t td_rdma_page_size(void) {
    long value = sysconf(_SC_PAGESIZE);

    return value > 0 ? (size_t)value : 4096u;
}

static void td_rdma_format_memlock(char *buf, size_t buf_len) {
    struct rlimit limit;

    if (buf_len == 0) {
        return;
    }
    if (getrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
        snprintf(buf, buf_len, "unknown");
        return;
    }
    if (limit.rlim_cur == RLIM_INFINITY && limit.rlim_max == RLIM_INFINITY) {
        snprintf(buf, buf_len, "soft=unlimited hard=unlimited");
        return;
    }
    if (limit.rlim_cur == RLIM_INFINITY) {
        snprintf(buf, buf_len, "soft=unlimited hard=%llu", (unsigned long long)limit.rlim_max);
        return;
    }
    if (limit.rlim_max == RLIM_INFINITY) {
        snprintf(buf, buf_len, "soft=%llu hard=unlimited", (unsigned long long)limit.rlim_cur);
        return;
    }
    snprintf(buf, buf_len, "soft=%llu hard=%llu",
        (unsigned long long)limit.rlim_cur,
        (unsigned long long)limit.rlim_max);
}

static void td_rdma_format_mapping(const void *addr, char *buf, size_t buf_len) {
    FILE *maps = NULL;
    unsigned long long target = (unsigned long long)(uintptr_t)addr;
    char line[256];

    if (buf_len == 0) {
        return;
    }

    maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        snprintf(buf, buf_len, "unavailable");
        return;
    }

    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char *newline;

        if (sscanf(line, "%llx-%llx", &start, &end) != 2) {
            continue;
        }
        if (target < start || target >= end) {
            continue;
        }
        newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        snprintf(buf, buf_len, "%s", line);
        fclose(maps);
        return;
    }

    fclose(maps);
    snprintf(buf, buf_len, "not-found");
}

static int td_rdma_try_reg_mr(struct ibv_pd *pd, void *base, size_t bytes, int access, int *saved_errno) {
    struct ibv_mr *mr;

    if (saved_errno != NULL) {
        *saved_errno = 0;
    }
    if (bytes == 0) {
        return 0;
    }

    errno = 0;
    mr = ibv_reg_mr(pd, base, bytes, access);
    if (mr == NULL) {
        if (saved_errno != NULL) {
            *saved_errno = errno;
        }
        return -1;
    }

    ibv_dereg_mr(mr);
    return 0;
}

static int td_rdma_probe_anon_mr(struct ibv_pd *pd, int access, int *saved_errno) {
    size_t bytes = td_rdma_page_size();
    void *base;
    int rc;

    base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        if (saved_errno != NULL) {
            *saved_errno = errno;
        }
        return -1;
    }

    memset(base, 0, bytes);
    rc = td_rdma_try_reg_mr(pd, base, bytes, access, saved_errno);
    munmap(base, bytes);
    return rc;
}

static const char *td_rdma_probe_result(int rc, int saved_errno, char *buf, size_t buf_len) {
    if (rc == 0) {
        snprintf(buf, buf_len, "ok");
    } else if (saved_errno != 0) {
        snprintf(buf, buf_len, "fail(errno=%d:%s)", saved_errno, strerror(saved_errno));
    } else {
        snprintf(buf, buf_len, "fail(errno=0)");
    }
    return buf;
}

static void td_rdma_log_mr_failure(struct ibv_pd *pd, td_local_region_t *region, int access, int reg_errno) {
    char memlock_buf[64];
    char mapping_buf[256];
    char region_probe_buf[64];
    char anon_probe_buf[64];
    size_t probe_bytes = region->mapped_bytes < td_rdma_page_size() ? region->mapped_bytes : td_rdma_page_size();
    int region_probe_errno = 0;
    int anon_probe_errno = 0;
    int region_probe_rc;
    int anon_probe_rc;

    td_rdma_format_memlock(memlock_buf, sizeof(memlock_buf));
    td_rdma_format_mapping(region->base, mapping_buf, sizeof(mapping_buf));
    region_probe_rc = td_rdma_try_reg_mr(pd, region->base, probe_bytes, access, &region_probe_errno);
    anon_probe_rc = td_rdma_probe_anon_mr(pd, access, &anon_probe_errno);

    fprintf(stderr,
        "rdma host mr debug base=%p len=%zu access=0x%x backing=%s errno=%d(%s) memlock=%s\n",
        region->base,
        region->mapped_bytes,
        access,
        region->backing_path,
        reg_errno,
        strerror(reg_errno != 0 ? reg_errno : EIO),
        memlock_buf);
    fprintf(stderr, "rdma host mr debug map=%s\n", mapping_buf);
    fprintf(stderr,
        "rdma host mr debug probe region_%zu=%s anon_%zu=%s\n",
        probe_bytes,
        td_rdma_probe_result(region_probe_rc, region_probe_errno, region_probe_buf, sizeof(region_probe_buf)),
        td_rdma_page_size(),
        td_rdma_probe_result(anon_probe_rc, anon_probe_errno, anon_probe_buf, sizeof(anon_probe_buf)));

    if (strcmp(region->backing_path, TD_TDX_SHM_DEVICE) == 0 && region_probe_rc != 0 && anon_probe_rc == 0) {
        fprintf(stderr,
            "rdma host mr diagnosis: %s mapping is user-visible but not pinnable by ibv_reg_mr; inspect tdx_shm VMA flags and GUP compatibility in the TDX VFIO path\n",
            TD_TDX_SHM_DEVICE);
    } else if (region_probe_rc == 0) {
        fprintf(stderr,
            "rdma host mr diagnosis: a one-page probe registers, so the full %zu-byte MR is likely hitting a size or pin-limit constraint\n",
            region->mapped_bytes);
    }
    fflush(stderr);
}

static uint64_t td_rdma_profile_begin(td_session_t *session) {
    return session->transport_profile != NULL ? td_now_ns() : 0;
}

static void td_rdma_profile_end(td_session_t *session, uint64_t start_ns, uint64_t *field) {
    if (session->transport_profile != NULL && field != NULL && start_ns != 0) {
        *field += td_now_ns() - start_ns;
    }
}

static int td_rdma_wait_event(
    struct rdma_event_channel *ec,
    enum rdma_cm_event_type expected,
    struct rdma_cm_event **out_event,
    char *err,
    size_t err_len) {
    struct rdma_cm_event *event = NULL;

    if (rdma_get_cm_event(ec, &event) != 0) {
        td_format_error(err, err_len, "rdma get cm event failed");
        return -1;
    }
    if (event->event != expected) {
        td_format_error(err, err_len, "unexpected rdma cm event %s(%d)", rdma_event_str(event->event), event->event);
        rdma_ack_cm_event(event);
        return -1;
    }
    *out_event = event;
    return 0;
}

static int td_rdma_poll_wc_on_cq(
    struct ibv_cq *cq,
    struct ibv_wc *wc_out,
    td_rdma_wait_profile_t *profile,
    char *err,
    size_t err_len) {
    for (;;) {
        uint64_t poll_start_ns = profile != NULL && profile->poll_cq_ns != NULL ? td_now_ns() : 0;
        int n = ibv_poll_cq(cq, 1, wc_out);

        if (poll_start_ns != 0) {
            *profile->poll_cq_ns += td_now_ns() - poll_start_ns;
        }
        if (n < 0) {
            td_format_error(err, err_len, "rdma poll cq failed");
            return -1;
        }
        if (n == 0) {
            uint64_t backoff_start_ns = profile != NULL && profile->backoff_ns != NULL ? td_now_ns() : 0;

            if (profile != NULL && profile->empty_polls != NULL) {
                ++(*profile->empty_polls);
            }
            sched_yield();
            if (profile != NULL && profile->backoff_count != NULL) {
                ++(*profile->backoff_count);
            }
            if (backoff_start_ns != 0) {
                *profile->backoff_ns += td_now_ns() - backoff_start_ns;
            }
            continue;
        }
        if (wc_out->status != IBV_WC_SUCCESS) {
            td_format_error(err, err_len, "rdma completion failed status=%d", wc_out->status);
            return -1;
        }
        return 0;
    }
}

static int td_rdma_poll_wc(
    td_rdma_impl_t *impl,
    enum ibv_wc_opcode expected,
    td_rdma_wait_profile_t *profile,
    char *err,
    size_t err_len) {
    struct ibv_wc wc;

    for (;;) {
        if (td_rdma_poll_wc_on_cq(impl->cq, &wc, profile, err, err_len) != 0) {
            return -1;
        }
        if (wc.opcode == expected) {
            return 0;
        }
    }
}

static int td_rdma_post_recv(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id, char *err, size_t err_len) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad_wr = NULL;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(id->qp, &wr, &bad_wr) != 0) {
        td_format_error(err, err_len, "rdma post recv failed");
        return -1;
    }
    return 0;
}

static int td_rdma_post_send_msg(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id, char *err, size_t err_len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    if (ibv_post_send(id->qp, &wr, &bad_wr) != 0) {
        td_format_error(err, err_len, "rdma send failed");
        return -1;
    }
    return 0;
}

static void td_rdma_destroy_impl(td_rdma_impl_t *impl) {
    if (impl->op_mr != NULL) {
        ibv_dereg_mr(impl->op_mr);
    }
    if (impl->recv_mr != NULL) {
        ibv_dereg_mr(impl->recv_mr);
    }
    if (impl->cas_mr != NULL) {
        ibv_dereg_mr(impl->cas_mr);
    }
    if (impl->id != NULL && impl->id->qp != NULL) {
        rdma_destroy_qp(impl->id);
    }
    if (impl->cq != NULL) {
        ibv_destroy_cq(impl->cq);
    }
    if (impl->pd != NULL) {
        ibv_dealloc_pd(impl->pd);
    }
    if (impl->id != NULL) {
        rdma_destroy_id(impl->id);
    }
    if (impl->ec != NULL) {
        rdma_destroy_event_channel(impl->ec);
    }
    if (impl->use_tdx_shm) {
        if (impl->workbuf_base != NULL && impl->workbuf_len > 0) {
            munmap(impl->workbuf_base, impl->workbuf_len);
        }
        if (impl->workbuf_fd >= 0) {
            close(impl->workbuf_fd);
        }
    } else {
        free(impl->workbuf_base);
    }
    memset(impl, 0, sizeof(*impl));
    impl->workbuf_fd = -1;
}

static int td_rdma_setup_impl(td_rdma_impl_t *impl, size_t op_buf_len, td_tdx_mode_t tdx_mode, char *err, size_t err_len) {
    struct ibv_qp_init_attr qp_attr;
    size_t send_bytes = op_buf_len > TD_RDMA_MSG_BUF_BYTES ? op_buf_len : TD_RDMA_MSG_BUF_BYTES;
    size_t cas_offset = (send_bytes + (sizeof(uint64_t) - 1)) & ~(sizeof(uint64_t) - 1);
    size_t recv_offset = (cas_offset + sizeof(uint64_t) + (sizeof(uint64_t) - 1)) & ~(sizeof(uint64_t) - 1);
    char alloc_err[128];

    impl->pd = ibv_alloc_pd(impl->id->verbs);
    impl->cq = ibv_create_cq(impl->id->verbs, 128, NULL, NULL, 0);
    if (impl->pd == NULL || impl->cq == NULL) {
        td_format_error(err, err_len, "rdma alloc pd/cq failed");
        return -1;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = impl->cq;
    qp_attr.recv_cq = impl->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = 4;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(impl->id, impl->pd, &qp_attr) != 0) {
        td_format_error(err, err_len, "rdma create qp failed");
        return -1;
    }

    impl->workbuf_fd = -1;
    impl->workbuf_len = recv_offset + TD_RDMA_MSG_BUF_BYTES;
    impl->op_buf_len = send_bytes;
    impl->recv_buf_len = TD_RDMA_MSG_BUF_BYTES;
    if (td_rdma_use_tdx_shm(tdx_mode)) {
        if (td_tdx_shm_open(impl->workbuf_len, &impl->workbuf_fd, &impl->workbuf_base, alloc_err, sizeof(alloc_err)) != 0) {
            td_format_error(err, err_len, "rdma shared buffer allocation failed: %s", alloc_err);
            return -1;
        }
        impl->use_tdx_shm = 1;
    } else {
        impl->workbuf_base = calloc(1, impl->workbuf_len);
        if (impl->workbuf_base == NULL) {
            td_format_error(err, err_len, "rdma buffer allocation failed");
            return -1;
        }
    }
    impl->op_buf = (unsigned char *)impl->workbuf_base;
    impl->cas_buf = (uint64_t *)((unsigned char *)impl->workbuf_base + cas_offset);
    impl->recv_buf = (unsigned char *)impl->workbuf_base + recv_offset;

    impl->op_mr = ibv_reg_mr(impl->pd, impl->op_buf, impl->op_buf_len, td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    impl->cas_mr = ibv_reg_mr(impl->pd, impl->cas_buf, sizeof(uint64_t), td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    impl->recv_mr = ibv_reg_mr(impl->pd, impl->recv_buf, impl->recv_buf_len, td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    if (impl->op_mr == NULL || impl->cas_mr == NULL || impl->recv_mr == NULL) {
        td_format_error(err, err_len, "rdma mr registration failed");
        return -1;
    }

    return 0;
}

static int td_rdma_client_read(td_session_t *session, size_t offset, void *buf, size_t len, char *err, size_t err_len) {
    td_rdma_impl_t *impl = (td_rdma_impl_t *)session->impl;
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t start_ns;

    if (len > impl->op_buf_len) {
        td_format_error(err, err_len, "rdma read length too large");
        return -1;
    }

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)impl->op_buf;
    sge.length = len;
    sge.lkey = impl->op_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = session->remote_addr + offset;
    wr.wr.rdma.rkey = session->rkey;

    start_ns = td_rdma_profile_begin(session);
    if (ibv_post_send(impl->id->qp, &wr, &bad_wr) != 0) {
        td_format_error(err, err_len, "rdma read op failed");
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->read_send_ns : NULL);
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->rdma_read_post_send_ns : NULL);

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_poll_wc(
            impl,
            IBV_WC_RDMA_READ,
            session->transport_profile != NULL ? &(td_rdma_wait_profile_t){
                .poll_cq_ns = &session->transport_profile->rdma_read_poll_cq_ns,
                .backoff_ns = &session->transport_profile->rdma_read_backoff_ns,
                .empty_polls = &session->transport_profile->rdma_read_empty_polls,
                .backoff_count = &session->transport_profile->rdma_read_backoff_count,
            } : NULL,
            err,
            err_len) != 0) {
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->read_wait_ns : NULL);

    start_ns = td_rdma_profile_begin(session);
    memcpy(buf, impl->op_buf, len);
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->read_copy_ns : NULL);
    return 0;
}

static int td_rdma_client_write(td_session_t *session, size_t offset, const void *buf, size_t len, char *err, size_t err_len) {
    td_rdma_impl_t *impl = (td_rdma_impl_t *)session->impl;
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t start_ns;

    if (len > impl->op_buf_len) {
        td_format_error(err, err_len, "rdma write length too large");
        return -1;
    }

    start_ns = td_rdma_profile_begin(session);
    memcpy(impl->op_buf, buf, len);
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->write_copy_ns : NULL);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)impl->op_buf;
    sge.length = len;
    sge.lkey = impl->op_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = session->remote_addr + offset;
    wr.wr.rdma.rkey = session->rkey;

    start_ns = td_rdma_profile_begin(session);
    if (ibv_post_send(impl->id->qp, &wr, &bad_wr) != 0) {
        td_format_error(err, err_len, "rdma write op failed");
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->write_send_ns : NULL);
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->rdma_write_post_send_ns : NULL);

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_poll_wc(
            impl,
            IBV_WC_RDMA_WRITE,
            session->transport_profile != NULL ? &(td_rdma_wait_profile_t){
                .poll_cq_ns = &session->transport_profile->rdma_write_poll_cq_ns,
                .backoff_ns = &session->transport_profile->rdma_write_backoff_ns,
                .empty_polls = &session->transport_profile->rdma_write_empty_polls,
                .backoff_count = &session->transport_profile->rdma_write_backoff_count,
            } : NULL,
            err,
            err_len) != 0) {
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->write_wait_ns : NULL);
    return 0;
}

static int td_rdma_client_cas(td_session_t *session, size_t offset, uint64_t compare, uint64_t swap, uint64_t *old_value, char *err, size_t err_len) {
    td_rdma_impl_t *impl = (td_rdma_impl_t *)session->impl;
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t start_ns;

    *impl->cas_buf = 0;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)impl->cas_buf;
    sge.length = sizeof(*impl->cas_buf);
    sge.lkey = impl->cas_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = session->remote_addr + offset;
    wr.wr.atomic.rkey = session->rkey;
    wr.wr.atomic.compare_add = compare;
    wr.wr.atomic.swap = swap;

    start_ns = td_rdma_profile_begin(session);
    if (ibv_post_send(impl->id->qp, &wr, &bad_wr) != 0) {
        td_format_error(err, err_len, "rdma atomic cas failed");
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->cas_send_ns : NULL);
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->rdma_atomic_post_send_ns : NULL);

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_poll_wc(
            impl,
            IBV_WC_COMP_SWAP,
            session->transport_profile != NULL ? &(td_rdma_wait_profile_t){
                .poll_cq_ns = &session->transport_profile->rdma_atomic_poll_cq_ns,
                .backoff_ns = &session->transport_profile->rdma_atomic_backoff_ns,
                .empty_polls = &session->transport_profile->rdma_atomic_empty_polls,
                .backoff_count = &session->transport_profile->rdma_atomic_backoff_count,
            } : NULL,
            err,
            err_len) != 0) {
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->cas_wait_ns : NULL);

    *old_value = *impl->cas_buf;
    return 0;
}

static int td_rdma_client_control(td_session_t *session, td_wire_op_t op, char *err, size_t err_len) {
    (void)session;
    (void)op;
    td_format_error(err, err_len, "rdma control ops are not supported in tdx-dist; use request ring");
    return -1;
}

static int td_rdma_client_msg_exchange(
    td_session_t *session,
    const td_wire_msg_t *request,
    const void *payload,
    td_wire_msg_t *response,
    void *response_payload,
    char *err,
    size_t err_len) {
    td_rdma_impl_t *impl = (td_rdma_impl_t *)session->impl;
    struct ibv_wc wc;
    size_t request_bytes = sizeof(*request) + (size_t)request->length;
    size_t max_send_payload = impl->op_buf_len > sizeof(*request) ? impl->op_buf_len - sizeof(*request) : 0;
    size_t max_recv_payload = impl->recv_buf_len > sizeof(*response) ? impl->recv_buf_len - sizeof(*response) : 0;
    size_t recv_bytes = 0;
    int send_done = 0;
    int recv_done = 0;

    if ((size_t)request->length > max_send_payload) {
        td_format_error(err, err_len, "rdma message payload too large");
        return -1;
    }

    memcpy(impl->op_buf, request, sizeof(*request));
    if (payload != NULL && request->length > 0) {
        memcpy(impl->op_buf + sizeof(*request), payload, (size_t)request->length);
    }

    if (td_rdma_post_recv(impl->id, impl->recv_buf, impl->recv_buf_len, impl->recv_mr, TD_RDMA_WRID_RECV, err, err_len) != 0) {
        return -1;
    }
    if (td_rdma_post_send_msg(impl->id, impl->op_buf, request_bytes, impl->op_mr, TD_RDMA_WRID_SEND, err, err_len) != 0) {
        return -1;
    }

    while (!(send_done && recv_done)) {
        if (td_rdma_poll_wc_on_cq(impl->cq, &wc, NULL, err, err_len) != 0) {
            return -1;
        }
        if (wc.wr_id == TD_RDMA_WRID_SEND && wc.opcode == IBV_WC_SEND) {
            send_done = 1;
            continue;
        }
        if (wc.wr_id == TD_RDMA_WRID_RECV && wc.opcode == IBV_WC_RECV) {
            recv_bytes = (size_t)wc.byte_len;
            recv_done = 1;
            continue;
        }
    }

    if (recv_bytes < sizeof(*response)) {
        td_format_error(err, err_len, "rdma message response too short");
        return -1;
    }
    memcpy(response, impl->recv_buf, sizeof(*response));
    if (response->magic != TD_WIRE_MAGIC || response->op != TD_WIRE_ACK) {
        td_format_error(err, err_len, "rdma message response invalid");
        return -1;
    }
    if ((size_t)response->length > max_recv_payload) {
        td_format_error(err, err_len, "rdma message response too large");
        return -1;
    }
    if (response_payload != NULL && response->length > 0) {
        memcpy(response_payload, impl->recv_buf + sizeof(*response), (size_t)response->length);
    }
    return 0;
}

static int td_rdma_client_msg_read(td_session_t *session, size_t offset, void *buf, size_t len, char *err, size_t err_len) {
    td_wire_msg_t request;
    td_wire_msg_t response;
    uint64_t start_ns;

    memset(&request, 0, sizeof(request));
    request.magic = TD_WIRE_MAGIC;
    request.op = TD_WIRE_READ;
    request.offset = offset;
    request.length = len;
    request.flags = session->transport_profile != NULL ? TD_WIRE_FLAG_PROFILE : 0;

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_client_msg_exchange(session, &request, NULL, &response, buf, err, err_len) != 0 || response.status != 0) {
        td_format_error(err, err_len, "rdma read failed for node %d", session->endpoint.node_id);
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->read_wait_ns : NULL);
    return 0;
}

static int td_rdma_client_msg_write(td_session_t *session, size_t offset, const void *buf, size_t len, char *err, size_t err_len) {
    td_wire_msg_t request;
    td_wire_msg_t response;
    uint64_t start_ns;

    memset(&request, 0, sizeof(request));
    request.magic = TD_WIRE_MAGIC;
    request.op = TD_WIRE_WRITE;
    request.offset = offset;
    request.length = len;
    request.flags = session->transport_profile != NULL ? TD_WIRE_FLAG_PROFILE : 0;

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_client_msg_exchange(session, &request, buf, &response, NULL, err, err_len) != 0 || response.status != 0) {
        td_format_error(err, err_len, "rdma write failed for node %d", session->endpoint.node_id);
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->write_wait_ns : NULL);
    return 0;
}

static int td_rdma_client_msg_cas(td_session_t *session, size_t offset, uint64_t compare, uint64_t swap, uint64_t *old_value, char *err, size_t err_len) {
    td_wire_msg_t request;
    td_wire_msg_t response;
    uint64_t start_ns;

    memset(&request, 0, sizeof(request));
    request.magic = TD_WIRE_MAGIC;
    request.op = TD_WIRE_CAS;
    request.offset = offset;
    request.compare = compare;
    request.swap = swap;
    request.flags = session->transport_profile != NULL ? TD_WIRE_FLAG_PROFILE : 0;

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_client_msg_exchange(session, &request, NULL, &response, NULL, err, err_len) != 0 || response.status != 0) {
        td_format_error(err, err_len, "rdma cas failed for node %d", session->endpoint.node_id);
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->cas_wait_ns : NULL);
    if (old_value != NULL) {
        *old_value = response.compare;
    }
    return 0;
}

static int td_rdma_client_msg_control(td_session_t *session, td_wire_op_t op, char *err, size_t err_len) {
    td_wire_msg_t request;
    td_wire_msg_t response;
    uint64_t start_ns;

    memset(&request, 0, sizeof(request));
    request.magic = TD_WIRE_MAGIC;
    request.op = (uint16_t)op;
    request.flags = session->transport_profile != NULL ? TD_WIRE_FLAG_PROFILE : 0;

    start_ns = td_rdma_profile_begin(session);
    if (td_rdma_client_msg_exchange(session, &request, NULL, &response, NULL, err, err_len) != 0 || response.status != 0) {
        td_format_error(err, err_len, "rdma control op %u failed", (unsigned int)op);
        return -1;
    }
    td_rdma_profile_end(session, start_ns, session->transport_profile != NULL ? &session->transport_profile->control_wait_ns : NULL);
    return 0;
}

static void td_rdma_client_close(td_session_t *session) {
    td_rdma_impl_t *impl = (td_rdma_impl_t *)session->impl;

    if (impl == NULL) {
        return;
    }
    if (impl->id != NULL) {
        rdma_disconnect(impl->id);
    }
    td_rdma_destroy_impl(impl);
    free(impl);
    session->impl = NULL;
}

static int td_rdma_client_connect(td_session_t *session, const td_config_t *cfg, const td_endpoint_t *endpoint, char *err, size_t err_len) {
    struct rdma_addrinfo hints;
    struct rdma_addrinfo *res = NULL;
    struct rdma_cm_event *event = NULL;
    struct rdma_conn_param conn_param;
    td_rdma_connect_info_t info;
    td_rdma_impl_t *impl = NULL;
    char port[16];

    snprintf(port, sizeof(port), "%d", endpoint->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;

    if (rdma_getaddrinfo((char *)endpoint->host, port, &hints, &res) != 0) {
        td_format_error(err, err_len, "rdma getaddrinfo failed for %s:%d", endpoint->host, endpoint->port);
        return -1;
    }

    impl = (td_rdma_impl_t *)calloc(1, sizeof(*impl));
    if (impl == NULL) {
        rdma_freeaddrinfo(res);
        td_format_error(err, err_len, "out of memory");
        return -1;
    }

    impl->ec = rdma_create_event_channel();
    if (impl->ec == NULL || rdma_create_id(impl->ec, &impl->id, NULL, RDMA_PS_TCP) != 0) {
        rdma_freeaddrinfo(res);
        td_rdma_destroy_impl(impl);
        free(impl);
        td_format_error(err, err_len, "rdma create id failed");
        return -1;
    }

    if (rdma_resolve_addr(impl->id, NULL, res->ai_dst_addr, 2000) != 0 ||
        td_rdma_wait_event(impl->ec, RDMA_CM_EVENT_ADDR_RESOLVED, &event, err, err_len) != 0) {
        rdma_freeaddrinfo(res);
        td_rdma_destroy_impl(impl);
        free(impl);
        return -1;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(impl->id, 2000) != 0 ||
        td_rdma_wait_event(impl->ec, RDMA_CM_EVENT_ROUTE_RESOLVED, &event, err, err_len) != 0) {
        rdma_freeaddrinfo(res);
        td_rdma_destroy_impl(impl);
        free(impl);
        return -1;
    }
    rdma_ack_cm_event(event);
    rdma_freeaddrinfo(res);

    if (td_rdma_setup_impl(impl, TD_RDMA_MSG_BUF_BYTES, cfg->tdx, err, err_len) != 0) {
        td_rdma_destroy_impl(impl);
        free(impl);
        return -1;
    }

    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 4;
    conn_param.responder_resources = 4;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;

    if (rdma_connect(impl->id, &conn_param) != 0 ||
        td_rdma_wait_event(impl->ec, RDMA_CM_EVENT_ESTABLISHED, &event, err, err_len) != 0) {
        td_rdma_destroy_impl(impl);
        free(impl);
        return -1;
    }

    memset(&info, 0, sizeof(info));
    if (event->param.conn.private_data_len < sizeof(info)) {
        rdma_ack_cm_event(event);
        td_rdma_destroy_impl(impl);
        free(impl);
        td_format_error(err, err_len, "rdma host did not return connect metadata");
        return -1;
    }
    memcpy(&info, event->param.conn.private_data, sizeof(info));
    rdma_ack_cm_event(event);

    if (info.magic != TD_PROJECT_MAGIC) {
        td_rdma_destroy_impl(impl);
        free(impl);
        td_format_error(err, err_len, "rdma host returned invalid metadata");
        return -1;
    }

    session->transport = TD_TRANSPORT_RDMA;
    session->endpoint = *endpoint;
    session->remote_addr = info.remote_addr;
    session->rkey = info.rkey;
    session->transport_flags = info.reserved;
    session->header = info.header;
    session->region_size = (size_t)info.header.region_size;
    session->impl = impl;
    if ((info.reserved & TD_RDMA_CONNECT_FLAG_MSG) != 0) {
        session->read_region = td_rdma_client_msg_read;
        session->write_region = td_rdma_client_msg_write;
        session->cas64 = td_rdma_client_msg_cas;
        session->control = td_rdma_client_msg_control;
    } else {
        session->read_region = td_rdma_client_read;
        session->write_region = td_rdma_client_write;
        session->cas64 = td_rdma_client_cas;
        session->control = td_rdma_client_control;
    }
    session->close = td_rdma_client_close;
    return 0;
}

static void td_rdma_destroy_host_conn(td_rdma_host_conn_t *conn) {
    if (conn->region_mr != NULL) {
        ibv_dereg_mr(conn->region_mr);
    }
    if (conn->send_mr != NULL) {
        ibv_dereg_mr(conn->send_mr);
    }
    if (conn->recv_mr != NULL) {
        ibv_dereg_mr(conn->recv_mr);
    }
    if (conn->id != NULL && conn->id->qp != NULL) {
        rdma_destroy_qp(conn->id);
    }
    if (conn->cq != NULL) {
        ibv_destroy_cq(conn->cq);
    }
    if (conn->pd != NULL) {
        ibv_dealloc_pd(conn->pd);
    }
    if (conn->id != NULL) {
        rdma_destroy_id(conn->id);
    }
    if (conn->use_tdx_shm) {
        if (conn->workbuf_base != NULL && conn->workbuf_len > 0) {
            munmap(conn->workbuf_base, conn->workbuf_len);
        }
        if (conn->workbuf_fd >= 0) {
            close(conn->workbuf_fd);
        }
    } else {
        free(conn->workbuf_base);
    }
    free(conn);
}

static int td_rdma_host_setup_msg_buffers(td_rdma_host_conn_t *conn, td_tdx_mode_t tdx_mode, char *err, size_t err_len) {
    char alloc_err[128];

    conn->workbuf_fd = -1;
    conn->workbuf_len = TD_RDMA_MSG_BUF_BYTES * 2u;
    conn->msg_buf_len = TD_RDMA_MSG_BUF_BYTES;
    if (td_rdma_use_tdx_shm(tdx_mode)) {
        if (td_tdx_shm_open(conn->workbuf_len, &conn->workbuf_fd, &conn->workbuf_base, alloc_err, sizeof(alloc_err)) != 0) {
            td_format_error(err, err_len, "rdma host message buffer allocation failed: %s", alloc_err);
            return -1;
        }
        conn->use_tdx_shm = 1;
    } else {
        conn->workbuf_base = calloc(1, conn->workbuf_len);
        if (conn->workbuf_base == NULL) {
            td_format_error(err, err_len, "rdma host message buffer allocation failed");
            return -1;
        }
    }

    conn->recv_buf = (unsigned char *)conn->workbuf_base;
    conn->send_buf = (unsigned char *)conn->workbuf_base + TD_RDMA_MSG_BUF_BYTES;
    conn->recv_mr = ibv_reg_mr(conn->pd, conn->recv_buf, conn->msg_buf_len, td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    conn->send_mr = ibv_reg_mr(conn->pd, conn->send_buf, conn->msg_buf_len, td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    if (conn->recv_mr == NULL || conn->send_mr == NULL) {
        td_format_error(err, err_len, "rdma host message mr registration failed");
        return -1;
    }
    if (td_rdma_post_recv(conn->id, conn->recv_buf, conn->msg_buf_len, conn->recv_mr, TD_RDMA_WRID_RECV, err, err_len) != 0) {
        return -1;
    }
    return 0;
}

static int td_rdma_host_send_response(td_rdma_host_conn_t *conn, size_t response_bytes, char *err, size_t err_len) {
    return td_rdma_post_send_msg(conn->id, conn->send_buf, response_bytes, conn->send_mr, TD_RDMA_WRID_SEND, err, err_len);
}

static int td_rdma_host_handle_msg_request(td_rdma_host_conn_t *conn, size_t request_bytes, char *err, size_t err_len) {
    td_wire_msg_t *request = (td_wire_msg_t *)conn->recv_buf;
    td_wire_msg_t *response = (td_wire_msg_t *)conn->send_buf;
    unsigned char *payload = conn->recv_buf + sizeof(*request);
    unsigned char *response_payload = conn->send_buf + sizeof(*response);
    size_t max_payload = conn->msg_buf_len > sizeof(*request) ? conn->msg_buf_len - sizeof(*request) : 0;
    size_t response_bytes = sizeof(*response);

    if (request_bytes < sizeof(*request) || request->magic != TD_WIRE_MAGIC || (size_t)request->length > max_payload) {
        td_format_error(err, err_len, "rdma host received invalid message");
        return -1;
    }

    memset(response, 0, sizeof(*response));
    response->magic = TD_WIRE_MAGIC;
    response->op = TD_WIRE_ACK;
    response->flags = request->flags;

    switch (request->op) {
        case TD_WIRE_HELLO:
            response->header = *conn->region->header;
            break;
        case TD_WIRE_READ:
            if (td_region_read_bytes(conn->region, (size_t)request->offset, response_payload, (size_t)request->length) != 0) {
                response->status = 1;
            } else {
                response->length = request->length;
                response_bytes += (size_t)response->length;
            }
            break;
        case TD_WIRE_WRITE:
            if (td_region_write_bytes(conn->region, (size_t)request->offset, payload, (size_t)request->length) != 0) {
                response->status = 1;
            }
            break;
        case TD_WIRE_CAS:
            if (td_region_cas64(conn->region, (size_t)request->offset, request->compare, request->swap, &response->compare) != 0) {
                response->status = 1;
            }
            break;
        case TD_WIRE_EVICT:
            td_region_evict_if_needed(conn->region, conn->eviction_threshold_pct);
            break;
        case TD_WIRE_CLOSE:
            break;
        default:
            response->status = 1;
            break;
    }

    if (td_rdma_host_send_response(conn, response_bytes, err, err_len) != 0) {
        return -1;
    }
    if (td_rdma_post_recv(conn->id, conn->recv_buf, conn->msg_buf_len, conn->recv_mr, TD_RDMA_WRID_RECV, err, err_len) != 0) {
        return -1;
    }
    return 0;
}

static int td_rdma_host_service_conn(td_rdma_host_conn_t *conn, char *err, size_t err_len) {
    struct ibv_wc wc;

    if (!conn->msg_mode) {
        return 0;
    }

    for (;;) {
        int n = ibv_poll_cq(conn->cq, 1, &wc);

        if (n < 0) {
            td_format_error(err, err_len, "rdma host cq poll failed");
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            td_format_error(err, err_len, "rdma host completion failed status=%d", wc.status);
            return -1;
        }
        if (wc.wr_id == TD_RDMA_WRID_RECV && wc.opcode == IBV_WC_RECV) {
            if (td_rdma_host_handle_msg_request(conn, (size_t)wc.byte_len, err, err_len) != 0) {
                return -1;
            }
        }
    }
}

static int td_rdma_host_setup_conn(
    td_rdma_host_conn_t *conn,
    const td_config_t *cfg,
    td_local_region_t *region,
    char *err,
    size_t err_len) {
    struct ibv_qp_init_attr qp_attr;
    int access;

    conn->region = region;
    conn->eviction_threshold_pct = cfg->eviction_threshold_pct;
    conn->msg_mode = (cfg->mode == TD_MODE_MN && cfg->tdx == TD_TDX_ON) ? 1 : 0;
    conn->pd = ibv_alloc_pd(conn->id->verbs);
    conn->cq = ibv_create_cq(conn->id->verbs, 128, NULL, NULL, 0);
    if (conn->pd == NULL || conn->cq == NULL) {
        td_format_error(err, err_len, "rdma host alloc pd/cq failed");
        return -1;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = conn->cq;
    qp_attr.recv_cq = conn->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = conn->msg_mode ? 4 : 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(conn->id, conn->pd, &qp_attr) != 0) {
        td_format_error(err, err_len, "rdma host create qp failed");
        return -1;
    }

    if (conn->msg_mode) {
        return td_rdma_host_setup_msg_buffers(conn, cfg->tdx, err, err_len);
    }

    access = td_rdma_mr_access(
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC,
        cfg->tdx);
    conn->region_mr = ibv_reg_mr(
        conn->pd,
        region->base,
        region->mapped_bytes,
        access);
    if (conn->region_mr == NULL) {
        int reg_errno = errno;

        td_rdma_log_mr_failure(conn->pd, region, access, reg_errno);
        td_format_error(err, err_len, "rdma host ibv_reg_mr failed: %s", strerror(reg_errno != 0 ? reg_errno : EIO));
        return -1;
    }
    return 0;
}

int td_rdma_host_run(const td_config_t *cfg, td_local_region_t *region, volatile sig_atomic_t *stop_flag, char *err, size_t err_len) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listen_id = NULL;
    struct rdma_addrinfo hints;
    struct rdma_addrinfo *res = NULL;
    td_rdma_host_conn_t *connections = NULL;
    char port[16];

    err[0] = '\0';
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = RAI_PASSIVE;
    hints.ai_port_space = RDMA_PS_TCP;
    snprintf(port, sizeof(port), "%d", cfg->listen_port);

    if (rdma_getaddrinfo((char *)cfg->listen_host, port, &hints, &res) != 0) {
        td_format_error(err, err_len, "rdma getaddrinfo failed on %s:%d", cfg->listen_host, cfg->listen_port);
        return -1;
    }

    ec = rdma_create_event_channel();
    if (ec == NULL || rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP) != 0 ||
        rdma_bind_addr(listen_id, res->ai_src_addr) != 0 || rdma_listen(listen_id, 16) != 0) {
        rdma_freeaddrinfo(res);
        if (listen_id != NULL) {
            rdma_destroy_id(listen_id);
        }
        if (ec != NULL) {
            rdma_destroy_event_channel(ec);
        }
        td_format_error(err, err_len, "rdma host listen setup failed");
        return -1;
    }
    rdma_freeaddrinfo(res);

    while (!(*stop_flag)) {
        struct pollfd pfd;
        int ready;
        td_rdma_host_conn_t *iter;

        pfd.fd = ec->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 10);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            td_format_error(err, err_len, "rdma host event poll failed");
            break;
        }
        if (ready == 0 || (pfd.revents & POLLIN) == 0) {
            for (iter = connections; iter != NULL; iter = iter->next) {
                if (td_rdma_host_service_conn(iter, err, err_len) != 0) {
                    break;
                }
            }
            if (err[0] != '\0') {
                break;
            }
            continue;
        }

        {
            struct rdma_cm_event *event = NULL;
            td_rdma_host_conn_t *dispose_conn = NULL;

            if (rdma_get_cm_event(ec, &event) != 0) {
                continue;
            }

            if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                td_rdma_host_conn_t *conn = (td_rdma_host_conn_t *)calloc(1, sizeof(*conn));

                if (conn == NULL) {
                    fprintf(stderr, "rdma host rejected connect request: out of memory\n");
                    (void)rdma_reject(event->id, NULL, 0);
                } else {
                    td_rdma_connect_info_t info;
                    struct rdma_conn_param conn_param;

                    conn->id = event->id;
                    if (td_rdma_host_setup_conn(conn, cfg, region, err, err_len) != 0) {
                        fprintf(stderr, "rdma host rejected connect request: %s\n", err);
                        fflush(stderr);
                        (void)rdma_reject(event->id, NULL, 0);
                        dispose_conn = conn;
                    } else {
                        memset(&info, 0, sizeof(info));
                        info.magic = TD_PROJECT_MAGIC;
                        info.remote_addr = conn->region_mr != NULL ? (uint64_t)(uintptr_t)region->base : 0;
                        info.rkey = conn->region_mr != NULL ? conn->region_mr->rkey : 0;
                        info.reserved = conn->msg_mode ? TD_RDMA_CONNECT_FLAG_MSG : 0;
                        info.header = *region->header;

                        memset(&conn_param, 0, sizeof(conn_param));
                        conn_param.initiator_depth = 4;
                        conn_param.responder_resources = 4;
                        conn_param.retry_count = 7;
                        conn_param.rnr_retry_count = 7;
                        conn_param.private_data = &info;
                        conn_param.private_data_len = sizeof(info);

                        if (rdma_accept(event->id, &conn_param) != 0) {
                            td_format_error(err, err_len, "rdma host accept failed: %s", strerror(errno));
                            fprintf(stderr, "%s\n", err);
                            fflush(stderr);
                            dispose_conn = conn;
                        } else {
                            conn->next = connections;
                            connections = conn;
                            conn = NULL;
                        }
                    }
                }
            } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
                td_rdma_host_conn_t **cursor = &connections;

                while (*cursor != NULL) {
                    if ((*cursor)->id == event->id) {
                        dispose_conn = *cursor;
                        *cursor = dispose_conn->next;
                        break;
                    }
                    cursor = &(*cursor)->next;
                }
            }

            rdma_ack_cm_event(event);
            if (dispose_conn != NULL) {
                td_rdma_destroy_host_conn(dispose_conn);
                err[0] = '\0';
            }
        }

        for (iter = connections; iter != NULL; iter = iter->next) {
            if (td_rdma_host_service_conn(iter, err, err_len) != 0) {
                break;
            }
        }
        if (err[0] != '\0') {
            break;
        }
    }

    while (connections != NULL) {
        td_rdma_host_conn_t *next = connections->next;

        if (connections->id != NULL) {
            rdma_disconnect(connections->id);
        }
        td_rdma_destroy_host_conn(connections);
        connections = next;
    }
    if (listen_id != NULL) {
        rdma_destroy_id(listen_id);
    }
    if (ec != NULL) {
        rdma_destroy_event_channel(ec);
    }
    return err[0] == '\0' ? 0 : -1;
}

int td_rdma_server_run(const td_config_t *cfg, td_local_region_t *region, volatile sig_atomic_t *stop_flag, char *err, size_t err_len) {
    return td_rdma_host_run(cfg, region, stop_flag, err, err_len);
}

int td_session_connect(td_session_t *session, const td_config_t *cfg, const td_endpoint_t *endpoint, char *err, size_t err_len) {
    memset(session, 0, sizeof(*session));
    if (cfg->transport == TD_TRANSPORT_TCP) {
        return td_tcp_client_connect(session, endpoint, err, err_len);
    }
    return td_rdma_client_connect(session, cfg, endpoint, err, err_len);
}

void td_session_close(td_session_t *session) {
    if (session->close != NULL) {
        session->close(session);
    }
    memset(session, 0, sizeof(*session));
}
