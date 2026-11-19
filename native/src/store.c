/* store.c - Almanac state, persistence, backup import/export and app actions (pure C11).
 * See store.h for the contract and SPEC.md for the behaviour. Only engine.h is used from
 * the engine; Win32 is touched only inside the alm_fs_* shim under #ifdef _WIN32.
 *
 * Internal string encoding (all Almanac strings): UTF-8, extended so that every JS string
 * survives a load/save round trip byte for byte:
 *   - U+0000 is stored as the overlong pair C0 80 (no raw NUL inside a C string);
 *   - a lone UTF-16 surrogate (from a "\udXXX" escape) is stored as its 3-byte WTF-8 form.
 * Raw invalid UTF-8 in a file becomes U+FFFD, as the web's UTF-8 decoder does.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* fileno, fsync */
#endif
#include "store.h"
#include "upper_table.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define JSON_MAX_DEPTH 256

/* ================================================================================
 * Buffers
 * ============================================================================== */

void alm_buf_free(AlmBuf *b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool buf_reserve(AlmBuf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1) return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { nc = need; break; }
        nc *= 2;
    }
    char *p = (char *)realloc(b->data, nc);
    if (!p) return false;
    b->data = p;
    b->cap = nc;
    return true;
}

static bool buf_add(AlmBuf *b, const void *s, size_t n)
{
    if (!buf_reserve(b, n)) return false;
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}

/* Bounded writer into a caller buffer: truncates on a UTF-8 boundary, always NUL terminated. */
typedef struct SB { char *p; size_t cap, len; bool full; } SB;

static void sb_init(SB *s, char *buf, size_t cap)
{
    s->p = buf; s->cap = cap; s->len = 0; s->full = (cap == 0);
    if (cap) buf[0] = 0;
}
static void sb_addn(SB *s, const char *t, size_t n)
{
    if (s->full || !t) return;
    size_t room = s->cap - 1 - s->len;
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
        s->full = true;
    }
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = 0;
}
static void sb_adds(SB *s, const char *t) { if (t) sb_addn(s, t, strlen(t)); }
static void sb_addf(SB *s, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_addn(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
/* formatted text into a fixed field, truncated on a UTF-8 boundary */
static size_t fmt_into(char *buf, size_t cap, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    SB s;
    sb_init(&s, buf, cap);
    if (n > 0) sb_addn(&s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    return s.len;
}
/* user text inside toast / confirm markup: & < > become entities (the web's esc()) */
static void sb_add_esc(SB *s, const char *t)
{
    for (; t && *t; t++) {
        if (*t == '&') sb_adds(s, "&amp;");
        else if (*t == '<') sb_adds(s, "&lt;");
        else if (*t == '>') sb_adds(s, "&gt;");
        else sb_addn(s, t, 1);
    }
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

/* ================================================================================
 * UTF-8 helpers (internal encoding, see the file comment)
 * ============================================================================== */

/* Decodes one code point (lenient: C0 80 -> 0, WTF-8 surrogates allowed, invalid -> FFFD/1). */
static uint32_t u8_next(const unsigned char *s, size_t n, size_t *adv)
{
    unsigned c = s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if (c == 0xC0 && n >= 2 && s[1] == 0x80) { *adv = 2; return 0; }
    if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
        *adv = 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        if (cp >= 0x800) { *adv = 3; return cp; }
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        if (cp >= 0x10000 && cp <= 0x10FFFF) { *adv = 4; return cp; }
    }
    *adv = 1;
    return 0xFFFD;
}

static size_t u8_put(uint32_t cp, char *o)
{
    if (cp == 0) { o[0] = (char)0xC0; o[1] = (char)0x80; return 2; }
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* JS WhiteSpace + LineTerminator (String.prototype.trim / StringToNumber). */
static bool js_space(uint32_t c)
{
    switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

/* trim(): [*b, *e) of s[0..n) */
static void u8_trim(const char *s, size_t n, size_t *b, size_t *e)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, first = n, last = 0, adv;
    while (i < n) {
        uint32_t c = u8_next(u + i, n - i, &adv);
        if (!js_space(c)) {
            if (first == n) first = i;
            last = i + adv;
        }
        i += adv;
    }
    if (first == n) { *b = *e = 0; return; }
    *b = first; *e = last;
}

static bool u8_blank(const char *s, size_t n)
{
    size_t b, e;
    u8_trim(s, n, &b, &e);
    return b == e;
}

/* Byte length of the longest prefix of s[0..n) holding at most max_units UTF-16 units.
 * A supplementary character that would not fit whole is dropped. */
static size_t u8_cut_units(const char *s, size_t n, size_t max_units)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, units = 0, adv;
    while (i < n) {
        uint32_t c = u8_next(u + i, n - i, &adv);
        size_t w = c >= 0x10000 ? 2 : 1;
        if (units + w > max_units) break;
        units += w;
        i += adv;
    }
    return i;
}

/* Longest prefix of at most max_bytes that does not split a character. */
static size_t u8_cut_bytes(const char *s, size_t n, size_t max_bytes)
{
    if (n <= max_bytes) return n;
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

/* trim + cut to units, as a new heap string */
static char *dup_trim_cut(const char *s, size_t max_units)
{
    size_t n = strlen(s), b, e;
    u8_trim(s, n, &b, &e);
    size_t k = u8_cut_units(s + b, e - b, max_units);
    return xstrndup(s + b, k);
}

/* Full upper-case mapping of one code point, as String.prototype.toUpperCase() does it
 * (generated table, see test/gen_upper.js). Writes 1..3 code points to out, returns the count. */
static int to_upper_full(uint32_t c, uint32_t out[3])
{
    size_t lo = 0, hi = sizeof UP_SPECIAL / sizeof UP_SPECIAL[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (UP_SPECIAL[mid].cp < c) lo = mid + 1; else hi = mid;
    }
    if (lo < sizeof UP_SPECIAL / sizeof UP_SPECIAL[0] && UP_SPECIAL[lo].cp == c) {
        int n = 0;
        while (n < 3 && UP_SPECIAL[lo].up[n]) { out[n] = UP_SPECIAL[lo].up[n]; n++; }
        return n;
    }
    lo = 0; hi = sizeof UP_RUNS / sizeof UP_RUNS[0];
    while (lo < hi) {             /* last run with start <= c */
        size_t mid = (lo + hi) / 2;
        if (UP_RUNS[mid].start <= c) lo = mid + 1; else hi = mid;
    }
    out[0] = c;
    if (lo > 0) {
        const UpRun *r = &UP_RUNS[lo - 1];
        uint32_t off = c - r->start;
        if (off % r->stride == 0 && off / r->stride < r->count) out[0] = (uint32_t)((int32_t)c + r->delta);
    }
    return 1;
}

/* ================================================================================
 * JS number semantics
 * ============================================================================== */

static int32_t to_int32(double x)
{
    if (!isfinite(x)) return 0;
    double t = trunc(x);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    uint32_t u = (uint32_t)m;
    return u >= 0x80000000u ? (int32_t)((int64_t)u - 4294967296LL) : (int32_t)u;
}

static bool is_integral(double x) { return isfinite(x) && trunc(x) == x; }

/* strtod on a bounded, already validated slice */
static double parse_decimal(const char *s, size_t n)
{
    char stack[80];
    char *tmp = n < sizeof stack ? stack : (char *)malloc(n + 1);
    if (!tmp) return NAN;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    double v = strtod(tmp, NULL);
    if (tmp != stack) free(tmp);
    return v;
}

/* ECMAScript StringToNumber on an internal UTF-8 string. */
static double js_str_to_number(const char *s, size_t n)
{
    size_t b, e;
    u8_trim(s, n, &b, &e);
    s += b; n = e - b;
    if (n == 0) return 0;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'o' || s[1] == 'O' ||
                                 s[1] == 'b' || s[1] == 'B')) {
        int base = (s[1] == 'x' || s[1] == 'X') ? 16 : (s[1] == 'o' || s[1] == 'O') ? 8 : 2;
        double v = 0;
        for (size_t i = 2; i < n; i++) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return NAN;
            if (d >= base) return NAN;
            v = v * base + d;
        }
        return v;
    }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; i = 1; }
    if (n - i == 8 && memcmp(s + i, "Infinity", 8) == 0) return neg ? -INFINITY : INFINITY;
    size_t digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    }
    if (!digits) return NAN;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t ed = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { i++; ed++; }
        if (!ed) return NAN;
    }
    if (i != n) return NAN;
    return parse_decimal(s, n);
}

size_t alm_fmt_js_number(double x, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    if (!isfinite(x)) { sb_adds(&sb, "null"); return sb.len; }
    if (x == 0) { sb_adds(&sb, "0"); return sb.len; }
    char e[48];
    bool neg = x < 0;
    double ax = fabs(x);
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(e, sizeof e, "%.*e", prec - 1, ax);
        if (strtod(e, NULL) == ax) break;
    }
    if (prec > 17) snprintf(e, sizeof e, "%.16e", ax);
    char dig[24];
    int k = 0;
    const char *p = e;
    for (; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9' && k < 20) dig[k++] = *p;
    int ex = (*p == 'e') ? atoi(p + 1) : 0;
    while (k > 1 && dig[k - 1] == '0') k--;
    dig[k] = 0;
    int nn = ex + 1;
    char out[64];
    size_t o = 0;
    if (neg) out[o++] = '-';
    if (k <= nn && nn <= 21) {
        memcpy(out + o, dig, (size_t)k); o += (size_t)k;
        for (int i = k; i < nn; i++) out[o++] = '0';
    } else if (0 < nn && nn <= 21) {
        memcpy(out + o, dig, (size_t)nn); o += (size_t)nn;
        out[o++] = '.';
        memcpy(out + o, dig + nn, (size_t)(k - nn)); o += (size_t)(k - nn);
    } else if (-6 < nn && nn <= 0) {
        out[o++] = '0'; out[o++] = '.';
        for (int i = 0; i < -nn; i++) out[o++] = '0';
        memcpy(out + o, dig, (size_t)k); o += (size_t)k;
    } else {
        out[o++] = dig[0];
        if (k > 1) { out[o++] = '.'; memcpy(out + o, dig + 1, (size_t)(k - 1)); o += (size_t)(k - 1); }
        o += (size_t)snprintf(out + o, sizeof out - o, "e%c%d", nn - 1 >= 0 ? '+' : '-', abs(nn - 1));
    }
    sb_addn(&sb, out, o);
    return sb.len;
}

/* ================================================================================
 * JSON DOM + parser
 * ============================================================================== */

