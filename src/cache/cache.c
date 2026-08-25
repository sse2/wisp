#include "cache.h"

#include "common.h"
#include "net/http.h"
#include "plat/plat.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define cache_fseek64 _fseeki64
#define cache_ftell64 _ftelli64
#else
#define cache_fseek64 fseeko
#define cache_ftell64 ftello
#endif

typedef struct {
    wisp_mutex *mtx;
    wisp_cond *cond;
    char *path;
    char *key;
    char *url;
    int64_t committed;
    int64_t total;
    bool complete;
    bool failed;
    bool pinned;
    bool downloading;
    int reader_count;
    atomic_bool abandon;
    FILE *writer;
    bool trust;
    struct wisp_cache *owner;
} cache_entry;

typedef struct {
    char *key;
    char *url;
    int64_t size;
} pin_row;

struct wisp_cache {
    char *dir;
    char *audio_dir;
    bool trust;
    wisp_mutex *mtx;
    cache_entry **entries;
    size_t entry_count, entry_cap;
    wisp_thread **threads;
    size_t thread_count, thread_cap;
    pin_row *pins;
    size_t pin_count, pin_cap;
    int64_t limit;
};

typedef struct {
    cache_entry *e;
    FILE *fh;
    int64_t off;
} cache_reader;

static void cache_enforce(wisp_cache *c);

static char *build_path(wisp_cache *c, const char *key) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = key; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return wisp_aprintf("%s%c%016llx.dat", c->audio_dir, WISP_SEP, (unsigned long long)h);
}

static int64_t file_size(const char *path) {
    FILE *f = wisp_fopen(path, "rb");
    if (!f)
        return -1;
    cache_fseek64(f, 0, SEEK_END);
    int64_t n = cache_ftell64(f);
    fclose(f);
    return n;
}

static void pins_load(wisp_cache *c);

wisp_cache *wisp_cache_new(const char *cache_dir, bool trust_tls) {
    wisp_cache *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->dir = wisp_strdup(cache_dir);
    c->audio_dir = wisp_path_join(cache_dir, "audio");
    c->trust = trust_tls;
    c->mtx = wisp_mutex_new();
    if (!c->dir || !c->audio_dir || !c->mtx) {
        wisp_cache_free(c);
        return NULL;
    }
    wisp_mkdirs(c->audio_dir);
    pins_load(c);
    return c;
}

static cache_entry *find_entry(wisp_cache *c, const char *key) {
    for (size_t i = 0; i < c->entry_count; i++)
        if (!strcmp(c->entries[i]->key, key))
            return c->entries[i];
    return NULL;
}

static bool dl_sink(void *ctx, const void *data, size_t len) {
    cache_entry *e = ctx;
    if (atomic_load(&e->abandon))
        return false;
    size_t w = fwrite(data, 1, len, e->writer);
    fflush(e->writer);
    wisp_mutex_lock(e->mtx);
    e->committed += (int64_t)w;
    wisp_cond_broadcast(e->cond);
    wisp_mutex_unlock(e->mtx);
    return w == len;
}

static void download_run(void *p) {
    cache_entry *e = p;
    e->writer = wisp_fopen_shared(e->path, "wb");
    if (!e->writer) {
        wisp_mutex_lock(e->mtx);
        e->failed = true;
        e->downloading = false;
        wisp_cond_broadcast(e->cond);
        wisp_mutex_unlock(e->mtx);
        return;
    }
    int64_t total = -1;
    wisp_err rc = wisp_http_download(e->url, NULL, 0, e->trust, dl_sink, e, &total);
    wisp_mutex_lock(e->mtx);
    if (rc == WISP_OK && (e->total <= 0 || e->committed == e->total))
        e->complete = true;
    else
        e->failed = true;
    if (e->writer) {
        fflush(e->writer);
        fclose(e->writer);
        e->writer = NULL;
    }
    bool complete = e->complete;
    e->downloading = false;
    wisp_cond_broadcast(e->cond);
    wisp_mutex_unlock(e->mtx);
    if (complete && e->owner)
        cache_enforce(e->owner);
}

