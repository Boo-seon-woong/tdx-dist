#include "td_transport.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t td_stop = 0;

static void td_format_bytes(char *buf, size_t buf_len, size_t bytes) {
    const unsigned long long gb = 1024ULL * 1024ULL * 1024ULL;
    const unsigned long long mb = 1024ULL * 1024ULL;
    const unsigned long long kb = 1024ULL;
    unsigned long long value = (unsigned long long)bytes;

    if (value % gb == 0) {
        snprintf(buf, buf_len, "%lluGB", value / gb);
    } else if (value % mb == 0) {
        snprintf(buf, buf_len, "%lluMB", value / mb);
    } else if (value % kb == 0) {
        snprintf(buf, buf_len, "%lluKB", value / kb);
    } else {
        snprintf(buf, buf_len, "%lluB", value);
    }
}

static int td_find_config(int argc, char **argv, const char **path) {
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            *path = argv[i + 1];
            return 0;
        }
    }
    return -1;
}

static void td_signal_handler(int signo) {
    (void)signo;
    td_stop = 1;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    td_config_t cfg;
    td_local_region_t region;
    char err[256];
    char total_buf[32];
    char ring_buf[32];

    if (td_find_config(argc, argv, &config_path) != 0) {
        fprintf(stderr, "usage: %s --config build/config/host.rdma.conf\n", argv[0]);
        return 1;
    }
    if (td_config_load(config_path, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 1;
    }
    if (cfg.mode != TD_MODE_HOST) {
        fprintf(stderr, "config error: mode must be host\n");
        return 1;
    }
    if (cfg.transport != TD_TRANSPORT_RDMA) {
        fprintf(stderr, "config error: host transport must be rdma\n");
        return 1;
    }
    if (td_region_open(&region, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "region error: %s\n", err);
        return 1;
    }

    signal(SIGINT, td_signal_handler);
    signal(SIGTERM, td_signal_handler);

    td_format_bytes(total_buf, sizeof(total_buf), region.mapped_bytes);
    td_format_bytes(ring_buf, sizeof(ring_buf), (size_t)region.header->request_ring_bytes);
    fprintf(stdout,
        "tdx-dist host listen=%s:%d backing=%s total=%s request_slots=%llu ring=%s slot_offset=%llu\n",
        cfg.listen_host,
        cfg.listen_port,
        region.backing_path,
        total_buf,
        (unsigned long long)region.header->request_capacity,
        ring_buf,
        (unsigned long long)region.header->slot_region_offset);
    fflush(stdout);

    if (td_rdma_host_run(&cfg, &region, &td_stop, err, sizeof(err)) != 0) {
        fprintf(stderr, "host error: %s\n", err);
        td_region_close(&region);
        return 1;
    }

    td_region_close(&region);
    return 0;
}
