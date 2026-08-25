#include "sync.h"

#include "common.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *js_str(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static int js_int(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (!v)
        return 0;
    if (cJSON_IsNumber(v))
        return v->valueint;
    if (cJSON_IsString(v))
        return atoi(v->valuestring);
    return 0;
}

static int64_t js_i64(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return v && cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

static bool js_present(cJSON *o, const char *k) { return cJSON_GetObjectItem(o, k) != NULL; }

static uint32_t resolve_artist(wisp_model *m, const char *ext_id, const char *name) {
    uint32_t id;
    if (ext_id && wisp_model_find_artist(m, ext_id, &id))
        return id;
    wisp_artist a = {.ext_id = (char *)(ext_id ? ext_id : name), .name = (char *)name,
                     .sort_name = (char *)name};
    return wisp_model_add_artist(m, &a);
}

static void sync_album_songs(wisp_subsonic *s, wisp_model *m, const char *album_ext,
                             uint32_t album_id, uint32_t artist_id) {
    char *extra = wisp_aprintf("id=%s", album_ext);
    cJSON *root, *resp;
    wisp_err e = wisp_subsonic_get(s, "getAlbum.view", extra, &root, &resp);
    free(extra);
    if (e != WISP_OK)
        return;
    cJSON *album = cJSON_GetObjectItem(resp, "album");
    cJSON *song;
    cJSON_ArrayForEach(song, cJSON_GetObjectItem(album, "song")) {
        wisp_track t = {0};
        t.ext_id = (char *)js_str(song, "id");
        t.title = (char *)js_str(song, "title");
        t.album_name = (char *)js_str(song, "album");
        t.artist_name = (char *)js_str(song, "artist");
        t.album_id = album_id;
        t.artist_id = artist_id;
        t.track_no = js_int(song, "track");
        t.disc_no = js_int(song, "discNumber");
        t.duration = js_int(song, "duration");
        t.year = js_int(song, "year");
        t.suffix = (char *)js_str(song, "suffix");
        t.bit_rate = js_int(song, "bitRate");
        t.size = js_i64(song, "size");
        t.cover_art = (char *)js_str(song, "coverArt");
        t.content_type = (char *)js_str(song, "contentType");
        t.starred = js_present(song, "starred");
        t.rating = js_int(song, "userRating");
        t.sampling_rate = js_int(song, "samplingRate");
        t.bit_depth = js_int(song, "bitDepth");
        t.channel_count = js_int(song, "channelCount");
        if (t.ext_id)
            wisp_model_add_track(m, &t);
    }
    cJSON_Delete(root);
}

static void add_entry_track(wisp_model *m, uint32_t plid, cJSON *entry) {
    const char *sid = js_str(entry, "id");
    if (!sid)
        return;
    uint32_t tid;
    if (!wisp_model_find_track(m, sid, &tid)) {
        wisp_track t = {0};
        t.ext_id = (char *)sid;
        t.title = (char *)js_str(entry, "title");
        t.album_name = (char *)js_str(entry, "album");
        t.artist_name = (char *)js_str(entry, "artist");
        t.album_id = UINT32_MAX;
        t.artist_id = UINT32_MAX;
        t.track_no = js_int(entry, "track");
        t.disc_no = js_int(entry, "discNumber");
        t.duration = js_int(entry, "duration");
        t.year = js_int(entry, "year");
        t.suffix = (char *)js_str(entry, "suffix");
        t.bit_rate = js_int(entry, "bitRate");
        t.size = js_i64(entry, "size");
        t.cover_art = (char *)js_str(entry, "coverArt");
        t.content_type = (char *)js_str(entry, "contentType");
        t.starred = js_present(entry, "starred");
        t.rating = js_int(entry, "userRating");
        t.sampling_rate = js_int(entry, "samplingRate");
        t.bit_depth = js_int(entry, "bitDepth");
        t.channel_count = js_int(entry, "channelCount");
        wisp_model_add_track(m, &t);
    }
    wisp_model_playlist_add_track(m, plid, sid);
}

static void sync_playlists(wisp_subsonic *s, wisp_model *m) {
    cJSON *root, *resp;
    if (wisp_subsonic_get(s, "getPlaylists.view", NULL, &root, &resp) != WISP_OK)
        return;
    cJSON *lists = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "playlists"), "playlist");
    cJSON *pl;
    cJSON_ArrayForEach(pl, lists) {
        const char *pid = js_str(pl, "id");
        const char *pname = js_str(pl, "name");
        if (!pid)
            continue;
        uint32_t plid = wisp_model_add_playlist(m, pid, pname ? pname : "playlist");
        char *extra = wisp_aprintf("id=%s", pid);
        cJSON *r2, *resp2;
        wisp_err e = wisp_subsonic_get(s, "getPlaylist.view", extra, &r2, &resp2);
        free(extra);
        if (e != WISP_OK)
            continue;
        cJSON *entry;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItem(cJSON_GetObjectItem(resp2, "playlist"),
                                                      "entry"))
            add_entry_track(m, plid, entry);
        cJSON_Delete(r2);
    }
    cJSON_Delete(root);
}

