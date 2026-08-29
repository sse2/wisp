#include "core.h"

#include "audio/engine.h"
#include "audio/output.h"
#include "audio/source.h"
#include "common.h"
#include "plat/plat.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum { INTENT_STOPPED, INTENT_PLAYING, INTENT_PAUSED } intent;

typedef enum {
    M_QUEUE_SET,
    M_QUEUE_ADD,
    M_QUEUE_PLAY,
    M_PLAY,
    M_PAUSE,
    M_TOGGLE,
    M_STOP,
    M_NEXT,
    M_PREV,
    M_JUMP,
    M_SEEK_TO,
    M_SEEK_BY,
    M_SET_VOLUME,
    M_SET_MUTED,
    M_SET_REPEAT,
    M_SET_SHUFFLE,
    M_QUERY,
} msg_type;

typedef struct {
    msg_type type;
    char **paths;
    size_t path_count;
    size_t index;
    double seconds;
    float volume;
    bool flag;
} core_msg;

struct wisp_core {
    wisp_output *out;
    wisp_engine *engine;

    wisp_chan *msgs;
    wisp_chan *events;
    wisp_thread *thread;
    atomic_bool running;

    char **queue;
    size_t queue_len;
    size_t cursor;
    intent intent;
    uint64_t token_next;
    uint64_t cur_token;
    bool pending_advance;
    bool started;

    size_t *order;
    size_t order_len;
    size_t order_pos;
    wisp_repeat repeat;
    bool shuffle;
    uint64_t rng;

    bool na_armed;
    size_t na_cursor;
    size_t na_order_pos;
    uint64_t na_token;

    bool muted;
    float saved_volume;
    double crossfade;

    wisp_state cur_state;
    uint64_t last_pos_token;
    double last_pos;
    uint64_t last_pos_ms;
    uint64_t last_keepalive_ms;

    wisp_mutex *qmtx;
    wisp_cond *qcond;
    bool qpending;
    wisp_status qresult;
    size_t qwin[128];
    int qwin_count;
    int qwin_cur;

    wisp_source_provider provider;
    void *provider_ctx;
    wisp_group_fn group;
    void *group_ctx;
};

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static void push(wisp_core *c, wisp_event ev) {
    wisp_chan_try_send(c->events, &ev);
}
static void push_state(wisp_core *c) {
    push(c, (wisp_event){.type = WISP_EV_STATE, .state = c->cur_state});
}
static void push_track(wisp_core *c, wisp_track_reason r) {
    c->started = true;
    push(c, (wisp_event){.type = WISP_EV_TRACK_CHANGED, .queue_pos = c->cursor, .reason = r});
}
static void end_current(wisp_core *c, wisp_track_reason r) {
    if (!c->started)
        return;
    c->started = false;
    double played = c->last_pos > 0 ? c->last_pos : 0;
    push(c,
         (wisp_event){
             .type = WISP_EV_TRACK_ENDED, .queue_pos = c->cursor, .reason = r, .played = played});
}
static void push_error(wisp_core *c) {
    push(c, (wisp_event){.type = WISP_EV_ERROR});
}

static void free_queue(wisp_core *c) {
    for (size_t i = 0; i < c->queue_len; i++)
        free(c->queue[i]);
    free(c->queue);
    c->queue = NULL;
    c->queue_len = 0;
    free(c->order);
    c->order = NULL;
    c->order_len = 0;
    c->order_pos = 0;
}

static uint64_t rng_next(wisp_core *c) {
    uint64_t x = c->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return c->rng = x ? x : 0x9E3779B97F4A7C15ull;
}

static void build_order(wisp_core *c, size_t start) {
    free(c->order);
    c->order_len = c->queue_len;
    c->order = c->queue_len ? malloc(c->queue_len * sizeof(size_t)) : NULL;
    for (size_t i = 0; i < c->queue_len; i++)
        c->order[i] = i;
    if (c->shuffle && c->queue_len > 1) {
        for (size_t i = c->queue_len - 1; i > 0; i--) {
            size_t j = (size_t)(rng_next(c) % (i + 1));
            size_t t = c->order[i];
            c->order[i] = c->order[j];
            c->order[j] = t;
        }
        for (size_t i = 0; i < c->queue_len; i++)
            if (c->order[i] == start) {
                c->order[i] = c->order[0];
                c->order[0] = start;
                break;
            }
        c->order_pos = 0;
    } else {
        c->order_pos = start < c->queue_len ? start : 0;
    }
    c->cursor = c->order_len ? c->order[c->order_pos] : 0;
}

