#ifndef WISP_UI_SCREEN_H
#define WISP_UI_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    WISP_KEY_NONE = 0,
    WISP_KEY_UP = -1,
    WISP_KEY_DOWN = -2,
    WISP_KEY_LEFT = -3,
    WISP_KEY_RIGHT = -4,
    WISP_KEY_ENTER = -5,
    WISP_KEY_ESC = -6,
    WISP_KEY_TAB = -7,
    WISP_KEY_BACKSPACE = -8,
    WISP_KEY_HOME = -9,
    WISP_KEY_END = -10,
    WISP_KEY_PGUP = -11,
    WISP_KEY_PGDN = -12,
    WISP_KEY_DELETE = -13,
    WISP_KEY_RESIZE = -14,
};

enum {
    WISP_ATTR_BOLD = 1,
    WISP_ATTR_DIM = 2,
    WISP_ATTR_UNDERLINE = 4,
    WISP_ATTR_REVERSE = 8,
};

typedef struct {
    uint32_t ch;
    uint32_t fg;
    uint32_t bg;
    uint8_t attr;
} wisp_cell;

typedef struct wisp_screen wisp_screen;

wisp_screen *wisp_screen_open(void);
wisp_screen *wisp_screen_open_headless(int w, int h);
void wisp_screen_dump(wisp_screen *s, FILE *out);
void wisp_screen_close(wisp_screen *s);
bool wisp_screen_truecolor(wisp_screen *s);
void wisp_screen_size(wisp_screen *s, int *w, int *h);

void wisp_screen_begin(wisp_screen *s, uint32_t bg);
void wisp_screen_cell(wisp_screen *s, int x, int y, uint32_t ch, uint32_t fg, uint32_t bg,
                      uint8_t attr);
int wisp_screen_text(wisp_screen *s, int x, int y, int max_w, const char *utf8, uint32_t fg,
                     uint32_t bg, uint8_t attr);
void wisp_screen_fill(wisp_screen *s, int x, int y, int w, int h, uint32_t ch, uint32_t fg,
                      uint32_t bg, uint8_t attr);
void wisp_screen_flush(wisp_screen *s);

int wisp_screen_read_key(wisp_screen *s, int timeout_ms);

#endif
