#include "output.h"

#include "common.h"

#include "miniaudio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define WISP_PCM_N 2048
#define WISP_SPECTRUM_WIN 1024

struct wisp_output {
    ma_context ctx;
    ma_device device;
    bool ctx_ok;
    bool device_ok;
    wisp_ring *ring;
    wisp_audio_format fmt;

    _Atomic float volume;
    atomic_uint_least64_t flush_epoch;
    atomic_uint_least64_t flush_ack;
    atomic_uint_least64_t frames_played;
    atomic_uint_least32_t underruns;
    atomic_bool draining;

    _Atomic float level;
    float viz_buf[WISP_VIZ_BINS];
    atomic_uint_least32_t viz_head;
    float pcm[WISP_PCM_N];
    atomic_uint_least32_t pcm_head;

    uint64_t cb_epoch;
    float cb_volume;
    float vol_step;
};

static void viz_push(wisp_output *o, const float *frames, size_t got, uint32_t ch) {
    if (got == 0)
        return;
    float peak = 0.0f;
    for (size_t i = 0; i < got; i++) {
        float s = 0.0f;
        for (uint32_t c = 0; c < ch; c++) {
            float v = frames[i * ch + c];
            s += v < 0 ? -v : v;
        }
        s /= (float)ch;
        if (s > peak)
            peak = s;
    }
    uint32_t head = atomic_load_explicit(&o->viz_head, memory_order_relaxed);
    o->viz_buf[head % WISP_VIZ_BINS] = peak;
    atomic_store_explicit(&o->viz_head, head + 1, memory_order_release);

    float prev = atomic_load_explicit(&o->level, memory_order_relaxed);
    float lvl = peak > prev ? peak : prev * 0.85f + peak * 0.15f;
    atomic_store_explicit(&o->level, lvl, memory_order_relaxed);

    uint32_t ph = atomic_load_explicit(&o->pcm_head, memory_order_relaxed);
    for (size_t i = 0; i < got; i++) {
        float s = 0.0f;
        for (uint32_t c = 0; c < ch; c++)
            s += frames[i * ch + c];
        o->pcm[(ph + (uint32_t)i) % WISP_PCM_N] = s / (float)ch;
    }
    atomic_store_explicit(&o->pcm_head, ph + (uint32_t)got, memory_order_release);
}