static bool order_next(wisp_core *c, size_t pos, size_t *np) {
    if (c->order_len == 0)
        return false;
    if (c->repeat == WISP_REPEAT_ONE) {
        *np = pos;
        return true;
    }
    if (pos + 1 < c->order_len) {
        *np = pos + 1;
        return true;
    }
    if (c->repeat == WISP_REPEAT_ALL) {
        *np = 0;
        return true;
    }
    return false;
}

static bool order_prev(wisp_core *c, size_t pos, size_t *pp) {
    if (c->order_len == 0)
        return false;
    if (pos > 0) {
        *pp = pos - 1;
        return true;
    }
    if (c->repeat == WISP_REPEAT_ALL) {
        *pp = c->order_len - 1;
        return true;
    }
    return false;
}

static wisp_source *resolve(wisp_core *c, const char *item) {
    return c->provider ? c->provider(c->provider_ctx, item) : wisp_source_file(item);
}

static void load_track(wisp_core *c, wisp_track_reason reason) {
    c->pending_advance = false;
    c->last_pos = -1.0;
    c->last_pos_ms = wisp_now_ms();
    c->na_armed = false;
    if (c->cursor >= c->queue_len) {
        c->intent = INTENT_STOPPED;
        wisp_engine_stop(c->engine);
        return;
    }
    wisp_source *src = resolve(c, c->queue[c->cursor]);
    c->cur_token = ++c->token_next;
    if (!src) {
        wisp_log("core load_track cursor=%zu item=%s token=%llu resolve NULL -> error", c->cursor,
                 c->queue[c->cursor], (unsigned long long)c->cur_token);
        push_error(c);
        return;
    }
    wisp_log("core load_track cursor=%zu item=%s token=%llu reason=%d", c->cursor,
             c->queue[c->cursor], (unsigned long long)c->cur_token, (int)reason);
    wisp_engine_open(c->engine, src, c->cur_token);
    if (c->intent == INTENT_PLAYING)
        wisp_engine_play(c->engine);
    push_track(c, reason);
}

static void arm_next(wisp_core *c) {
    if (c->intent != INTENT_PLAYING)
        return;
    size_t np;
    if (!order_next(c, c->order_pos, &np))
        return;
    if (c->na_armed && c->na_order_pos == np)
        return;
    size_t nx = c->order[np];
    wisp_source *src = resolve(c, c->queue[nx]);
    if (!src)
        return;
    bool splice_only = false;
    if (c->group && nx != c->cursor) {
        uint64_t g0 = c->group(c->group_ctx, c->queue[c->cursor]);
        uint64_t g1 = c->group(c->group_ctx, c->queue[nx]);
        splice_only = g0 != 0 && g0 == g1;
    }
    uint64_t tok = ++c->token_next;
    wisp_engine_set_next(c->engine, src, tok, splice_only);
    c->na_armed = true;
    c->na_cursor = nx;
    c->na_order_pos = np;
    c->na_token = tok;
}

static void skip_to_pos(wisp_core *c, size_t pos, wisp_track_reason reason) {
    end_current(c, WISP_TRACK_SKIPPED);
    if (c->na_armed) {
        wisp_engine_cancel_next(c->engine);
        c->na_armed = false;
    }
    c->order_pos = pos;
    c->cursor = c->order[pos];
    load_track(c, reason);
}

static void reorder_live(wisp_core *c) {
    build_order(c, c->cursor);
    if (c->na_armed) {
        wisp_engine_cancel_next(c->engine);
        c->na_armed = false;
    }
    arm_next(c);
}

static wisp_state derive_state(wisp_core *c) {
    if (c->intent == INTENT_STOPPED)
        return WISP_STATE_STOPPED;
    if (c->intent == INTENT_PAUSED)
        return WISP_STATE_PAUSED;
    return (wisp_now_ms() - c->last_pos_ms > 150) ? WISP_STATE_BUFFERING : WISP_STATE_PLAYING;
}

