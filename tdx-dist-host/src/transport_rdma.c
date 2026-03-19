#include "td_transport.h"

#include <errno.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct td_rdma_host_conn {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *region_mr;
    struct td_rdma_host_conn *next;
} td_rdma_host_conn_t;

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

static int td_rdma_host_setup_conn(td_rdma_host_conn_t *conn, td_local_region_t *region, char *err, size_t err_len) {
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
    qp_attr.cap.max_send_wr = 16;
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
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
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
                    (void)rdma_reject(event->id, NULL, 0);
                } else {
                    td_rdma_connect_info_t info;
                    struct rdma_conn_param conn_param;

                    conn->id = event->id;
                    if (td_rdma_host_setup_conn(conn, region, err, err_len) != 0) {
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
