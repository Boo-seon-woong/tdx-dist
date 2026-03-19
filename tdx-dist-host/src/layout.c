#include "td_layout.h"
#include "td_config.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define TD_REGION_VERSION 2ULL

size_t td_request_ring_bytes_for_slots(size_t request_slots) {
    return sizeof(td_request_ring_t) + (request_slots * sizeof(td_request_entry_t));
}

size_t td_region_required_bytes(const td_config_t *cfg) {
    return cfg->mn_memory_size;
}

int td_region_open(td_local_region_t *region, const td_config_t *cfg, char *err, size_t err_len) {
    td_request_ring_t *ring;
    size_t bytes = td_region_required_bytes(cfg);
    void *mapped;

    memset(region, 0, sizeof(*region));
    region->fd = open(cfg->memory_file, O_RDWR | O_CREAT, 0600);
    if (region->fd < 0) {
        td_format_error(err, err_len, "cannot open shared region %s", cfg->memory_file);
        return -1;
    }
    if (ftruncate(region->fd, (off_t)bytes) != 0) {
        td_format_error(err, err_len, "cannot size shared region %s", cfg->memory_file);
        close(region->fd);
        region->fd = -1;
        return -1;
    }

    mapped = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd, 0);
    if (mapped == MAP_FAILED) {
        td_format_error(err, err_len, "mmap failed for %s", cfg->memory_file);
        close(region->fd);
        region->fd = -1;
        return -1;
    }

    region->base = mapped;
    region->mapped_bytes = bytes;
    region->header = (td_region_header_t *)mapped;
    snprintf(region->backing_path, sizeof(region->backing_path), "%s", cfg->memory_file);

    memset(region->base, 0, bytes);
    region->header->magic = TD_PROJECT_MAGIC;
    region->header->version = TD_REGION_VERSION;
    region->header->node_id = (uint64_t)(cfg->node_id < 0 ? 0 : cfg->node_id);
    region->header->prime_slot_count = cfg->prime_slots;
    region->header->cache_slot_count = cfg->cache_slots;
    region->header->backup_slot_count = cfg->backup_slots;
    region->header->max_value_size = cfg->max_value_size;
    region->header->cache_usage = 0;
    region->header->eviction_cursor = 0;
    region->header->cache_mode = cfg->cache;
    region->header->region_size = region->mapped_bytes;
    region->header->request_ring_offset = sizeof(td_region_header_t);
    region->header->request_ring_bytes = td_request_ring_bytes_for_slots(cfg->request_slots);
    region->header->request_capacity = cfg->request_slots;
    region->header->request_entry_size = sizeof(td_request_entry_t);
    region->header->slot_region_offset = region->header->request_ring_offset + region->header->request_ring_bytes;

    ring = (td_request_ring_t *)((unsigned char *)region->base + region->header->request_ring_offset);
    ring->reserve_head = 0;
    ring->head = 0;
    ring->tail = 0;
    ring->capacity = cfg->request_slots;
    ring->entry_size = sizeof(td_request_entry_t);
    return 0;
}

void td_region_close(td_local_region_t *region) {
    if (region->base != NULL && region->mapped_bytes > 0) {
        munmap(region->base, region->mapped_bytes);
    }
    if (region->fd >= 0) {
        close(region->fd);
    }
    memset(region, 0, sizeof(*region));
    region->fd = -1;
}