static void fill_status(wisp_core *c, wisp_status *s) {
    wisp_engine_pos p = wisp_engine_position(c->engine);
    s->state = derive_state(c);
    s->position = p.seconds < 0 ? 0 : p.seconds;
    s->duration = 0;
    s->volume = c->muted ? c->saved_volume : wisp_output_volume(c->out);
    s->muted = c->muted;
    s->queue_len = c->queue_len;
    s->queue_pos = c->cursor;
    s->title = c->cursor < c->queue_len ? wisp_strdup(base_name(c->queue[c->cursor])) : NULL;
    s->repeat = c->repeat;
    s->shuffle = c->shuffle;
}

static void handle(wisp_core *c, core_msg *m) {
    switch (m->type) {
    case M_QUEUE_SET:
        end_current(c, WISP_TRACK_SKIPPED);
        if (c->na_armed) {
            wisp_engine_cancel_next(c->engine);
            c->na_armed = false;
        }
        free_queue(c);
        c->queue = m->paths;
        c->queue_len = m->path_count;
        build_order(c, 0);
        if (c->intent == INTENT_PLAYING)
            load_track(c, WISP_TRACK_STARTED);
        break;
    case M_QUEUE_PLAY:
        end_current(c, WISP_TRACK_SKIPPED);
        if (c->na_armed) {
            wisp_engine_cancel_next(c->engine);
            c->na_armed = false;
        }
        free_queue(c);
        c->queue = m->paths;
        c->queue_len = m->path_count;
        build_order(c, m->index < m->path_count ? m->index : 0);
        c->intent = INTENT_PLAYING;
        load_track(c, WISP_TRACK_STARTED);
        break;
    case M_QUEUE_ADD: {
        char **grown = realloc(c->queue, (c->queue_len + m->path_count) * sizeof(char *));
        if (grown) {
            c->queue = grown;
            for (size_t i = 0; i < m->path_count; i++)
                c->queue[c->queue_len + i] = m->paths[i];
            c->queue_len += m->path_count;
        }
        free(m->paths);
        reorder_live(c);
        break;
    }
    case M_PLAY:
        if (c->intent == INTENT_PAUSED) {
            c->intent = INTENT_PLAYING;
            wisp_engine_play(c->engine);
        } else if (c->queue_len > 0) {
            c->intent = INTENT_PLAYING;
            load_track(c, WISP_TRACK_STARTED);
        }
        break;
    case M_PAUSE:
        if (c->intent == INTENT_PLAYING) {
            c->intent = INTENT_PAUSED;
            wisp_engine_pause(c->engine);
        }
        break;
    case M_TOGGLE:
        if (c->intent == INTENT_PLAYING) {
            c->intent = INTENT_PAUSED;
            wisp_engine_pause(c->engine);
        } else if (c->intent == INTENT_PAUSED) {
            c->intent = INTENT_PLAYING;
            wisp_engine_play(c->engine);
        } else if (c->queue_len > 0) {
            c->intent = INTENT_PLAYING;
            load_track(c, WISP_TRACK_STARTED);
        }
        break;
    case M_STOP:
        end_current(c, WISP_TRACK_SKIPPED);
        if (c->na_armed) {
            wisp_engine_cancel_next(c->engine);
            c->na_armed = false;
        }
        c->intent = INTENT_STOPPED;
        wisp_engine_stop(c->engine);
        break;
    case M_NEXT: {
        size_t np;
        bool has = false;
        if (c->order_pos + 1 < c->order_len) {
            np = c->order_pos + 1;
            has = true;
        } else if (c->repeat == WISP_REPEAT_ALL && c->order_len > 0) {
            np = 0;
            has = true;
        }
        if (has) {
            skip_to_pos(c, np, WISP_TRACK_SKIPPED);
        } else {
            end_current(c, WISP_TRACK_SKIPPED);
            if (c->na_armed) {
                wisp_engine_cancel_next(c->engine);
                c->na_armed = false;
            }
            c->intent = INTENT_STOPPED;
            wisp_engine_stop(c->engine);
        }
        break;
    }
    case M_PREV: {
        size_t pp;
        if (order_prev(c, c->order_pos, &pp))
            skip_to_pos(c, pp, WISP_TRACK_SKIPPED);
        else
            wisp_engine_seek(c->engine, 0);
        break;
    }
    case M_JUMP:
        for (size_t p = 0; p < c->order_len; p++)
            if (c->order[p] == m->index) {
                c->intent = INTENT_PLAYING;
                skip_to_pos(c, p, WISP_TRACK_SKIPPED);
                break;
            }
        break;
    case M_SET_REPEAT:
        c->repeat = (wisp_repeat)m->index;
        if (c->na_armed) {
            wisp_engine_cancel_next(c->engine);
            c->na_armed = false;
        }
        arm_next(c);
        break;
    case M_SET_SHUFFLE:
        if (c->shuffle != m->flag) {
            c->shuffle = m->flag;
            if (c->queue_len > 0)
                reorder_live(c);
        }
        break;
    case M_SEEK_TO:
        wisp_engine_seek(c->engine, m->seconds < 0 ? 0 : m->seconds);
        break;
    case M_SEEK_BY: {
        double t = wisp_engine_position(c->engine).seconds + m->seconds;
        wisp_engine_seek(c->engine, t < 0 ? 0 : t);
        break;
    }
    case M_SET_VOLUME:
        c->muted = false;
        wisp_output_set_volume(c->out, m->volume);
        break;
    case M_SET_MUTED:
        if (m->flag && !c->muted) {
            c->saved_volume = wisp_output_volume(c->out);
            wisp_output_set_volume(c->out, 0);
            c->muted = true;
        } else if (!m->flag && c->muted) {
            wisp_output_set_volume(c->out, c->saved_volume);
            c->muted = false;
        }
        break;
    case M_QUERY: {
        wisp_mutex_lock(c->qmtx);
        fill_status(c, &c->qresult);
        int cap = 128;
        size_t start = c->order_pos > 16 ? c->order_pos - 16 : 0;
        c->qwin_count = 0;
        c->qwin_cur = (int)(c->order_pos - start);
        for (size_t i = start; i < c->order_len && c->qwin_count < cap; i++)
            c->qwin[c->qwin_count++] = c->order[i];
        c->qpending = false;
        wisp_cond_signal(c->qcond);
        wisp_mutex_unlock(c->qmtx);
        break;
    }
    }
}

