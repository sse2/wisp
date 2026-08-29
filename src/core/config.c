#include "config.h"

#include "common.h"
#include "plat/plat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *config_file(void) {
    char *dir = wisp_dir_path(WISP_DIR_CONFIG);
    if (!dir)
        return NULL;
    char *path = wisp_path_join(dir, "config");
    free(dir);
    return path;
}

static void set_str(char **slot, const char *value) {
    free(*slot);
    *slot = wisp_strdup(value);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

void wisp_config_init(wisp_config *cfg) {
    cfg->servers = NULL;
    cfg->server_count = 0;
    cfg->volume = 1.0f;
    cfg->theme = NULL;
    cfg->device_id = NULL;
    cfg->crossfade = 0.0f;
    cfg->reduced_motion = false;
    cfg->visualizer = true;
    cfg->viz_type = 0;
    cfg->cache_max_mb = 0;
    cfg->transcode_format = NULL;
    cfg->repeat = 0;
    cfg->shuffle = false;
}

wisp_server *wisp_config_add_server(wisp_config *cfg) {
    wisp_server *grown = realloc(cfg->servers, (cfg->server_count + 1) * sizeof *grown);
    if (!grown)
        return NULL;
    cfg->servers = grown;
    wisp_server *s = &cfg->servers[cfg->server_count++];
    memset(s, 0, sizeof *s);
    return s;
}

static void apply(wisp_config *cfg, wisp_server *server, const char *key, const char *value) {
    if (server) {
        if (!strcmp(key, "name"))
            set_str(&server->name, value);
        else if (!strcmp(key, "url"))
            set_str(&server->url, value);
        else if (!strcmp(key, "username"))
            set_str(&server->username, value);
        else if (!strcmp(key, "password"))
            set_str(&server->password, value);
        else if (!strcmp(key, "trust_self_signed"))
            server->trust_self_signed = atoi(value) != 0;
        return;
    }
    if (!strcmp(key, "volume"))
        cfg->volume = (float)atof(value);
    else if (!strcmp(key, "theme"))
        set_str(&cfg->theme, value);
    else if (!strcmp(key, "device_id"))
        set_str(&cfg->device_id, value);
    else if (!strcmp(key, "crossfade"))
        cfg->crossfade = (float)atof(value);
    else if (!strcmp(key, "reduced_motion"))
        cfg->reduced_motion = atoi(value) != 0;
    else if (!strcmp(key, "visualizer"))
        cfg->visualizer = atoi(value) != 0;
    else if (!strcmp(key, "viz_type"))
        cfg->viz_type = atoi(value);
    else if (!strcmp(key, "cache_max_mb"))
        cfg->cache_max_mb = atoi(value);
    else if (!strcmp(key, "transcode_format"))
        set_str(&cfg->transcode_format, value);
    else if (!strcmp(key, "repeat"))
        cfg->repeat = atoi(value);
    else if (!strcmp(key, "shuffle"))
        cfg->shuffle = atoi(value) != 0;
}

void wisp_config_load(wisp_config *cfg) {
    wisp_config_init(cfg);
    char *path = config_file();
    if (!path)
        return;
    void *data = NULL;
    bool got = wisp_file_read(path, &data, NULL);
    free(path);
    if (!got)
        return;

    wisp_server *server = NULL;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        if (!strcmp(s, "[server]")) {
            server = wisp_config_add_server(cfg);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        apply(cfg, server, trim(s), trim(eq + 1));
    }
    free(data);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

static void sb_add(strbuf *sb, const char *text) {
    if (!sb->data)
        return;
    size_t n = strlen(text);
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap)
            sb->cap = sb->cap ? sb->cap * 2 : 256;
        char *grown = realloc(sb->data, sb->cap);
        if (!grown) {
            free(sb->data);
            sb->data = NULL;
            return;
        }
        sb->data = grown;
    }
    memcpy(sb->data + sb->len, text, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_addf(strbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *line = wisp_avprintf(fmt, ap);
    va_end(ap);
    if (line) {
        sb_add(sb, line);
        free(line);
    }
}

bool wisp_config_save(const wisp_config *cfg) {
    char *path = config_file();
    if (!path)
        return false;

    strbuf sb = {.data = malloc(256), .len = 0, .cap = 256};
    sb_addf(&sb, "volume = %.3f\n", cfg->volume);
    sb_addf(&sb, "crossfade = %.2f\n", cfg->crossfade);
    sb_addf(&sb, "reduced_motion = %d\n", cfg->reduced_motion ? 1 : 0);
    sb_addf(&sb, "visualizer = %d\n", cfg->visualizer ? 1 : 0);
    sb_addf(&sb, "viz_type = %d\n", cfg->viz_type);
    sb_addf(&sb, "cache_max_mb = %d\n", cfg->cache_max_mb);
    sb_addf(&sb, "repeat = %d\n", cfg->repeat);
    sb_addf(&sb, "shuffle = %d\n", cfg->shuffle ? 1 : 0);
    if (cfg->theme)
        sb_addf(&sb, "theme = %s\n", cfg->theme);
    if (cfg->transcode_format)
        sb_addf(&sb, "transcode_format = %s\n", cfg->transcode_format);
    if (cfg->device_id)
        sb_addf(&sb, "device_id = %s\n", cfg->device_id);
    for (size_t i = 0; i < cfg->server_count; i++) {
        const wisp_server *sv = &cfg->servers[i];
        sb_add(&sb, "[server]\n");
        sb_addf(&sb, "name = %s\nurl = %s\nusername = %s\npassword = %s\ntrust_self_signed = %d\n",
                sv->name ? sv->name : "", sv->url ? sv->url : "", sv->username ? sv->username : "",
                sv->password ? sv->password : "", sv->trust_self_signed ? 1 : 0);
    }

    bool ok = sb.data && wisp_file_write(path, sb.data, sb.len, true);
    free(sb.data);
    free(path);
    return ok;
}

void wisp_config_free(wisp_config *cfg) {
    for (size_t i = 0; i < cfg->server_count; i++) {
        free(cfg->servers[i].name);
        free(cfg->servers[i].url);
        free(cfg->servers[i].username);
        free(cfg->servers[i].password);
    }
    free(cfg->servers);
    free(cfg->theme);
    free(cfg->device_id);
    free(cfg->transcode_format);
    wisp_config_init(cfg);
}
