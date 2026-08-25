#ifndef _WIN32

#include "tty.h"

#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct wisp_tty {
    struct termios orig;
    char *out;
    size_t out_len, out_cap;
};

static void ob(struct wisp_tty *t, const char *bytes, size_t n) {
    if (t->out_len + n > t->out_cap) {
        while (t->out_len + n > t->out_cap)
            t->out_cap = t->out_cap ? t->out_cap * 2 : 65536;
        t->out = realloc(t->out, t->out_cap);
    }
    memcpy(t->out + t->out_len, bytes, n);
    t->out_len += n;
}
static void obs(struct wisp_tty *t, const char *s) { ob(t, s, strlen(s)); }
static void obf(struct wisp_tty *t, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        ob(t, tmp, (size_t)n);
}
static void ob_cp(struct wisp_tty *t, uint32_t cp) {
    char b[4];
    if (cp < 0x80) {
        b[0] = (char)cp;
        ob(t, b, 1);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        ob(t, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        ob(t, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        ob(t, b, 4);
    }
}

wisp_tty *wisp_tty_open(bool *truecolor) {
    struct wisp_tty *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    tcgetattr(STDIN_FILENO, &t->orig);
    struct termios raw = t->orig;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    const char *ct = getenv("COLORTERM");
    *truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
    if (!*truecolor)
        *truecolor = true;

    const char *init = "\x1b[?1049h\x1b[?25l";
    if (write(STDOUT_FILENO, init, strlen(init)) < 0)
        (void)0;
    return t;
}

void wisp_tty_close(wisp_tty *t) {
    if (!t)
        return;
    const char *fin = "\x1b[0m\x1b[?25h\x1b[?1049l";
    if (write(STDOUT_FILENO, fin, strlen(fin)) < 0)
        (void)0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->orig);
    free(t->out);
    free(t);
}

void wisp_tty_size(wisp_tty *t, int *w, int *h) {
    (void)t;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

void wisp_tty_present(wisp_tty *t, const wisp_cell *back, wisp_cell *front, int w, int h,
                      bool full) {
    t->out_len = 0;
    int64_t cfg = -1, cbg = -1, cattr = -1;
    for (int y = 0; y < h; y++) {
        int run = -1;
        for (int x = 0; x < w; x++) {
            const wisp_cell *b = &back[y * w + x];
            wisp_cell *f = &front[y * w + x];
            if (!full && b->ch == f->ch && b->fg == f->fg && b->bg == f->bg && b->attr == f->attr)
                continue;
            if (run != x)
                obf(t, "\x1b[%d;%dH", y + 1, x + 1);
            if ((int64_t)b->fg != cfg || (int64_t)b->bg != cbg || (int64_t)b->attr != cattr) {
                obs(t, "\x1b[0");
                if (b->attr & WISP_ATTR_BOLD)
                    obs(t, ";1");
                if (b->attr & WISP_ATTR_DIM)
                    obs(t, ";2");
                if (b->attr & WISP_ATTR_UNDERLINE)
                    obs(t, ";4");
                if (b->attr & WISP_ATTR_REVERSE)
                    obs(t, ";7");
                obf(t, ";38;2;%u;%u;%u", (b->fg >> 16) & 0xFF, (b->fg >> 8) & 0xFF, b->fg & 0xFF);
                obf(t, ";48;2;%u;%u;%u", (b->bg >> 16) & 0xFF, (b->bg >> 8) & 0xFF, b->bg & 0xFF);
                obs(t, "m");
                cfg = b->fg;
                cbg = b->bg;
                cattr = b->attr;
            }
            ob_cp(t, b->ch ? b->ch : ' ');
            *f = *b;
            run = x + 1;
        }
    }
    obs(t, "\x1b[0m");
    if (t->out_len && write(STDOUT_FILENO, t->out, t->out_len) < 0)
        (void)0;
}

static int read_byte(int timeout_ms) {
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return -1;
    return c;
}

int wisp_tty_read_key(wisp_tty *t, int timeout_ms) {
    (void)t;
    int b = read_byte(timeout_ms);
    if (b < 0)
        return WISP_KEY_NONE;
    if (b == 27) {
        int b1 = read_byte(1);
        if (b1 < 0)
            return WISP_KEY_ESC;
        if (b1 == '[' || b1 == 'O') {
            int b2 = read_byte(1);
            switch (b2) {
            case 'A':
                return WISP_KEY_UP;
            case 'B':
                return WISP_KEY_DOWN;
            case 'C':
                return WISP_KEY_RIGHT;
            case 'D':
                return WISP_KEY_LEFT;
            case 'H':
                return WISP_KEY_HOME;
            case 'F':
                return WISP_KEY_END;
            case '3':
                read_byte(1);
                return WISP_KEY_DELETE;
            case '5':
                read_byte(1);
                return WISP_KEY_PGUP;
            case '6':
                read_byte(1);
                return WISP_KEY_PGDN;
            }
        }
        return WISP_KEY_ESC;
    }
    if (b == 13 || b == 10)
        return WISP_KEY_ENTER;
    if (b == 9)
        return WISP_KEY_TAB;
    if (b == 127 || b == 8)
        return WISP_KEY_BACKSPACE;
    if (b < 32)
        return b;
    int n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : 4;
    uint32_t cp = n == 1 ? (uint32_t)b : (uint32_t)(b & (0xFF >> (n + 1)));
    for (int i = 1; i < n; i++) {
        int cc = read_byte(1);
        if (cc < 0)
            break;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return (int)cp;
}

#endif