typedef enum JType { J_NULL = 0, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
typedef struct JMember JMember;

struct JVal {
    JType    t;
    double   num;
    char    *str;      /* J_STR: internal encoding, NUL terminated */
    size_t   slen;
    JVal    *items;    /* J_ARR */
    JMember *mem;      /* J_OBJ */
    size_t   n, cap;
};

struct JMember {
    char  *key;
    size_t klen;
    JVal   val;
};

static void jfree(JVal *v)
{
    if (!v) return;
    if (v->t == J_STR) free(v->str);
    else if (v->t == J_ARR) {
        for (size_t i = 0; i < v->n; i++) jfree(&v->items[i]);
        free(v->items);
    } else if (v->t == J_OBJ) {
        for (size_t i = 0; i < v->n; i++) { free(v->mem[i].key); jfree(&v->mem[i].val); }
        free(v->mem);
    }
    memset(v, 0, sizeof *v);
}

typedef struct JP {
    const unsigned char *s;
    size_t n, i;
    int depth;
} JP;

static void jp_ws(JP *p)
{
    while (p->i < p->n) {
        unsigned char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int hexv(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool jp_hex4(JP *p, uint32_t *out)
{
    if (p->n - p->i < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexv(p->s[p->i + (size_t)k]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

/* Strict UTF-8 check of raw text; returns the sequence length or 0 when invalid. Also
 * reports the length of the maximal valid prefix of an invalid sequence via *skip. */
static size_t raw_utf8_len(const unsigned char *s, size_t n, size_t *skip)
{
    unsigned c = s[0];
    *skip = 1;
    size_t need;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) { need = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
    else return 0;
    for (size_t k = 1; k < need; k++) {
        if (k >= n) { *skip = k; return 0; }
        unsigned b = s[k];
        unsigned l = (k == 1) ? lo : 0x80, h = (k == 1) ? hi : 0xBF;
        if (b < l || b > h) { *skip = k; return 0; }
    }
    return need;
}

static bool jp_string(JP *p, char **out, size_t *olen)
{
    /* p->s[p->i] == '"' */
    p->i++;
    AlmBuf b = {0};
    char enc[4];
    if (!buf_reserve(&b, 16)) return false;
    for (;;) {
        if (p->i >= p->n) goto fail;
        unsigned char c = p->s[p->i];
        if (c == '"') { p->i++; break; }
        if (c < 0x20) goto fail;
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) goto fail;
            unsigned char e = p->s[p->i++];
            uint32_t cp;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u':
                if (!jp_hex4(p, &cp)) goto fail;
                if (cp >= 0xD800 && cp <= 0xDBFF && p->n - p->i >= 6 && p->s[p->i] == '\\' &&
                    p->s[p->i + 1] == 'u') {
                    size_t save = p->i;
                    uint32_t lo;
                    p->i += 2;
                    if (jp_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        p->i = save;
                }
                break;
            default:
                goto fail;
            }
            if (!buf_add(&b, enc, u8_put(cp, enc))) goto fail;
            continue;
        }
        if (c < 0x80) {
            /* run of plain ASCII */
            size_t j = p->i;
            while (j < p->n && p->s[j] >= 0x20 && p->s[j] < 0x80 && p->s[j] != '"' && p->s[j] != '\\') j++;
            if (!buf_add(&b, p->s + p->i, j - p->i)) goto fail;
            p->i = j;
            continue;
        }
        size_t skip, len = raw_utf8_len(p->s + p->i, p->n - p->i, &skip);
        if (len) {
            if (!buf_add(&b, p->s + p->i, len)) goto fail;
            p->i += len;
        } else {
            if (!buf_add(&b, "\xEF\xBF\xBD", 3)) goto fail;
            p->i += skip;
        }
    }
    *out = b.data;
    *olen = b.len;
    return true;
fail:
    alm_buf_free(&b);
    return false;
}

static bool jp_value(JP *p, JVal *out);

static bool jp_number(JP *p, JVal *out)
{
    size_t st = p->i;
    const unsigned char *s = p->s;
    if (p->i < p->n && s[p->i] == '-') p->i++;
    if (p->i >= p->n) return false;
    if (s[p->i] == '0') p->i++;
    else if (s[p->i] >= '1' && s[p->i] <= '9') { while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9') p->i++; }
    else return false;
    if (p->i < p->n && s[p->i] == '.') {
        p->i++;
        size_t d = p->i;
        while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9') p->i++;
        if (p->i == d) return false;
    }
    if (p->i < p->n && (s[p->i] == 'e' || s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (s[p->i] == '+' || s[p->i] == '-')) p->i++;
        size_t d = p->i;
        while (p->i < p->n && s[p->i] >= '0' && s[p->i] <= '9') p->i++;
        if (p->i == d) return false;
    }
    out->t = J_NUM;
    out->num = parse_decimal((const char *)s + st, p->i - st);
    return !isnan(out->num);
}

static bool jp_lit(JP *p, const char *w, size_t n)
{
    if (p->n - p->i < n || memcmp(p->s + p->i, w, n) != 0) return false;
    p->i += n;
    return true;
}

static bool jp_value(JP *p, JVal *out)
{
    memset(out, 0, sizeof *out);
    jp_ws(p);
    if (p->i >= p->n) return false;
    unsigned char c = p->s[p->i];
    switch (c) {
    case '{': {
        if (++p->depth > JSON_MAX_DEPTH) return false;
        p->i++;
        out->t = J_OBJ;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; p->depth--; return true; }
        for (;;) {
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') return false;
            JMember m;
            memset(&m, 0, sizeof m);
            if (!jp_string(p, &m.key, &m.klen)) return false;
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { free(m.key); return false; }
            p->i++;
            if (!jp_value(p, &m.val)) { free(m.key); jfree(&m.val); return false; }
            if (out->n == out->cap) {
                size_t nc = out->cap ? out->cap * 2 : 8;
                JMember *nm = (JMember *)realloc(out->mem, nc * sizeof *nm);
                if (!nm) { free(m.key); jfree(&m.val); return false; }
                out->mem = nm;
                out->cap = nc;
            }
            out->mem[out->n++] = m;
            jp_ws(p);
            if (p->i >= p->n) return false;
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == '}') { p->i++; break; }
            return false;
        }
        p->depth--;
        return true;
    }
    case '[': {
        if (++p->depth > JSON_MAX_DEPTH) return false;
        p->i++;
        out->t = J_ARR;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; p->depth--; return true; }
        for (;;) {
            JVal v;
            if (!jp_value(p, &v)) { jfree(&v); return false; }
            if (out->n == out->cap) {
                size_t nc = out->cap ? out->cap * 2 : 8;
                JVal *ni = (JVal *)realloc(out->items, nc * sizeof *ni);
                if (!ni) { jfree(&v); return false; }
                out->items = ni;
                out->cap = nc;
            }
            out->items[out->n++] = v;
            jp_ws(p);
            if (p->i >= p->n) return false;
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == ']') { p->i++; break; }
            return false;
        }
        p->depth--;
        return true;
    }
    case '"':
        out->t = J_STR;
        return jp_string(p, &out->str, &out->slen);
    case 't': out->t = J_TRUE;  return jp_lit(p, "true", 4);
    case 'f': out->t = J_FALSE; return jp_lit(p, "false", 5);
    case 'n': out->t = J_NULL;  return jp_lit(p, "null", 4);
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p, out);
        return false;
    }
}

/* UTF-16 file (BOM FF FE / FE FF) -> UTF-8, as the web's FileReader would decode it. */
static bool utf16_to_utf8(const unsigned char *s, size_t n, bool le, AlmBuf *out)
{
    char enc[4];
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = le ? (uint32_t)(s[i] | (s[i + 1] << 8)) : (uint32_t)((s[i] << 8) | s[i + 1]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            uint32_t l = le ? (uint32_t)(s[i + 2] | (s[i + 3] << 8)) : (uint32_t)((s[i + 2] << 8) | s[i + 3]);
            if (l >= 0xDC00 && l <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); i += 2; }
            else u = 0xFFFD;
        } else if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        size_t k = (u == 0) ? 1 : u8_put(u, enc);
        if (u == 0) enc[0] = 0;
        if (!buf_add(out, enc, k)) return false;
    }
    if (n & 1) return buf_add(out, "\xEF\xBF\xBD", 3);
    return true;
}

typedef enum { JPARSE_OK = 0, JPARSE_BAD, JPARSE_OOM } JParse;

/* Parses a whole document (BOM aware). */
static JParse json_parse(const char *text, size_t len, JVal *out)
{
    memset(out, 0, sizeof *out);
    const unsigned char *s = (const unsigned char *)text;
    AlmBuf conv = {0};
    if (len >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) {
        if (!utf16_to_utf8(s + 2, len - 2, s[0] == 0xFF, &conv)) { alm_buf_free(&conv); return JPARSE_OOM; }
        s = (const unsigned char *)(conv.data ? conv.data : "");
        len = conv.len;
    } else if (len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
        s += 3; len -= 3;
    }
    JP p = { s, len, 0, 0 };
    bool ok = jp_value(&p, out);
    if (ok) { jp_ws(&p); ok = p.i == p.n; }
    alm_buf_free(&conv);
    if (!ok) { jfree(out); return JPARSE_BAD; }
    return JPARSE_OK;
}

/* obj[key] (last duplicate wins, as JSON.parse); NULL when absent or obj is not an object. */
static const JVal *jget(const JVal *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    size_t kl = strlen(key);
    for (size_t i = obj->n; i-- > 0;)
        if (obj->mem[i].klen == kl && memcmp(obj->mem[i].key, key, kl) == 0) return &obj->mem[i].val;
    return NULL;
}

static bool j_num(const JVal *v) { return v && v->t == J_NUM; }
static bool j_str_nonempty(const JVal *v) { return v && v->t == J_STR && v->slen > 0; }

/* Number.isInteger(v) && lo <= v <= hi */
static bool j_int(const JVal *v, double lo, double hi, int32_t *out)
{
    if (!j_num(v) || !is_integral(v->num) || v->num < lo || v->num > hi) return false;
    *out = (int32_t)v->num;
    return true;
}

static bool j_truthy(const JVal *v)
{
    if (!v) return false;
    switch (v->t) {
    case J_NULL: case J_FALSE: return false;
    case J_NUM: return !(v->num == 0 || isnan(v->num));
    case J_STR: return v->slen > 0;
    default: return true;
    }
}

/* ToNumber of a JSON value (for "x | 0"). */
static double j_to_number(const JVal *v, int depth)
{
    if (!v) return NAN;   /* undefined */
    switch (v->t) {
    case J_NULL: case J_FALSE: return 0;
    case J_TRUE: return 1;
    case J_NUM: return v->num;
    case J_STR: return js_str_to_number(v->str, v->slen);
    case J_ARR:
        /* ToPrimitive(array) = join(","): two or more elements always contain a comma */
        if (v->n == 0) return 0;
        if (v->n > 1 || depth > 64) return NAN;
        if (v->items[0].t == J_NULL) return 0;
        if (v->items[0].t == J_NUM || v->items[0].t == J_STR || v->items[0].t == J_ARR)
            return j_to_number(&v->items[0], depth + 1);
        return NAN;
    default:
        return NAN;
    }
}

/* ================================================================================
 * JSON writer (JSON.stringify formatting, compact or with an indent)
 * ============================================================================== */

typedef struct JW {
    AlmBuf *b;
    int     indent;          /* 0 = compact */
    int     depth;
    bool    first[16];
    bool    ok;
} JW;

static void jw_raw(JW *w, const char *s, size_t n) { if (w->ok && !buf_add(w->b, s, n)) w->ok = false; }
static void jw_s(JW *w, const char *s) { jw_raw(w, s, strlen(s)); }

static void jw_newline(JW *w, int depth)
{
    if (!w->indent) return;
    jw_raw(w, "\n", 1);
    for (int i = 0; i < depth * w->indent; i++) jw_raw(w, " ", 1);
}

static void jw_item(JW *w)
{
    if (!w->first[w->depth]) jw_raw(w, ",", 1);
    w->first[w->depth] = false;
    jw_newline(w, w->depth);
}

static void jw_begin(JW *w, char c)
{
    jw_raw(w, &c, 1);
    if (w->depth + 1 >= (int)(sizeof w->first / sizeof w->first[0])) { w->ok = false; return; }
    w->depth++;
    w->first[w->depth] = true;
}

static void jw_end(JW *w, char c)
{
    if (!w->first[w->depth]) jw_newline(w, w->depth - 1);
    w->depth--;
    jw_raw(w, &c, 1);
}

/* QuoteJSONString over the internal encoding */
static void jw_str(JW *w, const char *s)
{
    static const char hx[] = "0123456789abcdef";
    const unsigned char *u = (const unsigned char *)s;
    size_t n = strlen(s), i = 0, run = 0, adv;
    jw_raw(w, "\"", 1);
    while (i < n) {
        unsigned c = u[i];
        if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') { i++; run++; continue; }
        if (run) { jw_raw(w, (const char *)u + i - run, run); run = 0; }
        if (c < 0x80) {
            char esc[7];
            switch (c) {
            case '"': jw_raw(w, "\\\"", 2); break;
            case '\\': jw_raw(w, "\\\\", 2); break;
            case 8: jw_raw(w, "\\b", 2); break;
            case 9: jw_raw(w, "\\t", 2); break;
            case 10: jw_raw(w, "\\n", 2); break;
            case 12: jw_raw(w, "\\f", 2); break;
            case 13: jw_raw(w, "\\r", 2); break;
            default:
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hx[c >> 4]; esc[5] = hx[c & 15];
                jw_raw(w, esc, 6);
            }
            i++;
            continue;
        }
        uint32_t cp = u8_next(u + i, n - i, &adv);
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            char esc[7] = { '\\', 'u', hx[(cp >> 12) & 15], hx[(cp >> 8) & 15], hx[(cp >> 4) & 15], hx[cp & 15], 0 };
            jw_raw(w, esc, 6);
        } else if (cp == 0xFFFD && adv == 1) {
            jw_raw(w, "\xEF\xBF\xBD", 3);          /* stray invalid byte: keep the output valid */
        } else {
            jw_raw(w, (const char *)u + i, adv);
        }
        i += adv;
    }
    if (run) jw_raw(w, (const char *)u + i - run, run);
    jw_raw(w, "\"", 1);
}

static void jw_key(JW *w, const char *k)
{
    jw_item(w);
    jw_str(w, k);
    jw_raw(w, w->indent ? ": " : ":", w->indent ? 2 : 1);
}

static void jw_num(JW *w, double x)
{
    char t[ALM_NUM_CAP];
    size_t n = alm_fmt_js_number(x, t, sizeof t);
    jw_raw(w, t, n);
}
static void jw_int(JW *w, int64_t x)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", (long long)x);
    jw_raw(w, t, (size_t)n);
}
static void jw_bool(JW *w, bool v) { jw_s(w, v ? "true" : "false"); }

/* ================================================================================
 * Text maps (yearNames / notes)
 * ============================================================================== */

const char *alm_map_get(const AlmTextMap *m, int32_t key)
{
    for (int i = 0; i < m->count; i++)
        if (m->items[i].key == key) return m->items[i].text;
    return NULL;
}

static void map_free(AlmTextMap *m)
{
    for (int i = 0; i < m->count; i++) free(m->items[i].text);
    free(m->items);
    memset(m, 0, sizeof *m);
}

