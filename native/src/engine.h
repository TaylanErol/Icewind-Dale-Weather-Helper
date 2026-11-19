/* engine.h - Icewind Dale weather engine + Calendar of Harptos (pure C11).
 *
 * A port of the Almanac web app's js/engine.js and js/harptos.js. Reference copies of
 * both files live in native/test/reference/ and are the source of truth: every function
 * here must give the same result as its JS counterpart (integers, strings and doubles
 * exactly, bit for bit; see "Reproducibility").
 *
 * Rules of this header / its implementation (engine.c):
 *   - No Win32 headers, no allocation, no global mutable state. Compiles with host gcc.
 *   - All strings are UTF-8, NUL terminated, static storage unless a buffer is passed in.
 *   - Buffer writers take (buf, cap), always NUL terminate when cap > 0, truncate on a
 *     UTF-8 character boundary and return the number of bytes written (excluding NUL).
 *
 * ---------------------------------------------------------------------------------
 * Reproducibility: JavaScript number semantics the C code must imitate
 * ---------------------------------------------------------------------------------
 *  1. Math.imul(a, b)         -> (uint32_t)a * (uint32_t)b            (wraps mod 2^32)
 *     x >>> n                 -> logical shift of a uint32_t
 *     x ^ y, x | y, x | 0     -> 32-bit; do ALL hash/RNG arithmetic in uint32_t, never int
 *                                (signed overflow is undefined behaviour in C).
 *     "t ^= t + Math.imul(..)" in mulberry32: the JS sum is a double that is then wrapped
 *     by ToInt32; uint32_t addition gives the same bits.
 *  2. String hashing uses JS string indexing: str.length and charCodeAt() count UTF-16
 *     code units. iwd_hash_seed() therefore decodes its UTF-8 argument and hashes UTF-16
 *     units (a code point above U+FFFF contributes two units, high surrogate first).
 *     Test vectors (from node):  ""            -> 1382371643
 *                                "RIME-00FF"   -> 2105444690
 *                                "RIME-00FF|temp" -> 4064753795
 *                                "é☃𝄞" (U+00E9 U+2603 U+1D11E) -> 503696356
 *  3. rng.next() returns u / 4294967296 as a double; die(sides) = 1 + floor(next * sides).
 *     u * sides is exact in a double, so die == 1 + (uint32_t)(((uint64_t)u * sides) >> 32).
 *  4. Math.round(x) rounds halves toward +infinity: Math.round(-2.5) == -2, and
 *     Math.round(-50.5) == -50. C's round()/lround() round halves away from zero and are
 *     WRONG here. Use iwd_js_round() = floor(x + 0.5) with the exact-tie care it documents.
 *  5. Math.floor(a / b) on negative numbers floors toward -infinity; C integer '/'
 *     truncates toward zero. Use iwd_floor_div() / iwd_mod() (yearStart, isLeap, hour
 *     wrapping, tenday maths all depend on this for negative years / hours).
 *  6. JS '%' on doubles is C fmod() (sign of the dividend). mod(a, n) in harptos.js is
 *     fmod(fmod(a, n) + n, n): keep that exact shape for the moon age.
 *  7. Keep the JS order of floating point operations exactly as written in the reference
 *     (e.g. Math.PI * (h - 2) / 10 is (PI * (h - 2)) / 10; knot() is
 *     ((double)x / 4294967295.0) * 2 - 1 with divisor 2^32 - 1, not 2^32).
 *     Build engine.c with -ffp-contract=off and never with -ffast-math, so no FMA
 *     contraction or reassociation changes the last bit. Use double everywhere (no float,
 *     no long double). M_PI is not ISO C: use IWD_PI below (the same double as Math.PI).
 *  8. Math.cos: V8's Math.cos is fdlibm's cos. Platform cos() (glibc, msvcrt, UCRT) differs
 *     from it in the last bit for ~3% of arguments, so engine.c carries an exact port of
 *     fdlibm's cos (iwd_js_cos) and uses it for the diurnal temperature term and the moon's
 *     illuminated fraction. With it EVERY double the engine produces (rng, knots, offsets,
 *     temperature trace, moon age/phase/illum, scene blends) matches node bit for bit, on
 *     every platform; the parity tests compare raw IEEE-754 bits with no tolerance.
 *  9. "x | 0" (ToInt32) and Number.isInteger are needed only when reading JSON; that is
 *     store.c's business (see store.h).
 * 10. Day numbers ("abs") are int32_t here. The web app accepts any integer double; the
 *     native app restricts dates to years IWD_YEAR_MIN..IWD_YEAR_MAX and treats anything
 *     outside IWD_ABS_MIN..IWD_ABS_MAX as invalid. Do intermediate year maths in int64_t.
 */
#ifndef IWD_ENGINE_H
#define IWD_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================
 * Constants
 * ============================================================================== */

#define IWD_PI 3.141592653589793      /* Math.PI */

#define IWD_EVENT_COUNT      12       /* weather event ids 1..12 */
#define IWD_MAX_DAYS         3650     /* app.js MAX_DAYS: most weather days an almanac records */
#define IWD_SETTLE_HOUR      12       /* app.js SETTLE_HOUR: clock of a freshly opened day */

/* 1 Hammer 1489 DR = Harptos.toAbs({year:1489, month:0, day:1}) */
#define IWD_DEFAULT_START_ABS 543858

