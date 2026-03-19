#ifndef TD_CONFIG_H
#define TD_CONFIG_H

#include "td_common.h"

typedef struct td_config {
    td_mode_t mode;
    td_transport_t transport;
    td_tdx_mode_t tdx;
    td_cache_mode_t cache;
    size_t mn_memory_size;
    char listen_host[TD_HOST_BYTES];
    int listen_port;
    int node_id;
    char memory_file[TD_PATH_BYTES];
    size_t request_slots;
    size_t prime_slots;
    size_t cache_slots;
    size_t backup_slots;
    int prime_slots_explicit;
    int cache_slots_explicit;
    int backup_slots_explicit;
    size_t max_value_size;
} td_config_t;

void td_config_init_defaults(td_config_t *cfg);
int td_config_load(const char *path, td_config_t *cfg, char *err, size_t err_len);

#endif