static void poll_engine(wisp_core *c) {
    wisp_engine_event ev;
    while (wisp_engine_poll(c->engine, &ev)) {
        switch (ev.type) {
        case WISP_ENGINE_NEED_NEXT:
            if (ev.token == c->cur_token)
                arm_next(c);
            break;
        case WISP_ENGINE_ADVANCED:
            if (c->na_armed && ev.token == c->na_token) {
                end_current(c, WISP_TRACK_ENDED);
                c->cursor = c->na_cursor;
                c->order_pos = c->na_order_pos;
                c->cur_token = c->na_token;
                c->na_armed = false;
                c->last_pos = -1.0;
                c->last_pos_ms = wisp_now_ms();
                push_track(c, WISP_TRACK_ENDED);
            }
            break;
        case WISP_ENGINE_FINISHED:
            if (ev.token == c->cur_token)
                c->pending_advance = true;
            break;
        case WISP_ENGINE_ERROR:
            wisp_log("core engine ERROR token=%llu (current=%llu)", (unsigned long long)ev.token,
                     (unsigned long long)c->cur_token);
            if (ev.token == c->cur_token) {
                push_error(c);
                c->intent = INTENT_STOPPED;
            }
            break;
        }
    }
    if (c->pending_advance && c->intent == INTENT_PLAYING && wisp_engine_drained(c->engine)) {
        c->pending_advance = false;
        size_t np;
        if (order_next(c, c->order_pos, &np)) {
            end_current(c, WISP_TRACK_ENDED);
            c->order_pos = np;
            c->cursor = c->order[np];
            load_track(c, WISP_TRACK_ENDED);
        } else {
            end_current(c, WISP_TRACK_ENDED);
            c->intent = INTENT_STOPPED;
            wisp_engine_stop(c->engine);
        }
    }
}