/* Native date range (pitfall 10). ABS_MIN = yearStart(-99999), ABS_MAX = yearStart(100000)-1. */
#define IWD_YEAR_MIN (-99999)
#define IWD_YEAR_MAX 99999
#define IWD_ABS_MIN  (-36524634)
#define IWD_ABS_MAX  36524999

/* Temperature model (engine.js) */
#define IWD_NOISE_AMP   6             /* +-6 F value noise */
#define IWD_DIURNAL_AMP 3             /* -3 F at 02:00, +3 F at 12:00 */
#define IWD_KNOT_HOURS  4             /* noise knots every 4 absolute hours */

/* Selune (harptos.js) */
#define IWD_MOON_PERIOD 30.4375       /* days; 30 d 10.5 h */
#define IWD_FULL_EPOCH  501123        /* yearStart(1372): full at the first instant of 1 Hammer 1372 DR */
#define IWD_NEW_EPOCH   501138.21875  /* FULL_EPOCH + MOON_PERIOD / 2 */

/* ================================================================================
 * JS-semantics helpers (exposed so store.c, the UI and the tests share one definition)
 * ============================================================================== */

/* Math.round: nearest integer, halves toward +infinity. Implemented as
 *   double f = floor(x); r = (x - f >= 0.5) ? f + 1 : f;      (exact for all finite x)
 * and a zero result keeps the sign of x, as Math.round(-0.3) === -0 does. */
double  iwd_js_round(double x);
/* Math.cos exactly as V8 computes it (fdlibm port; libm only for |x| > 2^19*pi/2).
 * Use it wherever a cosine must agree with the web app; plain cos() is fine for drawing. */
double  iwd_js_cos(double x);
/* floor(a / b) for b > 0, correct for negative a. */
int64_t iwd_floor_div(int64_t a, int64_t b);
/* ((a % n) + n) % n for n > 0: always in [0, n). */
int64_t iwd_mod(int64_t a, int64_t n);

/* UTF-8 -> UTF-16 exactly as node's Buffer#toString('utf8') / TextDecoder decode it (WHATWG:
 * every maximal invalid subpart becomes one U+FFFD; code points above U+FFFF become a
 * surrogate pair). Writes at most cap units and returns the total count (like snprintf), so
 * iwd_utf8_to_utf16(s, NULL, 0) is JS `str.length`. out may be NULL. */
size_t iwd_utf8_to_utf16(const char *utf8, uint16_t *out, size_t cap);

/* ================================================================================
 * Seeded RNG (engine.js hashSeed / createRng)
 * ============================================================================== */

/* cyrb53-style hash of a string -> 32-bit unsigned. Hashes UTF-16 code units (pitfall 2).
 * Invalid UTF-8 bytes are treated as U+FFFD. */
uint32_t iwd_hash_seed(const char *utf8);
uint32_t iwd_hash_seed_utf16(const uint16_t *units, size_t count);

/* hashSeed(String(seed) + '|temp'): the separate hash that drives temperature noise. */
uint32_t iwd_temp_hash(const char *seed_utf8);

/* mulberry32 with exposed, resumable state. */
typedef struct IwdRng { uint32_t state; } IwdRng;

void     iwd_rng_init(IwdRng *rng, uint32_t state);
uint32_t iwd_rng_next_u32(IwdRng *rng);         /* the 32-bit value before the /2^32 */
double   iwd_rng_next(IwdRng *rng);             /* in [0, 1): next_u32 / 4294967296.0 */
int      iwd_rng_die(IwdRng *rng, int sides);   /* 1..sides */

/* IWD.randomSeed(): WORD-XXXX. The caller supplies the entropy (crypto.getRandomValues in
 * the web app): word = WORDS[r0 % 16], suffix = (r1 % 0x10000) as 4 upper-case hex digits.
 * WORDS = RIME FROST AURIL HOAR SLEET GLACIER TUNDRA BOREAL SPINE MAER DUN CAIRN KELVIN
 *         BREMEN TERMALAINE ICEFANG. */
#define IWD_RANDOM_SEED_CAP 16                  /* "TERMALAINE-FFFF" + NUL */
size_t iwd_random_seed(uint32_t r0, uint32_t r1, char *buf, size_t cap);

/* ================================================================================
 * Rules data
 * ============================================================================== */

typedef enum IwdWindKey { IWD_WIND_NONE = 0, IWD_WIND_MILD, IWD_WIND_MODERATE, IWD_WIND_SEVERE, IWD_WIND_COUNT } IwdWindKey;

typedef struct IwdWind {
    const char *key;      /* "none" "mild" "moderate" "severe" */
    const char *label;    /* "No Wind" "Mild Wind" "Moderate Wind" "Severe Wind" */
    int         level;    /* 0..3 */
} IwdWind;

const IwdWind *iwd_wind(IwdWindKey key);
/* Wind meter (4 pips): lit pips = level + (level ? 1 : 0)  -> 0, 2, 3, 4. */
int iwd_wind_meter(IwdWindKey key);

/* Effect categories. The first ten come from engine.js event data; NATURAL, AURORA and
 * CHILL are the three per-hour effects app.js synthesises (see SPEC.md "Effects in Play"). */
