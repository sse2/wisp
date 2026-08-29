#include "app.h"

#include "audio/source.h"
#include "cache/cache.h"
#include "common.h"
#include "core/config.h"
#include "core/core.h"
#include "model/model.h"
#include "plat/plat.h"
#include "screen.h"
#include "subsonic/subsonic.h"
#include "subsonic/sync.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIZ_MAX 256

typedef struct {
    uint32_t bg, panel, header, fg, dim, accent, accent2, selbg, selfg, green, red;
} theme;

static theme theme_default(void) {
    return (theme){0x1e1e2e, 0x181825, 0x11111b, 0xcdd6f4, 0x6c7086, 0x89b4fa,
                   0xf5c2e7, 0x45475a, 0xffffff, 0xa6e3a1, 0xf38ba8};
}

static const char *VIZ_NAMES[] = {"bars", "mirror", "scope", "peaks"};
enum { VIZ_BARS, VIZ_MIRROR, VIZ_SCOPE, VIZ_PEAKS, VIZ_COUNT };

enum { V_CONNECT, V_HOME, V_NOWPLAYING, V_LIBRARY, V_SEARCH, V_PLAYLISTS, V_DETAIL };
static const char *TAB_NAMES[] = {"", "Home", "Now Playing", "Library", "Search", "Playlists"};
enum { POP_NONE, POP_HELP, POP_SETTINGS, POP_ADDPL, POP_NEWPL, POP_CONFIRM };

typedef enum {
    ACT_NP,
    ACT_SUBMIT,
    ACT_STAR,
    ACT_UNSTAR,
    ACT_RATE,
    ACT_LYRICS,
    ACT_PL_ADD,
    ACT_PL_REMOVE,
    ACT_PL_REORDER,
    ACT_PL_CREATE,
    ACT_PL_DELETE,
    ACT_PL_RENAME,
    ACT_QUIT
} act_kind;

typedef struct {
    act_kind kind;
    char id[256];
    char arg[256];
    int rating;
} action;

typedef struct {
    wisp_screen *screen;
    int w, h;
    wisp_config config;
    wisp_subsonic sub;
    bool connected;
    theme th;
    struct {
        char name[32];
        theme th;
    } themes[32];
    int theme_count;

    wisp_model *model;
    wisp_mutex *model_mtx;
    wisp_cache *cache;
    wisp_core *core;

    int view;
    int popup;
    bool np_lyrics;
    int np_qsel;
    int prev_view;

    char cur_play_id[256];
    int play_src_kind;
    char play_src_ext[256];

    int detail_kind;
    uint32_t detail_id;
    int detail_sel, detail_scroll;

    int pane;
    int sel[3];
    int scroll[3];
    bool find_active;
    char find[128];
    int find_len;

    int pl_pane;
    int pl_sel[2];
    int pl_scroll[2];

    int home_sec;
    int home_sel[4];
    int home_scroll[4];
    uint64_t home_seed;
    uint32_t home_pick[2][40];
    int home_pick_n[2];

    char field_url[256], field_user[128], field_pass[128];
    int field;
    char connect_msg[128];

    char search[128];
    int search_len;
    uint32_t search_ids[512];
    size_t search_count;
    int search_sel, search_scroll;

    char **queue_ids;
    size_t queue_count;

    char np_id[256];
    char toast[128];
    uint64_t toast_until;
    uint64_t last_skip_ms;

    wisp_chan *actions;
    wisp_thread *act_thread;
    atomic_bool act_running;

    wisp_mutex *lyr_mtx;
    wisp_lyrics lyrics;
    char lyrics_id[256];
    bool lyrics_ready;
    char lyrics_want[256];

    int settings_sel;
    char st_name[128], st_url[256], st_user[128], st_pass[128];

    int addpl_sel, addpl_scroll;
    char addpl_song[256];
    char newpl[128];
    int newpl_len;
    char newpl_song[256];
    int newpl_mode;
    char newpl_target[256];
    uint32_t newpl_target_id;

    char confirm_msg[128];
    int confirm_kind;
    char confirm_ext[256];
    uint32_t confirm_id;
    int confirm_index;

    float peaks[VIZ_MAX];

    wisp_thread *sync_thread;
    wisp_model *pending_model;
    atomic_bool sync_done;
    bool syncing;

    bool quit;
    uint64_t frame;
} app;

static const char *SPINNER = "|/-\\";
static const char *BRAILLE = "\xe2\xa0\x8b\xe2\xa0\x99\xe2\xa0\xb9\xe2\xa0\xb8\xe2\xa0\xbc"
                             "\xe2\xa0\xb4\xe2\xa0\xa6\xe2\xa0\xa7\xe2\xa0\x87\xe2\xa0\x8f";

static uint64_t anim(app *a) {
    return a->config.reduced_motion ? 0 : a->frame;
}
static bool viz_on(app *a) {
    return a->config.visualizer && !a->config.reduced_motion;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) {
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = (int)(ar + (br - ar) * t), g = (int)(ag + (bg - ag) * t),
        bl = (int)(ab + (bb - ab) * t);
    return (uint32_t)((r << 16) | (g << 8) | bl);
}

static bool motion(app *a) {
    return !a->config.reduced_motion && wisp_screen_truecolor(a->screen);
}

static uint32_t shimmer(app *a, uint32_t base, uint32_t hi, int i, int span) {
    if (!motion(a))
        return base;
    double ph = (double)i / (double)(span > 0 ? span : 1) * 6.2831853 -
                (double)(a->frame % 48) / 48.0 * 6.2831853;
    float t = (float)(0.5 + 0.5 * sin(ph));
    return lerp_rgb(base, hi, t * 0.65f);
}

static uint32_t pulse(app *a, uint32_t c1, uint32_t c2) {
    if (!motion(a))
        return c1;
    float t = (float)(0.5 + 0.5 * sin((double)(a->frame % 44) / 44.0 * 6.2831853));
    return lerp_rgb(c1, c2, t);
}

static bool is_playing_track(app *a, const char *ext) {
    return a->cur_play_id[0] && ext && !strcmp(ext, a->cur_play_id);
}
static bool is_playing_src(app *a, int kind, const char *ext) {
    return a->play_src_kind == kind && ext && !strcmp(a->play_src_ext, ext);
}

static uint32_t rnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x ? x : 0x9E3779B97F4A7C15ull;
    return (uint32_t)(*s >> 11);
}

static void regen_home(app *a) {
    size_t counts[2] = {wisp_model_album_count(a->model), wisp_model_track_count(a->model)};
    for (int s = 0; s < 2; s++) {
        size_t total = counts[s];
        int want = total < 40 ? (int)total : 40;
        a->home_pick_n[s] = want;
        if (total == 0)
            continue;
        uint32_t *tmp = malloc(total * sizeof(uint32_t));
        for (size_t i = 0; i < total; i++)
            tmp[i] = (uint32_t)i;
        for (int i = 0; i < want; i++) {
            size_t j = (size_t)i + (size_t)(rnd(&a->home_seed) % (total - (size_t)i));
            uint32_t t = tmp[i];
            tmp[i] = tmp[j];
            tmp[j] = t;
            a->home_pick[s][i] = tmp[i];
        }
        free(tmp);
    }
}

static uint32_t np_bg(app *a, int x, int y) {
    if (!motion(a))
        return a->th.bg;
    double t = (double)a->frame * 0.05;
    double w1 = sin(x * 0.13 + y * 0.27 + t) * 0.5 + 0.5;
    double w2 = sin(x * 0.08 - y * 0.11 - t * 0.6) * 0.5 + 0.5;
    uint32_t tint = lerp_rgb(a->th.accent, a->th.accent2, (float)w2);
    return lerp_rgb(a->th.bg, tint, (float)((w1 * 0.6 + w2 * 0.4) * 0.10));
}

static uint32_t decode_utf8(const unsigned char *p, int *len) {
    if (*p < 0x80) {
        *len = 1;
        return *p;
    }
    if ((*p >> 5) == 6 && p[1]) {
        *len = 2;
        return ((uint32_t)(*p & 0x1f) << 6) | (p[1] & 0x3f);
    }
    if ((*p >> 4) == 14 && p[1] && p[2]) {
        *len = 3;
        return ((uint32_t)(*p & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    }
    *len = 1;
    return *p;
}

static void np_wash(app *a, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            wisp_screen_cell(a->screen, x + i, y + j, ' ', a->th.dim, np_bg(a, x + i, y + j), 0);
}

static void np_text(app *a, int x, int y, int maxw, const char *utf8, uint32_t fg, uint8_t attr) {
    int col = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p && col < maxw) {
        int len;
        uint32_t cp = decode_utf8(p, &len);
        wisp_screen_cell(a->screen, x + col, y, cp, fg, np_bg(a, x + col, y), attr);
        p += len;
        col++;
    }
}

static void set_str(char **slot, const char *v) {
    free(*slot);
    *slot = wisp_strdup(v ? v : "");
}

static bool ci_prefix(const char *hay, const char *needle) {
    for (; *needle; hay++, needle++) {
        int h = *hay >= 'A' && *hay <= 'Z' ? *hay + 32 : (unsigned char)*hay;
        int n = *needle >= 'A' && *needle <= 'Z' ? *needle + 32 : (unsigned char)*needle;
        if (!*hay || h != n)
            return false;
    }
    return true;
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle)
        return true;
    for (const char *p = hay; *p; p++)
        if (ci_prefix(p, needle))
            return true;
    return false;
}

static bool decodable(const char *suffix) {
    static const char *ok[] = {"mp3", "flac", "ogg", "oga", "opus", "wav", NULL};
    for (int i = 0; suffix && ok[i]; i++)
        if (!strcmp(suffix, ok[i]))
            return true;
    return false;
}

static const char *transcode_fmt(app *a) {
    const char *f = a->config.transcode_format;
    return f && *f ? f : "mp3";
}

static char *stream_url_for(app *a, const wisp_track *t, bool *raw_out) {
    bool raw = decodable(t->suffix);
    if (raw_out)
        *raw_out = raw;
    return wisp_subsonic_stream_url(&a->sub, t->ext_id, raw ? "raw" : transcode_fmt(a),
                                    raw ? 0 : 320, 0);
}

static wisp_source *ui_provider(void *ctx, const char *item) {
    app *a = ctx;
    wisp_mutex_lock(a->model_mtx);
    wisp_source *s = NULL;
    uint32_t id;
    if (wisp_model_find_track(a->model, item, &id)) {
        const wisp_track *t = wisp_model_track(a->model, id);
        bool raw = false;
        char *url = stream_url_for(a, t, &raw);
        s = wisp_cache_open(a->cache, item, url, raw ? t->size : 0);
        free(url);
        wisp_log("provider item=%s title=\"%s\" suffix=%s raw=%d expected=%lld -> %s", item,
                 t->title ? t->title : "", t->suffix ? t->suffix : "?", raw,
                 (long long)(raw ? t->size : 0), s ? "source" : "NULL");
    } else {
        wisp_log("provider item=%s NOT FOUND in model", item);
    }
    wisp_mutex_unlock(a->model_mtx);
    return s;
}

static uint64_t ui_group(void *ctx, const char *item) {
    app *a = ctx;
    wisp_mutex_lock(a->model_mtx);
    uint64_t g = 0;
    uint32_t id;
    if (wisp_model_find_track(a->model, item, &id))
        g = (uint64_t)wisp_model_track(a->model, id)->album_id + 1;
    wisp_mutex_unlock(a->model_mtx);
    return g;
}

static void fmt_time(int sec, char *buf, size_t cap) {
    if (sec < 0)
        sec = 0;
    snprintf(buf, cap, "%d:%02d", sec / 60, sec % 60);
}

static void toast(app *a, const char *msg) {
    snprintf(a->toast, sizeof a->toast, "%s", msg);
    a->toast_until = wisp_now_ms() + 2500;
}

static void post(app *a, act_kind kind, const char *id, const char *arg, int rating) {
    if (!a->actions)
        return;
    action ac = {.kind = kind, .rating = rating};
    if (id)
        snprintf(ac.id, sizeof ac.id, "%s", id);
    if (arg)
        snprintf(ac.arg, sizeof ac.arg, "%s", arg);
    wisp_chan_try_send(a->actions, &ac);
}

static bool playing_id(app *a, char *out, size_t cap) {
    if (!a->core)
        return false;
    wisp_status s = wisp_core_status(a->core);
    bool ok = s.queue_pos < a->queue_count && s.state != WISP_STATE_STOPPED;
    if (ok)
        snprintf(out, cap, "%s", a->queue_ids[s.queue_pos]);
    wisp_status_free(&s);
    return ok;
}

static uint32_t cur_artist(app *a) {
    return a->sel[0] < (int)wisp_model_artist_count(a->model) ? (uint32_t)a->sel[0] : 0;
}
static uint32_t cur_album(app *a) {
    const wisp_artist *ar = wisp_model_artist(a->model, cur_artist(a));
    if (ar && a->sel[1] < (int)ar->album_count)
        return ar->album_ids[a->sel[1]];
    return 0;
}
static const wisp_track *sel_track(app *a) {
    const wisp_album *al = wisp_model_album(a->model, cur_album(a));
    if (al && a->sel[2] < (int)al->track_count)
        return wisp_model_track(a->model, al->track_ids[a->sel[2]]);
    return NULL;
}

static const uint32_t *detail_tracks(app *a, size_t *count, const char **name, const char **sub) {
    *count = 0;
    *name = "?";
    *sub = "";
    if (a->detail_kind == 1) {
        const wisp_album *al = wisp_model_album(a->model, a->detail_id);
        if (al) {
            *count = al->track_count;
            *name = al->name ? al->name : "?";
            *sub = al->artist_name ? al->artist_name : "";
            return al->track_ids;
        }
    } else {
        const wisp_playlist *p = wisp_model_playlist(a->model, a->detail_id);
        if (p) {
            *count = p->track_count;
            *name = p->name ? p->name : "?";
            return p->track_ids;
        }
    }
    return NULL;
}

static int pane_count(app *a, int pane) {
    if (pane == 0)
        return (int)wisp_model_artist_count(a->model);
    if (pane == 1) {
        const wisp_artist *ar = wisp_model_artist(a->model, cur_artist(a));
        return ar ? (int)ar->album_count : 0;
    }
    const wisp_album *al = wisp_model_album(a->model, cur_album(a));
    return al ? (int)al->track_count : 0;
}

