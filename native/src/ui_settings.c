/* ui_settings.c - the Settings dialog (SPEC.md 9; app.js openSettings / applySettings /
 * exportBackup / importBackup).
 *
 * Window model: one WS_POPUP window (class "IwdSettingsDlg") owned by g.hwnd that exactly
 * covers the owner's client area, like the web's <dialog> with its ::backdrop. It paints a
 * dimmed copy of the main back buffer (g.back) as the backdrop and the dialog card centred
 * on top. While it is open the owner is disabled (EnableWindow FALSE), so the dialog is
 * modal, but settings_open() returns at once: the main message loop keeps dispatching (no
 * nested loop). A mouse press on the backdrop (outside the card) closes it without applying,
 * as the web's pointerdown on the dialog element does. Esc, Cancel, the X and Alt+F4 also
 * discard. If the owner moves or resizes, a WinEvent hook (EVENT_OBJECT_LOCATIONCHANGE,
 * this thread only, no DLL) re-covers its client area.
 *
 * Painting: custom-painted into a private Surface; the six text inputs are real EDIT
 * children (ui_make_edit / ui_style_edit from main.c) laid over painted field frames, themed
 * here via WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC with g.br_field / g.br_field_dis. Layout and
 * paint share one function (pass) so they never disagree. Nothing animates; repaints happen
 * only on input. The backdrop is dimmed once per open/resize and cached.
 *
 * Layout: two columns (Campaign start, Expedition seed, Year names | Temperature + chained
 * note switch, Advanced, Backup) when the window is wide enough, else one column; then the
 * red confirmation box (when armed) and Cancel / Apply. If the card is taller than the
 * window it scrolls with the mouse wheel and follows keyboard focus.
 *
 * Hit regions (private list, client coordinates of this popup; handled entirely in this
 * file - main.c never sees these ids):
 *   H_S_CLOSE   arg 0   the X in the card head ("Close")          -> discard
 *   H_S_MONTH   arg 0   "Month or festival" select                 -> ui_popup_choice
 *   H_S_RANDOM  arg 0   "Random" (fills a random seed, disarms, focuses the seed)
 *   H_S_UNIT_F  arg 0   °F segment;  H_S_UNIT_C arg 0  °C segment (dialog-local choice)
 *   H_S_CHAIN   arg 0   "Show the chained-days note" switch (dialog-local)
 *   H_S_ADV     arg 0   "Advanced" disclosure (Selûne offset)
 *   H_S_EXPORT  arg 0   "Export backup" (GetSaveFileNameW)
 *   H_S_IMPORT  arg 0   "Import backup" (GetOpenFileNameW)
 *   H_S_CANCEL  arg 0   "Cancel" -> discard
 *   H_S_APPLY   arg 0   "Apply" / "Regenerate weather" -> alm_settings_apply
 *
 * Keyboard: Tab / Shift+Tab cycle in DOM order (X, Day, Month, Year, Seed, Random, Year,
 * Name, unit, switch, Advanced, Selûne offset, Export, Import, Cancel, Apply; disabled Day
 * and the collapsed offset are skipped). Enter in a text field or on the select / unit /
 * switch submits (Apply), on a button activates it; Space activates the focused control;
 * Left/Right/Up/Down switch the unit; Up/Down step the select; Alt+Down / F4 open it;
 * Up/Down in the number fields step by 1 (Day clamped 1..30) or 0.5 (offset, -30..30).
 * Esc closes from anywhere.
 *
 * Closing: discard -> ui_after_settings(false). Apply DONE -> ui_after_settings(true), then
 * ui_apply(result) (the "Weather regenerated..." toast and flags). Import OK ->
 * ui_after_settings(true), then ui_toast("Backup restored · ..."). The owner is enabled
 * again before the popup is destroyed, so activation returns to the main window.
 */
#include "ui.h"

#include <commctrl.h>
#include <commdlg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

#define DLG_CLASS   L"IwdSettingsDlg"
#define WM_DLG_CMD  (WM_APP + 0x51)   /* deferred actions posted from the EDIT subclass */
enum { CMD_CLOSE = 1, CMD_APPLY, CMD_FOCUS_NEXT, CMD_FOCUS_PREV };

/* text inputs */
enum { ED_DAY, ED_YEAR, ED_SEED, ED_YN_YEAR, ED_YN_NAME, ED_MOON, ED_COUNT };
/* focus stops, in the web's DOM order */
enum { FS_CLOSE, FS_DAY, FS_MONTH, FS_YEAR, FS_SEED, FS_RANDOM, FS_YN_YEAR, FS_YN_NAME,
       FS_UNIT, FS_CHAIN, FS_ADV, FS_MOON, FS_EXPORT, FS_IMPORT, FS_CANCEL, FS_APPLY,
       FS_COUNT };

static const int k_fs_edit[FS_COUNT] = {
    -1, ED_DAY, -1, ED_YEAR, ED_SEED, -1, ED_YN_YEAR, ED_YN_NAME, -1, -1, -1, ED_MOON,
    -1, -1, -1, -1 };
static const int k_edit_fs[ED_COUNT] = { FS_DAY, FS_YEAR, FS_SEED, FS_YN_YEAR, FS_YN_NAME, FS_MOON };

static const WCHAR k_backup_default[] =
    L"Everything in one JSON file: seed, dates, notes, events and settings.";
static const WCHAR k_help_start[] =
    L"The first day of the expedition: weather day 1 and the starting point of the seeded chain.";
static const WCHAR k_help_seed[] =
    L"One seed drives the whole campaign. Each new day continues the same chain, and the "
    L"weather carries over from the previous night.";
static const WCHAR k_help_moon[] =
    L"Shifts the moon's phases. Positive values make them arrive later. Canon: full at the "
    L"first midnight of 1 Hammer 1372 DR, 30 days 10½ hours per cycle.";
static const WCHAR k_lbl_moon[] = L"Selûne offset (days)";
static const WCHAR k_yn_placeholder[] = L"e.g. Year of the Warrior Princess";

#define MSG_CAP 600
#define CF_SPANS 32

static struct {
    HINSTANCE inst;
    bool      registered;
    HWND      hwnd;
    bool      closing;
    Surface   surf;
    HotList   hl;

    HWND      ed[ED_COUNT];
    WNDPROC   ed_proc[ED_COUNT];
    RECT      fld[ED_COUNT];        /* painted field frames (client coords) */
    RECT      ed_at[ED_COUNT];      /* where each EDIT currently sits */
    RECT      rs[FS_COUNT];         /* custom control rects (client coords) */

    /* dialog-local values (not applied until Apply) */
    int       option;               /* alm_start_option index */
    bool      celsius, chain;
    bool      adv;                  /* "Advanced" expanded (kept across opens, like <details>) */
    bool      armed;                /* confirmation showing */
    bool      err_on;
    WCHAR     err[ALM_TOAST_CAP];
    WCHAR     msg[MSG_CAP];         /* backup message line */

    /* decoded confirmation text */
    WCHAR     cf_text[ALM_TOAST_CAP];
    Span      cf_sp[CF_SPANS];
    int       cf_n;

    /* focus / pointer */
    int       foc;
    bool      foc_vis;
    int       hot_id, press_id;
    bool      tracking;
    bool      filling;              /* suppress EN_CHANGE reactions while we set text */

    /* geometry */
    bool      two;
    int       cx, cy, cw, ch;       /* card (cy already scrolled) */
    int       scroll, max_scroll;

    /* cached dimmed backdrop */
    DWORD    *bd;
    int       bd_w, bd_h;
    bool      bd_dirty;

    HWINEVENTHOOK hook;
    DWORD     field_bg, field_dis;  /* 0xRRGGBB of g.br_field / g.br_field_dis */
    HBRUSH    br_own, br_own_dis;   /* fallbacks when main.c has none */
} D;

/* ================================================================== small helpers */

static bool finite_d(double x) { return x - x == 0.0; }

