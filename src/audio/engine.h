#ifndef WISP_AUDIO_ENGINE_H
#define WISP_AUDIO_ENGINE_H

#include "output.h"
#include "source.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct wisp_engine wisp_engine;

typedef enum {
    WISP_ENGINE_NEED_NEXT,
    WISP_ENGINE_ADVANCED,
    WISP_ENGINE_FINISHED,
    WISP_ENGINE_ERROR,
} wisp_engine_event_type;

typedef struct {
    wisp_engine_event_type type;
    uint64_t token;
} wisp_engine_event;

typedef struct {
    uint64_t token;
    double seconds;
    bool playing;
} wisp_engine_pos;

wisp_engine *wisp_engine_new(wisp_output *out);
void wisp_engine_free(wisp_engine *e);

void wisp_engine_open(wisp_engine *e, wisp_source *src, uint64_t token);
void wisp_engine_set_next(wisp_engine *e, wisp_source *src, uint64_t token, bool splice_only);
void wisp_engine_cancel_next(wisp_engine *e);
void wisp_engine_set_crossfade(wisp_engine *e, double seconds);
void wisp_engine_play(wisp_engine *e);
void wisp_engine_pause(wisp_engine *e);
void wisp_engine_stop(wisp_engine *e);
void wisp_engine_seek(wisp_engine *e, double seconds);

bool wisp_engine_poll(wisp_engine *e, wisp_engine_event *out);
wisp_engine_pos wisp_engine_position(wisp_engine *e);
bool wisp_engine_drained(wisp_engine *e);

#endif
