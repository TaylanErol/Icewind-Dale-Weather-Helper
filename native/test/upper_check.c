/* upper_check.c - prints alm_normalize_seed("x" + U+cp + "x") for every scalar value, one
 * line per code point (UTF-8 hex), so test_upper.sh can diff it against node's
 * "x...x".trim().toUpperCase(). */
#include "../src/store.h"
#include <stdio.h>
#include <string.h>

static size_t put8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63));
    o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}

int main(void)
{
    static char line[64];
    for (uint32_t c = 1; c <= 0x10FFFF; c++) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        char in[8], out[32];
        size_t n = 0;
        in[n++] = 'x';
        n += put8(c, in + n);
        in[n++] = 'x';
        in[n] = 0;
        size_t k = alm_normalize_seed(in, out, sizeof out);
        size_t p = 0;
        for (size_t i = 0; i < k; i++) p += (size_t)snprintf(line + p, sizeof line - p, "%02x", (unsigned char)out[i]);
        line[p] = 0;
        puts(line);
    }
    return 0;
}
