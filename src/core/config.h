#ifndef WISP_CONFIG_H
#define WISP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *name;
    char *url;
    char *username;
    char *password;
    bool trust_self_signed;
} wisp_server;

typedef struct {
    wisp_server *servers;
    size_t server_count;
    float volume;
    char *theme;
    char *device_id;
    float crossfade;
    bool reduced_motion;
    bool visualizer;
    int viz_type;
    int cache_max_mb;
    char *transcode_format;
} wisp_config;

void wisp_config_init(wisp_config *cfg);
void wisp_config_load(wisp_config *cfg);
bool wisp_config_save(const wisp_config *cfg);
void wisp_config_free(wisp_config *cfg);

wisp_server *wisp_config_add_server(wisp_config *cfg);

#endif
