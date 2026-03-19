#ifndef TD_TRANSPORT_H
#define TD_TRANSPORT_H

#include <signal.h>

#include "td_config.h"
#include "td_layout.h"

typedef struct {
    uint64_t magic;
    uint64_t remote_addr;
    uint32_t rkey;
    uint32_t reserved;
    td_region_header_t header;
} td_rdma_connect_info_t;

int td_rdma_host_run(const td_config_t *cfg, td_local_region_t *region, volatile sig_atomic_t *stop_flag, char *err, size_t err_len);

#endif