static void pane_label(app *a, int pane, int i, char *buf, size_t cap) {
    if (pane == 0) {
        const wisp_artist *ar = wisp_model_artist(a->model, (uint32_t)i);
        snprintf(buf, cap, "%s", ar && ar->name ? ar->name : "?");
    } else if (pane == 1) {
        const wisp_artist *ar = wisp_model_artist(a->model, cur_artist(a));
        const wisp_album *al = ar ? wisp_model_album(a->model, ar->album_ids[i]) : NULL;
        if (al && al->year)
            snprintf(buf, cap, "%s  (%d)", al->name ? al->name : "?", al->year);
        else
            snprintf(buf, cap, "%s", al && al->name ? al->name : "?");
    } else {
        const wisp_album *al = wisp_model_album(a->model, cur_album(a));
        const wisp_track *t = al ? wisp_model_track(a->model, al->track_ids[i]) : NULL;
        char star = t && t->starred ? '*' : ' ';
        snprintf(buf, cap, "%c%2d  %s", star, t ? t->track_no : 0, t && t->title ? t->title : "?");
    }
}

static void ensure_visible(int *sel, int *scroll, int rows, int count) {
    if (rows < 1)
        rows = 1;
    if (*sel < 0)
        *sel = 0;
    if (*sel >= count)
        *sel = count - 1;
    if (*sel < 0)
        *sel = 0;
    if (*sel < *scroll)
        *scroll = *sel;
    if (*sel >= *scroll + rows)
        *scroll = *sel - rows + 1;
    if (*scroll < 0)
        *scroll = 0;
}

static void draw_bar(app *a, int x, int y, int w, double frac, uint32_t on, uint32_t off) {
    if (w <= 0)
        return;
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    int fill = (int)(w * frac);
    for (int i = 0; i < w; i++)
        wisp_screen_cell(a->screen, x + i, y, 0x2501, i < fill ? on : off, a->th.panel, 0);
}

static void draw_track_2line(app *a, int x, int y, int w, const wisp_track *t, bool sel) {
    bool playing = is_playing_track(a, t->ext_id);
    uint32_t bg = sel ? a->th.selbg : a->th.bg;
    uint32_t fg = sel ? a->th.selfg : playing ? a->th.accent : a->th.fg;
    wisp_screen_fill(a->screen, x, y, w, 2, ' ', fg, bg, 0);
    if (playing)
        wisp_screen_text(a->screen, x + 1, y, 2, "\xe2\x96\xb6",
                         pulse(a, a->th.accent, a->th.accent2), bg, 0);
    char l1[256];
    snprintf(l1, sizeof l1, "%s%s", t->starred ? "\xe2\x98\x85 " : "", t->title ? t->title : "?");
    wisp_screen_text(a->screen, x + 3, y, w - 4, l1, fg, bg, sel || playing ? WISP_ATTR_BOLD : 0);
    int ax = x + 5;
    const char *artist = t->artist_name ? t->artist_name : "";
    wisp_screen_text(a->screen, ax, y + 1, w - 6, artist, a->th.dim, bg, 0);
    int alx = ax + (int)strlen(artist) + 2;
    if (t->album_name && *t->album_name && alx < x + w - 2)
        wisp_screen_text(a->screen, alx, y + 1, x + w - 2 - alx, t->album_name, a->th.dim, bg,
                         WISP_ATTR_DIM);
}

/* ---- visualizers ---- */

static void draw_viz_bar(app *a, int x, int y, int w, uint32_t bg) {
    float bins[VIZ_MAX];
    int n = a->core ? wisp_core_viz(a->core, bins, w < VIZ_MAX ? w : VIZ_MAX) : 0;
    for (int i = 0; i < w; i++) {
        float v = i < n ? bins[i] : 0.0f;
        if (v > 1)
            v = 1;
        int idx = (int)(v * 8);
        if (idx > 7)
            idx = 7;
        uint32_t fg = v > 0.66f ? a->th.accent2 : v > 0.33f ? a->th.accent : a->th.dim;
        wisp_screen_cell(a->screen, x + i, y, 0x2581 + (uint32_t)idx, fg, bg, 0);
    }
}

static void column(app *a, int bx, int y, int rows, float v, uint32_t bg) {
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    int cells = (int)(v * rows * 8);
    for (int r = 0; r < rows; r++) {
        int from_bottom = rows - 1 - r;
        int fill = cells - from_bottom * 8;
        if (fill < 0)
            fill = 0;
        if (fill > 8)
            fill = 8;
        if (fill == 0) {
            wisp_screen_cell(a->screen, bx, y + r, ' ', a->th.dim, bg, 0);
        } else {
            double hf = (double)from_bottom / (double)(rows > 1 ? rows - 1 : 1);
            uint32_t col = hf > 0.66 ? a->th.accent2 : hf > 0.33 ? a->th.accent : a->th.green;
            wisp_screen_cell(a->screen, bx, y + r, 0x2580 + (uint32_t)fill, col, bg, 0);
        }
    }
}

static void draw_viz_full(app *a, int x, int y, int w, int rows, uint32_t bg) {
    if (w < 2 || rows < 1)
        return;
    int type = a->config.viz_type;
    if (type == VIZ_SCOPE) {
        float pcm[256];
        int cap = w < 256 ? w : 256;
        int n = a->core ? wisp_core_pcm(a->core, pcm, cap) : 0;
        int mid = rows / 2;
        for (int i = 0; i < w; i++) {
            float v = i < n ? pcm[i] : 0.0f;
            if (v > 1)
                v = 1;
            if (v < -1)
                v = -1;
            int row = mid - (int)(v * mid);
            if (row < 0)
                row = 0;
            if (row >= rows)
                row = rows - 1;
            for (int r = 0; r < rows; r++) {
                bool on = (r >= row && r <= mid) || (r <= row && r >= mid);
                uint32_t col = r == row ? a->th.accent2 : a->th.accent;
                wisp_screen_cell(a->screen, x + i, y + r, on ? (r == row ? 0x2501 : 0x2502) : ' ',
                                 col, bg, 0);
            }
        }
        return;
    }
    int nb = (w + 1) / 2;
    if (nb > VIZ_MAX)
        nb = VIZ_MAX;
    float bands[VIZ_MAX];
    int got = a->core ? wisp_core_spectrum(a->core, bands, nb) : 0;
    if (type == VIZ_MIRROR) {
        for (int b = 0; b < got; b++) {
            int half = nb / 2;
            float v = bands[b < half ? half - 1 - b : b - half];
            int bx = x + b * 2;
            if (bx < x + w)
                column(a, bx, y, rows, v, bg);
        }
        return;
    }
    for (int b = 0; b < got; b++) {
        float v = bands[b];
        if (type == VIZ_PEAKS) {
            float pk = a->peaks[b];
            pk = a->config.reduced_motion ? v : pk * 0.92f;
            if (v > pk)
                pk = v;
            a->peaks[b] = pk;
        }
        int bx = x + b * 2;
        if (bx >= x + w)
            break;
        column(a, bx, y, rows, v, bg);
        if (type == VIZ_PEAKS) {
            int cap_from_bottom = (int)(a->peaks[b] * rows);
            int r = rows - 1 - cap_from_bottom;
            if (r >= y - y && r >= 0 && r < rows)
                wisp_screen_cell(a->screen, bx, y + r, 0x2594, a->th.accent2, bg, 0);
        }
    }
}

/* ---- xtras ---- */

static void draw_tabs(app *a) {
    wisp_screen_fill(a->screen, 0, 0, a->w, 1, ' ', a->th.dim, a->th.header, 0);
    const char *wm = "wisp";
    for (int i = 0; wm[i]; i++)
        wisp_screen_cell(a->screen, 1 + i, 0, (uint32_t)wm[i],
                         shimmer(a, a->th.accent2, a->th.accent, i, 4), a->th.header,
                         WISP_ATTR_BOLD);
    int x = 7;
    for (int v = V_HOME; v <= V_PLAYLISTS; v++) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, " %s ", TAB_NAMES[v]);
        bool on = a->view == v;
        uint32_t fg = on ? a->th.selfg : a->th.dim;
        uint32_t bg = on ? a->th.accent : a->th.header;
        int lw = (int)strlen(lbl);
        wisp_screen_fill(a->screen, x, 0, lw, 1, ' ', fg, bg, on ? WISP_ATTR_BOLD : 0);
        wisp_screen_text(a->screen, x, 0, lw, lbl, fg, bg, on ? WISP_ATTR_BOLD : 0);
        x += lw + 1;
    }
    int right = a->w;
    if (a->syncing) {
        char sp[24];
        snprintf(sp, sizeof sp, "sync %c", SPINNER[(anim(a) / 3) % 4]);
        right = a->w - 8;
        wisp_screen_text(a->screen, right, 0, 7, sp, a->th.green, a->th.header, 0);
    }
    const char *srv = a->config.server_count ? a->config.servers[0].name : "";
    if (srv && *srv) {
        int slen = (int)strlen(srv);
        int sx = right - slen - 2;
        if (sx > x + 1)
            for (int i = 0; i < slen; i++)
                wisp_screen_cell(a->screen, sx + i, 0, (uint32_t)(unsigned char)srv[i],
                                 shimmer(a, a->th.dim, a->th.accent2, i, slen), a->th.header, 0);
    }
}

static const char *state_icon(wisp_state s) {
    switch (s) {
    case WISP_STATE_PLAYING:
        return "\xe2\x96\xb6";
    case WISP_STATE_PAUSED:
        return "\xe2\x8f\xb8";
    case WISP_STATE_BUFFERING:
        return "\xe2\x8b\xaf";
    default:
        return "\xe2\x96\xa0";
    }
}

static void now_playing_info(app *a, wisp_status *s, char *title, size_t tcap, char *artist,
                             size_t acap, int *dur, bool *starred) {
    snprintf(title, tcap, "nothing playing");
    artist[0] = '\0';
    *dur = 0;
    *starred = false;
    if (s->queue_pos < a->queue_count) {
        uint32_t id;
        if (wisp_model_find_track(a->model, a->queue_ids[s->queue_pos], &id)) {
            const wisp_track *t = wisp_model_track(a->model, id);
            snprintf(title, tcap, "%s", t->title ? t->title : "?");
            snprintf(artist, acap, "%s", t->artist_name ? t->artist_name : "");
            *dur = t->duration;
            *starred = t->starred;
        }
    }
}

static void draw_now_bar(app *a) {
    int y0 = a->h - 3, y1 = a->h - 2;
    wisp_screen_fill(a->screen, 0, y0, a->w, 2, ' ', a->th.fg, a->th.panel, 0);

    wisp_state st = WISP_STATE_STOPPED;
    double pos = 0;
    float vol = 1.0f;
    bool muted = false, starred = false, s_shuffle = false;
    wisp_repeat s_repeat = WISP_REPEAT_OFF;
    int dur = 0;
    char title[192], artist[128];
    if (a->core) {
        wisp_status s = wisp_core_status(a->core);
        st = s.state;
        pos = s.position;
        vol = s.volume;
        muted = s.muted;
        s_shuffle = s.shuffle;
        s_repeat = s.repeat;
        now_playing_info(a, &s, title, sizeof title, artist, sizeof artist, &dur, &starred);
        wisp_status_free(&s);
    } else {
        snprintf(title, sizeof title, "nothing playing");
        artist[0] = '\0';
    }

    uint32_t icfg = st == WISP_STATE_PLAYING     ? pulse(a, a->th.green, a->th.accent2)
                    : st == WISP_STATE_BUFFERING ? a->th.accent2
                                                 : a->th.dim;
    if (st == WISP_STATE_BUFFERING) {
        char sp[8];
        snprintf(sp, sizeof sp, "%.3s", BRAILLE + 3 * ((anim(a) / 2) % 10));
        wisp_screen_text(a->screen, 1, y0, 2, sp, icfg, a->th.panel, WISP_ATTR_BOLD);
    } else {
        wisp_screen_text(a->screen, 1, y0, 3, state_icon(st), icfg, a->th.panel, WISP_ATTR_BOLD);
    }
    int tx = 4;
    if (starred) {
        wisp_screen_text(a->screen, tx, y0, 2, "\xe2\x98\x85", a->th.accent2, a->th.panel, 0);
        tx += 2;
    }
    wisp_screen_text(a->screen, tx, y0, a->w / 2, title, a->th.fg, a->th.panel, WISP_ATTR_BOLD);
    wisp_screen_text(a->screen, tx + (int)strlen(title) + 2, y0, a->w / 3, artist, a->th.dim,
                     a->th.panel, 0);
    if (viz_on(a) && st == WISP_STATE_PLAYING && a->w > 60)
        draw_viz_bar(a, a->w - 15, y0, 14, a->th.panel);

    char e[16], d[16];
    fmt_time((int)pos, e, sizeof e);
    fmt_time(dur, d, sizeof d);
    char clock[40];
    snprintf(clock, sizeof clock, "%s / %s", e, d);
    wisp_screen_text(a->screen, 1, y1, 16, clock, a->th.dim, a->th.panel, 0);
    int bar_x = 14, bar_w = a->w - 14 - 22;
    if (bar_w > 4)
        draw_bar(a, bar_x, y1, bar_w, dur > 0 ? pos / dur : 0, a->th.accent, a->th.selbg);
    wisp_screen_cell(a->screen, a->w - 20, y1, 0x21c4, s_shuffle ? a->th.accent : a->th.selbg,
                     a->th.panel, 0);
    wisp_screen_cell(a->screen, a->w - 18, y1, 0x21bb,
                     s_repeat != WISP_REPEAT_OFF ? a->th.accent : a->th.selbg, a->th.panel, 0);
    if (s_repeat == WISP_REPEAT_ONE)
        wisp_screen_cell(a->screen, a->w - 17, y1, '1', a->th.accent, a->th.panel, 0);
    char vbuf[16];
    if (muted)
        snprintf(vbuf, sizeof vbuf, "muted");
    else
        snprintf(vbuf, sizeof vbuf, "vol %d%%", (int)(vol * 100));
    wisp_screen_text(a->screen, a->w - 14, y1, 13, vbuf, a->th.dim, a->th.panel, 0);
}

