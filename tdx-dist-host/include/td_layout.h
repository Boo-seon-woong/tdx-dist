#ifndef TD_LAYOUT_H
#define TD_LAYOUT_H

#include "td_common.h"

typedef struct td_config td_config_t;

#define TD_SLOT_FLAG_VALID 0x1u
#define TD_SLOT_FLAG_TOMBSTONE 0x2u

typedef enum {
    TD_REGION_PRIME = 0,
    TD_REGION_CACHE = 1,
    TD_REGION_BACKUP = 2,
} td_region_kind_t;

typedef struct {
    uint64_t guard_epoch;
    uint64_t visible_epoch;
    uint64_t key_hash;
    uint64_t tie_breaker;
    uint32_t flags;
    uint32_t value_len;
    unsigned char iv[16];
    unsigned char mac[32];
    unsigned char ciphertext[TD_MAX_VALUE_SIZE];
} td_slot_t;

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
    uint64_t magic;
    uint64_t version;
    uint64_t node_id;
    uint64_t prime_slot_count;
    uint64_t cache_slot_count;
    uint64_t backup_slot_count;
    uint64_t max_value_size;
    uint64_t cache_usage;
    uint64_t eviction_cursor;
    uint64_t cache_mode;
    uint64_t region_size;
    uint64_t request_ring_offset;
    uint64_t request_ring_bytes;
    uint64_t request_capacity;
    uint64_t request_entry_size;
    uint64_t slot_region_offset;
} td_region_header_t;

typedef struct {
    uint64_t reserve_head;
    uint64_t head;
    uint64_t tail;
    uint64_t capacity;
    uint64_t entry_size;
} td_request_ring_t;

typedef struct {
    void *base;
    size_t mapped_bytes;
    int fd;
    char backing_path[TD_PATH_BYTES];
    td_region_header_t *header;
} td_local_region_t;

size_t td_request_ring_bytes_for_slots(size_t request_slots);
size_t td_region_required_bytes(const td_config_t *cfg);
int td_region_open(td_local_region_t *region, const td_config_t *cfg, char *err, size_t err_len);
void td_region_close(td_local_region_t *region);

#endif
