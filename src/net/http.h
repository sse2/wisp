#ifndef WISP_NET_HTTP_H
#define WISP_NET_HTTP_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char scheme[8];
    char host[256];
    int port;
    char *path;
    bool tls;
} wisp_url;

bool wisp_url_parse(const char *url, wisp_url *out);
void wisp_url_free(wisp_url *u);

typedef struct {
    int status;
    char *body;
    size_t body_len;
    char content_type[128];
    int64_t content_length;
    bool accepts_ranges;
} wisp_http_response;

wisp_err wisp_http_get(const char *url, const char **headers, size_t header_count,
                       bool trust_self_signed, wisp_http_response *out);
void wisp_http_response_free(wisp_http_response *r);

typedef bool (*wisp_http_sink)(void *ctx, const void *data, size_t len);

wisp_err wisp_http_download(const char *url, const char **headers, size_t header_count,
                            bool trust_self_signed, wisp_http_sink sink, void *ctx,
                            int64_t *out_total);

#endif
