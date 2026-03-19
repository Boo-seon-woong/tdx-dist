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
#include <unistd.h>

typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *op_mr;
    struct ibv_mr *cas_mr;
    unsigned char *op_buf;
    uint64_t *cas_buf;
    size_t op_buf_len;
} td_rdma_impl_t;

typedef struct td_rdma_host_conn {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *region_mr;
    struct td_rdma_host_conn *next;
} td_rdma_host_conn_t;

typedef struct {
    uint64_t *poll_cq_ns;
    uint64_t *backoff_ns;
    size_t *empty_polls;
    size_t *backoff_count;
} td_rdma_wait_profile_t;

int td_tcp_client_connect(td_session_t *session, const td_endpoint_t *endpoint, char *err, size_t err_len);

static int td_rdma_mr_access(int base_access, td_tdx_mode_t tdx_mode) {
#ifdef IBV_ACCESS_ON_DEMAND
    if (tdx_mode == TD_TDX_ON) {
        return base_access | IBV_ACCESS_ON_DEMAND;
    }
#endif
    return base_access;
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

static int td_rdma_poll_wc(
    td_rdma_impl_t *impl,
    enum ibv_wc_opcode expected,
    td_rdma_wait_profile_t *profile,
    char *err,
    size_t err_len) {
    struct ibv_wc wc;

    for (;;) {
        uint64_t poll_start_ns = profile != NULL && profile->poll_cq_ns != NULL ? td_now_ns() : 0;
        int n = ibv_poll_cq(impl->cq, 1, &wc);

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
        if (wc.status != IBV_WC_SUCCESS) {
            td_format_error(err, err_len, "rdma completion failed status=%d", wc.status);
            return -1;
        }
        if (wc.opcode == expected) {
            return 0;
        }
    }
}

static void td_rdma_destroy_impl(td_rdma_impl_t *impl) {
    if (impl->op_mr != NULL) {
        ibv_dereg_mr(impl->op_mr);
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
    free(impl->op_buf);
    free(impl->cas_buf);
    memset(impl, 0, sizeof(*impl));
}

static int td_rdma_setup_impl(td_rdma_impl_t *impl, size_t op_buf_len, td_tdx_mode_t tdx_mode, char *err, size_t err_len) {
    struct ibv_qp_init_attr qp_attr;

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
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(impl->id, impl->pd, &qp_attr) != 0) {
        td_format_error(err, err_len, "rdma create qp failed");
        return -1;
    }

    impl->op_buf = (unsigned char *)calloc(1, op_buf_len);
    impl->cas_buf = (uint64_t *)calloc(1, sizeof(uint64_t));
    impl->op_buf_len = op_buf_len;
    if (impl->op_buf == NULL || impl->cas_buf == NULL) {
        td_format_error(err, err_len, "rdma buffer allocation failed");
        return -1;
    }

    impl->op_mr = ibv_reg_mr(impl->pd, impl->op_buf, impl->op_buf_len, td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    impl->cas_mr = ibv_reg_mr(impl->pd, impl->cas_buf, sizeof(uint64_t), td_rdma_mr_access(IBV_ACCESS_LOCAL_WRITE, tdx_mode));
    if (impl->op_mr == NULL || impl->cas_mr == NULL) {
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

    if (td_rdma_setup_impl(impl, sizeof(td_request_entry_t), cfg->tdx, err, err_len) != 0) {
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
    session->header = info.header;
    session->region_size = (size_t)info.header.region_size;
    session->impl = impl;
    session->read_region = td_rdma_client_read;
    session->write_region = td_rdma_client_write;
    session->cas64 = td_rdma_client_cas;
    session->control = td_rdma_client_control;
    session->close = td_rdma_client_close;
    return 0;
}

static void td_rdma_destroy_host_conn(td_rdma_host_conn_t *conn) {
    if (conn->region_mr != NULL) {
        ibv_dereg_mr(conn->region_mr);
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
    free(conn);
}

static int td_rdma_host_setup_conn(
    td_rdma_host_conn_t *conn,
    const td_config_t *cfg,
    td_local_region_t *region,
    char *err,
    size_t err_len) {
    struct ibv_qp_init_attr qp_attr;

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
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(conn->id, conn->pd, &qp_attr) != 0) {
        td_format_error(err, err_len, "rdma host create qp failed");
        return -1;
    }

    conn->region_mr = ibv_reg_mr(
        conn->pd,
        region->base,
        region->mapped_bytes,
        td_rdma_mr_access(
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC,
            cfg->tdx));
    if (conn->region_mr == NULL) {
        td_format_error(err, err_len, "rdma host ibv_reg_mr failed");
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

        pfd.fd = ec->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            td_format_error(err, err_len, "rdma host event poll failed");
            break;
        }
        if (ready == 0 || (pfd.revents & POLLIN) == 0) {
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
                        info.remote_addr = (uint64_t)(uintptr_t)region->base;
                        info.rkey = conn->region_mr->rkey;
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