/* takes ownership of text */
static bool map_put(AlmTextMap *m, int32_t key, char *text)
{
    for (int i = 0; i < m->count; i++)
        if (m->items[i].key == key) { free(m->items[i].text); m->items[i].text = text; return true; }
    if (m->count == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        AlmTextEntry *ni = (AlmTextEntry *)realloc(m->items, (size_t)nc * sizeof *ni);
        if (!ni) return false;
        m->items = ni;
        m->cap = nc;
    }
    m->items[m->count].key = key;
    m->items[m->count].text = text;
    m->count++;
    return true;
}

static void map_del(AlmTextMap *m, int32_t key)
{
    for (int i = 0; i < m->count; i++)
        if (m->items[i].key == key) {
            free(m->items[i].text);
            memmove(m->items + i, m->items + i + 1, (size_t)(m->count - i - 1) * sizeof *m->items);
            m->count--;
            return;
        }
}

/* JS own-key order: array indices (here: keys >= 0) ascending, then the rest in insertion order. */
static const AlmTextMap *g_sort_map;
static int cmp_map_order(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    int32_t ka = g_sort_map->items[a].key, kb = g_sort_map->items[b].key;
    bool ia = ka >= 0, ib = kb >= 0;
    if (ia != ib) return ia ? -1 : 1;
    if (ia && ka != kb) return ka < kb ? -1 : 1;
    return a < b ? -1 : a > b;
}

static void jw_map(JW *w, const AlmTextMap *m)
{
    jw_begin(w, '{');
    if (m->count > 0) {
        int *ord = (int *)malloc((size_t)m->count * sizeof *ord);
        if (!ord) { w->ok = false; return; }
        for (int i = 0; i < m->count; i++) ord[i] = i;
        g_sort_map = m;
        qsort(ord, (size_t)m->count, sizeof *ord, cmp_map_order);
        for (int i = 0; i < m->count; i++) {
            char k[16];
            snprintf(k, sizeof k, "%ld", (long)m->items[ord[i]].key);
            jw_key(w, k);
            jw_str(w, m->items[ord[i]].text);
        }
        free(ord);
    }
    jw_end(w, '}');
}

/* cleanMap(m): see store.h. Reproduces Object.keys order, duplicate keys and key
 * renormalisation ("012" -> 12, " 5" -> 5) exactly. */
typedef struct MapSrc {
    const char *key; size_t klen;
    const JVal *val;
    size_t pos;         /* first occurrence */
    size_t lastpos;     /* occurrence whose value wins */
    bool   canon;       /* canonical array index */
    double idx;
} MapSrc;

static int cmp_src_key(const void *pa, const void *pb)
{
    const MapSrc *a = (const MapSrc *)pa, *b = (const MapSrc *)pb;
    size_t n = a->klen < b->klen ? a->klen : b->klen;
    int c = memcmp(a->key, b->key, n);
    if (c) return c;
    if (a->klen != b->klen) return a->klen < b->klen ? -1 : 1;
    return a->pos < b->pos ? -1 : a->pos > b->pos;
}
static int cmp_src_order(const void *pa, const void *pb)
{
    const MapSrc *a = (const MapSrc *)pa, *b = (const MapSrc *)pb;
    if (a->canon != b->canon) return a->canon ? -1 : 1;
    if (a->canon && a->idx != b->idx) return a->idx < b->idx ? -1 : 1;
    return a->pos < b->pos ? -1 : a->pos > b->pos;
}

typedef struct MapOut { int32_t key; size_t order; const JVal *val; size_t first; } MapOut;
static int cmp_out_key(const void *pa, const void *pb)
{
    const MapOut *a = (const MapOut *)pa, *b = (const MapOut *)pb;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}
static int cmp_out_first(const void *pa, const void *pb)
{
    const MapOut *a = (const MapOut *)pa, *b = (const MapOut *)pb;
    return a->first < b->first ? -1 : a->first > b->first;
}

static bool canon_index(const char *k, size_t n, double *idx)
{
    if (n == 0 || n > 10) return false;
    if (n > 1 && k[0] == '0') return false;
    double v = 0;
    for (size_t i = 0; i < n; i++) {
        if (k[i] < '0' || k[i] > '9') return false;
        v = v * 10 + (k[i] - '0');
    }
    if (v > 4294967294.0) return false;
    *idx = v;
    return true;
}

static bool clean_map(const JVal *m, double lo, double hi, size_t max_bytes, AlmTextMap *out)
{
    memset(out, 0, sizeof *out);
    if (!m || (m->t != J_OBJ && m->t != J_ARR) || m->n == 0) return true;
    size_t n = m->n;
    MapSrc *src = (MapSrc *)calloc(n, sizeof *src);
    MapOut *res = (MapOut *)calloc(n, sizeof *res);
    char (*akeys)[12] = NULL;
    bool ok = false;
    if (!src || !res) goto done;
    size_t u = 0;
    if (m->t == J_ARR) {
        akeys = (char (*)[12])malloc(n * sizeof *akeys);
        if (!akeys) goto done;
        for (size_t i = 0; i < n; i++) {
            int kl = snprintf(akeys[i], sizeof akeys[i], "%lu", (unsigned long)i);
            src[i].key = akeys[i]; src[i].klen = (size_t)kl; src[i].val = &m->items[i];
            src[i].pos = src[i].lastpos = i;
            src[i].canon = true; src[i].idx = (double)i;
        }
        u = n;
    } else {
        for (size_t i = 0; i < n; i++) {
            src[i].key = m->mem[i].key; src[i].klen = m->mem[i].klen; src[i].val = &m->mem[i].val;
            src[i].pos = src[i].lastpos = i;
        }
        qsort(src, n, sizeof *src, cmp_src_key);
        for (size_t i = 0; i < n;) {
            size_t j = i;
            while (j + 1 < n && src[j + 1].klen == src[i].klen && memcmp(src[j + 1].key, src[i].key, src[i].klen) == 0) j++;
            MapSrc g = src[i];             /* first occurrence: order */
            g.val = src[j].val;            /* last occurrence: value */
            g.lastpos = src[j].pos;
            g.canon = canon_index(g.key, g.klen, &g.idx);
            src[u++] = g;
            i = j + 1;
        }
        qsort(src, u, sizeof *src, cmp_src_order);
    }
    size_t r = 0;
    for (size_t i = 0; i < u; i++) {
        const JVal *v = src[i].val;
        if (v->t != J_STR || u8_blank(v->str, v->slen)) continue;
        double num = js_str_to_number(src[i].key, src[i].klen);
        if (!is_integral(num) || num < lo || num > hi) continue;
        res[r].key = (int32_t)num;     /* -0 -> 0 */
        res[r].order = i;
        res[r].val = v;
        r++;
    }
    /* group by numeric key: value of the last assignment, position of the first */
    qsort(res, r, sizeof *res, cmp_out_key);
    size_t w = 0;
    for (size_t i = 0; i < r;) {
        size_t j = i;
        while (j + 1 < r && res[j + 1].key == res[i].key) j++;
        res[w] = res[j];
        res[w].first = res[i].order;
        w++;
        i = j + 1;
    }
    qsort(res, w, sizeof *res, cmp_out_first);
    for (size_t i = 0; i < w; i++) {
        size_t len = u8_cut_bytes(res[i].val->str, res[i].val->slen, max_bytes);
        char *t = xstrndup(res[i].val->str, len);
        if (!t || !map_put(out, res[i].key, t)) { free(t); map_free(out); goto done; }
    }
    ok = true;
done:
    free(src);
    free(res);
    free(akeys);
    return ok;
}

/* ================================================================================
 * Event types
 * ============================================================================== */

static const char *const EV_KEYS[ALM_EV_TYPE_COUNT] = { "natural", "unnatural", "auril", "reminder" };
static const char *const EV_LABELS[ALM_EV_TYPE_COUNT] = { "Natural", "Unnatural", "Auril", "Reminder" };
static const uint32_t EV_RGB[ALM_EV_TYPE_COUNT] = { 0x56e6b6, 0xb897ff, 0xe6f7ff, 0xffc46b };
static const int EV_PRIO[ALM_EV_TYPE_COUNT] = { 2, 1, 0, 3 };

const char *alm_ev_type_key(AlmEvType t) { return (unsigned)t < ALM_EV_TYPE_COUNT ? EV_KEYS[t] : "natural"; }
const char *alm_ev_type_label(AlmEvType t) { return (unsigned)t < ALM_EV_TYPE_COUNT ? EV_LABELS[t] : "Natural"; }
AlmEvType alm_ev_type_from_key(const char *key)
{
    if (key)
        for (int i = 0; i < ALM_EV_TYPE_COUNT; i++)
            if (strcmp(key, EV_KEYS[i]) == 0) return (AlmEvType)i;
    return ALM_EV_TYPE_COUNT;
}
int alm_ev_type_priority(AlmEvType t) { return (unsigned)t < ALM_EV_TYPE_COUNT ? EV_PRIO[t] : 4; }
uint32_t alm_ev_type_rgb(AlmEvType t) { return (unsigned)t < ALM_EV_TYPE_COUNT ? EV_RGB[t] : EV_RGB[0]; }

static void event_free(AlmEvent *e)
{
    free(e->id); free(e->title); free(e->details);
    memset(e, 0, sizeof *e);
}

/* ================================================================================
 * Entropy
 * ============================================================================== */

void alm_seed_entropy(Almanac *a, uint64_t entropy) { a->rand_state = entropy ^ 0x9E3779B97F4A7C15ull; }

