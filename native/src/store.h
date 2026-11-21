/* store.h - Almanac state, persistence, backup import/export and app actions (pure C11).
 *
 * Mirrors the web app's state object S (js/app.js) and its snapshot()/clean()/applyState()
 * logic, plus the calendar-side helpers of js/calendar.js that need the event list. The UI
 * (main.c / ui_*.c) owns the window, animation and painting; it calls these functions for
 * every state change and reads derived values from them. See SPEC.md for the behaviour.
 *
 * Rules:
 *   - No Win32 types in this header. File IO goes through the alm_fs_* shim: store.c
 *     implements it under #ifdef _WIN32 with UTF-8 -> UTF-16 paths, _wfopen and
 *     MoveFileExW(MOVEFILE_REPLACE_EXISTING) (<windows.h> included only inside that
 *     #ifdef), and with fopen/rename elsewhere, so the host test build works with gcc.
 *   - Strings are UTF-8, heap allocated with malloc and owned by the Almanac. Every
 *     setter copies its input. alm_free() releases everything.
 *     So that any JS string survives load -> save byte for byte, strings read from JSON use
 *     two extensions: U+0000 is stored as the overlong pair C0 80 (never a raw NUL) and a
 *     lone UTF-16 surrogate from a "\udXXX" escape is stored in its 3-byte WTF-8 form; the
 *     writer turns both back into \u escapes. Raw invalid UTF-8 in a file becomes U+FFFD (as
 *     the web's UTF-8 decoder). A UTF-16 file (BOM FF FE / FE FF) is decoded too; a UTF-8 BOM
 *     is skipped. JSON nesting deeper than 256 levels is rejected (native limit).
 *   - Functions that can fail on allocation return false / a negative status; the
 *     Almanac is left valid (unchanged) on failure.
 *   - Not thread safe; one Almanac per process is expected.
 *
 * ---------------------------------------------------------------------------------
 * Persisted JSON (web app: localStorage key "iwd-weather-v2"; native: the save file)
 * ---------------------------------------------------------------------------------
 *  {
 *    "v": 2,
 *    "seed": "RIME-00FF",          string, non-empty
 *    "started": 1700000000000,     number: ms since 1970 when this expedition was rolled
 *    "startAbs": 543858,           integer: abs day of weather day 1 (index 0)
 *    "dayCount": 12,               integer 0..3650: days of weather generated
 *    "current": 11,                integer -1..dayCount-1: the party's "today" (index)
 *    "currentAbs": 543869,         integer or null: abs of today (kept even if current==-1)
 *    "view": 11,                   integer -1..dayCount-1: day on screen (index)
 *    "selAbs": 543869,             integer: selected calendar day (any abs, may have no weather)
 *    "time": 12,                   number [0, 24): clock of the viewed day
 *    "unit": "F",                  "F" | "C"
 *    "moonOffset": 0,              number [-30, 30]: Selune phase shift in days
 *    "yearNames": {"1489": "Year of the Warrior Princess"},
 *    "notes": {"543869": "free text"},
 *    "events": [{"id": "e...", "abs": 543869, "title": "...", "type": "natural",
 *                "repeat": "none", "details": ""}],      (+ "days": n when n > 1, last)
 *    "ui": {"chainNoteHidden": false, "calYear": 1489, "calMonth": 0}
 *  }
 *  The web writes it with JSON.stringify(snapshot()) (compact). A backup file is
 *  JSON.stringify(Object.assign({app: "icewind-dale-almanac", exported: <ISO 8601>},
 *  snapshot()), null, 2): the same object with "app" and "exported" first, 2-space indent.
 *  Object key order of yearNames / notes follows JS rules: keys that are array indices
 *  (integers 0..4294967294) ascending, then all other keys (negative numbers) in insertion
 *  order. The native writer reproduces this, so its output is byte-identical to the web's
 *  for the same state (numbers formatted as ECMAScript Number.prototype.toString).
 *
 *  Native save file: the same compact v2 object followed by one extra key "native"
 *  (window placement, sound, interior, dice ledger open) that the web app ignores. "native"
 *  is never exported. Native key order: "sound", "interior", "maximized", "ledger", then
 *  "x" "y" "w" "h" when the
 *  window rect is known; an old "tab" key is ignored.
 *
 *  v1 migration (web: localStorage "iwd-weather-v1", used only when no v2 save exists):
 *  an object with a non-empty string "seed" becomes
 *    {seed, started, startAbs: IWD_DEFAULT_START_ABS, dayCount: n, current: n - 1,
 *     view, time, unit}     with n = clamp(dayCount | 0, 0, 3650)
 *  and then goes through clean(). Native rule: a loaded file whose "v" is absent or 1 is
 *  treated as v1 (only those keys survive); any other "v" is cleaned as v2. Imports always
 *  go straight through clean(), exactly like the web's importBackup().
 *
 * ---------------------------------------------------------------------------------
 * clean(): any stored shape -> a complete valid state (applied on load and import)
 * ---------------------------------------------------------------------------------
 *  JS helpers:  isInt(x) = typeof x == number && finite && integral (Number.isInteger)
 *               x | 0    = ToInt32 (NaN/Inf/non-number -> 0, truncate, wrap mod 2^32)
 *  seed        string && non-empty ? s.seed : randomSeed()
 *  started     typeof number ? s.started : 0
 *  startAbs    isInt ? s.startAbs : IWD_DEFAULT_START_ABS
 *  dayCount    clamp(s.dayCount | 0, 0, 3650)
 *  current     isInt && -1 <= v < dayCount ? s.current : dayCount - 1
 *  currentAbs  isInt(s.currentAbs) ? s.currentAbs : (current >= 0 ? startAbs + current : null);
 *              then if current >= 0: currentAbs = startAbs + current (always)
 *  view        isInt && 0 <= v < dayCount ? s.view : current
 *  selAbs      isInt ? s.selAbs : startAbs + max(0, view)
 *  time        typeof number && 0 <= t < 24 ? s.time : 12 (IWD_SETTLE_HOUR)
 *  unit        s.unit === "C" ? "C" : "F"
 *  moonOffset  typeof number && finite ? clamp(s.moonOffset, -30, 30) : 0
 *  yearNames, notes   cleanMap: for each key k with isInt(+k) (JS ToNumber of the key
 *              string) whose value is a string with a non-blank trim(): out[+k] = value
 *              (value kept untrimmed; key renormalised, e.g. "012" -> 12)
 *  events      array ? each cleanEvent, dropping nulls : []
 *     cleanEvent: object, isInt(abs), title string with non-blank trim(), else dropped;
 *       id      non-empty string ? id : "e" + 8 random base-36 chars
 *       title   trim(), then first 120 UTF-16 code units
 *       type    one of natural|unnatural|auril|reminder, else "natural"
 *       repeat  one of none|newmoon|fullmoon|tenday|monthly|yearly, else "none"
 *       details string ? first 2000 UTF-16 code units (NOT trimmed here) : ""
 *       days    NATIVE ONLY (the web's cleanEvent drops the key): isInt ? clamp(days, 1,
 *               ALM_EVENT_DAYS_MAX) : 1. Written after "details", and only when > 1, so a
 *               file whose events all last one day is byte-identical to the web's.
 *  ui          chainNoteHidden = !!ui.chainNoteHidden;
 *              calYear  = isInt ? ui.calYear : monthOf(selAbs).year
 *              calMonth = isInt && 0..11 ? ui.calMonth : monthOf(selAbs).month
 *  applyState: rebuild dayCount days from the seed; if view < 0 then time = 23.
 *  trim() is the JS one: strips Unicode White_Space + U+FEFF + line terminators.
 *
 *  Native limits (deviations, documented in SPEC.md): abs values (startAbs, currentAbs,
 *  selAbs, event abs, note keys) must lie in IWD_ABS_MIN..IWD_ABS_MAX, year-name keys in
 *  IWD_YEAR_MIN..IWD_YEAR_MAX, calYear likewise; out-of-range values are treated like
 *  non-integers (fallback / entry dropped). Cutting at 120 / 2000 UTF-16 units never leaves
 *  half a surrogate pair (a dangling high surrogate is dropped). Files above
 *  ALM_MAX_FILE_BYTES are rejected.
 */
