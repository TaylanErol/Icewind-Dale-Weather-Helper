/* ui.h - shared declarations of the native Win32 UI (main.c, ui_draw.c, ui_view.c,
 * ui_settings.c). The UI owns the window, painting and input; all behaviour lives in
 * engine.c / store.c (see SPEC.md).
 *
 * Painting model: one back-buffer DIB per window (Surface). Every frame is composed from
 * cached pieces (scaled scene, timeline strip, thumbnails) plus GDI text; nothing animates
 * while idle. Hit regions are collected during painting (immediate-mode style) and used for
 * hover, clicks and tooltips until the next paint.
 */
#ifndef IWD_UI_H
#define IWD_UI_H

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "store.h"
#include "audio.h"

/* ------------------------------------------------------------------ palette (0xRRGGBB) */
#define C_BG       0x0B111B
#define C_PANEL    0x0F1724
#define C_PANEL2   0x142032
#define C_LINE     0x1C2838
#define C_FRAME    0x2B3B53
#define C_FIELD    0x09101A
#define C_TEXT     0xD3DCE8
#define C_BRIGHT   0xFFFFFF
#define C_SOFT     0xB5C4D6
#define C_MUTED    0x8496AE
#define C_DIM      0x5E6E85
#define C_ICE      0xEAF6FF
#define C_ACCENT   0x86C5F2
#define C_AURORA   0x7DFFCF
#define C_DANGER   0xFFB4C2
#define C_WARN     0xFF9FB2
#define C_SHADOW   0x03060B
#define RGB_OF(c)  RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

/* ------------------------------------------------------------------ fonts */
enum {
    F_BODY, F_BODY_B, F_SMALL, F_SMALL_B, F_SMALL_I, F_TINY, F_TINY_B, F_CAPS, F_CAPS_S,
    F_NAME, F_DATE, F_FLAVOR, F_TEMP, F_UNIT, F_BTN, F_BTN_BIG, F_MONTH, F_MONO,
    F_VALUE, F_NUM, F_TITLE, F_FX, F_FX_B, F_CLOCK,
    F_COUNT
};

/* ------------------------------------------------------------------ surfaces */
typedef struct Surface {
    HDC     dc;
    HBITMAP bmp, old;
    DWORD  *px;
    int     w, h;
    void   *gfx;        /* GpGraphics on dc (antialiased shapes) */
} Surface;

extern Surface *g_tgt;           /* current drawing target */
bool surf_resize(Surface *s, int w, int h);
void surf_free(Surface *s);

/* ------------------------------------------------------------------ hit regions */
enum {
    H_NONE = 0,
    H_UNIT_F, H_UNIT_C, H_SOUND, H_SETTINGS,
    H_ADVANCE, H_STEP,                /* H_STEP arg = minutes: -60 -30 -5 5 30 60 */
    H_DAY_PREV, H_DAY_NEXT, H_TIMELINE, H_TIP,
    H_CAL_PREV, H_CAL_NEXT, H_CAL_TODAY, H_CAL_CELL, H_UPCOMING,
    H_SET_CURRENT, H_EV_EDIT, H_EV_DEL, H_ADD_EVENT, H_FORM_TYPE, H_FORM_REPEAT,
    H_FORM_CANCEL, H_FORM_SAVE, H_NOTES, H_TOAST, H_SCROLL,
    H_HEAD_NOW, H_BACK_TODAY, H_KEYS, H_KEYS_CARD, H_SAVE_FAIL, H_INTRO_SETTINGS,
    H_LEDGER,                         /* footer dice chip: dice ledger open / closed */
    /* settings dialog */
    H_S_MONTH, H_S_RANDOM, H_S_EXPORT, H_S_IMPORT, H_S_CANCEL, H_S_APPLY, H_S_CLOSE
};

typedef struct Hot {
    RECT   r;
    int    id;
    int    arg;
    bool   enabled;
    WCHAR  tip[200];
} Hot;

#define HOT_MAX 640
typedef struct HotList {
    Hot  items[HOT_MAX];
    int  n;
} HotList;

extern HotList *g_hl;            /* list being filled by the current paint */
Hot *hot_add(int x0, int y0, int x1, int y1, int id, int arg, bool enabled, const WCHAR *tip);
const Hot *hot_find(const HotList *l, int x, int y);

/* ------------------------------------------------------------------ app state */
#define TIP_CAP 256

