#ifndef WISP_CORE_H
#define WISP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct wisp_core wisp_core;
typedef struct wisp_source wisp_source;

typedef wisp_source *(*wisp_source_provider)(void *ctx, const char *item);

typedef enum {
    WISP_STATE_STOPPED,
    WISP_STATE_PLAYING,
    WISP_STATE_PAUSED,
    WISP_STATE_BUFFERING,
} wisp_state;

typedef enum {
    WISP_REPEAT_OFF,
    WISP_REPEAT_ALL,
    WISP_REPEAT_ONE,
} wisp_repeat;

typedef enum {
    WISP_TRACK_STARTED,
    WISP_TRACK_ENDED,
    WISP_TRACK_SKIPPED,
    WISP_TRACK_STOPPED,
} wisp_track_reason;

typedef enum {
    WISP_EV_TRACK_CHANGED,
    WISP_EV_TRACK_ENDED,
    WISP_EV_STATE,
    WISP_EV_ERROR,
} wisp_event_type;

typedef struct {
    wisp_event_type type;
    wisp_state state;
    size_t queue_pos;
    wisp_track_reason reason;
    double played;
} wisp_event;

typedef struct {
    wisp_state state;
    double position;
    double duration;
    float volume;
    bool muted;
    size_t queue_len;
    size_t queue_pos;
    char *title;
    wisp_repeat repeat;
    bool shuffle;
} wisp_status;

typedef uint64_t (*wisp_group_fn)(void *ctx, const char *item);

wisp_core *wisp_core_new(void);
void wisp_core_free(wisp_core *c);
void wisp_core_set_source_provider(wisp_core *c, wisp_source_provider provider, void *ctx);
void wisp_core_set_group_fn(wisp_core *c, wisp_group_fn fn, void *ctx);
void wisp_core_set_crossfade(wisp_core *c, double seconds);

void wisp_core_queue_set(wisp_core *c, const char **paths, size_t n);
void wisp_core_queue_add(wisp_core *c, const char **paths, size_t n);
void wisp_core_queue_play(wisp_core *c, const char **paths, size_t n, size_t start);
void wisp_core_play(wisp_core *c);
void wisp_core_pause(wisp_core *c);
void wisp_core_toggle_pause(wisp_core *c);
void wisp_core_stop(wisp_core *c);
void wisp_core_next(wisp_core *c);
void wisp_core_prev(wisp_core *c);
void wisp_core_jump_to(wisp_core *c, size_t queue_index);
void wisp_core_seek_to(wisp_core *c, double seconds);
void wisp_core_seek_by(wisp_core *c, double delta);
void wisp_core_set_volume(wisp_core *c, float volume);
void wisp_core_set_muted(wisp_core *c, bool muted);
void wisp_core_set_repeat(wisp_core *c, wisp_repeat repeat);
void wisp_core_cycle_repeat(wisp_core *c);
void wisp_core_set_shuffle(wisp_core *c, bool shuffle);
void wisp_core_toggle_shuffle(wisp_core *c);

wisp_status wisp_core_status(wisp_core *c);
int wisp_core_queue_window(wisp_core *c, size_t *out, int max, int *cur);
void wisp_status_free(wisp_status *s);
bool wisp_core_poll_event(wisp_core *c, wisp_event *out);

float wisp_core_level(wisp_core *c);
int wisp_core_viz(wisp_core *c, float *out, int max);
int wisp_core_spectrum(wisp_core *c, float *out, int n);
int wisp_core_pcm(wisp_core *c, float *out, int n);

#endif