wisp_err wisp_subsonic_full_sync(wisp_subsonic *s, wisp_model *m, wisp_sync_progress cb,
                                 void *ctx) {
    wisp_model_clear(m);

    cJSON *root, *resp;
    wisp_err e = wisp_subsonic_get(s, "getArtists.view", NULL, &root, &resp);
    if (e != WISP_OK)
        return e;
    cJSON *index, *artist;
    cJSON_ArrayForEach(index, cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "artists"), "index")) {
        cJSON_ArrayForEach(artist, cJSON_GetObjectItem(index, "artist")) {
            wisp_artist a = {.ext_id = (char *)js_str(artist, "id"),
                             .name = (char *)js_str(artist, "name"),
                             .sort_name = (char *)js_str(artist, "sortName")};
            if (!a.sort_name)
                a.sort_name = a.name;
            if (a.ext_id)
                wisp_model_add_artist(m, &a);
        }
    }
    cJSON_Delete(root);
    if (cb)
        cb(ctx, "artists", (int)wisp_model_artist_count(m), (int)wisp_model_artist_count(m));

    int offset = 0, page = 500, total_albums = 0;
    for (;;) {
        char *extra = wisp_aprintf("type=alphabeticalByName&size=%d&offset=%d", page, offset);
        e = wisp_subsonic_get(s, "getAlbumList2.view", extra, &root, &resp);
        free(extra);
        if (e != WISP_OK)
            return e;
        cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "albumList2"), "album");
        int count = cJSON_GetArraySize(list);
        cJSON *album;
        cJSON_ArrayForEach(album, list) {
            const char *aid = js_str(album, "artistId");
            const char *aname = js_str(album, "artist");
            uint32_t artist_id = resolve_artist(m, aid, aname);
            wisp_album al = {.ext_id = (char *)js_str(album, "id"),
                             .name = (char *)js_str(album, "name"),
                             .sort_name = (char *)js_str(album, "sortName"),
                             .artist_name = (char *)aname,
                             .artist_id = artist_id,
                             .year = js_int(album, "year"),
                             .duration = js_int(album, "duration"),
                             .cover_art = (char *)js_str(album, "coverArt"),
                             .starred = js_present(album, "starred")};
            if (!al.sort_name)
                al.sort_name = al.name;
            if (!al.ext_id)
                continue;
            char *ext = wisp_strdup(al.ext_id);
            uint32_t album_id = wisp_model_add_album(m, &al);
            sync_album_songs(s, m, ext, album_id, artist_id);
            free(ext);
            total_albums++;
            if (cb && (total_albums % 10) == 0)
                cb(ctx, "albums", total_albums, 0);
        }
        cJSON_Delete(root);
        if (count < page)
            break;
        offset += page;
    }

    sync_playlists(s, m);

    if (cb)
        cb(ctx, "done", (int)wisp_model_track_count(m), (int)wisp_model_track_count(m));
    wisp_model_finalize(m);
    return WISP_OK;
}
