#include "engine.h"

#include "decoders.h"
#include "plat/plat.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define WISP_HALF_PI 1.57079632679489661923

typedef enum {
    CMD_OPEN,
    CMD_SET_NEXT,
    CMD_CANCEL_NEXT,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_STOP,
    CMD_SEEK,
    CMD_CROSSFADE,
} cmd_type;

typedef struct {
    cmd_type type;
    wisp_source *src;
    uint64_t token;
    double seconds;
    bool flag;
} engine_cmd;

typedef struct {
    wisp_decoder *dec;
    uint64_t token;
    bool eof;
    float gain;
} chain;

typedef enum { PL_EMPTY, PL_LOADING, PL_OPENING, PL_READY, PL_FAILED } pl_state;

struct wisp_engine {
    wisp_output *out;
    wisp_ring *ring;
    wisp_audio_format fmt;

    wisp_chan *cmds;
    wisp_chan *events;
    wisp_thread *thread;
    atomic_bool running;

    float *chunk;
    float *mixbuf;
    size_t chunk_frames;

    chain cur;
    chain fade_out;
    bool xf_active;
    double xf_len;
    uint64_t xf_pos;
    uint64_t xf_frames;

    bool playing;
    bool finished;
    bool want_start;
    bool device_running;
    size_t prime_frames;

    wisp_mutex *mtx;
    uint64_t anchor_base;
    uint64_t produced;
    uint64_t m_base_abs;
    uint64_t m_offset;
    uint64_t m_token;
    struct {
        uint64_t at;
        uint64_t token;
    } pend[16];
    int pend_n;
    uint64_t asked_token;

    wisp_thread *pl_thread;
    wisp_mutex *pl_mtx;
    wisp_cond *pl_cond;
    pl_state pl_st;
    wisp_source *pl_src;
    uint64_t pl_token;
    bool pl_splice_only;
    uint64_t pl_gen;
    wisp_decoder *pl_dec;

    atomic_bool playing_pub;
    atomic_bool drained;
};

static wisp_decoder *decoder_open_heap(wisp_engine *e, wisp_source *src) {
    wisp_decoder *d = calloc(1, sizeof *d);
    if (!d) {
        if (src)
            src->close(src);
        return NULL;
    }
    if (!wisp_decoder_open(d, src, e->fmt.sample_rate, e->fmt.channels)) {
        wisp_decoder_close(d);
        free(d);
        return NULL;
    }
    return d;
}

static void decoder_free(wisp_decoder *d) {
    if (!d)
        return;
    wisp_decoder_close(d);
    free(d);
}

static void chain_close(chain *c) {
    decoder_free(c->dec);
    c->dec = NULL;
}

static void emit(wisp_engine *e, wisp_engine_event_type type, uint64_t token) {
    wisp_engine_event ev = {.type = type, .token = token};
    wisp_chan_try_send(e->events, &ev);
}

static void set_marker(wisp_engine *e, uint64_t base_abs, uint64_t offset, uint64_t token) {
    wisp_mutex_lock(e->mtx);
    e->m_base_abs = base_abs;
    e->m_offset = offset;
    e->m_token = token;
    wisp_mutex_unlock(e->mtx);
}

static uint64_t flush_and_anchor(wisp_engine *e) {
    uint64_t epoch = wisp_output_flush(e->out);
    if (e->device_running)
        for (int i = 0; i < 100 && wisp_output_flush_ack(e->out) < epoch; i++)
            wisp_sleep_ms(1);
    return wisp_output_frames_played(e->out);
}

static void pl_discard_locked(wisp_engine *e) {
    if (e->pl_st == PL_READY) {
        decoder_free(e->pl_dec);
        e->pl_dec = NULL;
    } else if (e->pl_st == PL_LOADING && e->pl_src) {
        e->pl_src->close(e->pl_src);
    }
    e->pl_gen++;
    e->pl_src = NULL;
    e->pl_st = PL_EMPTY;
}

static void pl_set(wisp_engine *e, wisp_source *src, uint64_t token, bool splice_only) {
    wisp_mutex_lock(e->pl_mtx);
    pl_discard_locked(e);
    e->pl_src = src;
    e->pl_token = token;
    e->pl_splice_only = splice_only;
    e->pl_st = PL_LOADING;
    wisp_cond_signal(e->pl_cond);
    wisp_mutex_unlock(e->pl_mtx);
}