static void copy_u8(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    if (!src) src = "";
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* keep whole chars */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void wcopy(WCHAR *dst, int cap, const WCHAR *src) { lstrcpynW(dst, src ? src : L"", cap); }

/* HTML <input type=number> value sanitising: a valid floating-point number, else "".
 * (Lenient about surrounding blanks and a leading '+'.) */
static bool html_number(const char *s, double *out)
{
    const char *p = s, *start;
    int di = 0, df = 0;
    while (*p == ' ' || *p == '\t') p++;
    start = p;
    if (*p == '-' || *p == '+') p++;
    while (*p >= '0' && *p <= '9') { p++; di++; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { p++; df++; } }
    if (!di && !df) return false;
    if (*p == 'e' || *p == 'E') {
        int de = 0;
        p++;
        if (*p == '-' || *p == '+') p++;
        while (*p >= '0' && *p <= '9') { p++; de++; }
        if (!de) return false;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return false;
    {
        char buf[64]; size_t n = (size_t)(p - start);
        if (n >= sizeof buf) {
            /* absurdly long literal: saturate by sign */
            *out = (*start == '-') ? -1e308 : 1e308;
            return true;
        }
        memcpy(buf, start, n); buf[n] = 0;
        while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
        *out = strtod(buf[0] == '+' ? buf + 1 : buf, NULL);
    }
    return true;
}

/* JS parseInt(s, 10), saturated to int32. */
static int32_t js_parse_int(const char *s, bool *ok)
{
    const char *p = s; bool neg = false; long long v = 0; int nd = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    while (*p >= '0' && *p <= '9') {
        if (v < 100000000000LL) v = v * 10 + (*p - '0');
        p++; nd++;
    }
    *ok = nd > 0;
    if (neg) v = -v;
    if (v > 2147483647LL) v = 2147483647LL;
    if (v < -2147483647LL) v = -2147483647LL;
    return (int32_t)v;
}

static char *edit_text(int e)
{
    char *t = D.ed[e] ? ui_edit_text(D.ed[e]) : NULL;
    if (!t) { t = (char *)malloc(1); if (t) t[0] = 0; }
    return t;
}

/* number field -> parseInt(value, 10) */
static int32_t field_int(int e, bool *ok)
{
    char *t = edit_text(e); double d; int32_t v = 0;
    *ok = false;
    if (t && html_number(t, &d)) v = js_parse_int(t, ok);
    free(t);
    return v;
}

/* number field -> parseFloat(value); non-finite counts as invalid (isFinite check) */
static double field_float(int e, bool *ok)
{
    char *t = edit_text(e); double d = 0;
    *ok = t && html_number(t, &d) && finite_d(d);
    free(t);
    return *ok ? d : 0;
}

static void set_text(int e, const WCHAR *s)
{
    if (!D.ed[e]) return;
    D.filling = true;
    SetWindowTextW(D.ed[e], s ? s : L"");
    D.filling = false;
}

static void set_int(int e, int32_t v)
{
    WCHAR b[24];
    wsprintfW(b, L"%d", (int)v);
    set_text(e, b);
}

static DWORD brush_rgb(HBRUSH br, DWORD fallback)
{
    LOGBRUSH lb;
    if (!br || GetObjectW(br, sizeof lb, &lb) != (int)sizeof lb) return fallback;
    return ((DWORD)GetRValue(lb.lbColor) << 16) | ((DWORD)GetGValue(lb.lbColor) << 8) |
           (DWORD)GetBValue(lb.lbColor);
}

static HBRUSH field_brush(bool disabled)
{
    if (!disabled && g.br_field) return g.br_field;
    if (disabled && g.br_field_dis) return g.br_field_dis;
    if (disabled) {
        if (!D.br_own_dis) D.br_own_dis = CreateSolidBrush(RGB_OF(mix(C_FIELD, C_PANEL, 128)));
        return D.br_own_dis;
    }
    if (!D.br_own) D.br_own = CreateSolidBrush(RGB_OF(C_FIELD));
    return D.br_own;
}

static void read_field_colors(void)
{
    D.field_bg  = brush_rgb(field_brush(false), C_FIELD);
    D.field_dis = brush_rgb(field_brush(true), mix(C_FIELD, C_PANEL, 128));
}

static bool option_is_festival(int i)
{
    const AlmStartOption *o = alm_start_option(i);
    return o && o->festival != IWD_FEST_NONE;
}

static void redraw(void) { if (D.hwnd) InvalidateRect(D.hwnd, NULL, FALSE); }

static void relayout(void);
static void set_focus_stop(int s, bool visible, bool select);
static void close_dialog(bool changed, const AlmResult *res, const char *toast);

/* ================================================================== confirmation text */

/* Splits the store's confirm text on <b>, </b>, <code>, </code> and decodes &amp; &lt; &gt;
 * (plus &quot; / &#39; for safety) into spans over D.cf_text. */
static void build_confirm(const char *msg_u8)
{
    WCHAR *w = wz_dup(msg_u8 ? msg_u8 : "");
    const WCHAR *p;
    int n = 0, font = F_SMALL, start = 0;
    DWORD color = C_TEXT;
    D.cf_n = 0;
    D.cf_text[0] = 0;
    if (!w) return;
#define CF_FLUSH() do { if (n > start && D.cf_n < CF_SPANS) { Span *s_ = &D.cf_sp[D.cf_n++]; \
        s_->s = D.cf_text + start; s_->n = n - start; s_->font = (BYTE)font; \
        s_->flags = font == F_MONO ? SP_BOX : 0; \
        s_->color = color; } start = n; } while (0)
    for (p = w; *p && n < ALM_TOAST_CAP - 1; ) {
        if (*p == L'<') {
            int nf = -1; DWORD nc = C_TEXT; size_t len = 0;
            if (!wcsncmp(p, L"<b>", 3))          { nf = F_SMALL_B; nc = C_BRIGHT; len = 3; }
            else if (!wcsncmp(p, L"</b>", 4))    { nf = F_SMALL;   nc = C_TEXT;   len = 4; }
            else if (!wcsncmp(p, L"<code>", 6))  { nf = F_MONO;    nc = C_ICE;    len = 6; }
            else if (!wcsncmp(p, L"</code>", 7)) { nf = F_SMALL;   nc = C_TEXT;   len = 7; }
            if (nf >= 0) { CF_FLUSH(); font = nf; color = nc; p += len; continue; }
        } else if (*p == L'&') {
            WCHAR c = 0; size_t len = 0;
            if (!wcsncmp(p, L"&amp;", 5))       { c = L'&';  len = 5; }
            else if (!wcsncmp(p, L"&lt;", 4))   { c = L'<';  len = 4; }
            else if (!wcsncmp(p, L"&gt;", 4))   { c = L'>';  len = 4; }
            else if (!wcsncmp(p, L"&quot;", 6)) { c = L'"';  len = 6; }
            else if (!wcsncmp(p, L"&#39;", 5))  { c = L'\''; len = 5; }
            if (len) { D.cf_text[n++] = c; p += len; continue; }
        }
        D.cf_text[n++] = *p++;
    }
    CF_FLUSH();
#undef CF_FLUSH
    D.cf_text[n] = 0;
    free(w);
}

static int confirm_lh(void)
{
    int a = fh(F_SMALL_B), b = fh(F_MONO);
    return (a > b ? a : b) + dp(2);
}

/* ================================================================== drawing helpers */

static bool is_hot(int id)   { return D.hot_id == id && (D.press_id == 0 || D.press_id == id); }
static bool is_press(int id) { return D.press_id == id && D.hot_id == id; }

static void focus_ring(int s, int x0, int y0, int x1, int y1)
{
    if (D.foc_vis && D.foc == s && GetFocus() == D.hwnd)
        px_frame(x0 - dp(3), y0 - dp(3), x1 + dp(3), y1 + dp(3), 1, C_ACCENT);
}

enum { BTN_GHOST, BTN_SOLID, BTN_WARN };

