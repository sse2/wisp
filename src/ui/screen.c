#include "screen.h"

#include "tty.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct wisp_screen {
    wisp_tty *tty;
    wisp_cell *back;
    wisp_cell *front;
    int w, h;
    bool truecolor;
    bool need_full;
};

static void alloc_buffers(wisp_screen *s, int w, int h) {
    free(s->back);
    free(s->front);
    s->w = w > 0 ? w : 1;
    s->h = h > 0 ? h : 1;
    s->back = calloc((size_t)s->w * s->h, sizeof(wisp_cell));
    s->front = calloc((size_t)s->w * s->h, sizeof(wisp_cell));
    s->need_full = true;
}

wisp_screen *wisp_screen_open(void) {
    wisp_screen *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->tty = wisp_tty_open(&s->truecolor);
    if (!s->tty) {
        free(s);
        return NULL;
    }
    int w, h;
    wisp_tty_size(s->tty, &w, &h);
    alloc_buffers(s, w, h);
    return s;
}

wisp_screen *wisp_screen_open_headless(int w, int h) {
    wisp_screen *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->truecolor = true;
    alloc_buffers(s, w, h);
    return s;
}

static void put_cp(char *b, size_t *n, uint32_t cp) {
    if (cp < 0x80)
        b[(*n)++] = (char)cp;
    else if (cp < 0x800) {
        b[(*n)++] = (char)(0xC0 | (cp >> 6));
        b[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[(*n)++] = (char)(0xE0 | (cp >> 12));
        b[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

void wisp_screen_dump(wisp_screen *s, FILE *out) {
    char *line = malloc((size_t)s->w * 4 + 1);
    for (int y = 0; y < s->h; y++) {
        size_t n = 0;
        for (int x = 0; x < s->w; x++) {
            uint32_t cp = s->back[y * s->w + x].ch;
            put_cp(line, &n, cp ? cp : ' ');
        }
        line[n] = '\0';
        fprintf(out, "%s\n", line);
    }
    free(line);
}

void wisp_screen_close(wisp_screen *s) {
    if (!s)
        return;
    if (s->tty)
        wisp_tty_close(s->tty);
    free(s->back);
    free(s->front);
    free(s);
}

bool wisp_screen_truecolor(wisp_screen *s) { return s->truecolor; }
void wisp_screen_size(wisp_screen *s, int *w, int *h) {
    *w = s->w;
    *h = s->h;
}

void wisp_screen_begin(wisp_screen *s, uint32_t bg) {
    if (s->tty) {
        int w, h;
        wisp_tty_size(s->tty, &w, &h);
        if (w != s->w || h != s->h)
            alloc_buffers(s, w, h);
    }
    wisp_cell blank = {.ch = ' ', .fg = 0xc0c0c0, .bg = bg, .attr = 0};
    for (int i = 0; i < s->w * s->h; i++)
        s->back[i] = blank;
}

void wisp_screen_cell(wisp_screen *s, int x, int y, uint32_t ch, uint32_t fg, uint32_t bg,
                      uint8_t attr) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    s->back[y * s->w + x] = (wisp_cell){.ch = ch, .fg = fg, .bg = bg, .attr = attr};
}

static uint32_t utf8_next(const char *s, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[*i];
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    uint32_t cp = n == 1 ? c : (uint32_t)(c & (0xFF >> (n + 1)));
    for (int j = 1; j < n; j++) {
        if ((u[*i + j] & 0xC0) != 0x80) {
            n = 1;
            cp = c;
            break;
        }
        cp = (cp << 6) | (u[*i + j] & 0x3F);
    }
    *i += n;
    return cp;
}

int wisp_screen_text(wisp_screen *s, int x, int y, int max_w, const char *utf8, uint32_t fg,
                     uint32_t bg, uint8_t attr) {
    if (!utf8 || y < 0 || y >= s->h)
        return 0;
    int col = 0;
    size_t i = 0;
    while (utf8[i] && col < max_w) {
        uint32_t cp = utf8_next(utf8, &i);
        if (cp == '\t')
            cp = ' ';
        wisp_screen_cell(s, x + col, y, cp, fg, bg, attr);
        col++;
    }
    return col;
}

void wisp_screen_fill(wisp_screen *s, int x, int y, int w, int h, uint32_t ch, uint32_t fg,
                      uint32_t bg, uint8_t attr) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            wisp_screen_cell(s, xx, yy, ch, fg, bg, attr);
}

void wisp_screen_flush(wisp_screen *s) {
    if (!s->tty)
        return;
    wisp_tty_present(s->tty, s->back, s->front, s->w, s->h, s->need_full);
    s->need_full = false;
}

int wisp_screen_read_key(wisp_screen *s, int timeout_ms) {
    if (!s->tty)
        return WISP_KEY_NONE;
    int k = wisp_tty_read_key(s->tty, timeout_ms);
    if (k == WISP_KEY_RESIZE)
        s->need_full = true;
    return k;
}