static void draw_hint(app *a, const char *hint) {
    wisp_screen_fill(a->screen, 0, a->h - 1, a->w, 1, ' ', a->th.dim, a->th.bg, 0);
    if (a->toast[0] && wisp_now_ms() < a->toast_until)
        wisp_screen_text(a->screen, 1, a->h - 1, a->w - 2, a->toast, a->th.accent2, a->th.bg,
                         WISP_ATTR_BOLD);
    else
        wisp_screen_text(a->screen, 1, a->h - 1, a->w - 2, hint, a->th.dim, a->th.bg, 0);
}

/* ---- library ---- */

static void draw_pane(app *a, int x, int w, const char *title, int pane) {
    bool active = a->pane == pane && a->view == V_LIBRARY && !a->find_active;
    int item_rows = a->h - (a->find_active ? 6 : 5);
    int count = pane_count(a, pane);
    ensure_visible(&a->sel[pane], &a->scroll[pane], item_rows, count);
    uint32_t tfg = active ? a->th.accent : a->th.dim;
    wisp_screen_fill(a->screen, x, 1, w, 1, ' ', tfg, a->th.panel, active ? WISP_ATTR_BOLD : 0);
    wisp_screen_text(a->screen, x + 1, 1, w - 2, title, tfg, a->th.panel,
                     active ? WISP_ATTR_BOLD : 0);
    for (int i = 0; i < item_rows; i++) {
        int idx = a->scroll[pane] + i;
        int yy = 2 + i;
        bool selected = idx == a->sel[pane];
        uint32_t fg = a->th.fg, bg = a->th.bg;
        if (selected && a->pane == pane) {
            bg = active ? a->th.selbg : a->th.panel;
            fg = active ? a->th.selfg : a->th.fg;
        }
        bool playing = false;
        if (pane == 2 && idx < count) {
            const wisp_album *al = wisp_model_album(a->model, cur_album(a));
            if (al && idx < (int)al->track_count)
                playing =
                    is_playing_track(a, wisp_model_track(a->model, al->track_ids[idx])->ext_id);
        }
        if (playing && !(selected && a->pane == pane))
            fg = a->th.accent;
        wisp_screen_fill(a->screen, x, yy, w, 1, ' ', fg, bg, 0);
        if (idx < count) {
            char buf[256];
            pane_label(a, pane, idx, buf, sizeof buf);
            if (playing)
                wisp_screen_text(a->screen, x, yy, 2, "\xe2\x96\xb6",
                                 pulse(a, a->th.accent, a->th.accent2), bg, 0);
            wisp_screen_text(a->screen, x + 1, yy, w - 2, buf, fg, bg,
                             (selected && active) || playing ? WISP_ATTR_BOLD : 0);
        }
    }
}

static void draw_find_bar(app *a) {
    int y = a->h - 4;
    wisp_screen_fill(a->screen, 0, y, a->w, 1, ' ', a->th.fg, a->th.header, 0);
    char buf[160];
    snprintf(buf, sizeof buf, "find: %s", a->find);
    wisp_screen_text(a->screen, 1, y, a->w - 2, buf, a->th.accent2, a->th.header, WISP_ATTR_BOLD);
    wisp_screen_cell(a->screen, 1 + 6 + a->find_len, y, '_', a->th.accent2, a->th.header, 0);
}

static void draw_library(app *a) {
    int aw = a->w < 90 ? 18 : 24;
    int tw = a->w < 90 ? 30 : 42;
    int bw = a->w - aw - tw;
    if (bw < 16) {
        aw = a->w / 3;
        bw = a->w / 3;
        tw = a->w - aw - bw;
    }
    draw_pane(a, 0, aw, "Artists", 0);
    draw_pane(a, aw, bw, "Albums", 1);
    draw_pane(a, aw + bw, tw, "Tracks", 2);
    if (a->find_active)
        draw_find_bar(a);
    draw_hint(a,
              "\xe2\x86\x91\xe2\x86\x93 move  \xe2\x86\x90\xe2\x86\x92 pane  Enter play  / find  "
              "a +playlist  s star  P pin  v viz  S settings  ? help");
}

/* ---- home ---- */

static int home_count(app *a, int sec) {
    if (sec == 0)
        return 1;
    if (sec == 1)
        return (int)wisp_model_playlist_count(a->model);
    return a->home_pick_n[sec - 2];
}

static uint32_t home_item(app *a, int sec, int i) {
    if (sec <= 1)
        return (uint32_t)i;
    if (i >= 0 && i < a->home_pick_n[sec - 2])
        return a->home_pick[sec - 2][i];
    return 0;
}

static bool home_playing(app *a, int sec, uint32_t id) {
    if (sec == 1) {
        const wisp_playlist *p = wisp_model_playlist(a->model, id);
        return p && is_playing_src(a, 2, p->ext_id);
    }
    if (sec == 2) {
        const wisp_album *al = wisp_model_album(a->model, id);
        return al && is_playing_src(a, 1, al->ext_id);
    }
    if (sec == 3) {
        const wisp_track *t = wisp_model_track(a->model, id);
        return t && is_playing_track(a, t->ext_id);
    }
    return false;
}

static void home_label(app *a, int sec, int i, char *buf, size_t cap, char *sub, size_t subcap) {
    sub[0] = '\0';
    if (sec == 0) {
        snprintf(buf, cap, "\xe2\x96\xb6 Shuffle library");
        snprintf(sub, subcap, "shuffle everything");
        return;
    }
    uint32_t id = home_item(a, sec, i);
    if (sec == 1) {
        const wisp_playlist *p = wisp_model_playlist(a->model, id);
        snprintf(buf, cap, "%s", p && p->name ? p->name : "?");
        if (p)
            snprintf(sub, subcap, "%zu tracks", p->track_count);
    } else if (sec == 2) {
        const wisp_album *al = wisp_model_album(a->model, id);
        snprintf(buf, cap, "%s", al && al->name ? al->name : "?");
        if (al)
            snprintf(sub, subcap, "%s", al->artist_name ? al->artist_name : "");
    } else {
        const wisp_track *t = wisp_model_track(a->model, id);
        snprintf(buf, cap, "%s", t && t->title ? t->title : "?");
        if (t)
            snprintf(sub, subcap, "%s", t->artist_name ? t->artist_name : "");
    }
}

static void draw_home_section(app *a, int y, const char *title, int sec, int cardw) {
    wisp_screen_text(a->screen, 2, y, a->w - 4, title,
                     a->home_sec == sec ? a->th.accent : a->th.dim, a->th.bg, WISP_ATTR_BOLD);
    int count = home_count(a, sec);
    int per = (a->w - 3) / cardw;
    if (per < 1)
        per = 1;
    ensure_visible(&a->home_sel[sec], &a->home_scroll[sec], per, count);
    for (int i = 0; i < per; i++) {
        int idx = a->home_scroll[sec] + i;
        int cx = 2 + i * cardw;
        if (idx >= count)
            break;
        bool sel = idx == a->home_sel[sec] && a->home_sec == sec;
        bool playing = home_playing(a, sec, home_item(a, sec, idx));
        uint32_t bg = sel ? a->th.selbg : a->th.panel;
        wisp_screen_fill(a->screen, cx, y + 1, cardw - 1, 2, ' ', a->th.fg, bg, 0);
        char name[128], sub[128];
        home_label(a, sec, idx, name, sizeof name, sub, sizeof sub);
        uint32_t nfg = sec == 0  ? a->th.green
                       : sel     ? a->th.selfg
                       : playing ? a->th.accent
                                 : a->th.fg;
        if (playing)
            wisp_screen_text(a->screen, cx, y + 1, 2, "\xe2\x96\xb6",
                             pulse(a, a->th.accent, a->th.accent2), bg, 0);
        wisp_screen_text(a->screen, cx + (playing ? 2 : 1), y + 1, cardw - 2 - (playing ? 1 : 0),
                         name, nfg, bg, sel || playing ? WISP_ATTR_BOLD : 0);
        wisp_screen_text(a->screen, cx + 1, y + 2, cardw - 2, sub, a->th.dim, bg, 0);
    }
}

static void draw_home(app *a) {
    int cardw = a->w < 80 ? 18 : 24;
    const char *titles[] = {"Start", "Your Playlists", "Albums", "Songs"};
    for (int sec = 0; sec < 4; sec++)
        draw_home_section(a, 2 + sec * 4, titles[sec], sec, cardw);
    draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 section  \xe2\x86\x90\xe2\x86\x92 pick  Enter open  z "
                 "shuffle  Tab views  S settings  ? help");
}

/* ---- now playing ---- */

static void draw_lyrics_panel(app *a, int x, int y, int w, int rows, double pos) {
    wisp_mutex_lock(a->lyr_mtx);
    bool ready = a->lyrics_ready;
    bool synced = a->lyrics.synced;
    size_t count = a->lyrics.count;
    int cur_line = -1;
    if (ready && synced) {
        int64_t nowms = (int64_t)(pos * 1000);
        for (size_t i = 0; i < count; i++)
            if (a->lyrics.lines[i].ms >= 0 && a->lyrics.lines[i].ms <= nowms)
                cur_line = (int)i;
    }
    if (!ready) {
        np_text(a, x, y, w, "fetching lyrics\xe2\x80\xa6", a->th.dim, 0);
    } else if (count == 0) {
        np_text(a, x, y + rows / 2, w, "\xe2\x99\xac  no lyrics for this track", a->th.dim, 0);
    } else {
        int start = synced && cur_line >= 0 ? cur_line - rows / 2 : 0;
        if (start > (int)count - rows)
            start = (int)count - rows;
        if (start < 0)
            start = 0;
        for (int i = 0; i < rows && start + i < (int)count; i++) {
            int li = start + i;
            bool cur = li == cur_line;
            uint32_t fg = cur ? a->th.accent2 : synced && li < cur_line ? a->th.dim : a->th.fg;
            const char *txt = a->lyrics.lines[li].text ? a->lyrics.lines[li].text : "";
            np_text(a, x, y + i, w, *txt ? txt : " ", fg, cur ? WISP_ATTR_BOLD : 0);
        }
    }
    wisp_mutex_unlock(a->lyr_mtx);
}

static void draw_np_title(app *a, int x, int y, int maxw, const char *utf8, bool shine) {
    int col = 0, span = (int)strlen(utf8);
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p && col < maxw) {
        int len;
        uint32_t cp = decode_utf8(p, &len);
        uint32_t fg = shine ? shimmer(a, a->th.accent2, a->th.accent, col, span) : a->th.dim;
        wisp_screen_cell(a->screen, x + col, y, cp, fg, np_bg(a, x + col, y), WISP_ATTR_BOLD);
        p += len;
        col++;
    }
}

static void draw_queue_panel(app *a, int x, int y, int w, int rows) {
    size_t win[128];
    int cur = 0;
    int n = a->core ? wisp_core_queue_window(a->core, win, 128, &cur) : 0;
    if (n == 0) {
        np_text(a, x, y + rows / 2, w, "\xe2\x99\xac  queue is empty", a->th.dim, 0);
        return;
    }
    int per = rows / 2;
    int sel = cur + a->np_qsel;
    if (sel < 0)
        sel = 0;
    if (sel >= n)
        sel = n - 1;
    a->np_qsel = sel - cur;
    int maxstart = n - per;
    if (maxstart < 0)
        maxstart = 0;
    int start = sel - per / 2;
    if (start < 0)
        start = 0;
    if (start > maxstart)
        start = maxstart;
    for (int i = 0; i < per; i++) {
        int idx = start + i;
        int yy = y + i * 2;
        if (idx >= n)
            break;
        size_t qidx = win[idx];
        if (qidx >= a->queue_count)
            continue;
        uint32_t tid;
        if (!wisp_model_find_track(a->model, a->queue_ids[qidx], &tid))
            continue;
        const wisp_track *t = wisp_model_track(a->model, tid);
        char l2[192];
        snprintf(l2, sizeof l2, "%s   %s", t->artist_name ? t->artist_name : "",
                 t->album_name ? t->album_name : "");
        bool issel = idx == sel, iscur = idx == cur;
        if (issel) {
            wisp_screen_fill(a->screen, x, yy, w, 2, ' ', a->th.selfg, a->th.selbg, 0);
            const char *mk = iscur ? "\xe2\x96\xb6" : "\xe2\x80\xba";
            wisp_screen_text(a->screen, x + 1, yy, 2, mk,
                             iscur ? pulse(a, a->th.accent, a->th.accent2) : a->th.accent,
                             a->th.selbg, 0);
            wisp_screen_text(a->screen, x + 3, yy, w - 4, t->title ? t->title : "?", a->th.selfg,
                             a->th.selbg, WISP_ATTR_BOLD);
            wisp_screen_text(a->screen, x + 5, yy + 1, w - 6, l2, a->th.dim, a->th.selbg, 0);
        } else if (iscur) {
            wisp_screen_text(a->screen, x + 1, yy, 2, "\xe2\x96\xb6",
                             pulse(a, a->th.accent, a->th.accent2), np_bg(a, x + 1, yy), 0);
            np_text(a, x + 3, yy, w - 4, t->title ? t->title : "?", a->th.accent, WISP_ATTR_BOLD);
            np_text(a, x + 5, yy + 1, w - 6, l2, a->th.dim, 0);
        } else {
            uint32_t fg = idx < cur ? a->th.dim : a->th.fg;
            np_text(a, x + 3, yy, w - 4, t->title ? t->title : "?", fg, 0);
            np_text(a, x + 5, yy + 1, w - 6, l2, a->th.dim, 0);
        }
    }
}