static void start_download(wisp_cache *c, cache_entry *e) {
    wisp_thread *th = wisp_thread_start(download_run, e);
    if (!th)
        return;
    if (c->thread_count == c->thread_cap) {
        c->thread_cap = c->thread_cap ? c->thread_cap * 2 : 16;
        c->threads = realloc(c->threads, c->thread_cap * sizeof(wisp_thread *));
    }
    c->threads[c->thread_count++] = th;
}

static size_t reader_read(wisp_source *s, void *buf, size_t n) {
    cache_reader *r = s->impl;
    cache_entry *e = r->e;
    wisp_mutex_lock(e->mtx);
    while (e->committed < r->off + (int64_t)n && !e->complete && !e->failed)
        wisp_cond_wait(e->cond, e->mtx);
    int64_t avail = e->committed - r->off;
    wisp_mutex_unlock(e->mtx);
    if (avail <= 0)
        return 0;
    size_t toread = (int64_t)n < avail ? n : (size_t)avail;
    if (!r->fh) {
        r->fh = wisp_fopen_shared(e->path, "rb");
        if (!r->fh)
            return 0;
    }
    cache_fseek64(r->fh, r->off, SEEK_SET);
    size_t got = fread(buf, 1, toread, r->fh);
    r->off += (int64_t)got;
    return got;
}