static void button(RECT *r, int s, int id, int style, const WCHAR *label, bool draw)
{
    DWORD fill, border, ink;
    bool hot = is_hot(id), press = is_press(id);
    D.rs[s] = *r;
    hot_add(r->left, r->top, r->right, r->bottom, id, 0, true, NULL);
    if (!draw) return;
    if (style == BTN_SOLID) {          /* primary: the Advance button's recipe */
        fill = mix(C_PANEL2, C_ACCENT, hot ? 46 : 14); border = C_ACCENT; ink = C_BRIGHT;
    } else if (style == BTN_WARN) {    /* destructive confirm */
        fill = mix(C_PANEL2, C_WARN, hot ? 54 : 30); border = C_WARN; ink = C_BRIGHT;
    } else {
        fill = hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2;
        border = hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME; ink = C_BRIGHT;
    }
    if (press) fill = mix(fill, C_BG, 60);
    px_box(r->left, r->top, r->right, r->bottom, fill, border);
    tx_center(F_BTN, (r->left + r->right) / 2, (r->top + r->bottom - fh(F_BTN)) / 2, label, ink);
    focus_ring(s, r->left, r->top, r->right, r->bottom);
}

static int btn_w(const WCHAR *label) { return tw(F_BTN, label, -1) + dp(30); }
static int fld_h(void) { return dp(32); }
static int lbl_h(void) { return fh(F_SMALL) + dp(5); }

static void label(int x, int y, const WCHAR *s, DWORD c, bool draw)
{
    if (draw) tx(F_SMALL, x, y, s, -1, c);
}

static void field(int e, int x, int y, int w, bool draw)
{
    bool en, focused;
    SetRect(&D.fld[e], x, y, x + w, y + fld_h());
    if (!draw) return;
    en = D.ed[e] && IsWindowEnabled(D.ed[e]);
    focused = D.ed[e] && GetFocus() == D.ed[e];
    px_box(x, y, x + w, y + fld_h(), en ? D.field_bg : D.field_dis,
           focused ? C_ACCENT : en ? C_FRAME : C_LINE);
}

static int group_head(int x, int y, int w, const WCHAR *title, const WCHAR *help, bool draw)
{
    if (draw) tx(F_BODY_B, x, y, title, -1, C_ICE);
    y += fh(F_BODY_B) + dp(4);
    if (help) y += wrap(F_SMALL, x, y, w, help, C_MUTED, draw) + dp(10);
    else y += dp(6);
    return y;
}

static int separator(int x, int y, int w, bool draw)
{
    y += dp(16);
    if (draw) px_fill(x, y, x + w, y + 1, C_LINE);
    return y + 1 + dp(16);
}

static double fdp(double v) { return v * g.dpi / 96.0; }

static void chevron(int xr, int cy, DWORD c)
{
    double x = xr - fdp(14), y = cy;
    aa_line(x - fdp(4), y - fdp(2), x, y + fdp(2), c, 255, fdp(1.5));
    aa_line(x, y + fdp(2), x + fdp(4), y - fdp(2), c, 255, fdp(1.5));
    aa_flush();
}

/* ================================================================== groups */

static int grp_start(int x, int y, int w, bool draw)
{
    int gap = dp(10), dw = dp(64), yw = dp(96), mw, mx, yx;
    bool fest = option_is_festival(D.option);
    y = group_head(x, y, w, L"Campaign start", k_help_start, draw);
    mw = w - dw - yw - 2 * gap;
    if (mw < dp(100)) { yw = dp(76); mw = w - dw - yw - 2 * gap; }
    mx = x + dw + gap; yx = mx + mw + gap;
    label(x, y, L"Day", fest ? C_DIM : C_SOFT, draw);
    label(mx, y, L"Month or festival", C_SOFT, draw);
    label(yx, y, L"Year (DR)", C_SOFT, draw);
    y += lbl_h();
    field(ED_DAY, x, y, dw, draw);
    field(ED_YEAR, yx, y, yw, draw);
    {   /* the select */
        RECT *r = &D.rs[FS_MONTH];
        const AlmStartOption *o = alm_start_option(D.option);
        bool hot = is_hot(H_S_MONTH);
        SetRect(r, mx, y, mx + mw, y + fld_h());
        hot_add(r->left, r->top, r->right, r->bottom, H_S_MONTH, 0, true, NULL);
        if (draw) {
            bool f = D.foc == FS_MONTH && GetFocus() == D.hwnd;
            px_box(r->left, r->top, r->right, r->bottom, hot ? mix(D.field_bg, C_PANEL2, 160) : D.field_bg,
                   f ? C_ACCENT : hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME);
            tx_ellipsis(F_BODY, r->left + dp(10), (r->top + r->bottom - fh(F_BODY)) / 2,
                        mw - dp(36), wz(o ? o->label : ""), C_TEXT);
            chevron(r->right, (r->top + r->bottom) / 2, C_MUTED);
        }
    }
    y += fld_h();
    if (D.err_on) {
        y += dp(8);
        y += wrap(F_SMALL, x, y, w, D.err, C_DANGER, draw);
    }
    return y;
}

static int grp_seed(int x, int y, int w, bool draw)
{
    int gap = dp(10), rw = btn_w(L"Random");
    RECT r;
    y = group_head(x, y, w, L"Expedition seed", k_help_seed, draw);
    label(x, y, L"Seed", C_SOFT, draw);
    y += lbl_h();
    field(ED_SEED, x, y, w - rw - gap, draw);
    SetRect(&r, x + w - rw, y, x + w, y + fld_h());
    button(&r, FS_RANDOM, H_S_RANDOM, BTN_GHOST, L"Random", draw);
    return y + fld_h();
}

static int grp_years(int x, int y, int w, bool draw)
{
    int gap = dp(10), yw = dp(96), nx = x + yw + gap;
    y = group_head(x, y, w, L"Year names", NULL, draw);
    label(x, y, L"Year (DR)", C_SOFT, draw);
    if (draw) {
        int lw = tw(F_SMALL, L"Name ", -1);
        tx(F_SMALL, nx, y, L"Name ", -1, C_SOFT);
        tx(F_SMALL_I, nx + lw, y, L"(optional)", -1, C_MUTED);
    }
    y += lbl_h();
    field(ED_YN_YEAR, x, y, yw, draw);
    field(ED_YN_NAME, nx, y, w - yw - gap, draw);
    return y + fld_h();
}

static int grp_temp(int x, int y, int w, bool draw)
{
    int segw = dp(50), th = fld_h(), sx, sy, trw = dp(34), trh = dp(18);
    const WCHAR *swl = L"Show the chained-days note";
    int need = trw + dp(10) + tw(F_BODY, swl, -1);
    RECT *u = &D.rs[FS_UNIT], *c = &D.rs[FS_CHAIN];
    y = group_head(x, y, w, L"Display", NULL, draw);
    SetRect(u, x, y, x + 2 * segw, y + th);
    hot_add(x, y, x + segw, y + th, H_S_UNIT_F, 0, true, NULL);
    hot_add(x + segw, y, x + 2 * segw, y + th, H_S_UNIT_C, 0, true, NULL);
    if (draw) {
        int i;
        px_box(u->left, u->top, u->right, u->bottom, D.field_bg, C_FRAME);
        for (i = 0; i < 2; i++) {
            bool on = (i == 1) == D.celsius;
            bool hot = is_hot(i ? H_S_UNIT_C : H_S_UNIT_F);
            int a = x + i * segw, b = a + segw;
            if (on) px_fill(a + 2, y + 2, b - 2, y + th - 2, mix(C_PANEL2, C_ACCENT, 70));
            else if (hot) px_fill(a + 2, y + 2, b - 2, y + th - 2, mix(D.field_bg, C_PANEL2, 200));
            tx_center(F_BTN, (a + b) / 2, y + (th - fh(F_BTN)) / 2, i ? L"°C" : L"°F",
                      on ? C_BRIGHT : hot ? C_TEXT : C_MUTED);
        }
        focus_ring(FS_UNIT, u->left, u->top, u->right, u->bottom);
    }
    /* switch: beside the toggle when it fits (web .set-split), else below */
    sx = x + 2 * segw + dp(32);
    sy = y;
    if (sx + need > x + w) { sx = x; sy = y + th + dp(14); }
    SetRect(c, sx, sy, sx + need, sy + th);
    hot_add(c->left, c->top, c->right, c->bottom, H_S_CHAIN, 0, true, NULL);
    if (draw) {
        bool hot = is_hot(H_S_CHAIN);
        double ty = sy + (th - trh) / 2.0, r = trh / 2.0;
        DWORD tr = D.chain ? mix(C_PANEL2, C_ACCENT, hot ? 190 : 160) : hot ? C_PANEL2 : D.field_bg;
        px_fill(sx + (int)r, (int)ty, sx + trw - (int)r, (int)ty + trh, tr);
        aa_circle(sx + r, ty + r, r, tr, 255);
        aa_circle(sx + trw - r, ty + r, r, tr, 255);
        aa_circle(D.chain ? sx + trw - r : sx + r, ty + r, r - fdp(3), D.chain ? C_BRIGHT : C_MUTED, 255);
        aa_flush();
        if (!D.chain) {   /* thin outline of the off track */
            px_fill(sx + (int)r, (int)ty, sx + trw - (int)r, (int)ty + 1, C_FRAME);
            px_fill(sx + (int)r, (int)ty + trh - 1, sx + trw - (int)r, (int)ty + trh, C_FRAME);
        }
        tx(F_BODY, sx + trw + dp(10), sy + (th - fh(F_BODY)) / 2, swl, -1, hot ? C_BRIGHT : C_TEXT);
        focus_ring(FS_CHAIN, c->left, c->top, c->right, c->bottom);
    }
    return (sy + th > y + th ? sy + th : y + th);
}

