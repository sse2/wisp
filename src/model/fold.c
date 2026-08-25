#include "fold.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t utf8_next(const unsigned char *s, size_t *i) {
    unsigned char c = s[*i];
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    uint32_t cp = n == 1 ? c : (uint32_t)(c & (0xFF >> (n + 1)));
    for (int j = 1; j < n; j++) {
        unsigned char cc = s[*i + j];
        if ((cc & 0xC0) != 0x80) {
            n = 1;
            cp = c;
            break;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += n;
    return cp;
}

static char fold_cp(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') ? (char)(cp + 32) : (char)cp;
    switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6:
    case 0x100: case 0x101: case 0x102: case 0x103: case 0x104: case 0x105:
        return 'a';
    case 0xC7: case 0xE7: case 0x106: case 0x107: case 0x10C: case 0x10D:
        return 'c';
    case 0xD0: case 0xF0: case 0x110: case 0x111: case 0x10E: case 0x10F:
        return 'd';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0x112: case 0x113: case 0x118: case 0x119: case 0x11A: case 0x11B:
        return 'e';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0x128: case 0x129: case 0x12A: case 0x12B: case 0x130: case 0x131:
        return 'i';
    case 0x141: case 0x142:
        return 'l';
    case 0xD1: case 0xF1: case 0x143: case 0x144: case 0x147: case 0x148:
        return 'n';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
    case 0x14C: case 0x14D: case 0x150: case 0x151:
        return 'o';
    case 0x158: case 0x159:
        return 'r';
    case 0xDF: case 0x15A: case 0x15B: case 0x160: case 0x161: case 0x15E: case 0x15F:
        return 's';
    case 0xDE: case 0xFE: case 0x162: case 0x163: case 0x164: case 0x165:
        return 't';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xF9: case 0xFA: case 0xFB: case 0xFC:
    case 0x168: case 0x169: case 0x16A: case 0x16B: case 0x16E: case 0x16F:
        return 'u';
    case 0xDD: case 0xFD: case 0xFF: case 0x176: case 0x177: case 0x178:
        return 'y';
    case 0x179: case 0x17A: case 0x17B: case 0x17C: case 0x17D: case 0x17E:
        return 'z';
    default:
        return 0;
    }
}

char *wisp_fold(const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    const unsigned char *in = (const unsigned char *)s;
    size_t i = 0, o = 0;
    while (in[i]) {
        size_t start = i;
        uint32_t cp = utf8_next(in, &i);
        char f = fold_cp(cp);
        if (f)
            out[o++] = f;
        else
            for (size_t k = start; k < i; k++)
                out[o++] = (char)in[k];
    }
    out[o] = '\0';
    return out;
}