typedef enum IwdCat {
    IWD_CAT_SIGHT = 0, IWD_CAT_HEARING, IWD_CAT_RANGED, IWD_CAT_ATTACK, IWD_CAT_COLD,
    IWD_CAT_TRAVEL, IWD_CAT_FLAME, IWD_CAT_CONCENTRATION, IWD_CAT_HAZARD, IWD_CAT_LIGHT,
    IWD_CAT_NATURAL, IWD_CAT_AURORA, IWD_CAT_CHILL,
    IWD_CAT_COUNT
} IwdCat;

/* "sight" "hearing" "ranged" "attack" "cold" "travel" "flame" "concentration" "hazard"
 * "light" "natural" "aurora" "chill" */
const char *iwd_cat_key(IwdCat cat);
/* app.js CAT_LABEL: "Sight" "Hearing" "Ranged attacks" "Attacks" "Extreme cold" "Travel"
 * "Extinguishing winds" "Concentration" "Hazard" "Unnatural light" "Natural light"
 * "Auril's Aurora" "Cold table" */
const char *iwd_cat_label(IwdCat cat);

typedef struct IwdEffect {
    IwdCat      cat;
    const char *text;     /* exact engine.js text, e.g. "No [Sight] Perception checks." */
} IwdEffect;

/* ev.light in engine.js */
typedef enum IwdEventLight { IWD_EVLIGHT_NONE = 0, IWD_EVLIGHT_DIM, IWD_EVLIGHT_LIGHTNING } IwdEventLight;

typedef struct IwdEventDef {
    int              id;         /* 1..12 */
    const char      *name;       /* "Clear Skies" ... "Wind Vortex" */
    int              dice;       /* number of d4 rolled for the duration: 1 or 2 */
    IwdWindKey       wind;
    int              temp_f;     /* the PDF's temperature, e.g. -50 */
    IwdEventLight    light;
    const char      *moon;       /* "½" "¾" "Full" (UTF-8), or NULL when the event has none */
    bool             natural;    /* PDF asterisk: natural light / Auril's Aurora can reach the party */
    uint8_t          next[3];    /* ** table: 1st / 2nd / 3rd option */
    const char      *flavor;
    const IwdEffect *effects;    /* in engine.js order (spread constants expanded in place) */
    int              n_effects;
} IwdEventDef;

/* id 1..12 -> definition; anything else -> NULL. */
const IwdEventDef *iwd_event(int id);

/* app.js SEVERITY: {1:1, 2:1, 5:1, 9:2, 3:2, 10:3, 6:3, 4:3, 11:3, 12:4, 7:4, 8:5}. */
int iwd_severity(int event_id);                 /* 1..5 (0 for a bad id) */
/* app.js SEV_LABEL: 0 "—" (U+2014), 1 "Calm", 2 "Uneasy", 3 "Dangerous", 4 "Deadly", 5 "Lethal". */
const char *iwd_severity_label(int severity);

/* calendar.js COLORS: the event's colour as 0xRRGGBB (timeline segments, day strips, cells).
 * 1 a6ecff  2 8ccbeb  3 6d819b  4 46526b  5 dcf3ff  6 a9c5e2
 * 7 7aa7d8  8 f5fbff  9 b9c7d1 10 b49cff 11 7fd1d9 12 5ee0c8 */
uint32_t iwd_event_rgb(int event_id);
/* app.js inkFor(): true when the colour itself is dark, i.e.
 * 0.2126 r + 0.7152 g + 0.0722 b < 140 (ids 3 and 4). Labels on such a segment use the
 * light ink #e9f6ff; labels on every other segment use the dark ink #04213a. */
bool iwd_event_rgb_is_dark(int event_id);

/* Transition rule (nextIndexFor): 1d4: 1-2 -> 0, 3 -> 1, 4 -> 2.  2d4: 2-4 -> 0, 5-6 -> 1, 7-8 -> 2. */
int iwd_next_index_for(int dice, int duration);

/* ================================================================================
 * Light cycles (engine.js CYCLES / inWindow / lightAt)
 * ============================================================================== */

typedef enum IwdCycleKey { IWD_CYCLE_MOON = 0, IWD_CYCLE_TWILIGHT, IWD_CYCLE_AURORA, IWD_CYCLE_COUNT } IwdCycleKey;

typedef struct IwdCycle {
    const char *label;   /* "Moon Cycle" "Twilight" "Auril's Aurora" */
    int from, to;        /* hours: moon 20..6, twilight 10..14, aurora 22..2 (end exclusive, wraps) */
} IwdCycle;

const IwdCycle *iwd_cycle(IwdCycleKey key);
/* w.from < w.to ? (h >= from && h < to) : (h >= from || h < to) */
bool iwd_in_window(int hour, const IwdCycle *w);

typedef enum IwdLightLevel { IWD_LIGHT_NONE = 0, IWD_LIGHT_DIM, IWD_LIGHT_DARK } IwdLightLevel;

enum {                              /* IwdLight.sources bits, in the JS push order */
    IWD_SRC_MOON      = 1,          /* "<ev.moon> Moon", e.g. "½ Moon" */
    IWD_SRC_AURORA    = 2,          /* "Auril's Aurora" */
    IWD_SRC_TWILIGHT  = 4,          /* "Twilight" */
    IWD_SRC_LIGHTNING = 8           /* "Rapid lightning" (lightning events only) */
};