static bool reader_seek(wisp_source *s, int64_t off, int whence) {
    cache_reader *r = s->impl;
    cache_entry *e = r->e;
    wisp_mutex_lock(e->mtx);
    int64_t end = e->total > 0 ? e->total : (e->complete ? e->committed : -1);
    if (whence == SEEK_END && end < 0) {
        wisp_mutex_unlock(e->mtx);
        return false;
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? r->off : end;
    int64_t target = base + off;
    if (target < 0) {
        wisp_mutex_unlock(e->mtx);
        return false;
    }
    while (target > e->committed && !e->complete && !e->failed)
        wisp_cond_wait(e->cond, e->mtx);
    bool ok = target <= e->committed || e->complete;
    wisp_mutex_unlock(e->mtx);
    if (ok)
        r->off = target;
    return ok;
}

static int64_t reader_tell(wisp_source *s) { return ((cache_reader *)s->impl)->off; }

static int64_t reader_size(wisp_source *s) {
    cache_reader *r = s->impl;
    cache_entry *e = r->e;
    wisp_mutex_lock(e->mtx);
    int64_t sz = e->total > 0 ? e->total : (e->complete ? e->committed : -1);
    wisp_mutex_unlock(e->mtx);
    return sz;
}

static void reader_close(wisp_source *s) {
    cache_reader *r = s->impl;
    cache_entry *e = r->e;
    if (r->fh)
        fclose(r->fh);
    wisp_mutex_lock(e->mtx);
    e->reader_count--;
    if (e->reader_count == 0 && !e->complete && !e->pinned)
        atomic_store(&e->abandon, true);
    wisp_mutex_unlock(e->mtx);
    free(r);
    free(s);
}

static wisp_source *make_reader(cache_entry *e) {
    wisp_source *s = calloc(1, sizeof *s);
    cache_reader *r = calloc(1, sizeof *r);
    if (!s || !r) {
        free(s);
        free(r);
        return NULL;
    }
    r->e = e;
    s->impl = r;
    s->read = reader_read;
    s->seek = reader_seek;
    s->tell = reader_tell;
    s->size = reader_size;
    s->close = reader_close;
    return s;
}

wisp_source *wisp_cache_open(wisp_cache *c, const char *key, const char *url, int64_t expected) {
    wisp_mutex_lock(c->mtx);
    cache_entry *e = find_entry(c, key);
    if (e) {
        wisp_mutex_lock(e->mtx);
        if (e->complete) {
            char *path = wisp_strdup(e->path);
            wisp_mutex_unlock(e->mtx);
            wisp_mutex_unlock(c->mtx);
            wisp_source *s = wisp_source_file(path);
            free(path);
            return s;
        }
        if (e->failed) {
            e->committed = 0;
            e->failed = false;
            e->complete = false;
        }
        atomic_store(&e->abandon, false);
        e->reader_count++;
        bool restart = !e->downloading && !e->complete;
        if (restart)
            e->downloading = true;
        wisp_mutex_unlock(e->mtx);
        if (restart)
            start_download(c, e);
        wisp_mutex_unlock(c->mtx);
        return make_reader(e);
    }

    char *path = build_path(c, key);
    if (expected > 0 && file_size(path) == expected) {
        wisp_mutex_unlock(c->mtx);
        wisp_source *s = wisp_source_file(path);
        free(path);
        return s;
    }

    e = calloc(1, sizeof *e);
    e->mtx = wisp_mutex_new();
    e->cond = wisp_cond_new();
    e->key = wisp_strdup(key);
    e->url = wisp_strdup(url);
    e->path = path;
    e->total = expected > 0 ? expected : -1;
    e->trust = c->trust;
    e->reader_count = 1;
    e->downloading = true;
    e->owner = c;
    atomic_init(&e->abandon, false);
    if (c->entry_count == c->entry_cap) {
        c->entry_cap = c->entry_cap ? c->entry_cap * 2 : 16;
        c->entries = realloc(c->entries, c->entry_cap * sizeof(cache_entry *));
    }
    c->entries[c->entry_count++] = e;
    start_download(c, e);
    wisp_mutex_unlock(c->mtx);
    return make_reader(e);
}

static char *pins_path(wisp_cache *c) { return wisp_path_join(c->dir, "pins"); }

static int pin_find(wisp_cache *c, const char *key) {
    for (size_t i = 0; i < c->pin_count; i++)
        if (!strcmp(c->pins[i].key, key))
            return (int)i;
    return -1;
}

static void pin_add_row(wisp_cache *c, const char *key, const char *url, int64_t size) {
    if (c->pin_count == c->pin_cap) {
        c->pin_cap = c->pin_cap ? c->pin_cap * 2 : 8;
        c->pins = realloc(c->pins, c->pin_cap * sizeof *c->pins);
    }
    c->pins[c->pin_count].key = wisp_strdup(key);
    c->pins[c->pin_count].url = wisp_strdup(url ? url : "");
    c->pins[c->pin_count].size = size;
    c->pin_count++;
}

static void pins_save(wisp_cache *c) {
    char *path = pins_path(c);
    if (!path)
        return;
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (buf)
        buf[0] = '\0';
    for (size_t i = 0; i < c->pin_count && buf; i++) {
        char *line = wisp_aprintf("%s\t%s\t%lld\n", c->pins[i].key, c->pins[i].url,
                                  (long long)c->pins[i].size);
        if (!line)
            continue;
        size_t n = strlen(line);
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap)
                cap *= 2;
            char *g = realloc(buf, cap);
            if (!g) {
                free(buf);
                buf = NULL;
                free(line);
                break;
            }
            buf = g;
        }
        memcpy(buf + len, line, n);
        len += n;
        buf[len] = '\0';
        free(line);
    }
    if (buf)
        wisp_file_write(path, buf, len, false);
    free(buf);
    free(path);
}

static void pins_load(wisp_cache *c) {
    char *path = pins_path(c);
    if (!path)
        return;
    void *data = NULL;
    bool got = wisp_file_read(path, &data, NULL);
    free(path);
    if (!got)
        return;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *t1 = strchr(line, '\t');
        if (!t1)
            continue;
        *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2)
            continue;
        *t2 = '\0';
        pin_add_row(c, line, t1 + 1, strtoll(t2 + 1, NULL, 10));
    }
    free(data);
}

static cache_entry *entry_new_pinned(wisp_cache *c, const char *key, const char *url, int64_t size) {
    cache_entry *e = calloc(1, sizeof *e);
    e->mtx = wisp_mutex_new();
    e->cond = wisp_cond_new();
    e->key = wisp_strdup(key);
    e->url = wisp_strdup(url);
    e->path = build_path(c, key);
    e->total = size > 0 ? size : -1;
    e->trust = c->trust;
    e->pinned = true;
    e->reader_count = 0;
    e->owner = c;
    atomic_init(&e->abandon, false);
    if (c->entry_count == c->entry_cap) {
        c->entry_cap = c->entry_cap ? c->entry_cap * 2 : 16;
        c->entries = realloc(c->entries, c->entry_cap * sizeof(cache_entry *));
    }
    c->entries[c->entry_count++] = e;
    return e;
}