uint32_t alm_rand_u32(Almanac *a)
{
    /* splitmix64 */
    uint64_t z = (a->rand_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

size_t alm_random_seed(Almanac *a, char *buf, size_t cap)
{
    uint32_t r0 = alm_rand_u32(a), r1 = alm_rand_u32(a);
    return iwd_random_seed(r0, r1, buf, cap);
}

static void base36(uint64_t v, char *out, size_t cap)
{
    char t[16];
    int n = 0;
    do { t[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]; v /= 36; } while (v && n < 15);
    size_t k = 0;
    while (n > 0 && k + 1 < cap) out[k++] = t[--n];
    out[k] = 0;
}

/* ================================================================================
 * State construction (clean + applyState)
 * ============================================================================== */

static void state_free(Almanac *a)
{
    free(a->seed);
    map_free(&a->year_names);
    map_free(&a->notes);
    for (int i = 0; i < a->n_events; i++) event_free(&a->events[i]);
    free(a->events);
    free(a->days);
    a->seed = NULL; a->events = NULL; a->days = NULL;
    a->n_events = a->cap_events = 0;
    a->n_days = a->cap_days = 0;
}

void alm_free(Almanac *a)
{
    if (!a) return;
    state_free(a);
}

bool alm_ensure_days(Almanac *a, int32_t n)
{
    if (n > IWD_MAX_DAYS) n = IWD_MAX_DAYS;
    if (n <= a->n_days) return true;
    if (n > a->cap_days) {
        int32_t nc = a->cap_days ? a->cap_days : 16;
        while (nc < n) nc *= 2;
        if (nc > IWD_MAX_DAYS) nc = IWD_MAX_DAYS;
        if (nc < n) nc = n;
        IwdDay *nd = (IwdDay *)realloc(a->days, (size_t)nc * sizeof *nd);
        if (!nd) return false;
        a->days = nd;
        a->cap_days = nc;
    }
    while (a->n_days < n) iwd_next_day(&a->chain, &a->days[a->n_days++]);
    return true;
}

static void show_month_of(Almanac *a, int32_t abs)
{
    int32_t y;
    int m;
    iwd_month_of(abs, &y, &m);
    a->ui.cal_year = y;
    a->ui.cal_month = m;
}

#define ABS_LO ((double)IWD_ABS_MIN)
#define ABS_HI ((double)IWD_ABS_MAX)
#define START_HI ((double)(IWD_ABS_MAX - IWD_MAX_DAYS))

static bool clean_event(Almanac *c, const JVal *e, AlmEvent *out)
{
    memset(out, 0, sizeof *out);
    if (!e || e->t != J_OBJ) return true;      /* dropped */
    int32_t abs;
    const JVal *title = jget(e, "title");
    if (!j_int(jget(e, "abs"), ABS_LO, ABS_HI, &abs)) return true;
    if (!title || title->t != J_STR || u8_blank(title->str, title->slen)) return true;
    const JVal *id = jget(e, "id"), *type = jget(e, "type"), *rep = jget(e, "repeat"), *det = jget(e, "details");
    if (j_str_nonempty(id)) out->id = xstrndup(id->str, id->slen);
    else {
        char t[12];
        t[0] = 'e';
        for (int k = 1; k <= 8; k++) t[k] = "0123456789abcdefghijklmnopqrstuvwxyz"[alm_rand_u32(c) % 36];
        t[9] = 0;
        out->id = xstrdup(t);
    }
    out->abs = abs;
    out->title = dup_trim_cut(title->str, ALM_TITLE_MAX_UNITS);
    out->type = ALM_EV_NATURAL;
    if (type && type->t == J_STR) {
        AlmEvType t = alm_ev_type_from_key(type->str);
        if (t != ALM_EV_TYPE_COUNT) out->type = t;
    }
    out->repeat = IWD_REPEAT_NONE;
    if (rep && rep->t == J_STR) {
        IwdRepeat r = iwd_repeat_from_key(rep->str);
        if ((unsigned)r < IWD_REPEAT_COUNT) out->repeat = r;
    }
    if (det && det->t == J_STR) out->details = xstrndup(det->str, u8_cut_units(det->str, det->slen, ALM_DETAILS_MAX_UNITS));
    else out->details = xstrdup("");
    if (!out->id || !out->title || !out->details) { event_free(out); return false; }
    return true;
}

static void parse_native(const JVal *nv, AlmNative *n)
{
    if (!nv || nv->t != J_OBJ) return;
    const JVal *v;
    if ((v = jget(nv, "sound"))) n->sound = v->t == J_TRUE;
    if ((v = jget(nv, "maximized"))) n->maximized = v->t == J_TRUE;
    if ((v = jget(nv, "ledger"))) n->ledger = v->t == J_TRUE;
    int32_t x, y, w, h;
    if (j_int(jget(nv, "x"), -1e6, 1e6, &x) && j_int(jget(nv, "y"), -1e6, 1e6, &y) &&
        j_int(jget(nv, "w"), 1, 1e6, &w) && j_int(jget(nv, "h"), 1, 1e6, &h)) {
        n->has_rect = true;
        n->win_x = x; n->win_y = y; n->win_w = w; n->win_h = h;
    }
}

/* clean(s) + applyState into a zeroed *c (rand_state / native preset). */
static bool clean_into(Almanac *c, const JVal *s)
{
    static const JVal empty = { J_OBJ, 0, NULL, 0, NULL, NULL, 0, 0 };
    if (!s || (s->t != J_OBJ && s->t != J_ARR)) s = &empty;
    const JVal *v;

    v = jget(s, "seed");
    if (j_str_nonempty(v)) c->seed = xstrndup(v->str, v->slen);
    else {
        char t[IWD_RANDOM_SEED_CAP];
        alm_random_seed(c, t, sizeof t);
        c->seed = xstrdup(t);
    }
    if (!c->seed) return false;

    v = jget(s, "started");
    c->started = j_num(v) ? v->num : 0;

    if (!j_int(jget(s, "startAbs"), ABS_LO, START_HI, &c->start_abs)) c->start_abs = IWD_DEFAULT_START_ABS;

    int32_t dc = to_int32(j_to_number(jget(s, "dayCount"), 0));
    if (dc < 0) dc = 0;
    if (dc > IWD_MAX_DAYS) dc = IWD_MAX_DAYS;

    int32_t t32;
    if (j_int(jget(s, "current"), -1, (double)dc - 1, &t32)) c->current = t32;
    else c->current = dc - 1;

    if (j_int(jget(s, "currentAbs"), ABS_LO, ABS_HI, &t32)) { c->has_current_abs = true; c->current_abs = t32; }
    else if (c->current >= 0) { c->has_current_abs = true; c->current_abs = c->start_abs + c->current; }
    else c->has_current_abs = false;
    if (c->current >= 0) { c->has_current_abs = true; c->current_abs = c->start_abs + c->current; }

    if (j_int(jget(s, "view"), 0, (double)dc - 1, &t32)) c->view = t32;
    else c->view = c->current;

    if (j_int(jget(s, "selAbs"), ABS_LO, ABS_HI, &t32)) c->sel_abs = t32;
    else c->sel_abs = c->start_abs + (c->view > 0 ? c->view : 0);

    v = jget(s, "time");
    c->time = (j_num(v) && v->num >= 0 && v->num < 24) ? v->num : IWD_SETTLE_HOUR;

    v = jget(s, "unit");
    c->celsius = v && v->t == J_STR && strcmp(v->str, "C") == 0 && v->slen == 1;

    v = jget(s, "moonOffset");
    if (j_num(v) && isfinite(v->num))
        c->moon_offset = v->num < ALM_MOON_OFFSET_MIN ? ALM_MOON_OFFSET_MIN : v->num > ALM_MOON_OFFSET_MAX ? ALM_MOON_OFFSET_MAX : v->num;
    else c->moon_offset = 0;

    if (!clean_map(jget(s, "yearNames"), IWD_YEAR_MIN, IWD_YEAR_MAX, (size_t)-1, &c->year_names)) return false;
    if (!clean_map(jget(s, "notes"), ABS_LO, ABS_HI, ALM_MAX_NOTE_BYTES, &c->notes)) return false;

    v = jget(s, "events");
    if (v && v->t == J_ARR && v->n > 0) {
        size_t cap = v->n < ALM_MAX_EVENTS ? v->n : ALM_MAX_EVENTS;
        c->events = (AlmEvent *)calloc(cap, sizeof *c->events);
        if (!c->events) return false;
        c->cap_events = (int)cap;
        for (size_t i = 0; i < v->n && c->n_events < (int)cap; i++) {
            AlmEvent e;
            if (!clean_event(c, &v->items[i], &e)) return false;
            if (e.id) c->events[c->n_events++] = e;
        }
    }

    const JVal *ui = jget(s, "ui");
    if (!ui || (ui->t != J_OBJ && ui->t != J_ARR)) ui = &empty;
    int32_t my;
    int mm;
    iwd_month_of(c->sel_abs, &my, &mm);
    c->ui.chain_note_hidden = j_truthy(jget(ui, "chainNoteHidden"));
    if (!j_int(jget(ui, "calYear"), IWD_YEAR_MIN, IWD_YEAR_MAX, &c->ui.cal_year)) c->ui.cal_year = my;
    if (j_int(jget(ui, "calMonth"), 0, 11, &t32)) c->ui.cal_month = t32;
    else c->ui.cal_month = mm;

    /* applyState */
    iwd_chain_init(&c->chain, c->seed);
    c->temp_hash = iwd_temp_hash(c->seed);
    if (!alm_ensure_days(c, dc)) return false;
    if (c->view < 0) c->time = 23;
    return true;
}

/* Builds a complete new state from s and swaps it into *a (unchanged on failure).
 * native: an optional "native" object to read prefs from (else a->native is kept). */
static bool build_state(Almanac *a, const JVal *s, const JVal *native)
{
    Almanac n;
    memset(&n, 0, sizeof n);
    n.rand_state = a->rand_state;
    n.native = a->native;
    parse_native(native, &n.native);
    if (!clean_into(&n, s)) { state_free(&n); return false; }
    n.epoch = a->epoch + 1;
    state_free(a);
    *a = n;
    return true;
}

bool alm_init(Almanac *a, uint64_t entropy)
{
    memset(a, 0, sizeof *a);
    alm_seed_entropy(a, entropy);
    return build_state(a, NULL, NULL);
}

/* ================================================================================
 * Snapshot / save / load / import / export
 * ============================================================================== */

static void write_snapshot(const Almanac *a, JW *w, const char *exported_iso, bool with_native)
{
    jw_begin(w, '{');
    if (exported_iso) {
        jw_key(w, "app"); jw_str(w, ALM_BACKUP_APP);
        jw_key(w, "exported"); jw_str(w, exported_iso);
    }
    jw_key(w, "v"); jw_int(w, ALM_STORE_VERSION);
    jw_key(w, "seed"); jw_str(w, a->seed);
    jw_key(w, "started"); jw_num(w, a->started);
    jw_key(w, "startAbs"); jw_int(w, a->start_abs);
    jw_key(w, "dayCount"); jw_int(w, a->n_days);
    jw_key(w, "current"); jw_int(w, a->current);
    jw_key(w, "currentAbs"); if (a->has_current_abs) jw_int(w, a->current_abs); else jw_s(w, "null");
    jw_key(w, "view"); jw_int(w, a->view);
    jw_key(w, "selAbs"); jw_int(w, a->sel_abs);
    jw_key(w, "time"); jw_num(w, a->time);
    jw_key(w, "unit"); jw_s(w, a->celsius ? "\"C\"" : "\"F\"");
    jw_key(w, "moonOffset"); jw_num(w, a->moon_offset);
    jw_key(w, "yearNames"); jw_map(w, &a->year_names);
    jw_key(w, "notes"); jw_map(w, &a->notes);
    jw_key(w, "events");
    jw_begin(w, '[');
    for (int i = 0; i < a->n_events; i++) {
        const AlmEvent *e = &a->events[i];
        jw_item(w);
        jw_begin(w, '{');
        jw_key(w, "id"); jw_str(w, e->id);
        jw_key(w, "abs"); jw_int(w, e->abs);
        jw_key(w, "title"); jw_str(w, e->title);
        jw_key(w, "type"); jw_str(w, alm_ev_type_key(e->type));
        jw_key(w, "repeat"); jw_str(w, iwd_repeat_key(e->repeat));
        jw_key(w, "details"); jw_str(w, e->details ? e->details : "");
        jw_end(w, '}');
    }
    jw_end(w, ']');
    jw_key(w, "ui");
    jw_begin(w, '{');
    jw_key(w, "chainNoteHidden"); jw_bool(w, a->ui.chain_note_hidden);
    jw_key(w, "calYear"); jw_int(w, a->ui.cal_year);
    jw_key(w, "calMonth"); jw_int(w, a->ui.cal_month);
    jw_end(w, '}');
    if (with_native) {
        const AlmNative *n = &a->native;
        jw_key(w, "native");
        jw_begin(w, '{');
        jw_key(w, "sound"); jw_bool(w, n->sound);
        jw_key(w, "maximized"); jw_bool(w, n->maximized);
        jw_key(w, "ledger"); jw_bool(w, n->ledger);
        if (n->has_rect) {
            jw_key(w, "x"); jw_int(w, n->win_x);
            jw_key(w, "y"); jw_int(w, n->win_y);
            jw_key(w, "w"); jw_int(w, n->win_w);
            jw_key(w, "h"); jw_int(w, n->win_h);
        }
        jw_end(w, '}');
    }
    jw_end(w, '}');
}

bool alm_snapshot_json(const Almanac *a, bool with_native, AlmBuf *out)
{
    out->len = 0;
    if (!buf_reserve(out, 1024)) return false;
    out->data[0] = 0;
    JW w;
    memset(&w, 0, sizeof w);
    w.b = out; w.ok = true;
    write_snapshot(a, &w, NULL, with_native);
    return w.ok;
}

bool alm_export_json(const Almanac *a, const char *exported_iso, AlmBuf *out)
{
    out->len = 0;
    if (!buf_reserve(out, 1024)) return false;
    out->data[0] = 0;
    JW w;
    memset(&w, 0, sizeof w);
    w.b = out; w.indent = 2; w.ok = true;
    write_snapshot(a, &w, exported_iso ? exported_iso : "", false);
    return w.ok;
}

AlmIoStatus alm_save_file(const Almanac *a, const char *path_utf8)
{
    return alm_save_file_ex(a, path_utf8, true);
}

AlmIoStatus alm_save_file_ex(const Almanac *a, const char *path_utf8, bool durable)
{
    AlmBuf b = {0};
    if (!alm_snapshot_json(a, true, &b)) { alm_buf_free(&b); return ALM_IO_ERROR; }
    AlmIoStatus st = alm_fs_write_atomic_ex(path_utf8, b.data, b.len, durable);
    alm_buf_free(&b);
    return st;
}

size_t alm_export_filename(const Almanac *a, int year, int month1, int day, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    sb_adds(&sb, "icewind-almanac-");
    for (const unsigned char *p = (const unsigned char *)a->seed; *p; p++) {
        char c = (char)*p;
        if (*p < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || *p == 0x7F)
            c = '_';
        sb_addn(&sb, &c, 1);
    }
    sb_addf(&sb, "-%d%02d%02d.json", year, month1, day);
    return sb.len;
}

static AlmLoadStatus load_fresh(Almanac *a, AlmLoadStatus st)
{
    return build_state(a, NULL, NULL) ? st : ALM_LOAD_NO_MEMORY;
}

/* v1 shape -> v2 input object (shallow; members point into v1). */
static bool migrate_v1(Almanac *a, const JVal *v1, const JVal *native)
{
    int32_t n = to_int32(j_to_number(jget(v1, "dayCount"), 0));
    if (n < 0) n = 0;
    if (n > IWD_MAX_DAYS) n = IWD_MAX_DAYS;
    JMember mem[8];
    JVal obj;
    memset(&obj, 0, sizeof obj);
    memset(mem, 0, sizeof mem);
    obj.t = J_OBJ;
    obj.mem = mem;
    static const char *const copy[] = { "seed", "started", "view", "time", "unit" };
    for (size_t i = 0; i < sizeof copy / sizeof copy[0]; i++) {
        const JVal *v = jget(v1, copy[i]);
        if (!v) continue;
        mem[obj.n].key = (char *)copy[i];
        mem[obj.n].klen = strlen(copy[i]);
        mem[obj.n].val = *v;
        obj.n++;
    }
    mem[obj.n].key = (char *)"startAbs"; mem[obj.n].klen = 8;
    mem[obj.n].val.t = J_NUM; mem[obj.n].val.num = IWD_DEFAULT_START_ABS; obj.n++;
    mem[obj.n].key = (char *)"dayCount"; mem[obj.n].klen = 8;
    mem[obj.n].val.t = J_NUM; mem[obj.n].val.num = n; obj.n++;
    mem[obj.n].key = (char *)"current"; mem[obj.n].klen = 7;
    mem[obj.n].val.t = J_NUM; mem[obj.n].val.num = n - 1; obj.n++;
    return build_state(a, &obj, native);   /* obj itself is never freed */
}

AlmLoadStatus alm_load_json(Almanac *a, const char *json, size_t len)
{
    JVal root;
    JParse pr = json_parse(json, len, &root);
    if (pr == JPARSE_OOM) return ALM_LOAD_NO_MEMORY;
    if (pr != JPARSE_OK) return load_fresh(a, ALM_LOAD_CORRUPT);
    if (root.t != J_OBJ) { jfree(&root); return load_fresh(a, ALM_LOAD_CORRUPT); }
    const JVal *native = jget(&root, "native");
    const JVal *ver = jget(&root, "v");
    AlmLoadStatus st;
    if (!ver || (ver->t == J_NUM && ver->num == 1)) {
        if (j_str_nonempty(jget(&root, "seed")))
            st = migrate_v1(a, &root, native) ? ALM_LOAD_MIGRATED_V1 : ALM_LOAD_NO_MEMORY;
        else
            st = load_fresh(a, ALM_LOAD_CORRUPT);
    } else {
        st = build_state(a, &root, native) ? ALM_LOAD_OK : ALM_LOAD_NO_MEMORY;
    }
    jfree(&root);
    return st;
}

AlmLoadStatus alm_load_file(Almanac *a, const char *path_utf8)
{
    AlmBuf b = {0};
    AlmIoStatus io = alm_fs_read(path_utf8, &b);
    AlmLoadStatus st;
    if (io == ALM_IO_NOT_FOUND) st = load_fresh(a, ALM_LOAD_NO_FILE);
    else if (io != ALM_IO_OK) st = load_fresh(a, ALM_LOAD_CORRUPT);
    else st = alm_load_json(a, b.data ? b.data : "", b.len);
    alm_buf_free(&b);
    return st;
}

AlmImportStatus alm_import_json(Almanac *a, const char *json, size_t len, double now_ms)
{
    JVal root;
    JParse pr = json_parse(json, len, &root);
    if (pr == JPARSE_OOM) return ALM_IMPORT_NO_MEMORY;
    if (pr != JPARSE_OK) return ALM_IMPORT_NOT_BACKUP;
    if (root.t != J_OBJ || !j_str_nonempty(jget(&root, "seed"))) { jfree(&root); return ALM_IMPORT_NOT_BACKUP; }
    if (a->busy) { jfree(&root); return ALM_IMPORT_BUSY; }
    if (!build_state(a, &root, NULL)) { jfree(&root); return ALM_IMPORT_NO_MEMORY; }
    const JVal *st = jget(&root, "started");
    a->started = (j_num(st) && st->num != 0 && !isnan(st->num)) ? st->num : now_ms;
    jfree(&root);
    return ALM_IMPORT_OK;
}

AlmImportStatus alm_import_file(Almanac *a, const char *path_utf8, double now_ms)
{
    AlmBuf b = {0};
    AlmIoStatus io = alm_fs_read(path_utf8, &b);
    AlmImportStatus st;
    if (io == ALM_IO_TOO_LARGE) st = ALM_IMPORT_NOT_BACKUP;
    else if (io != ALM_IO_OK) st = ALM_IMPORT_READ_ERROR;
    else st = alm_import_json(a, b.data ? b.data : "", b.len, now_ms);
    alm_buf_free(&b);
    return st;
}

size_t alm_fmt_import_toast(const Almanac *a, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    sb_addf(&sb, "Backup restored \xC2\xB7 %d day%s, %d event%s", (int)a->n_days, a->n_days == 1 ? "" : "s",
            a->n_events, a->n_events == 1 ? "" : "s");
    return sb.len;
}

/* ================================================================================
 * File shim
 * ============================================================================== */

#ifdef _WIN32
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}
static FILE *open_utf8(const char *path, const wchar_t *mode)
{
    wchar_t *w = widen(path);
    if (!w) return NULL;
    FILE *f = _wfopen(w, mode);
    free(w);
    return f;
}
#else
static FILE *open_utf8(const char *path, const char *mode) { return fopen(path, mode); }
#endif

AlmIoStatus alm_fs_read(const char *path_utf8, AlmBuf *out)
{
    out->len = 0;
#ifdef _WIN32
    FILE *f = open_utf8(path_utf8, L"rb");
#else
    FILE *f = open_utf8(path_utf8, "rb");
#endif
    if (!f) return errno == ENOENT ? ALM_IO_NOT_FOUND : ALM_IO_ERROR;
    char chunk[65536];
    size_t n;
    AlmIoStatus st = ALM_IO_OK;
    if (!buf_reserve(out, 0)) st = ALM_IO_ERROR;
    else out->data[0] = 0;
    while (st == ALM_IO_OK && (n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (out->len + n > ALM_MAX_FILE_BYTES) { st = ALM_IO_TOO_LARGE; break; }
        if (!buf_add(out, chunk, n)) st = ALM_IO_ERROR;
    }
    if (st == ALM_IO_OK && ferror(f)) st = ALM_IO_ERROR;
    fclose(f);
    if (st != ALM_IO_OK) alm_buf_free(out);
    return st;
}

AlmIoStatus alm_fs_write_atomic(const char *path_utf8, const char *data, size_t len)
{
    return alm_fs_write_atomic_ex(path_utf8, data, len, true);
}

AlmIoStatus alm_fs_write_atomic_ex(const char *path_utf8, const char *data, size_t len, bool durable)
{
    size_t pl = strlen(path_utf8);
    char *tmp = (char *)malloc(pl + 5);
    if (!tmp) return ALM_IO_ERROR;
    memcpy(tmp, path_utf8, pl);
    memcpy(tmp + pl, ".tmp", 5);
#ifdef _WIN32
    FILE *f = open_utf8(tmp, L"wb");
#else
    FILE *f = open_utf8(tmp, "wb");
#endif
    if (!f) { free(tmp); return ALM_IO_ERROR; }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fflush(f) != 0) ok = false;
#ifdef _WIN32
    if (ok && durable) _commit(_fileno(f));
#else
    if (ok && durable) fsync(fileno(f));
#endif
    if (fclose(f) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        wchar_t *wt = widen(tmp), *wp = widen(path_utf8);
        ok = wt && wp && MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0));
        if (!ok && wt) DeleteFileW(wt);
        free(wt);
        free(wp);
#else
        ok = rename(tmp, path_utf8) == 0;
        if (!ok) remove(tmp);
#endif
    } else {
        remove(tmp);
    }
    free(tmp);
    return ok ? ALM_IO_OK : ALM_IO_ERROR;
}

