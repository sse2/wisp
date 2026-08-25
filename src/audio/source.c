#define _FILE_OFFSET_BITS 64
#include "source.h"

#include "plat/plat.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define wisp_fseek64 _fseeki64
#define wisp_ftell64 _ftelli64
#else
#define wisp_fseek64 fseeko
#define wisp_ftell64 ftello
#endif

typedef struct {
    FILE *f;
    int64_t size;
} file_impl;

static size_t file_read(wisp_source *s, void *buf, size_t n) {
    file_impl *fi = s->impl;
    return fread(buf, 1, n, fi->f);
}

static bool file_seek(wisp_source *s, int64_t off, int whence) {
    file_impl *fi = s->impl;
    return wisp_fseek64(fi->f, off, whence) == 0;
}

static int64_t file_tell(wisp_source *s) {
    file_impl *fi = s->impl;
    return wisp_ftell64(fi->f);
}

static int64_t file_size(wisp_source *s) { return ((file_impl *)s->impl)->size; }

static void file_close(wisp_source *s) {
    file_impl *fi = s->impl;
    if (fi) {
        if (fi->f)
            fclose(fi->f);
        free(fi);
    }
    free(s);
}

wisp_source *wisp_source_file(const char *path) {
    FILE *f = wisp_fopen(path, "rb");
    if (!f)
        return NULL;
    wisp_source *s = calloc(1, sizeof *s);
    file_impl *fi = calloc(1, sizeof *fi);
    if (!s || !fi) {
        fclose(f);
        free(s);
        free(fi);
        return NULL;
    }
    fi->f = f;
    wisp_fseek64(f, 0, SEEK_END);
    fi->size = wisp_ftell64(f);
    wisp_fseek64(f, 0, SEEK_SET);
    s->impl = fi;
    s->read = file_read;
    s->seek = file_seek;
    s->tell = file_tell;
    s->size = file_size;
    s->close = file_close;
    return s;
}