typedef struct Ui {
    HINSTANCE inst;
    HWND      hwnd, tip_hwnd, set_hwnd;
    int       dpi;
    HFONT     font[F_COUNT];
    int       font_h[F_COUNT];
    HBRUSH    br_field, br_field_dis;

    Almanac   a;
    char      state_path[MAX_PATH * 3];
    bool      bad_file;           /* loaded file was corrupt (renamed before first save) */

    Surface   back;
    HotList   hl;
    int       hot_id, hot_arg;    /* hovered */
    int       press_id, press_arg;
    bool      tracking;
    bool      scrubbing;
    bool      sb_drag; int sb_grab;

    int       scroll, content_h;
    bool      layout_dirty;
    bool      wide;

    /* toast */
    bool      toast_on;
    char      toast[ALM_TOAST_CAP];
    bool      toast_day;          /* day toast with marks */
    int32_t   toast_abs;
    char      toast_lead[48];

    bool      sound_missing;

    /* event form */
    bool      form_open;
    char      form_id[64];        /* "" = new event */
    int       form_type;          /* AlmEvType */
    int       form_repeat;        /* IwdRepeat */
    bool      form_invalid;
    HWND      ed_title, ed_details;
    RECT      rc_title, rc_details;   /* content coords (unscrolled) */

    /* notes */
    HWND      ed_notes;
    int32_t   notes_abs;
    RECT      rc_notes;
    bool      note_pending;
    int       note_state;         /* 0 none, 1 editing, 2 saved */

    /* delete arming */
    char      armed_id[64];

    /* sound */
    int       last_wind_event;

    WCHAR     tip[TIP_CAP];

    /* in-game helpers (main.c owns the logic, ui_view*.c paints) */
    double    party_time;         /* today's clock while another day is viewed; < 0 none */
    bool      keys_on;            /* keyboard shortcuts card (F1 or the "?" chip) */
    char      toast_hint[96];     /* second, muted line of the toast ("Ctrl+Z steps back") */
    bool      toast_sticky;       /* no auto-hide (startup problems) */
    bool      save_failed;        /* the last save failed: header shows "Not saved" */
    bool      fade_req;           /* the next hero change came from a discrete user step */
    int       fade_k;             /* hero cross-fade ticks left (0 = none) */
    int       saved_scroll;       /* scroll to restore after a shortcut form / note; < 0 none */
    unsigned  rev;                /* bumped by every change beyond the clock (paint caches) */
} Ui;

extern Ui g;

