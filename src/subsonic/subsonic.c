#include "subsonic.h"

#include "net/http.h"
#include "plat/plat.h"

#include "cJSON.h"
#include "mbedtls/md5.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void wisp_subsonic_init(wisp_subsonic *s, const char *url, const char *user, const char *pass,
                        bool trust) {
    memset(s, 0, sizeof *s);
    size_t n = url ? strlen(url) : 0;
    while (n > 0 && (url[n - 1] == '/' || url[n - 1] == ' '))
        n--;
    s->url = url ? wisp_aprintf("%.*s", (int)n, url) : wisp_strdup("");
    s->username = wisp_strdup(user ? user : "");
    s->password = wisp_strdup(pass ? pass : "");
    s->trust_self_signed = trust;
    s->client = wisp_strdup("wisp");
    s->version = wisp_strdup("1.16.1");
}

void wisp_subsonic_free(wisp_subsonic *s) {
    free(s->url);
    free(s->username);
    free(s->password);
    free(s->client);
    free(s->version);
    memset(s, 0, sizeof *s);
}

static void md5_hex(const char *in, char out[33]) {
    unsigned char d[16];
    mbedtls_md5((const unsigned char *)in, strlen(in), d);
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void gen_salt(char out[17]) {
    static uint64_t counter = 0;
    uint64_t v = wisp_now_ms() ^ (++counter * 0x9E3779B97F4A7C15ull);
    snprintf(out, 17, "%016llx", (unsigned long long)v);
}

static char *url_encode(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[j++] = (char)c;
        else {
            snprintf(out + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
    return out;
}

static char *auth_params(wisp_subsonic *s) {
    char salt[17];
    gen_salt(salt);
    char *salted = wisp_aprintf("%s%s", s->password, salt);
    char token[33];
    md5_hex(salted, token);
    free(salted);
    char *user = url_encode(s->username);
    char *params =
        wisp_aprintf("u=%s&t=%s&s=%s&c=%s&v=%s&f=json", user, token, salt, s->client, s->version);
    free(user);
    return params;
}

static wisp_err classify(cJSON *response) {
    cJSON *status = cJSON_GetObjectItem(response, "status");
    if (status && cJSON_IsString(status) && !strcmp(status->valuestring, "ok"))
        return WISP_OK;
    cJSON *error = cJSON_GetObjectItem(response, "error");
    int code = 0;
    if (error) {
        cJSON *c = cJSON_GetObjectItem(error, "code");
        if (c && cJSON_IsNumber(c))
            code = c->valueint;
    }
    return (code == 40 || code == 41 || code == 44 || code == 50) ? WISP_ERR_AUTH : WISP_ERR_SERVER;
}

wisp_err wisp_subsonic_get(wisp_subsonic *s, const char *endpoint, const char *extra,
                           cJSON **out_root, cJSON **out_response) {
    char *params = auth_params(s);
    if (!params)
        return WISP_ERR_OOM;
    char *url = extra ? wisp_aprintf("%s/rest/%s?%s&%s", s->url, endpoint, params, extra)
                      : wisp_aprintf("%s/rest/%s?%s", s->url, endpoint, params);
    free(params);
    if (!url)
        return WISP_ERR_OOM;

    wisp_http_response resp;
    wisp_err e = wisp_http_get(url, NULL, 0, s->trust_self_signed, &resp);
    free(url);
    if (e != WISP_OK)
        return e;
    if (resp.status != 200) {
        wisp_http_response_free(&resp);
        return WISP_ERR_HTTP;
    }

    cJSON *root = cJSON_Parse(resp.body);
    wisp_http_response_free(&resp);
    if (!root)
        return WISP_ERR_PARSE;
    cJSON *response = cJSON_GetObjectItem(root, "subsonic-response");
    if (!response) {
        cJSON_Delete(root);
        return WISP_ERR_PARSE;
    }
    wisp_err rc = classify(response);
    if (rc != WISP_OK) {
        cJSON_Delete(root);
        return rc;
    }
    *out_root = root;
    if (out_response)
        *out_response = response;
    return WISP_OK;
}

wisp_err wisp_subsonic_ping(wisp_subsonic *s, char **server_label) {
    cJSON *root, *response;
    wisp_err e = wisp_subsonic_get(s, "ping.view", NULL, &root, &response);
    if (e != WISP_OK)
        return e;
    if (server_label) {
        cJSON *type = cJSON_GetObjectItem(response, "type");
        cJSON *ver = cJSON_GetObjectItem(response, "serverVersion");
        *server_label = wisp_aprintf("%s %s", type && type->valuestring ? type->valuestring : "?",
                                     ver && ver->valuestring ? ver->valuestring : "");
    }
    cJSON_Delete(root);
    return WISP_OK;
}

wisp_err wisp_subsonic_negotiate_caps(wisp_subsonic *s) {
    cJSON *root, *response;
    wisp_err e = wisp_subsonic_get(s, "getOpenSubsonicExtensions.view", NULL, &root, &response);
    if (e != WISP_OK)
        return e;
    cJSON *exts = cJSON_GetObjectItem(response, "openSubsonicExtensions");
    cJSON *ext;
    cJSON_ArrayForEach(ext, exts) {
        cJSON *name = cJSON_GetObjectItem(ext, "name");
        if (!name || !name->valuestring)
            continue;
        if (!strcmp(name->valuestring, "transcodeOffset"))
            s->ext_transcode_offset = true;
        else if (!strcmp(name->valuestring, "songLyrics"))
            s->ext_song_lyrics = true;
        else if (!strcmp(name->valuestring, "formPost"))
            s->ext_form_post = true;
        else if (!strcmp(name->valuestring, "apiKeyAuthentication"))
            s->ext_api_key_auth = true;
    }
    cJSON_Delete(root);
    return WISP_OK;
}

char *wisp_subsonic_stream_url(wisp_subsonic *s, const char *track_id, const char *format,
                               int max_bitrate, int time_offset) {
    char *params = auth_params(s);
    char *id = url_encode(track_id);
    char *extra = wisp_strdup("");
    if (format && *format) {
        char *efmt = url_encode(format);
        char *n = wisp_aprintf("%s&format=%s", extra, efmt);
        free(efmt);
        free(extra);
        extra = n;
    }
    if (max_bitrate > 0) {
        char *n = wisp_aprintf("%s&maxBitRate=%d", extra, max_bitrate);
        free(extra);
        extra = n;
    }
    if (time_offset > 0) {
        char *n = wisp_aprintf("%s&timeOffset=%d", extra, time_offset);
        free(extra);
        extra = n;
    }
    char *url = wisp_aprintf("%s/rest/stream.view?%s&id=%s%s", s->url, params, id, extra);
    free(params);
    free(id);
    free(extra);
    return url;
}

char *wisp_subsonic_cover_url(wisp_subsonic *s, const char *cover_id, int size) {
    char *params = auth_params(s);
    char *id = url_encode(cover_id);
    char *url = size > 0 ? wisp_aprintf("%s/rest/getCoverArt.view?%s&id=%s&size=%d", s->url, params,
                                        id, size)
                         : wisp_aprintf("%s/rest/getCoverArt.view?%s&id=%s", s->url, params, id);
    free(params);
    free(id);
    return url;
}

static wisp_err simple_call(wisp_subsonic *s, const char *endpoint, const char *extra) {
    cJSON *root, *response;
    wisp_err e = wisp_subsonic_get(s, endpoint, extra, &root, &response);
    if (e == WISP_OK)
        cJSON_Delete(root);
    return e;
}

wisp_err wisp_subsonic_scrobble(wisp_subsonic *s, const char *track_id, bool submission) {
    char *id = url_encode(track_id);
    char *extra = wisp_aprintf("id=%s&submission=%s", id, submission ? "true" : "false");
    wisp_err e = simple_call(s, "scrobble.view", extra);
    free(id);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_star(wisp_subsonic *s, const char *track_id, bool star) {
    char *id = url_encode(track_id);
    char *extra = wisp_aprintf("id=%s", id);
    wisp_err e = simple_call(s, star ? "star.view" : "unstar.view", extra);
    free(id);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_set_rating(wisp_subsonic *s, const char *track_id, int rating) {
    if (rating < 0)
        rating = 0;
    if (rating > 5)
        rating = 5;
    char *id = url_encode(track_id);
    char *extra = wisp_aprintf("id=%s&rating=%d", id, rating);
    wisp_err e = simple_call(s, "setRating.view", extra);
    free(id);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_create_playlist(wisp_subsonic *s, const char *name, const char *song_id,
                                       char **out_id) {
    if (out_id)
        *out_id = NULL;
    char *ename = url_encode(name);
    char *extra;
    if (song_id && *song_id) {
        char *esong = url_encode(song_id);
        extra = wisp_aprintf("name=%s&songId=%s", ename, esong);
        free(esong);
    } else {
        extra = wisp_aprintf("name=%s", ename);
    }
    free(ename);
    cJSON *root, *response;
    wisp_err e = wisp_subsonic_get(s, "createPlaylist.view", extra, &root, &response);
    free(extra);
    if (e != WISP_OK)
        return e;
    if (out_id) {
        cJSON *pl = cJSON_GetObjectItem(response, "playlist");
        cJSON *id = pl ? cJSON_GetObjectItem(pl, "id") : NULL;
        if (id && cJSON_IsString(id))
            *out_id = wisp_strdup(id->valuestring);
    }
    cJSON_Delete(root);
    return WISP_OK;
}

wisp_err wisp_subsonic_playlist_add(wisp_subsonic *s, const char *playlist_id,
                                    const char *song_id) {
    char *epl = url_encode(playlist_id);
    char *esong = url_encode(song_id);
    char *extra = wisp_aprintf("playlistId=%s&songIdToAdd=%s", epl, esong);
    wisp_err e = simple_call(s, "updatePlaylist.view", extra);
    free(epl);
    free(esong);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_playlist_remove(wisp_subsonic *s, const char *playlist_id, int index) {
    char *epl = url_encode(playlist_id);
    char *extra = wisp_aprintf("playlistId=%s&songIndexToRemove=%d", epl, index);
    wisp_err e = simple_call(s, "updatePlaylist.view", extra);
    free(epl);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_playlist_replace(wisp_subsonic *s, const char *playlist_id, const char *name,
                                        const char *const *song_ids, size_t n) {
    char *epl = url_encode(playlist_id);
    char *ename = url_encode(name ? name : "");
    char *extra = wisp_aprintf("playlistId=%s&name=%s", epl, ename);
    free(epl);
    free(ename);
    for (size_t i = 0; i < n && extra; i++) {
        char *esong = url_encode(song_ids[i]);
        char *next = wisp_aprintf("%s&songId=%s", extra, esong);
        free(esong);
        free(extra);
        extra = next;
    }
    if (!extra)
        return WISP_ERR_OOM;
    wisp_err e = simple_call(s, "createPlaylist.view", extra);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_delete_playlist(wisp_subsonic *s, const char *playlist_id) {
    char *id = url_encode(playlist_id);
    char *extra = wisp_aprintf("id=%s", id);
    wisp_err e = simple_call(s, "deletePlaylist.view", extra);
    free(id);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_rename_playlist(wisp_subsonic *s, const char *playlist_id,
                                       const char *name) {
    char *id = url_encode(playlist_id);
    char *ename = url_encode(name);
    char *extra = wisp_aprintf("playlistId=%s&name=%s", id, ename);
    wisp_err e = simple_call(s, "updatePlaylist.view", extra);
    free(id);
    free(ename);
    free(extra);
    return e;
}

wisp_err wisp_subsonic_get_lyrics(wisp_subsonic *s, const char *track_id, wisp_lyrics *out) {
    memset(out, 0, sizeof *out);
    char *id = url_encode(track_id);
    char *extra = wisp_aprintf("id=%s", id);
    free(id);
    cJSON *root, *response;
    wisp_err e = wisp_subsonic_get(s, "getLyricsBySongId.view", extra, &root, &response);
    free(extra);
    if (e != WISP_OK)
        return e;

    cJSON *list = cJSON_GetObjectItem(response, "lyricsList");
    cJSON *structured = list ? cJSON_GetObjectItem(list, "structuredLyrics") : NULL;
    cJSON *block = structured ? cJSON_GetArrayItem(structured, 0) : NULL;
    if (block) {
        cJSON *synced = cJSON_GetObjectItem(block, "synced");
        out->synced = synced && cJSON_IsBool(synced) && cJSON_IsTrue(synced);
        cJSON *lines = cJSON_GetObjectItem(block, "line");
        size_t n = lines ? (size_t)cJSON_GetArraySize(lines) : 0;
        if (n) {
            out->lines = calloc(n, sizeof *out->lines);
            cJSON *ln;
            size_t i = 0;
            cJSON_ArrayForEach(ln, lines) {
                if (i >= n)
                    break;
                cJSON *start = cJSON_GetObjectItem(ln, "start");
                cJSON *value = cJSON_GetObjectItem(ln, "value");
                out->lines[i].ms =
                    start && cJSON_IsNumber(start) ? (int64_t)start->valuedouble : -1;
                out->lines[i].text =
                    wisp_strdup(value && value->valuestring ? value->valuestring : "");
                i++;
            }
            out->count = i;
        }
    }
    cJSON_Delete(root);
    return WISP_OK;
}

void wisp_lyrics_free(wisp_lyrics *l) {
    for (size_t i = 0; i < l->count; i++)
        free(l->lines[i].text);
    free(l->lines);
    l->lines = NULL;
    l->count = 0;
    l->synced = false;
}