static void draw_nowplaying(app *a) {
    wisp_status s = a->core ? wisp_core_status(a->core) : (wisp_status){0};
    char title[192] = "nothing playing", artist[128] = "";
    int dur = 0;
    bool starred = false;
    if (a->core)
        now_playing_info(a, &s, title, sizeof title, artist, sizeof artist, &dur, &starred);

    int cy = 3;
    bool playing = s.state == WISP_STATE_PLAYING;
    np_wash(a, 0, 1, a->w, a->h - 2);

    if (starred)
        np_text(a, 2, cy, 3, "\xe2\x98\x85", a->th.accent2, 0);
    int tx = starred ? 5 : 2;
    draw_np_title(a, tx, cy, a->w - tx - 2, title, playing);
    np_text(a, 2, cy + 1, a->w - 4, artist, a->th.dim, 0);

    char badge[96] = "";
    bool transcoded = false;
    if (a->core && s.queue_pos < a->queue_count) {
        uint32_t tid;
        if (wisp_model_find_track(a->model, a->queue_ids[s.queue_pos], &tid)) {
            const wisp_track *t = wisp_model_track(a->model, tid);
            const char *sfx = t->suffix ? t->suffix : "";
            transcoded = *sfx && !decodable(sfx);
            char src[64];
            if (t->bit_depth > 0 && t->sampling_rate > 0)
                snprintf(src, sizeof src, "%d/%gkHz %s", t->bit_depth, t->sampling_rate / 1000.0,
                         sfx);
            else if (t->sampling_rate > 0)
                snprintf(src, sizeof src, "%gkHz %s", t->sampling_rate / 1000.0, sfx);
            else if (t->bit_rate > 0)
                snprintf(src, sizeof src, "%s \xc2\xb7 %dkbps", sfx, t->bit_rate);
            else
                snprintf(src, sizeof src, "%s", sfx);
            if (transcoded)
                snprintf(badge, sizeof badge, "%s \xe2\x86\x92 %s", src, transcode_fmt(a));
            else
                snprintf(badge, sizeof badge, "%s", src);
        }
    }
    if (badge[0]) {
        int bl = (int)strlen(badge);
        int bx = a->w - 8 - bl;
        if (bx > 22)
            np_text(a, bx, cy, bl, badge, transcoded ? a->th.accent : a->th.dim, 0);
    }

    uint32_t shc = s.shuffle ? a->th.accent : a->th.selbg;
    wisp_screen_cell(a->screen, a->w - 6, cy, 0x21c4, shc, np_bg(a, a->w - 6, cy), 0);
    wisp_screen_cell(a->screen, a->w - 4, cy, 0x21bb,
                     s.repeat != WISP_REPEAT_OFF ? a->th.accent : a->th.selbg,
                     np_bg(a, a->w - 4, cy), 0);
    if (s.repeat == WISP_REPEAT_ONE)
        wisp_screen_cell(a->screen, a->w - 3, cy, '1', a->th.accent, np_bg(a, a->w - 3, cy), 0);

    if (dur > 0) {
        char e[16], d[16];
        fmt_time((int)s.position, e, sizeof e);
        fmt_time(dur, d, sizeof d);
        char clk[40];
        snprintf(clk, sizeof clk, "%s / %s", e, d);
        np_text(a, 2, cy + 3, 16, clk, a->th.dim, 0);
        draw_bar(a, 18, cy + 3, a->w - 22, s.position / dur, a->th.accent, a->th.selbg);
    }

    int vy = cy + 5;
    int total = a->h - 4 - vy;
    if (total >= 2) {
        int viz_rows = viz_on(a) && playing ? (total > 12 ? 8 : total / 2) : 0;
        int main_rows = total - (viz_rows ? viz_rows + 1 : 0);
        if (a->np_lyrics) {
            np_text(a, 2, vy - 1, a->w - 4, "lyrics", a->th.accent, 0);
            draw_lyrics_panel(a, 2, vy, a->w - 4, main_rows, s.position);
        } else {
            np_text(a, 2, vy - 1, a->w - 4, "queue", a->th.dim, 0);
            draw_queue_panel(a, 2, vy, a->w - 4, main_rows);
        }
        if (viz_rows >= 2) {
            int vzy = vy + main_rows + 1;
            char lbl[40];
            snprintf(lbl, sizeof lbl, "visualizer \xc2\xb7 %s", VIZ_NAMES[a->config.viz_type]);
            np_text(a, 2, vzy - 1, a->w - 4, lbl, a->th.dim, 0);
            draw_viz_full(a, 2, vzy, a->w - 4, viz_rows, a->th.bg);
        }
    }
    if (a->core && a->h > 9) {
        int vy2 = a->h - 3;
        char vbuf[16];
        if (s.muted)
            snprintf(vbuf, sizeof vbuf, "muted");
        else
            snprintf(vbuf, sizeof vbuf, "vol %d%%", (int)(s.volume * 100));
        int vlen = (int)strlen(vbuf);
        int vizw = viz_on(a) && playing && a->w > 40 ? 14 : 0;
        int vizx = a->w - 2 - vizw;
        int volx = vizw ? vizx - 1 - vlen : a->w - 2 - vlen;
        if (volx < 2)
            volx = 2;
        np_text(a, volx, vy2, vlen, vbuf, a->th.dim, 0);
        if (vizw)
            draw_viz_bar(a, vizx, vy2, vizw, a->th.bg);
    }
    if (a->core)
        wisp_status_free(&s);
    draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 pick  Enter play  Space pause  ,/. seek  n/p skip  l "
                 "lyrics  v viz  r/z repeat/shuffle");
}

/* ---- search ---- */

static void draw_search(app *a) {
    wisp_screen_fill(a->screen, 0, 1, a->w, 1, ' ', a->th.fg, a->th.panel, 0);
    char prompt[160];
    snprintf(prompt, sizeof prompt, "search: %s", a->search);
    wisp_screen_text(a->screen, 1, 1, a->w - 2, prompt, a->th.accent, a->th.panel, WISP_ATTR_BOLD);
    wisp_screen_cell(a->screen, 1 + 8 + a->search_len, 1, '_', a->th.accent, a->th.panel,
                     WISP_ATTR_BOLD);

    int rows = a->h - 5;
    int per = rows / 2;
    ensure_visible(&a->search_sel, &a->search_scroll, per, (int)a->search_count);
    for (int i = 0; i < per; i++) {
        int idx = a->search_scroll + i;
        int yy = 2 + i * 2;
        if (idx >= (int)a->search_count)
            break;
        const wisp_track *t = wisp_model_track(a->model, a->search_ids[idx]);
        draw_track_2line(a, 0, yy, a->w, t, idx == a->search_sel);
    }
    draw_hint(a, "type to search   \xe2\x86\x91\xe2\x86\x93 select   Enter play   Tab views   Esc "
                 "back");
}

/* ---- playlists ---- */

static const wisp_playlist *cur_playlist(app *a) {
    return wisp_model_playlist(a->model, (uint32_t)a->pl_sel[0]);
}

static void draw_playlists(app *a) {
    int lw = a->w < 80 ? 24 : 32;
    int rows = a->h - 5;
    int pcount = (int)wisp_model_playlist_count(a->model);
    ensure_visible(&a->pl_sel[0], &a->pl_scroll[0], rows, pcount);

    wisp_screen_fill(a->screen, 0, 1, lw, 1, ' ', a->pl_pane == 0 ? a->th.accent : a->th.dim,
                     a->th.panel, WISP_ATTR_BOLD);
    wisp_screen_text(a->screen, 1, 1, lw - 2, "Playlists",
                     a->pl_pane == 0 ? a->th.accent : a->th.dim, a->th.panel, WISP_ATTR_BOLD);
    for (int i = 0; i < rows; i++) {
        int idx = a->pl_scroll[0] + i;
        int yy = 2 + i;
        bool sel = idx == a->pl_sel[0];
        bool act = sel && a->pl_pane == 0;
        uint32_t fg = act ? a->th.selfg : a->th.fg,
                 bg = sel ? (act ? a->th.selbg : a->th.panel) : a->th.bg;
        wisp_screen_fill(a->screen, 0, yy, lw, 1, ' ', fg, bg, 0);
        if (idx < pcount) {
            const wisp_playlist *p = wisp_model_playlist(a->model, (uint32_t)idx);
            bool playing = is_playing_src(a, 2, p->ext_id);
            char buf[128];
            snprintf(buf, sizeof buf, "%s", p->name ? p->name : "?");
            if (playing && !act)
                fg = a->th.accent;
            if (playing)
                wisp_screen_text(a->screen, 0, yy, 2, "\xe2\x96\xb6",
                                 pulse(a, a->th.accent, a->th.accent2), bg, 0);
            wisp_screen_text(a->screen, 1, yy, lw - 2, buf, fg, bg,
                             act || playing ? WISP_ATTR_BOLD : 0);
        }
    }

    const wisp_playlist *p = cur_playlist(a);
    int tcount = p ? (int)p->track_count : 0;
    int per = rows / 2;
    ensure_visible(&a->pl_sel[1], &a->pl_scroll[1], per, tcount);
    uint32_t hfg = a->pl_pane == 1 ? a->th.accent : a->th.dim;
    wisp_screen_fill(a->screen, lw, 1, a->w - lw, 1, ' ', hfg, a->th.panel, WISP_ATTR_BOLD);
    wisp_screen_text(a->screen, lw + 1, 1, a->w - lw - 2, p && p->name ? p->name : "Tracks", hfg,
                     a->th.panel, WISP_ATTR_BOLD);
    for (int i = 0; i < per; i++) {
        int idx = a->pl_scroll[1] + i;
        int yy = 2 + i * 2;
        if (idx >= tcount)
            break;
        const wisp_track *t = wisp_model_track(a->model, p->track_ids[idx]);
        draw_track_2line(a, lw, yy, a->w - lw, t, idx == a->pl_sel[1] && a->pl_pane == 1);
    }
    if (a->pl_pane == 1)
        draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 select  Enter play  x remove  K/J reorder  a "
                     "+playlist  Tab views");
    else
        draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 move  Enter open  c new  e rename  d delete  Tab "
                     "views");
}

static void draw_detail(app *a) {
    size_t count;
    const char *name, *sub;
    const uint32_t *tracks = detail_tracks(a, &count, &name, &sub);
    wisp_screen_text(a->screen, 2, 1, a->w - 4, name, a->th.accent2, a->th.bg, WISP_ATTR_BOLD);
    char meta[160];
    snprintf(meta, sizeof meta, "%s%s%zu tracks", sub, *sub ? "   \xc2\xb7   " : "", count);
    wisp_screen_text(a->screen, 2, 2, a->w - 4, meta, a->th.dim, a->th.bg, 0);
    int top = 4, rows = a->h - top - 3, per = rows / 2;
    ensure_visible(&a->detail_sel, &a->detail_scroll, per, (int)count);
    for (int i = 0; i < per; i++) {
        int idx = a->detail_scroll + i;
        int yy = top + i * 2;
        if (idx >= (int)count || !tracks)
            break;
        const wisp_track *t = wisp_model_track(a->model, tracks[idx]);
        draw_track_2line(a, 0, yy, a->w, t, idx == a->detail_sel);
    }
    if (count == 0)
        wisp_screen_text(a->screen, 3, top + rows / 2, a->w - 6, "empty", a->th.dim, a->th.bg, 0);
    draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 select  Enter play  a +playlist  s star  Esc back");
}

/* ---- popups ---- */

static void draw_help(app *a) {
    int cx = a->w / 2 - 27, cy = 2;
    if (cx < 1)
        cx = 1;
    wisp_screen_fill(a->screen, cx, cy, 54, a->h - 5, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, 50, "wisp \xe2\x80\x94 keys", a->th.accent2,
                     a->th.panel, WISP_ATTR_BOLD);
    const char *keys[][2] = {
        {"\xe2\x86\x91 \xe2\x86\x93 \xe2\x86\x90 \xe2\x86\x92", "navigate"},
        {"Tab", "switch views"},
        {"Enter", "play / descend"},
        {"Space", "pause / resume"},
        {"n / p", "next / previous"},
        {", / .", "seek -5s / +5s"},
        {"- / =", "volume down / up"},
        {"m", "mute"},
        {"s", "star / unstar"},
        {"[ / ]", "rate down / up"},
        {"a", "add track to playlist"},
        {"c / e / d", "new / rename / delete playlist"},
        {"x / K J", "remove / reorder in playlist"},
        {"r / z", "repeat / shuffle"},
        {"P", "pin album offline"},
        {"v", "cycle visualizer"},
        {"l", "lyrics (Now Playing)"},
        {"/", "search / find"},
        {"S", "settings"},
        {"R", "resync   \xc2\xb7   q quit"},
        {NULL, NULL},
    };
    for (int i = 0; keys[i][0]; i++) {
        int y = cy + 3 + i;
        wisp_screen_text(a->screen, cx + 3, y, 16, keys[i][0], a->th.accent, a->th.panel, 0);
        wisp_screen_text(a->screen, cx + 20, y, 32, keys[i][1], a->th.fg, a->th.panel, 0);
    }
    draw_hint(a, "? or Esc to close");
}

