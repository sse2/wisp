#include "plat.h"

#include "common.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *wisp_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out)
        memcpy(out, s, n);
    return out;
}

char *wisp_avprintf(const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out)
        return NULL;
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    return out;
}

char *wisp_aprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = wisp_avprintf(fmt, ap);
    va_end(ap);
    return out;
}

char *wisp_path_join(const char *a, const char *b) {
    if (!a || !*a)
        return wisp_strdup(b ? b : "");
    if (!b || !*b)
        return wisp_strdup(a);
    size_t la = strlen(a);
    bool has_sep = a[la - 1] == '/' || a[la - 1] == '\\';
    while (*b == '/' || *b == '\\')
        b++;
    return has_sep ? wisp_aprintf("%s%s", a, b) : wisp_aprintf("%s%c%s", a, WISP_SEP, b);
}

bool wisp_file_exists(const char *path) {
    FILE *f = wisp_fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool wisp_file_read(const char *path, void **out_data, size_t *out_len) {
    FILE *f = wisp_fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *out_data = buf;
    if (out_len)
        *out_len = got;
    return true;
}

struct wisp_chan {
    wisp_mutex *mtx;
    wisp_cond *not_full;
    wisp_cond *not_empty;
    unsigned char *buf;
    size_t elem_size;
    size_t cap;
    size_t head;
    size_t count;
    bool closed;
};

wisp_chan *wisp_chan_new(size_t elem_size, size_t capacity) {
    wisp_chan *ch = calloc(1, sizeof *ch);
    if (!ch)
        return NULL;
    ch->buf = malloc(elem_size * capacity);
    ch->mtx = wisp_mutex_new();
    ch->not_full = wisp_cond_new();
    ch->not_empty = wisp_cond_new();
    ch->elem_size = elem_size;
    ch->cap = capacity;
    if (!ch->buf || !ch->mtx || !ch->not_full || !ch->not_empty) {
        wisp_chan_free(ch);
        return NULL;
    }
    return ch;
}

void wisp_chan_free(wisp_chan *ch) {
    if (!ch)
        return;
    if (ch->not_empty)
        wisp_cond_free(ch->not_empty);
    if (ch->not_full)
        wisp_cond_free(ch->not_full);
    if (ch->mtx)
        wisp_mutex_free(ch->mtx);
    free(ch->buf);
    free(ch);
}

static void chan_store(wisp_chan *ch, const void *elem) {
    size_t slot = (ch->head + ch->count) % ch->cap;
    memcpy(ch->buf + slot * ch->elem_size, elem, ch->elem_size);
    ch->count++;
}

static void chan_take(wisp_chan *ch, void *out) {
    memcpy(out, ch->buf + ch->head * ch->elem_size, ch->elem_size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
}

bool wisp_chan_send(wisp_chan *ch, const void *elem) {
    wisp_mutex_lock(ch->mtx);
    while (ch->count == ch->cap && !ch->closed)
        wisp_cond_wait(ch->not_full, ch->mtx);
    if (ch->closed) {
        wisp_mutex_unlock(ch->mtx);
        return false;
    }
    chan_store(ch, elem);
    wisp_cond_signal(ch->not_empty);
    wisp_mutex_unlock(ch->mtx);
    return true;
}

bool wisp_chan_try_send(wisp_chan *ch, const void *elem) {
    wisp_mutex_lock(ch->mtx);
    if (ch->closed || ch->count == ch->cap) {
        wisp_mutex_unlock(ch->mtx);
        return false;
    }
    chan_store(ch, elem);
    wisp_cond_signal(ch->not_empty);
    wisp_mutex_unlock(ch->mtx);
    return true;
}

bool wisp_chan_recv(wisp_chan *ch, void *out) {
    wisp_mutex_lock(ch->mtx);
    while (ch->count == 0 && !ch->closed)
        wisp_cond_wait(ch->not_empty, ch->mtx);
    if (ch->count == 0) {
        wisp_mutex_unlock(ch->mtx);
        return false;
    }
    chan_take(ch, out);
    wisp_cond_signal(ch->not_full);
    wisp_mutex_unlock(ch->mtx);
    return true;
}

bool wisp_chan_recv_timeout(wisp_chan *ch, void *out, uint32_t ms) {
    wisp_mutex_lock(ch->mtx);
    uint64_t deadline = wisp_now_ms() + ms;
    while (ch->count == 0 && !ch->closed) {
        uint64_t now = wisp_now_ms();
        if (now >= deadline)
            break;
        wisp_cond_wait_ms(ch->not_empty, ch->mtx, (uint32_t)(deadline - now));
    }
    if (ch->count == 0) {
        wisp_mutex_unlock(ch->mtx);
        return false;
    }
    chan_take(ch, out);
    wisp_cond_signal(ch->not_full);
    wisp_mutex_unlock(ch->mtx);
    return true;
}

bool wisp_chan_try_recv(wisp_chan *ch, void *out) {
    wisp_mutex_lock(ch->mtx);
    if (ch->count == 0) {
        wisp_mutex_unlock(ch->mtx);
        return false;
    }
    chan_take(ch, out);
    wisp_cond_signal(ch->not_full);
    wisp_mutex_unlock(ch->mtx);
    return true;
}

size_t wisp_chan_len(wisp_chan *ch) {
    wisp_mutex_lock(ch->mtx);
    size_t n = ch->count;
    wisp_mutex_unlock(ch->mtx);
    return n;
}

void wisp_chan_close(wisp_chan *ch) {
    wisp_mutex_lock(ch->mtx);
    ch->closed = true;
    wisp_cond_broadcast(ch->not_empty);
    wisp_cond_broadcast(ch->not_full);
    wisp_mutex_unlock(ch->mtx);
}

static _Atomic int g_log_state;
static wisp_mutex *g_log_mtx;

static wisp_mutex *log_mutex(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_log_state, &expected, 1)) {
        g_log_mtx = wisp_mutex_new();
        atomic_store(&g_log_state, 2);
    } else {
        while (atomic_load(&g_log_state) != 2)
            ;
    }
    return g_log_mtx;
}

void wisp_log(const char *fmt, ...) {
    if (!getenv("WISP_DEBUG"))
        return;
    wisp_mutex *m = log_mutex();
    wisp_mutex_lock(m);
    static FILE *fh;
    if (!fh) {
        char *dir = wisp_dir_path(WISP_DIR_DATA);
        if (dir) {
            wisp_mkdirs(dir);
            char *path = wisp_path_join(dir, "wisp-debug.log");
            if (path) {
                fh = wisp_fopen(path, "a");
                free(path);
            }
            free(dir);
        }
    }
    if (fh) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(fh, "%llu  ", (unsigned long long)wisp_now_ms());
        vfprintf(fh, fmt, ap);
        fputc('\n', fh);
        fflush(fh);
        va_end(ap);
    }
    wisp_mutex_unlock(m);
}