AlmIoStatus alm_fs_mkdirs(const char *dir_utf8)
{
    size_t n = strlen(dir_utf8);
    if (n == 0) return ALM_IO_OK;
    char *p = xstrdup(dir_utf8);
    if (!p) return ALM_IO_ERROR;
    AlmIoStatus st = ALM_IO_OK;
    for (size_t i = 1; i <= n && st == ALM_IO_OK; i++) {
        if (i < n && p[i] != '/' && p[i] != '\\') continue;
        if (i < n && p[i - 1] == ':') continue;        /* "C:\" */
        char save = p[i];
        p[i] = 0;
#ifdef _WIN32
        wchar_t *w = widen(p);
        if (!w) st = ALM_IO_ERROR;
        else {
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                DWORD at = GetFileAttributesW(w);
                if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) st = ALM_IO_ERROR;
            }
            free(w);
        }
#else
        if (mkdir(p, 0777) != 0 && errno != EEXIST) st = ALM_IO_ERROR;
#endif
        p[i] = save;
    }
    if (st == ALM_IO_OK) {
#ifdef _WIN32
        wchar_t *w = widen(dir_utf8);
        DWORD at = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
        free(w);
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) st = ALM_IO_ERROR;
#else
        struct stat sbuf;
        if (stat(dir_utf8, &sbuf) != 0 || !S_ISDIR(sbuf.st_mode)) st = ALM_IO_ERROR;
#endif
    }
    free(p);
    return st;
}

/* ================================================================================
 * Days and derived lookups
 * ============================================================================== */

bool alm_today_abs(const Almanac *a, int32_t *abs_out)
{
    if (a->current < 0) return false;
    if (abs_out) *abs_out = a->start_abs + a->current;
    return true;
}

const IwdDay *alm_weather_of(const Almanac *a, int32_t abs)
{
    int64_t i = (int64_t)abs - a->start_abs;
    return (i >= 0 && i < a->n_days) ? &a->days[i] : NULL;
}

int alm_hour_now(const Almanac *a)
{
    double f = floor(a->time);
    return f < 0 ? 0 : f > 23 ? 23 : (int)f;
}

int alm_minute_now(const Almanac *a)
{
    int h = alm_hour_now(a);
    double f = floor((a->time - h) * 60 + 1e-6);
    int m = isnan(f) || f < 0 ? 0 : f > 59 ? 59 : (int)f;
    return h * 60 + m;
}

bool alm_has_view(const Almanac *a) { return a->n_days > 0 && a->view >= 0 && a->view < a->n_days; }

int alm_event_at(const Almanac *a, int32_t v, int hour)
{
    if (a->n_days == 0 || v < 0) return 1;
    if (v >= a->n_days) v = a->n_days - 1;
    int64_t d = v, h = hour;
    while (h >= 24 && d < a->n_days - 1) { h -= 24; d++; }
    while (h < 0 && d > 0) { h += 24; d--; }
    if (h < 0) h = 0;
    if (h >= 24) {
        const IwdActive *c = &a->days[a->n_days - 1].carry_out;
        return (h - 24 < (int64_t)c->duration - c->elapsed) ? c->id : c->next;
    }
    return a->days[d].hour_id[h];
}

int alm_event_at_cb(void *user, int hour)
{
    const Almanac *a = (const Almanac *)user;
    return alm_event_at(a, a->view, hour);
}

/* ================================================================================
 * Small text helpers
 * ============================================================================== */

static void fmt_date(int32_t abs, bool short_form, char out[IWD_DATE_CAP])
{
    iwd_format_date(abs, short_form, out, IWD_DATE_CAP);
}

static AlmResult res0(void)
{
    AlmResult r;
    memset(&r, 0, sizeof r);
    return r;
}

static AlmResult rejected(void)
{
    AlmResult r = res0();
    r.fx = ALM_FX_REJECTED;
    return r;
}

static void res_toast(AlmResult *r, const char *fmt_head, const char *bold, const char *tail)
{
    SB sb;
    sb_init(&sb, r->toast, sizeof r->toast);
    sb_adds(&sb, fmt_head);
    if (bold) { sb_adds(&sb, "<b>"); sb_add_esc(&sb, bold); sb_adds(&sb, "</b>"); }
    sb_adds(&sb, tail);
    r->fx |= ALM_FX_TOAST;
}

static bool valid_abs(int64_t abs) { return abs >= IWD_ABS_MIN && abs <= IWD_ABS_MAX; }

/* ================================================================================
 * Actions
 * ============================================================================== */

AlmResult alm_advance(Almanac *a, double land_time)
{
    bool first = a->current < 0;
    int32_t target = first ? 0 : a->current + 1;
    if (a->busy || target >= IWD_MAX_DAYS) return rejected();
    if (!alm_ensure_days(a, target + 1)) return rejected();
    AlmResult r = res0();
    if (!(land_time >= 0 && land_time < 24)) land_time = IWD_SETTLE_HOUR;
    a->current = target;
    a->has_current_abs = true;
    a->current_abs = alm_abs_of(a, target);
    a->view = target;
    a->sel_abs = a->current_abs;
    a->time = land_time;
    show_month_of(a, a->sel_abs);
    r.fx = ALM_FX_SAVE | ALM_FX_RENDER_ALL | ALM_FX_ANNOUNCE;
    r.toast_abs = a->current_abs;
    r.toast_lead = first ? "The expedition begins" : "Today";
    if (alm_marks_on(a, a->current_abs, NULL, 0) > 0) r.fx |= ALM_FX_DAY_TOAST;
    return r;
}

