/* engine.c - Icewind Dale weather engine + Calendar of Harptos (pure C11).
 * Port of js/engine.js and js/harptos.js (reference copies in native/test/reference/),
 * plus the small pure presentation helpers of app.js / calendar.js / scene.js that
 * engine.h declares. See engine.h for the JS-semantics rules this file follows.
 * Build with -ffp-contract=off (never -ffast-math). */
#include "engine.h"

#include <math.h>
#include <string.h>

/* ================================================================================
 * Small internal helpers
 * ============================================================================== */

typedef struct Out {
    char  *buf;
    size_t cap;
    size_t len;
    bool   full;
} Out;

static Out out_begin(char *buf, size_t cap)
{
    Out o = { buf, cap, 0, false };
    if (buf && cap) buf[0] = '\0';
    return o;
}

/* Appends n bytes, stopping at a UTF-8 character boundary when out of room. */
static void out_put(Out *o, const char *s, size_t n)
{
    if (o->full || !o->buf || o->cap == 0) { o->full = true; return; }
    size_t room = o->cap - 1 - o->len;
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        o->full = true;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }

/* Decimal digits of |v| (no sign) into tmp, returns the length. No printf: msvcrt's
 * printf family does not reliably support %lld, and the engine needs no stdio. */
static size_t fmt_udec(char *tmp, unsigned long long u)
{
    char rev[24];
    size_t n = 0;
    do { rev[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    for (size_t i = 0; i < n; i++) tmp[i] = rev[n - 1 - i];
    return n;
}

static unsigned long long abs_ull(long long v)
{
    return v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
}

static void out_int(Out *o, long long v)
{
    char tmp[24];
    if (v < 0) out_str(o, "-");
    out_put(o, tmp, fmt_udec(tmp, abs_ull(v)));
}

/* minus(n): String(n).replace('-', '\u2212') */
static void out_minus(Out *o, long long v)
{
    char tmp[24];
    if (v < 0) out_str(o, "\xE2\x88\x92");
    out_put(o, tmp, fmt_udec(tmp, abs_ull(v)));
}

/* String(n).padStart(2, '0') */
static void out_pad2(Out *o, long long v)
{
    char tmp[24];
    size_t n = fmt_udec(tmp, abs_ull(v));
    if (v < 0) { out_str(o, "-"); n++; }
    if (n < 2) out_str(o, "0");
    out_put(o, tmp, v < 0 ? n - 1 : n);
}

static size_t out_end(Out *o) { return o->len; }

/* isfinite()/isnan() without the macros (mingw's expand to float conversions under
 * -Wconversion). Valid because the engine is never built with -ffast-math. */
static bool dfinite(double x) { return x - x == 0; }
static bool dnan(double x) { return x != x; }

/* ToInt32 of a finite double (x | 0). */
static uint32_t js_to_uint32(double x)
{
    if (!dfinite(x)) return 0;
    double t = trunc(x);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}

/* ================================================================================
 * JS-semantics helpers
 * ============================================================================== */

double iwd_js_round(double x)
{
    if (!dfinite(x)) return x;
    double f = floor(x);
    double r = (x - f >= 0.5) ? f + 1.0 : f;
    return r == 0 ? copysign(0.0, x) : r;    /* Math.round(-0.3) is -0 */
}

int64_t iwd_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

int64_t iwd_mod(int64_t a, int64_t n)
{
    int64_t r = a % n;
    return r < 0 ? r + n : r;
}

/* JS mod on doubles: ((a % n) + n) % n */
static double js_dmod(double a, double n) { return fmod(fmod(a, n) + n, n); }

/* ================================================================================
 * Math.cos, bit for bit: V8's Math.cos is fdlibm's cos (v8/src/base/ieee754.cc), so this
 * is a straight port of fdlibm 5.3 __kernel_cos / __kernel_sin / __ieee754_rem_pio2 for
 * |x| <= 2^19 * pi/2. Platform cos() (glibc, msvcrt, UCRT) can differ in the last bit,
 * which would make temperatures and moon fractions platform dependent. Larger |x| (never
 * used by the app) falls back to libm.
 * ============================================================================== */

static double dbl_from_words(uint32_t hi, uint32_t lo)
{
    uint64_t b = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static uint32_t dbl_high_word(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return (uint32_t)(b >> 32);
}

static double fdl_kernel_cos(double x, double y)
{
    const double C1 = dbl_from_words(0x3FA55555u, 0x5555554Cu);
    const double C2 = dbl_from_words(0xBF56C16Cu, 0x16C15177u);
    const double C3 = dbl_from_words(0x3EFA01A0u, 0x19CB1590u);
    const double C4 = dbl_from_words(0xBE927E4Fu, 0x809C52ADu);
    const double C5 = dbl_from_words(0x3E21EE9Eu, 0xBDB4B1C4u);
    const double C6 = dbl_from_words(0xBDA8FAE9u, 0xBE8838D4u);
    uint32_t ix = dbl_high_word(x) & 0x7fffffffu;
    if (ix < 0x3e400000u) {                 /* |x| < 2^-27 */
        if ((int)x == 0) return 1.0;
    }
    double z = x * x;
    double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333u) return 1.0 - (0.5 * z - (z * r - x * y));
    double qx;
    if (ix > 0x3fe90000u) qx = 0.28125;
    else qx = dbl_from_words(ix - 0x00200000u, 0);
    double hz = 0.5 * z - qx;
    double a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}

static double fdl_kernel_sin(double x, double y, int iy)
{
    const double S1 = dbl_from_words(0xBFC55555u, 0x55555549u);
    const double S2 = dbl_from_words(0x3F811111u, 0x1110F8A6u);
    const double S3 = dbl_from_words(0xBF2A01A0u, 0x19C161D5u);
    const double S4 = dbl_from_words(0x3EC71DE3u, 0x57B1FE7Du);
    const double S5 = dbl_from_words(0xBE5AE5E6u, 0x8A2B9CEBu);
    const double S6 = dbl_from_words(0x3DE5D93Au, 0x5ACFD57Cu);
    uint32_t ix = dbl_high_word(x) & 0x7fffffffu;
    if (ix < 0x3e400000u) {
        if ((int)x == 0) return x;
    }
    double z = x * x;
    double v = z * x;
    double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static const uint32_t NPIO2_HW[32] = {
    0x3FF921FBu, 0x400921FBu, 0x4012D97Cu, 0x401921FBu, 0x401F6A7Au, 0x4022D97Cu, 0x4025FDBBu, 0x402921FBu,
    0x402C463Au, 0x402F6A7Au, 0x4031475Cu, 0x4032D97Cu, 0x40346B9Cu, 0x4035FDBBu, 0x40378FDBu, 0x403921FBu,
    0x403AB41Bu, 0x403C463Au, 0x403DD85Au, 0x403F6A7Au, 0x40407E4Cu, 0x4041475Cu, 0x4042106Cu, 0x4042D97Cu,
    0x4043A28Cu, 0x40446B9Cu, 0x404534ACu, 0x4045FDBBu, 0x4046C6CBu, 0x40478FDBu, 0x404858EBu, 0x404921FBu,
};

/* Returns n (quadrant) and x - n*pi/2 as y[0] + y[1]; only for |x| <= 2^19*pi/2. */
static int fdl_rem_pio2(double x, double *y)
{
    const double invpio2 = dbl_from_words(0x3FE45F30u, 0x6DC9C883u);
    const double pio2_1 = dbl_from_words(0x3FF921FBu, 0x54400000u);
    const double pio2_1t = dbl_from_words(0x3DD0B461u, 0x1A626331u);
    const double pio2_2 = dbl_from_words(0x3DD0B461u, 0x1A600000u);
    const double pio2_2t = dbl_from_words(0x3BA3198Au, 0x2E037073u);
    const double pio2_3 = dbl_from_words(0x3BA3198Au, 0x2E000000u);
    const double pio2_3t = dbl_from_words(0x397B839Au, 0x252049C1u);
    uint32_t hxw = dbl_high_word(x);
    bool neg = (hxw >> 31) != 0;
    uint32_t ix = hxw & 0x7fffffffu;
    double z, w, t, r, fn;
    if (ix <= 0x3fe921fbu) { y[0] = x; y[1] = 0; return 0; }
    if (ix < 0x4002d97cu) {                 /* |x| < 3pi/4 */
        if (!neg) {
            z = x - pio2_1;
            if (ix != 0x3ff921fbu) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        z = x + pio2_1;
        if (ix != 0x3ff921fbu) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        } else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1;
    }
    /* medium size (caller guarantees ix <= 0x413921fb) */
    t = fabs(x);
    int n = (int)(t * invpio2 + 0.5);
    fn = (double)n;
    r = t - fn * pio2_1;
    w = fn * pio2_1t;
    if (n < 32 && ix != NPIO2_HW[n - 1]) {
        y[0] = r - w;
    } else {
        int j = (int)(ix >> 20);
        y[0] = r - w;
        int i = j - (int)((dbl_high_word(y[0]) >> 20) & 0x7ff);
        if (i > 16) {
            t = r;
            w = fn * pio2_2;
            r = t - w;
            w = fn * pio2_2t - ((t - r) - w);
            y[0] = r - w;
            i = j - (int)((dbl_high_word(y[0]) >> 20) & 0x7ff);
            if (i > 49) {
                t = r;
                w = fn * pio2_3;
                r = t - w;
                w = fn * pio2_3t - ((t - r) - w);
                y[0] = r - w;
            }
        }
    }
    y[1] = (r - y[0]) - w;
    if (neg) { y[0] = -y[0]; y[1] = -y[1]; return -n; }
    return n;
}

double iwd_js_cos(double x)
{
    uint32_t ix = dbl_high_word(x) & 0x7fffffffu;
    if (ix <= 0x3fe921fbu) return fdl_kernel_cos(x, 0.0);
    if (ix >= 0x7ff00000u) return x - x;    /* NaN */
    if (ix > 0x413921fbu) return cos(x);   /* huge argument: not used by the app */
    double y[2];
    int n = fdl_rem_pio2(x, y);
    switch (n & 3) {
    case 0:  return fdl_kernel_cos(y[0], y[1]);
    case 1:  return -fdl_kernel_sin(y[0], y[1], 1);
    case 2:  return -fdl_kernel_cos(y[0], y[1]);
    default: return fdl_kernel_sin(y[0], y[1], 1);
    }
}

/* ================================================================================
 * UTF-8 -> UTF-16 (WHATWG decoder: each maximal invalid subpart becomes one U+FFFD,
 * the same thing node's Buffer#toString('utf8') and TextDecoder do)
 * ============================================================================== */

typedef struct U8Dec {
    const unsigned char *p;
    uint16_t pending;        /* low surrogate waiting to be emitted, or 0 */
} U8Dec;

/* Next code point, or -1 at the end. */
static int32_t u8_next_cp(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned b = *p;
    if (b == 0) return -1;
    if (b < 0x80) { *pp = p + 1; return (int32_t)b; }
    int need;
    uint32_t cp;
    unsigned lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) {
        need = 2; cp = b & 0x0F;
        if (b == 0xE0) lo = 0xA0;
        if (b == 0xED) hi = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        need = 3; cp = b & 0x07;
        if (b == 0xF0) lo = 0x90;
        if (b == 0xF4) hi = 0x8F;
    } else { *pp = p + 1; return 0xFFFD; }
    p++;
    for (int i = 0; i < need; i++) {
        unsigned c = *p;
        if (c < lo || c > hi) { *pp = p; return 0xFFFD; }   /* reprocess c */
        lo = 0x80; hi = 0xBF;
        cp = (cp << 6) | (c & 0x3F);
        p++;
    }
    *pp = p;
    return (int32_t)cp;
}

/* Next UTF-16 unit, or -1 at the end. */
static int32_t u8_next_unit(U8Dec *d)
{
    if (d->pending) { int32_t u = d->pending; d->pending = 0; return u; }
    int32_t cp = u8_next_cp(&d->p);
    if (cp < 0) return -1;
    if (cp >= 0x10000) {
        uint32_t v = (uint32_t)cp - 0x10000;
        d->pending = (uint16_t)(0xDC00 + (v & 0x3FF));
        return (int32_t)(0xD800 + (v >> 10));
    }
    return cp;
}

static size_t u8_count_units(const char *s)
{
    U8Dec d = { (const unsigned char *)s, 0 };
    size_t n = 0;
    while (u8_next_unit(&d) >= 0) n++;
    return n;
}

size_t iwd_utf8_to_utf16(const char *utf8, uint16_t *out, size_t cap)
{
    U8Dec d = { (const unsigned char *)(utf8 ? utf8 : ""), 0 };
    size_t n = 0;
    int32_t u;
    while ((u = u8_next_unit(&d)) >= 0) {
        if (out && n < cap) out[n] = (uint16_t)u;
        n++;
    }
    return n;
}

/* ================================================================================
 * Seeded RNG
 * ============================================================================== */

typedef struct Cyrb { uint32_t h1, h2; } Cyrb;

static Cyrb cyrb_begin(uint32_t len)
{
    Cyrb c = { 0xdeadbeefu ^ len, 0x41c6ce57u ^ len };
    return c;
}

static void cyrb_unit(Cyrb *c, uint32_t ch)
{
    c->h1 = (c->h1 ^ ch) * 2654435761u;
    c->h2 = (c->h2 ^ ch) * 1597334677u;
}

static uint32_t cyrb_end(Cyrb *c)
{
    uint32_t h1 = c->h1, h2 = c->h2;
    h1 = ((h1 ^ (h1 >> 16)) * 2246822507u) ^ ((h2 ^ (h2 >> 13)) * 3266489909u);
    h2 = ((h2 ^ (h2 >> 16)) * 2246822507u) ^ ((h1 ^ (h1 >> 13)) * 3266489909u);
    return h1 ^ h2;
}

static uint32_t hash_utf8_suffix(const char *utf8, const char *ascii_suffix)
{
    if (!utf8) utf8 = "";
    size_t slen = ascii_suffix ? strlen(ascii_suffix) : 0;
    size_t len = u8_count_units(utf8) + slen;
    Cyrb c = cyrb_begin((uint32_t)len);
    U8Dec d = { (const unsigned char *)utf8, 0 };
    int32_t u;
    while ((u = u8_next_unit(&d)) >= 0) cyrb_unit(&c, (uint32_t)u);
    for (size_t i = 0; i < slen; i++) cyrb_unit(&c, (unsigned char)ascii_suffix[i]);
    return cyrb_end(&c);
}

uint32_t iwd_hash_seed(const char *utf8) { return hash_utf8_suffix(utf8, NULL); }

uint32_t iwd_hash_seed_utf16(const uint16_t *units, size_t count)
{
    Cyrb c = cyrb_begin((uint32_t)count);
    for (size_t i = 0; i < count; i++) cyrb_unit(&c, units[i]);
    return cyrb_end(&c);
}

uint32_t iwd_temp_hash(const char *seed_utf8) { return hash_utf8_suffix(seed_utf8, "|temp"); }

void iwd_rng_init(IwdRng *rng, uint32_t state) { rng->state = state; }

uint32_t iwd_rng_next_u32(IwdRng *rng)
{
    rng->state += 0x6d2b79f5u;
    uint32_t t = rng->state;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return t ^ (t >> 14);
}

double iwd_rng_next(IwdRng *rng) { return (double)iwd_rng_next_u32(rng) / 4294967296.0; }

int iwd_rng_die(IwdRng *rng, int sides)
{
    uint32_t u = iwd_rng_next_u32(rng);
    return 1 + (int)(((uint64_t)u * (uint64_t)sides) >> 32);
}

static const char *const SEED_WORDS[16] = {
    "RIME", "FROST", "AURIL", "HOAR", "SLEET", "GLACIER", "TUNDRA", "BOREAL",
    "SPINE", "MAER", "DUN", "CAIRN", "KELVIN", "BREMEN", "TERMALAINE", "ICEFANG",
};

size_t iwd_random_seed(uint32_t r0, uint32_t r1, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    char hex[5];
    unsigned v = (unsigned)(r1 % 0x10000u);
    for (int i = 3; i >= 0; i--) { hex[i] = "0123456789ABCDEF"[v & 15]; v >>= 4; }
    hex[4] = '\0';
    out_str(&o, SEED_WORDS[r0 % 16u]);
    out_str(&o, "-");
    out_str(&o, hex);
    return out_end(&o);
}

/* ================================================================================
 * Rules data
 * ============================================================================== */

static const IwdWind WINDS[IWD_WIND_COUNT] = {
    { "none",     "No Wind",       0 },
    { "mild",     "Mild Wind",     1 },
    { "moderate", "Moderate Wind", 2 },
    { "severe",   "Severe Wind",   3 },
};

const IwdWind *iwd_wind(IwdWindKey key)
{
    if ((int)key < 0 || key >= IWD_WIND_COUNT) return NULL;
    return &WINDS[key];
}

int iwd_wind_meter(IwdWindKey key)
{
    const IwdWind *w = iwd_wind(key);
    if (!w) return 0;
    return w->level + (w->level ? 1 : 0);
}

static const char *const CAT_KEYS[IWD_CAT_COUNT] = {
    "sight", "hearing", "ranged", "attack", "cold", "travel", "flame", "concentration",
    "hazard", "light", "natural", "aurora", "chill",
};
static const char *const CAT_LABELS[IWD_CAT_COUNT] = {
    "Sight", "Hearing", "Ranged attacks", "Attacks", "Extreme cold", "Travel",
    "Extinguishing winds", "Concentration", "Hazard", "Unnatural light", "Natural light",
    "Auril's Aurora", "Cold table",
};

const char *iwd_cat_key(IwdCat cat)
{
    return ((int)cat >= 0 && cat < IWD_CAT_COUNT) ? CAT_KEYS[cat] : "";
}

const char *iwd_cat_label(IwdCat cat)
{
    return ((int)cat >= 0 && cat < IWD_CAT_COUNT) ? CAT_LABELS[cat] : "";
}

/* Shared effect texts (engine.js constants) */
#define T_NOLIGHT1 "No natural light: creatures relying on it are Blinded."
#define T_NOLIGHT2 "No [Sight] Perception checks."
#define T_NOLIGHT3 "Disadvantage on attack rolls."
#define NO_LIGHT \
    { IWD_CAT_SIGHT, T_NOLIGHT1 }, { IWD_CAT_SIGHT, T_NOLIGHT2 }, { IWD_CAT_ATTACK, T_NOLIGHT3 }
#define DIM_SIGHT { IWD_CAT_SIGHT, "Disadvantage on [Sight] Perception checks." }
#define COLD_NO_RESIST { IWD_CAT_COLD, "Extreme cold (creatures without natural cold resistance): DC 10 CON save every hour, +1 exhaustion on a fail." }
#define COLD_EVEN_RESIST \
    { IWD_CAT_COLD, "Extreme cold affects even creatures with natural cold resistance." }, \
    { IWD_CAT_COLD, "With cold resistance: DC 10 CON save every hour, +1 exhaustion on a fail." }, \
    { IWD_CAT_COLD, "Without cold resistance: DC 15 CON save every hour, +1 exhaustion on a fail." }
#define RANGED { IWD_CAT_RANGED, "Disadvantage on ranged attack rolls." }
#define HEARING { IWD_CAT_HEARING, "Disadvantage on [Hearing] Perception checks." }
#define EXTINGUISH { IWD_CAT_FLAME, "Extinguishing winds: open flames, fog, tracks in the snow, and non-magical flying are impossible." }
#define CONCENTRATION { IWD_CAT_CONCENTRATION, "At the end of each turn, concentration spells require a DC 10 CON save to maintain." }

static const IwdEffect FX1[] = { DIM_SIGHT };
static const IwdEffect FX2[] = { DIM_SIGHT };
static const IwdEffect FX3[] = { RANGED, NO_LIGHT };
static const IwdEffect FX4[] = { RANGED, HEARING, NO_LIGHT, COLD_NO_RESIST };
static const IwdEffect FX5[] = { DIM_SIGHT };
static const IwdEffect FX6[] = { NO_LIGHT, COLD_NO_RESIST };
static const IwdEffect FX7[] = {
    RANGED, HEARING,
    { IWD_CAT_HEARING, "Hearing range reduced to 100 ft." },
    NO_LIGHT,
    { IWD_CAT_SIGHT, "Characters without goggles are Blinded even in lit conditions." },
    { IWD_CAT_LIGHT, "Visibility from unnatural light reduced to 30 ft." },
    COLD_NO_RESIST, EXTINGUISH,
    { IWD_CAT_TRAVEL, "Veering off course: the navigator makes a DC 15 Survival check every hour while travelling." },
    { IWD_CAT_TRAVEL, "Fail with 14\xE2\x80\x93" "10: the party makes no progress toward its destination." },
    { IWD_CAT_TRAVEL, "Fail with 9 or lower: one random untied party member is lost." },
    { IWD_CAT_TRAVEL, "Finding a lost member: group DC 15 Survival check every hour." },
    CONCENTRATION,
};
static const IwdEffect FX8[] = {
    RANGED, HEARING,
    { IWD_CAT_HEARING, "Hearing range reduced to 30 ft." },
    NO_LIGHT,
    { IWD_CAT_SIGHT, "Characters without goggles are Blinded even in lit conditions." },
    { IWD_CAT_LIGHT, "Visibility from unnatural light reduced to 10 ft." },
    COLD_EVEN_RESIST, EXTINGUISH,
    { IWD_CAT_TRAVEL, "Veering off course: the navigator makes a DC 18 Survival check every hour while travelling." },
    { IWD_CAT_TRAVEL, "Fail with 17\xE2\x80\x93" "15: the party makes no progress toward its destination." },
    { IWD_CAT_TRAVEL, "Fail with 14\xE2\x80\x93" "10: one random untied party member is lost." },
    { IWD_CAT_TRAVEL, "Fail with 9 or lower: one random tied party member is lost." },
    { IWD_CAT_TRAVEL, "Finding a lost member: group DC 18 Survival check every hour." },
    CONCENTRATION,
};
static const IwdEffect FX9[] = {
    DIM_SIGHT,
    { IWD_CAT_LIGHT, "Visibility from unnatural light reduced to 5 ft." },
    { IWD_CAT_TRAVEL, "Veering off course: the navigator makes a DC 12 Survival check every hour while travelling." },
    { IWD_CAT_TRAVEL, "Fail with 11\xE2\x80\x93" "8: the party makes no progress toward its destination." },
    { IWD_CAT_TRAVEL, "Fail with 7 or lower: one random untied party member is lost." },
    { IWD_CAT_TRAVEL, "Finding a lost member: group DC 12 Survival check every hour." },
};
static const IwdEffect FX10[] = {
    { IWD_CAT_SIGHT, "Dim natural light from rapid lightning: disadvantage on [Sight] Perception checks." },
    { IWD_CAT_HAZARD, "Every hour, each player caught in the open tundra rolls a d100." },
    { IWD_CAT_HAZARD, "On a 001: DC 15 DEX save \xE2\x80\x94 8d8 lightning damage on a fail, half on a success." },
    { IWD_CAT_HAZARD, "Each creature within 15 ft of the strike also makes a DC 15 DEX save \xE2\x80\x94 half the full damage on a fail, none on a success." },
    { IWD_CAT_HAZARD, "Every hour, booming thunder has a 50% chance to trigger an avalanche in the mountains." },
    HEARING,
};
static const IwdEffect FX11[] = {
    RANGED, HEARING, NO_LIGHT, COLD_NO_RESIST,
    { IWD_CAT_HAZARD, "Icy monstrosity: DC 10 DEX save every hour \xE2\x80\x94 1d12 bludgeoning on a fail, half on a success." },
};
static const IwdEffect FX12[] = { RANGED, HEARING, DIM_SIGHT, COLD_EVEN_RESIST, EXTINGUISH };

#define NFX(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MOON_HALF "\xC2\xBD"
#define MOON_3Q   "\xC2\xBE"

static const IwdEventDef EVENTS[IWD_EVENT_COUNT + 1] = {
    { 0, NULL, 0, IWD_WIND_NONE, 0, IWD_EVLIGHT_NONE, NULL, false, { 0, 0, 0 }, NULL, NULL, 0 },
    { 1, "Clear Skies", 2, IWD_WIND_NONE, -50, IWD_EVLIGHT_DIM, MOON_HALF, true, { 2, 5, 9 },
      "The sky is a frozen lens. Every star burns cold and close.", FX1, NFX(FX1) },
    { 2, "Light Cloud Coverage", 1, IWD_WIND_MILD, -55, IWD_EVLIGHT_DIM, MOON_3Q, true, { 1, 3, 5 },
      "Thin veils of cloud drift across the stars like breath on glass.", FX2, NFX(FX2) },
    { 3, "Overcast", 1, IWD_WIND_MODERATE, -65, IWD_EVLIGHT_NONE, NULL, false, { 2, 4, 11 },
      "A low iron lid of cloud seals the dale. The world goes grey, then black.", FX3, NFX(FX3) },
    { 4, "Storm Clouds", 1, IWD_WIND_SEVERE, -80, IWD_EVLIGHT_NONE, NULL, false, { 3, 6, 10 },
      "Bruised thunderheads roll off the Spine of the World, howling as they come.", FX4, NFX(FX4) },
    { 5, "Light Snow Dusting", 1, IWD_WIND_NONE, -60, IWD_EVLIGHT_DIM, "Full", true, { 2, 6, 10 },
      "Fine snow sifts down in perfect silence, glittering where the light finds it.", FX5, NFX(FX5) },
    { 6, "Heavy Snowfall", 1, IWD_WIND_MILD, -70, IWD_EVLIGHT_NONE, NULL, false, { 3, 7, 11 },
      "Fat flakes fall thick as wool, burying tracks within minutes.", FX6, NFX(FX6) },
    { 7, "Blizzard", 2, IWD_WIND_MODERATE, -90, IWD_EVLIGHT_NONE, NULL, false, { 4, 6, 8 },
      "Auril's fury made manifest: a screaming wall of ice and wind.", FX7, NFX(FX7) },
    { 8, "White Out", 1, IWD_WIND_SEVERE, -130, IWD_EVLIGHT_NONE, NULL, false, { 5, 6, 7 },
      "Sky, ground and horizon dissolve into one blinding, killing white.", FX8, NFX(FX8) },
    { 9, "Extreme Fog", 1, IWD_WIND_NONE, -55, IWD_EVLIGHT_DIM, "Full", true, { 1, 5, 12 },
      "Freezing fog pours across the tundra; lanterns become ghosts at arm\xE2\x80\x99s length.", FX9, NFX(FX9) },
    { 10, "Lightning Storm", 2, IWD_WIND_MILD, -60, IWD_EVLIGHT_LIGHTNING, NULL, false, { 4, 5, 6 },
      "Violet lightning stalks the tundra, and the thunder shakes snow from the peaks.", FX10, NFX(FX10) },
    { 11, "Hail Storm", 1, IWD_WIND_SEVERE, -80, IWD_EVLIGHT_NONE, NULL, false, { 3, 6, 7 },
      "Stones of ice the size of knuckles hammer down out of the dark.", FX11, NFX(FX11) },
    { 12, "Wind Vortex", 1, IWD_WIND_SEVERE, -110, IWD_EVLIGHT_DIM, MOON_HALF, true, { 1, 1, 1 },
      "A spinning column of wind tears across the dale, dragging the snow into the sky.", FX12, NFX(FX12) },
};

const IwdEventDef *iwd_event(int id)
{
    return (id >= 1 && id <= IWD_EVENT_COUNT) ? &EVENTS[id] : NULL;
}

static const uint8_t SEVERITY[IWD_EVENT_COUNT + 1] = { 0, 1, 1, 2, 3, 1, 3, 4, 5, 2, 3, 3, 4 };
static const char *const SEV_LABEL[6] = { "\xE2\x80\x94", "Calm", "Uneasy", "Dangerous", "Deadly", "Lethal" };

int iwd_severity(int event_id)
{
    return (event_id >= 1 && event_id <= IWD_EVENT_COUNT) ? SEVERITY[event_id] : 0;
}

const char *iwd_severity_label(int severity)
{
    return (severity >= 0 && severity <= 5) ? SEV_LABEL[severity] : SEV_LABEL[0];
}

static const uint32_t EVENT_RGB[IWD_EVENT_COUNT + 1] = {
    0x000000, 0xa6ecff, 0x8ccbeb, 0x6d819b, 0x46526b, 0xdcf3ff, 0xa9c5e2,
    0x7aa7d8, 0xf5fbff, 0xb9c7d1, 0xb49cff, 0x7fd1d9, 0x5ee0c8,
};

uint32_t iwd_event_rgb(int event_id)
{
    return (event_id >= 1 && event_id <= IWD_EVENT_COUNT) ? EVENT_RGB[event_id] : 0;
}

bool iwd_event_rgb_is_dark(int event_id)
{
    uint32_t n = iwd_event_rgb(event_id);
    int r = (int)((n >> 16) & 255), g = (int)((n >> 8) & 255), b = (int)(n & 255);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b < 140;
}

int iwd_next_index_for(int dice, int duration)
{
    if (dice == 1) return duration <= 2 ? 0 : duration == 3 ? 1 : 2;
    return duration <= 4 ? 0 : duration <= 6 ? 1 : 2;
}

/* ================================================================================
 * Light cycles
 * ============================================================================== */

static const IwdCycle CYCLES[IWD_CYCLE_COUNT] = {
    { "Moon Cycle",     20, 6 },
    { "Twilight",       10, 14 },
    { "Auril's Aurora", 22, 2 },
};

const IwdCycle *iwd_cycle(IwdCycleKey key)
{
    return ((int)key >= 0 && key < IWD_CYCLE_COUNT) ? &CYCLES[key] : NULL;
}

bool iwd_in_window(int h, const IwdCycle *w)
{
    return w->from < w->to ? (h >= w->from && h < w->to) : (h >= w->from || h < w->to);
}

IwdLight iwd_light_at(const IwdEventDef *ev, double hour)
{
    IwdLight L;
    memset(&L, 0, sizeof L);
    int h = dfinite(hour) ? (int)js_dmod(floor(hour), 24.0) : 0;
    L.moon = iwd_in_window(h, &CYCLES[IWD_CYCLE_MOON]);
    L.twilight = iwd_in_window(h, &CYCLES[IWD_CYCLE_TWILIGHT]);
    L.aurora = iwd_in_window(h, &CYCLES[IWD_CYCLE_AURORA]);
    if (!ev || ev->light == IWD_EVLIGHT_NONE) {
        L.level = IWD_LIGHT_NONE;
        return L;
    }
    if (ev->light == IWD_EVLIGHT_LIGHTNING) {
        L.level = IWD_LIGHT_DIM;
        L.sources = IWD_SRC_LIGHTNING;
        return L;
    }
    L.aurora_visible = ev->natural && L.aurora;
    if (L.moon) L.sources |= IWD_SRC_MOON;
    if (L.aurora) L.sources |= IWD_SRC_AURORA;
    if (L.twilight) L.sources |= IWD_SRC_TWILIGHT;
    L.level = L.sources ? IWD_LIGHT_DIM : IWD_LIGHT_DARK;
    return L;
}

size_t iwd_light_label(const IwdEventDef *ev, const IwdLight *light, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    if (light->level == IWD_LIGHT_NONE) {
        out_str(&o, "No natural light");
    } else if (light->sources & IWD_SRC_LIGHTNING) {
        out_str(&o, "Dim light \xE2\x80\x94 rapid lightning");
    } else if (light->level == IWD_LIGHT_DARK) {
        out_str(&o, "Between light cycles: no natural source");
    } else {
        out_str(&o, "Dim light \xE2\x80\x94 ");
        bool first = true;
        if (light->sources & IWD_SRC_MOON) {
            /* ev.moon + ' Moon'  (String(undefined) for an event without a moon) */
            out_str(&o, (ev && ev->moon) ? ev->moon : "undefined");
            out_str(&o, " Moon");
            first = false;
        }
        if (light->sources & IWD_SRC_AURORA) {
            if (!first) out_str(&o, ", ");
            out_str(&o, "Auril's Aurora");
            first = false;
        }
        if (light->sources & IWD_SRC_TWILIGHT) {
            if (!first) out_str(&o, ", ");
            out_str(&o, "Twilight");
        }
    }
    return out_end(&o);
}

const char *iwd_light_level_key(IwdLightLevel level)
{
    switch (level) {
    case IWD_LIGHT_DIM:  return "dim";
    case IWD_LIGHT_DARK: return "dark";
    default:             return "none";
    }
}

const char *iwd_light_badge(IwdLightLevel level)
{
    switch (level) {
    case IWD_LIGHT_DIM:  return "Dim light";
    case IWD_LIGHT_DARK: return "Between light cycles";
    default:             return "No natural light";
    }
}

/* ================================================================================
 * Temperature
 * ============================================================================== */

double iwd_temp_knot(uint32_t sh, int32_t n)
{
    uint32_t x = ((uint32_t)n * 0x9e3779b1u) ^ sh;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    x ^= x >> 16;
    return ((double)x / 4294967295.0) * 2 - 1;
}

double iwd_temp_diurnal(double hour)
{
    double h = js_dmod(hour, 24.0);
    if (h >= 2 && h <= 12) return -IWD_DIURNAL_AMP * iwd_js_cos(IWD_PI * (h - 2) / 10);
    return IWD_DIURNAL_AMP * iwd_js_cos(IWD_PI * js_dmod((h - 12) + 24, 24.0) / 14);
}

double iwd_temp_offset_at(uint32_t sh, double abs_hour)
{
    double k = abs_hour / IWD_KNOT_HOURS;
    double n = floor(k);
    double f = k - n;
    double s = f * f * (3 - 2 * f);
    int32_t ni = (int32_t)js_to_uint32(n);
    int32_t ni1 = (int32_t)js_to_uint32(n + 1);
    return IWD_NOISE_AMP * (iwd_temp_knot(sh, ni) * (1 - s) + iwd_temp_knot(sh, ni1) * s) +
           iwd_temp_diurnal(abs_hour);
}

double iwd_temp_offset(const char *seed_utf8, int32_t day_index, double hour)
{
    return iwd_temp_offset_at(iwd_temp_hash(seed_utf8), (double)day_index * 24 + hour);
}

int iwd_f_to_c(int temp_f)
{
    return (int)iwd_js_round((double)((temp_f - 32) * 5) / 9);
}

static const IwdColdTier COLD_TIERS[IWD_COLD_TIER_COUNT] = {
    { "cold",    "Cold",         10, 0,    -20 },
    { "frigid",  "Frigid Cold",  15, -40,  -40 },
    { "severe",  "Severe Cold",  20, -80,  -60 },
    { "extreme", "Extreme Cold", 25, -110, -80 },
    { "deathly", "Deathly Cold", 28, -130, -100 },
};

const IwdColdTier *iwd_cold_tier(int index)
{
    return (index >= 0 && index < IWD_COLD_TIER_COUNT) ? &COLD_TIERS[index] : NULL;
}

IwdCold iwd_cold_dc(int temp_f)
{
    int ti = 0;
    for (int i = 0; i < IWD_COLD_TIER_COUNT; i++)
        if (COLD_TIERS[i].f >= temp_f) ti = i;
    const IwdColdTier *t = &COLD_TIERS[ti];
    int dc;
    if (temp_f > t->f) {
        dc = t->dc - (temp_f - t->f) / 10;
        if (dc < 0) dc = 0;
    } else {
        dc = t->dc + (t->f - temp_f) / 10;
    }
    IwdCold c = { t, ti, dc, dc - t->dc };
    return c;
}

/* ================================================================================
 * The chain
 * ============================================================================== */

static IwdActive start_event(IwdRng *rng, int id, IwdCauseType cause, int cause_arg, int32_t day, int hour)
{
    const IwdEventDef *ev = &EVENTS[id];
    IwdActive a;
    memset(&a, 0, sizeof a);
    a.id = (uint8_t)id;
    a.n_rolls = (uint8_t)ev->dice;
    int sum = 0;
    for (int i = 0; i < ev->dice; i++) {
        a.rolls[i] = (uint8_t)iwd_rng_die(rng, 4);
        sum += a.rolls[i];
    }
    a.duration = (uint8_t)sum;
    a.elapsed = 0;
    a.next_idx = (uint8_t)iwd_next_index_for(ev->dice, sum);
    a.next = ev->next[a.next_idx];
    a.cause_type = (uint8_t)cause;
    a.cause_arg = (uint8_t)cause_arg;
    a.start_day = day;
    a.start_hour = (uint8_t)hour;
    return a;
}

void iwd_chain_init(IwdChain *chain, const char *seed_utf8)
{
    memset(chain, 0, sizeof *chain);
    chain->rng_state = iwd_hash_seed(seed_utf8);
    chain->temp_hash = iwd_temp_hash(seed_utf8);
    chain->day_count = 0;
    chain->has_current = false;
}

void iwd_next_day(IwdChain *chain, IwdDay *out)
{
    IwdRng rng;
    iwd_rng_init(&rng, chain->rng_state);
    int32_t day_no = chain->day_count + 1;

    memset(out, 0, sizeof *out);
    out->day = day_no;
    out->has_carried_in = chain->has_current;
    if (chain->has_current) out->carried_in = chain->current;

    IwdActive cur;
    if (!chain->has_current) {
        int d12 = iwd_rng_die(&rng, 12);
        cur = start_event(&rng, d12, IWD_CAUSE_GENESIS, d12, day_no, 0);
    } else {
        cur = chain->current;
    }

    int hour = 0;
    while (hour < 24) {
        int remaining = cur.duration - cur.elapsed;
        int take = remaining < 24 - hour ? remaining : 24 - hour;
        IwdSegment *s = &out->segments[out->n_segments++];
        s->id = cur.id;
        s->start = (uint8_t)hour;
        s->end = (uint8_t)(hour + take);
        s->duration = cur.duration;
        s->n_rolls = cur.n_rolls;
        s->rolls[0] = cur.rolls[0];
        s->rolls[1] = cur.rolls[1];
        s->next = cur.next;
        s->next_idx = cur.next_idx;
        s->elapsed_before = cur.elapsed;
        s->from_prev_day = cur.elapsed > 0 && hour == 0;
        s->into_next_day = hour + take >= 24 && cur.elapsed + take < cur.duration;
        s->change_at = (uint8_t)(hour + remaining);
        s->cause_type = cur.cause_type;
        s->cause_arg = cur.cause_arg;
        hour += take;
        cur.elapsed = (uint8_t)(cur.elapsed + take);
        if (cur.elapsed >= cur.duration) {
            int from = cur.id;
            cur = start_event(&rng, cur.next, IWD_CAUSE_TRANSITION, from, day_no, hour % 24);
            if (hour >= 24) cur.start_day = day_no + 1;
        }
    }

    chain->current = cur;
    chain->has_current = true;
    chain->rng_state = rng.state;
    chain->day_count = day_no;
    out->carry_out = cur;

    int tmin = 0, tmax = 0;
    for (int h = 0; h < 24; h++) {
        const IwdSegment *s = iwd_day_segment_at(out, h);
        int id = s->id;
        double t = (double)EVENTS[id].temp_f +
                   iwd_temp_offset_at(chain->temp_hash, (double)(day_no - 1) * 24 + h);
        int tf = (int)iwd_js_round(t);
        out->hour_id[h] = (uint8_t)id;
        out->temp_f[h] = (int16_t)tf;
        if (h == 0 || tf < tmin) tmin = tf;
        if (h == 0 || tf > tmax) tmax = tf;
    }
    out->temp_min = (int16_t)tmin;
    out->temp_max = (int16_t)tmax;
}

const IwdSegment *iwd_day_segment_at(const IwdDay *day, int hour)
{
    if (hour < 0 || hour > 23) return NULL;
    for (int i = 0; i < day->n_segments; i++) {
        const IwdSegment *s = &day->segments[i];
        if (hour >= s->start && hour < s->end) return s;
    }
    return NULL;
}

int iwd_day_log(const IwdDay *day, const IwdSegment *out[IWD_MAX_SEGMENTS])
{
    int n = 0;
    for (int i = 0; i < day->n_segments; i++)
        if (day->segments[i].elapsed_before == 0) out[n++] = &day->segments[i];
    return n;
}

int iwd_day_dominant(const IwdDay *day)
{
    int tally[IWD_EVENT_COUNT + 1] = { 0 };
    for (int i = 0; i < day->n_segments; i++) {
        const IwdSegment *s = &day->segments[i];
        tally[s->id] += s->end - s->start;
    }
    int best = 0;
    for (int id = 1; id <= IWD_EVENT_COUNT; id++)
        if (tally[id] > 0 && (best == 0 || tally[id] > tally[best])) best = id;
    return best;
}

double iwd_day_temp_at(const IwdDay *day, uint32_t temp_hash, double t)
{
    double tt = t < 23.999 ? t : 23.999;
    if (!(tt > 0)) tt = 0;   /* Math.max(0, ...) ; NaN -> NaN in JS, 0 here */
    int h = (int)floor(tt);
    return (double)EVENTS[day->hour_id[h]].temp_f +
           iwd_temp_offset_at(temp_hash, (double)(day->day - 1) * 24 + tt);
}

/* ================================================================================
 * Scene looks (scene.js)
 * ============================================================================== */

static const IwdLook LOOKS[IWD_EVENT_COUNT + 1] = {
    /*        cloud cloudDark snow flake wind  hail fog  whiteout lightning vortex stars moon dark extinguish */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0.04, 0.1,  0,    0.5,  0.02, 0, 0,    0,    0, 0, 1,    0.5,  0,    0 },
    { 0.38, 0.2,  0,    0.5,  0.2,  0, 0,    0,    0, 0, 0.6,  0.75, 0.1,  0 },
    { 0.92, 0.5,  0,    0.5,  0.42, 0, 0.1,  0,    0, 0, 0,    0,    0.62, 0 },
    { 1,    0.9,  0.06, 0.6,  0.75, 0, 0.05, 0,    0, 0, 0,    0,    0.82, 0 },
    { 0.42, 0.15, 0.24, 0.35, 0.03, 0, 0.05, 0,    0, 0, 0.45, 1,    0.05, 0 },
    { 0.96, 0.55, 0.72, 1,    0.22, 0, 0.15, 0.05, 0, 0, 0,    0,    0.7,  0 },
    { 1,    0.7,  1,    0.75, 0.68, 0, 0.2,  0.42, 0, 0, 0,    0,    0.72, 1 },
    { 1,    0.1,  1,    0.6,  1,    0, 0.4,  0.92, 0, 0, 0,    0,    0.35, 1 },
    { 0.25, 0.1,  0,    0.5,  0,    0, 1,    0.1,  0, 0, 0.12, 1,    0.15, 0 },
    { 0.9,  0.85, 0.04, 0.5,  0.2,  0, 0,    0,    1, 0, 0,    0,    0.6,  0 },
    { 1,    0.8,  0.1,  0.5,  0.82, 1, 0.05, 0.05, 0, 0, 0,    0,    0.78, 0 },
    { 0.3,  0.3,  0.3,  0.4,  1,    0, 0,    0.1,  0, 1, 0.5,  0.5,  0.2,  1 },
};

const IwdLook *iwd_look(int event_id)
{
    return (event_id >= 1 && event_id <= IWD_EVENT_COUNT) ? &LOOKS[event_id] : NULL;
}

static double smooth01(double t) { return t * t * (3 - 2 * t); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }

void iwd_look_at(IwdEventAtFn at, void *user, double t, IwdLook *out)
{
    double basef = floor(t);
    int base = (int)basef;
    double frac = t - basef;
    int ia = at(user, base), ib = at(user, base + 1), ip = at(user, base - 1);
    const IwdLook *a = iwd_look(ia), *b = iwd_look(ib);
    const IwdLook *p = ip != ia ? iwd_look(ip) : a;
    if (!a) a = &LOOKS[1];
    if (!b) b = a;
    if (!p) p = a;
    double wIn = frac > 0.6 ? smooth01((frac - 0.6) / 0.4) * 0.5 : 0;
    double wOut = frac < 0.25 ? (1 - smooth01(frac / 0.25)) * 0.5 : 0;
    const double *av = &a->cloud, *bv = &b->cloud, *pv = &p->cloud;
    double *ov = &out->cloud;
    const int nfields = (int)(sizeof(IwdLook) / sizeof(double));
    for (int k = 0; k < nfields; k++) {
        double v = av[k];
        if (b != a) v = lerp(v, bv[k], wIn);
        if (p != a) v = lerp(v, pv[k], wOut);
        ov[k] = v;
    }
}

double iwd_band(double h, double from, double to, double fade)
{
    double len = js_dmod(to - from, 24.0);
    if (len == 0 || dnan(len)) len = 24;
    double x = js_dmod(h - from, 24.0);
    double d = x < len ? fmin(x, len - x) : -fmin(x - len, 24 - x);
    double c = 0.5 + d / fade;
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    return smooth01(c);
}

/* ================================================================================
 * Calendar of Harptos
 * ============================================================================== */

static const IwdMonth MONTHS[12] = {
    { "Hammer",    "Deepwinter" },
    { "Alturiak",  "The Claw of Winter" },
    { "Ches",      "The Claw of the Sunsets" },
    { "Tarsakh",   "The Claw of the Storms" },
    { "Mirtul",    "The Melting" },
    { "Kythorn",   "The Time of Flowers" },
    { "Flamerule", "Summertide" },
    { "Eleasis",   "Highsun" },
    { "Eleint",    "The Fading" },
    { "Marpenoth", "Leaffall" },
    { "Uktar",     "The Rotting" },
    { "Nightal",   "The Drawing Down" },
};

const IwdMonth *iwd_month(int month)
{
    return (month >= 0 && month < 12) ? &MONTHS[month] : NULL;
}

static const IwdFestivalDef FESTIVALS[IWD_FEST_COUNT] = {
    { "midwinter",      "Midwinter",         0,  false },
    { "greengrass",     "Greengrass",        3,  false },
    { "midsummer",      "Midsummer",         6,  false },
    { "shieldmeet",     "Shieldmeet",        6,  true },
    { "highharvestide", "Highharvestide",    8,  false },
    { "feastofthemoon", "Feast of the Moon", 10, false },
};

const IwdFestivalDef *iwd_festival(IwdFestival f)
{
    return ((int)f >= 0 && f < IWD_FEST_COUNT) ? &FESTIVALS[f] : NULL;
}

IwdFestival iwd_festival_from_key(const char *key)
{
    if (!key) return IWD_FEST_NONE;
    for (int i = 0; i < IWD_FEST_COUNT; i++)
        if (strcmp(FESTIVALS[i].key, key) == 0) return (IwdFestival)i;
    return IWD_FEST_NONE;
}

bool iwd_is_leap(int32_t year) { return iwd_mod(year, 4) == 0; }

int iwd_days_in_year(int32_t year) { return iwd_is_leap(year) ? 366 : 365; }

static int64_t year_start64(int64_t y) { return 365 * y + iwd_floor_div(y + 3, 4); }

int32_t iwd_year_start(int32_t year) { return (int32_t)year_start64(year); }

static bool fest_in_year(int f, bool leap) { return !FESTIVALS[f].leap_only || leap; }

/* offsets.month[m] in buildTable(leap) */
static int month_offset(int m, bool leap)
{
    int off = 30 * m;
    for (int f = 0; f < IWD_FEST_COUNT; f++)
        if (FESTIVALS[f].after < m && fest_in_year(f, leap)) off++;
    return off;
}

/* offsets.fest[key], or -1 when the festival does not exist that year */
static int fest_offset(int fest, bool leap)
{
    if (!fest_in_year(fest, leap)) return -1;
    int off = 30 * (FESTIVALS[fest].after + 1);
    for (int f = 0; f < fest; f++)
        if (fest_in_year(f, leap)) off++;
    return off;
}

bool iwd_to_abs(int32_t year, int month, int day, IwdFestival festival, int32_t *abs_out)
{
    if (year < IWD_YEAR_MIN || year > IWD_YEAR_MAX) return false;
    bool leap = iwd_is_leap(year);
    int off;
    if (festival != IWD_FEST_NONE) {
        if ((int)festival < 0 || festival >= IWD_FEST_COUNT) return false;
        off = fest_offset(festival, leap);
        if (off < 0) return false;
    } else {
        if (!(month >= 0 && month < 12 && day >= 1 && day <= 30)) return false;
        off = month_offset(month, leap) + day - 1;
    }
    if (abs_out) *abs_out = (int32_t)(year_start64(year) + off);
    return true;
}

static int64_t year_of(int64_t abs)
{
    int64_t y = (int64_t)floor((double)abs / 365.25);
    while (year_start64(y) > abs) y--;
    while (year_start64(y + 1) <= abs) y++;
    return y;
}

IwdDate iwd_from_abs(int32_t abs)
{
    if (abs < IWD_ABS_MIN) abs = IWD_ABS_MIN;
    if (abs > IWD_ABS_MAX) abs = IWD_ABS_MAX;
    IwdDate d;
    memset(&d, 0, sizeof d);
    int64_t year = year_of(abs);
    int doy = (int)(abs - year_start64(year));
    bool leap = iwd_is_leap((int32_t)year);
    d.abs = abs;
    d.year = (int32_t)year;
    d.day_of_year = doy + 1;
    d.month = -1;
    d.day = 0;
    d.festival = IWD_FEST_NONE;
    int rest = doy;
    for (int m = 0; m < 12; m++) {
        if (rest < 30) { d.month = m; d.day = rest + 1; break; }
        rest -= 30;
        bool found = false;
        for (int f = 0; f < IWD_FEST_COUNT; f++) {
            if (FESTIVALS[f].after != m || !fest_in_year(f, leap)) continue;
            if (rest == 0) { d.festival = (IwdFestival)f; found = true; break; }
            rest--;
        }
        if (found) break;
    }
    if (d.festival == IWD_FEST_NONE) {
        d.tenday_index = (d.day - 1) / 10;
        d.day_of_tenday = ((d.day - 1) % 10) + 1;
    } else {
        d.tenday_index = -1;
        d.day_of_tenday = 0;
    }
    return d;
}

size_t iwd_format_date(int32_t abs, bool short_form, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    IwdDate d = iwd_from_abs(abs);
    if (d.festival != IWD_FEST_NONE) {
        out_str(&o, FESTIVALS[d.festival].name);
        if (!short_form) { out_str(&o, ", "); out_int(&o, d.year); out_str(&o, " DR"); }
    } else {
        out_int(&o, d.day);
        out_str(&o, " ");
        out_str(&o, MONTHS[d.month].name);
        if (!short_form) { out_str(&o, " "); out_int(&o, d.year); out_str(&o, " DR"); }
    }
    return out_end(&o);
}

void iwd_month_of(int32_t abs, int32_t *year_out, int *month_out)
{
    IwdDate d = iwd_from_abs(abs);
    if (year_out) *year_out = d.year;
    if (month_out) *month_out = d.festival != IWD_FEST_NONE ? FESTIVALS[d.festival].after : d.month;
}

void iwd_month_layout(int32_t year, int month, IwdMonthLayout *out)
{
    memset(out, 0, sizeof *out);
    if (month < 0) month = 0;
    if (month > 11) month = 11;
    bool leap = iwd_is_leap(year);
    int64_t ys = year_start64(year);
    out->year = year;
    out->month = month;
    out->name = MONTHS[month].name;
    out->common = MONTHS[month].common;
    out->first_abs = (int32_t)(ys + month_offset(month, leap));
    for (int f = 0; f < IWD_FEST_COUNT && out->n_festivals < 2; f++) {
        if (FESTIVALS[f].after != month) continue;
        int off = fest_offset(f, leap);
        if (off < 0) continue;
        out->festivals[out->n_festivals].abs = (int32_t)(ys + off);
        out->festivals[out->n_festivals].festival = (IwdFestival)f;
        out->n_festivals++;
    }
}

/* ---------- Selune ---------- */

static const char *const PHASES[8] = {
    "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
    "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent",
};

static bool hits_day(double epoch, double abs)
{
    double k = ceil((abs - epoch) / IWD_MOON_PERIOD);
    return epoch + k * IWD_MOON_PERIOD < abs + 1;
}

IwdMoon iwd_moon(int32_t abs_i, double offset_days)
{
    IwdMoon m;
    double abs = (double)abs_i;
    double off = (dfinite(offset_days) && offset_days != 0) ? offset_days : 0;
    double full_epoch = (double)IWD_FULL_EPOCH;
    double new_epoch = full_epoch + IWD_MOON_PERIOD / 2;
    m.age = js_dmod(abs + 0.5 - (new_epoch + off), IWD_MOON_PERIOD);
    m.phase = m.age / IWD_MOON_PERIOD;
    m.illum = (1 - iwd_js_cos(2 * IWD_PI * m.phase)) / 2;
    m.waxing = m.age < IWD_MOON_PERIOD / 2;
    m.is_new = hits_day(new_epoch + off, abs);
    m.is_full = hits_day(full_epoch + off, abs);
    int oct = (int)fmod(floor(m.phase * 8 + 0.5), 8.0);
    if (oct == 0 && !m.is_new) oct = m.waxing ? 1 : 7;
    if (oct == 4 && !m.is_full) oct = m.waxing ? 3 : 5;
    if (m.is_new) oct = 0;
    if (m.is_full) oct = 4;
    m.octant = oct;
    m.name = PHASES[oct];
    return m;
}

int iwd_moon_percent(const IwdMoon *moon) { return (int)iwd_js_round(moon->illum * 100); }

/* ---------- Recurrence ---------- */

static const char *const REPEAT_KEYS[IWD_REPEAT_COUNT] = {
    "none", "newmoon", "fullmoon", "tenday", "monthly", "yearly",
};
static const char *const REPEAT_LABELS[IWD_REPEAT_COUNT] = {
    "Never", "Every new moon", "Every full moon", "Every tenday", "Every month", "Every year",
};

const char *iwd_repeat_key(IwdRepeat r)
{
    return ((int)r >= 0 && r < IWD_REPEAT_COUNT) ? REPEAT_KEYS[r] : "";
}

const char *iwd_repeat_label(IwdRepeat r)
{
    return ((int)r >= 0 && r < IWD_REPEAT_COUNT) ? REPEAT_LABELS[r] : "";
}

IwdRepeat iwd_repeat_from_key(const char *key)
{
    if (!key) return IWD_REPEAT_COUNT;
    for (int i = 0; i < IWD_REPEAT_COUNT; i++)
        if (strcmp(REPEAT_KEYS[i], key) == 0) return (IwdRepeat)i;
    return IWD_REPEAT_COUNT;
}

bool iwd_occurs_on(int32_t start, IwdRepeat repeat, int32_t abs, double moon_offset)
{
    if (abs < start) return false;
    if (repeat == IWD_REPEAT_NEWMOON) return iwd_moon(abs, moon_offset).is_new;
    if (repeat == IWD_REPEAT_FULLMOON) return iwd_moon(abs, moon_offset).is_full;
    if (abs == start) return true;
    switch (repeat) {
    case IWD_REPEAT_YEARLY: {
        IwdDate e = iwd_from_abs(start), d = iwd_from_abs(abs);
        return e.festival != IWD_FEST_NONE ? d.festival == e.festival
                                           : (d.festival == IWD_FEST_NONE && d.month == e.month && d.day == e.day);
    }
    case IWD_REPEAT_MONTHLY: {
        IwdDate e = iwd_from_abs(start), d = iwd_from_abs(abs);
        return e.festival == IWD_FEST_NONE && d.festival == IWD_FEST_NONE && d.day == e.day;
    }
    case IWD_REPEAT_TENDAY: {
        IwdDate e = iwd_from_abs(start), d = iwd_from_abs(abs);
        return e.festival == IWD_FEST_NONE && d.festival == IWD_FEST_NONE && d.day_of_tenday == e.day_of_tenday;
    }
    default:
        return false;
    }
}

int iwd_occurrences(int32_t start, IwdRepeat repeat, int32_t from, int32_t to,
                    double moon_offset, int32_t *out, int cap)
{
    int n = 0;
    int64_t a = from > start ? from : start;
    for (; a <= to; a++) {
        if (iwd_occurs_on(start, repeat, (int32_t)a, moon_offset)) {
            if (out && n < cap) out[n] = (int32_t)a;
            n++;
        }
    }
    return n;
}

/* ================================================================================
 * Derived per-hour presentation
 * ============================================================================== */

const char *iwd_temp_note(int temp_f)
{
    return temp_f <= -100 ? "killing cold" : temp_f <= -80 ? "flesh-freezing" : "air temperature";
}

static void out_tier_anchor(Out *o, const IwdColdTier *t, bool celsius)
{
    if (celsius) {
        out_minus(o, iwd_f_to_c(t->f));
        out_str(o, "\xC2\xB0" "C (");
        out_minus(o, t->f);
        out_str(o, "\xC2\xB0" "F)");
    } else {
        out_minus(o, t->f);
        out_str(o, "\xC2\xB0" "F");
    }
}

size_t iwd_fmt_tier_anchor(const IwdColdTier *tier, bool celsius, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_tier_anchor(&o, tier, celsius);
    return out_end(&o);
}

const char *iwd_dc_step(bool celsius)
{
    return celsius ? "\xC2\xB1" "1 per 10\xC2\xB0" "F (\xE2\x89\x88" "5.6\xC2\xB0" "C)"
                   : "\xC2\xB1" "1 per 10\xC2\xB0" "F";
}

size_t iwd_fmt_tier_tip(const IwdCold *cold, bool celsius, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_str(&o, "Cold table: DC ");
    out_int(&o, cold->tier->dc);
    out_str(&o, " at ");
    out_tier_anchor(&o, cold->tier, celsius);
    out_str(&o, ", ");
    out_str(&o, iwd_dc_step(celsius));
    return out_end(&o);
}

size_t iwd_fmt_light_value(const IwdEventDef *ev, const IwdLight *light, char *buf, size_t cap)
{
    char label[IWD_LIGHT_LABEL_CAP];
    iwd_light_label(ev, light, label, sizeof label);
    Out o = out_begin(buf, cap);
    out_str(&o, label);
    if (light->level == IWD_LIGHT_NONE) out_str(&o, " \xE2\x80\x94 Blinded");
    return out_end(&o);
}

IwdForecast iwd_forecast(const IwdDay *day, int hour)
{
    IwdForecast f;
    memset(&f, 0, sizeof f);
    const IwdSegment *s = iwd_day_segment_at(day, hour);
    if (!s) return f;
    f.next_id = s->next;
    f.change_at = s->change_at;
    f.left = s->change_at - hour;
    f.tomorrow = s->change_at >= 24;
    f.dice = EVENTS[s->id].dice;
    f.duration = s->duration;
    return f;
}

size_t iwd_fmt_forecast(const IwdForecast *f, char *buf, size_t cap, size_t *name_len)
{
    Out o = out_begin(buf, cap);
    const IwdEventDef *nx = iwd_event(f->next_id);
    out_str(&o, nx ? nx->name : "");
    if (name_len) *name_len = o.len;
    char hh[8];
    iwd_fmt_hour(f->change_at, hh, sizeof hh);
    out_str(&o, " at ");
    out_str(&o, hh);
    if (f->tomorrow) out_str(&o, " tomorrow");
    out_str(&o, " (in ");
    out_int(&o, f->left);
    out_str(&o, " h \xC2\xB7 ");
    out_int(&o, f->dice);
    out_str(&o, "d4 rolled ");
    out_int(&o, f->duration);
    out_str(&o, ")");
    return out_end(&o);
}

void iwd_hour_effects(int event_id, int hour, int temp_f, bool celsius, IwdHourEffects *out)
{
    memset(out, 0, sizeof *out);
    const IwdEventDef *ev = iwd_event(event_id);
    if (!ev) return;
    IwdLight light = iwd_light_at(ev, hour);
    if (light.aurora_visible) {
        out->items[out->count].cat = IWD_CAT_AURORA;
        out->items[out->count++].text = IWD_FX_TEXT_AURORA;
    }
    if (light.level == IWD_LIGHT_DARK) {
        out->items[out->count].cat = IWD_CAT_NATURAL;
        out->items[out->count++].text = IWD_FX_TEXT_DARK;
    }
    IwdCold cold = iwd_cold_dc(temp_f);
    Out o = out_begin(out->chill_text, sizeof out->chill_text);
    out_str(&o, "Cold table: ");
    out_str(&o, cold.tier->name);
    out_str(&o, " \xE2\x80\x94 DC ");
    out_int(&o, cold.dc);
    out_str(&o, " at ");
    out_minus(&o, celsius ? iwd_f_to_c(temp_f) : temp_f);
    out_str(&o, celsius ? "\xC2\xB0" "C (DC " : "\xC2\xB0" "F (DC ");
    out_int(&o, cold.tier->dc);
    out_str(&o, " at ");
    out_tier_anchor(&o, cold.tier, celsius);
    out_str(&o, ", ");
    out_str(&o, iwd_dc_step(celsius));
    if (cold.delta) {
        out_str(&o, "; ");
        out_str(&o, cold.delta > 0 ? "+" : "\xE2\x88\x92");
        out_int(&o, cold.delta > 0 ? cold.delta : -cold.delta);
        out_str(&o, " here)");
    } else {
        out_str(&o, ")");
    }
    out->items[out->count].cat = IWD_CAT_CHILL;
    out->items[out->count++].text = out->chill_text;
    for (int i = 0; i < ev->n_effects && out->count < IWD_MAX_HOUR_EFFECTS; i++)
        out->items[out->count++] = ev->effects[i];
}

const char *iwd_next_option_label(const IwdEventDef *ev, int next_idx)
{
    if (ev && ev->next[0] == ev->next[1] && ev->next[1] == ev->next[2]) return "always";
    switch (next_idx) {
    case 0:  return "1st option:";
    case 1:  return "2nd option:";
    default: return "3rd option:";
    }
}

/* ================================================================================
 * Small text helpers
 * ============================================================================== */

size_t iwd_fmt_hour(int hour, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_pad2(&o, iwd_mod(hour, 24));
    out_str(&o, ":00");
    return out_end(&o);
}

size_t iwd_fmt_clock(double t, char *buf, size_t cap)
{
    double tt = t < 23.999 ? t : 23.999;
    double h = floor(tt);
    double m = floor((tt - h) * 60);
    Out o = out_begin(buf, cap);
    out_pad2(&o, (long long)h);
    out_str(&o, ":");
    out_pad2(&o, (long long)m);
    return out_end(&o);
}

size_t iwd_fmt_minus(int n, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_minus(&o, n);
    return out_end(&o);
}

size_t iwd_fmt_temp(int temp_f, bool celsius, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_minus(&o, celsius ? iwd_f_to_c(temp_f) : temp_f);
    out_str(&o, "\xC2\xB0");
    return out_end(&o);
}

size_t iwd_rel_days(int32_t n, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    if (n == 0) out_str(&o, "Today");
    else if (n == 1) out_str(&o, "Tomorrow");
    else if (n == -1) out_str(&o, "Yesterday");
    else if (n > 0) { out_str(&o, "In "); out_int(&o, n); out_str(&o, " days"); }
    else { out_int(&o, -(long long)n); out_str(&o, " days ago"); }
    return out_end(&o);
}

size_t iwd_fmt_pad2(int n, char *buf, size_t cap)
{
    Out o = out_begin(buf, cap);
    out_pad2(&o, n);
    return out_end(&o);
}