static void draw_settings(app *a) {
    int cw = 54, ch = 26;
    int cx = a->w / 2 - cw / 2, cy = a->h / 2 - ch / 2;
    if (cx < 1)
        cx = 1;
    if (cy < 1)
        cy = 1;
    wisp_screen_fill(a->screen, cx, cy, cw, ch, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, cw - 4, "Settings", a->th.accent2, a->th.panel,
                     WISP_ATTR_BOLD);

    char vals[11][64];
    if (a->config.crossfade <= 0)
        snprintf(vals[0], 64, "off");
    else
        snprintf(vals[0], 64, "%.0f s", a->config.crossfade);
    snprintf(vals[1], 64, "%s", a->config.theme ? a->config.theme : "default");
    snprintf(vals[2], 64, "%s", a->config.reduced_motion ? "on" : "off");
    snprintf(vals[3], 64, "%s", a->config.visualizer ? "on" : "off");
    snprintf(vals[4], 64, "%s", VIZ_NAMES[a->config.viz_type]);
    if (a->config.cache_max_mb <= 0)
        snprintf(vals[5], 64, "unlimited");
    else if (a->config.cache_max_mb >= 1024)
        snprintf(vals[5], 64, "%.1f GB", a->config.cache_max_mb / 1024.0);
    else
        snprintf(vals[5], 64, "%d MB", a->config.cache_max_mb);
    snprintf(vals[6], 64, "%s", transcode_fmt(a));
    snprintf(vals[7], 64, "%s", a->st_name);
    snprintf(vals[8], 64, "%s", a->st_url);
    snprintf(vals[9], 64, "%s", a->st_user);
    size_t pl = strlen(a->st_pass);
    for (size_t k = 0; k < pl && k < 63; k++)
        vals[10][k] = '*';
    vals[10][pl < 63 ? pl : 63] = '\0';
    const char *labels[] = {"Crossfade", "Theme",       "Reduced motion", "Visualizer",
                            "Viz type",  "Cache limit", "Transcode to",   "Server name",
                            "URL",       "Username",    "Password"};
    bool choice[] = {true, true, true, true, true, true, true, false, false, false, false};
    for (int i = 0; i < 11; i++) {
        int y = cy + 3 + i;
        bool sel = a->settings_sel == i;
        uint32_t fg = sel ? a->th.accent : a->th.fg;
        wisp_screen_text(a->screen, cx + 3, y, 16, labels[i], fg, a->th.panel,
                         sel ? WISP_ATTR_BOLD : 0);
        uint32_t vbg = !choice[i] && sel ? a->th.selbg : a->th.panel;
        wisp_screen_fill(a->screen, cx + 19, y, cw - 22, 1, ' ', a->th.fg, vbg, 0);
        wisp_screen_text(a->screen, cx + 19, y, cw - 23, vals[i], sel ? a->th.accent2 : a->th.dim,
                         vbg, 0);
        if (sel && choice[i]) {
            wisp_screen_text(a->screen, cx + 17, y, 2, "\xe2\x97\x80", a->th.accent, a->th.panel,
                             0);
            wisp_screen_text(a->screen, cx + 19 + (int)strlen(vals[i]) + 1, y, 2, "\xe2\x96\xb6",
                             a->th.accent, a->th.panel, 0);
        } else if (sel && !choice[i]) {
            wisp_screen_cell(a->screen, cx + 19 + (int)strlen(vals[i]), y, '_', a->th.accent, vbg,
                             0);
        }
    }
    int by = cy + 3 + 11 + 1;
    bool bsel = a->settings_sel == 11;
    wisp_screen_text(a->screen, cx + 3, by, cw - 6,
                     a->connected ? "[ Reconnect / apply ]" : "[ Connect ]",
                     bsel ? a->th.accent2 : a->th.accent, a->th.panel, bsel ? WISP_ATTR_BOLD : 0);
    if (a->connect_msg[0])
        wisp_screen_text(a->screen, cx + 3, by + 1, cw - 6, a->connect_msg,
                         a->connect_msg[0] == '!' ? a->th.red : a->th.green, a->th.panel, 0);
    draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 choose  \xe2\x86\x90\xe2\x86\x92 change / type  Enter "
                 "apply  S/Esc close");
}

static void draw_addpl(app *a) {
    int cw = 44, ch = a->h - 6;
    int cx = a->w / 2 - cw / 2, cy = 3;
    if (cx < 1)
        cx = 1;
    wisp_screen_fill(a->screen, cx, cy, cw, ch, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, cw - 4, "Add to playlist", a->th.accent2,
                     a->th.panel, WISP_ATTR_BOLD);
    int n = (int)wisp_model_playlist_count(a->model);
    int rows = ch - 4;
    int total = n + 1;
    ensure_visible(&a->addpl_sel, &a->addpl_scroll, rows, total);
    for (int i = 0; i < rows; i++) {
        int idx = a->addpl_scroll + i;
        int y = cy + 3 + i;
        if (idx >= total)
            break;
        bool sel = idx == a->addpl_sel;
        uint32_t bg = sel ? a->th.selbg : a->th.panel;
        uint32_t fg = sel ? a->th.selfg : a->th.fg;
        wisp_screen_fill(a->screen, cx + 1, y, cw - 2, 1, ' ', fg, bg, 0);
        char buf[128];
        if (idx < n)
            snprintf(buf, sizeof buf, "%s", wisp_model_playlist(a->model, (uint32_t)idx)->name);
        else
            snprintf(buf, sizeof buf, "\xef\xbc\x8b New playlist\xe2\x80\xa6");
        wisp_screen_text(a->screen, cx + 2, y, cw - 4, buf, idx < n ? fg : a->th.green, bg,
                         sel ? WISP_ATTR_BOLD : 0);
    }
    draw_hint(a, "\xe2\x86\x91\xe2\x86\x93 pick  Enter add  Esc cancel");
}

static void draw_newpl(app *a) {
    int cw = 44, cx = a->w / 2 - cw / 2, cy = a->h / 2 - 3;
    if (cx < 1)
        cx = 1;
    wisp_screen_fill(a->screen, cx, cy, cw, 5, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, cw - 4,
                     a->newpl_mode == 1 ? "Rename playlist" : "New playlist name", a->th.accent2,
                     a->th.panel, WISP_ATTR_BOLD);
    wisp_screen_fill(a->screen, cx + 2, cy + 3, cw - 4, 1, ' ', a->th.fg, a->th.selbg, 0);
    wisp_screen_text(a->screen, cx + 3, cy + 3, cw - 6, a->newpl, a->th.fg, a->th.selbg, 0);
    wisp_screen_cell(a->screen, cx + 3 + a->newpl_len, cy + 3, '_', a->th.accent, a->th.selbg, 0);
    draw_hint(a, a->newpl_mode == 1 ? "type name  Enter rename  Esc cancel"
                                    : "type name  Enter create  Esc cancel");
}

static void draw_confirm(app *a) {
    int cw = 48, cx = a->w / 2 - cw / 2, cy = a->h / 2 - 3;
    if (cx < 1)
        cx = 1;
    wisp_screen_fill(a->screen, cx, cy, cw, 5, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, cw - 4, a->confirm_msg, a->th.fg, a->th.panel,
                     WISP_ATTR_BOLD);
    wisp_screen_text(a->screen, cx + 2, cy + 3, cw - 4, "y  yes      n  cancel", a->th.dim,
                     a->th.panel, 0);
    draw_hint(a, "y confirm  n / Esc cancel");
}

static void draw_connect(app *a) {
    int cx = a->w / 2 - 24, cy = a->h / 2 - 5;
    if (cx < 1)
        cx = 1;
    wisp_screen_fill(a->screen, cx, cy, 48, 11, ' ', a->th.fg, a->th.panel, 0);
    wisp_screen_text(a->screen, cx + 2, cy + 1, 44, "Connect to Navidrome", a->th.accent2,
                     a->th.panel, WISP_ATTR_BOLD);
    const char *labels[3] = {"Server URL", "Username", "Password"};
    char *vals[3] = {a->field_url, a->field_user, a->field_pass};
    for (int i = 0; i < 3; i++) {
        int y = cy + 3 + i * 2;
        bool act = a->field == i;
        wisp_screen_text(a->screen, cx + 2, y, 12, labels[i], act ? a->th.accent : a->th.dim,
                         a->th.panel, act ? WISP_ATTR_BOLD : 0);
        char shown[128];
        if (i == 2) {
            size_t n = strlen(vals[i]);
            for (size_t k = 0; k < n && k < sizeof shown - 1; k++)
                shown[k] = '*';
            shown[n < sizeof shown - 1 ? n : sizeof shown - 1] = '\0';
        } else
            snprintf(shown, sizeof shown, "%s", vals[i]);
        uint32_t bg = act ? a->th.selbg : a->th.bg;
        wisp_screen_fill(a->screen, cx + 14, y, 30, 1, ' ', a->th.fg, bg, 0);
        wisp_screen_text(a->screen, cx + 15, y, 28, shown, a->th.fg, bg, 0);
        if (act)
            wisp_screen_cell(a->screen, cx + 15 + (int)strlen(shown), y, '_', a->th.accent, bg, 0);
    }
    wisp_screen_text(a->screen, cx + 2, cy + 9, 44, a->connect_msg,
                     a->connect_msg[0] == '!' ? a->th.red : a->th.dim, a->th.panel, 0);
    draw_hint(a, "Tab next field   Enter connect   Esc quit");
}

static void render(app *a) {
    wisp_screen_size(a->screen, &a->w, &a->h);
    if (a->h < 8 || a->w < 20)
        return;
    if (!a->core || !playing_id(a, a->cur_play_id, sizeof a->cur_play_id))
        a->cur_play_id[0] = '\0';
    wisp_screen_begin(a->screen, a->th.bg);
    if (a->view == V_CONNECT) {
        draw_connect(a);
        wisp_screen_flush(a->screen);
        return;
    }
    draw_tabs(a);
    switch (a->view) {
    case V_HOME:
        draw_home(a);
        break;
    case V_NOWPLAYING:
        draw_nowplaying(a);
        break;
    case V_LIBRARY:
        draw_library(a);
        break;
    case V_SEARCH:
        draw_search(a);
        break;
    case V_PLAYLISTS:
        draw_playlists(a);
        break;
    case V_DETAIL:
        draw_detail(a);
        break;
    }
    if (a->view != V_NOWPLAYING)
        draw_now_bar(a);
    switch (a->popup) {
    case POP_HELP:
        draw_help(a);
        break;
    case POP_SETTINGS:
        draw_settings(a);
        break;
    case POP_ADDPL:
        draw_addpl(a);
        break;
    case POP_NEWPL:
        draw_newpl(a);
        break;
    case POP_CONFIRM:
        draw_confirm(a);
        break;
    }
    wisp_screen_flush(a->screen);
}

/* ---- playback ---- */

static void set_queue(app *a, char **ids, size_t n, size_t start) {
    for (size_t i = 0; i < a->queue_count; i++)
        free(a->queue_ids[i]);
    free(a->queue_ids);
    a->queue_ids = ids;
    a->queue_count = n;
    const char **paths = malloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        paths[i] = ids[i];
    wisp_core_queue_play(a->core, paths, n, start);
    free(paths);
}

static void play_track_indices(app *a, const uint32_t *idx, size_t n, size_t start) {
    if (!n || !a->core)
        return;
    a->play_src_kind = 0;
    a->play_src_ext[0] = '\0';
    char **ids = malloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        ids[i] = wisp_strdup(wisp_model_track(a->model, idx[i])->ext_id);
    set_queue(a, ids, n, start);
}

static void play_album_from(app *a, uint32_t album_id, int start) {
    const wisp_album *al = wisp_model_album(a->model, album_id);
    if (al && al->track_count) {
        play_track_indices(a, al->track_ids, al->track_count, (size_t)start);
        a->play_src_kind = 1;
        snprintf(a->play_src_ext, sizeof a->play_src_ext, "%s", al->ext_id ? al->ext_id : "");
    }
}

static void play_playlist_from(app *a, uint32_t pl_id, int start) {
    const wisp_playlist *p = wisp_model_playlist(a->model, pl_id);
    if (p && p->track_count) {
        play_track_indices(a, p->track_ids, p->track_count, (size_t)start);
        a->play_src_kind = 2;
        snprintf(a->play_src_ext, sizeof a->play_src_ext, "%s", p->ext_id ? p->ext_id : "");
    }
}

static void shuffle_all(app *a) {
    size_t n = wisp_model_track_count(a->model);
    if (!n || !a->core)
        return;
    a->play_src_kind = 0;
    a->play_src_ext[0] = '\0';
    char **ids = malloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        ids[i] = wisp_strdup(wisp_model_track(a->model, (uint32_t)i)->ext_id);
    wisp_core_set_shuffle(a->core, true);
    a->config.shuffle = true;
    wisp_config_save(&a->config);
    size_t start = (size_t)(wisp_now_ms() % n);
    for (size_t i = 0; i < a->queue_count; i++)
        free(a->queue_ids[i]);
    free(a->queue_ids);
    a->queue_ids = ids;
    a->queue_count = n;
    const char **paths = malloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        paths[i] = ids[i];
    wisp_core_queue_play(a->core, paths, n, start);
    free(paths);
    toast(a, "shuffling your library");
}

static void recompute_search(app *a) {
    a->search_count = wisp_model_search_tracks(a->model, a->search, a->search_ids, 512);
    a->search_sel = 0;
    a->search_scroll = 0;
}

static void pin_album(app *a, uint32_t album_id) {
    const wisp_album *al = wisp_model_album(a->model, album_id);
    if (!al || !a->cache)
        return;
    bool any_pinned = false;
    for (size_t i = 0; i < al->track_count; i++)
        if (wisp_cache_is_pinned(a->cache, wisp_model_track(a->model, al->track_ids[i])->ext_id))
            any_pinned = true;
    for (size_t i = 0; i < al->track_count; i++) {
        const wisp_track *t = wisp_model_track(a->model, al->track_ids[i]);
        if (any_pinned) {
            wisp_cache_unpin(a->cache, t->ext_id);
        } else {
            bool raw = false;
            char *url = stream_url_for(a, t, &raw);
            wisp_cache_pin(a->cache, t->ext_id, url, raw ? t->size : 0);
            free(url);
        }
    }
    toast(a, any_pinned ? "album unpinned" : "album pinned for offline");
}

static void star_track(app *a, const char *ext_id) {
    if (!ext_id || !a->core)
        return;
    uint32_t id;
    if (!wisp_model_find_track(a->model, ext_id, &id))
        return;
    bool now = !wisp_model_track(a->model, id)->starred;
    wisp_model_set_track_starred(a->model, id, now);
    post(a, now ? ACT_STAR : ACT_UNSTAR, ext_id, NULL, 0);
    toast(a, now ? "starred" : "unstarred");
}

static void rate_track(app *a, const char *ext_id, int delta) {
    if (!ext_id || !a->core)
        return;
    uint32_t id;
    if (!wisp_model_find_track(a->model, ext_id, &id))
        return;
    int r = wisp_model_track(a->model, id)->rating + delta;
    if (r < 0)
        r = 0;
    if (r > 5)
        r = 5;
    wisp_model_set_track_rating(a->model, id, r);
    post(a, ACT_RATE, ext_id, NULL, r);
    char msg[32];
    snprintf(msg, sizeof msg, "rated %d\xe2\x98\x85", r);
    toast(a, msg);
}