typedef struct IwdLight {
    IwdLightLevel level;            /* JS level 'none' | 'dim' | 'dark' */
    unsigned      sources;          /* IWD_SRC_* bits */
    bool          aurora_visible;   /* ev.natural && aurora window; false for 'none'/'lightning' events */
    bool          moon, twilight, aurora;   /* raw cycle windows at this hour */
} IwdLight;

/* lightAt(ev, hour): h = ((floor(hour) % 24) + 24) % 24.
 *   ev.light none      -> level NONE, no sources, aurora_visible false
 *   ev.light lightning -> level DIM, sources LIGHTNING, aurora_visible false
 *   ev.light dim       -> sources from the open windows; none open -> level DARK
 *                         (aurora_visible keeps ev.natural && aurora in both cases) */
IwdLight iwd_light_at(const IwdEventDef *ev, double hour);

/* light.label:
 *   NONE                -> "No natural light"
 *   lightning           -> "Dim light — rapid lightning"            (U+2014 em dash)
 *   DARK                -> "Between light cycles: no natural source"
 *   DIM                 -> "Dim light — " + sources joined by ", " in the order
 *                          "<moon> Moon", "Auril's Aurora", "Twilight"
 *                          e.g. "Dim light — Full Moon, Auril's Aurora" */
#define IWD_LIGHT_LABEL_CAP 96
size_t iwd_light_label(const IwdEventDef *ev, const IwdLight *light, char *buf, size_t cap);
/* "none" "dim" "dark" */
const char *iwd_light_level_key(IwdLightLevel level);
/* Hero badge text (app.js): DIM "Dim light", DARK "Between light cycles", NONE "No natural light". */
const char *iwd_light_badge(IwdLightLevel level);

/* ================================================================================
 * Temperature (engine.js knot / diurnal / tempOffsetAt / tempOffset)
 * ============================================================================== */

/* Value-noise knot in [-1, 1] for integer knot index n (may be negative) and hash sh. */
double iwd_temp_knot(uint32_t temp_hash, int32_t n);
/* Diurnal swing in F at an hour of day (any real; wrapped with fmod):
 *   h in [2, 12]: -3 * cos(PI * (h - 2) / 10)      else: 3 * cos(PI * (((h - 12) + 24) % 24) / 14) */
double iwd_temp_diurnal(double hour);
/* Offset in F at a fractional absolute hour (day_index * 24 + hour of day):
 *   k = abs_hour / 4; n = floor(k); f = k - n; s = f*f*(3 - 2*f);
 *   6 * (knot(n) * (1 - s) + knot(n + 1) * s) + diurnal(abs_hour) */
double iwd_temp_offset_at(uint32_t temp_hash, double abs_hour);
/* tempOffset(seedText, dayIndex, hour) */
double iwd_temp_offset(const char *seed_utf8, int32_t day_index, double hour);

/* app.js toUnit for C: Math.round((f - 32) * 5 / 9), halves toward +infinity. */
int iwd_f_to_c(int temp_f);

/* Cold table (COLD_TIERS / coldDC): DC +-1 per 10 F away from the tier's anchor. */
typedef struct IwdColdTier {
    const char *key;     /* "cold" "frigid" "severe" "extreme" "deathly" */
    const char *name;    /* "Cold" "Frigid Cold" "Severe Cold" "Extreme Cold" "Deathly Cold" */
    int dc;              /* 10 15 20 25 28 */
    int f;               /* anchor F:   0 -40 -80 -110 -130 */
    int c;               /* table's own C anchor (unused by the UI): -20 -40 -60 -80 -100 */
} IwdColdTier;

#define IWD_COLD_TIER_COUNT 5
const IwdColdTier *iwd_cold_tier(int index);     /* 0..4 */

typedef struct IwdCold {
    const IwdColdTier *tier;
    int tier_index;      /* 0..4 */
    int dc;
    int delta;           /* dc - tier->dc */
} IwdCold;

/* coldDC(tempF): tier = the LAST tier whose anchor f >= tempF (tier 0 when tempF > 0);
 *   tempF > tier.f ? max(0, tier.dc - floor((tempF - tier.f) / 10))
 *                  : tier.dc + floor((tier.f - tempF) / 10)
 * e.g. -50 -> Frigid 16 (+1); -79 -> Frigid 18; -80 -> Severe 20; -139 -> Deathly 28; 5 -> Cold 10. */
IwdCold iwd_cold_dc(int temp_f);

/* ================================================================================
 * The chain (engine.js startEvent / newChain / nextDay)
 * ============================================================================== */

typedef enum IwdCauseType { IWD_CAUSE_GENESIS = 0, IWD_CAUSE_TRANSITION = 1 } IwdCauseType;

/* A rolled event as it lives in the chain (JS object from startEvent). */
typedef struct IwdActive {
    uint8_t id;            /* event id 1..12 */
    uint8_t n_rolls;       /* == dice (1 or 2) */
    uint8_t rolls[2];      /* each 1..4 */
    uint8_t duration;      /* sum of rolls, 1..8 hours */
    uint8_t elapsed;       /* hours already played, 0..duration */
    uint8_t next_idx;      /* 0..2 */
    uint8_t next;          /* id of the event that follows */
    uint8_t cause_type;    /* IwdCauseType */
    uint8_t cause_arg;     /* genesis: the d12 result; transition: id of the previous event */
    uint8_t start_hour;    /* 0..23 */
    int32_t start_day;     /* 1-based chain day the event begins on */
} IwdActive;

