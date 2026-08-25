#include "decoders.h"

#include <stdio.h>
#include <string.h>

extern ma_decoding_backend_vtable *ma_decoding_backend_libvorbis;
extern ma_decoding_backend_vtable *ma_decoding_backend_libopus;

static ma_result vfs_open(ma_vfs *vfs, const char *path, ma_uint32 mode, ma_vfs_file *file) {
    (void)path;
    (void)mode;
    wisp_source *s = ((wisp_source_vfs *)vfs)->src;
    *file = (ma_vfs_file)s;
    return s ? MA_SUCCESS : MA_ERROR;
}

static ma_result vfs_close(ma_vfs *vfs, ma_vfs_file file) {
    (void)vfs;
    (void)file;
    return MA_SUCCESS;
}

static ma_result vfs_read(ma_vfs *vfs, ma_vfs_file file, void *dst, size_t n, size_t *read) {
    (void)vfs;
    wisp_source *s = (wisp_source *)file;
    size_t got = s->read(s, dst, n);
    if (read)
        *read = got;
    return got == 0 ? MA_AT_END : MA_SUCCESS;
}

static ma_result vfs_seek(ma_vfs *vfs, ma_vfs_file file, ma_int64 off, ma_seek_origin origin) {
    (void)vfs;
    wisp_source *s = (wisp_source *)file;
    int whence = origin == ma_seek_origin_start ? SEEK_SET
                 : origin == ma_seek_origin_end ? SEEK_END
                                                : SEEK_CUR;
    return s->seek(s, off, whence) ? MA_SUCCESS : MA_ERROR;
}

static ma_result vfs_tell(ma_vfs *vfs, ma_vfs_file file, ma_int64 *cursor) {
    (void)vfs;
    wisp_source *s = (wisp_source *)file;
    int64_t c = s->tell(s);
    if (c < 0)
        return MA_ERROR;
    *cursor = c;
    return MA_SUCCESS;
}

static ma_result vfs_info(ma_vfs *vfs, ma_vfs_file file, ma_file_info *info) {
    (void)vfs;
    wisp_source *s = (wisp_source *)file;
    int64_t size = s->size(s);
    info->sizeInBytes = size < 0 ? 0 : (ma_uint64)size;
    return MA_SUCCESS;
}

bool wisp_decoder_open(wisp_decoder *d, wisp_source *s, uint32_t rate, uint32_t channels) {
    memset(d, 0, sizeof *d);
    d->src = s;
    d->vfs.src = s;
    d->vfs.cb.onOpen = vfs_open;
    d->vfs.cb.onClose = vfs_close;
    d->vfs.cb.onRead = vfs_read;
    d->vfs.cb.onSeek = vfs_seek;
    d->vfs.cb.onTell = vfs_tell;
    d->vfs.cb.onInfo = vfs_info;

    ma_decoding_backend_vtable *backends[] = {
        ma_decoding_backend_libvorbis,
        ma_decoding_backend_libopus,
    };
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, channels, rate);
    cfg.ppCustomBackendVTables = backends;
    cfg.customBackendCount = 2;
    cfg.pCustomBackendUserData = NULL;

    if (ma_decoder_init_vfs((ma_vfs *)&d->vfs, "wisp", &cfg, &d->ma) != MA_SUCCESS)
        return false;
    d->open = true;
    return true;
}

void wisp_decoder_close(wisp_decoder *d) {
    if (d->open) {
        ma_decoder_uninit(&d->ma);
        d->open = false;
    }
    if (d->src) {
        d->src->close(d->src);
        d->src = NULL;
    }
}

size_t wisp_decoder_read(wisp_decoder *d, float *out, size_t frames, bool *eof) {
    ma_uint64 got = 0;
    ma_result r = ma_decoder_read_pcm_frames(&d->ma, out, frames, &got);
    if (eof)
        *eof = r == MA_AT_END || got == 0;
    return (size_t)got;
}

bool wisp_decoder_seek(wisp_decoder *d, uint64_t frame) {
    return ma_decoder_seek_to_pcm_frame(&d->ma, frame) == MA_SUCCESS;
}

uint64_t wisp_decoder_length(wisp_decoder *d) {
    ma_uint64 len = 0;
    if (!d->open || ma_decoder_get_length_in_pcm_frames(&d->ma, &len) != MA_SUCCESS)
        return 0;
    return len;
}

uint64_t wisp_decoder_cursor(wisp_decoder *d) {
    ma_uint64 cur = 0;
    if (!d->open || ma_decoder_get_cursor_in_pcm_frames(&d->ma, &cur) != MA_SUCCESS)
        return 0;
    return cur;
}