static const char *context_id(app *a, char *buf, size_t cap) {
    if (a->view == V_LIBRARY && a->pane == 2) {
        const wisp_track *t = sel_track(a);
        if (t) {
            snprintf(buf, cap, "%s", t->ext_id);
            return buf;
        }
    }
    if (a->view == V_SEARCH && a->search_sel < (int)a->search_count) {
        snprintf(buf, cap, "%s", wisp_model_track(a->model, a->search_ids[a->search_sel])->ext_id);
        return buf;
    }
    if (a->view == V_PLAYLISTS && a->pl_pane == 1) {
        const wisp_playlist *p = cur_playlist(a);
        if (p && a->pl_sel[1] < (int)p->track_count) {
            snprintf(buf, cap, "%s",
                     wisp_model_track(a->model, p->track_ids[a->pl_sel[1]])->ext_id);
            return buf;
        }
    }
    if (a->view == V_DETAIL) {
        size_t count;
        const char *n, *s;
        const uint32_t *tracks = detail_tracks(a, &count, &n, &s);
        if (tracks && a->detail_sel < (int)count) {
            snprintf(buf, cap, "%s", wisp_model_track(a->model, tracks[a->detail_sel])->ext_id);
            return buf;
        }
    }
    if (playing_id(a, buf, cap))
        return buf;
    return NULL;
}

/* ---- lyrics/actions/sync ---- */

static void request_lyrics(app *a, const char *id) {
    wisp_mutex_lock(a->lyr_mtx);
    bool have = a->lyrics_ready && !strcmp(a->lyrics_id, id);
    if (!have)
        snprintf(a->lyrics_want, sizeof a->lyrics_want, "%s", id);
    wisp_mutex_unlock(a->lyr_mtx);
    if (!have)
        post(a, ACT_LYRICS, id, NULL, 0);
}

static void action_run(void *arg) {
    app *a = arg;
    while (atomic_load(&a->act_running)) {
        action ac;
        if (!wisp_chan_recv_timeout(a->actions, &ac, 200))
            continue;
        switch (ac.kind) {
        case ACT_NP:
            wisp_subsonic_scrobble(&a->sub, ac.id, false);
            break;
        case ACT_SUBMIT:
            wisp_subsonic_scrobble(&a->sub, ac.id, true);
            break;
        case ACT_STAR:
            wisp_subsonic_star(&a->sub, ac.id, true);
            break;
        case ACT_UNSTAR:
            wisp_subsonic_star(&a->sub, ac.id, false);
            break;
        case ACT_RATE:
            wisp_subsonic_set_rating(&a->sub, ac.id, ac.rating);
            break;
        case ACT_PL_ADD:
            wisp_subsonic_playlist_add(&a->sub, ac.arg, ac.id);
            break;
        case ACT_PL_REMOVE:
            wisp_subsonic_playlist_remove(&a->sub, ac.arg, ac.rating);
            break;
        case ACT_PL_REORDER: {
            wisp_mutex_lock(a->model_mtx);
            uint32_t pid;
            char **ids = NULL;
            char *name = NULL;
            size_t n = 0;
            if (wisp_model_find_playlist(a->model, ac.arg, &pid)) {
                const wisp_playlist *p = wisp_model_playlist(a->model, pid);
                n = p->track_count;
                name = wisp_strdup(p->name ? p->name : "");
                ids = n ? malloc(n * sizeof(char *)) : NULL;
                for (size_t i = 0; i < n && ids; i++)
                    ids[i] = wisp_strdup(wisp_model_track(a->model, p->track_ids[i])->ext_id);
            }
            wisp_mutex_unlock(a->model_mtx);
            if (name)
                wisp_subsonic_playlist_replace(&a->sub, ac.arg, name, (const char *const *)ids, n);
            for (size_t i = 0; i < n && ids; i++)
                free(ids[i]);
            free(ids);
            free(name);
            break;
        }
        case ACT_PL_CREATE: {
            char *newid = NULL;
            if (wisp_subsonic_create_playlist(&a->sub, ac.arg, ac.id[0] ? ac.id : NULL, &newid) ==
                    WISP_OK &&
                newid) {
                wisp_mutex_lock(a->model_mtx);
                uint32_t plid = wisp_model_add_playlist(a->model, newid, ac.arg);
                if (ac.id[0])
                    wisp_model_playlist_add_track(a->model, plid, ac.id);
                wisp_mutex_unlock(a->model_mtx);
            }
            free(newid);
            break;
        }
        case ACT_PL_DELETE:
            wisp_subsonic_delete_playlist(&a->sub, ac.arg);
            break;
        case ACT_PL_RENAME:
            wisp_subsonic_rename_playlist(&a->sub, ac.arg, ac.id);
            break;
        case ACT_LYRICS: {
            wisp_lyrics ly;
            wisp_err e = wisp_subsonic_get_lyrics(&a->sub, ac.id, &ly);
            wisp_mutex_lock(a->lyr_mtx);
            if (!strcmp(a->lyrics_want, ac.id)) {
                wisp_lyrics_free(&a->lyrics);
                a->lyrics = e == WISP_OK ? ly : (wisp_lyrics){0};
                snprintf(a->lyrics_id, sizeof a->lyrics_id, "%s", ac.id);
                a->lyrics_ready = true;
            } else if (e == WISP_OK) {
                wisp_lyrics_free(&ly);
            }
            wisp_mutex_unlock(a->lyr_mtx);
            break;
        }
        case ACT_QUIT:
            return;
        }
    }
}

static void handle_events(app *a) {
    if (!a->core)
        return;
    wisp_event ev;
    while (wisp_core_poll_event(a->core, &ev)) {
        if (ev.type == WISP_EV_TRACK_CHANGED && ev.queue_pos < a->queue_count) {
            const char *id = a->queue_ids[ev.queue_pos];
            if (strcmp(a->np_id, id)) {
                snprintf(a->np_id, sizeof a->np_id, "%s", id);
                post(a, ACT_NP, id, NULL, 0);
                a->np_qsel = 0;
                if (a->view == V_NOWPLAYING && a->np_lyrics)
                    request_lyrics(a, id);
            }
        } else if (ev.type == WISP_EV_TRACK_ENDED && ev.queue_pos < a->queue_count) {
            if (ev.reason == WISP_TRACK_ENDED)
                post(a, ACT_SUBMIT, a->queue_ids[ev.queue_pos], NULL, 0);
        }
    }
}

static void sync_run(void *arg) {
    app *a = arg;
    wisp_model *nm = wisp_model_new();
    wisp_subsonic_full_sync(&a->sub, nm, NULL, NULL);
    a->pending_model = nm;
    atomic_store(&a->sync_done, true);
}

static void start_sync(app *a) {
    if (a->syncing || !a->connected)
        return;
    a->syncing = true;
    atomic_store(&a->sync_done, false);
    a->sync_thread = wisp_thread_start(sync_run, a);
}

static void adopt_sync(app *a) {
    if (!atomic_load(&a->sync_done))
        return;
    atomic_store(&a->sync_done, false);
    if (a->sync_thread) {
        wisp_thread_join(a->sync_thread);
        a->sync_thread = NULL;
    }
    wisp_mutex_lock(a->model_mtx);
    wisp_model *old = a->model;
    a->model = a->pending_model;
    a->pending_model = NULL;
    wisp_mutex_unlock(a->model_mtx);
    a->syncing = false;
    regen_home(a);
    char *snap = wisp_path_join(wisp_dir_path(WISP_DIR_DATA), "library.snap");
    wisp_model_save(a->model, snap);
    free(snap);
    wisp_model_free(old);
}

static void enter_connected(app *a) {
    if (a->core)
        return;
    char *cache_dir = wisp_dir_path(WISP_DIR_CACHE);
    a->cache =
        wisp_cache_new(cache_dir, a->config.server_count && a->config.servers[0].trust_self_signed);
    free(cache_dir);
    if (a->cache)
        wisp_cache_set_limit(a->cache, (int64_t)a->config.cache_max_mb * 1024 * 1024);
    a->core = wisp_core_new();
    if (a->core) {
        wisp_core_set_source_provider(a->core, ui_provider, a);
        wisp_core_set_group_fn(a->core, ui_group, a);
        wisp_core_set_crossfade(a->core, a->config.crossfade);
        wisp_core_set_volume(a->core, a->config.volume);
        wisp_core_set_repeat(a->core, (wisp_repeat)a->config.repeat);
        wisp_core_set_shuffle(a->core, a->config.shuffle);
    }
    a->actions = wisp_chan_new(sizeof(action), 64);
    a->act_thread = wisp_thread_start(action_run, a);
    char *snap = wisp_path_join(wisp_dir_path(WISP_DIR_DATA), "library.snap");
    bool have = wisp_model_load(a->model, snap) && wisp_model_track_count(a->model) > 0;
    free(snap);
    regen_home(a);
    if (!have)
        start_sync(a);
}

static bool try_connect(app *a, const char *url, const char *user, const char *pass,
                        const char *name) {
    wisp_subsonic ns;
    wisp_subsonic_init(&ns, url, user, pass, false);
    char *label = NULL;
    wisp_err e = wisp_subsonic_ping(&ns, &label);
    if (e != WISP_OK) {
        snprintf(a->connect_msg, sizeof a->connect_msg, "! %s",
                 e == WISP_ERR_AUTH ? "wrong username or password" : "could not reach server");
        wisp_subsonic_free(&ns);
        free(label);
        return false;
    }
    wisp_subsonic_negotiate_caps(&ns);
    wisp_subsonic_free(&a->sub);
    a->sub = ns;
    if (a->config.server_count == 0)
        wisp_config_add_server(&a->config);
    wisp_server *sv = &a->config.servers[0];
    set_str(&sv->name, name && *name ? name : (label ? label : "navidrome server"));
    set_str(&sv->url, url);
    set_str(&sv->username, user);
    set_str(&sv->password, pass);
    wisp_config_save(&a->config);
    free(label);
    a->connected = true;
    snprintf(a->connect_msg, sizeof a->connect_msg, "connected");
    enter_connected(a);
    start_sync(a);
    return true;
}

/* ---- input ---- */

static char *ttrim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static theme parse_theme_file(const char *path) {
    theme t = theme_default();
    void *data = NULL;
    if (!wisp_file_read(path, &data, NULL))
        return t;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *s = ttrim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = ttrim(s);
        uint32_t v = (uint32_t)strtoul(ttrim(eq + 1), NULL, 16);
        if (!strcmp(k, "bg"))
            t.bg = v;
        else if (!strcmp(k, "panel"))
            t.panel = v;
        else if (!strcmp(k, "header"))
            t.header = v;
        else if (!strcmp(k, "fg"))
            t.fg = v;
        else if (!strcmp(k, "dim"))
            t.dim = v;
        else if (!strcmp(k, "accent"))
            t.accent = v;
        else if (!strcmp(k, "accent2"))
            t.accent2 = v;
        else if (!strcmp(k, "selbg"))
            t.selbg = v;
        else if (!strcmp(k, "selfg"))
            t.selfg = v;
        else if (!strcmp(k, "green"))
            t.green = v;
        else if (!strcmp(k, "red"))
            t.red = v;
    }
    free(data);
    return t;
}

static void theme_add(app *a, const char *name, theme th) {
    if (a->theme_count >= 32)
        return;
    for (int i = 0; i < a->theme_count; i++)
        if (!strcmp(a->themes[i].name, name))
            return;
    snprintf(a->themes[a->theme_count].name, sizeof a->themes[0].name, "%s", name);
    a->themes[a->theme_count].th = th;
    a->theme_count++;
}

typedef struct {
    app *a;
    const char *dir;
} theme_scan;

static void theme_scan_cb(void *ctx, const char *name) {
    theme_scan *sc = ctx;
    size_t len = strlen(name);
    if (len < 7 || strcmp(name + len - 6, ".theme") != 0)
        return;
    char base[32];
    snprintf(base, sizeof base, "%.*s", (int)(len - 6), name);
    char *path = wisp_path_join(sc->dir, name);
    if (path) {
        theme_add(sc->a, base, parse_theme_file(path));
        free(path);
    }
}

static void load_themes(app *a) {
    a->theme_count = 0;
    theme_add(a, "default", theme_default());
    char *dir = wisp_dir_path(WISP_DIR_CONFIG);
    if (dir) {
        theme_scan sc = {a, dir};
        wisp_dir_list(dir, theme_scan_cb, &sc);
        free(dir);
    }
}

static void apply_theme(app *a) {
    a->th = theme_default();
    for (int i = 0; i < a->theme_count; i++)
        if (a->config.theme && !strcmp(a->themes[i].name, a->config.theme))
            a->th = a->themes[i].th;
}

static void utf8_push(char *field, int *len, size_t cap, int key) {
    uint32_t cp = (uint32_t)key;
    char b[4];
    size_t n = 0;
    if (cp < 0x80)
        b[n++] = (char)cp;
    else if (cp < 0x800) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    if (*len + (int)n >= (int)cap - 1)
        return;
    for (size_t k = 0; k < n; k++)
        field[(*len)++] = b[k];
    field[*len] = '\0';
}

static void find_jump(app *a) {
    int count = pane_count(a, a->pane);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            char buf[256];
            pane_label(a, a->pane, i, buf, sizeof buf);
            const char *hay = buf;
            if (a->pane == 2)
                while (*hay == ' ' || *hay == '*' || (*hay >= '0' && *hay <= '9'))
                    hay++;
            if (pass == 0 ? ci_prefix(hay, a->find) : ci_contains(hay, a->find)) {
                a->sel[a->pane] = i;
                if (a->pane == 0)
                    a->sel[1] = a->sel[2] = 0;
                if (a->pane == 1)
                    a->sel[2] = 0;
                return;
            }
        }
    }
}

static void transport(app *a, int key) {
    char idbuf[256];
    switch (key) {
    case ' ':
        wisp_core_toggle_pause(a->core);
        break;
    case 'n':
    case 'p': {
        uint64_t now = wisp_now_ms();
        if (now - a->last_skip_ms < 140)
            break;
        a->last_skip_ms = now;
        if (key == 'n')
            wisp_core_next(a->core);
        else
            wisp_core_prev(a->core);
        break;
    }
    case '.':
        wisp_core_seek_by(a->core, 5);
        break;
    case ',':
        wisp_core_seek_by(a->core, -5);
        break;
    case '=':
    case '+': {
        wisp_status s = wisp_core_status(a->core);
        wisp_core_set_volume(a->core, s.volume + 0.05f);
        wisp_status_free(&s);
        break;
    }
    case '-': {
        wisp_status s = wisp_core_status(a->core);
        wisp_core_set_volume(a->core, s.volume - 0.05f);
        wisp_status_free(&s);
        break;
    }
    case 'm': {
        wisp_status s = wisp_core_status(a->core);
        wisp_core_set_muted(a->core, !s.muted);
        wisp_status_free(&s);
        break;
    }
    case 's':
        star_track(a, context_id(a, idbuf, sizeof idbuf));
        break;
    case '[':
        rate_track(a, context_id(a, idbuf, sizeof idbuf), -1);
        break;
    case ']':
        rate_track(a, context_id(a, idbuf, sizeof idbuf), +1);
        break;
    }
}