AlmResult alm_select(Almanac *a, int32_t abs, bool from_calendar)
{
    if (!valid_abs(abs)) return rejected();
    AlmResult r = res0();
    int64_t i = (int64_t)abs - a->start_abs;
    if (i >= IWD_MAX_DAYS) {
        SB sb;
        sb_init(&sb, r.toast, sizeof r.toast);
        sb_addf(&sb, "Beyond the almanac: at most %d days of weather can be recorded.", IWD_MAX_DAYS);
        r.fx |= ALM_FX_TOAST;
    }
    int32_t prev = a->view;
    a->sel_abs = abs;
    if (i >= 0 && i < IWD_MAX_DAYS && !a->busy) {
        if (alm_ensure_days(a, (int32_t)i + 1) && i != a->view) {
            if (a->view < 0) a->time = IWD_SETTLE_HOUR;
            a->view = (int32_t)i;
        }
    }
    if (!from_calendar) show_month_of(a, abs);
    else {
        int32_t y;
        int m;
        iwd_month_of(abs, &y, &m);
        if (y != a->ui.cal_year || m != a->ui.cal_month) show_month_of(a, abs);
    }
    r.fx |= ALM_FX_RENDER_ALL | ALM_FX_SAVE;
    if (a->view != prev) r.fx |= ALM_FX_ANNOUNCE;
    return r;
}

AlmResult alm_set_current(Almanac *a, int32_t abs)
{
    if (a->busy || !valid_abs(abs)) return rejected();
    AlmResult r = res0();
    int64_t i = (int64_t)abs - a->start_abs;
    char d[IWD_DATE_CAP];
    if (i < 0) {
        fmt_date(a->start_abs, false, d);
        res_toast(&r, "The expedition begins on ", d, ". Change the start date in Settings to begin earlier.");
        return r;
    }
    if (i >= IWD_MAX_DAYS) {
        SB sb;
        sb_init(&sb, r.toast, sizeof r.toast);
        sb_addf(&sb, "Beyond the almanac: at most %d days can be recorded.", IWD_MAX_DAYS);
        r.fx |= ALM_FX_TOAST;
        return r;
    }
    if (i == a->current) {
        if (a->sel_abs != abs) return alm_select(a, abs, false);
        return r;
    }
    if (!alm_ensure_days(a, (int32_t)i + 1)) return rejected();
    if (a->view < 0) a->time = IWD_SETTLE_HOUR;
    a->current = (int32_t)i;
    a->has_current_abs = true;
    a->current_abs = abs;
    a->view = (int32_t)i;
    a->sel_abs = abs;
    show_month_of(a, abs);
    r.fx = ALM_FX_RENDER_ALL | ALM_FX_SAVE | ALM_FX_ANNOUNCE;
    r.toast_abs = abs;
    r.toast_lead = "Today is now";
    if (alm_marks_on(a, abs, NULL, 0) > 0) r.fx |= ALM_FX_DAY_TOAST;
    else {
        fmt_date(abs, false, d);
        res_toast(&r, "Today is now ", d, "");
    }
    return r;
}

AlmResult alm_step_day(Almanac *a, int dir)
{
    if (a->busy || a->view < 0) return rejected();
    int64_t i = (int64_t)a->view + dir;
    if (i < 0 || i >= IWD_MAX_DAYS) return rejected();
    return alm_select(a, alm_abs_of(a, (int32_t)i), false);
}

AlmResult alm_set_time(Almanac *a, double t)
{
    if (!alm_has_view(a) || isnan(t)) return rejected();
    AlmResult r = res0();
    if (t < 0) t = 0;
    if (t > 23.9999) t = 23.9999;
    a->time = t;
    r.fx = ALM_FX_RENDER_HOUR | ALM_FX_SAVE;
    return r;
}

AlmResult alm_step_minutes(Almanac *a, int delta)
{
    if (a->busy || !alm_has_view(a) || delta == 0 || delta >= 1440 || delta <= -1440) return rejected();
    int n = alm_minute_now(a) + delta;
    AlmResult r;
    if (n >= 0 && n < 1440) {
        r = res0();
        a->time = n / 60.0;
    } else if (n >= 1440) {
        double land = (n - 1440) / 60.0;
        if (a->view != a->current) {
            int64_t i = (int64_t)a->view + 1;
            if (i >= IWD_MAX_DAYS) return rejected();
            r = alm_select(a, alm_abs_of(a, (int32_t)i), false);
            if (r.fx & ALM_FX_REJECTED) return r;
            a->time = land;
        } else {
            r = alm_advance(a, land);
            if (r.fx & ALM_FX_REJECTED) return r;
        }
    } else if (a->view > 0) {
        r = alm_select(a, alm_abs_of(a, a->view - 1), false);
        if (r.fx & ALM_FX_REJECTED) return r;
        a->time = (n + 1440) / 60.0;
    } else {
        return rejected();
    }
    r.fx |= ALM_FX_ANNOUNCE | ALM_FX_SAVE | ALM_FX_RENDER_HOUR;
    return r;
}

AlmResult alm_set_unit(Almanac *a, bool celsius)
{
    AlmResult r = res0();
    a->celsius = celsius;
    r.fx = ALM_FX_RENDER_ALL | ALM_FX_SAVE;
    return r;
}

AlmResult alm_cal_step(Almanac *a, int dir)
{
    int32_t y = a->ui.cal_year;
    int m = a->ui.cal_month + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
    if (m < 0) { m = 11; y--; }
    if (m > 11) { m = 0; y++; }
    if (y < IWD_YEAR_MIN || y > IWD_YEAR_MAX) return rejected();
    a->ui.cal_year = y;
    a->ui.cal_month = m;
    AlmResult r = res0();
    r.fx = ALM_FX_RENDER_CAL | ALM_FX_SAVE;
    return r;
}


AlmResult alm_cal_today(Almanac *a)
{
    int32_t t;
    return alm_select(a, alm_today_abs(a, &t) ? t : a->start_abs, false);
}

/* ================================================================================
 * Settings
 * ============================================================================== */

static const AlmStartOption START_OPTIONS[ALM_START_OPTION_COUNT] = {
    { 0, IWD_FEST_NONE, "Hammer \xE2\x80\x94 Deepwinter" },
    { -1, IWD_FEST_MIDWINTER, "Midwinter (festival)" },
    { 1, IWD_FEST_NONE, "Alturiak \xE2\x80\x94 The Claw of Winter" },
    { 2, IWD_FEST_NONE, "Ches \xE2\x80\x94 The Claw of the Sunsets" },
    { 3, IWD_FEST_NONE, "Tarsakh \xE2\x80\x94 The Claw of the Storms" },
    { -1, IWD_FEST_GREENGRASS, "Greengrass (festival)" },
    { 4, IWD_FEST_NONE, "Mirtul \xE2\x80\x94 The Melting" },
    { 5, IWD_FEST_NONE, "Kythorn \xE2\x80\x94 The Time of Flowers" },
    { 6, IWD_FEST_NONE, "Flamerule \xE2\x80\x94 Summertide" },
    { -1, IWD_FEST_MIDSUMMER, "Midsummer (festival)" },
    { -1, IWD_FEST_SHIELDMEET, "Shieldmeet (festival, leap years)" },
    { 7, IWD_FEST_NONE, "Eleasis \xE2\x80\x94 Highsun" },
    { 8, IWD_FEST_NONE, "Eleint \xE2\x80\x94 The Fading" },
    { -1, IWD_FEST_HIGHHARVESTIDE, "Highharvestide (festival)" },
    { 9, IWD_FEST_NONE, "Marpenoth \xE2\x80\x94 Leaffall" },
    { 10, IWD_FEST_NONE, "Uktar \xE2\x80\x94 The Rotting" },
    { -1, IWD_FEST_FEASTOFTHEMOON, "Feast of the Moon (festival)" },
    { 11, IWD_FEST_NONE, "Nightal \xE2\x80\x94 The Drawing Down" },
};

const AlmStartOption *alm_start_option(int index)
{
    return (index >= 0 && index < ALM_START_OPTION_COUNT) ? &START_OPTIONS[index] : NULL;
}

int alm_start_option_index(int month, IwdFestival festival)
{
    for (int i = 0; i < ALM_START_OPTION_COUNT; i++) {
        if (festival != IWD_FEST_NONE) { if (START_OPTIONS[i].festival == festival) return i; }
        else if (START_OPTIONS[i].festival == IWD_FEST_NONE && START_OPTIONS[i].month == month) return i;
    }
    return 0;
}

const char *alm_year_name(const Almanac *a, int32_t year)
{
    const char *s = alm_map_get(&a->year_names, year);
    return s ? s : "";
}

static void copy_cut(char *dst, size_t cap, const char *src)
{
    SB sb;
    sb_init(&sb, dst, cap);
    sb_adds(&sb, src);
}

void alm_settings_fill(const Almanac *a, AlmSettingsForm *f)
{
    memset(f, 0, sizeof *f);
    IwdDate d = iwd_from_abs(a->start_abs);
    f->start_year = d.year;
    f->start_year_valid = true;
    f->start_option = alm_start_option_index(d.month, d.festival);
    f->start_day = d.day ? d.day : 1;
    f->start_day_valid = true;
    copy_cut(f->seed, sizeof f->seed, a->seed);
    f->yn_year = a->ui.cal_year;
    f->yn_year_valid = true;
    copy_cut(f->yn_name, sizeof f->yn_name, alm_year_name(a, a->ui.cal_year));
    f->celsius = a->celsius;
    f->show_chain_note = !a->ui.chain_note_hidden;
    f->moon_offset = a->moon_offset;
    f->moon_offset_valid = true;
    f->now_ms = 0;
}

size_t alm_normalize_seed(const char *in, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    size_t n = strlen(in), b, e, adv;
    u8_trim(in, n, &b, &e);
    const unsigned char *u = (const unsigned char *)in;
    for (size_t i = b; i < e; i += adv) {
        uint32_t c = u8_next(u + i, e - i, &adv);
        char enc[4];
        if (c == 0xFFFD && adv == 1) sb_addn(&sb, "\xEF\xBF\xBD", 3);
        else if (c >= 0xD800 && c <= 0xDFFF) sb_addn(&sb, (const char *)u + i, adv);
        else {
            uint32_t up[3];
            int k = to_upper_full(c, up);
            for (int j = 0; j < k; j++) sb_addn(&sb, enc, u8_put(up[j], enc));
        }
    }
    return sb.len;
}

bool alm_regenerate(Almanac *a, int32_t start_abs, const char *seed, double now_ms)
{
    if (!valid_abs(start_abs) || start_abs > IWD_ABS_MAX - IWD_MAX_DAYS || !seed || !*seed) return false;
    char *ns = xstrdup(seed);
    if (!ns) return false;
    bool has_target = a->has_current_abs;
    int32_t target = a->current_abs;
    free(a->seed);
    a->seed = ns;
    a->start_abs = start_abs;
    a->started = now_ms;
    iwd_chain_init(&a->chain, a->seed);
    a->temp_hash = iwd_temp_hash(a->seed);
    a->n_days = 0;
    a->epoch++;
    int64_t i = has_target ? (int64_t)target - start_abs : -1;
    if (i >= 0 && alm_ensure_days(a, (int32_t)(i + 1 > IWD_MAX_DAYS ? IWD_MAX_DAYS : i + 1)) && a->n_days > 0) {
        a->current = a->n_days - 1;
        a->has_current_abs = true;
        a->current_abs = alm_abs_of(a, a->current);
        a->view = a->current;
        a->sel_abs = a->current_abs;
    } else {
        a->n_days = 0;
        a->current = -1;
        a->view = -1;
        a->time = 23;
        if (a->sel_abs < start_abs) a->sel_abs = start_abs;
    }
    show_month_of(a, a->view >= 0 ? a->sel_abs : start_abs);
    return true;
}