typedef struct IwdChain {
    uint32_t  rng_state;   /* resumable mulberry32 state; starts at hashSeed(seed) */
    uint32_t  temp_hash;   /* hashSeed(seed + '|temp'), cached */
    int32_t   day_count;   /* days generated so far */
    bool      has_current; /* false until the first day is generated */
    IwdActive current;     /* event in progress (or rolled for the coming midnight) */
} IwdChain;

/* One stretch of a single event inside a day. */
typedef struct IwdSegment {
    uint8_t id;
    uint8_t start, end;          /* hours, 0 <= start < end <= 24 */
    uint8_t duration;            /* the event's full rolled duration */
    uint8_t n_rolls;
    uint8_t rolls[2];
    uint8_t next;                /* id of the event after this one */
    uint8_t next_idx;
    uint8_t elapsed_before;      /* hours of the event played before this segment */
    bool    from_prev_day;       /* elapsed_before > 0 && start == 0 */
    bool    into_next_day;       /* end == 24 && the event is not finished */
    uint8_t change_at;           /* start + remaining: hour the event ends, relative to this
                                    day; may exceed 24 (max 31) */
    uint8_t cause_type;
    uint8_t cause_arg;
} IwdSegment;

#define IWD_MAX_SEGMENTS 24

/* One generated day. ~0.5 KB, so 3650 days stay under 2 MB.
 * Differences from the JS record, all derivable:
 *   hours[h].light  -> iwd_light_at(iwd_event(hour_id[h]), h)
 *   log             -> the segments with elapsed_before == 0, entry hour = segment start
 *                      (iwd_day_log); a genesis roll has cause_type GENESIS
 *   seed            -> owned by the caller */
typedef struct IwdDay {
    int32_t    day;                          /* 1-based chain day */
    uint8_t    n_segments;
    IwdSegment segments[IWD_MAX_SEGMENTS];
    uint8_t    hour_id[24];                  /* event id per hour */
    int16_t    temp_f[24];                   /* Math.round(ev.tempF + tempOffsetAt(sh, (day-1)*24 + h)) */
    int16_t    temp_min, temp_max;           /* over the 24 hourly values */
    bool       has_carried_in;               /* false only for day 1 */
    IwdActive  carried_in;                   /* chain.current before this day (copy) */
    IwdActive  carry_out;                    /* chain.current after this day (copy) */
} IwdDay;

/* newChain(seed): rng_state = hashSeed(seed), no current event, day_count 0. */
void iwd_chain_init(IwdChain *chain, const char *seed_utf8);

/* nextDay(chain): generates the next 24 h into *out and advances the chain.
 *   - First day: d12 picks the event (cause GENESIS, cause_arg = d12), then its xd4.
 *   - Later days never re-roll the d12: the carried event continues (mid-event if needed).
 *   - When an event's hours run out, the next event's duration is rolled IMMEDIATELY
 *     (even at hour 24: that event then has start_day = day + 1, start_hour 0, elapsed 0,
 *     and opens the next day as a log entry at 00:00). RNG call order must match the JS:
 *     d12 (first day only), then for each started event its dice in order.
 *   - Temperatures do not touch the chain RNG. */
void iwd_next_day(IwdChain *chain, IwdDay *out);

/* The segment covering hour h (0..23); NULL if h is out of range. */
const IwdSegment *iwd_day_segment_at(const IwdDay *day, int hour);
/* Ledger entries: pointers to the segments whose event starts in this day (elapsed_before
 * == 0), in order. Returns the count (<= 24). */
int iwd_day_log(const IwdDay *day, const IwdSegment *out[IWD_MAX_SEGMENTS]);
/* calendar.js dominant(day): the event id with the most hours; ties go to the LOWEST id
 * (JS integer keys enumerate ascending and the sort is stable). */
int iwd_day_dominant(const IwdDay *day);

/* app.js tempAt(day, t) for the temperature trace: tt = clamp(t, 0, 23.999);
 *   EVENTS[day.hour_id[floor(tt)]].tempF + tempOffsetAt(temp_hash, (day.day - 1) * 24 + tt) */
double iwd_day_temp_at(const IwdDay *day, uint32_t temp_hash, double t);

/* ================================================================================
 * Scene looks (js/scene.js LOOKS / band / paramsFor) - pure data for tints and audio
 * ============================================================================== */

/* Visual signature per event; every field is 0..1. The native UI uses `wind` (audio and
 * optional streak overlays), `dark`, `fog`, `whiteout` (contrast switch) and may ignore the
 * rest. Values are listed in SPEC.md. */
typedef struct IwdLook {
    double cloud, cloud_dark, snow, flake, wind, hail, fog, whiteout,
           lightning, vortex, stars, moon, dark, extinguish;
} IwdLook;

const IwdLook *iwd_look(int event_id);

/* Callback: event id at an hour relative to the viewed day; may be < 0 or >= 24
 * (store.h's almanac_event_at walks into neighbouring days). */
typedef int (*IwdEventAtFn)(void *user, int hour);