void wisp_cache_pin(wisp_cache *c, const char *key, const char *url, int64_t size) {
    wisp_mutex_lock(c->mtx);
    if (pin_find(c, key) < 0) {
        pin_add_row(c, key, url, size);
        pins_save(c);
    }
    char *path = build_path(c, key);
    bool on_disk = size > 0 && file_size(path) == size;
    free(path);
    cache_entry *e = find_entry(c, key);
    if (e) {
        wisp_mutex_lock(e->mtx);
        e->pinned = true;
        if (e->failed) {
            e->failed = false;
            e->committed = 0;
            e->complete = false;
            atomic_store(&e->abandon, false);
        }
        bool restart = !e->downloading && !e->complete && !on_disk;
        if (restart)
            e->downloading = true;
        wisp_mutex_unlock(e->mtx);
        if (restart)
            start_download(c, e);
    } else if (!on_disk) {
        e = entry_new_pinned(c, key, url, size);
        e->downloading = true;
        start_download(c, e);
    }
    wisp_mutex_unlock(c->mtx);
}

void wisp_cache_unpin(wisp_cache *c, const char *key) {
    wisp_mutex_lock(c->mtx);
    int idx = pin_find(c, key);
    if (idx >= 0) {
        free(c->pins[idx].key);
        free(c->pins[idx].url);
        c->pins[idx] = c->pins[--c->pin_count];
        pins_save(c);
    }
    cache_entry *e = find_entry(c, key);
    if (e) {
        wisp_mutex_lock(e->mtx);
        e->pinned = false;
        wisp_mutex_unlock(e->mtx);
    }
    wisp_mutex_unlock(c->mtx);
}

bool wisp_cache_is_pinned(wisp_cache *c, const char *key) {
    wisp_mutex_lock(c->mtx);
    bool r = pin_find(c, key) >= 0;
    wisp_mutex_unlock(c->mtx);
    return r;
}

size_t wisp_cache_stats(wisp_cache *c, wisp_cache_stat **out) {
    wisp_mutex_lock(c->mtx);
    size_t n = c->pin_count;
    wisp_cache_stat *list = n ? calloc(n, sizeof *list) : NULL;
    for (size_t i = 0; i < n && list; i++) {
        list[i].key = wisp_strdup(c->pins[i].key);
        list[i].total = c->pins[i].size;
        cache_entry *e = find_entry(c, c->pins[i].key);
        if (e) {
            wisp_mutex_lock(e->mtx);
            list[i].committed = e->committed;
            if (e->total > 0)
                list[i].total = e->total;
            list[i].complete = e->complete;
            wisp_mutex_unlock(e->mtx);
        } else {
            char *path = build_path(c, c->pins[i].key);
            int64_t fs = file_size(path);
            free(path);
            list[i].committed = fs > 0 ? fs : 0;
            list[i].complete = c->pins[i].size > 0 && fs == c->pins[i].size;
        }
    }
    wisp_mutex_unlock(c->mtx);
    *out = list;
    return list ? n : 0;
}

void wisp_cache_stats_free(wisp_cache_stat *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(list[i].key);
    free(list);
}

typedef struct {
    char **names;
    size_t n, cap;
} namelist;

static void collect_dat(void *ctx, const char *name) {
    namelist *nl = ctx;
    size_t len = strlen(name);
    if (len < 4 || strcmp(name + len - 4, ".dat") != 0)
        return;
    if (nl->n == nl->cap) {
        nl->cap = nl->cap ? nl->cap * 2 : 32;
        nl->names = realloc(nl->names, nl->cap * sizeof(char *));
    }
    nl->names[nl->n++] = wisp_strdup(name);
}

typedef struct {
    char *path;
    int64_t size, mtime;
} evrec;

