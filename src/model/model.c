#include "model.h"

#include "common.h"
#include "fold.h"
#include "plat/plat.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *key;
    uint32_t val;
    bool used;
} map_slot;

typedef struct {
    map_slot *slots;
    size_t cap;
    size_t count;
} smap;

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static void smap_reset(smap *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

static void smap_put(smap *m, const char *key, uint32_t val);

static void smap_grow(smap *m) {
    size_t ncap = m->cap ? m->cap * 2 : 128;
    smap old = *m;
    m->slots = calloc(ncap, sizeof(map_slot));
    m->cap = ncap;
    m->count = 0;
    for (size_t i = 0; i < old.cap; i++)
        if (old.slots[i].used)
            smap_put(m, old.slots[i].key, old.slots[i].val);
    free(old.slots);
}

static void smap_put(smap *m, const char *key, uint32_t val) {
    if (!key)
        return;
    if ((m->count + 1) * 10 >= m->cap * 7)
        smap_grow(m);
    size_t i = hash_str(key) & (m->cap - 1);
    while (m->slots[i].used) {
        if (!strcmp(m->slots[i].key, key)) {
            m->slots[i].val = val;
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->slots[i] = (map_slot){.key = key, .val = val, .used = true};
    m->count++;
}

static bool smap_get(smap *m, const char *key, uint32_t *out) {
    if (!m->cap || !key)
        return false;
    size_t i = hash_str(key) & (m->cap - 1);
    while (m->slots[i].used) {
        if (!strcmp(m->slots[i].key, key)) {
            *out = m->slots[i].val;
            return true;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return false;
}

struct wisp_model {
    wisp_artist *artists;
    size_t artist_count, artist_cap;
    wisp_album *albums;
    size_t album_count, album_cap;
    wisp_track *tracks;
    size_t track_count, track_cap;
    wisp_playlist *playlists;
    size_t playlist_count, playlist_cap;
    smap artist_map, album_map, track_map, playlist_map;
    char **track_fold;
};

wisp_model *wisp_model_new(void) { return calloc(1, sizeof(wisp_model)); }

void wisp_model_clear(wisp_model *m) {
    for (size_t i = 0; i < m->artist_count; i++) {
        free(m->artists[i].ext_id);
        free(m->artists[i].name);
        free(m->artists[i].sort_name);
        free(m->artists[i].album_ids);
    }
    for (size_t i = 0; i < m->album_count; i++) {
        free(m->albums[i].ext_id);
        free(m->albums[i].name);
        free(m->albums[i].sort_name);
        free(m->albums[i].artist_name);
        free(m->albums[i].cover_art);
        free(m->albums[i].track_ids);
    }
    for (size_t i = 0; i < m->track_count; i++) {
        free(m->tracks[i].ext_id);
        free(m->tracks[i].title);
        free(m->tracks[i].album_name);
        free(m->tracks[i].artist_name);
        free(m->tracks[i].suffix);
        free(m->tracks[i].cover_art);
        free(m->tracks[i].content_type);
        if (m->track_fold)
            free(m->track_fold[i]);
    }
    for (size_t i = 0; i < m->playlist_count; i++) {
        free(m->playlists[i].ext_id);
        free(m->playlists[i].name);
        free(m->playlists[i].track_ids);
    }
    free(m->artists);
    free(m->albums);
    free(m->tracks);
    free(m->playlists);
    free(m->track_fold);
    smap_reset(&m->artist_map);
    smap_reset(&m->album_map);
    smap_reset(&m->track_map);
    smap_reset(&m->playlist_map);
    memset(m, 0, sizeof *m);
}

void wisp_model_free(wisp_model *m) {
    if (!m)
        return;
    wisp_model_clear(m);
    free(m);
}

#define GROW(arr, count, cap)                                                                      \
    do {                                                                                           \
        if ((count) == (cap)) {                                                                    \
            (cap) = (cap) ? (cap) * 2 : 64;                                                        \
            (arr) = realloc((arr), (cap) * sizeof *(arr));                                         \
        }                                                                                          \
    } while (0)

uint32_t wisp_model_add_artist(wisp_model *m, const wisp_artist *in) {
    GROW(m->artists, m->artist_count, m->artist_cap);
    wisp_artist *a = &m->artists[m->artist_count];
    memset(a, 0, sizeof *a);
    a->ext_id = wisp_strdup(in->ext_id);
    a->name = wisp_strdup(in->name);
    a->sort_name = wisp_strdup(in->sort_name);
    uint32_t id = (uint32_t)m->artist_count++;
    smap_put(&m->artist_map, a->ext_id, id);
    return id;
}

uint32_t wisp_model_add_album(wisp_model *m, const wisp_album *in) {
    GROW(m->albums, m->album_count, m->album_cap);
    wisp_album *a = &m->albums[m->album_count];
    memset(a, 0, sizeof *a);
    a->ext_id = wisp_strdup(in->ext_id);
    a->name = wisp_strdup(in->name);
    a->sort_name = wisp_strdup(in->sort_name);
    a->artist_name = wisp_strdup(in->artist_name);
    a->artist_id = in->artist_id;
    a->year = in->year;
    a->duration = in->duration;
    a->cover_art = wisp_strdup(in->cover_art);
    a->starred = in->starred;
    uint32_t id = (uint32_t)m->album_count++;
    smap_put(&m->album_map, a->ext_id, id);
    return id;
}

uint32_t wisp_model_add_track(wisp_model *m, const wisp_track *in) {
    GROW(m->tracks, m->track_count, m->track_cap);
    wisp_track *t = &m->tracks[m->track_count];
    *t = *in;
    t->ext_id = wisp_strdup(in->ext_id);
    t->title = wisp_strdup(in->title);
    t->album_name = wisp_strdup(in->album_name);
    t->artist_name = wisp_strdup(in->artist_name);
    t->suffix = wisp_strdup(in->suffix);
    t->cover_art = wisp_strdup(in->cover_art);
    t->content_type = wisp_strdup(in->content_type);
    uint32_t id = (uint32_t)m->track_count++;
    smap_put(&m->track_map, t->ext_id, id);
    return id;
}

static void push_id(uint32_t **arr, size_t *count, uint32_t id);

uint32_t wisp_model_add_playlist(wisp_model *m, const char *ext_id, const char *name) {
    GROW(m->playlists, m->playlist_count, m->playlist_cap);
    wisp_playlist *p = &m->playlists[m->playlist_count];
    memset(p, 0, sizeof *p);
    p->ext_id = wisp_strdup(ext_id);
    p->name = wisp_strdup(name);
    uint32_t id = (uint32_t)m->playlist_count++;
    smap_put(&m->playlist_map, p->ext_id, id);
    return id;
}

void wisp_model_playlist_add_track(wisp_model *m, uint32_t playlist_id, const char *track_ext_id) {
    if (playlist_id >= m->playlist_count)
        return;
    uint32_t tid;
    if (!smap_get(&m->track_map, track_ext_id, &tid))
        return;
    wisp_playlist *p = &m->playlists[playlist_id];
    push_id(&p->track_ids, &p->track_count, tid);
}

bool wisp_model_find_artist(wisp_model *m, const char *ext_id, uint32_t *out) {
    return smap_get(&m->artist_map, ext_id, out);
}
bool wisp_model_find_album(wisp_model *m, const char *ext_id, uint32_t *out) {
    return smap_get(&m->album_map, ext_id, out);
}
bool wisp_model_find_track(wisp_model *m, const char *ext_id, uint32_t *out) {
    return smap_get(&m->track_map, ext_id, out);
}
bool wisp_model_find_playlist(wisp_model *m, const char *ext_id, uint32_t *out) {
    return smap_get(&m->playlist_map, ext_id, out);
}

static void push_id(uint32_t **arr, size_t *count, uint32_t id) {
    *arr = realloc(*arr, (*count + 1) * sizeof(uint32_t));
    (*arr)[(*count)++] = id;
}

void wisp_model_finalize(wisp_model *m) {
    for (size_t i = 0; i < m->artist_count; i++) {
        free(m->artists[i].album_ids);
        m->artists[i].album_ids = NULL;
        m->artists[i].album_count = 0;
    }
    for (size_t i = 0; i < m->album_count; i++) {
        free(m->albums[i].track_ids);
        m->albums[i].track_ids = NULL;
        m->albums[i].track_count = 0;
    }
    for (size_t i = 0; i < m->album_count; i++) {
        wisp_album *al = &m->albums[i];
        if (al->artist_id < m->artist_count)
            push_id(&m->artists[al->artist_id].album_ids, &m->artists[al->artist_id].album_count,
                    (uint32_t)i);
    }
    free(m->track_fold);
    m->track_fold = calloc(m->track_count ? m->track_count : 1, sizeof(char *));
    for (size_t i = 0; i < m->track_count; i++) {
        wisp_track *t = &m->tracks[i];
        if (t->album_id < m->album_count)
            push_id(&m->albums[t->album_id].track_ids, &m->albums[t->album_id].track_count,
                    (uint32_t)i);
        char *combined = wisp_aprintf("%s %s %s", t->title ? t->title : "",
                                      t->artist_name ? t->artist_name : "",
                                      t->album_name ? t->album_name : "");
        m->track_fold[i] = wisp_fold(combined);
        free(combined);
    }
}

size_t wisp_model_artist_count(wisp_model *m) { return m->artist_count; }
const wisp_artist *wisp_model_artist(wisp_model *m, uint32_t id) {
    return id < m->artist_count ? &m->artists[id] : NULL;
}
size_t wisp_model_album_count(wisp_model *m) { return m->album_count; }
const wisp_album *wisp_model_album(wisp_model *m, uint32_t id) {
    return id < m->album_count ? &m->albums[id] : NULL;
}
size_t wisp_model_track_count(wisp_model *m) { return m->track_count; }
const wisp_track *wisp_model_track(wisp_model *m, uint32_t id) {
    return id < m->track_count ? &m->tracks[id] : NULL;
}

size_t wisp_model_playlist_count(wisp_model *m) { return m->playlist_count; }
const wisp_playlist *wisp_model_playlist(wisp_model *m, uint32_t id) {
    return id < m->playlist_count ? &m->playlists[id] : NULL;
}

void wisp_model_rename_playlist(wisp_model *m, uint32_t id, const char *name) {
    if (id < m->playlist_count) {
        free(m->playlists[id].name);
        m->playlists[id].name = wisp_strdup(name);
    }
}

void wisp_model_remove_playlist(wisp_model *m, uint32_t id) {
    if (id >= m->playlist_count)
        return;
    free(m->playlists[id].ext_id);
    free(m->playlists[id].name);
    free(m->playlists[id].track_ids);
    for (size_t i = id; i + 1 < m->playlist_count; i++)
        m->playlists[i] = m->playlists[i + 1];
    m->playlist_count--;
    smap_reset(&m->playlist_map);
    for (size_t i = 0; i < m->playlist_count; i++)
        smap_put(&m->playlist_map, m->playlists[i].ext_id, (uint32_t)i);
}

void wisp_model_set_track_starred(wisp_model *m, uint32_t id, bool starred) {
    if (id < m->track_count)
        m->tracks[id].starred = starred;
}

void wisp_model_set_track_rating(wisp_model *m, uint32_t id, int rating) {
    if (id < m->track_count)
        m->tracks[id].rating = rating;
}

static bool is_subseq(const char *hay, const char *needle) {
    while (*needle) {
        hay = strchr(hay, *needle);
        if (!hay)
            return false;
        hay++;
        needle++;
    }
    return true;
}

static bool word_prefix(const char *hay, const char *q, size_t qlen) {
    for (const char *p = hay; p;) {
        if (!strncmp(p, q, qlen))
            return true;
        p = strchr(p, ' ');
        if (p)
            p++;
    }
    return false;
}

static int score_track(const char *hay, const char *q, size_t qlen) {
    if (!hay)
        return 0;
    if (word_prefix(hay, q, qlen))
        return 3;
    if (strstr(hay, q))
        return 2;
    if (is_subseq(hay, q))
        return 1;
    return 0;
}

typedef struct {
    uint32_t id;
    int score;
} hit;

static int cmp_hits(const void *a, const void *b) {
    const hit *x = a, *y = b;
    if (x->score != y->score)
        return y->score - x->score;
    return x->id < y->id ? -1 : x->id > y->id;
}

size_t wisp_model_search_tracks(wisp_model *m, const char *query, uint32_t *out_ids, size_t max) {
    char *q = wisp_fold(query);
    if (!q || !*q) {
        free(q);
        return 0;
    }
    size_t qlen = strlen(q);
    hit *hits = malloc((m->track_count ? m->track_count : 1) * sizeof *hits);
    size_t nh = 0;
    for (size_t i = 0; i < m->track_count; i++) {
        int sc = score_track(m->track_fold[i], q, qlen);
        if (sc)
            hits[nh++] = (hit){.id = (uint32_t)i, .score = sc};
    }
    qsort(hits, nh, sizeof *hits, cmp_hits);
    size_t n = nh < max ? nh : max;
    for (size_t i = 0; i < n; i++)
        out_ids[i] = hits[i].id;
    free(hits);
    free(q);
    return n;
}

typedef struct {
    unsigned char *data;
    size_t len, cap;
} buf;

static void buf_need(buf *b, size_t n) {
    if (b->len + n > b->cap) {
        while (b->len + n > b->cap)
            b->cap = b->cap ? b->cap * 2 : 8192;
        b->data = realloc(b->data, b->cap);
    }
}
static void put_u32(buf *b, uint32_t v) {
    buf_need(b, 4);
    for (int i = 0; i < 4; i++)
        b->data[b->len++] = (v >> (i * 8)) & 0xFF;
}
static void put_u64(buf *b, uint64_t v) {
    buf_need(b, 8);
    for (int i = 0; i < 8; i++)
        b->data[b->len++] = (v >> (i * 8)) & 0xFF;
}
static void put_str(buf *b, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    put_u32(b, n);
    buf_need(b, n);
    if (n) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
    }
}
static void put_u8(buf *b, uint8_t v) {
    buf_need(b, 1);
    b->data[b->len++] = v;
}

typedef struct {
    const unsigned char *data;
    size_t len, pos;
    bool err;
} rdr;

static uint32_t get_u32(rdr *r) {
    if (r->pos + 4 > r->len) {
        r->err = true;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)r->data[r->pos++] << (i * 8);
    return v;
}
static uint64_t get_u64(rdr *r) {
    if (r->pos + 8 > r->len) {
        r->err = true;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)r->data[r->pos++] << (i * 8);
    return v;
}
static char *get_str(rdr *r) {
    uint32_t n = get_u32(r);
    if (r->err || r->pos + n > r->len) {
        r->err = true;
        return NULL;
    }
    char *s = malloc(n + 1);
    memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    return s;
}
static uint8_t get_u8(rdr *r) {
    if (r->pos + 1 > r->len) {
        r->err = true;
        return 0;
    }
    return r->data[r->pos++];
}

#define WISP_SNAP_MAGIC 0x31505357u
#define WISP_SNAP_VERSION 3

bool wisp_model_save(wisp_model *m, const char *path) {
    buf b = {0};
    put_u32(&b, WISP_SNAP_MAGIC);
    put_u32(&b, WISP_SNAP_VERSION);

    put_u32(&b, (uint32_t)m->artist_count);
    for (size_t i = 0; i < m->artist_count; i++) {
        put_str(&b, m->artists[i].ext_id);
        put_str(&b, m->artists[i].name);
        put_str(&b, m->artists[i].sort_name);
    }
    put_u32(&b, (uint32_t)m->album_count);
    for (size_t i = 0; i < m->album_count; i++) {
        wisp_album *a = &m->albums[i];
        put_str(&b, a->ext_id);
        put_str(&b, a->name);
        put_str(&b, a->sort_name);
        put_str(&b, a->artist_name);
        put_u32(&b, a->artist_id);
        put_u32(&b, (uint32_t)a->year);
        put_u32(&b, (uint32_t)a->duration);
        put_str(&b, a->cover_art);
        put_u8(&b, a->starred ? 1 : 0);
    }
    put_u32(&b, (uint32_t)m->track_count);
    for (size_t i = 0; i < m->track_count; i++) {
        wisp_track *t = &m->tracks[i];
        put_str(&b, t->ext_id);
        put_str(&b, t->title);
        put_str(&b, t->album_name);
        put_str(&b, t->artist_name);
        put_u32(&b, t->album_id);
        put_u32(&b, t->artist_id);
        put_u32(&b, (uint32_t)t->track_no);
        put_u32(&b, (uint32_t)t->disc_no);
        put_u32(&b, (uint32_t)t->duration);
        put_u32(&b, (uint32_t)t->year);
        put_str(&b, t->suffix);
        put_u32(&b, (uint32_t)t->bit_rate);
        put_u64(&b, (uint64_t)t->size);
        put_str(&b, t->cover_art);
        put_str(&b, t->content_type);
        put_u8(&b, t->starred ? 1 : 0);
        put_u32(&b, (uint32_t)t->rating);
        put_u32(&b, (uint32_t)t->sampling_rate);
        put_u32(&b, (uint32_t)t->bit_depth);
        put_u32(&b, (uint32_t)t->channel_count);
    }
    put_u32(&b, (uint32_t)m->playlist_count);
    for (size_t i = 0; i < m->playlist_count; i++) {
        wisp_playlist *p = &m->playlists[i];
        put_str(&b, p->ext_id);
        put_str(&b, p->name);
        put_u32(&b, (uint32_t)p->track_count);
        for (size_t j = 0; j < p->track_count; j++)
            put_u32(&b, p->track_ids[j]);
    }

    bool ok = b.data && wisp_file_write(path, b.data, b.len, false);
    free(b.data);
    return ok;
}

bool wisp_model_load(wisp_model *m, const char *path) {
    void *data = NULL;
    size_t len = 0;
    if (!wisp_file_read(path, &data, &len))
        return false;
    rdr r = {.data = data, .len = len};
    uint32_t magic = get_u32(&r);
    uint32_t ver = get_u32(&r);
    if (magic != WISP_SNAP_MAGIC || ver < 1 || ver > WISP_SNAP_VERSION) {
        free(data);
        return false;
    }
    wisp_model_clear(m);

    uint32_t na = get_u32(&r);
    for (uint32_t i = 0; i < na && !r.err; i++) {
        wisp_artist a = {0};
        a.ext_id = get_str(&r);
        a.name = get_str(&r);
        a.sort_name = get_str(&r);
        GROW(m->artists, m->artist_count, m->artist_cap);
        m->artists[m->artist_count] = a;
        smap_put(&m->artist_map, a.ext_id, (uint32_t)m->artist_count);
        m->artist_count++;
    }
    uint32_t nb = get_u32(&r);
    for (uint32_t i = 0; i < nb && !r.err; i++) {
        wisp_album a = {0};
        a.ext_id = get_str(&r);
        a.name = get_str(&r);
        a.sort_name = get_str(&r);
        a.artist_name = get_str(&r);
        a.artist_id = get_u32(&r);
        a.year = (int)get_u32(&r);
        a.duration = (int)get_u32(&r);
        a.cover_art = get_str(&r);
        a.starred = get_u8(&r) != 0;
        GROW(m->albums, m->album_count, m->album_cap);
        m->albums[m->album_count] = a;
        smap_put(&m->album_map, a.ext_id, (uint32_t)m->album_count);
        m->album_count++;
    }
    uint32_t nt = get_u32(&r);
    for (uint32_t i = 0; i < nt && !r.err; i++) {
        wisp_track t = {0};
        t.ext_id = get_str(&r);
        t.title = get_str(&r);
        t.album_name = get_str(&r);
        t.artist_name = get_str(&r);
        t.album_id = get_u32(&r);
        t.artist_id = get_u32(&r);
        t.track_no = (int)get_u32(&r);
        t.disc_no = (int)get_u32(&r);
        t.duration = (int)get_u32(&r);
        t.year = (int)get_u32(&r);
        t.suffix = get_str(&r);
        t.bit_rate = (int)get_u32(&r);
        t.size = (int64_t)get_u64(&r);
        t.cover_art = get_str(&r);
        t.content_type = get_str(&r);
        t.starred = get_u8(&r) != 0;
        t.rating = (int)get_u32(&r);
        if (ver >= 3) {
            t.sampling_rate = (int)get_u32(&r);
            t.bit_depth = (int)get_u32(&r);
            t.channel_count = (int)get_u32(&r);
        }
        GROW(m->tracks, m->track_count, m->track_cap);
        m->tracks[m->track_count] = t;
        smap_put(&m->track_map, t.ext_id, (uint32_t)m->track_count);
        m->track_count++;
    }
    uint32_t np = ver >= 2 ? get_u32(&r) : 0;
    for (uint32_t i = 0; i < np && !r.err; i++) {
        wisp_playlist p = {0};
        p.ext_id = get_str(&r);
        p.name = get_str(&r);
        uint32_t nc = get_u32(&r);
        for (uint32_t j = 0; j < nc && !r.err; j++) {
            uint32_t tid = get_u32(&r);
            if (tid < m->track_count)
                push_id(&p.track_ids, &p.track_count, tid);
        }
        GROW(m->playlists, m->playlist_count, m->playlist_cap);
        m->playlists[m->playlist_count] = p;
        smap_put(&m->playlist_map, p.ext_id, (uint32_t)m->playlist_count);
        m->playlist_count++;
    }

    free(data);
    if (r.err) {
        wisp_model_clear(m);
        return false;
    }
    wisp_model_finalize(m);
    return true;
}