#ifndef IWD_STORE_H
#define IWD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================
 * Limits and constants
 * ============================================================================== */

#define ALM_STORE_VERSION      2
#define ALM_BACKUP_APP         "icewind-dale-almanac"
#define ALM_TITLE_MAX_UNITS    120     /* UTF-16 code units, as JS .slice(0, 120) */
#define ALM_DETAILS_MAX_UNITS  2000
#define ALM_YEARNAME_MAX_UNITS 80
#define ALM_SEED_INPUT_MAX     40      /* settings field maxlength (UTF-16 units); clean() accepts longer */
#define ALM_MOON_OFFSET_MIN    (-30.0)
#define ALM_MOON_OFFSET_MAX    30.0
#define ALM_MAX_EVENTS         20000   /* native safety cap (web: unlimited) */
#define ALM_EVENT_DAYS_MAX     30      /* longest event span in days (one Harptos month) */
#define ALM_MAX_NOTE_BYTES     (256 * 1024)
#define ALM_MAX_FILE_BYTES     (32 * 1024 * 1024)
#define ALM_UPCOMING_DAYS      10      /* upcoming list covers base .. base + 10 (11 days) */
#define ALM_TOAST_MS           4200
#define ALM_DAY_TOAST_MS       7000
#define ALM_DELETE_ARM_MS      3000    /* two-step delete: second press within 3 s */
#define ALM_NOTE_DEBOUNCE_MS   600     /* notes autosave after typing pauses */
#define ALM_SAVE_SOON_MS       250     /* saveSoon() debounce (calendar month paging) */

/* ================================================================================
 * Small owned containers
 * ============================================================================== */

/* Growable byte buffer used for JSON output and file contents. Always NUL terminated
 * when data != NULL. */
typedef struct AlmBuf {
    char  *data;
    size_t len;
    size_t cap;
} AlmBuf;

void alm_buf_free(AlmBuf *b);

/* yearNames / notes: integer key -> UTF-8 text, kept in JS insertion order. */
typedef struct AlmTextEntry {
    int32_t key;
    char   *text;
} AlmTextEntry;

typedef struct AlmTextMap {
    AlmTextEntry *items;
    int           count;
    int           cap;
} AlmTextMap;

/* NULL when absent. */
const char *alm_map_get(const AlmTextMap *m, int32_t key);

/* ================================================================================
 * Events and reminders (calendar.js)
 * ============================================================================== */

typedef enum AlmEvType {
    ALM_EV_NATURAL = 0, ALM_EV_UNNATURAL, ALM_EV_AURIL, ALM_EV_REMINDER, ALM_EV_TYPE_COUNT
} AlmEvType;

/* "natural" "unnatural" "auril" "reminder" */
const char *alm_ev_type_key(AlmEvType t);
/* "Natural" "Unnatural" "Auril" "Reminder" */
const char *alm_ev_type_label(AlmEvType t);
/* ALM_EV_TYPE_COUNT for an unknown key. */
AlmEvType   alm_ev_type_from_key(const char *key);
/* calendar.js PRIORITY: auril 0, unnatural 1, natural 2, reminder 3 (lower sorts first). */
int         alm_ev_type_priority(AlmEvType t);
/* CSS colours --t-natural #56e6b6, --t-unnatural #b897ff, --t-auril #e6f7ff,
 * --t-reminder #ffc46b, as 0xRRGGBB. */
