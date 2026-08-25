#include "http.h"

#include "plat/plat.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WISP_HTTP_MAX_BODY (64 * 1024 * 1024)
#define WISP_HTTP_MAX_REDIRECTS 6

bool wisp_url_parse(const char *url, wisp_url *u) {
    memset(u, 0, sizeof *u);
    const char *p = url;
    if (!strncmp(p, "https://", 8)) {
        u->tls = true;
        snprintf(u->scheme, sizeof u->scheme, "https");
        u->port = 443;
        p += 8;
    } else if (!strncmp(p, "http://", 7)) {
        u->tls = false;
        snprintf(u->scheme, sizeof u->scheme, "http");
        u->port = 80;
        p += 7;
    } else {
        return false;
    }
    const char *host = p;
    while (*p && *p != '/' && *p != ':')
        p++;
    size_t hlen = (size_t)(p - host);
    if (hlen == 0 || hlen >= sizeof u->host)
        return false;
    memcpy(u->host, host, hlen);
    if (*p == ':') {
        p++;
        u->port = atoi(p);
        while (*p && *p != '/')
            p++;
    }
    u->path = wisp_strdup(*p ? p : "/");
    return u->path != NULL;
}

void wisp_url_free(wisp_url *u) {
    free(u->path);
    u->path = NULL;
}

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt cacert;
    bool tls;
} wisp_conn;

static void add_der(void *ctx, const unsigned char *der, size_t len) {
    mbedtls_x509_crt_parse_der((mbedtls_x509_crt *)ctx, der, len);
}

static bool conn_open(wisp_conn *c, const char *host, int port, bool tls, bool trust) {
    memset(c, 0, sizeof *c);
    c->tls = tls;
    mbedtls_net_init(&c->net);
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (mbedtls_net_connect(&c->net, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0)
        return false;
    if (!tls)
        return true;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_x509_crt_init(&c->cacert);

    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, NULL, 0) != 0)
        return false;
    wisp_plat_system_cas(add_der, &c->cacert);
    char *bundle = wisp_plat_ca_bundle_path();
    if (bundle) {
        mbedtls_x509_crt_parse_file(&c->cacert, bundle);
        free(bundle);
    }
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    mbedtls_ssl_conf_authmode(&c->conf,
                              trust ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0)
        return false;
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    for (int r; (r = mbedtls_ssl_handshake(&c->ssl)) != 0;)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
            return false;
    return true;
}

static void conn_close(wisp_conn *c) {
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_x509_crt_free(&c->cacert);
        mbedtls_entropy_free(&c->entropy);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ssl_free(&c->ssl);
    }
    mbedtls_net_free(&c->net);
}