static void open_addpl(app *a) {
    char buf[256];
    const char *id = context_id(a, buf, sizeof buf);
    if (!id)
        return;
    snprintf(a->addpl_song, sizeof a->addpl_song, "%s", id);
    a->addpl_sel = 0;
    a->addpl_scroll = 0;
    a->popup = POP_ADDPL;
}

static void open_settings(app *a) {
    wisp_server *sv = a->config.server_count ? &a->config.servers[0] : NULL;
    snprintf(a->st_name, sizeof a->st_name, "%s", sv && sv->name ? sv->name : "");
    snprintf(a->st_url, sizeof a->st_url, "%s", sv && sv->url ? sv->url : "");
    snprintf(a->st_user, sizeof a->st_user, "%s", sv && sv->username ? sv->username : "");
    snprintf(a->st_pass, sizeof a->st_pass, "%s", sv && sv->password ? sv->password : "");
    a->connect_msg[0] = '\0';
    a->settings_sel = 0;
    a->popup = POP_SETTINGS;
}

static void set_view(app *a, int v) {
    a->view = v;
    a->find_active = false;
    if (v == V_SEARCH) {
        a->search[0] = '\0';
        a->search_len = 0;
        a->search_count = 0;
    } else if (v == V_NOWPLAYING) {
        a->np_qsel = 0;
        if (a->np_lyrics) {
            char id[256];
            if (playing_id(a, id, sizeof id))
                request_lyrics(a, id);
        }
    }
}

static bool global_key(app *a, int key) {
    switch (key) {
    case WISP_KEY_TAB:
        set_view(a, a->view >= V_PLAYLISTS ? V_HOME : a->view + 1);
        return true;
    case '?':
        a->popup = POP_HELP;
        return true;
    case 'S':
        open_settings(a);
        return true;
    case 'R':
        start_sync(a);
        return true;
    case 'a':
        open_addpl(a);
        return true;
    case 'v':
        a->config.viz_type = (a->config.viz_type + 1) % VIZ_COUNT;
        wisp_config_save(&a->config);
        toast(a, VIZ_NAMES[a->config.viz_type]);
        return true;
    case 'r':
        if (a->core) {
            wisp_core_cycle_repeat(a->core);
            wisp_status s = wisp_core_status(a->core);
            a->config.repeat = (int)s.repeat;
            wisp_config_save(&a->config);
            toast(a, s.repeat == WISP_REPEAT_ONE   ? "repeat: one"
                     : s.repeat == WISP_REPEAT_ALL ? "repeat: all"
                                                   : "repeat: off");
            wisp_status_free(&s);
        }
        return true;
    case 'z':
        if (a->core) {
            wisp_core_toggle_shuffle(a->core);
            wisp_status s = wisp_core_status(a->core);
            a->config.shuffle = s.shuffle;
            wisp_config_save(&a->config);
            toast(a, s.shuffle ? "shuffle: on" : "shuffle: off");
            wisp_status_free(&s);
        }
        return true;
    case 'q':
        a->quit = true;
        return true;
    }
    if (a->core && key > 0 && strchr(" np,.=+-ms[]", key))
        transport(a, key);
    else
        return false;
    return true;
}

static void handle_find(app *a, int key) {
    if (key == WISP_KEY_ESC || key == WISP_KEY_ENTER) {
        a->find_active = false;
    } else if (key == WISP_KEY_UP) {
        a->sel[a->pane]--;
    } else if (key == WISP_KEY_DOWN) {
        a->sel[a->pane]++;
    } else if (key == WISP_KEY_BACKSPACE) {
        if (a->find_len > 0) {
            a->find[--a->find_len] = '\0';
            find_jump(a);
        }
    } else if (key >= 32 && key < 0x110000) {
        utf8_push(a->find, &a->find_len, sizeof a->find, key);
        find_jump(a);
    }
    if (a->pane == 0)
        a->sel[1] = a->sel[2] = 0;
    if (a->pane == 1)
        a->sel[2] = 0;
}

static void handle_library(app *a, int key) {
    switch (key) {
    case '/':
        a->find_active = true;
        a->find[0] = '\0';
        a->find_len = 0;
        return;
    case WISP_KEY_UP:
    case 'k':
        a->sel[a->pane]--;
        break;
    case WISP_KEY_DOWN:
    case 'j':
        a->sel[a->pane]++;
        break;
    case WISP_KEY_LEFT:
    case 'h':
        if (a->pane > 0)
            a->pane--;
        break;
    case WISP_KEY_RIGHT:
    case 'l':
        if (a->pane < 2)
            a->pane++;
        break;
    case WISP_KEY_ENTER:
        if (a->pane < 2)
            a->pane++;
        else
            play_album_from(a, cur_album(a), a->sel[2]);
        break;
    case 'P':
        pin_album(a, cur_album(a));
        break;
    default:
        transport(a, key);
        break;
    }
    if (a->pane == 0)
        a->sel[1] = a->sel[2] = 0;
    if (a->pane == 1)
        a->sel[2] = 0;
}

static void handle_search(app *a, int key) {
    if (key == WISP_KEY_TAB) {
        set_view(a, V_PLAYLISTS);
    } else if (key == WISP_KEY_ESC) {
        set_view(a, V_HOME);
    } else if (key == WISP_KEY_ENTER) {
        if (a->search_count)
            play_track_indices(a, a->search_ids, a->search_count, (size_t)a->search_sel);
    } else if (key == WISP_KEY_UP) {
        a->search_sel--;
    } else if (key == WISP_KEY_DOWN) {
        a->search_sel++;
    } else if (key == WISP_KEY_BACKSPACE) {
        if (a->search_len > 0) {
            a->search[--a->search_len] = '\0';
            recompute_search(a);
        }
    } else if (key >= 32 && key < 0x110000 && a->search_len < (int)sizeof(a->search) - 4) {
        utf8_push(a->search, &a->search_len, sizeof a->search, key);
        recompute_search(a);
    }
}

static void open_detail(app *a, int kind, uint32_t id) {
    a->prev_view = a->view == V_DETAIL ? V_HOME : a->view;
    a->detail_kind = kind;
    a->detail_id = id;
    a->detail_sel = 0;
    a->detail_scroll = 0;
    a->view = V_DETAIL;
}

static void handle_home(app *a, int key) {
    switch (key) {
    case WISP_KEY_UP:
    case 'k':
        if (a->home_sec > 0)
            a->home_sec--;
        break;
    case WISP_KEY_DOWN:
    case 'j':
        if (a->home_sec < 3)
            a->home_sec++;
        break;
    case WISP_KEY_LEFT:
    case 'h':
        a->home_sel[a->home_sec]--;
        break;
    case WISP_KEY_RIGHT:
    case 'l':
        a->home_sel[a->home_sec]++;
        break;
    case WISP_KEY_ENTER: {
        int sec = a->home_sec;
        if (sec == 0) {
            shuffle_all(a);
            a->view = V_NOWPLAYING;
            break;
        }
        uint32_t id = home_item(a, sec, a->home_sel[sec]);
        if (sec == 1) {
            play_playlist_from(a, id, 0);
            a->view = V_PLAYLISTS;
            a->pl_sel[0] = (int)id;
            a->pl_pane = 1;
            a->pl_sel[1] = 0;
        } else if (sec == 2) {
            play_album_from(a, id, 0);
            a->view = V_NOWPLAYING;
            // open_detail(a, 1, id);
        } else {
            play_track_indices(a, &id, 1, 0);
            a->view = V_NOWPLAYING;
        }
        break;
    }
    default:
        transport(a, key);
        break;
    }
    if (a->home_sel[a->home_sec] < 0)
        a->home_sel[a->home_sec] = 0;
}

static void handle_nowplaying(app *a, int key) {
    if (key == 'l' || key == 'L') {
        a->np_lyrics = !a->np_lyrics;
        if (a->np_lyrics) {
            char id[256];
            if (playing_id(a, id, sizeof id))
                request_lyrics(a, id);
        }
    } else if (!a->np_lyrics && (key == WISP_KEY_UP || key == 'k')) {
        a->np_qsel--;
    } else if (!a->np_lyrics && (key == WISP_KEY_DOWN || key == 'j')) {
        a->np_qsel++;
    } else if (!a->np_lyrics && key == WISP_KEY_ENTER) {
        size_t win[128];
        int cur = 0;
        int n = a->core ? wisp_core_queue_window(a->core, win, 128, &cur) : 0;
        int sel = cur + a->np_qsel;
        if (n > 0 && sel >= 0 && sel < n) {
            wisp_core_jump_to(a->core, win[sel]);
            a->np_qsel = 0;
        }
    } else {
        transport(a, key);
    }
}

static void handle_playlists(app *a, int key) {
    switch (key) {
    case WISP_KEY_UP:
    case 'k':
        a->pl_sel[a->pl_pane]--;
        break;
    case WISP_KEY_DOWN:
    case 'j':
        a->pl_sel[a->pl_pane]++;
        break;
    case WISP_KEY_LEFT:
    case 'h':
        a->pl_pane = 0;
        break;
    case WISP_KEY_RIGHT:
    case 'l':
        a->pl_pane = 1;
        break;
    case WISP_KEY_ENTER:
        if (a->pl_pane == 0) {
            a->pl_pane = 1;
            a->pl_sel[1] = 0;
        } else {
            play_playlist_from(a, (uint32_t)a->pl_sel[0], a->pl_sel[1]);
        }
        break;
    case 'c':
        a->newpl[0] = '\0';
        a->newpl_len = 0;
        a->newpl_song[0] = '\0';
        a->newpl_mode = 0;
        a->popup = POP_NEWPL;
        break;
    case 'e': {
        const wisp_playlist *p = cur_playlist(a);
        if (p) {
            snprintf(a->newpl, sizeof a->newpl, "%s", p->name ? p->name : "");
            a->newpl_len = (int)strlen(a->newpl);
            a->newpl_mode = 1;
            a->newpl_target_id = (uint32_t)a->pl_sel[0];
            snprintf(a->newpl_target, sizeof a->newpl_target, "%s", p->ext_id);
            a->popup = POP_NEWPL;
        }
        break;
    }
    case 'd': {
        const wisp_playlist *p = cur_playlist(a);
        if (p) {
            a->confirm_kind = 1;
            a->confirm_id = (uint32_t)a->pl_sel[0];
            snprintf(a->confirm_ext, sizeof a->confirm_ext, "%s", p->ext_id);
            snprintf(a->confirm_msg, sizeof a->confirm_msg, "Delete playlist \"%s\"?",
                     p->name ? p->name : "?");
            a->popup = POP_CONFIRM;
        }
        break;
    }
    case 'x': {
        const wisp_playlist *p = cur_playlist(a);
        if (a->pl_pane == 1 && p && a->pl_sel[1] < (int)p->track_count) {
            const wisp_track *t = wisp_model_track(a->model, p->track_ids[a->pl_sel[1]]);
            a->confirm_kind = 2;
            a->confirm_id = (uint32_t)a->pl_sel[0];
            a->confirm_index = a->pl_sel[1];
            snprintf(a->confirm_ext, sizeof a->confirm_ext, "%s", p->ext_id);
            snprintf(a->confirm_msg, sizeof a->confirm_msg, "Remove \"%s\" from playlist?",
                     t && t->title ? t->title : "track");
            a->popup = POP_CONFIRM;
        }
        break;
    }
    case 'K': {
        const wisp_playlist *p = cur_playlist(a);
        if (a->pl_pane == 1 && p && a->pl_sel[1] > 0) {
            wisp_mutex_lock(a->model_mtx);
            wisp_model_playlist_move_track(a->model, (uint32_t)a->pl_sel[0], (size_t)a->pl_sel[1],
                                           -1);
            wisp_mutex_unlock(a->model_mtx);
            a->pl_sel[1]--;
            post(a, ACT_PL_REORDER, NULL, p->ext_id, 0);
        }
        break;
    }
    case 'J': {
        const wisp_playlist *p = cur_playlist(a);
        if (a->pl_pane == 1 && p && a->pl_sel[1] + 1 < (int)p->track_count) {
            wisp_mutex_lock(a->model_mtx);
            wisp_model_playlist_move_track(a->model, (uint32_t)a->pl_sel[0], (size_t)a->pl_sel[1],
                                           +1);
            wisp_mutex_unlock(a->model_mtx);
            a->pl_sel[1]++;
            post(a, ACT_PL_REORDER, NULL, p->ext_id, 0);
        }
        break;
    }
    default:
        transport(a, key);
        break;
    }
    if (a->pl_pane == 0)
        a->pl_sel[1] = 0;
}

static void handle_detail(app *a, int key) {
    size_t count;
    const char *n, *s;
    const uint32_t *tracks = detail_tracks(a, &count, &n, &s);
    switch (key) {
    case WISP_KEY_ESC:
        a->view = a->prev_view ? a->prev_view : V_HOME;
        break;
    case WISP_KEY_UP:
    case 'k':
        a->detail_sel--;
        break;
    case WISP_KEY_DOWN:
    case 'j':
        a->detail_sel++;
        break;
    case WISP_KEY_ENTER:
        if (tracks && count) {
            if (a->detail_kind == 1)
                play_album_from(a, a->detail_id, a->detail_sel);
            else
                play_playlist_from(a, a->detail_id, a->detail_sel);
        }
        break;
    default:
        transport(a, key);
        break;
    }
    if (a->detail_sel < 0)
        a->detail_sel = 0;
}