static int grp_adv(int x, int y, int w, bool draw)
{
    RECT *r = &D.rs[FS_ADV];
    int sh = dp(26), tw_ = tw(F_BODY_B, L"Advanced", -1);
    SetRect(r, x, y, x + dp(18) + tw_ + dp(6), y + sh);
    hot_add(r->left, r->top, r->right, r->bottom, H_S_ADV, 0, true, NULL);
    if (draw) {
        bool hot = is_hot(H_S_ADV);
        DWORD c = hot ? C_BRIGHT : C_ICE;
        double cx = x + fdp(6), cy = y + sh / 2.0, k = fdp(4);
        double tri[6];
        if (D.adv) { tri[0] = cx - k; tri[1] = cy - k * 0.6; tri[2] = cx + k; tri[3] = cy - k * 0.6; tri[4] = cx; tri[5] = cy + k * 0.8; }
        else       { tri[0] = cx - k * 0.6; tri[1] = cy - k; tri[2] = cx - k * 0.6; tri[3] = cy + k; tri[4] = cx + k * 0.8; tri[5] = cy; }
        aa_poly(tri, 3, hot ? C_BRIGHT : C_MUTED, 255);
        aa_flush();
        tx(F_BODY_B, x + dp(18), y + (sh - fh(F_BODY_B)) / 2, L"Advanced", -1, c);
        focus_ring(FS_ADV, r->left, r->top, r->right, r->bottom);
    }
    y += sh;
    if (D.adv) {
        int fw = tw(F_SMALL, k_lbl_moon, -1), hx, hw, hy, h1;
        if (fw < dp(110)) fw = dp(110);
        y += dp(10);
        hx = x + fw + dp(16); hw = x + w - hx;
        label(x, y, k_lbl_moon, C_SOFT, draw);
        field(ED_MOON, x, y + lbl_h(), fw, draw);
        h1 = lbl_h() + fld_h();
        if (hw >= dp(160)) {
            int hh = wrap(F_SMALL, hx, y, hw, k_help_moon, C_MUTED, draw);
            y += h1 > hh ? h1 : hh;
        } else {
            hy = y + h1 + dp(8);
            y = hy + wrap(F_SMALL, x, hy, w, k_help_moon, C_MUTED, draw);
        }
    } else {
        SetRectEmpty(&D.fld[ED_MOON]);
    }
    return y;
}

static int grp_backup(int x, int y, int w, bool draw)
{
    RECT r;
    int ew = btn_w(L"Export backup"), iw = btn_w(L"Import backup");
    y = group_head(x, y, w, L"Backup", NULL, draw);
    SetRect(&r, x, y, x + ew, y + fld_h());
    button(&r, FS_EXPORT, H_S_EXPORT, BTN_GHOST, L"Export backup", draw);
    SetRect(&r, x + ew + dp(10), y, x + ew + dp(10) + iw, y + fld_h());
    button(&r, FS_IMPORT, H_S_IMPORT, BTN_GHOST, L"Import backup", draw);
    y += fld_h() + dp(10);
    y += wrap(F_SMALL, x, y, w, D.msg, C_MUTED, draw);
    return y;
}

/* One layout/paint pass over the card; returns its height. */
static int pass(bool draw)
{
    int pad = dp(24), x = D.cx, w = D.cw, y = D.cy, inner = w - 2 * pad, xl = x + pad;
    RECT r;
    if (draw) {
        int i;
        for (i = 1; i <= 6; i++)    /* soft shadow */
            px_blend(x - i, D.cy - i + dp(2), x + w + i, D.cy + D.ch + i + dp(2), C_SHADOW, 40 - i * 5);
        px_box(x, D.cy, x + w, D.cy + D.ch, C_PANEL, C_FRAME);
    }
    /* head */
    y += dp(18);
    if (draw) tx(F_TITLE, xl, y, L"Settings", -1, C_BRIGHT);
    {
        int s = dp(30), cx0 = x + w - pad - s + dp(8), cy0 = y + (fh(F_TITLE) - s) / 2;
        bool hot = is_hot(H_S_CLOSE);
        SetRect(&D.rs[FS_CLOSE], cx0, cy0, cx0 + s, cy0 + s);
        hot_add(cx0, cy0, cx0 + s, cy0 + s, H_S_CLOSE, 0, true, L"Close");
        if (draw) {
            double c0 = cx0 + s / 2.0, c1 = cy0 + s / 2.0, k = fdp(5.5);
            DWORD ink = hot ? C_BRIGHT : C_MUTED;
            if (hot) px_box(cx0, cy0, cx0 + s, cy0 + s, mix(C_PANEL, C_PANEL2, 200), C_FRAME);
            aa_line(c0 - k, c1 - k, c0 + k, c1 + k, ink, 255, fdp(1.5));
            aa_line(c0 - k, c1 + k, c0 + k, c1 - k, ink, 255, fdp(1.5));
            aa_flush();
            focus_ring(FS_CLOSE, cx0, cy0, cx0 + s, cy0 + s);
        }
    }
    y += fh(F_TITLE) + dp(16);
    if (draw) px_fill(x + 1, y, x + w - 1, y + 1, C_LINE);
    y += 1 + dp(18);

    if (D.two) {
        int gap = dp(36), colw = (inner - gap) / 2, xr = xl + colw + gap, yl = y, yr = y;
        yl = grp_start(xl, yl, colw, draw);
        yl = separator(xl, yl, colw, draw);
        yl = grp_seed(xl, yl, colw, draw);
        yl = separator(xl, yl, colw, draw);
        yl = grp_years(xl, yl, colw, draw);
        yr = grp_temp(xr, yr, colw, draw);
        yr = separator(xr, yr, colw, draw);
        yr = grp_adv(xr, yr, colw, draw);
        yr = separator(xr, yr, colw, draw);
        yr = grp_backup(xr, yr, colw, draw);
        if (draw) px_fill(xl + colw + gap / 2, y, xl + colw + gap / 2 + 1, (yl > yr ? yl : yr), C_LINE);
        y = yl > yr ? yl : yr;
    } else {
        y = grp_start(xl, y, inner, draw);
        y = separator(xl, y, inner, draw);
        y = grp_seed(xl, y, inner, draw);
        y = separator(xl, y, inner, draw);
        y = grp_years(xl, y, inner, draw);
        y = separator(xl, y, inner, draw);
        y = grp_temp(xl, y, inner, draw);
        y = separator(xl, y, inner, draw);
        y = grp_adv(xl, y, inner, draw);
        y = separator(xl, y, inner, draw);
        y = grp_backup(xl, y, inner, draw);
    }
    y += dp(20);

    /* the red confirmation box */
    if (D.armed && D.cf_n > 0) {
        int bp = dp(12), th = rich(D.cf_sp, D.cf_n, xl + bp, y + bp, inner - 2 * bp, confirm_lh(), false);
        if (draw) {
            px_box(xl, y, xl + inner, y + th + 2 * bp, mix(C_PANEL, C_DANGER, 22), mix(C_PANEL, C_DANGER, 150));
            rich(D.cf_sp, D.cf_n, xl + bp, y + bp, inner - 2 * bp, confirm_lh(), true);
        }
        y += th + 2 * bp + dp(16);
    }

    /* actions */
    {
        const WCHAR *al = D.armed ? L"Regenerate weather" : L"Apply";
        int aw = btn_w(al), cw = btn_w(L"Cancel"), bh = dp(34), xr = xl + inner;
        if (aw < dp(96)) aw = dp(96);
        if (cw < dp(96)) cw = dp(96);
        SetRect(&r, xr - aw, y, xr, y + bh);
        button(&r, FS_APPLY, H_S_APPLY, D.armed ? BTN_WARN : BTN_SOLID, al, draw);
        SetRect(&r, xr - aw - dp(10) - cw, y, xr - aw - dp(10), y + bh);
        button(&r, FS_CANCEL, H_S_CANCEL, BTN_GHOST, L"Cancel", draw);
        y += bh;
    }
    y += pad;
    return y - D.cy;
}