uint32_t    alm_ev_type_rgb(AlmEvType t);

typedef struct AlmEvent {
    char     *id;          /* non-empty, unique within the almanac in practice */
    int32_t   abs;         /* anchor day (first occurrence) */
    char     *title;       /* trimmed, <= 120 UTF-16 units, non-empty */
    AlmEvType type;
    IwdRepeat repeat;
    char     *details;     /* trimmed by the form, <= 2000 UTF-16 units, may be "" */
    int       days;        /* span: each occurrence lasts 1..ALM_EVENT_DAYS_MAX days */
} AlmEvent;

/* Multi-day spans (native). An occurrence that starts on day S (any day iwd_occurs_on
 * accepts for the event's anchor and repeat) covers S .. S + days - 1, whatever lies in
 * between (month ends, festival days, Shieldmeet, the year's end). When occurrences overlap
 * (a span longer than its repeat interval, e.g. 15 days every tenday) a day belongs to the
 * LATEST occurrence covering it: the event shows once on that day and its day count
 * restarts at each new occurrence.
 * Returns the 0-based day of the span on abs, or -1 when the event is not on abs;
 * *occ_start (may be NULL) receives the occurrence's first day. */
int alm_event_span_day(const AlmEvent *e, int32_t abs, double moon_offset, int32_t *occ_start);

/* One mark shown on a day: a user event, or the built-in new-moon sacrifice
 * (calendar.js NEW_MOON: type auril, title "New moon — sacrifice to Auril",
 *  short "Sacrifice to Auril"). */
typedef struct AlmMark {
    bool            builtin;
    const AlmEvent *event;   /* NULL when builtin */
    AlmEvType       type;
    const char     *title;   /* full title (tooltip) */
    const char     *chip;    /* chip text: builtin "Sacrifice to Auril", else the title */
    int             span_day;   /* 0-based day of the event's span on this day (0 builtin) */
    int             span_len;   /* event->days (1 builtin) */
    int32_t         span_start; /* first day of the occurrence shown on this day */
} AlmMark;

#define ALM_NEW_MOON_TITLE "New moon — sacrifice to Auril"
#define ALM_NEW_MOON_SHORT "Sacrifice to Auril"
#define ALM_NEW_MOON_META  "Built in · every new moon of Selûne"

/* ================================================================================
 * The almanac state (app.js S + chain + days)
 * ============================================================================== */

typedef struct AlmUi {
    bool    chain_note_hidden;   /* web: hides its "Days are chained..." note. The native UI
                                    has no such note; the flag is kept for the web's sake */
    int32_t cal_year;            /* calendar page shown */
    int     cal_month;           /* 0..11 */
} AlmUi;

/* Native-only preferences, saved under "native" in the save file, never exported. */
typedef struct AlmNative {
    bool    sound;               /* wind ambience on (web: always starts off) */
    bool    interior;            /* sound muffled as heard indoors (sound only) */
    bool    has_rect;            /* window rect below is valid */
    int32_t win_x, win_y, win_w, win_h;   /* restored (non-maximised) window rect, px */
    bool    maximized;
    bool    ledger;              /* the dice ledger under the footer is open */
} AlmNative;

typedef struct Almanac {
    /* ---- persisted (snapshot v2) ---- */
    char      *seed;
    double     started;          /* ms since epoch; 0 = unknown */
    int32_t    start_abs;
    int32_t    current;          /* -1 = expedition not begun */
    bool       has_current_abs;  /* JSON currentAbs != null */
    int32_t    current_abs;
    int32_t    view;             /* -1 only while current == -1 (intro) */
    int32_t    sel_abs;
    double     time;             /* [0, 24) */
    bool       celsius;          /* unit "C" */
    double     moon_offset;
    AlmTextMap year_names;
    AlmTextMap notes;
    AlmEvent  *events;
    int        n_events, cap_events;
    AlmUi      ui;
    AlmNative  native;

    /* ---- derived: the weather (rebuilt from the seed, never saved) ---- */
    IwdChain   chain;            /* state after days[n_days - 1] */
    IwdDay    *days;             /* n_days generated days; dayCount == n_days */
    int32_t    n_days, cap_days;
    uint32_t   temp_hash;        /* iwd_temp_hash(seed) */
    uint32_t   epoch;            /* bumped whenever the day list is replaced (cache key) */

    /* ---- runtime only ---- */
    bool       busy;             /* an advance/transition animation is running */
    uint64_t   rand_state;       /* entropy for randomSeed / event ids (see alm_seed_entropy) */
} Almanac;

/* ================================================================================
 * Lifetime, entropy
 * ============================================================================== */

/* Fresh state: exactly clean({}) + applyState (random seed, no days, time 12, today unset).
 * entropy seeds rand_state (UI: e.g. QueryPerformanceCounter ^ GetTickCount64 ^ pid). */
bool alm_init(Almanac *a, uint64_t entropy);
void alm_free(Almanac *a);

/* Replace the entropy source (tests use a fixed value for reproducible ids/seeds). */
void     alm_seed_entropy(Almanac *a, uint64_t entropy);
uint32_t alm_rand_u32(Almanac *a);
/* IWD.randomSeed() from two alm_rand_u32 draws, e.g. "GLACIER-0A3F". */
size_t   alm_random_seed(Almanac *a, char *buf, size_t cap);

/* ================================================================================
 * Portable file shim (UTF-8 paths)
 * ============================================================================== */

typedef enum AlmIoStatus {
    ALM_IO_OK = 0,
    ALM_IO_NOT_FOUND,
    ALM_IO_TOO_LARGE,
    ALM_IO_ERROR
} AlmIoStatus;