static int evrec_cmp(const void *a, const void *b) {
    int64_t ma = ((const evrec *)a)->mtime, mb = ((const evrec *)b)->mtime;
    return ma < mb ? -1 : ma > mb ? 1 : 0;
}

static bool path_in(char **arr, size_t n, const char *p) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(arr[i], p))
            return true;
    return false;
}

static void cache_enforce(wisp_cache *c) {
    wisp_mutex_lock(c->mtx);
    if (c->limit <= 0) {
        wisp_mutex_unlock(c->mtx);
        return;
    }
    char **prot = malloc((c->pin_count + c->entry_count + 1) * sizeof(char *));
    size_t pn = 0;
    for (size_t i = 0; i < c->pin_count; i++)
        prot[pn++] = build_path(c, c->pins[i].key);
    for (size_t i = 0; i < c->entry_count; i++) {
        cache_entry *e = c->entries[i];
        wisp_mutex_lock(e->mtx);
        bool busy = e->reader_count > 0 || e->downloading;
        wisp_mutex_unlock(e->mtx);
        if (busy)
            prot[pn++] = wisp_strdup(e->path);
    }
    namelist nl = {0};
    wisp_dir_list(c->audio_dir, collect_dat, &nl);
    evrec *ev = malloc((nl.n ? nl.n : 1) * sizeof(evrec));
    size_t evn = 0;
    int64_t total = 0;
    for (size_t i = 0; i < nl.n; i++) {
        char *full = wisp_path_join(c->audio_dir, nl.names[i]);
        int64_t sz = 0, mt = 0;
        if (full && wisp_file_stat(full, &sz, &mt)) {
            total += sz;
            if (!path_in(prot, pn, full)) {
                ev[evn].path = full;
                ev[evn].size = sz;
                ev[evn].mtime = mt;
                evn++;
                full = NULL;
            }
        }
        free(full);
        free(nl.names[i]);
    }
    free(nl.names);
    if (total > c->limit) {
        qsort(ev, evn, sizeof(evrec), evrec_cmp);
        for (size_t i = 0; i < evn && total > c->limit; i++)
            if (wisp_file_delete(ev[i].path))
                total -= ev[i].size;
    }
    for (size_t i = 0; i < evn; i++)
        free(ev[i].path);
    free(ev);
    for (size_t i = 0; i < pn; i++)
        free(prot[i]);
    free(prot);
    wisp_mutex_unlock(c->mtx);
}

void wisp_cache_set_limit(wisp_cache *c, int64_t bytes) {
    c->limit = bytes;
    cache_enforce(c);
}

int64_t wisp_cache_usage(wisp_cache *c) {
    namelist nl = {0};
    wisp_dir_list(c->audio_dir, collect_dat, &nl);
    int64_t total = 0;
    for (size_t i = 0; i < nl.n; i++) {
        char *full = wisp_path_join(c->audio_dir, nl.names[i]);
        int64_t sz = 0;
        if (full && wisp_file_stat(full, &sz, NULL))
            total += sz;
        free(full);
        free(nl.names[i]);
    }
    free(nl.names);
    return total;
}

void wisp_cache_free(wisp_cache *c) {
    if (!c)
        return;
    for (size_t i = 0; i < c->entry_count; i++)
        atomic_store(&c->entries[i]->abandon, true);
    for (size_t i = 0; i < c->thread_count; i++)
        wisp_thread_join(c->threads[i]);
    for (size_t i = 0; i < c->entry_count; i++) {
        cache_entry *e = c->entries[i];
        if (e->writer)
            fclose(e->writer);
        wisp_mutex_free(e->mtx);
        wisp_cond_free(e->cond);
        free(e->key);
        free(e->url);
        free(e->path);
        free(e);
    }
    free(c->entries);
    free(c->threads);
    for (size_t i = 0; i < c->pin_count; i++) {
        free(c->pins[i].key);
        free(c->pins[i].url);
    }
    free(c->pins);
    if (c->mtx)
        wisp_mutex_free(c->mtx);
    free(c->audio_dir);
    free(c->dir);
    free(c);
}
