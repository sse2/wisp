#include "core/core.h"
#include "plat/plat.h"
#include "ui/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(wisp_state s) {
    switch (s) {
    case WISP_STATE_PLAYING:
        return "playing";
    case WISP_STATE_PAUSED:
        return "paused";
    case WISP_STATE_BUFFERING:
        return "buffering";
    default:
        return "stopped";
    }
}

static int play_queue(int count, char **paths) {
    wisp_core *c = wisp_core_new();
    if (!c) {
        fprintf(stderr, "wisp: could not start (no audio device?)\n");
        return 1;
    }
    wisp_core_queue_set(c, (const char **)paths, (size_t)count);
    wisp_core_play(c);
    bool started = false;
    for (;;) {
        wisp_event ev;
        while (wisp_core_poll_event(c, &ev))
            if (ev.type == WISP_EV_ERROR)
                fprintf(stderr, "\nwisp: playback error\n");
        wisp_status s = wisp_core_status(c);
        if (s.state == WISP_STATE_PLAYING || s.state == WISP_STATE_BUFFERING)
            started = true;
        printf("\r  [%zu/%zu] %-28.28s  %6.1f s  %-9s  vol %.2f    ", s.queue_pos + 1, s.queue_len,
               s.title ? s.title : "", s.position, state_name(s.state), (double)s.volume);
        fflush(stdout);
        bool done = started && s.state == WISP_STATE_STOPPED;
        wisp_status_free(&s);
        if (done)
            break;
        wisp_sleep_ms(150);
    }
    printf("\n");
    wisp_core_free(c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--dump")) {
        int w = 0, h = 0;
        if (sscanf(argv[2], "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
            return wisp_ui_dump(w, h, argc >= 4 ? argv[3] : NULL);
        fprintf(stderr, "usage: wisp --dump <w>x<h> [view]\n");
        return 1;
    }
    if (argc >= 2)
        return play_queue(argc - 1, &argv[1]);
    return wisp_ui_run();
}