/* Reads a whole file into out (NUL terminated). */
AlmIoStatus alm_fs_read(const char *path_utf8, AlmBuf *out);
/* Writes path.tmp then atomically replaces path (rename / MoveFileEx semantics). */
AlmIoStatus alm_fs_write_atomic(const char *path_utf8, const char *data, size_t len);
/* Same; durable = false skips the flush (_commit / fsync) and MOVEFILE_WRITE_THROUGH. The
 * tmp + rename still keeps the file whole; only the last few hundred ms can be lost on a
 * power cut. alm_fs_write_atomic() == durable true. */
AlmIoStatus alm_fs_write_atomic_ex(const char *path_utf8, const char *data, size_t len, bool durable);
/* Creates the directory and its parents if missing. */
AlmIoStatus alm_fs_mkdirs(const char *dir_utf8);

/* ================================================================================
 * Load / save / import / export
 * ============================================================================== */

typedef enum AlmLoadStatus {
    ALM_LOAD_OK = 0,          /* v2 file cleaned and applied */
    ALM_LOAD_MIGRATED_V1,     /* v1 file migrated */
    ALM_LOAD_NO_FILE,         /* no save yet: fresh state */
    ALM_LOAD_CORRUPT,         /* unreadable / not JSON / not an object: fresh state; the UI
                                 should keep the bad file (rename to .bad) before saving */
    ALM_LOAD_NO_MEMORY
} AlmLoadStatus;

/* Loads the save file into *a (which must be alm_init'ed; previous content is replaced),
 * then rebuilds the days. Never leaves *a invalid. */
AlmLoadStatus alm_load_file(Almanac *a, const char *path_utf8);
/* Same from memory (tests). */
AlmLoadStatus alm_load_json(Almanac *a, const char *json, size_t len);

/* Compact snapshot JSON exactly as JSON.stringify(snapshot()) would produce it, plus the
 * "native" key when with_native. */
bool alm_snapshot_json(const Almanac *a, bool with_native, AlmBuf *out);
/* Save file = alm_snapshot_json(a, true) written atomically. */
AlmIoStatus alm_save_file(const Almanac *a, const char *path_utf8);
/* alm_save_file with the durability of alm_fs_write_atomic_ex (routine debounced saves:
 * durable = false; close, import, regenerate and other explicit actions: true). */
AlmIoStatus alm_save_file_ex(const Almanac *a, const char *path_utf8, bool durable);

/* Backup text: JSON.stringify(Object.assign({app, exported}, snapshot()), null, 2).
 * exported_iso is a Date.toISOString() string, e.g. "2026-11-19T18:04:05.123Z". */
bool alm_export_json(const Almanac *a, const char *exported_iso, AlmBuf *out);
/* Suggested file name "icewind-almanac-<seed>-<YYYYMMDD>.json" (local date). Characters
 * Windows forbids in file names (<>:"/\|?* and controls) are replaced by '_' (native
 * deviation; the web uses the seed verbatim). */
size_t alm_export_filename(const Almanac *a, int year, int month1, int day,
                           char *buf, size_t cap);

typedef enum AlmImportStatus {
    ALM_IMPORT_OK = 0,
    ALM_IMPORT_NOT_BACKUP,    /* not JSON / not an object / seed not a non-empty string:
                                 "That file is not an almanac backup." */
    ALM_IMPORT_READ_ERROR,    /* "Could not read that file." */
    ALM_IMPORT_BUSY,          /* ignored while busy */
    ALM_IMPORT_NO_MEMORY
} AlmImportStatus;

/* importBackup: on success the whole state is replaced by clean(obj) + applyState, then
 * started = (obj.started is a non-zero number) ? obj.started : now_ms.
 * Native prefs (a->native) are kept. The UI then closes Settings, re-renders, saves and
 * toasts "Backup restored · N day(s), M event(s)" (see alm_fmt_import_toast). */
AlmImportStatus alm_import_json(Almanac *a, const char *json, size_t len, double now_ms);
AlmImportStatus alm_import_file(Almanac *a, const char *path_utf8, double now_ms);
/* "Backup restored · 12 days, 3 events" ("1 day", "1 event" singular). */
size_t alm_fmt_import_toast(const Almanac *a, char *buf, size_t cap);

/* ECMAScript Number::toString(x) for finite x (shortest round-trip digits; "1e+21",
 * "1e-7", "0.000001", "-0" -> "0"). Non-finite -> "null" (as JSON.stringify). */
#define ALM_NUM_CAP 32
size_t alm_fmt_js_number(double x, char *buf, size_t cap);

/* ================================================================================
 * Days and derived lookups
 * ============================================================================== */

/* Generate days until n_days >= min(n, IWD_MAX_DAYS). The chain only grows forward, so this
 * is the same weather a day-by-day roll gives. Returns false on allocation failure. */
bool alm_ensure_days(Almanac *a, int32_t n);

static inline int32_t alm_abs_of(const Almanac *a, int32_t index) { return a->start_abs + index; }
/* today's abs, or false when the expedition has not begun (current < 0). */
bool alm_today_abs(const Almanac *a, int32_t *abs_out);
/* Weather day for an abs day, or NULL if none generated there. */
const IwdDay *alm_weather_of(const Almanac *a, int32_t abs);
/* hourNow(): clamp(floor(time), 0, 23). The rules (weather, temperature, cold table,
 * light, effects) are per whole hour and always read this. */
int alm_hour_now(const Almanac *a);
/* Clock of the viewed day in whole minutes 0..1439, consistent with alm_hour_now():
 *   h = alm_hour_now(a); m = floor((time - h) * 60 + 1e-6) clamped to 0..59; h * 60 + m.
 * The UI formats every clock from this ("%02d:%02d"), never from iwd_fmt_clock (which
 * floors frac * 60 and could print 5/60 as ":04"). A legacy fractional time such as
 * 13.7283 from a web backup reads as 13:43 (823). */