static void pl_cancel(wisp_engine *e) {
    wisp_mutex_lock(e->pl_mtx);
    pl_discard_locked(e);
    wisp_mutex_unlock(e->pl_mtx);
}

static bool pl_take(wisp_engine *e, chain *dst, bool *splice_only) {
    wisp_mutex_lock(e->pl_mtx);
    bool ok = e->pl_st == PL_READY;
    if (ok) {
        dst->dec = e->pl_dec;
        dst->token = e->pl_token;
        dst->eof = false;
        dst->gain = 1.0f;
        if (splice_only)
            *splice_only = e->pl_splice_only;
        e->pl_dec = NULL;
        e->pl_gen++;
        e->pl_src = NULL;
        e->pl_st = PL_EMPTY;
    }
    wisp_mutex_unlock(e->pl_mtx);
    return ok;
}

static bool pl_ready_xfade(wisp_engine *e) {
    wisp_mutex_lock(e->pl_mtx);
    bool r = e->pl_st == PL_READY && !e->pl_splice_only;
    wisp_mutex_unlock(e->pl_mtx);
    return r;
}

static void pl_run(void *p) {
    wisp_engine *e = p;
    wisp_mutex_lock(e->pl_mtx);
    while (atomic_load(&e->running)) {
        while (atomic_load(&e->running) && e->pl_st != PL_LOADING)
            wisp_cond_wait_ms(e->pl_cond, e->pl_mtx, 100);
        if (!atomic_load(&e->running))
            break;
        wisp_source *src = e->pl_src;
        uint64_t gen = e->pl_gen;
        e->pl_st = PL_OPENING;
        wisp_mutex_unlock(e->pl_mtx);

        wisp_decoder *d = decoder_open_heap(e, src);

        wisp_mutex_lock(e->pl_mtx);
        if (e->pl_gen != gen) {
            decoder_free(d);
        } else if (d) {
            e->pl_dec = d;
            e->pl_st = PL_READY;
        } else {
            e->pl_st = PL_FAILED;
        }
    }
    wisp_mutex_unlock(e->pl_mtx);
}

static void ask_next(wisp_engine *e, uint64_t token) {
    if (token == e->asked_token)
        return;
    e->asked_token = token;
    emit(e, WISP_ENGINE_NEED_NEXT, token);
}

static void push_boundary(wisp_engine *e, uint64_t at, uint64_t token) {
    if (e->pend_n < (int)(sizeof e->pend / sizeof e->pend[0])) {
        e->pend[e->pend_n].at = at;
        e->pend[e->pend_n].token = token;
        e->pend_n++;
    }
}

static void fire_boundaries(wisp_engine *e) {
    uint64_t fp = wisp_output_frames_played(e->out);
    uint64_t elapsed = fp >= e->anchor_base ? fp - e->anchor_base : 0;
    while (e->pend_n > 0 && elapsed >= e->pend[0].at) {
        uint64_t tok = e->pend[0].token;
        set_marker(e, e->anchor_base + e->pend[0].at, 0, tok);
        emit(e, WISP_ENGINE_ADVANCED, tok);
        for (int i = 1; i < e->pend_n; i++)
            e->pend[i - 1] = e->pend[i];
        e->pend_n--;
        ask_next(e, tok);
    }
}

static void collapse_pending(wisp_engine *e) {
    while (e->pend_n > 0) {
        uint64_t tok = e->pend[e->pend_n - 1].token;
        set_marker(e, e->anchor_base + e->pend[e->pend_n - 1].at, 0, tok);
        emit(e, WISP_ENGINE_ADVANCED, tok);
        e->pend_n--;
    }
}

static void end_crossfade(wisp_engine *e) {
    chain_close(&e->fade_out);
    e->xf_active = false;
}