static void handle_settings(app *a, int key) {
    int *sel = &a->settings_sel;
    if (key == WISP_KEY_ESC || key == 'S') {
        a->popup = POP_NONE;
        return;
    }
    if (key == WISP_KEY_UP) {
        *sel = (*sel + 11) % 12;
        return;
    }
    if (key == WISP_KEY_DOWN) {
        *sel = (*sel + 1) % 12;
        return;
    }
    bool changed = false;
    if (*sel <= 6 && (key == WISP_KEY_LEFT || key == WISP_KEY_RIGHT)) {
        int dir = key == WISP_KEY_RIGHT ? 1 : -1;
        if (*sel == 0) {
            a->config.crossfade += dir * 2.0f;
            if (a->config.crossfade < 0)
                a->config.crossfade = 0;
            if (a->config.crossfade > 12)
                a->config.crossfade = 12;
            if (a->core)
                wisp_core_set_crossfade(a->core, a->config.crossfade);
        } else if (*sel == 1) {
            int cur = 0;
            for (int i = 0; i < a->theme_count; i++)
                if (a->config.theme && !strcmp(a->config.theme, a->themes[i].name))
                    cur = i;
            int n = a->theme_count > 0 ? a->theme_count : 1;
            cur = (cur + dir + n) % n;
            set_str(&a->config.theme, a->themes[cur].name);
            apply_theme(a);
        } else if (*sel == 2) {
            a->config.reduced_motion = !a->config.reduced_motion;
        } else if (*sel == 3) {
            a->config.visualizer = !a->config.visualizer;
        } else if (*sel == 4) {
            a->config.viz_type = (a->config.viz_type + dir + VIZ_COUNT) % VIZ_COUNT;
        } else if (*sel == 5) {
            int step = a->config.cache_max_mb >= 1024 ? 1024 : 256;
            if (dir < 0 && a->config.cache_max_mb <= 256)
                step = 256;
            a->config.cache_max_mb += dir * step;
            if (a->config.cache_max_mb < 0)
                a->config.cache_max_mb = 0;
            if (a->config.cache_max_mb > 65536)
                a->config.cache_max_mb = 65536;
            if (a->cache)
                wisp_cache_set_limit(a->cache, (int64_t)a->config.cache_max_mb * 1024 * 1024);
        } else {
            set_str(&a->config.transcode_format, strcmp(transcode_fmt(a), "opus") ? "opus" : "mp3");
        }
        changed = true;
    } else if (*sel >= 7 && *sel <= 10) {
        char *f = *sel == 7   ? a->st_name
                  : *sel == 8 ? a->st_url
                  : *sel == 9 ? a->st_user
                              : a->st_pass;
        size_t cap = *sel == 8 ? sizeof a->st_url : sizeof a->st_name;
        size_t len = strlen(f);
        if (key == WISP_KEY_BACKSPACE) {
            if (len)
                f[len - 1] = '\0';
        } else if (key >= 32 && key < 128 && len < cap - 1) {
            f[len] = (char)key;
            f[len + 1] = '\0';
        }
    } else if (*sel == 11 && key == WISP_KEY_ENTER) {
        try_connect(a, a->st_url, a->st_user, a->st_pass, a->st_name);
    }
    if (changed)
        wisp_config_save(&a->config);
}

static void handle_addpl(app *a, int key) {
    int n = (int)wisp_model_playlist_count(a->model);
    if (key == WISP_KEY_ESC) {
        a->popup = POP_NONE;
    } else if (key == WISP_KEY_UP) {
        a->addpl_sel--;
    } else if (key == WISP_KEY_DOWN) {
        a->addpl_sel++;
    } else if (key == WISP_KEY_ENTER) {
        if (a->addpl_sel >= n) {
            snprintf(a->newpl_song, sizeof a->newpl_song, "%s", a->addpl_song);
            a->newpl[0] = '\0';
            a->newpl_len = 0;
            a->popup = POP_NEWPL;
        } else {
            const wisp_playlist *p = wisp_model_playlist(a->model, (uint32_t)a->addpl_sel);
            uint32_t tid;
            if (wisp_model_find_track(a->model, a->addpl_song, &tid))
                wisp_model_playlist_add_track(a->model, (uint32_t)a->addpl_sel, a->addpl_song);
            post(a, ACT_PL_ADD, a->addpl_song, p->ext_id, 0);
            toast(a, "added to playlist");
            a->popup = POP_NONE;
        }
    }
    if (a->addpl_sel < 0)
        a->addpl_sel = 0;
    if (a->addpl_sel > n)
        a->addpl_sel = n;
}

static void handle_newpl(app *a, int key) {
    if (key == WISP_KEY_ESC) {
        a->popup = POP_NONE;
    } else if (key == WISP_KEY_ENTER) {
        if (a->newpl_len > 0) {
            if (a->newpl_mode == 1) {
                wisp_mutex_lock(a->model_mtx);
                wisp_model_rename_playlist(a->model, a->newpl_target_id, a->newpl);
                wisp_mutex_unlock(a->model_mtx);
                post(a, ACT_PL_RENAME, a->newpl, a->newpl_target, 0);
                toast(a, "playlist renamed");
            } else {
                post(a, ACT_PL_CREATE, a->newpl_song[0] ? a->newpl_song : NULL, a->newpl, 0);
                toast(a, "playlist created");
            }
        }
        a->popup = POP_NONE;
    } else if (key == WISP_KEY_BACKSPACE) {
        if (a->newpl_len > 0)
            a->newpl[--a->newpl_len] = '\0';
    } else if (key >= 32 && key < 0x110000) {
        utf8_push(a->newpl, &a->newpl_len, sizeof a->newpl, key);
    }
}

static void handle_confirm(app *a, int key) {
    if (key == 'y' || key == WISP_KEY_ENTER) {
        if (a->confirm_kind == 1) {
            wisp_mutex_lock(a->model_mtx);
            wisp_model_remove_playlist(a->model, a->confirm_id);
            wisp_mutex_unlock(a->model_mtx);
            post(a, ACT_PL_DELETE, NULL, a->confirm_ext, 0);
            if (a->pl_sel[0] > 0)
                a->pl_sel[0]--;
            a->pl_pane = 0;
            toast(a, "playlist deleted");
        } else if (a->confirm_kind == 2) {
            wisp_mutex_lock(a->model_mtx);
            wisp_model_playlist_remove_track(a->model, a->confirm_id, (size_t)a->confirm_index);
            wisp_mutex_unlock(a->model_mtx);
            post(a, ACT_PL_REMOVE, NULL, a->confirm_ext, a->confirm_index);
            const wisp_playlist *p = wisp_model_playlist(a->model, a->confirm_id);
            int tc = p ? (int)p->track_count : 0;
            if (a->pl_sel[1] >= tc && a->pl_sel[1] > 0)
                a->pl_sel[1]--;
            toast(a, "removed from playlist");
        }
        a->popup = POP_NONE;
    } else if (key == 'n' || key == WISP_KEY_ESC) {
        a->popup = POP_NONE;
    }
}

static void do_connect(app *a) {
    if (try_connect(a, a->field_url, a->field_user, a->field_pass, NULL))
        a->view = V_HOME;
}

static void handle_connect(app *a, int key) {
    if (key == WISP_KEY_ESC)
        a->quit = true;
    else if (key == WISP_KEY_TAB || key == WISP_KEY_DOWN)
        a->field = (a->field + 1) % 3;
    else if (key == WISP_KEY_UP)
        a->field = (a->field + 2) % 3;
    else if (key == WISP_KEY_ENTER)
        do_connect(a);
    else {
        char *f = a->field == 0 ? a->field_url : a->field == 1 ? a->field_user : a->field_pass;
        size_t cap = a->field == 0 ? sizeof a->field_url : sizeof a->field_user;
        size_t len = strlen(f);
        if (key == WISP_KEY_BACKSPACE) {
            if (len)
                f[len - 1] = '\0';
        } else if (key >= 32 && key < 128 && len < cap - 1) {
            f[len] = (char)key;
            f[len + 1] = '\0';
        }
    }
}

static void handle_key(app *a, int key) {
    if (key == WISP_KEY_NONE || key == WISP_KEY_RESIZE)
        return;
    if (a->view == V_CONNECT) {
        handle_connect(a, key);
        return;
    }
    switch (a->popup) {
    case POP_HELP:
        if (key == '?' || key == WISP_KEY_ESC || key == 'q')
            a->popup = POP_NONE;
        return;
    case POP_SETTINGS:
        handle_settings(a, key);
        return;
    case POP_ADDPL:
        handle_addpl(a, key);
        return;
    case POP_NEWPL:
        handle_newpl(a, key);
        return;
    case POP_CONFIRM:
        handle_confirm(a, key);
        return;
    }
    if (a->view == V_LIBRARY && a->find_active) {
        handle_find(a, key);
        return;
    }
    if (a->view == V_SEARCH) {
        handle_search(a, key);
        return;
    }
    if (global_key(a, key))
        return;
    switch (a->view) {
    case V_HOME:
        handle_home(a, key);
        break;
    case V_NOWPLAYING:
        handle_nowplaying(a, key);
        break;
    case V_LIBRARY:
        handle_library(a, key);
        break;
    case V_PLAYLISTS:
        handle_playlists(a, key);
        break;
    case V_DETAIL:
        handle_detail(a, key);
        break;
    }
}

static int run_loop(app *a) {
    while (!a->quit) {
        adopt_sync(a);
        handle_events(a);
        render(a);
        int key = wisp_screen_read_key(a->screen, 60);
        a->frame++;
        handle_key(a, key);
    }
    return 0;
}

int wisp_ui_run(void) {
    app a = {0};
    a.model = wisp_model_new();
    a.model_mtx = wisp_mutex_new();
    a.lyr_mtx = wisp_mutex_new();
    a.home_seed = wisp_now_ms() | 1;
    atomic_init(&a.sync_done, false);
    atomic_init(&a.act_running, true);
    wisp_config_load(&a.config);
    load_themes(&a);
    apply_theme(&a);

    bool have_server = a.config.server_count > 0;
    if (have_server) {
        wisp_server *sv = &a.config.servers[0];
        snprintf(a.field_url, sizeof a.field_url, "%s", sv->url ? sv->url : "");
        snprintf(a.field_user, sizeof a.field_user, "%s", sv->username ? sv->username : "");
        snprintf(a.field_pass, sizeof a.field_pass, "%s", sv->password ? sv->password : "");
        wisp_subsonic_init(&a.sub, sv->url, sv->username, sv->password, sv->trust_self_signed);
        char *label = NULL;
        a.connected = wisp_subsonic_ping(&a.sub, &label) == WISP_OK;
        free(label);
    }
    a.view = a.connected ? V_HOME : V_CONNECT;
    if (a.connected) {
        wisp_subsonic_negotiate_caps(&a.sub);
        enter_connected(&a);
    }

    a.screen = wisp_screen_open();
    if (!a.screen) {
        fprintf(stderr, "wisp: no terminal\n");
        return 1;
    }
    run_loop(&a);
    wisp_screen_close(a.screen);

    if (a.act_thread) {
        atomic_store(&a.act_running, false);
        post(&a, ACT_QUIT, NULL, NULL, 0);
        wisp_thread_join(a.act_thread);
    }
    if (a.actions)
        wisp_chan_free(a.actions);
    if (a.sync_thread)
        wisp_thread_join(a.sync_thread);
    if (a.core)
        wisp_core_free(a.core);
    if (a.cache)
        wisp_cache_free(a.cache);
    for (size_t i = 0; i < a.queue_count; i++)
        free(a.queue_ids[i]);
    free(a.queue_ids);
    wisp_lyrics_free(&a.lyrics);
    wisp_model_free(a.model);
    wisp_model_free(a.pending_model);
    wisp_mutex_free(a.model_mtx);
    wisp_mutex_free(a.lyr_mtx);
    wisp_subsonic_free(&a.sub);
    wisp_config_free(&a.config);
    return 0;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

static int view_by_name(const char *name) {
    if (!name)
        return V_LIBRARY;
    for (int v = V_HOME; v <= V_PLAYLISTS; v++)
        if (ieq(name, TAB_NAMES[v]))
            return v;
    if (ieq(name, "nowplaying"))
        return V_NOWPLAYING;
    if (ieq(name, "detail"))
        return V_DETAIL;
    return V_LIBRARY;
}

int wisp_ui_dump(int w, int h, const char *view) {
    app a = {0};
    a.model = wisp_model_new();
    a.model_mtx = wisp_mutex_new();
    a.lyr_mtx = wisp_mutex_new();
    wisp_config_load(&a.config);
    load_themes(&a);
    apply_theme(&a);
    a.home_seed = wisp_now_ms() | 1;
    char vname[32];
    snprintf(vname, sizeof vname, "%s", view ? view : "");
    char *colon = strchr(vname, ':');
    if (colon)
        *colon = '\0';
    a.view = view_by_name(view ? vname : NULL);
    char *snap = wisp_path_join(wisp_dir_path(WISP_DIR_DATA), "library.snap");
    bool loaded = wisp_model_load(a.model, snap);
    free(snap);
    if (!loaded) {
        fprintf(stderr, "no snapshot to render\n");
        return 1;
    }
    regen_home(&a);
    const char *q = view ? strchr(view, ':') : NULL;
    if (q && a.view == V_SEARCH) {
        snprintf(a.search, sizeof a.search, "%s", q + 1);
        a.search_len = (int)strlen(a.search);
        recompute_search(&a);
    }
    if (ieq(vname, "detail")) {
        a.detail_kind = 1;
        a.detail_id = 0;
    } else if (ieq(vname, "confirm")) {
        a.view = V_PLAYLISTS;
        a.popup = POP_CONFIRM;
        a.confirm_kind = 1;
        snprintf(a.confirm_msg, sizeof a.confirm_msg, "Delete playlist \"test\"?");
    } else if (ieq(vname, "settings")) {
        a.view = V_HOME;
        open_settings(&a);
    } else if (ieq(vname, "help")) {
        a.view = V_LIBRARY;
        a.popup = POP_HELP;
    } else if (ieq(vname, "addpl")) {
        a.view = V_LIBRARY;
        a.popup = POP_ADDPL;
    }
    a.screen = wisp_screen_open_headless(w, h);
    a.w = w;
    a.h = h;
    render(&a);
    wisp_screen_dump(a.screen, stdout);
    wisp_screen_close(a.screen);
    wisp_lyrics_free(&a.lyrics);
    wisp_model_free(a.model);
    wisp_mutex_free(a.model_mtx);
    wisp_mutex_free(a.lyr_mtx);
    wisp_config_free(&a.config);
    return 0;
}
