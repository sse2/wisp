#ifndef WISP_CACHE_H
#define WISP_CACHE_H

#include "audio/source.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct wisp_cache wisp_cache;

wisp_cache *wisp_cache_new(const char *cache_dir, bool trust_tls);
void wisp_cache_free(wisp_cache *c);

wisp_source *wisp_cache_open(wisp_cache *c, const char *key, const char *url,
                             int64_t expected_size);

void wisp_cache_set_limit(wisp_cache *c, int64_t bytes);
int64_t wisp_cache_usage(wisp_cache *c);

void wisp_cache_pin(wisp_cache *c, const char *key, const char *url, int64_t size);
void wisp_cache_unpin(wisp_cache *c, const char *key);
bool wisp_cache_is_pinned(wisp_cache *c, const char *key);

typedef struct {
    char *key;
    int64_t committed;
    int64_t total;
    bool complete;
} wisp_cache_stat;

size_t wisp_cache_stats(wisp_cache *c, wisp_cache_stat **out);
void wisp_cache_stats_free(wisp_cache_stat *list, size_t n);

#endif
