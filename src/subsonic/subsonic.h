#ifndef WISP_SUBSONIC_H
#define WISP_SUBSONIC_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

typedef struct {
    char *url;
    char *username;
    char *password;
    bool trust_self_signed;
    char *client;
    char *version;

    bool ext_transcode_offset;
    bool ext_song_lyrics;
    bool ext_form_post;
    bool ext_api_key_auth;
} wisp_subsonic;

void wisp_subsonic_init(wisp_subsonic *s, const char *url, const char *user, const char *pass,
                        bool trust);
void wisp_subsonic_free(wisp_subsonic *s);

wisp_err wisp_subsonic_get(wisp_subsonic *s, const char *endpoint, const char *extra_params,
                           cJSON **out_root, cJSON **out_response);

wisp_err wisp_subsonic_ping(wisp_subsonic *s, char **server_label);
wisp_err wisp_subsonic_negotiate_caps(wisp_subsonic *s);

char *wisp_subsonic_stream_url(wisp_subsonic *s, const char *track_id, const char *format,
                               int max_bitrate, int time_offset);
char *wisp_subsonic_cover_url(wisp_subsonic *s, const char *cover_id, int size);

wisp_err wisp_subsonic_scrobble(wisp_subsonic *s, const char *track_id, bool submission);
wisp_err wisp_subsonic_star(wisp_subsonic *s, const char *track_id, bool star);
wisp_err wisp_subsonic_set_rating(wisp_subsonic *s, const char *track_id, int rating);
wisp_err wisp_subsonic_create_playlist(wisp_subsonic *s, const char *name, const char *song_id,
                                       char **out_id);
wisp_err wisp_subsonic_playlist_add(wisp_subsonic *s, const char *playlist_id, const char *song_id);
wisp_err wisp_subsonic_playlist_remove(wisp_subsonic *s, const char *playlist_id, int index);
wisp_err wisp_subsonic_playlist_replace(wisp_subsonic *s, const char *playlist_id, const char *name,
                                        const char *const *song_ids, size_t n);
wisp_err wisp_subsonic_delete_playlist(wisp_subsonic *s, const char *playlist_id);
wisp_err wisp_subsonic_rename_playlist(wisp_subsonic *s, const char *playlist_id, const char *name);

typedef struct {
    int64_t ms;
    char *text;
} wisp_lyric_line;

typedef struct {
    bool synced;
    wisp_lyric_line *lines;
    size_t count;
} wisp_lyrics;

wisp_err wisp_subsonic_get_lyrics(wisp_subsonic *s, const char *track_id, wisp_lyrics *out);
void wisp_lyrics_free(wisp_lyrics *l);

#endif