/* Scene.paramsFor(eventAt, t): base = floor(t), frac = t - base;
 *   a = LOOKS[at(base)], b = LOOKS[at(base + 1)],
 *   p = at(base - 1) != at(base) ? LOOKS[at(base - 1)] : a
 *   wIn  = frac > 0.6  ? smooth((frac - 0.6) / 0.4) * 0.5 : 0
 *   wOut = frac < 0.25 ? (1 - smooth(frac / 0.25)) * 0.5 : 0      smooth(t) = t*t*(3 - 2t)
 *   v = a; if (b != a) v = lerp(v, b, wIn); if (p != a) v = lerp(v, p, wOut)   per field */
void iwd_look_at(IwdEventAtFn at, void *user, double t, IwdLook *out);

/* scene.js band(h, from, to, fade): 0..1 strength of a wrapping hour window with soft
 * edges `fade` hours wide centred on each edge. The web scene uses
 *   twilight band(h, 10, 14, 1.6), moon-up band(h, 20, 6, 1.2), aurora-up band(h, 22, 2, 0.8). */
double iwd_band(double hour, double from, double to, double fade);

/* ================================================================================
 * Calendar of Harptos (js/harptos.js)
 * ============================================================================== */
/* Dates are absolute day numbers: abs 0 = 1 Hammer 0 DR; negative years work.
 * 12 months of 30 days (three tendays) + festival days that belong to no month.
 * Shieldmeet follows Midsummer in years divisible by 4 (mod 4 == 0, also for negative
 * years and year 0), giving 366 days. */

typedef struct IwdMonth {
    const char *name;     /* Hammer Alturiak Ches Tarsakh Mirtul Kythorn Flamerule Eleasis Eleint Marpenoth Uktar Nightal */
    const char *common;   /* Deepwinter | The Claw of Winter | The Claw of the Sunsets | The Claw of the Storms |
                             The Melting | The Time of Flowers | Summertide | Highsun | The Fading | Leaffall |
                             The Rotting | The Drawing Down */
} IwdMonth;

const IwdMonth *iwd_month(int month);            /* 0..11 */

typedef enum IwdFestival {
    IWD_FEST_NONE = -1,
    IWD_FEST_MIDWINTER = 0,       /* after month 0  (Hammer) */
    IWD_FEST_GREENGRASS,          /* after month 3  (Tarsakh) */
    IWD_FEST_MIDSUMMER,           /* after month 6  (Flamerule) */
    IWD_FEST_SHIELDMEET,          /* after month 6, the day after Midsummer, leap years only */
    IWD_FEST_HIGHHARVESTIDE,      /* after month 8  (Eleint) */
    IWD_FEST_FEASTOFTHEMOON,      /* after month 10 (Uktar) */
    IWD_FEST_COUNT
} IwdFestival;

typedef struct IwdFestivalDef {
    const char *key;      /* "midwinter" "greengrass" "midsummer" "shieldmeet" "highharvestide" "feastofthemoon" */
    const char *name;     /* "Midwinter" "Greengrass" "Midsummer" "Shieldmeet" "Highharvestide" "Feast of the Moon" */
    int  after;           /* index of the month the festival follows */
    bool leap_only;
} IwdFestivalDef;

const IwdFestivalDef *iwd_festival(IwdFestival f);          /* NULL for NONE / out of range */
IwdFestival iwd_festival_from_key(const char *key);         /* IWD_FEST_NONE if unknown */

bool    iwd_is_leap(int32_t year);                          /* mod(year, 4) == 0 */
int     iwd_days_in_year(int32_t year);                     /* 365 / 366 */
int32_t iwd_year_start(int32_t year);                       /* 365*y + floor((y + 3) / 4) */

typedef struct IwdDate {
    int32_t     abs;
    int32_t     year;
    int         month;          /* 0..11, or -1 on a festival day (JS null) */
    int         day;            /* 1..30, or 0 on a festival day (JS null) */
    IwdFestival festival;       /* IWD_FEST_NONE on a month day */
    int         day_of_year;    /* 1..366 */
    int         tenday_index;   /* 0..2, or -1 on a festival day */
    int         day_of_tenday;  /* 1..10, or 0 on a festival day */
} IwdDate;

/* toAbs. Pass festival = IWD_FEST_NONE and month 0..11, day 1..30 for a month day, or a
 * festival (month/day ignored). Returns false for impossible dates (Shieldmeet in a common
 * year, day 31, month 12, year outside IWD_YEAR_MIN..IWD_YEAR_MAX). */
bool iwd_to_abs(int32_t year, int month, int day, IwdFestival festival, int32_t *abs_out);
/* fromAbs. abs must be within IWD_ABS_MIN..IWD_ABS_MAX (clamped otherwise). */
IwdDate iwd_from_abs(int32_t abs);

/* format(abs, {short}):
 *   month day : "1 Hammer 1489 DR"        short: "1 Hammer"
 *   festival  : "Midwinter, 1489 DR"      short: "Midwinter"
 * Negative years print with an ASCII '-' ("-5 DR"), as String(year) does. */
#define IWD_DATE_CAP 48
size_t iwd_format_date(int32_t abs, bool short_form, char *buf, size_t cap);

/* calendar.js monthOf(abs): the month a date is shown in; a festival lives in the month it follows. */
void iwd_month_of(int32_t abs, int32_t *year_out, int *month_out);

