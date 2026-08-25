#ifndef WISP_AUDIO_SOURCE_H
#define WISP_AUDIO_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct wisp_source wisp_source;

struct wisp_source {
    size_t (*read)(wisp_source *s, void *buf, size_t n);
    bool (*seek)(wisp_source *s, int64_t off, int whence);
    int64_t (*tell)(wisp_source *s);
    int64_t (*size)(wisp_source *s);
    void (*close)(wisp_source *s);
    void *impl;
};

wisp_source *wisp_source_file(const char *path);

#endif
