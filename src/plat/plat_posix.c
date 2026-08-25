#ifndef _WIN32

#include "plat.h"

#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct wisp_thread {
    pthread_t handle;
};
struct wisp_mutex {
    pthread_mutex_t m;
};
struct wisp_cond {
    pthread_cond_t c;
};

typedef struct {
    void (*entry)(void *);
    void *arg;
} thread_start;

static void *thread_thunk(void *p) {
    thread_start s = *(thread_start *)p;
    free(p);
    s.entry(s.arg);
    return NULL;
}

wisp_thread *wisp_thread_start(void (*entry)(void *), void *arg) {
    wisp_thread *t = malloc(sizeof *t);
    thread_start *s = malloc(sizeof *s);
    if (!t || !s) {
        free(t);
        free(s);
        return NULL;
    }
    s->entry = entry;
    s->arg = arg;
    if (pthread_create(&t->handle, NULL, thread_thunk, s) != 0) {
        free(t);
        free(s);
        return NULL;
    }
    return t;
}

void wisp_thread_join(wisp_thread *t) {
    if (!t)
        return;
    pthread_join(t->handle, NULL);
    free(t);
}

wisp_mutex *wisp_mutex_new(void) {
    wisp_mutex *m = malloc(sizeof *m);
    if (m)
        pthread_mutex_init(&m->m, NULL);
    return m;
}

void wisp_mutex_free(wisp_mutex *m) {
    if (!m)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void wisp_mutex_lock(wisp_mutex *m) { pthread_mutex_lock(&m->m); }
void wisp_mutex_unlock(wisp_mutex *m) { pthread_mutex_unlock(&m->m); }

wisp_cond *wisp_cond_new(void) {
    wisp_cond *c = malloc(sizeof *c);
    if (c)
        pthread_cond_init(&c->c, NULL);
    return c;
}

void wisp_cond_free(wisp_cond *c) {
    if (!c)
        return;
    pthread_cond_destroy(&c->c);
    free(c);
}

void wisp_cond_wait(wisp_cond *c, wisp_mutex *m) { pthread_cond_wait(&c->c, &m->m); }

bool wisp_cond_wait_ms(wisp_cond *c, wisp_mutex *m, uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}

void wisp_cond_signal(wisp_cond *c) { pthread_cond_signal(&c->c); }
void wisp_cond_broadcast(wisp_cond *c) { pthread_cond_broadcast(&c->c); }

uint64_t wisp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void wisp_sleep_ms(uint32_t ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static char *home_join(const char *env, const char *fallback, const char *tail) {
    const char *base = env ? getenv(env) : NULL;
    char *root;
    if (base && *base) {
        root = wisp_strdup(base);
    } else {
        const char *home = getenv("HOME");
        root = wisp_path_join(home ? home : ".", fallback);
    }
    char *out = wisp_path_join(root, tail);
    free(root);
    return out;
}

char *wisp_dir_path(wisp_dir which) {
    char *path = NULL;
#ifdef __APPLE__
    switch (which) {
    case WISP_DIR_CONFIG:
    case WISP_DIR_DATA:
        path = home_join(NULL, "Library/Application Support", "wisp");
        break;
    case WISP_DIR_CACHE:
        path = home_join(NULL, "Library/Caches", "wisp");
        break;
    }
#else
    switch (which) {
    case WISP_DIR_CONFIG:
        path = home_join("XDG_CONFIG_HOME", ".config", "wisp");
        break;
    case WISP_DIR_DATA:
        path = home_join("XDG_DATA_HOME", ".local/share", "wisp");
        break;
    case WISP_DIR_CACHE:
        path = home_join("XDG_CACHE_HOME", ".cache", "wisp");
        break;
    }
#endif
    if (path)
        wisp_mkdirs(path);
    return path;
}

FILE *wisp_fopen(const char *path, const char *mode) { return fopen(path, mode); }
FILE *wisp_fopen_shared(const char *path, const char *mode) { return fopen(path, mode); }

bool wisp_mkdirs(const char *path) {
    char *copy = wisp_strdup(path);
    if (!copy)
        return false;
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(copy, 0755);
            *p = '/';
        }
    }
    bool ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

void wisp_dir_list(const char *dir, void (*cb)(void *ctx, const char *name), void *ctx) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            cb(ctx, e->d_name);
    closedir(d);
}

bool wisp_file_stat(const char *path, int64_t *out_size, int64_t *out_mtime) {
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    if (out_size)
        *out_size = (int64_t)st.st_size;
    if (out_mtime)
        *out_mtime = (int64_t)st.st_mtime;
    return true;
}

bool wisp_file_delete(const char *path) { return unlink(path) == 0; }

void wisp_plat_system_cas(void (*add_der)(void *ctx, const unsigned char *der, size_t len),
                          void *ctx) {
    (void)add_der;
    (void)ctx;
}

char *wisp_plat_ca_bundle_path(void) {
    const char *paths[] = {"/etc/ssl/certs/ca-certificates.crt",
                           "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem",
                           "/usr/local/etc/openssl/cert.pem", NULL};
    for (int i = 0; paths[i]; i++)
        if (wisp_file_exists(paths[i]))
            return wisp_strdup(paths[i]);
    return NULL;
}

bool wisp_file_write(const char *path, const void *data, size_t len, bool private_perms) {
    char *tmp = wisp_aprintf("%s.tmp", path);
    if (!tmp)
        return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, private_perms ? 0600 : 0644);
    bool ok = fd >= 0;
    for (size_t off = 0; ok && off < len;) {
        ssize_t n = write(fd, (const char *)data + off, len - off);
        if (n <= 0) {
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    if (ok)
        ok = rename(tmp, path) == 0;
    free(tmp);
    return ok;
}

#endif
