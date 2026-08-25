#ifndef WISP_MODEL_H
#define WISP_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *ext_id;
    char *name;
    char *sort_name;
    uint32_t *album_ids;
    size_t album_count;
} wisp_artist;

typedef struct {
    char *ext_id;
    char *name;
    char *sort_name;
    char *artist_name;
    uint32_t artist_id;
    int year;
    int duration;
    char *cover_art;
    bool starred;
    uint32_t *track_ids;
    size_t track_count;
} wisp_album;

typedef struct {
    char *ext_id;
    char *title;
    char *album_name;
    char *artist_name;
    uint32_t album_id;
    uint32_t artist_id;
    int track_no;
    int disc_no;
    int duration;
    int year;
    char *suffix;
    int bit_rate;
    int64_t size;
    char *cover_art;
    char *content_type;
    bool starred;
    int rating;
    int sampling_rate;
    int bit_depth;
    int channel_count;
} wisp_track;

typedef struct {
    char *ext_id;
    char *name;
    uint32_t *track_ids;
    size_t track_count;
} wisp_playlist;

typedef struct wisp_model wisp_model;

wisp_model *wisp_model_new(void);
void wisp_model_free(wisp_model *m);
void wisp_model_clear(wisp_model *m);

uint32_t wisp_model_add_artist(wisp_model *m, const wisp_artist *in);
uint32_t wisp_model_add_album(wisp_model *m, const wisp_album *in);
uint32_t wisp_model_add_track(wisp_model *m, const wisp_track *in);
uint32_t wisp_model_add_playlist(wisp_model *m, const char *ext_id, const char *name);
void wisp_model_playlist_add_track(wisp_model *m, uint32_t playlist_id, const char *track_ext_id);
bool wisp_model_find_artist(wisp_model *m, const char *ext_id, uint32_t *out);
bool wisp_model_find_album(wisp_model *m, const char *ext_id, uint32_t *out);
bool wisp_model_find_track(wisp_model *m, const char *ext_id, uint32_t *out);
bool wisp_model_find_playlist(wisp_model *m, const char *ext_id, uint32_t *out);
void wisp_model_finalize(wisp_model *m);

size_t wisp_model_artist_count(wisp_model *m);
const wisp_artist *wisp_model_artist(wisp_model *m, uint32_t id);
size_t wisp_model_album_count(wisp_model *m);
const wisp_album *wisp_model_album(wisp_model *m, uint32_t id);
size_t wisp_model_track_count(wisp_model *m);
const wisp_track *wisp_model_track(wisp_model *m, uint32_t id);
void wisp_model_set_track_starred(wisp_model *m, uint32_t id, bool starred);
void wisp_model_set_track_rating(wisp_model *m, uint32_t id, int rating);
size_t wisp_model_playlist_count(wisp_model *m);
const wisp_playlist *wisp_model_playlist(wisp_model *m, uint32_t id);
void wisp_model_rename_playlist(wisp_model *m, uint32_t id, const char *name);
void wisp_model_remove_playlist(wisp_model *m, uint32_t id);

size_t wisp_model_search_tracks(wisp_model *m, const char *query, uint32_t *out_ids, size_t max);

bool wisp_model_save(wisp_model *m, const char *path);
bool wisp_model_load(wisp_model *m, const char *path);

#endif
