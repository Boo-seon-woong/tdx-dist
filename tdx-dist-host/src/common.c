#include "td_common.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char *td_trim(char *text) {
    char *end;

    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }
    return text;
}

static int td_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

int td_hex_to_bytes(const char *hex, unsigned char *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    size_t i;

    if (hex_len != (out_len * 2)) {
        return -1;
    }

    for (i = 0; i < out_len; ++i) {
        int hi = td_hex_digit(hex[i * 2]);
        int lo = td_hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

void td_format_error(char *buf, size_t buf_len, const char *fmt, ...) {
    va_list args;

    if (buf_len == 0) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(buf, buf_len, fmt, args);
    va_end(args);
}