int alm_minute_now(const Almanac *a);
/* True when there is weather on screen (n_days > 0 && view >= 0); otherwise the UI shows
 * the intro ("Before the First Dawn" / "The Dale Awaits"). */
bool alm_has_view(const Almanac *a);

/* app.js eventAt(v, hour): event id at any hour relative to day v, walking into
 * neighbouring generated days; past the last day it uses that day's carry_out
 * (h - 24 < duration - elapsed ? carry_out.id : carry_out.next); before day 0 clamps to
 * hour 0. Returns 1 when there are no days or v < 0. Suitable as an IwdEventAtFn
 * through alm_event_at_cb (user = Almanac*, uses a->view). */
int alm_event_at(const Almanac *a, int32_t v, int hour);
int alm_event_at_cb(void *user, int hour);

/* ================================================================================
 * Actions (app.js). Each returns what the UI must do next through AlmEffect flags.
 * None of them saves: the UI calls alm_save_file() when ALM_FX_SAVE is set (and on exit).
 * ============================================================================== */

enum {
    ALM_FX_NONE          = 0,
    ALM_FX_SAVE          = 1 << 0,   /* persist now */
    ALM_FX_RENDER_DAY    = 1 << 1,   /* day-level redraw (controls, timeline, ledger, hero) */
    ALM_FX_RENDER_HOUR   = 1 << 2,   /* hour-level redraw (hero weather, stats, effects) */
    ALM_FX_RENDER_CAL    = 1 << 3,   /* calendar grid, upcoming, day panel */
    ALM_FX_DAY_TOAST     = 1 << 4,   /* show the marks of out->toast_abs (alm_day_toast) */
    ALM_FX_TOAST         = 1 << 5,   /* show out->toast (plain text, may contain <b>..</b>) */
    ALM_FX_ANNOUNCE      = 1 << 6,   /* screen-reader summary changed (alm_fmt_announce) */
    ALM_FX_REJECTED      = 1 << 7    /* action ignored (busy, out of range) */
};
#define ALM_FX_RENDER_ALL (ALM_FX_RENDER_DAY | ALM_FX_RENDER_HOUR | ALM_FX_RENDER_CAL)

/* Toast text: plain UTF-8; the only markup is <b>...</b> around emphasised parts (the
 * web's <strong>) and, in the settings confirm text only, <code>...</code>. User text
 * inside toasts / confirm texts is escaped like the web's esc(): '&' '<' '>' appear as
 * "&amp;" "&lt;" "&gt;", so the UI splits on the tags, then decodes these three entities. */
#define ALM_TOAST_CAP 512
typedef struct AlmResult {
    unsigned fx;
    int32_t  toast_abs;              /* for ALM_FX_DAY_TOAST */
    const char *toast_lead;          /* day toast lead, e.g. "Today", "Today is now" */
    char     toast[ALM_TOAST_CAP];   /* for ALM_FX_TOAST */
} AlmResult;

/* Advance Day (generate button / N key). First call ("Begin the Expedition"): current = 0.
 * Otherwise current + 1; if view != current the view first returns to today. Then
 * current = view = target, sel_abs = currentAbs, calendar shows its month, time = land_time.
 * land_time: the web lands at 0 and tweens to IWD_SETTLE_HOUR (12); pass 12 for web parity,
 * or the carried-over clock when the advance came from a time step past midnight (native
 * minute stepping, alm_step_minutes, SPEC 6.2).
 * Rejected when busy or target >= IWD_MAX_DAYS. Day toast lead: first "The expedition
 * begins", else "Today". */
AlmResult alm_advance(Almanac *a, double land_time);

/* select(abs, {fromCalendar}): view a day. sel_abs = abs; if 0 <= i < MAX_DAYS and not busy,
 * days are rolled up to i and view = i (time = 12 if coming from the intro). The calendar
 * page follows the day unless from_calendar and the day is already on the shown page.
 * i >= MAX_DAYS toasts "Beyond the almanac: at most 3650 days of weather can be recorded."
 * ANNOUNCE when the view changed. Always SAVE. */
AlmResult alm_select(Almanac *a, int32_t abs, bool from_calendar);

/* setCurrent(abs) (double click / "Make this day today" in the UI): today jumps there.
 *  i < 0   -> toast "The expedition begins on <b>DATE</b>. Change the start date in
 *             Settings to begin earlier."  (no change)
 *  i >= MAX-> toast "Beyond the almanac: at most 3650 days can be recorded."
 *  i == current -> just select(abs) if not already selected
 *  else roll days, current = view = i, sel_abs = abs, (time = 12 if from intro),
 *       day toast "Today is now" or, with no marks, toast
 *       "Today is now <b>DATE</b>". */
AlmResult alm_set_current(Almanac *a, int32_t abs);

/* stepDay(dir): select(abs_of(view + dir)) when not busy, view >= 0 and in range. */
AlmResult alm_step_day(Almanac *a, int dir);

/* Set the clock of the viewed day (timeline scrubbing, undo, the party clock). Clamped to
 * [0, 23.9999]. Rejected without a view. Sets RENDER_HOUR | SAVE. */
AlmResult alm_set_time(Almanac *a, double t);

