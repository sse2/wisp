#include "ring.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct wisp_ring {
    float *buf;
    size_t cap;
    size_t mask;
    uint32_t ch;
    atomic_size_t head;
    atomic_size_t tail;
};

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

wisp_ring *wisp_ring_new(size_t capacity_frames, uint32_t channels) {
    wisp_ring *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->cap = next_pow2(capacity_frames);
    r->mask = r->cap - 1;
    r->ch = channels;
    r->buf = malloc(r->cap * channels * sizeof(float));
    if (!r->buf) {
        free(r);
        return NULL;
    }
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r;
}

void wisp_ring_free(wisp_ring *r) {
    if (!r)
        return;
    free(r->buf);
    free(r);
}

size_t wisp_ring_readable(wisp_ring *r) {
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    return h - t;
}

size_t wisp_ring_writable(wisp_ring *r) {
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    return r->cap - (h - t);
}

size_t wisp_ring_write(wisp_ring *r, const float *frames, size_t n) {
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t space = r->cap - (h - t);
    if (n > space)
        n = space;
    size_t at = h & r->mask;
    size_t first = r->cap - at;
    if (first > n)
        first = n;
    memcpy(r->buf + at * r->ch, frames, first * r->ch * sizeof(float));
    if (n > first)
        memcpy(r->buf, frames + first * r->ch, (n - first) * r->ch * sizeof(float));
    atomic_store_explicit(&r->head, h + n, memory_order_release);
    return n;
}

size_t wisp_ring_read(wisp_ring *r, float *out, size_t n) {
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t avail = h - t;
    if (n > avail)
        n = avail;
    size_t at = t & r->mask;
    size_t first = r->cap - at;
    if (first > n)
        first = n;
    memcpy(out, r->buf + at * r->ch, first * r->ch * sizeof(float));
    if (n > first)
        memcpy(out + first * r->ch, r->buf, (n - first) * r->ch * sizeof(float));
    atomic_store_explicit(&r->tail, t + n, memory_order_release);
    return n;
}

void wisp_ring_clear(wisp_ring *r) {
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, h, memory_order_release);
}