/* monthLayout(year, month): the 30 days are first_abs .. first_abs + 29; then 0..2
 * festival days that follow the month (Midsummer and, in leap years, Shieldmeet after
 * Flamerule), in calendar order. */
typedef struct IwdMonthLayout {
    int32_t     year;
    int         month;
    const char *name, *common;
    int32_t     first_abs;
    int         n_festivals;
    struct { int32_t abs; IwdFestival festival; } festivals[2];
} IwdMonthLayout;

void iwd_month_layout(int32_t year, int month, IwdMonthLayout *out);

/* ---------- Selune ---------- */

typedef struct IwdMoon {
    double      age;       /* days since new, [0, MOON_PERIOD): mod(abs + 0.5 - (NEW_EPOCH + off), MOON_PERIOD) */
    double      phase;     /* age / MOON_PERIOD, [0, 1) */
    double      illum;     /* (1 - cos(2*PI*phase)) / 2 */
    bool        waxing;    /* age < MOON_PERIOD / 2 */
    int         octant;    /* index into the phase names, 0..7 */
    const char *name;      /* "New Moon" "Waxing Crescent" "First Quarter" "Waxing Gibbous"
                              "Full Moon" "Waning Gibbous" "Last Quarter" "Waning Crescent" */
    bool        is_new;    /* the new-moon instant falls inside [abs, abs + 1) */
    bool        is_full;   /* the full-moon instant falls inside [abs, abs + 1) */
} IwdMoon;

/* moon(abs, offsetDays). offset_days: NaN is treated as 0 (Number(x) || 0); so are +-Infinity
 * (the web would produce NaN fields; store.c never passes a non-finite offset).
 *   hitsDay(epoch, abs): k = ceil((abs - epoch) / MOON_PERIOD); epoch + k*MOON_PERIOD < abs + 1
 *   is_new  = hitsDay(NEW_EPOCH + off, abs);  is_full = hitsDay(FULL_EPOCH + off, abs)
 *   oct = floor(phase * 8 + 0.5) % 8
 *   if (oct == 0 && !is_new)  oct = waxing ? 1 : 7;
 *   if (oct == 4 && !is_full) oct = waxing ? 3 : 5;
 *   if (is_new) oct = 0;  if (is_full) oct = 4;      (in that order)
 * Check: moon(543858, 0) = age 16.46875, phase 0.5410677618069816, illum 0.98344646...,
 * waning, "Waning Gibbous". */
IwdMoon iwd_moon(int32_t abs, double offset_days);

/* "NN" of the day panel's "NN% lit": Math.round(illum * 100). */
int iwd_moon_percent(const IwdMoon *moon);

/* ---------- Recurrence ---------- */

typedef enum IwdRepeat {
    IWD_REPEAT_NONE = 0, IWD_REPEAT_NEWMOON, IWD_REPEAT_FULLMOON,
    IWD_REPEAT_TENDAY, IWD_REPEAT_MONTHLY, IWD_REPEAT_YEARLY,
    IWD_REPEAT_COUNT
} IwdRepeat;

/* "none" "newmoon" "fullmoon" "tenday" "monthly" "yearly" */
const char *iwd_repeat_key(IwdRepeat r);
/* "Never" "Every new moon" "Every full moon" "Every tenday" "Every month" "Every year" */
const char *iwd_repeat_label(IwdRepeat r);
/* Unknown key -> IWD_REPEAT_COUNT (callers treat that as "invalid", clean() maps it to NONE). */
IwdRepeat iwd_repeat_from_key(const char *key);

/* occursOn(event, abs, moonOffset), with start = the event's anchor day:
 *   abs < start                -> false
 *   NEWMOON / FULLMOON         -> moon(abs).is_new / is_full  (the anchor day counts ONLY
 *                                 if it is itself such a day)
 *   abs == start               -> true
 *   YEARLY   -> festival anchor: same festival (Shieldmeet: leap years only);
 *               otherwise same month and day
 *   MONTHLY  -> neither day is a festival and same day of month
 *   TENDAY   -> neither day is a festival and same day of tenday
 *   NONE     -> false */
bool iwd_occurs_on(int32_t start_abs, IwdRepeat repeat, int32_t abs, double moon_offset);

/* occurrences(event, from, to, moonOffset): the abs days in [max(from, start), to] on which
 * the event occurs, ascending. Writes at most cap entries; returns the total count (which
 * may exceed cap, like snprintf). */
int iwd_occurrences(int32_t start_abs, IwdRepeat repeat, int32_t from, int32_t to,
                    double moon_offset, int32_t *out, int cap);

/* ================================================================================
 * Derived per-hour presentation (app.js renderHour / announce / ledger)
 * Pure functions of engine data, kept here so host tests can compare them with the JS.
 * ============================================================================== */

/* Chart scale of the temperature trace (app.js T_MIN / T_MAX), in F. */
#define IWD_CHART_T_MIN (-142)
#define IWD_CHART_T_MAX (-38)

/* tempNote: temp_f <= -100 "killing cold"; <= -80 "flesh-freezing"; else "air temperature". */
const char *iwd_temp_note(int temp_f);

/* tierAnchor(t):  F: minus(t.f) + "°F"                       e.g. "−40°F"
 *                 C: minus(round((t.f - 32) * 5 / 9)) + "°C (" + minus(t.f) + "°F)"
 *                                                            e.g. "−40°C (−40°F)" */