static void tick_state(wisp_core *c) {
    wisp_engine_pos p = wisp_engine_position(c->engine);
    uint64_t now = wisp_now_ms();
    if (p.token != c->last_pos_token) {
        c->last_pos_token = p.token;
        c->last_pos = -1.0;
        c->last_pos_ms = now;
    }
    if (p.seconds > c->last_pos + 1e-6 || p.seconds + 0.5 < c->last_pos) {
        c->last_pos = p.seconds;
        c->last_pos_ms = now;
    }
    wisp_state s = derive_state(c);
    if (s != c->cur_state) {
        c->cur_state = s;
        push_state(c);
        c->last_keepalive_ms = now;
    } else if (now - c->last_keepalive_ms >= 1000) {
        c->last_keepalive_ms = now;
        push_state(c);
    }
}

static void player_run(void *p) {
    wisp_core *c = p;
    while (atomic_load(&c->running)) {
        core_msg m;
        if (wisp_chan_recv_timeout(c->msgs, &m, 15)) {
            handle(c, &m);
            while (wisp_chan_try_recv(c->msgs, &m))
                handle(c, &m);
        }
        if (!atomic_load(&c->running))
            break;
        poll_engine(c);
        tick_state(c);
    }
}

wisp_core *wisp_core_new(void) {
    wisp_core *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->out = wisp_output_new();
    if (!c->out) {
        free(c);
        return NULL;
    }
    c->engine = wisp_engine_new(c->out);
    c->msgs = wisp_chan_new(sizeof(core_msg), 128);
    c->events = wisp_chan_new(sizeof(wisp_event), 256);
    c->qmtx = wisp_mutex_new();
    c->qcond = wisp_cond_new();
    if (!c->engine || !c->msgs || !c->events || !c->qmtx || !c->qcond) {
        wisp_core_free(c);
        return NULL;
    }
    c->rng = wisp_now_ms() * 2654435761ull + 0x9E3779B97F4A7C15ull;
    atomic_init(&c->running, true);
    c->thread = wisp_thread_start(player_run, c);
    if (!c->thread) {
        atomic_store(&c->running, false);
        wisp_core_free(c);
        return NULL;
    }
    return c;
}

void wisp_core_free(wisp_core *c) {
    if (!c)
        return;
    if (c->thread) {
        atomic_store(&c->running, false);
        wisp_thread_join(c->thread);
    }
    if (c->engine)
        wisp_engine_free(c->engine);
    if (c->out)
        wisp_output_free(c->out);
    free_queue(c);
    if (c->msgs)
        wisp_chan_free(c->msgs);
    if (c->events)
        wisp_chan_free(c->events);
    if (c->qmtx)
        wisp_mutex_free(c->qmtx);
    if (c->qcond)
        wisp_cond_free(c->qcond);
    free(c);
}

void wisp_core_set_source_provider(wisp_core *c, wisp_source_provider provider, void *ctx) {
    c->provider = provider;
    c->provider_ctx = ctx;
}

void wisp_core_set_group_fn(wisp_core *c, wisp_group_fn fn, void *ctx) {
    c->group = fn;
    c->group_ctx = ctx;
}

void wisp_core_set_crossfade(wisp_core *c, double seconds) {
    c->crossfade = seconds < 0 ? 0 : seconds;
    wisp_engine_set_crossfade(c->engine, c->crossfade);
}

static void send(wisp_core *c, core_msg m) {
    wisp_chan_send(c->msgs, &m);
}

static char **dup_paths(const char **paths, size_t n) {
    char **out = malloc(n * sizeof(char *));
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++)
        out[i] = wisp_strdup(paths[i]);
    return out;
}