/* Native time stepping (SPEC 6.2). delta in minutes; the UI sends +-5, +-30, +-60.
 * m = alm_minute_now(a), n = m + delta; time is always stored as minutes / 60.0:
 *   0 <= n < 1440 : time = n / 60.0
 *   n >= 1440     : view != current -> select(abs_of(view + 1)), time = (n - 1440) / 60.0
 *                                       (REJECTED at IWD_MAX_DAYS, nothing changes)
 *                   view == current -> alm_advance(a, (n - 1440) / 60.0): today moves on
 *                                       (REJECTED when the almanac is full)
 *   n < 0         : view > 0 -> select(abs_of(view - 1)), time = (n + 1440) / 60.0
 *                              (today does NOT move back; the DM now views yesterday)
 *                   view == 0 -> REJECTED
 * Rejected when busy, without a view, for delta == 0 or |delta| >= 1440. Sets
 * ANNOUNCE | SAVE | RENDER_HOUR (plus whatever select / advance set). */
AlmResult alm_step_minutes(Almanac *a, int delta);

/* setUnit(u). */
AlmResult alm_set_unit(Almanac *a, bool celsius);

/* Calendar paging: step(dir) by months (wraps the year). Paging sets
 * SAVE (the web debounces it by ALM_SAVE_SOON_MS). */
AlmResult alm_cal_step(Almanac *a, int dir);
/* "Today" button: select(today or start_abs). */
AlmResult alm_cal_today(Almanac *a);

/* ================================================================================
 * Settings dialog (app.js openSettings / readStart / applySettings)
 * ============================================================================== */

/* "Month or festival" choices, in order: for each month i: "m<i>" with label
 * "<Name> — <Common>", followed by the festivals after it: "f<key>" with label
 * "<Name> (festival)" or "<Name> (festival, leap years)". 18 entries. */
#define ALM_START_OPTION_COUNT 18
typedef struct AlmStartOption {
    int         month;         /* 0..11, or -1 for a festival */
    IwdFestival festival;      /* IWD_FEST_NONE for a month */
    const char *label;         /* static UTF-8 */
} AlmStartOption;
const AlmStartOption *alm_start_option(int index);   /* 0..17 */
int alm_start_option_index(int month, IwdFestival festival);

typedef struct AlmSettingsForm {
    /* campaign start */
    int  start_day;            /* 1..30; ignored (field disabled) for a festival */
    bool start_day_valid;      /* the day field parsed as an integer */
    int  start_option;         /* index into alm_start_option */
    int32_t start_year;
    bool start_year_valid;     /* the year field parsed as an integer */
    /* seed (raw field text, trimmed + upper-cased by apply; empty -> random) */
    char seed[4 * ALM_SEED_INPUT_MAX + 1];
    /* year names */
    int32_t yn_year;
    bool    yn_year_valid;
    char    yn_name[4 * ALM_YEARNAME_MAX_UNITS + 1];
    /* temperature + note: the native dialog has no controls for these; they stay as
     * alm_settings_fill set them, so Apply writes them back unchanged */
    bool celsius;
    bool show_chain_note;
    /* advanced */
    double moon_offset;
    bool   moon_offset_valid;  /* parseFloat succeeded */
    /* Date.now() at apply time: becomes `started` of a re-rolled expedition. Set by the UI
     * just before alm_settings_apply (alm_settings_fill sets 0). */
    double now_ms;
} AlmSettingsForm;

/* openSettings: fill the form from the state (start date of start_abs, seed, yn_year =
 * ui.cal_year with its name, unit, !chain_note_hidden, moon_offset). */
void alm_settings_fill(const Almanac *a, AlmSettingsForm *f);
/* Year-name field follows its year field: name for yn_year or "". */
const char *alm_year_name(const Almanac *a, int32_t year);

typedef enum AlmApplyStatus {
    ALM_APPLY_DONE = 0,       /* applied: close the dialog, render all, save */
    ALM_APPLY_DATE_ERROR,     /* show err (dialog stays open) */
    ALM_APPLY_NEEDS_CONFIRM,  /* show confirm text; Apply becomes "Regenerate weather" */
    ALM_APPLY_BUSY
} AlmApplyStatus;

/* applySettings. confirm_armed = the dialog is already showing the confirmation (the
 * second press). Date errors (exact strings):
 *   "Enter a year."
 *   "Months have 30 days: pick a day from 1 to 30."
 *   "Shieldmeet only falls in leap years; <y> DR is not one."
 *   "That date does not exist."
 * Confirm (only when the start or seed changes and at least one day exists), with <b>:
 *   "<b>Regenerate the weather?</b> All recorded weather is replaced and rolled again from
 *    <b>START</b> with seed <code>SEED</code>" + upTo + " Notes and events are kept."
 *   upTo = ", up to the party's current day, <b>TODAY</b>."  when currentAbs >= start
 *        = ". The party's current day falls before the new start, so the expedition will
 *           begin on <b>START</b>."  otherwise
 * On DONE with a re-roll the toast is "Weather regenerated from <b>START</b> · seed SEED". */
typedef struct AlmApplyResult {
    AlmApplyStatus status;
    char     message[ALM_TOAST_CAP];    /* error text or confirm text */
    AlmResult result;                   /* for DONE */
} AlmApplyResult;

AlmApplyResult alm_settings_apply(Almanac *a, const AlmSettingsForm *f, bool confirm_armed);

/* regenerate(startAbs, seed): re-roll from the new start up to the party's calendar date
 * (currentAbs); notes/events kept; started = now_ms. Exposed for tests; apply calls it. */
bool alm_regenerate(Almanac *a, int32_t start_abs, const char *seed, double now_ms);

/* JS String.prototype.trim() + toUpperCase() for the seed field. Native deviation:
 * upper-casing uses simple 1:1 case mapping (no 'ß' -> "SS" expansion). */
size_t alm_normalize_seed(const char *in, char *buf, size_t cap);

/* ================================================================================
 * Events, notes, year names
 * ============================================================================== */