AlmApplyResult alm_settings_apply(Almanac *a, const AlmSettingsForm *f, bool confirm_armed)
{
    AlmApplyResult out;
    memset(&out, 0, sizeof out);
    if (a->busy) { out.status = ALM_APPLY_BUSY; return out; }
    SB msg;
    sb_init(&msg, out.message, sizeof out.message);

    /* readStart */
    const AlmStartOption *opt = alm_start_option(f->start_option);
    bool fest = opt && opt->festival != IWD_FEST_NONE;
    int32_t abs = 0;
    if (!f->start_year_valid) { sb_adds(&msg, "Enter a year."); out.status = ALM_APPLY_DATE_ERROR; return out; }
    if (opt && !fest && !(f->start_day_valid && f->start_day >= 1 && f->start_day <= 30)) {
        sb_adds(&msg, "Months have 30 days: pick a day from 1 to 30.");
        out.status = ALM_APPLY_DATE_ERROR;
        return out;
    }
    if (!opt || !iwd_to_abs(f->start_year, fest ? 0 : opt->month, fest ? 1 : f->start_day,
                            fest ? opt->festival : IWD_FEST_NONE, &abs) ||
        abs > IWD_ABS_MAX - IWD_MAX_DAYS) {
        if (opt && opt->festival == IWD_FEST_SHIELDMEET && f->start_year >= IWD_YEAR_MIN && f->start_year <= IWD_YEAR_MAX &&
            !iwd_is_leap(f->start_year))
            sb_addf(&msg, "Shieldmeet only falls in leap years; %ld DR is not one.", (long)f->start_year);
        else
            sb_adds(&msg, "That date does not exist.");
        out.status = ALM_APPLY_DATE_ERROR;
        return out;
    }

    char seed[3 * sizeof f->seed + IWD_RANDOM_SEED_CAP];   /* upper-casing can triple the bytes (e.g. U+0390) */
    alm_normalize_seed(f->seed, seed, sizeof seed);
    if (!seed[0]) alm_random_seed(a, seed, sizeof seed);
    bool reroll = abs != a->start_abs || strcmp(seed, a->seed) != 0;

    if (reroll && a->n_days > 0 && !confirm_armed) {
        char ds[IWD_DATE_CAP], ts[IWD_DATE_CAP];
        fmt_date(abs, false, ds);
        sb_adds(&msg, "<b>Regenerate the weather?</b> All recorded weather is replaced and rolled again from <b>");
        sb_add_esc(&msg, ds);
        sb_adds(&msg, "</b> with seed <code>");
        sb_add_esc(&msg, seed);
        sb_adds(&msg, "</code>");
        if (a->has_current_abs && a->current_abs >= abs) {
            fmt_date(a->current_abs, false, ts);
            sb_adds(&msg, ", up to the party's current day, <b>");
            sb_add_esc(&msg, ts);
            sb_adds(&msg, "</b>.");
        } else {
            sb_adds(&msg, ". The party's current day falls before the new start, so the expedition will begin on <b>");
            sb_add_esc(&msg, ds);
            sb_adds(&msg, "</b>.");
        }
        sb_adds(&msg, " Notes and events are kept.");
        out.status = ALM_APPLY_NEEDS_CONFIRM;
        return out;
    }

    a->celsius = f->celsius;
    a->ui.chain_note_hidden = !f->show_chain_note;
    double off = f->moon_offset;
    a->moon_offset = (f->moon_offset_valid && isfinite(off))
        ? (off < ALM_MOON_OFFSET_MIN ? ALM_MOON_OFFSET_MIN : off > ALM_MOON_OFFSET_MAX ? ALM_MOON_OFFSET_MAX : off)
        : 0;
    if (f->yn_year_valid && f->yn_year >= IWD_YEAR_MIN && f->yn_year <= IWD_YEAR_MAX)
        alm_year_name_set(a, f->yn_year, f->yn_name);
    if (reroll && !alm_regenerate(a, abs, seed, f->now_ms)) {
        out.status = ALM_APPLY_DATE_ERROR;
        sb_adds(&msg, "That date does not exist.");
        return out;
    }
    out.status = ALM_APPLY_DONE;
    out.result.fx = ALM_FX_RENDER_ALL | ALM_FX_SAVE;
    if (reroll) {
        char ds[IWD_DATE_CAP];
        fmt_date(a->start_abs, false, ds);
        SB t;
        sb_init(&t, out.result.toast, sizeof out.result.toast);
        sb_adds(&t, "Weather regenerated from <b>");
        sb_add_esc(&t, ds);
        sb_adds(&t, "</b> \xC2\xB7 seed ");
        sb_add_esc(&t, a->seed);
        out.result.fx |= ALM_FX_TOAST | ALM_FX_ANNOUNCE;
    }
    return out;
}

/* ================================================================================
 * Events, notes, year names
 * ============================================================================== */

const AlmEvent *alm_event_find(const Almanac *a, const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < a->n_events; i++)
        if (strcmp(a->events[i].id, id) == 0) return &a->events[i];
    return NULL;
}

AlmResult alm_event_save(Almanac *a, const char *id, int32_t abs, const char *title,
                         AlmEvType type, IwdRepeat repeat, const char *details,
                         double now_ms, bool *was_update)
{
    if (was_update) *was_update = false;
    if (!title) return rejected();
    int idx = -1;
    if (id && *id)
        for (int i = 0; i < a->n_events; i++)
            if (strcmp(a->events[i].id, id) == 0) { idx = i; break; }
    if (idx < 0 && !valid_abs(abs)) return rejected();
    AlmEvent e;
    memset(&e, 0, sizeof e);
    e.title = dup_trim_cut(title, ALM_TITLE_MAX_UNITS);
    if (!e.title) return rejected();
    if (!e.title[0]) { free(e.title); return rejected(); }
    e.details = dup_trim_cut(details ? details : "", ALM_DETAILS_MAX_UNITS);
    e.type = (unsigned)type < ALM_EV_TYPE_COUNT ? type : ALM_EV_NATURAL;
    e.repeat = (unsigned)repeat < IWD_REPEAT_COUNT ? repeat : IWD_REPEAT_NONE;
    if (idx >= 0) {
        e.id = xstrdup(a->events[idx].id);
        e.abs = a->events[idx].abs;          /* edits keep their anchor (web) */
    } else {
        char t[40], p1[16], p2[16];
        double ms = now_ms > 0 && now_ms < 9e15 ? floor(now_ms) : 0;
        base36((uint64_t)ms, p1, sizeof p1);
        base36((uint64_t)floor((alm_rand_u32(a) / 4294967296.0) * 1e6), p2, sizeof p2);
        snprintf(t, sizeof t, "e%s%s", p1, p2);
        /* keep ids unique even with a frozen clock */
        while (alm_event_find(a, t)) {
            base36(alm_rand_u32(a) % 1000000u, p2, sizeof p2);
            snprintf(t, sizeof t, "e%s%s", p1, p2);
        }
        e.id = xstrdup(t);
        e.abs = abs;
    }
    if (!e.id || !e.details) { event_free(&e); return rejected(); }
    AlmResult r = res0();
    if (idx >= 0) {
        event_free(&a->events[idx]);
        a->events[idx] = e;
        if (was_update) *was_update = true;
        res_toast(&r, "", e.title, " updated");
    } else {
        if (a->n_events >= ALM_MAX_EVENTS) {
            event_free(&e);
            r = rejected();
            SB sb;
            sb_init(&sb, r.toast, sizeof r.toast);
            sb_addf(&sb, "The almanac holds at most %d events.", ALM_MAX_EVENTS);
            r.fx |= ALM_FX_TOAST;
            return r;
        }
        if (a->n_events == a->cap_events) {
            int nc = a->cap_events ? a->cap_events * 2 : 16;
            AlmEvent *ne = (AlmEvent *)realloc(a->events, (size_t)nc * sizeof *ne);
            if (!ne) { event_free(&e); return rejected(); }
            a->events = ne;
            a->cap_events = nc;
        }
        a->events[a->n_events++] = e;
        res_toast(&r, "", e.title, " added to the almanac");
    }
    r.fx |= ALM_FX_RENDER_CAL | ALM_FX_RENDER_DAY | ALM_FX_SAVE;
    return r;
}

AlmResult alm_event_delete(Almanac *a, const char *id)
{
    if (!id) return rejected();
    char *key = xstrdup(id);             /* id may point into the event being removed */
    if (!key) return rejected();
    id = key;
    AlmResult r = res0();
    char *title = NULL;
    int w = 0;
    for (int i = 0; i < a->n_events; i++) {
        if (strcmp(a->events[i].id, id) == 0) {
            if (!title) { title = a->events[i].title; a->events[i].title = NULL; }
            event_free(&a->events[i]);
        } else {
            a->events[w++] = a->events[i];
        }
    }
    a->n_events = w;
    r.fx = ALM_FX_RENDER_CAL | ALM_FX_RENDER_DAY | ALM_FX_SAVE;
    if (title) { res_toast(&r, "", title, " removed"); free(title); }
    free(key);
    return r;
}

