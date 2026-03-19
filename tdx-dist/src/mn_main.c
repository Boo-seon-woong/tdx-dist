#include "td_transport.h"
#include "td_request.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t td_stop = 0;

typedef struct {
    td_local_region_t *region;
    size_t threshold_pct;
} td_eviction_ctx_t;

typedef struct {
    const td_config_t *cfg;
    td_local_region_t *region;
    volatile sig_atomic_t *stop_flag;
    char err[256];
    int rc;
} td_rdma_host_ctx_t;

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

static const char *td_slot_layout_mode(const td_config_t *cfg) {
    int explicit_count = 0;

    explicit_count += cfg->prime_slots_explicit ? 1 : 0;
    explicit_count += cfg->cache_slots_explicit ? 1 : 0;
    explicit_count += cfg->backup_slots_explicit ? 1 : 0;
    if (explicit_count == 0) {
        return "auto";
    }
    if (explicit_count == 3) {
        return "manual";
    }
    return "mixed";
}

static void td_print_memory_layout(const td_config_t *cfg, const td_local_region_t *region) {
    char total_buf[32];
    char header_buf[32];
    char ring_buf[32];
    char prime_buf[32];
    char cache_buf[32];
    char backup_buf[32];
    char used_buf[32];
    char unused_buf[32];
    size_t header_bytes = sizeof(td_region_header_t);
    size_t ring_bytes = (size_t)region->header->request_ring_bytes;
    size_t slot_bytes = sizeof(td_slot_t);
    size_t prime_bytes = cfg->prime_slots * slot_bytes;
    size_t cache_bytes = cfg->cache_slots * slot_bytes;
    size_t backup_bytes = cfg->backup_slots * slot_bytes;
    size_t used_bytes = header_bytes + ring_bytes + prime_bytes + cache_bytes + backup_bytes;
    size_t unused_bytes = region->mapped_bytes > used_bytes ? region->mapped_bytes - used_bytes : 0;

    td_format_bytes(total_buf, sizeof(total_buf), region->mapped_bytes);
    td_format_bytes(header_buf, sizeof(header_buf), header_bytes);
    td_format_bytes(ring_buf, sizeof(ring_buf), ring_bytes);
    td_format_bytes(prime_buf, sizeof(prime_buf), prime_bytes);
    td_format_bytes(cache_buf, sizeof(cache_buf), cache_bytes);
    td_format_bytes(backup_buf, sizeof(backup_buf), backup_bytes);
    td_format_bytes(used_buf, sizeof(used_buf), used_bytes);
    td_format_bytes(unused_buf, sizeof(unused_buf), unused_bytes);

    fprintf(stdout,
        "tdx-dist mn layout mode=%s total=%s(%llu) header=%s ring=%llu:%zuB prime=%zu:%s cache=%zu:%s backup=%zu:%s used=%s unused=%s slot_bytes=%zu\n",
        td_slot_layout_mode(cfg),
        total_buf,
        (unsigned long long)region->mapped_bytes,
        header_buf,
        (unsigned long long)region->header->request_capacity,
        ring_bytes,
        cfg->prime_slots,
        prime_buf,
        cfg->cache_slots,
        cache_buf,
        cfg->backup_slots,
        backup_buf,
        used_buf,
        unused_buf,
        slot_bytes);
}

static void td_signal_handler(int signo) {
    (void)signo;
    td_stop = 1;
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

static int td_request_server_run(td_local_region_t *region, volatile sig_atomic_t *stop_flag, size_t eviction_threshold_pct, char *err, size_t err_len) {
    while (!(*stop_flag)) {
        int rc = td_request_consume_once(region, eviction_threshold_pct, err, err_len);

        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            usleep(100);
        }
    }
    return 0;
}

static void *td_eviction_thread(void *arg) {
    td_eviction_ctx_t *ctx = (td_eviction_ctx_t *)arg;
    while (!td_stop) {
        td_region_evict_if_needed(ctx->region, ctx->threshold_pct);
        usleep(100000);
    }
    return NULL;
}

static void *td_rdma_host_thread(void *arg) {
    td_rdma_host_ctx_t *ctx = (td_rdma_host_ctx_t *)arg;
    ctx->rc = td_rdma_host_run(ctx->cfg, ctx->region, ctx->stop_flag, ctx->err, sizeof(ctx->err));
    return NULL;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    td_config_t cfg;
    td_local_region_t region;
    td_eviction_ctx_t eviction_ctx;
    td_rdma_host_ctx_t rdma_host_ctx;
    char err[256];
    pthread_t evict_thread;
    pthread_t rdma_host_thread_id;
    int have_rdma_host_thread = 0;
    int rc;

    if (td_find_config(argc, argv, &config_path) != 0) {
        fprintf(stderr, "usage: %s --config build/config/mn1.conf\n", argv[0]);
        return 1;
    }
    if (td_config_load(config_path, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 1;
    }
    if (cfg.mode != TD_MODE_MN) {
        fprintf(stderr, "config error: mode must be mn\n");
        return 1;
    }
    if (td_region_open(&region, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "region error: %s\n", err);
        return 1;
    }

    signal(SIGINT, td_signal_handler);
    signal(SIGTERM, td_signal_handler);
    eviction_ctx.region = &region;
    eviction_ctx.threshold_pct = cfg.eviction_threshold_pct;
    pthread_create(&evict_thread, NULL, td_eviction_thread, &eviction_ctx);

    if (cfg.transport == TD_TRANSPORT_RDMA) {
        memset(&rdma_host_ctx, 0, sizeof(rdma_host_ctx));
        rdma_host_ctx.cfg = &cfg;
        rdma_host_ctx.region = &region;
        rdma_host_ctx.stop_flag = &td_stop;
        pthread_create(&rdma_host_thread_id, NULL, td_rdma_host_thread, &rdma_host_ctx);
        have_rdma_host_thread = 1;
        fprintf(stdout, "tdx-dist mn rdma endpoint %s:%d started\n", cfg.listen_host, cfg.listen_port);
    }

    fprintf(stdout, "tdx-dist mn node_id=%d transport=%s backing=%s bytes=%llu\n",
        cfg.node_id,
        cfg.transport == TD_TRANSPORT_RDMA ? "rdma" : "tcp",
        region.backing_path,
        (unsigned long long)region.mapped_bytes);
    td_print_memory_layout(&cfg, &region);
    fflush(stdout);

    if (cfg.transport == TD_TRANSPORT_RDMA) {
        rc = td_request_server_run(&region, &td_stop, cfg.eviction_threshold_pct, err, sizeof(err));
    } else {
        rc = td_tcp_server_run(&cfg, &region, &td_stop, err, sizeof(err));
    }

    td_stop = 1;
    if (have_rdma_host_thread) {
        pthread_join(rdma_host_thread_id, NULL);
        if (rdma_host_ctx.rc != 0) {
            fprintf(stderr, "rdma host error: %s\n", rdma_host_ctx.err);
            if (rc == 0) {
                rc = rdma_host_ctx.rc;
            }
        }
    }
    pthread_join(evict_thread, NULL);
    td_region_close(&region);

    if (rc != 0) {
        fprintf(stderr, "server error: %s\n", err);
        return 1;
    }
    return 0;
}
