#ifndef WISP_COMMON_H
#define WISP_COMMON_H

#include <stdarg.h>

typedef enum {
    WISP_OK = 0,
    WISP_ERR = -1,
    WISP_ERR_OOM = -2,
    WISP_ERR_IO = -3,
    WISP_ERR_NET = -4,
    WISP_ERR_TLS = -5,
    WISP_ERR_HTTP = -6,
    WISP_ERR_AUTH = -7,
    WISP_ERR_PARSE = -8,
    WISP_ERR_SERVER = -9,
    WISP_ERR_ABANDONED = -10,
    WISP_ERR_NOTFOUND = -11,
    WISP_ERR_UNSUPPORTED = -12,
} wisp_err;

char *wisp_strdup(const char *s);
char *wisp_aprintf(const char *fmt, ...);
char *wisp_avprintf(const char *fmt, va_list ap);

#endif