static inline int dp(int v) { return MulDiv(v, g.dpi, 96); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------ ui_draw.c */
DWORD mix(DWORD d, DWORD c, int a);         /* a = 0..256 weight of c */
void  px_fill(int x0, int y0, int x1, int y1, DWORD c);
void  px_blend(int x0, int y0, int x1, int y1, DWORD c, int a);
void  px_frame(int x0, int y0, int x1, int y1, int t, DWORD c);
void  px_vgrad(int x0, int y0, int x1, int y1, DWORD top, DWORD bot);
void  px_vgrad_a(int x0, int y0, int x1, int y1, DWORD c, int a_top, int a_bot);
void  px_box(int x0, int y0, int x1, int y1, DWORD fill, DWORD border);   /* 1 px border, soft corners */
void  px_copy(const DWORD *src, int sw, int sh, int dx, int dy);

void  fonts_make(void);
void  fonts_free(void);
HFONT font(int f);
int   fh(int f);                             /* line height in px */
int   tw(int f, const WCHAR *s, int n);      /* width; n < 0 = wcslen */
void  tx(int f, int x, int y, const WCHAR *s, int n, DWORD c);
void  tx_sh(int f, int x, int y, const WCHAR *s, int n, DWORD c);     /* with drop shadow */
void  tx_right(int f, int xr, int y, const WCHAR *s, DWORD c);
void  tx_center(int f, int cx, int y, const WCHAR *s, DWORD c);
int   tx_ellipsis(int f, int x, int y, int w, const WCHAR *s, DWORD c); /* returns drawn width */
void  tx_caps(int f, int x, int y, const WCHAR *s, DWORD c);           /* letter-spaced */
int   tw_caps(int f, const WCHAR *s);

const WCHAR *wz(const char *utf8);          /* rotating conversion buffers */
WCHAR *wz_dup(const char *utf8);            /* heap copy, free() */
char  *u8_dup(const WCHAR *w);              /* heap copy, free() */

/* Rich text: spans wrapped at spaces / newlines into width w. */
enum { SP_BOX = 1, SP_SHADOW = 2 };
typedef struct Span {
    const WCHAR *s;
    int          n;          /* < 0: wcslen */
    BYTE         font;
    BYTE         flags;
    DWORD        color;
} Span;
int rich(const Span *sp, int nsp, int x, int y, int w, int lh, bool draw);
int wrap(int f, int x, int y, int w, const WCHAR *s, DWORD c, bool draw);   /* lh = fh(f) */

/* antialiased shapes (GDI+) on the current target */
void aa_moon(double cx, double cy, double r, const IwdMoon *m);
void aa_circle(double cx, double cy, double r, DWORD fill, int alpha);
void aa_ring(double cx, double cy, double r, DWORD c, int alpha, double width);
void aa_flake(double cx, double cy, double r, DWORD c, int alpha);
void aa_line(double x0, double y0, double x1, double y1, DWORD c, int alpha, double width);
void aa_flush(void);
void gdip_up(void);
void gdip_down(void);

/* images */
typedef struct Image { DWORD *px; int w, h; } Image;
const Image *img_full(int id);              /* 1200x400 scene, decoded lazily (small LRU) */
const Image *img_med(int id);               /* 360x120 copy kept for strips and thumbnails */
void  img_free_all(void);
void  resample_cover(const Image *im, DWORD *dst, int dw, int dh, int dpitch);
void  resample_box(const Image *im, double sx0, double sy0, double sx1, double sy1,
                   DWORD *dst, int dw, int dh, int dpitch);

/* scene composite for the hero (cached) */
typedef struct SceneLook {
    int    event_id;
    int    light_level;   /* IwdLightLevel; -1 intro */
    double twilight;      /* 0..1 */
    double aurora;        /* 0..1 aurora strength */
    double baked_aurora_subdue;   /* 0..1 for w01 outside the aurora window */
    double dark;          /* 0..1 extra darkness */
    double cold;          /* 0..1 frost */
    bool   glare;         /* bright sky -> stronger scrim */
    bool   light_ground;  /* White Out: light page, dark ink, no scrims */
} SceneLook;
const DWORD *scene_get(const SceneLook *lk, int w, int h);
void  scene_free(void);
/* tiny crop of an event's scene for calendar cells (cached per size) */
const DWORD *thumb_get(int id, int w, int h);
void  thumb_free(void);

/* ------------------------------------------------------------------ ui_view.c */
void view_layout(void);             /* recompute block positions (content_h) */
void view_paint(void);              /* compose the whole back buffer */
void view_free(void);
int  view_header_h(void);
void view_scroll_to(int y);
void view_ensure_visible(int y0, int y1);
int  view_scroll_for_thumb(int mouse_y);   /* scrollbar thumb drag: client y -> scroll (uses g.sb_grab) */
int  view_cells(AlmCell out[32]);   /* defined in ui_view_cal.c */
RECT view_block_rect(int which);    /* content coords */
enum { BLK_HERO, BLK_CONTROLS, BLK_STATS, BLK_EFFECTS, BLK_TIMELINE, BLK_LEDGER,
       BLK_CALENDAR, BLK_DAY, BLK_FOOT, BLK_COUNT };
double view_timeline_time(int x);   /* client x -> time on the timeline */
bool   view_timeline_hit(int x, int y);
void   view_invalidate_timeline(void);

/* ------------------------------------------------------------------ main.c */
void ui_apply(AlmResult r);         /* act on an action result (render, save, toast...) */
void ui_redraw(void);               /* repaint without re-layout */
void ui_relayout(void);             /* re-layout + repaint */
void ui_save_now(void);             /* durable save now (cancels a pending debounced one) */
void ui_apply_durable(AlmResult r); /* ui_apply, but ALM_FX_SAVE is written durably at once */
void ui_toast(const char *text);
void ui_open_settings(void);
void ui_after_settings(bool changed);
double ui_now_ms(void);
void ui_style_edit(HWND ed, int fnt);
HWND ui_make_edit(HWND parent, int id, bool multiline, int maxlen, int fnt, const char *text_utf8);
char *ui_edit_text(HWND ed);        /* heap UTF-8 */
/* x, y: SCREEN coordinates of the popup's top-left; returns chosen index or -1 */
int  ui_popup_choice(HWND owner, int x, int y, int w, const char *const *labels, int n, int sel);
LRESULT ui_menu_measure(MEASUREITEMSTRUCT *mi);
LRESULT ui_menu_draw(DRAWITEMSTRUCT *di);
void ui_sound_toggle(void);
void ui_open_form(const AlmEvent *ev);
void ui_close_form(void);
void ui_form_submit(void);
void ui_notes_begin(void);
void ui_notes_flush(void);
void ui_fade_start(void);           /* hero cross-fade timer (ui_view.c sets g.fade_k) */
void ui_export_backup(HWND owner, WCHAR *msg, int cap);   /* ui_settings.c: Save dialog + write */

/* ------------------------------------------------------------------ ui_settings.c */
bool settings_register(HINSTANCE inst);
void settings_open(void);
void settings_fonts_changed(void);

#endif /* IWD_UI_H */
