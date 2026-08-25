#ifndef WISP_AUDIO_OUTPUT_H
#define WISP_AUDIO_OUTPUT_H

#include "ring.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct wisp_output wisp_output;

typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
} wisp_audio_format;

typedef struct {
    char *name;
    void *id;
    bool is_default;
} wisp_device_info;

wisp_output *wisp_output_new(void);
void wisp_output_free(wisp_output *o);
bool wisp_output_start(wisp_output *o);
void wisp_output_stop(wisp_output *o);

wisp_audio_format wisp_output_format(wisp_output *o);
wisp_ring *wisp_output_ring(wisp_output *o);

void wisp_output_set_volume(wisp_output *o, float v);
float wisp_output_volume(wisp_output *o);

uint64_t wisp_output_flush(wisp_output *o);
uint64_t wisp_output_flush_ack(wisp_output *o);

uint64_t wisp_output_frames_played(wisp_output *o);
uint32_t wisp_output_underruns(wisp_output *o);
void wisp_output_set_draining(wisp_output *o, bool draining);

#define WISP_VIZ_BINS 64
float wisp_output_level(wisp_output *o);
int wisp_output_viz(wisp_output *o, float *out, int max);
int wisp_output_spectrum(wisp_output *o, float *out, int n);
int wisp_output_pcm(wisp_output *o, float *out, int n);

int wisp_output_list_devices(wisp_output *o, wisp_device_info **out);
void wisp_output_free_devices(wisp_device_info *list, int count);

#endif
