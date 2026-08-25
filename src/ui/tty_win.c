#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "tty.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wisp_tty {
    HANDLE hin, hout, hout_orig;
    DWORD in_mode, out_mode;
    bool vt;
    bool alt_created;
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

static void write_console(HANDLE h, const char *s) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wn <= 1)
        return;
    wchar_t *w = malloc((size_t)wn * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
    DWORD done;
    WriteConsoleW(h, w, (DWORD)(wn - 1), &done, NULL);
    free(w);
}

wisp_tty *wisp_tty_open(bool *truecolor) {
    struct wisp_tty *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->hin = GetStdHandle(STD_INPUT_HANDLE);
    t->hout_orig = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(t->hin, &t->in_mode);
    GetConsoleMode(t->hout_orig, &t->out_mode);
    SetConsoleMode(t->hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);

    DWORD om = t->out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    t->vt = SetConsoleMode(t->hout_orig, om) != 0;
    if (t->vt) {
        t->hout = t->hout_orig;
        write_console(t->hout, "\x1b[?1049h\x1b[?25l");
    } else {
        t->hout = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                            CONSOLE_TEXTMODE_BUFFER, NULL);
        SetConsoleActiveScreenBuffer(t->hout);
        t->alt_created = true;
        CONSOLE_CURSOR_INFO ci;
        GetConsoleCursorInfo(t->hout, &ci);
        ci.bVisible = FALSE;
        SetConsoleCursorInfo(t->hout, &ci);
    }
    *truecolor = t->vt;
    return t;
}

void wisp_tty_close(wisp_tty *t) {
    if (!t)
        return;
    if (t->vt) {
        write_console(t->hout, "\x1b[0m\x1b[?25h\x1b[?1049l");
        SetConsoleMode(t->hout_orig, t->out_mode);
    } else {
        SetConsoleActiveScreenBuffer(t->hout_orig);
        if (t->alt_created)
            CloseHandle(t->hout);
    }
    SetConsoleMode(t->hin, t->in_mode);
    free(t->out);
    free(t);
}

void wisp_tty_size(wisp_tty *t, int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(t->hout, &bi)) {
        *w = bi.srWindow.Right - bi.srWindow.Left + 1;
        *h = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    } else {
        *w = 80;
        *h = 24;
    }
}

static const uint32_t PAL16[16] = {0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080,
                                   0x808000, 0xc0c0c0, 0x808080, 0x0000ff, 0x00ff00, 0x00ffff,
                                   0xff0000, 0xff00ff, 0xffff00, 0xffffff};

static int nearest16(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF, best = 0;
    long bestd = 1L << 30;
    for (int i = 0; i < 16; i++) {
        int pr = (PAL16[i] >> 16) & 0xFF, pg = (PAL16[i] >> 8) & 0xFF, pb = PAL16[i] & 0xFF;
        long d = (long)(r - pr) * (r - pr) + (long)(g - pg) * (g - pg) + (long)(b - pb) * (b - pb);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void present_console(wisp_tty *t, const wisp_cell *back, int w, int h) {
    CHAR_INFO *buf = malloc((size_t)w * h * sizeof(CHAR_INFO));
    for (int i = 0; i < w * h; i++) {
        const wisp_cell *c = &back[i];
        int fg = nearest16(c->fg), bg = nearest16(c->bg);
        if (c->attr & WISP_ATTR_REVERSE) {
            int tmp = fg;
            fg = bg;
            bg = tmp;
        }
        if (c->attr & WISP_ATTR_BOLD)
            fg |= 8;
        buf[i].Char.UnicodeChar = c->ch && c->ch <= 0xFFFF ? (WCHAR)c->ch : L' ';
        buf[i].Attributes = (WORD)(fg | (bg << 4));
    }
    COORD size = {(SHORT)w, (SHORT)h}, org = {0, 0};
    SMALL_RECT region = {0, 0, (SHORT)(w - 1), (SHORT)(h - 1)};
    WriteConsoleOutputW(t->hout, buf, size, org, &region);
    free(buf);
}

void wisp_tty_present(wisp_tty *t, const wisp_cell *back, wisp_cell *front, int w, int h,
                      bool full) {
    if (!t->vt) {
        present_console(t, back, w, h);
        memcpy(front, back, (size_t)w * h * sizeof(wisp_cell));
        return;
    }
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
    if (t->out_len) {
        ob(t, "\0", 1);
        write_console(t->hout, t->out);
    }
}

int wisp_tty_read_key(wisp_tty *t, int timeout_ms) {
    DWORD r = WaitForSingleObject(t->hin, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (r != WAIT_OBJECT_0)
        return WISP_KEY_NONE;
    INPUT_RECORD rec;
    DWORD n;
    if (!ReadConsoleInputW(t->hin, &rec, 1, &n) || n == 0)
        return WISP_KEY_NONE;
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT)
        return WISP_KEY_RESIZE;
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
        return WISP_KEY_NONE;
    WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
    wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
    switch (vk) {
    case VK_UP:
        return WISP_KEY_UP;
    case VK_DOWN:
        return WISP_KEY_DOWN;
    case VK_LEFT:
        return WISP_KEY_LEFT;
    case VK_RIGHT:
        return WISP_KEY_RIGHT;
    case VK_RETURN:
        return WISP_KEY_ENTER;
    case VK_ESCAPE:
        return WISP_KEY_ESC;
    case VK_TAB:
        return WISP_KEY_TAB;
    case VK_BACK:
        return WISP_KEY_BACKSPACE;
    case VK_HOME:
        return WISP_KEY_HOME;
    case VK_END:
        return WISP_KEY_END;
    case VK_PRIOR:
        return WISP_KEY_PGUP;
    case VK_NEXT:
        return WISP_KEY_PGDN;
    case VK_DELETE:
        return WISP_KEY_DELETE;
    }
    if (ch >= 32)
        return (int)ch;
    if (ch == 13)
        return WISP_KEY_ENTER;
    if (ch == 9)
        return WISP_KEY_TAB;
    if (ch == 27)
        return WISP_KEY_ESC;
    if (ch == 8 || ch == 127)
        return WISP_KEY_BACKSPACE;
    if (ch > 0)
        return (int)ch;
    return WISP_KEY_NONE;
}

#endif