void wisp_core_queue_set(wisp_core *c, const char **paths, size_t n) {
    send(c, (core_msg){.type = M_QUEUE_SET, .paths = dup_paths(paths, n), .path_count = n});
}
void wisp_core_queue_add(wisp_core *c, const char **paths, size_t n) {
    send(c, (core_msg){.type = M_QUEUE_ADD, .paths = dup_paths(paths, n), .path_count = n});
}
void wisp_core_queue_play(wisp_core *c, const char **paths, size_t n, size_t start) {
    send(c,
         (core_msg){
             .type = M_QUEUE_PLAY, .paths = dup_paths(paths, n), .path_count = n, .index = start});
}
void wisp_core_play(wisp_core *c) {
    send(c, (core_msg){.type = M_PLAY});
}
void wisp_core_pause(wisp_core *c) {
    send(c, (core_msg){.type = M_PAUSE});
}
void wisp_core_toggle_pause(wisp_core *c) {
    send(c, (core_msg){.type = M_TOGGLE});
}
void wisp_core_stop(wisp_core *c) {
    send(c, (core_msg){.type = M_STOP});
}
void wisp_core_next(wisp_core *c) {
    send(c, (core_msg){.type = M_NEXT});
}
void wisp_core_prev(wisp_core *c) {
    send(c, (core_msg){.type = M_PREV});
}
void wisp_core_jump_to(wisp_core *c, size_t queue_index) {
    send(c, (core_msg){.type = M_JUMP, .index = queue_index});
}
void wisp_core_seek_to(wisp_core *c, double seconds) {
    send(c, (core_msg){.type = M_SEEK_TO, .seconds = seconds});
}
void wisp_core_seek_by(wisp_core *c, double delta) {
    send(c, (core_msg){.type = M_SEEK_BY, .seconds = delta});
}
void wisp_core_set_volume(wisp_core *c, float volume) {
    send(c, (core_msg){.type = M_SET_VOLUME, .volume = volume});
}
void wisp_core_set_muted(wisp_core *c, bool muted) {
    send(c, (core_msg){.type = M_SET_MUTED, .flag = muted});
}
void wisp_core_set_repeat(wisp_core *c, wisp_repeat repeat) {
    send(c, (core_msg){.type = M_SET_REPEAT, .index = (size_t)repeat});
}
void wisp_core_cycle_repeat(wisp_core *c) {
    wisp_status s = wisp_core_status(c);
    wisp_repeat next = s.repeat == WISP_REPEAT_OFF   ? WISP_REPEAT_ALL
                       : s.repeat == WISP_REPEAT_ALL ? WISP_REPEAT_ONE
                                                     : WISP_REPEAT_OFF;
    wisp_status_free(&s);
    wisp_core_set_repeat(c, next);
}
void wisp_core_set_shuffle(wisp_core *c, bool shuffle) {
    send(c, (core_msg){.type = M_SET_SHUFFLE, .flag = shuffle});
}
void wisp_core_toggle_shuffle(wisp_core *c) {
    wisp_status s = wisp_core_status(c);
    bool ns = !s.shuffle;
    wisp_status_free(&s);
    wisp_core_set_shuffle(c, ns);
}

wisp_status wisp_core_status(wisp_core *c) {
    wisp_mutex_lock(c->qmtx);
    c->qpending = true;
    core_msg m = {.type = M_QUERY};
    wisp_chan_send(c->msgs, &m);
    while (c->qpending)
        wisp_cond_wait(c->qcond, c->qmtx);
    wisp_status s = c->qresult;
    wisp_mutex_unlock(c->qmtx);
    return s;
}

int wisp_core_queue_window(wisp_core *c, size_t *out, int max, int *cur) {
    wisp_mutex_lock(c->qmtx);
    c->qpending = true;
    core_msg m = {.type = M_QUERY};
    wisp_chan_send(c->msgs, &m);
    while (c->qpending)
        wisp_cond_wait(c->qcond, c->qmtx);
    int n = c->qwin_count < max ? c->qwin_count : max;
    for (int i = 0; i < n; i++)
        out[i] = c->qwin[i];
    if (cur)
        *cur = c->qwin_cur;
    wisp_mutex_unlock(c->qmtx);
    return n;
}

void wisp_status_free(wisp_status *s) {
    free(s->title);
    s->title = NULL;
}

bool wisp_core_poll_event(wisp_core *c, wisp_event *out) {
    return wisp_chan_try_recv(c->events, out);
}

float wisp_core_level(wisp_core *c) {
    return wisp_output_level(c->out);
}
int wisp_core_viz(wisp_core *c, float *out, int max) {
    return wisp_output_viz(c->out, out, max);
}
int wisp_core_spectrum(wisp_core *c, float *out, int n) {
    return wisp_output_spectrum(c->out, out, n);
}
int wisp_core_pcm(wisp_core *c, float *out, int n) {
    return wisp_output_pcm(c->out, out, n);
}
