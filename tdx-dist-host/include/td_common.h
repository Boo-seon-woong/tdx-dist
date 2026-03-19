#ifndef TD_COMMON_H
#define TD_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TD_PROJECT_MAGIC 0x5444444953544b56ULL
#define TD_MAX_VALUE_SIZE 256
#define TD_PATH_BYTES 256
#define TD_HOST_BYTES 64

typedef enum {
    TD_MODE_HOST = 0,
} td_mode_t;

typedef enum {
    TD_TRANSPORT_RDMA = 0,
} td_transport_t;

typedef enum {
    TD_CACHE_OFF = 0,
    TD_CACHE_ON = 1,
} td_cache_mode_t;

typedef enum {
    TD_TDX_OFF = 0,
    TD_TDX_ON = 1,
} td_tdx_mode_t;

char *td_trim(char *text);
void td_format_error(char *buf, size_t buf_len, const char *fmt, ...);

#endif
