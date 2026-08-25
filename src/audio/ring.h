#ifndef WISP_AUDIO_RING_H
#define WISP_AUDIO_RING_H

#include <stddef.h>
#include <stdint.h>

typedef struct wisp_ring wisp_ring;

wisp_ring *wisp_ring_new(size_t capacity_frames, uint32_t channels);
void wisp_ring_free(wisp_ring *r);

size_t wisp_ring_write(wisp_ring *r, const float *frames, size_t nframes);
size_t wisp_ring_read(wisp_ring *r, float *out, size_t nframes);
size_t wisp_ring_readable(wisp_ring *r);
size_t wisp_ring_writable(wisp_ring *r);
void wisp_ring_clear(wisp_ring *r);

#endif