static void on_cur_eof(wisp_engine *e) {
    e->cur.eof = true;
    chain nxt = {0};
    bool splice_only = false;
    if (pl_take(e, &nxt, &splice_only)) {
        chain_close(&e->cur);
        e->cur = nxt;
        e->finished = false;
        wisp_output_set_draining(e->out, false);
        push_boundary(e, e->produced, e->cur.token);
    } else {
        e->finished = true;
        emit(e, WISP_ENGINE_FINISHED, e->cur.token);
        wisp_output_set_draining(e->out, true);
    }
}

static void begin_crossfade(wisp_engine *e) {
    chain inc = {0};
    if (!pl_take(e, &inc, NULL))
        return;
    uint64_t len_out = wisp_decoder_length(e->cur.dec);
    uint64_t cur_out = wisp_decoder_cursor(e->cur.dec);
    uint64_t rem_out = len_out > cur_out ? len_out - cur_out : 0;
    uint64_t xff = (uint64_t)(e->xf_len * e->fmt.sample_rate);
    if (rem_out > 0 && rem_out < xff)
        xff = rem_out;

    e->fade_out = e->cur;
    e->fade_out.eof = false;
    e->cur = inc;
    push_boundary(e, e->produced, e->cur.token);
    e->xf_frames = xff > 0 ? xff : 1;
    e->xf_pos = 0;
    e->xf_active = true;
    e->finished = false;
    wisp_output_set_draining(e->out, false);
}

static void feed_plain(wisp_engine *e) {
    size_t writable = wisp_ring_writable(e->ring);
    if (writable < e->chunk_frames) {
        wisp_sleep_ms(2);
        return;
    }
    bool eof = false;
    size_t got = wisp_decoder_read(e->cur.dec, e->chunk, e->chunk_frames, &eof);
    if (got > 0) {
        wisp_ring_write(e->ring, e->chunk, got);
        e->produced += got;
    }
    if (eof)
        on_cur_eof(e);
}

static void feed_crossfade(wisp_engine *e) {
    size_t writable = wisp_ring_writable(e->ring);
    if (writable < e->chunk_frames) {
        wisp_sleep_ms(2);
        return;
    }
    uint32_t ch = e->fmt.channels;
    bool eof_in = false, eof_out = false;
    size_t got_in = wisp_decoder_read(e->cur.dec, e->chunk, e->chunk_frames, &eof_in);
    size_t got_out = wisp_decoder_read(e->fade_out.dec, e->mixbuf, e->chunk_frames, &eof_out);
    size_t n = got_in > got_out ? got_in : got_out;
    for (size_t i = 0; i < n; i++) {
        double p = (double)(e->xf_pos + i) / (double)e->xf_frames;
        if (p > 1.0)
            p = 1.0;
        float gin = (float)sin(p * WISP_HALF_PI) * e->cur.gain;
        float gout = (float)cos(p * WISP_HALF_PI) * e->fade_out.gain;
        for (uint32_t c = 0; c < ch; c++) {
            float a = i < got_in ? e->chunk[i * ch + c] * gin : 0.0f;
            float b = i < got_out ? e->mixbuf[i * ch + c] * gout : 0.0f;
            e->chunk[i * ch + c] = a + b;
        }
    }
    if (n > 0) {
        wisp_ring_write(e->ring, e->chunk, n);
        e->produced += n;
        e->xf_pos += n;
    }
    if (e->xf_pos >= e->xf_frames || eof_out || eof_in)
        end_crossfade(e);
    if (eof_in)
        on_cur_eof(e);
}

static void feed(wisp_engine *e) {
    if (!e->xf_active && e->xf_len > 0 && e->playing && e->cur.dec && !e->cur.eof &&
        pl_ready_xfade(e)) {
        uint64_t len = wisp_decoder_length(e->cur.dec);
        if (len > 0) {
            uint64_t curs = wisp_decoder_cursor(e->cur.dec);
            uint64_t rem = len > curs ? len - curs : 0;
            uint64_t xff = (uint64_t)(e->xf_len * e->fmt.sample_rate);
            if (rem > 0 && rem <= xff)
                begin_crossfade(e);
        }
    }
    if (e->xf_active)
        feed_crossfade(e);
    else
        feed_plain(e);

    if (e->want_start && (e->finished || wisp_ring_readable(e->ring) >= e->prime_frames)) {
        wisp_output_start(e->out);
        e->device_running = true;
        e->want_start = false;
    }
}

