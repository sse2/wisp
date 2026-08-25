#ifndef WISP_UI_TTY_H
#define WISP_UI_TTY_H

#include "screen.h"

typedef struct wisp_tty wisp_tty;

wisp_tty *wisp_tty_open(bool *truecolor);
void wisp_tty_close(wisp_tty *t);
void wisp_tty_size(wisp_tty *t, int *w, int *h);
void wisp_tty_present(wisp_tty *t, const wisp_cell *back, wisp_cell *front, int w, int h,
                      bool full);
int wisp_tty_read_key(wisp_tty *t, int timeout_ms);

#endif