static bool conn_send(wisp_conn *c, const void *data, size_t len) {
    const unsigned char *p = data;
    size_t sent = 0;
    while (sent < len) {
        int n = c->tls ? mbedtls_ssl_write(&c->ssl, p + sent, len - sent)
                       : mbedtls_net_send(&c->net, p + sent, len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static int conn_recv(wisp_conn *c, unsigned char *buf, size_t cap) {
    for (;;) {
        int n = c->tls ? mbedtls_ssl_read(&c->ssl, buf, cap)
                       : mbedtls_net_recv(&c->net, buf, cap);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        return n < 0 ? -1 : n;
    }
}

static bool read_all(wisp_conn *c, char **out, size_t *out_len) {
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return false;
    for (;;) {
        if (len + 16384 > cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown || cap > WISP_HTTP_MAX_BODY) {
                free(buf);
                free(grown);
                return false;
            }
            buf = grown;
        }
        int n = conn_recv(c, (unsigned char *)buf + len, 16384);
        if (n < 0) {
            free(buf);
            return false;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    buf[len < cap ? len : cap - 1] = '\0';
    *out = buf;
    *out_len = len;
    return true;
}

static bool hdr_is(const char *line, const char *name) {
    size_t i = 0;
    for (; name[i]; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != name[i])
            return false;
    }
    return line[i] == ':';
}

static const char *hdr_val(const char *line) {
    const char *p = strchr(line, ':');
    if (!p)
        return "";
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static void dechunk(char *body, size_t len, char **out, size_t *out_len) {
    char *dst = malloc(len + 1);
    if (!dst) {
        *out = NULL;
        return;
    }
    size_t di = 0, si = 0;
    while (si < len) {
        char *end;
        long chunk = strtol(body + si, &end, 16);
        if (end == body + si || chunk <= 0)
            break;
        si = (size_t)(end - body);
        while (si < len && body[si] != '\n')
            si++;
        si++;
        if (si + (size_t)chunk > len)
            break;
        memcpy(dst + di, body + si, (size_t)chunk);
        di += (size_t)chunk;
        si += (size_t)chunk + 2;
    }
    dst[di] = '\0';
    *out = dst;
    *out_len = di;
}

static wisp_err do_get(const wisp_url *u, const char **headers, size_t header_count, bool trust,
                       wisp_http_response *out, char **redirect) {
    wisp_conn conn;
    if (!conn_open(&conn, u->host, u->port, u->tls, trust))
        return u->tls ? WISP_ERR_TLS : WISP_ERR_NET;

    char req[2048];
    int off = snprintf(req, sizeof req,
                       "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: wisp/0.1\r\n"
                       "Accept: */*\r\nConnection: close\r\n",
                       u->path, u->host);
    for (size_t i = 0; i < header_count && off < (int)sizeof req - 4; i++)
        off += snprintf(req + off, sizeof req - off, "%s\r\n", headers[i]);
    off += snprintf(req + off, sizeof req - off, "\r\n");

    bool sent = conn_send(&conn, req, (size_t)off);
    char *raw = NULL;
    size_t raw_len = 0;
    bool got = sent && read_all(&conn, &raw, &raw_len);
    conn_close(&conn);
    if (!got) {
        free(raw);
        return WISP_ERR_NET;
    }

    char *split = strstr(raw, "\r\n\r\n");
    if (!split) {
        free(raw);
        return WISP_ERR_HTTP;
    }
    *split = '\0';
    char *body = split + 4;
    size_t body_len = raw_len - (size_t)(body - raw);

    memset(out, 0, sizeof *out);
    out->content_length = -1;
    bool chunked = false;
    char *location = NULL;

    char *sp = strchr(raw, ' ');
    out->status = sp ? (int)strtol(sp + 1, NULL, 10) : 0;
    char *cursor = strstr(raw, "\r\n");
    while (cursor) {
        cursor += 2;
        if (!*cursor)
            break;
        char *eol = strstr(cursor, "\r\n");
        if (eol)
            *eol = '\0';
        if (hdr_is(cursor, "content-length"))
            out->content_length = strtoll(hdr_val(cursor), NULL, 10);
        else if (hdr_is(cursor, "content-type"))
            snprintf(out->content_type, sizeof out->content_type, "%s", hdr_val(cursor));
        else if (hdr_is(cursor, "accept-ranges"))
            out->accepts_ranges = strstr(hdr_val(cursor), "bytes") != NULL;
        else if (hdr_is(cursor, "transfer-encoding"))
            chunked = strstr(hdr_val(cursor), "chunked") != NULL;
        else if (hdr_is(cursor, "location"))
            location = wisp_strdup(hdr_val(cursor));
        cursor = eol;
    }

    if (out->status >= 300 && out->status < 400 && location) {
        *redirect = location;
        free(raw);
        return WISP_OK;
    }
    free(location);

    if (chunked) {
        dechunk(body, body_len, &out->body, &out->body_len);
        free(raw);
        if (!out->body)
            return WISP_ERR_OOM;
    } else {
        char *b = malloc(body_len + 1);
        if (!b) {
            free(raw);
            return WISP_ERR_OOM;
        }
        memcpy(b, body, body_len);
        b[body_len] = '\0';
        out->body = b;
        out->body_len = body_len;
        free(raw);
    }
    return WISP_OK;
}

wisp_err wisp_http_get(const char *url, const char **headers, size_t header_count, bool trust,
                       wisp_http_response *out) {
    char *current = wisp_strdup(url);
    if (!current)
        return WISP_ERR_OOM;
    wisp_err rc = WISP_ERR;
    for (int hop = 0; hop < WISP_HTTP_MAX_REDIRECTS; hop++) {
        wisp_url u;
        if (!wisp_url_parse(current, &u)) {
            rc = WISP_ERR;
            break;
        }
        char *redirect = NULL;
        rc = do_get(&u, headers, header_count, trust, out, &redirect);
        wisp_url_free(&u);
        if (rc != WISP_OK) {
            free(redirect);
            break;
        }
        if (!redirect) {
            free(current);
            return WISP_OK;
        }
        free(current);
        current = redirect;
    }
    free(current);
    return rc == WISP_OK ? WISP_ERR_HTTP : rc;
}

void wisp_http_response_free(wisp_http_response *r) {
    free(r->body);
    r->body = NULL;
}

static char *find_header_end(char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    return NULL;
}

static void parse_resp_headers(char *block, int *status, int64_t *clen, bool *chunked,
                               char **location) {
    char *sp = strchr(block, ' ');
    *status = sp ? (int)strtol(sp + 1, NULL, 10) : 0;
    char *cursor = strstr(block, "\r\n");
    while (cursor) {
        cursor += 2;
        if (!*cursor)
            break;
        char *eol = strstr(cursor, "\r\n");
        if (eol)
            *eol = '\0';
        if (hdr_is(cursor, "content-length"))
            *clen = strtoll(hdr_val(cursor), NULL, 10);
        else if (hdr_is(cursor, "transfer-encoding"))
            *chunked = strstr(hdr_val(cursor), "chunked") != NULL;
        else if (hdr_is(cursor, "location"))
            *location = wisp_strdup(hdr_val(cursor));
        cursor = eol;
    }
}

typedef struct {
    int state;
    size_t remaining;
    char sizeline[40];
    int sizelen;
    bool done;
} chunk_state;

static bool chunk_feed(chunk_state *cs, const unsigned char *b, size_t len, wisp_http_sink sink,
                       void *ctx) {
    size_t i = 0;
    while (i < len && !cs->done) {
        if (cs->state == 0) {
            while (i < len && b[i] != '\n') {
                if (cs->sizelen < 39)
                    cs->sizeline[cs->sizelen++] = (char)b[i];
                i++;
            }
            if (i < len) {
                i++;
                cs->sizeline[cs->sizelen] = '\0';
                cs->remaining = (size_t)strtol(cs->sizeline, NULL, 16);
                cs->sizelen = 0;
                cs->state = cs->remaining == 0 ? 3 : 1;
                cs->done = cs->remaining == 0;
            }
        } else if (cs->state == 1) {
            size_t take = cs->remaining < len - i ? cs->remaining : len - i;
            if (take && !sink(ctx, b + i, take))
                return false;
            i += take;
            cs->remaining -= take;
            if (cs->remaining == 0)
                cs->state = 2;
        } else {
            while (i < len && b[i] != '\n')
                i++;
            if (i < len) {
                i++;
                cs->state = 0;
            }
        }
    }
    return true;
}

static wisp_err do_download(const wisp_url *u, const char **headers, size_t header_count,
                            bool trust, wisp_http_sink sink, void *ctx, int64_t *out_total,
                            char **redirect) {
    wisp_conn conn;
    if (!conn_open(&conn, u->host, u->port, u->tls, trust))
        return u->tls ? WISP_ERR_TLS : WISP_ERR_NET;

    char req[2048];
    int off = snprintf(req, sizeof req,
                       "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: wisp/0.1\r\n"
                       "Accept: */*\r\nConnection: close\r\n",
                       u->path, u->host);
    for (size_t i = 0; i < header_count && off < (int)sizeof req - 4; i++)
        off += snprintf(req + off, sizeof req - off, "%s\r\n", headers[i]);
    off += snprintf(req + off, sizeof req - off, "\r\n");
    if (!conn_send(&conn, req, (size_t)off)) {
        conn_close(&conn);
        return WISP_ERR_NET;
    }

    unsigned char rbuf[32768];
    char *hbuf = NULL;
    size_t hlen = 0, hcap = 0;
    char *sep = NULL;
    while (!sep) {
        int n = conn_recv(&conn, rbuf, sizeof rbuf);
        if (n <= 0) {
            free(hbuf);
            conn_close(&conn);
            return WISP_ERR_NET;
        }
        if (hlen + (size_t)n + 1 > hcap) {
            hcap = (hlen + (size_t)n + 1) * 2;
            hbuf = realloc(hbuf, hcap);
        }
        memcpy(hbuf + hlen, rbuf, (size_t)n);
        hlen += (size_t)n;
        sep = find_header_end(hbuf, hlen);
        if (!sep && hlen > 256 * 1024) {
            free(hbuf);
            conn_close(&conn);
            return WISP_ERR_HTTP;
        }
    }

    size_t body_off = (size_t)(sep - hbuf) + 4;
    *sep = '\0';
    int status = 0;
    int64_t clen = -1;
    bool chunked = false;
    char *location = NULL;
    parse_resp_headers(hbuf, &status, &clen, &chunked, &location);

    if (status >= 300 && status < 400 && location) {
        *redirect = location;
        free(hbuf);
        conn_close(&conn);
        return WISP_OK;
    }
    free(location);
    if (status != 200 && status != 206) {
        free(hbuf);
        conn_close(&conn);
        return WISP_ERR_HTTP;
    }
    if (out_total)
        *out_total = chunked ? -1 : clen;

    unsigned char *body0 = (unsigned char *)hbuf + body_off;
    size_t leftover = hlen - body_off;
    wisp_err rc = WISP_OK;

    if (chunked) {
        chunk_state cs = {0};
        if (chunk_feed(&cs, body0, leftover, sink, ctx))
            while (!cs.done) {
                int n = conn_recv(&conn, rbuf, sizeof rbuf);
                if (n <= 0)
                    break;
                if (!chunk_feed(&cs, rbuf, (size_t)n, sink, ctx))
                    break;
            }
    } else {
        int64_t remaining = clen;
        bool go = true;
        if (leftover) {
            go = sink(ctx, body0, leftover);
            if (remaining > 0)
                remaining -= (int64_t)leftover;
        }
        while (go && remaining != 0) {
            int n = conn_recv(&conn, rbuf, sizeof rbuf);
            if (n < 0) {
                rc = WISP_ERR_NET;
                break;
            }
            if (n == 0)
                break;
            go = sink(ctx, rbuf, (size_t)n);
            if (remaining > 0)
                remaining -= n;
        }
    }

    free(hbuf);
    conn_close(&conn);
    return rc;
}

wisp_err wisp_http_download(const char *url, const char **headers, size_t header_count,
                            bool trust, wisp_http_sink sink, void *ctx, int64_t *out_total) {
    char *current = wisp_strdup(url);
    if (!current)
        return WISP_ERR_OOM;
    wisp_err rc = WISP_ERR;
    for (int hop = 0; hop < WISP_HTTP_MAX_REDIRECTS; hop++) {
        wisp_url u;
        if (!wisp_url_parse(current, &u)) {
            rc = WISP_ERR;
            break;
        }
        char *redirect = NULL;
        rc = do_download(&u, headers, header_count, trust, sink, ctx, out_total, &redirect);
        wisp_url_free(&u);
        if (rc != WISP_OK || !redirect) {
            free(redirect);
            break;
        }
        free(current);
        current = redirect;
    }
    free(current);
    return rc;
}
