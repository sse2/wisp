#ifndef WISP_SYNC_H
#define WISP_SYNC_H

#include "common.h"
#include "model/model.h"
#include "subsonic/subsonic.h"

typedef void (*wisp_sync_progress)(void *ctx, const char *phase, int done, int total);

wisp_err wisp_subsonic_full_sync(wisp_subsonic *s, wisp_model *m, wisp_sync_progress cb, void *ctx);

#endif