#define IWD_TIER_ANCHOR_CAP 40
size_t iwd_fmt_tier_anchor(const IwdColdTier *tier, bool celsius, char *buf, size_t cap);
/* dcStep():  F "±1 per 10°F"     C "±1 per 10°F (≈5.6°C)" */
const char *iwd_dc_step(bool celsius);
/* Tier badge tooltip: "Cold table: DC <tier.dc> at <tierAnchor>, <dcStep>" */
#define IWD_TIER_TIP_CAP 96
size_t iwd_fmt_tier_tip(const IwdCold *cold, bool celsius, char *buf, size_t cap);

/* Natural light stat: light label + (level == NONE ? " — Blinded" : ""). */
size_t iwd_fmt_light_value(const IwdEventDef *ev, const IwdLight *light, char *buf, size_t cap);

/* Forecast for hour h of a day (app.js renderHour "forecast"). seg = the segment at h:
 *   next_id   = seg.next          change_at = seg.change_at (may be >= 24)
 *   left      = change_at - h     tomorrow  = change_at >= 24
 *   dice      = EVENTS[seg.id].dice, duration = seg.duration */
typedef struct IwdForecast {
    int  next_id;
    int  change_at;
    int  left;
    bool tomorrow;
    int  dice;
    int  duration;
} IwdForecast;

IwdForecast iwd_forecast(const IwdDay *day, int hour);
/* Plain text, the web's markup flattened:
 *   "<Next name> at <fmtHour(change_at)>[ tomorrow] (in <left> h · <dice>d4 rolled <duration>)"
 *   e.g. "Light Cloud Coverage at 03:00 tomorrow (in 5 h · 2d4 rolled 6)"
 * The web shows the name in bold and the parenthesis muted: name_len (optional) receives
 * the byte length of the leading name so the UI can style it. */
#define IWD_FORECAST_CAP 128
size_t iwd_fmt_forecast(const IwdForecast *f, char *buf, size_t cap, size_t *name_len);

/* The two effects app.js synthesises in front of the event's own list (exact strings). */
#define IWD_FX_TEXT_DARK   "Between light cycles: no moon, twilight or aurora this hour. The rules name no natural light source here; the [Sight] effects below still apply (DM may rule it darkness)."
#define IWD_FX_TEXT_AURORA "Auril's Aurora is visible. Characters who see it during a rest are magically healed: −1 exhaustion level and +1d4 HP per hour."

/* Effects in play for one hour, in display order:
 *   [AURORA if light.aurora_visible] or [NATURAL if light.level == DARK]   (never both)
 *   CHILL  "Cold table: <tier.name> — DC <dc> at <minus(toUnit(t))>°<F|C> (DC <tier.dc> at
 *           <tierAnchor>, <dcStep>" + (delta ? "; <+|−><|delta|> here)" : ")")
 *          e.g. "Cold table: Frigid Cold — DC 16 at −50°F (DC 15 at −40°F, ±1 per 10°F; +1 here)"
 *   then the event's own effects in engine.js order.
 * items[k].text points either at static data or at out->chill_text, so never copy an
 * IwdHourEffects by value (pass pointers). Header note: "<count> in play · <event name>". */
#define IWD_MAX_HOUR_EFFECTS 24
#define IWD_CHILL_TEXT_CAP   192
typedef struct IwdHourEffects {
    int       count;
    IwdEffect items[IWD_MAX_HOUR_EFFECTS];
    char      chill_text[IWD_CHILL_TEXT_CAP];
} IwdHourEffects;

void iwd_hour_effects(int event_id, int hour, int temp_f, bool celsius, IwdHourEffects *out);

/* Ledger "next" label (app.js): "always" when all three ** options are the same event
 * (Wind Vortex), else "1st option:" / "2nd option:" / "3rd option:" for next_idx 0..2. */
const char *iwd_next_option_label(const IwdEventDef *ev, int next_idx);

/* ================================================================================
 * Small text helpers shared by the UI and the tests (app.js / calendar.js)
 * ============================================================================== */

/* fmtHour(h): pad2(((floor(h) % 24) + 24) % 24) + ":00"   e.g. 26 -> "02:00". cap >= 6. */
size_t iwd_fmt_hour(int hour, char *buf, size_t cap);
/* fmtClock(t): tt = min(t, 23.999); "HH:MM" with MM = floor((tt - floor(tt)) * 60). cap >= 6. */
size_t iwd_fmt_clock(double t, char *buf, size_t cap);
/* minus(n): decimal integer with U+2212 MINUS SIGN instead of '-'   e.g. -50 -> "−50". */
size_t iwd_fmt_minus(int n, char *buf, size_t cap);
/* fmtTemp(f): minus(celsius ? iwd_f_to_c(f) : f) + "°"   e.g. "−46°". */
size_t iwd_fmt_temp(int temp_f, bool celsius, char *buf, size_t cap);
/* calendar.js relDays(n): 0 "Today", 1 "Tomorrow", -1 "Yesterday", n > 0 "In n days",
 * n < 0 "n days ago" (positive number printed). */
size_t iwd_rel_days(int32_t n, char *buf, size_t cap);
/* pad2: two-digit zero padded non-negative number, e.g. 7 -> "07", 24 -> "24". */
size_t iwd_fmt_pad2(int n, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IWD_ENGINE_H */