/* saveEvent: replace the event with the same id or append. The form supplies an already
 * trimmed title (non-empty) and details; they are trimmed again (idempotent) and cut to the
 * unit limits here; a blank title is REJECTED. id NULL (or unknown) -> a new id "e" +
 * base36(now_ms) + base36(floor(rand * 1e6)) anchored on abs. An update keeps the stored
 * event's id and anchor abs (the web never moves an event); abs is then ignored.
 * At ALM_MAX_EVENTS a new event is REJECTED with a toast. Sets *was_update.
 * Toast: "<b>TITLE</b> updated" / "<b>TITLE</b> added to the almanac".
 * alm_event_save keeps an updated event's days (new events last 1 day). */
AlmResult alm_event_save(Almanac *a, const char *id, int32_t abs, const char *title,
                         AlmEvType type, IwdRepeat repeat, const char *details,
                         double now_ms, bool *was_update);
/* The same with a span length: days 1..ALM_EVENT_DAYS_MAX (clamped); days <= 0 keeps an
 * updated event's length (1 for a new event). The anchor never moves, so editing from any
 * day of a span edits the one event and its days count from the anchor. */
AlmResult alm_event_save_span(Almanac *a, const char *id, int32_t abs, const char *title,
                              AlmEvType type, IwdRepeat repeat, const char *details, int days,
                              double now_ms, bool *was_update);
/* deleteEvent (all occurrences of a repeating event, every day of a span). Toast
 * "<b>TITLE</b> removed", plus " · all N days" for a span. */
AlmResult alm_event_delete(Almanac *a, const char *id);
const AlmEvent *alm_event_find(const Almanac *a, const char *id);
/* Native undo of a deletion. alm_event_dup deep-copies an event (false on out of memory;
 * dst is then empty); alm_event_release frees such a copy. alm_event_index: the event's
 * position in a->events, -1 if absent. alm_event_restore re-inserts a copy with its
 * original id, anchor, type, repeat, details and days at position index (clamped; the
 * array order decides ties in eventsOn); REJECTED when the id is already present, the
 * anchor is invalid or the almanac is full. Toast "<b>TITLE</b> restored". */
bool alm_event_dup(AlmEvent *dst, const AlmEvent *src);
void alm_event_release(AlmEvent *e);
int  alm_event_index(const Almanac *a, const char *id);
AlmResult alm_event_restore(Almanac *a, const AlmEvent *ev, int index);

/* eventsOn(abs): user events occurring on abs (on any day of a span), sorted by type priority then anchor abs
 * (stable: ties keep array order). allOn(abs): the built-in new-moon mark first when the
 * moon is new that day, then eventsOn. Return the total count; write at most cap (out may be
 * NULL with cap 0 to count). AlmMark / AlmEvent pointers stay valid only until the event
 * list changes (save, delete, load, import). */
int alm_events_on(const Almanac *a, int32_t abs, const AlmEvent **out, int cap);
int alm_marks_on(const Almanac *a, int32_t abs, AlmMark *out, int cap);

/* Day panel event meta line: builtin ALM_NEW_MOON_META; else
 * "<Type label> · <"One-time" | repeat label>" + (repeat && abs != anchor ?
 * " · since <short date> <year>" : "")   e.g. "Auril · Every tenday · since 3 Hammer 1489"
 * A span adds " · Day <k> of <n> · <alm_fmt_span_range of this occurrence>", e.g.
 * "Natural · Every year · since 3 Hammer 1489 · Day 2 of 5 · 3–7 Hammer". */
size_t alm_fmt_mark_meta(const AlmMark *m, int32_t shown_abs, char *buf, size_t cap);
/* The days start .. start + days - 1 (days clamped to 1..ALM_EVENT_DAYS_MAX) as short
 * dates whose inner spaces are U+00A0 (a date never wraps inside itself):
 *   1 day                 "5 Hammer"
 *   within one month      "5–9 Hammer"
 *   otherwise             "30 Hammer – 1 Alturiak", "30 Hammer – Midwinter" */
size_t alm_fmt_span_range(int32_t start, int days, char *buf, size_t cap);
/* Chip text of a mark: the chip text, then for a span " · day <k> of <n>" (in progress), or
 * with starts true " · <n> days" (an Upcoming row where the span begins). */
size_t alm_fmt_mark_chip(const AlmMark *m, bool starts, char *buf, size_t cap);

/* setNote(abs, text): text with a non-blank trim() is stored verbatim (insertion order kept
 * for an existing key), otherwise the note is deleted. Native cap ALM_MAX_NOTE_BYTES. */
AlmResult   alm_note_set(Almanac *a, int32_t abs, const char *text);
const char *alm_note_get(const Almanac *a, int32_t abs);   /* "" when none */
/* Year name (trimmed, <= 80 units); empty deletes. */
bool alm_year_name_set(Almanac *a, int32_t year, const char *name);

/* ================================================================================
 * Calendar presentation (calendar.js)
 * ============================================================================== */

/* One month cell or festival cell. */
typedef struct AlmCell {
    int32_t abs;
    IwdDate date;
    IwdMoon moon;
    bool    is_festival;
    const char *festival_name;    /* festival cells */
    bool    is_today, is_sel, is_pre /* before start */, is_start, is_new_moon;
    bool    has_note;
    bool    has_weather;
    int     dominant_id;          /* when has_weather: iwd_day_dominant */
    int     temp_min_f;           /* when has_weather: shown as fmtTemp(day.tempMin) */
    int     n_events;             /* user events on the day (eventsOn) */
    const AlmEvent *chips[2];     /* first two of eventsOn; "+<n_events - 2>" if more */
    bool    has_top_event;        /* underline in the colour of the first user event */
    AlmEvType top_type;
    const char *tag;              /* "Today" | "Start" | NULL (Today wins) */
} AlmCell;