/* ================================================================== layout */

static void place_edits(void)
{
    int e;
    for (e = 0; e < ED_COUNT; e++) {
        RECT f = D.fld[e], t;
        int fnt = e == ED_SEED ? F_MONO : F_BODY, h = fh(fnt) + dp(4);
        bool show = D.ed[e] && !IsRectEmpty(&f);
        if (!D.ed[e]) continue;
        if (show) {
            SetRect(&t, f.left + dp(8), (f.top + f.bottom - h) / 2, f.right - dp(8), (f.top + f.bottom - h) / 2 + h);
            if (!EqualRect(&t, &D.ed_at[e]))
                SetWindowPos(D.ed[e], NULL, t.left, t.top, t.right - t.left, t.bottom - t.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            D.ed_at[e] = t;
        }
        if (!show != !IsWindowVisible(D.ed[e])) ShowWindow(D.ed[e], show ? SW_SHOWNA : SW_HIDE);
    }
}

static void relayout(void)
{
    RECT cr; int cw, chh, m = dp(16), h;
    static HotList scratch;          /* ~275 KB: keep it off the stack */
    Surface *old = g_tgt; HotList *oldhl = g_hl;
    if (!D.hwnd) return;
    GetClientRect(D.hwnd, &cr);
    cw = cr.right; chh = cr.bottom;
    D.two = cw - 2 * m >= dp(800);
    D.cw = D.two ? (cw - 2 * m < dp(900) ? cw - 2 * m : dp(900))
                 : (cw - 2 * m < dp(540) ? cw - 2 * m : dp(540));
    if (D.cw < dp(300)) D.cw = cw - 2 * m > 0 ? cw - 2 * m : cw;
    D.cx = (cw - D.cw) / 2;
    /* measure (hit regions of this pass are thrown away) */
    if (D.surf.dc) g_tgt = &D.surf;
    scratch.n = 0;
    g_hl = &scratch;
    D.cy = 0;
    h = pass(false);
    D.ch = h;
    D.max_scroll = h + 2 * m > chh ? h + 2 * m - chh : 0;
    D.scroll = clampi(D.scroll, 0, D.max_scroll);
    D.cy = D.max_scroll ? m - D.scroll : (chh - h) / 2;
    pass(false);                 /* final rects at the final position */
    g_tgt = old; g_hl = oldhl;
    place_edits();
    redraw();
}

static void ensure_visible(int s)
{
    RECT r; RECT cr; int m = dp(16);
    int e = k_fs_edit[s];
    if (!D.hwnd || !D.max_scroll) return;
    r = e >= 0 ? D.fld[e] : D.rs[s];
    if (IsRectEmpty(&r)) return;
    GetClientRect(D.hwnd, &cr);
    if (r.top < m) D.scroll -= m - r.top;
    else if (r.bottom > cr.bottom - m) D.scroll += r.bottom - (cr.bottom - m);
    else return;
    relayout();
}

/* ================================================================== backdrop */

static void build_backdrop(void)
{
    RECT cr; int w, h, x, y;
    D.bd_dirty = false;
    if (!D.hwnd) return;
    GetClientRect(D.hwnd, &cr);
    w = cr.right; h = cr.bottom;
    if (w <= 0 || h <= 0) return;
    if (!D.bd || D.bd_w != w || D.bd_h != h) {
        DWORD *p = (DWORD *)realloc(D.bd, sizeof(DWORD) * (size_t)w * (size_t)h);
        if (!p) { free(D.bd); D.bd = NULL; D.bd_w = D.bd_h = 0; return; }
        D.bd = p; D.bd_w = w; D.bd_h = h;
    }
    GdiFlush();
    for (y = 0; y < h; y++) {
        DWORD *d = D.bd + (size_t)y * w;
        const DWORD *s = (g.back.px && y < g.back.h) ? g.back.px + (size_t)y * g.back.w : NULL;
        for (x = 0; x < w; x++) {
            DWORD c = (s && x < g.back.w) ? s[x] : C_BG;
            d[x] = mix(c, C_SHADOW, 176);
        }
    }
}

static void reposition(void)
{
    RECT cr; POINT p = { 0, 0 };
    if (!D.hwnd || !g.hwnd || IsIconic(g.hwnd)) return;
    GetClientRect(g.hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    ClientToScreen(g.hwnd, &p);
    SetWindowPos(D.hwnd, NULL, p.x, p.y, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CALLBACK owner_moved(HWINEVENTHOOK hk, DWORD ev, HWND hwnd, LONG obj, LONG child,
                                 DWORD thread, DWORD time)
{
    RECT a, b; POINT p = { 0, 0 };
    (void)hk; (void)ev; (void)thread; (void)time;
    if (hwnd != g.hwnd || obj != OBJID_WINDOW || child != CHILDID_SELF || !D.hwnd) return;
    if (IsIconic(g.hwnd)) return;
    GetClientRect(g.hwnd, &a); ClientToScreen(g.hwnd, &p);
    OffsetRect(&a, p.x, p.y);
    GetWindowRect(D.hwnd, &b);
    if (!EqualRect(&a, &b)) reposition();
}

/* ================================================================== focus */

static bool stop_ok(int s)
{
    if (s == FS_DAY) return D.ed[ED_DAY] && IsWindowEnabled(D.ed[ED_DAY]);
    if (s == FS_MOON) return D.adv && D.ed[ED_MOON];
    if (k_fs_edit[s] >= 0) return D.ed[k_fs_edit[s]] != NULL;
    return true;
}

static void set_focus_stop(int s, bool visible, bool select)
{
    int e;
    if (!D.hwnd) return;
    if (s < 0 || s >= FS_COUNT || !stop_ok(s)) s = FS_MONTH;
    D.foc = s;
    e = k_fs_edit[s];
    if (e >= 0) {
        SetFocus(D.ed[e]);
        if (select) SendMessageW(D.ed[e], EM_SETSEL, 0, -1);
    } else {
        D.foc_vis = visible;
        SetFocus(D.hwnd);
    }
    ensure_visible(s);
    redraw();
}

static void focus_step(int dir)
{
    int s = D.foc, i;
    for (i = 0; i < FS_COUNT; i++) {
        s = (s + dir + FS_COUNT) % FS_COUNT;
        if (stop_ok(s)) break;
    }
    set_focus_stop(s, true, true);
}

/* ================================================================== actions */

static void disarm(void)
{
    if (!D.armed) return;
    D.armed = false;
    relayout();
}

static void set_option(int i)
{
    bool fest;
    if (i < 0 || i >= ALM_START_OPTION_COUNT) return;
    D.option = i;
    fest = option_is_festival(i);
    if (D.ed[ED_DAY]) {
        if (fest && GetFocus() == D.ed[ED_DAY]) set_focus_stop(FS_MONTH, true, false);
        EnableWindow(D.ed[ED_DAY], !fest);
    }
    if (D.armed) disarm(); else relayout();
}

static void open_month_popup(void)
{
    const char *labels[ALM_START_OPTION_COUNT];
    RECT r = D.rs[FS_MONTH];
    POINT p;
    int i, sel;
    for (i = 0; i < ALM_START_OPTION_COUNT; i++) {
        const AlmStartOption *o = alm_start_option(i);
        labels[i] = o ? o->label : "";
    }
    p.x = r.left; p.y = r.bottom;
    ClientToScreen(D.hwnd, &p);                      /* screen coordinates (see CONTRACT GAPS) */
    sel = ui_popup_choice(D.hwnd, p.x, p.y, r.right - r.left, labels, ALM_START_OPTION_COUNT, D.option);
    if (!D.hwnd) return;
    if (sel >= 0 && sel < ALM_START_OPTION_COUNT && sel != D.option) set_option(sel);
    else redraw();
}

static void do_random(void)
{
    char buf[64]; int n;
    alm_random_seed(&g.a, buf, sizeof buf);
    set_text(ED_SEED, wz(buf));
    disarm();
    set_focus_stop(FS_SEED, false, false);
    n = GetWindowTextLengthW(D.ed[ED_SEED]);
    SendMessageW(D.ed[ED_SEED], EM_SETSEL, (WPARAM)n, (LPARAM)n);
}

static void fill_year_name(void)
{
    bool ok; int32_t y = field_int(ED_YN_YEAR, &ok);
    const char *name = ok ? alm_year_name(&g.a, y) : NULL;
    set_text(ED_YN_NAME, wz(name ? name : ""));
}

static void read_form(AlmSettingsForm *f)
{
    char *t;
    memset(f, 0, sizeof *f);
    f->start_day = (int)field_int(ED_DAY, &f->start_day_valid);
    f->start_option = D.option;
    f->start_year = field_int(ED_YEAR, &f->start_year_valid);
    t = edit_text(ED_SEED); copy_u8(f->seed, sizeof f->seed, t); free(t);
    f->yn_year = field_int(ED_YN_YEAR, &f->yn_year_valid);
    t = edit_text(ED_YN_NAME); copy_u8(f->yn_name, sizeof f->yn_name, t); free(t);
    f->celsius = D.celsius;
    f->show_chain_note = D.chain;
    f->moon_offset = field_float(ED_MOON, &f->moon_offset_valid);
    f->now_ms = ui_now_ms();
}

static void do_apply(void)
{
    AlmSettingsForm f;
    AlmApplyResult *r;
    if (!D.hwnd || g.a.busy) return;
    r = (AlmApplyResult *)malloc(sizeof *r);
    if (!r) return;
    read_form(&f);
    *r = alm_settings_apply(&g.a, &f, D.armed);
    switch (r->status) {
    case ALM_APPLY_DATE_ERROR:
        wcopy(D.err, ALM_TOAST_CAP, wz(r->message));
        D.err_on = true;
        relayout();
        break;
    case ALM_APPLY_NEEDS_CONFIRM:
        D.err_on = false;
        build_confirm(r->message);
        D.armed = true;
        relayout();
        set_focus_stop(FS_APPLY, true, false);            /* web: setApply.focus() */
        break;
    case ALM_APPLY_DONE:
        close_dialog(true, &r->result, NULL);
        break;
    default:
        break;
    }
    free(r);
}

static void set_msg(const WCHAR *s) { wcopy(D.msg, MSG_CAP, s); relayout(); }

static const WCHAR k_filter[] = L"Almanac backup (*.json)\0*.json\0All files (*.*)\0*.*\0";

/* Save dialog + atomic write of the web-format backup. msg receives the outcome text
 * ("" when the dialog was cancelled). Also used by main.c when saving fails on close. */
void ui_export_backup(HWND owner, WCHAR *msg, int cap)
{
    SYSTEMTIME st, lt;
    char iso[40], name[512];
    WCHAR path[2048];
    OPENFILENAMEW ofn;
    AlmBuf b;
    if (msg && cap > 0) msg[0] = 0;
    memset(&b, 0, sizeof b);
    GetSystemTime(&st);
    GetLocalTime(&lt);
    wsprintfA(iso, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (!alm_export_json(&g.a, iso, &b) || !b.data) {
        alm_buf_free(&b);
        if (msg) wcopy(msg, cap, L"Could not save the backup.");
        return;
    }
    alm_export_filename(&g.a, lt.wYear, lt.wMonth, lt.wDay, name, sizeof name);
    wcopy(path, 2048, wz(name));
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = k_filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 2048;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Export backup";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn)) {
        char *p8 = u8_dup(path);
        AlmIoStatus io = p8 ? alm_fs_write_atomic(p8, b.data, b.len) : ALM_IO_ERROR;
        free(p8);
        if (msg) {
            if (io == ALM_IO_OK) {
                const WCHAR *base = path + (ofn.nFileOffset < 2048 ? ofn.nFileOffset : 0);
                wcopy(msg, cap, L"Backup saved as ");
                lstrcpynW(msg + lstrlenW(msg), base, cap - lstrlenW(msg));
            } else {
                wcopy(msg, cap, L"Could not save the backup.");
            }
        }
    }
    alm_buf_free(&b);
}

static void do_export(void)
{
    WCHAR m[MSG_CAP];
    ui_export_backup(D.hwnd, m, MSG_CAP);
    if (D.hwnd && m[0]) set_msg(m);
}

static void do_import(void)
{
    WCHAR path[2048];
    OPENFILENAMEW ofn;
    char *p8;
    AlmImportStatus st;
    path[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = D.hwnd;
    ofn.lpstrFilter = k_filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 2048;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Import backup";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn) || !D.hwnd) return;
    p8 = u8_dup(path);
    if (!p8) { set_msg(L"Could not read that file."); return; }
    st = alm_import_file(&g.a, p8, ui_now_ms());
    free(p8);
    switch (st) {
    case ALM_IMPORT_OK: {
        char t[160];
        alm_fmt_import_toast(&g.a, t, sizeof t);
        close_dialog(true, NULL, t);
        break;
    }
    case ALM_IMPORT_NOT_BACKUP: set_msg(L"That file is not an almanac backup."); break;
    case ALM_IMPORT_BUSY: break;
    default: set_msg(L"Could not read that file."); break;
    }
}

static void activate(int id)
{
    switch (id) {
    case H_S_CLOSE: case H_S_CANCEL: close_dialog(false, NULL, NULL); break;
    case H_S_APPLY:  do_apply(); break;
    case H_S_RANDOM: do_random(); break;
    case H_S_UNIT_F: D.celsius = false; redraw(); break;
    case H_S_UNIT_C: D.celsius = true; redraw(); break;
    case H_S_CHAIN:  D.chain = !D.chain; redraw(); break;
    case H_S_ADV:
        D.adv = !D.adv;
        if (!D.adv && GetFocus() == D.ed[ED_MOON]) set_focus_stop(FS_ADV, true, false);
        relayout();
        break;
    case H_S_EXPORT: do_export(); break;
    case H_S_IMPORT: do_import(); break;
    case H_S_MONTH:  open_month_popup(); break;
    default: break;
    }
}

static int stop_of_hot(int id)
{
    switch (id) {
    case H_S_CLOSE: return FS_CLOSE;
    case H_S_MONTH: return FS_MONTH;
    case H_S_RANDOM: return FS_RANDOM;
    case H_S_UNIT_F: case H_S_UNIT_C: return FS_UNIT;
    case H_S_CHAIN: return FS_CHAIN;
    case H_S_ADV: return FS_ADV;
    case H_S_EXPORT: return FS_EXPORT;
    case H_S_IMPORT: return FS_IMPORT;
    case H_S_CANCEL: return FS_CANCEL;
    case H_S_APPLY: return FS_APPLY;
    default: return -1;
    }
}

/* Up/Down in a number field: step like <input type=number> (min/max clamp). */
static void step_number(int e, int dir)
{
    bool ok;
    if (e == ED_MOON) {
        char b[ALM_NUM_CAP];
        double v = field_float(ED_MOON, &ok);
        if (!ok) v = 0;
        /* snap to the 0.5 step grid, then move one step (HTML stepUp / stepDown) */
        v = dir > 0 ? floor(v * 2.0 + 1e-9) / 2.0 + 0.5 : ceil(v * 2.0 - 1e-9) / 2.0 - 0.5;
        if (v < ALM_MOON_OFFSET_MIN) v = ALM_MOON_OFFSET_MIN;
        if (v > ALM_MOON_OFFSET_MAX) v = ALM_MOON_OFFSET_MAX;
        alm_fmt_js_number(v, b, sizeof b);
        SetWindowTextW(D.ed[e], wz(b));
    } else {
        int32_t v = field_int(e, &ok);
        long long n = ok ? (long long)v + dir : (e == ED_DAY ? 1 : 0);
        if (e == ED_DAY) n = n < 1 ? 1 : n > 30 ? 30 : n;
        if (n > IWD_YEAR_MAX) n = IWD_YEAR_MAX;
        if (n < IWD_YEAR_MIN) n = IWD_YEAR_MIN;
        {
            WCHAR b[24]; wsprintfW(b, L"%d", (int)n);
            SetWindowTextW(D.ed[e], b);           /* EN_CHANGE -> disarm / year name */
        }
    }
    {
        int len = GetWindowTextLengthW(D.ed[e]);
        SendMessageW(D.ed[e], EM_SETSEL, (WPARAM)len, (LPARAM)len);
    }
}

/* ================================================================== EDIT subclass */

static int edit_index(HWND h)
{
    int e;
    for (e = 0; e < ED_COUNT; e++) if (D.ed[e] == h) return e;
    return -1;
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    int e = edit_index(h);
    WNDPROC orig = e >= 0 ? D.ed_proc[e] : NULL;
    if (!orig) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            PostMessageW(D.hwnd, WM_DLG_CMD, GetKeyState(VK_SHIFT) < 0 ? CMD_FOCUS_PREV : CMD_FOCUS_NEXT, 0);
            return 0;
        }
        if (wp == VK_RETURN) { PostMessageW(D.hwnd, WM_DLG_CMD, CMD_APPLY, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(D.hwnd, WM_DLG_CMD, CMD_CLOSE, 0); return 0; }
        if ((wp == VK_UP || wp == VK_DOWN) && e != ED_SEED && e != ED_YN_NAME) {
            step_number(e, wp == VK_UP ? 1 : -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\t' || wp == L'\r' || wp == L'\n' || wp == 27) return 0;   /* no beep */
        break;
    case WM_GETDLGCODE:
        return CallWindowProcW(orig, h, m, wp, lp) | DLGC_WANTALLKEYS;
    default:
        break;
    }
    return CallWindowProcW(orig, h, m, wp, lp);
}

/* ================================================================== open / close */

static void fill_from_state(void)
{
    AlmSettingsForm f;
    char b[ALM_NUM_CAP];
    alm_settings_fill(&g.a, &f);
    set_int(ED_DAY, f.start_day);
    set_int(ED_YEAR, f.start_year);
    set_text(ED_SEED, wz(f.seed));
    set_int(ED_YN_YEAR, f.yn_year);
    set_text(ED_YN_NAME, wz(f.yn_name));
    alm_fmt_js_number(f.moon_offset, b, sizeof b);
    set_text(ED_MOON, wz(b));
    D.option = clampi(f.start_option, 0, ALM_START_OPTION_COUNT - 1);
    if (D.ed[ED_DAY]) EnableWindow(D.ed[ED_DAY], !option_is_festival(D.option));
    D.celsius = f.celsius;
    D.chain = f.show_chain_note;
    D.armed = false;
    D.err_on = false;
    D.err[0] = 0;
    wcopy(D.msg, MSG_CAP, k_backup_default);
}

static void close_dialog(bool changed, const AlmResult *res, const char *toast)
{
    AlmResult copy;
    bool have = res != NULL;
    if (!D.hwnd) return;
    memset(&copy, 0, sizeof copy);
    if (have) copy = *res;
    D.closing = true;
    if (g.hwnd) EnableWindow(g.hwnd, TRUE);
    DestroyWindow(D.hwnd);              /* WM_DESTROY resets D.hwnd */
    if (g.hwnd) { SetActiveWindow(g.hwnd); SetFocus(g.hwnd); }
    ui_after_settings(changed);
    if (have) ui_apply_durable(copy);   /* settings apply / regenerate: explicit */
    if (toast) ui_toast(toast);
}

static void create_edits(void)
{
    static const int fnt[ED_COUNT] = { F_BODY, F_BODY, F_MONO, F_BODY, F_BODY, F_BODY };
    static const int maxlen[ED_COUNT] = { 12, 12, ALM_SEED_INPUT_MAX, 12, ALM_YEARNAME_MAX_UNITS, 16 };
    int e;
    for (e = 0; e < ED_COUNT; e++) {
        D.ed[e] = ui_make_edit(D.hwnd, 1000 + e, false, maxlen[e], fnt[e], "");
        SetRectEmpty(&D.ed_at[e]);
        if (!D.ed[e]) continue;
        SendMessageW(D.ed[e], EM_LIMITTEXT, (WPARAM)maxlen[e], 0);
        D.ed_proc[e] = (WNDPROC)SetWindowLongPtrW(D.ed[e], GWLP_WNDPROC, (LONG_PTR)edit_proc);
        ShowWindow(D.ed[e], SW_HIDE);
    }
    if (D.ed[ED_YN_NAME])
        SendMessageW(D.ed[ED_YN_NAME], EM_SETCUEBANNER, FALSE, (LPARAM)k_yn_placeholder);
}

void settings_open(bool focus_seed)
{
    RECT cr; POINT p = { 0, 0 };
    ui_notes_flush();                   /* web: cal.flushNote() */
    if (!D.hwnd) {
        if (!g.hwnd || !D.registered) return;
        GetClientRect(g.hwnd, &cr);
        ClientToScreen(g.hwnd, &p);
        D.hwnd = CreateWindowExW(0, DLG_CLASS, L"Settings", WS_POPUP | WS_CLIPCHILDREN,
                                 p.x, p.y, cr.right > 0 ? cr.right : 1, cr.bottom > 0 ? cr.bottom : 1,
                                 g.hwnd, NULL, D.inst, NULL);
        if (!D.hwnd) return;
        g.set_hwnd = D.hwnd;
        D.closing = false;
        D.scroll = 0;
        D.hot_id = D.press_id = 0;
        D.tracking = false;
        read_field_colors();
        create_edits();
        D.hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
                                 owner_moved, GetCurrentProcessId(), GetCurrentThreadId(),
                                 WINEVENT_OUTOFCONTEXT);
        D.bd_dirty = true;
    }
    fill_from_state();
    relayout();
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(D.hwnd, SW_SHOW);
    SetForegroundWindow(D.hwnd);
    if (focus_seed) set_focus_stop(FS_SEED, true, true);
    else set_focus_stop(option_is_festival(D.option) ? FS_MONTH : FS_DAY, true, false);
}

void settings_fonts_changed(void)
{
    int e;
    if (!D.hwnd) return;
    for (e = 0; e < ED_COUNT; e++) {
        if (!D.ed[e]) continue;
        ui_style_edit(D.ed[e], e == ED_SEED ? F_MONO : F_BODY);
        SetRectEmpty(&D.ed_at[e]);
    }
    read_field_colors();
    reposition();
    D.bd_dirty = true;
    relayout();
}

/* ================================================================== window procedure */

static void paint(HWND h)
{
    PAINTSTRUCT ps; RECT cr;
    Surface *old = g_tgt; HotList *oldhl = g_hl;
    HDC dc = BeginPaint(h, &ps);
    GetClientRect(h, &cr);
    if (cr.right > 0 && cr.bottom > 0 && surf_resize(&D.surf, cr.right, cr.bottom)) {
        if (D.bd_dirty || !D.bd || D.bd_w != cr.right || D.bd_h != cr.bottom) build_backdrop();
        g_tgt = &D.surf;
        g_hl = &D.hl;
        D.hl.n = 0;
        if (D.bd && D.bd_w == cr.right && D.bd_h == cr.bottom) px_copy(D.bd, D.bd_w, D.bd_h, 0, 0);
        else px_fill(0, 0, cr.right, cr.bottom, C_SHADOW);
        pass(true);
        aa_flush();
        GdiFlush();
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, D.surf.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        g_tgt = old;
        g_hl = oldhl;
    }
    EndPaint(h, &ps);
}

static void set_hot(int id)
{
    if (id != D.hot_id) { D.hot_id = id; redraw(); }
}

static void on_key(WPARAM vk, bool alt)
{
    bool shift = GetKeyState(VK_SHIFT) < 0;
    int s = D.foc;
    if (vk == VK_ESCAPE) { close_dialog(false, NULL, NULL); return; }
    if (vk == VK_TAB) { focus_step(shift ? -1 : 1); return; }
    if (s == FS_MONTH) {
        if ((alt && (vk == VK_DOWN || vk == VK_UP)) || vk == VK_F4 || vk == VK_SPACE) { open_month_popup(); return; }
        if (vk == VK_UP || vk == VK_LEFT) { if (D.option > 0) set_option(D.option - 1); D.foc_vis = true; return; }
        if (vk == VK_DOWN || vk == VK_RIGHT) { if (D.option < ALM_START_OPTION_COUNT - 1) set_option(D.option + 1); D.foc_vis = true; return; }
        if (vk == VK_HOME) { set_option(0); return; }
        if (vk == VK_END) { set_option(ALM_START_OPTION_COUNT - 1); return; }
    }
    if (s == FS_UNIT) {
        if (vk == VK_LEFT || vk == VK_UP)    { D.celsius = false; D.foc_vis = true; redraw(); return; }
        if (vk == VK_RIGHT || vk == VK_DOWN) { D.celsius = true; D.foc_vis = true; redraw(); return; }
        if (vk == VK_SPACE) return;           /* already checked */
    }
    if (vk == VK_SPACE) {
        static const int ids[FS_COUNT] = { H_S_CLOSE, 0, H_S_MONTH, 0, 0, H_S_RANDOM, 0, 0, 0,
                                           H_S_CHAIN, H_S_ADV, 0, H_S_EXPORT, H_S_IMPORT,
                                           H_S_CANCEL, H_S_APPLY };
        if (s >= 0 && s < FS_COUNT && ids[s]) activate(ids[s]);
        return;
    }
    if (vk == VK_RETURN) {
        switch (s) {
        case FS_CLOSE: activate(H_S_CLOSE); break;
        case FS_RANDOM: activate(H_S_RANDOM); break;
        case FS_ADV: activate(H_S_ADV); break;
        case FS_EXPORT: activate(H_S_EXPORT); break;
        case FS_IMPORT: activate(H_S_IMPORT); break;
        case FS_CANCEL: activate(H_S_CANCEL); break;
        default: do_apply(); break;           /* implicit form submission */
        }
    }
}

static LRESULT CALLBACK dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_SIZE:
        D.bd_dirty = true;
        if (D.hwnd) relayout();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && D.hwnd) {
            int s = D.foc;
            if (s < 0 || s >= FS_COUNT || !stop_ok(s)) s = FS_MONTH;
            if (k_fs_edit[s] >= 0) SetFocus(D.ed[k_fs_edit[s]]);
            else SetFocus(h);
            redraw();
            return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        redraw();
        return 0;
    case WM_MOUSEMOVE: {
        const Hot *ht = hot_find(&D.hl, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!D.tracking) {
            TRACKMOUSEEVENT t; t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = h; t.dwHoverTime = 0;
            D.tracking = TrackMouseEvent(&t) != 0;
        }
        set_hot(ht && ht->enabled ? ht->id : 0);
        return 0;
    }
    case WM_MOUSELEAVE:
        D.tracking = false;
        set_hot(0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) {
            POINT p; const Hot *ht;
            GetCursorPos(&p); ScreenToClient(h, &p);
            ht = hot_find(&D.hl, p.x, p.y);
            SetCursor(LoadCursorW(NULL, ht && ht->enabled ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), e;
        POINT p; RECT card;
        const Hot *ht;
        p.x = x; p.y = y;
        SetRect(&card, D.cx, D.cy, D.cx + D.cw, D.cy + D.ch);
        if (!PtInRect(&card, p)) { close_dialog(false, NULL, NULL); return 0; }   /* backdrop */
        for (e = 0; e < ED_COUNT; e++)
            if (D.ed[e] && IsWindowVisible(D.ed[e]) && IsWindowEnabled(D.ed[e]) && PtInRect(&D.fld[e], p)) {
                SetFocus(D.ed[e]);
                return 0;
            }
        ht = hot_find(&D.hl, x, y);
        if (!ht || !ht->enabled) return 0;
        D.foc = stop_of_hot(ht->id) >= 0 ? stop_of_hot(ht->id) : D.foc;
        D.foc_vis = false;
        SetFocus(h);
        if (ht->id == H_S_MONTH) { open_month_popup(); return 0; }   /* selects open on press */
        D.press_id = ht->id;
        D.hot_id = ht->id;
        SetCapture(h);
        redraw();
        return 0;
    }
    case WM_LBUTTONUP: {
        int id = D.press_id;
        const Hot *ht = hot_find(&D.hl, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!id) return 0;
        D.press_id = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (ht && ht->id == id) activate(id);
        redraw();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (D.press_id) { D.press_id = 0; redraw(); }
        return 0;
    case WM_MOUSEWHEEL:
        if (D.max_scroll) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            D.scroll = clampi(D.scroll - delta * dp(48) / WHEEL_DELTA, 0, D.max_scroll);
            relayout();
        }
        return 0;
    case WM_MOUSEHWHEEL:
        return 0;
    case WM_KEYDOWN:
        on_key(wp, false);
        return 0;
    case WM_SYSKEYDOWN:
        if ((wp == VK_DOWN || wp == VK_UP) && D.foc == FS_MONTH && GetFocus() == h) { on_key(wp, true); return 0; }
        break;
    case WM_DLG_CMD:
        switch ((int)wp) {
        case CMD_CLOSE: close_dialog(false, NULL, NULL); break;
        case CMD_APPLY: do_apply(); break;
        case CMD_FOCUS_NEXT: focus_step(1); break;
        case CMD_FOCUS_PREV: focus_step(-1); break;
        default: break;
        }
        return 0;
    case WM_COMMAND: {
        int e = (int)LOWORD(wp) - 1000, code = HIWORD(wp);
        if (e < 0 || e >= ED_COUNT || (HWND)lp != D.ed[e]) break;
        if (code == EN_CHANGE && !D.filling) {
            if (e == ED_DAY || e == ED_YEAR || e == ED_SEED) disarm();
            else if (e == ED_YN_YEAR) fill_year_name();
        } else if (code == EN_SETFOCUS) {
            D.foc = k_edit_fs[e];
            ensure_visible(D.foc);
            redraw();
        } else if (code == EN_KILLFOCUS) {
            redraw();
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB_OF(C_TEXT));
        SetBkColor(dc, RGB_OF(D.field_bg));
        return (LRESULT)field_brush(false);
    }
    case WM_CTLCOLORSTATIC: {           /* disabled / read-only EDIT */
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB_OF(C_DIM));
        SetBkColor(dc, RGB_OF(D.field_dis));
        return (LRESULT)field_brush(true);
    }
    case WM_MEASUREITEM:
        return ui_menu_measure((MEASUREITEMSTRUCT *)lp);
    case WM_DRAWITEM:
        return ui_menu_draw((DRAWITEMSTRUCT *)lp);
    case WM_CLOSE:
        close_dialog(false, NULL, NULL);
        return 0;
    case WM_DESTROY: {
        int e;
        if (D.hook) { UnhookWinEvent(D.hook); D.hook = NULL; }
        for (e = 0; e < ED_COUNT; e++) { D.ed[e] = NULL; D.ed_proc[e] = NULL; }
        free(D.bd); D.bd = NULL; D.bd_w = D.bd_h = 0;
        surf_free(&D.surf);
        D.hl.n = 0;
        if (!D.closing && g.hwnd) EnableWindow(g.hwnd, TRUE);   /* destroyed from outside */
        D.closing = false;
        D.hwnd = NULL;
        if (g.set_hwnd == h) g.set_hwnd = NULL;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool settings_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    D.inst = inst;
    if (D.registered) return true;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = DLG_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    D.registered = true;
    return true;
}