static void hard_open(wisp_engine *e, wisp_source *src, uint64_t token) {
    end_crossfade(e);
    chain_close(&e->cur);
    pl_cancel(e);
    e->pend_n = 0;
    e->cur.token = token;
    e->cur.eof = false;
    e->cur.gain = 1.0f;
    e->finished = false;
    e->asked_token = 0;
    atomic_store(&e->drained, false);
    wisp_output_set_draining(e->out, false);
    e->cur.dec = decoder_open_heap(e, src);
    if (!e->cur.dec) {
        wisp_log("engine hard_open token=%llu decoder open FAILED", (unsigned long long)token);
        emit(e, WISP_ENGINE_ERROR, token);
        return;
    }
    wisp_log("engine hard_open token=%llu ok len=%llu frames", (unsigned long long)token,
             (unsigned long long)wisp_decoder_length(e->cur.dec));
    e->anchor_base = flush_and_anchor(e);
    e->produced = 0;
    set_marker(e, e->anchor_base, 0, token);
    ask_next(e, token);
}

static void handle(wisp_engine *e, engine_cmd *c) {
    switch (c->type) {
    case CMD_OPEN:
        hard_open(e, c->src, c->token);
        break;
    case CMD_SET_NEXT:
        pl_set(e, c->src, c->token, c->flag);
        break;
    case CMD_CANCEL_NEXT:
        pl_cancel(e);
        break;
    case CMD_CROSSFADE:
        e->xf_len = c->seconds < 0 ? 0 : c->seconds;
        break;
    case CMD_PLAY:
        e->playing = true;
        atomic_store(&e->playing_pub, true);
        e->want_start = true;
        break;
    case CMD_PAUSE:
        e->playing = false;
        atomic_store(&e->playing_pub, false);
        e->want_start = false;
        if (e->device_running) {
            wisp_output_stop(e->out);
            e->device_running = false;
        }
        break;
    case CMD_STOP:
        e->playing = false;
        atomic_store(&e->playing_pub, false);
        e->want_start = false;
        if (e->device_running) {
            wisp_output_stop(e->out);
            e->device_running = false;
        }
        wisp_output_flush(e->out);
        wisp_output_set_draining(e->out, false);
        end_crossfade(e);
        chain_close(&e->cur);
        pl_cancel(e);
        e->pend_n = 0;
        e->finished = false;
        e->asked_token = 0;
        break;
    case CMD_SEEK:
        if (e->cur.dec) {
            collapse_pending(e);
            end_crossfade(e);
            uint64_t frame = (uint64_t)(c->seconds * e->fmt.sample_rate);
            wisp_decoder_seek(e->cur.dec, frame);
            e->cur.eof = false;
            e->finished = false;
            atomic_store(&e->drained, false);
            wisp_output_set_draining(e->out, false);
            e->anchor_base = flush_and_anchor(e);
            e->produced = 0;
            set_marker(e, e->anchor_base, frame, e->cur.token);
        }
        break;
    }
}

static void engine_run(void *p) {
    wisp_engine *e = p;
    while (atomic_load(&e->running)) {
        engine_cmd c;
        while (wisp_chan_try_recv(e->cmds, &c))
            handle(e, &c);
        if (!atomic_load(&e->running))
            break;
        fire_boundaries(e);
        if (e->playing && e->cur.dec && !e->cur.eof) {
            feed(e);
        } else if (e->finished) {
            if (wisp_ring_readable(e->ring) == 0)
                atomic_store(&e->drained, true);
            wisp_sleep_ms(5);
        } else {
            wisp_sleep_ms(5);
        }
    }
}

