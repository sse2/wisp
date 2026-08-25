#ifndef WISP_AUDIO_DECODERS_H
#define WISP_AUDIO_DECODERS_H

#include "source.h"

#include "miniaudio.h"

typedef struct {
    ma_vfs_callbacks cb;
    wisp_source *src;
} wisp_source_vfs;

typedef struct {
    ma_decoder ma;
    wisp_source_vfs vfs;
    wisp_source *src;
    bool open;
} wisp_decoder;

bool wisp_decoder_open(wisp_decoder *d, wisp_source *s, uint32_t rate, uint32_t channels);
void wisp_decoder_close(wisp_decoder *d);
size_t wisp_decoder_read(wisp_decoder *d, float *out, size_t frames, bool *eof);
bool wisp_decoder_seek(wisp_decoder *d, uint64_t frame);
uint64_t wisp_decoder_length(wisp_decoder *d);
uint64_t wisp_decoder_cursor(wisp_decoder *d);

#endif