/* The 30 days of ui.cal_year/ui.cal_month then its festival days. Returns the count
 * (30..32). Cells are laid out as a 10-column grid (three tendays) + a festival row. */
int alm_month_cells(const Almanac *a, AlmCell out[32]);
/* Accessible label: date · moon name · "sacrifice to Auril" · "<dominant>, low <temp>" ·
 * event titles · "has notes" · "current day" · "expedition start" (non-empty parts joined
 * by " · "). A festival cell adds nothing more: its date already names the festival
 * (the web's aria-label appends ", festival"). */
size_t alm_fmt_cell_label(const Almanac *a, const AlmCell *c, char *buf, size_t cap);

/* Upcoming: days base..base+10 with at least one mark, base = today or start_abs.
 * Each item: abs, rel = relDays(abs - base), short date. Note line:
 * "next ten days from today" (today set) / "from the expedition start". Empty list text:
 * "Nothing marked for the next ten days."
 * Native: a span is listed once, on the row where its occurrence starts (or on the first
 * row, base, while it is already in progress there); n_marks counts the listed marks only
 * and days whose only marks are continuing spans have no row. alm_upcoming_marks gives a
 * row's listed marks (same rules, same order as alm_marks_on). */
typedef struct AlmUpcoming {
    int32_t abs;
    int32_t rel_days;
    int     n_marks;
} AlmUpcoming;
int alm_upcoming(const Almanac *a, AlmUpcoming out[ALM_UPCOMING_DAYS + 1]);
int alm_upcoming_marks(const Almanac *a, const AlmUpcoming *row, AlmMark *out, int cap);

/* Day panel texts for sel_abs (calendar.js renderDetail):
 *  eyebrow: i < 0 "Before the expedition"; today "Today · Day N of the expedition";
 *           else "Day N of the expedition" + (today set ? " · " + lower(relDays) : "")
 *  sub    : festival "Festival day · after <Month>" ; month day
 *           "<Month> — <Common> · <First|Second|Third> tenday, day <d>"; both + " · <year
 *           name>" when the year has one
 *  moon   : "Selûne · <phase>" and sub "New moon — the sacrifice to Auril is due" or
 *           "<pct>% lit · waxing|waning"
 *  weather: i < 0 "No weather — the expedition begins on <date>."; beyond MAX
 *           "Beyond the almanac: at most 3650 days can be recorded."; not rolled
 *           "Weather not rolled yet."; else dominant name, sequence of distinct
 *           consecutive events (first 4 joined " → ", "…" if more), strip, and stats
 *           "Temperature <min> to <max><F|C>" and "Cold table DC a–b · TierA → TierB"
 *  set-current button: hidden if i < 0 or i >= MAX; disabled with "This is the current
 *           day" when sel is today; else "Set as current day". */
typedef struct AlmDayPanel {
    char eyebrow[96];
    char date[IWD_DATE_CAP];
    char sub[256];
    char moon_name[64];
    char moon_sub[96];
    bool moon_is_new;
    const IwdDay *weather;        /* NULL -> show weather_none */
    char weather_none[128];
    char wx_seq[192];             /* "Clear Skies → Extreme Fog → …" */
    int  dominant_id;
    char wx_temp[64];             /* "−72° to −45°F" */
    char wx_cold[96];             /* "DC 16–19 · Frigid Cold" or "DC 18–20 · Frigid Cold → Severe Cold" */
    bool show_set_current;
    bool set_current_disabled;
    const char *set_current_label;
} AlmDayPanel;
void alm_day_panel(const Almanac *a, AlmDayPanel *out);

/* ================================================================================
 * Hero / controls / announcements (app.js renderHeroDay / renderControls / announce)
 * ============================================================================== */

/* Hero date line for abs = view >= 0 ? abs_of(view) : start_abs:
 *   date "1 Hammer 1489 DR", moon (for the disc and label), tag:
 *   view < 0 -> "Expedition start"; abs == today -> "Today";
 *   today set -> relDays(abs - today) ("Tomorrow", "3 days ago", ...); else none.
 *   day label: view >= 0 ? "Day <view+1> of the expedition" : "Before the First Dawn". */
typedef enum AlmTagKind { ALM_TAG_NONE = 0, ALM_TAG_START, ALM_TAG_TODAY, ALM_TAG_AHEAD, ALM_TAG_PAST } AlmTagKind;
typedef struct AlmHero {
    int32_t    abs;
    char       date[IWD_DATE_CAP];
    IwdMoon    moon;
    AlmTagKind tag_kind;
    char       tag[48];
    char       day_label[64];
} AlmHero;
void alm_hero(const Almanac *a, AlmHero *out);

/* Advance button (renderControls), one line in the native UI:
 *   current < 0      "Begin the Expedition"
 *   current+1 >= MAX "The Almanac Is Full"  (disabled)
 *   else             "Advance to <short date of current+1>"
 * Day navigator buttons: prev disabled when busy or view <= 0; next disabled when busy,
 * view < 0 or view + 1 >= MAX. */
typedef struct AlmControls {
    char main[96];
    bool advance_enabled;
    bool prev_enabled, next_enabled;
} AlmControls;
void alm_controls(const Almanac *a, AlmControls *out);

/* announce(): "<date>, day <n>, <HH:00>: <event>, <temp>°<F|C>, <tier> DC <dc>, <wind>". */
size_t alm_fmt_announce(const Almanac *a, char *buf, size_t cap);

/* Day toast content: "<lead> · <date>" plus the day's marks (chips). Returns false when
 * the day has no marks (no toast). *n_marks receives the number written (<= cap). */
bool alm_day_toast(const Almanac *a, int32_t abs, const char *lead,
                   char *title, size_t title_cap, AlmMark *marks, int cap, int *n_marks);

#ifdef __cplusplus
}
#endif

#endif /* IWD_STORE_H */