wisp_engine *wisp_engine_new(wisp_output *out) {
    wisp_engine *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->out = out;
    e->ring = wisp_output_ring(out);
    e->fmt = wisp_output_format(out);
    e->chunk_frames = 2048;
    e->prime_frames = e->fmt.sample_rate / 10;
    e->chunk = malloc(e->chunk_frames * e->fmt.channels * sizeof(float));
    e->mixbuf = malloc(e->chunk_frames * e->fmt.channels * sizeof(float));
    e->cmds = wisp_chan_new(sizeof(engine_cmd), 64);
    e->events = wisp_chan_new(sizeof(wisp_engine_event), 64);
    e->mtx = wisp_mutex_new();
    e->pl_mtx = wisp_mutex_new();
    e->pl_cond = wisp_cond_new();
    if (!e->chunk || !e->mixbuf || !e->cmds || !e->events || !e->mtx || !e->pl_mtx || !e->pl_cond) {
        wisp_engine_free(e);
        return NULL;
    }
    atomic_init(&e->running, true);
    atomic_init(&e->playing_pub, false);
    atomic_init(&e->drained, false);
    e->pl_thread = wisp_thread_start(pl_run, e);
    e->thread = wisp_thread_start(engine_run, e);
    if (!e->pl_thread || !e->thread) {
        atomic_store(&e->running, false);
        wisp_engine_free(e);
        return NULL;
    }
    return e;
}

void wisp_engine_free(wisp_engine *e) {
    if (!e)
        return;
    atomic_store(&e->running, false);
    if (e->pl_cond) {
        wisp_mutex_lock(e->pl_mtx);
        wisp_cond_broadcast(e->pl_cond);
        wisp_mutex_unlock(e->pl_mtx);
    }
    if (e->thread)
        wisp_thread_join(e->thread);
    if (e->pl_thread)
        wisp_thread_join(e->pl_thread);
    end_crossfade(e);
    chain_close(&e->cur);
    if (e->pl_st == PL_READY)
        decoder_free(e->pl_dec);
    else if (e->pl_src)
        e->pl_src->close(e->pl_src);
    if (e->cmds)
        wisp_chan_free(e->cmds);
    if (e->events)
        wisp_chan_free(e->events);
    if (e->mtx)
        wisp_mutex_free(e->mtx);
    if (e->pl_mtx)
        wisp_mutex_free(e->pl_mtx);
    if (e->pl_cond)
        wisp_cond_free(e->pl_cond);
    free(e->chunk);
    free(e->mixbuf);
    free(e);
}

static void send(wisp_engine *e, engine_cmd c) {
    wisp_chan_send(e->cmds, &c);
}

void wisp_engine_open(wisp_engine *e, wisp_source *src, uint64_t token) {
    send(e, (engine_cmd){.type = CMD_OPEN, .src = src, .token = token});
}
void wisp_engine_set_next(wisp_engine *e, wisp_source *src, uint64_t token, bool splice_only) {
    send(e, (engine_cmd){.type = CMD_SET_NEXT, .src = src, .token = token, .flag = splice_only});
}
void wisp_engine_cancel_next(wisp_engine *e) {
    send(e, (engine_cmd){.type = CMD_CANCEL_NEXT});
}
void wisp_engine_set_crossfade(wisp_engine *e, double seconds) {
    send(e, (engine_cmd){.type = CMD_CROSSFADE, .seconds = seconds});
}
void wisp_engine_play(wisp_engine *e) {
    send(e, (engine_cmd){.type = CMD_PLAY});
}
void wisp_engine_pause(wisp_engine *e) {
    send(e, (engine_cmd){.type = CMD_PAUSE});
}
void wisp_engine_stop(wisp_engine *e) {
    send(e, (engine_cmd){.type = CMD_STOP});
}
void wisp_engine_seek(wisp_engine *e, double seconds) {
    send(e, (engine_cmd){.type = CMD_SEEK, .seconds = seconds});
}

bool wisp_engine_poll(wisp_engine *e, wisp_engine_event *out) {
    return wisp_chan_try_recv(e->events, out);
}

wisp_engine_pos wisp_engine_position(wisp_engine *e) {
    uint64_t fp = wisp_output_frames_played(e->out);
    wisp_mutex_lock(e->mtx);
    uint64_t base = e->m_base_abs, off = e->m_offset, tok = e->m_token;
    wisp_mutex_unlock(e->mtx);
    uint64_t frame = fp <= base ? off : off + (fp - base);
    wisp_engine_pos p = {.token = tok,
                         .seconds = (double)frame / (double)e->fmt.sample_rate,
                         .playing = atomic_load(&e->playing_pub)};
    return p;
}

bool wisp_engine_drained(wisp_engine *e) {
    return atomic_load(&e->drained);
}