typedef struct EvRef { const AlmEvent *e; int prio; int32_t abs; int idx; } EvRef;
static int cmp_evref(const void *pa, const void *pb)
{
    const EvRef *a = (const EvRef *)pa, *b = (const EvRef *)pb;
    if (a->prio != b->prio) return a->prio < b->prio ? -1 : 1;
    if (a->abs != b->abs) return a->abs < b->abs ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

int alm_events_on(const Almanac *a, int32_t abs, const AlmEvent **out, int cap)
{
    EvRef stack[64];
    EvRef *refs = stack;
    int n = 0, cap_refs = 64;
    for (int i = 0; i < a->n_events; i++) {
        const AlmEvent *e = &a->events[i];
        if (!iwd_occurs_on(e->abs, e->repeat, abs, a->moon_offset)) continue;
        if (n == cap_refs) {
            int nc = cap_refs * 2;
            EvRef *nr = (EvRef *)malloc((size_t)nc * sizeof *nr);
            if (!nr) break;
            memcpy(nr, refs, (size_t)n * sizeof *nr);
            if (refs != stack) free(refs);
            refs = nr;
            cap_refs = nc;
        }
        refs[n].e = e; refs[n].prio = alm_ev_type_priority(e->type); refs[n].abs = e->abs; refs[n].idx = i;
        n++;
    }
    if (n > 1) qsort(refs, (size_t)n, sizeof *refs, cmp_evref);
    for (int i = 0; out && i < n && i < cap; i++) out[i] = refs[i].e;
    if (refs != stack) free(refs);
    return n;
}

int alm_marks_on(const Almanac *a, int32_t abs, AlmMark *out, int cap)
{
    IwdMoon m = iwd_moon(abs, a->moon_offset);
    int k = 0;
    if (m.is_new) {
        if (out && k < cap) {
            out[k].builtin = true; out[k].event = NULL; out[k].type = ALM_EV_AURIL;
            out[k].title = ALM_NEW_MOON_TITLE; out[k].chip = ALM_NEW_MOON_SHORT;
        }
        k++;
    }
    int room = (out && cap > k) ? cap - k : 0;
    const AlmEvent *stack[32];
    const AlmEvent **evs = stack;
    if (room > 32) {
        evs = (const AlmEvent **)malloc((size_t)room * sizeof *evs);
        if (!evs) { evs = stack; room = 32; }
    }
    int n = alm_events_on(a, abs, room ? evs : NULL, room);
    for (int i = 0; i < n && i < room; i++) {
        AlmMark *mk = &out[k + i];
        mk->builtin = false; mk->event = evs[i]; mk->type = evs[i]->type;
        mk->title = evs[i]->title; mk->chip = evs[i]->title;
    }
    if (evs != stack) free((void *)evs);
    return k + n;
}

size_t alm_fmt_mark_meta(const AlmMark *m, int32_t shown_abs, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    if (m->builtin || !m->event) { sb_adds(&sb, ALM_NEW_MOON_META); return sb.len; }
    const AlmEvent *e = m->event;
    sb_adds(&sb, alm_ev_type_label(e->type));
    sb_adds(&sb, " \xC2\xB7 ");
    sb_adds(&sb, e->repeat == IWD_REPEAT_NONE ? "One-time" : iwd_repeat_label(e->repeat));
    if (e->repeat != IWD_REPEAT_NONE && e->abs != shown_abs) {
        char d[IWD_DATE_CAP];
        fmt_date(e->abs, true, d);
        sb_adds(&sb, " \xC2\xB7 since ");
        sb_adds(&sb, d);
        sb_addf(&sb, " %ld", (long)iwd_from_abs(e->abs).year);
    }
    return sb.len;
}

AlmResult alm_note_set(Almanac *a, int32_t abs, const char *text)
{
    if (!valid_abs(abs)) return rejected();
    AlmResult r = res0();
    size_t n = text ? strlen(text) : 0;
    if (text && !u8_blank(text, n)) {
        char *t = xstrndup(text, u8_cut_bytes(text, n, ALM_MAX_NOTE_BYTES));
        if (!t || !map_put(&a->notes, abs, t)) { free(t); return rejected(); }
    } else {
        map_del(&a->notes, abs);
    }
    r.fx = ALM_FX_RENDER_CAL | ALM_FX_SAVE;
    return r;
}

const char *alm_note_get(const Almanac *a, int32_t abs)
{
    const char *s = alm_map_get(&a->notes, abs);
    return s ? s : "";
}

bool alm_year_name_set(Almanac *a, int32_t year, const char *name)
{
    if (year < IWD_YEAR_MIN || year > IWD_YEAR_MAX) return false;
    char *t = dup_trim_cut(name ? name : "", ALM_YEARNAME_MAX_UNITS);
    if (!t) return false;
    if (!t[0]) { free(t); map_del(&a->year_names, year); return true; }
    if (!map_put(&a->year_names, year, t)) { free(t); return false; }
    return true;
}

/* ================================================================================
 * Calendar presentation
 * ============================================================================== */

static void fill_cell(const Almanac *a, int32_t abs, IwdFestival fest, AlmCell *c)
{
    memset(c, 0, sizeof *c);
    c->abs = abs;
    c->date = iwd_from_abs(abs);
    c->moon = iwd_moon(abs, a->moon_offset);
    c->is_festival = fest != IWD_FEST_NONE;
    if (c->is_festival) { const IwdFestivalDef *fd = iwd_festival(fest); c->festival_name = fd ? fd->name : ""; }
    int64_t i = (int64_t)abs - a->start_abs;
    int32_t t;
    c->is_today = alm_today_abs(a, &t) && t == abs;
    c->is_sel = abs == a->sel_abs;
    c->is_pre = i < 0;
    c->is_start = i == 0;
    c->is_new_moon = c->moon.is_new;
    c->has_note = alm_map_get(&a->notes, abs) != NULL;
    const IwdDay *d = alm_weather_of(a, abs);
    if (d) {
        c->has_weather = true;
        c->dominant_id = iwd_day_dominant(d);
        c->temp_min_f = d->temp_min;
    }
    const AlmEvent *evs[2];
    c->n_events = alm_events_on(a, abs, evs, 2);
    for (int k = 0; k < 2 && k < c->n_events; k++) c->chips[k] = evs[k];
    if (c->n_events > 0) { c->has_top_event = true; c->top_type = evs[0]->type; }
    c->tag = c->is_today ? "Today" : c->is_start ? "Start" : NULL;
}

int alm_month_cells(const Almanac *a, AlmCell out[32])
{
    IwdMonthLayout L;
    iwd_month_layout(a->ui.cal_year, a->ui.cal_month, &L);
    int k = 0;
    for (int d = 0; d < 30; d++) fill_cell(a, L.first_abs + d, IWD_FEST_NONE, &out[k++]);
    for (int f = 0; f < L.n_festivals && f < 2; f++) fill_cell(a, L.festivals[f].abs, L.festivals[f].festival, &out[k++]);
    return k;
}

static void sb_part(SB *sb, bool *any, const char *s)
{
    if (!s || !*s) return;
    if (*any) sb_adds(sb, " \xC2\xB7 ");
    sb_adds(sb, s);
    *any = true;
}

size_t alm_fmt_cell_label(const Almanac *a, const AlmCell *c, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    bool any = false;
    char d[IWD_DATE_CAP], t[32], wx[96];
    fmt_date(c->abs, false, d);
    sb_part(&sb, &any, d);
    sb_part(&sb, &any, c->moon.name);
    if (c->is_new_moon) sb_part(&sb, &any, "sacrifice to Auril");
    if (c->has_weather) {
        const IwdEventDef *ev = iwd_event(c->dominant_id);
        iwd_fmt_temp(c->temp_min_f, a->celsius, t, sizeof t);
        fmt_into(wx, sizeof wx, "%s, low %s", ev ? ev->name : "", t);
        sb_part(&sb, &any, wx);
    }
    if (c->n_events > 0) {
        const AlmEvent **evs = (const AlmEvent **)malloc((size_t)c->n_events * sizeof *evs);
        if (evs) {
            int n = alm_events_on(a, c->abs, evs, c->n_events);
            if (n > c->n_events) n = c->n_events;
            if (n > 0) {
                if (any) sb_adds(&sb, " \xC2\xB7 ");
                any = true;
                for (int i = 0; i < n; i++) {
                    if (i) sb_adds(&sb, ", ");
                    sb_adds(&sb, evs[i]->title);
                }
            }
            free((void *)evs);
        }
    }
    if (c->has_note) sb_part(&sb, &any, "has notes");
    if (c->is_today) sb_part(&sb, &any, "current day");
    if (c->is_start) sb_part(&sb, &any, "expedition start");
    if (c->is_festival) sb_adds(&sb, ", festival");
    return sb.len;
}

int alm_upcoming(const Almanac *a, AlmUpcoming out[ALM_UPCOMING_DAYS + 1])
{
    int32_t base;
    if (!alm_today_abs(a, &base)) base = a->start_abs;
    int k = 0;
    for (int32_t d = 0; d <= ALM_UPCOMING_DAYS; d++) {
        int64_t abs = (int64_t)base + d;
        if (!valid_abs(abs)) break;
        int n = alm_marks_on(a, (int32_t)abs, NULL, 0);
        if (n > 0) { out[k].abs = (int32_t)abs; out[k].rel_days = d; out[k].n_marks = n; k++; }
    }
    return k;
}

static void ascii_lower(char *s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

void alm_day_panel(const Almanac *a, AlmDayPanel *out)
{
    static const char *const ORD[3] = { "First", "Second", "Third" };
    memset(out, 0, sizeof *out);
    int32_t abs = a->sel_abs;
    int64_t i = (int64_t)abs - a->start_abs;
    IwdDate d = iwd_from_abs(abs);
    IwdMoon m = iwd_moon(abs, a->moon_offset);
    int32_t t;
    bool has_t = alm_today_abs(a, &t);
    const IwdDay *day = alm_weather_of(a, abs);
    SB sb;

    sb_init(&sb, out->eyebrow, sizeof out->eyebrow);
    if (i < 0) sb_adds(&sb, "Before the expedition");
    else if (has_t && abs == t) sb_addf(&sb, "Today \xC2\xB7 Day %lld of the expedition", (long long)(i + 1));
    else {
        sb_addf(&sb, "Day %lld of the expedition", (long long)(i + 1));
        if (has_t) {
            char rel[48];
            iwd_rel_days((int32_t)((int64_t)abs - t), rel, sizeof rel);
            ascii_lower(rel);
            sb_adds(&sb, " \xC2\xB7 ");
            sb_adds(&sb, rel);
        }
    }
    fmt_date(abs, false, out->date);

    const char *yn = alm_map_get(&a->year_names, d.year);
    sb_init(&sb, out->sub, sizeof out->sub);
    if (d.festival != IWD_FEST_NONE) {
        const IwdFestivalDef *fd = iwd_festival(d.festival);
        const IwdMonth *mo = iwd_month(fd ? fd->after : 0);
        sb_adds(&sb, "Festival day \xC2\xB7 after ");
        sb_adds(&sb, mo ? mo->name : "");
    } else {
        const IwdMonth *mo = iwd_month(d.month);
        sb_addf(&sb, "%s \xE2\x80\x94 %s \xC2\xB7 %s tenday, day %d", mo ? mo->name : "", mo ? mo->common : "",
                ORD[d.tenday_index >= 0 && d.tenday_index < 3 ? d.tenday_index : 0], d.day_of_tenday);
    }
    if (yn) { sb_adds(&sb, " \xC2\xB7 "); sb_adds(&sb, yn); }

    sb_init(&sb, out->moon_name, sizeof out->moon_name);
    sb_adds(&sb, "Sel\xC3\xBBne \xC2\xB7 ");
    sb_adds(&sb, m.name);
    out->moon_is_new = m.is_new;
    sb_init(&sb, out->moon_sub, sizeof out->moon_sub);
    if (m.is_new) sb_adds(&sb, "New moon \xE2\x80\x94 the sacrifice to Auril is due");
    else sb_addf(&sb, "%d%% lit \xC2\xB7 %s", iwd_moon_percent(&m), m.waxing ? "waxing" : "waning");

    sb_init(&sb, out->weather_none, sizeof out->weather_none);
    if (i < 0) {
        char sd[IWD_DATE_CAP];
        fmt_date(a->start_abs, false, sd);
        sb_adds(&sb, "No weather \xE2\x80\x94 the expedition begins on ");
        sb_adds(&sb, sd);
        sb_adds(&sb, ".");
    } else if (!day) {
        if (i >= IWD_MAX_DAYS) sb_addf(&sb, "Beyond the almanac: at most %d days can be recorded.", IWD_MAX_DAYS);
        else sb_adds(&sb, "Weather not rolled yet.");
    } else {
        out->weather = day;
        out->dominant_id = iwd_day_dominant(day);
        int seq[IWD_MAX_SEGMENTS], ns = 0;
        for (int s = 0; s < day->n_segments; s++)
            if (ns == 0 || seq[ns - 1] != day->segments[s].id) seq[ns++] = day->segments[s].id;
        sb_init(&sb, out->wx_seq, sizeof out->wx_seq);
        for (int s = 0; s < ns && s < 4; s++) {
            const IwdEventDef *ev = iwd_event(seq[s]);
            if (s) sb_adds(&sb, " \xE2\x86\x92 ");
            sb_adds(&sb, ev ? ev->name : "");
        }
        if (ns > 4) sb_adds(&sb, " \xE2\x80\xA6");
        char lo[32], hi[32];
        iwd_fmt_temp(day->temp_min, a->celsius, lo, sizeof lo);
        iwd_fmt_temp(day->temp_max, a->celsius, hi, sizeof hi);
        fmt_into(out->wx_temp, sizeof out->wx_temp, "%s to %s%s", lo, hi, a->celsius ? "C" : "F");
        IwdCold cl = iwd_cold_dc(day->temp_max), ch = iwd_cold_dc(day->temp_min);
        sb_init(&sb, out->wx_cold, sizeof out->wx_cold);
        if (cl.dc == ch.dc) sb_addf(&sb, "DC %d", ch.dc);
        else sb_addf(&sb, "DC %d\xE2\x80\x93%d", cl.dc, ch.dc);
        sb_adds(&sb, " \xC2\xB7 ");
        if (cl.tier == ch.tier) sb_adds(&sb, ch.tier->name);
        else { sb_adds(&sb, cl.tier->name); sb_adds(&sb, " \xE2\x86\x92 "); sb_adds(&sb, ch.tier->name); }
    }
    out->show_set_current = !(i < 0 || i >= IWD_MAX_DAYS);
    out->set_current_disabled = has_t && abs == t;
    out->set_current_label = out->set_current_disabled ? "This is the current day" : "Set as current day";
}

/* ================================================================================
 * Hero / controls / announcements
 * ============================================================================== */

void alm_hero(const Almanac *a, AlmHero *out)
{
    memset(out, 0, sizeof *out);
    int32_t abs = a->view >= 0 ? alm_abs_of(a, a->view) : a->start_abs;
    out->abs = abs;
    fmt_date(abs, false, out->date);
    out->moon = iwd_moon(abs, a->moon_offset);
    int32_t t;
    if (a->view < 0) {
        out->tag_kind = ALM_TAG_START;
        fmt_into(out->tag, sizeof out->tag, "Expedition start");
        fmt_into(out->day_label, sizeof out->day_label, "Before the First Dawn");
        return;
    }
    fmt_into(out->day_label, sizeof out->day_label, "Day %ld of the expedition", (long)a->view + 1);
    if (alm_today_abs(a, &t)) {
        if (abs == t) { out->tag_kind = ALM_TAG_TODAY; fmt_into(out->tag, sizeof out->tag, "Today"); }
        else {
            out->tag_kind = abs > t ? ALM_TAG_AHEAD : ALM_TAG_PAST;
            iwd_rel_days((int32_t)((int64_t)abs - t), out->tag, sizeof out->tag);
        }
    }
}

void alm_controls(const Almanac *a, AlmControls *out)
{
    memset(out, 0, sizeof *out);
    char d[IWD_DATE_CAP];
    if (a->current < 0) {
        fmt_date(a->start_abs, false, d);
        fmt_into(out->main, sizeof out->main, "Begin the Expedition");
        out->advance_enabled = !a->busy;
    } else if (a->current + 1 >= IWD_MAX_DAYS) {
        fmt_into(out->main, sizeof out->main, "The Almanac Is Full");
        out->advance_enabled = false;
    } else {
        fmt_date(alm_abs_of(a, a->current + 1), true, d);
        fmt_into(out->main, sizeof out->main, "Advance to %s", d);
        out->advance_enabled = !a->busy;
    }
    int32_t v = a->view;
    out->prev_enabled = !a->busy && v > 0;
    out->next_enabled = !a->busy && v >= 0 && v + 1 < IWD_MAX_DAYS;
}

size_t alm_fmt_announce(const Almanac *a, char *buf, size_t cap)
{
    SB sb;
    sb_init(&sb, buf, cap);
    if (!alm_has_view(a)) return 0;
    const IwdDay *day = &a->days[a->view];
    int h = alm_hour_now(a);
    const IwdEventDef *ev = iwd_event(day->hour_id[h]);
    if (!ev) return 0;
    char d[IWD_DATE_CAP], hr[8], tv[24];
    fmt_date(alm_abs_of(a, a->view), false, d);
    iwd_fmt_hour(h, hr, sizeof hr);
    int tf = day->temp_f[h];
    iwd_fmt_minus(a->celsius ? iwd_f_to_c(tf) : tf, tv, sizeof tv);
    IwdCold cold = iwd_cold_dc(tf);
    const IwdWind *w = iwd_wind(ev->wind);
    sb_addf(&sb, "%s, day %ld, %s: %s, %s\xC2\xB0%s, %s DC %d, %s", d, (long)day->day, hr, ev->name, tv,
            a->celsius ? "C" : "F", cold.tier->name, cold.dc, w ? w->label : "");
    return sb.len;
}

bool alm_day_toast(const Almanac *a, int32_t abs, const char *lead,
                   char *title, size_t title_cap, AlmMark *marks, int cap, int *n_marks)
{
    int n = alm_marks_on(a, abs, marks, cap);
    if (n_marks) *n_marks = n < cap ? n : cap;
    if (n == 0) { if (title && title_cap) title[0] = 0; return false; }
    if (title && title_cap) {
        SB sb;
        char d[IWD_DATE_CAP];
        sb_init(&sb, title, title_cap);
        fmt_date(abs, false, d);
        sb_adds(&sb, lead ? lead : "");
        sb_adds(&sb, " \xC2\xB7 ");
        sb_adds(&sb, d);
    }
    return true;
}