static void data_cb(ma_device *dev, void *out_v, const void *in, ma_uint32 frames) {
    (void)in;
    wisp_output *o = dev->pUserData;
    float *out = out_v;
    uint32_t ch = o->fmt.channels;

    uint64_t ep = atomic_load_explicit(&o->flush_epoch, memory_order_acquire);
    if (ep != o->cb_epoch) {
        wisp_ring_clear(o->ring);
        o->cb_epoch = ep;
        atomic_store_explicit(&o->flush_ack, ep, memory_order_release);
    }

    size_t got = wisp_ring_read(o->ring, out, frames);

    float target = atomic_load_explicit(&o->volume, memory_order_relaxed);
    float cur = o->cb_volume;
    float step = o->vol_step;
    for (size_t i = 0; i < got; i++) {
        if (cur < target) {
            cur += step;
            if (cur > target)
                cur = target;
        } else if (cur > target) {
            cur -= step;
            if (cur < target)
                cur = target;
        }
        for (uint32_t c = 0; c < ch; c++)
            out[i * ch + c] *= cur;
    }
    o->cb_volume = cur;

    viz_push(o, out, got, ch);

    if (got < frames) {
        memset(out + got * ch, 0, ((size_t)frames - got) * ch * sizeof(float));
        if (!atomic_load_explicit(&o->draining, memory_order_relaxed))
            atomic_fetch_add_explicit(&o->underruns, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&o->frames_played, got, memory_order_relaxed);
}

wisp_output *wisp_output_new(void) {
    wisp_output *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    if (ma_context_init(NULL, 0, NULL, &o->ctx) != MA_SUCCESS) {
        free(o);
        return NULL;
    }
    o->ctx_ok = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 0;
    cfg.dataCallback = data_cb;
    cfg.pUserData = o;

    if (ma_device_init(&o->ctx, &cfg, &o->device) != MA_SUCCESS) {
        ma_context_uninit(&o->ctx);
        free(o);
        return NULL;
    }
    o->device_ok = true;

    o->fmt.sample_rate = o->device.sampleRate;
    o->fmt.channels = o->device.playback.channels;
    o->ring = wisp_ring_new(1u << 14, o->fmt.channels);
    if (!o->ring) {
        wisp_output_free(o);
        return NULL;
    }

    atomic_init(&o->volume, 1.0f);
    atomic_init(&o->flush_epoch, 0);
    atomic_init(&o->flush_ack, 0);
    atomic_init(&o->frames_played, 0);
    atomic_init(&o->underruns, 0);
    atomic_init(&o->draining, false);
    atomic_init(&o->level, 0.0f);
    atomic_init(&o->viz_head, 0);
    atomic_init(&o->pcm_head, 0);
    o->cb_epoch = 0;
    o->cb_volume = 1.0f;
    o->vol_step = 1.0f / (0.010f * (float)o->fmt.sample_rate);
    return o;
}

void wisp_output_free(wisp_output *o) {
    if (!o)
        return;
    if (o->device_ok)
        ma_device_uninit(&o->device);
    if (o->ctx_ok)
        ma_context_uninit(&o->ctx);
    wisp_ring_free(o->ring);
    free(o);
}

bool wisp_output_start(wisp_output *o) { return ma_device_start(&o->device) == MA_SUCCESS; }
void wisp_output_stop(wisp_output *o) { ma_device_stop(&o->device); }

wisp_audio_format wisp_output_format(wisp_output *o) { return o->fmt; }
wisp_ring *wisp_output_ring(wisp_output *o) { return o->ring; }

void wisp_output_set_volume(wisp_output *o, float v) {
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    atomic_store_explicit(&o->volume, v, memory_order_relaxed);
}

float wisp_output_volume(wisp_output *o) {
    return atomic_load_explicit(&o->volume, memory_order_relaxed);
}

uint64_t wisp_output_flush(wisp_output *o) {
    return atomic_fetch_add_explicit(&o->flush_epoch, 1, memory_order_release) + 1;
}

uint64_t wisp_output_flush_ack(wisp_output *o) {
    return atomic_load_explicit(&o->flush_ack, memory_order_acquire);
}

uint64_t wisp_output_frames_played(wisp_output *o) {
    return atomic_load_explicit(&o->frames_played, memory_order_relaxed);
}

uint32_t wisp_output_underruns(wisp_output *o) {
    return atomic_load_explicit(&o->underruns, memory_order_relaxed);
}

void wisp_output_set_draining(wisp_output *o, bool draining) {
    atomic_store_explicit(&o->draining, draining, memory_order_relaxed);
}

float wisp_output_level(wisp_output *o) {
    return atomic_load_explicit(&o->level, memory_order_relaxed);
}

int wisp_output_viz(wisp_output *o, float *out, int max) {
    int n = max < WISP_VIZ_BINS ? max : WISP_VIZ_BINS;
    uint32_t head = atomic_load_explicit(&o->viz_head, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        uint32_t idx = head - (uint32_t)n + (uint32_t)i;
        out[i] = o->viz_buf[idx % WISP_VIZ_BINS];
    }
    return n;
}

int wisp_output_pcm(wisp_output *o, float *out, int n) {
    if (n < 1)
        return 0;
    if (n > WISP_PCM_N)
        n = WISP_PCM_N;
    uint32_t head = atomic_load_explicit(&o->pcm_head, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        uint32_t idx = head - (uint32_t)n + (uint32_t)i;
        out[i] = o->pcm[idx % WISP_PCM_N];
    }
    return n;
}

int wisp_output_spectrum(wisp_output *o, float *out, int n) {
    if (n < 1)
        return 0;
    if (n > 256)
        n = 256;
    static const int M = WISP_SPECTRUM_WIN;
    float win[WISP_SPECTRUM_WIN];
    uint32_t head = atomic_load_explicit(&o->pcm_head, memory_order_acquire);
    for (int i = 0; i < M; i++) {
        uint32_t idx = head - (uint32_t)M + (uint32_t)i;
        float w = 0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)(M - 1));
        win[i] = o->pcm[idx % WISP_PCM_N] * w;
    }
    float sr = (float)o->fmt.sample_rate;
    float fmin = 55.0f, fmax = sr * 0.45f;
    if (fmax > 16000.0f)
        fmax = 16000.0f;
    for (int b = 0; b < n; b++) {
        float frac = n > 1 ? (float)b / (float)(n - 1) : 0.0f;
        float f = fmin * powf(fmax / fmin, frac);
        float coeff = 2.0f * cosf(6.2831853f * f / sr);
        float s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < M; i++) {
            float s0 = win[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        float mag = sqrtf(power > 0 ? power : 0) / ((float)M * 0.5f);
        float v = sqrtf(mag) * 3.2f * (0.7f + 0.9f * frac);
        out[b] = v > 1.0f ? 1.0f : v;
    }
    return n;
}

int wisp_output_list_devices(wisp_output *o, wisp_device_info **out) {
    ma_device_info *infos;
    ma_uint32 count;
    if (ma_context_get_devices(&o->ctx, &infos, &count, NULL, NULL) != MA_SUCCESS) {
        *out = NULL;
        return 0;
    }
    wisp_device_info *list = calloc(count ? count : 1, sizeof *list);
    if (!list) {
        *out = NULL;
        return 0;
    }
    for (ma_uint32 i = 0; i < count; i++) {
        list[i].name = wisp_strdup(infos[i].name);
        list[i].is_default = infos[i].isDefault;
        ma_device_id *id = malloc(sizeof *id);
        if (id)
            memcpy(id, &infos[i].id, sizeof *id);
        list[i].id = id;
    }
    *out = list;
    return (int)count;
}

void wisp_output_free_devices(wisp_device_info *list, int count) {
    for (int i = 0; i < count; i++) {
        free(list[i].name);
        free(list[i].id);
    }
    free(list);
}
