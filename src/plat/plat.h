#ifndef WISP_PLAT_H
#define WISP_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define WISP_SEP '\\'
#else
#define WISP_SEP '/'
#endif

typedef struct wisp_thread wisp_thread;
typedef struct wisp_mutex wisp_mutex;
typedef struct wisp_cond wisp_cond;
typedef struct wisp_chan wisp_chan;

wisp_thread *wisp_thread_start(void (*entry)(void *), void *arg);
void wisp_thread_join(wisp_thread *t);

wisp_mutex *wisp_mutex_new(void);
void wisp_mutex_free(wisp_mutex *m);
void wisp_mutex_lock(wisp_mutex *m);
void wisp_mutex_unlock(wisp_mutex *m);

wisp_cond *wisp_cond_new(void);
void wisp_cond_free(wisp_cond *c);
void wisp_cond_wait(wisp_cond *c, wisp_mutex *m);
bool wisp_cond_wait_ms(wisp_cond *c, wisp_mutex *m, uint32_t ms);
void wisp_cond_signal(wisp_cond *c);
void wisp_cond_broadcast(wisp_cond *c);

wisp_chan *wisp_chan_new(size_t elem_size, size_t capacity);
void wisp_chan_free(wisp_chan *ch);
bool wisp_chan_send(wisp_chan *ch, const void *elem);
bool wisp_chan_try_send(wisp_chan *ch, const void *elem);
bool wisp_chan_recv(wisp_chan *ch, void *out);
bool wisp_chan_recv_timeout(wisp_chan *ch, void *out, uint32_t ms);
bool wisp_chan_try_recv(wisp_chan *ch, void *out);
size_t wisp_chan_len(wisp_chan *ch);
void wisp_chan_close(wisp_chan *ch);

uint64_t wisp_now_ms(void);
void wisp_sleep_ms(uint32_t ms);

typedef enum { WISP_DIR_CONFIG, WISP_DIR_DATA, WISP_DIR_CACHE } wisp_dir;
char *wisp_dir_path(wisp_dir which);

FILE *wisp_fopen(const char *path, const char *mode);
FILE *wisp_fopen_shared(const char *path, const char *mode);
bool wisp_mkdirs(const char *path);
bool wisp_file_exists(const char *path);
bool wisp_file_read(const char *path, void **out_data, size_t *out_len);
bool wisp_file_write(const char *path, const void *data, size_t len, bool private_perms);
char *wisp_path_join(const char *a, const char *b);
void wisp_dir_list(const char *dir, void (*cb)(void *ctx, const char *name), void *ctx);
bool wisp_file_stat(const char *path, int64_t *out_size, int64_t *out_mtime);
bool wisp_file_delete(const char *path);

void wisp_plat_system_cas(void (*add_der)(void *ctx, const unsigned char *der, size_t len),
                          void *ctx);
char *wisp_plat_ca_bundle_path(void);

#endif
