/* main.c - Icewind Dale Almanac, native Win32 front end: the window, the message loop,
 * input dispatch, EDIT child controls, the owner-drawn popup menu, timers, persistence,
 * sound and the command-line test hooks. Painting is done by ui_view*.c into g.back;
 * all behaviour lives in engine.c / store.c (SPEC.md).
 *
 * Idle cost: nothing runs while idle. Timers exist only while something is in motion:
 *   T_TOAST    one-shot, hides the toast after 4.2 s (day toasts 7 s)
 *   T_NOTE     one-shot notes autosave debounce (600 ms)
 *   T_ARM      one-shot, disarms a two-step delete after 3 s
 *   T_SAVE     one-shot debounced save (ALM_SAVE_SOON_MS = 250 ms, killed when it fires):
 *              routine steps (time / day steps including a midnight advance, Advance Day,
 *              scrubbing, calendar paging, select, the dice-ledger chip) save non-durably
 *              through it. Explicit or destructive actions (close / end session, boot,
 *              import, settings, event add / edit / delete, notes, unit and sound toggles,
 *              make-today, undo) save durably at once via ui_save_now(), which also cancels
 *              a pending debounced save (the durable write covers it).
 *   T_THUNDER  one-shot, re-armed only while sound is on and the viewed hour is a
 *              Lightning Storm (random 0.8-4.3 s)
 *   T_FADE     30 ms ticks, 8 at most: hero cross-fade when a discrete step (key or click,
 *              never scrubbing) changes the weather event
 * WM_PAINT re-composes the back buffer (view_paint) only when something changed
 * (ui_redraw / ui_relayout / resize); plain exposes just blit it.
 *
 * HIT-REGION DISPATCH (id: arg expected here; every id is handled defensively)
 *   H_NONE, H_TIP          ignored (cover / tooltip only)
 *   H_UNIT_F / H_UNIT_C 0  alm_set_unit
 *   H_SOUND           0    ui_sound_toggle
 *   H_INTERIOR        0    ui_interior_toggle (only while the sound is on)
 *   H_SETTINGS, H_INTRO_SETTINGS 0  Settings
 *   H_ADVANCE         0    alm_advance(12)
 *   H_STEP            d    alm_step_minutes(d), d = -60 -30 -5 5 30 60
 *   H_DAY_PREV / H_DAY_NEXT 0       alm_step_day(-1 / +1)
 *   H_TIMELINE        any  press starts scrubbing: alm_set_time(view_timeline_time(x)),
 *                          snapped to 5 minutes, saved debounced
 *   H_CAL_PREV / H_CAL_NEXT 0       alm_cal_step (save debounced 250 ms)
 *   H_CAL_TODAY, H_BACK_TODAY 0     alm_cal_today (back to the party's day and clock)
 *   H_CAL_CELL        k    index into view_cells(); click = alm_select(abs, true),
 *                          double click = alm_set_current(abs)
 *   H_UPCOMING        abs  alm_select(abs, false)
 *   H_SET_CURRENT     0    alm_set_current(sel_abs)
 *   H_EV_EDIT         i    index into alm_marks_on(sel_abs): ui_open_form(marks[i].event)
 *   H_EV_DEL          i    same index: first press arms (g.armed_id, 3 s), second deletes
 *   H_ADD_EVENT       0    ui_open_form(NULL)
 *   H_FORM_TYPE       t    g.form_type = t (AlmEvType)
 *   H_FORM_REPEAT     0    ui_popup_choice under the hit rect -> g.form_repeat
 *   H_FORM_DAYS      -1/+1 "Lasts" stepper: one day shorter / longer (1..ALM_EVENT_DAYS_MAX);
 *                          the number field g.ed_days takes typed digits, Up / Down step
 *   H_FORM_CANCEL / H_FORM_SAVE 0   ui_close_form / ui_form_submit
 *   H_NOTES           0    ui_notes_begin
 *   H_LEDGER          0    open / close the dice ledger (native.ledger, persisted)
 *   H_KEYS / H_KEYS_CARD 0 open / close the keyboard shortcuts card
 *   H_HEAD_NOW        0    back to the weather (scroll to top)
 *   H_SAVE_FAIL       0    Save dialog for a backup
 *   H_TOAST           0    dismiss the toast
 *   H_SCROLL          0 thumb drag (g.sb_grab = y - thumb top), -1 / +1 page up / down
 *   H_S_*                  settings dialog (handled by ui_settings.c; ignored here)
 *
 * KEYBOARD (main view focused; ignored while a text field has focus) - SPEC 6.2
 *   Right, Space        +1 hour         Left        -1 hour
 *   Shift+Right / Left  +-30 minutes    Ctrl+Right / Left  +-5 minutes
 *                       Forward past midnight on today advances the party (Ctrl+Z steps
 *                       back); back past 00:00 views the previous day (today stays).
 *                       Auto-repeat steps the clock but never crosses midnight.
 *   N                   Advance Day (lands at 12:00); Enter never advances (too easy to
 *                       hit twice). Every advance / make-today toasts "Ctrl+Z steps back".
 *   Ctrl+Z              one-level undo of the last advance / make-today / midnight step,
 *                       or of an event deletion when that came later ("restored")
 *   PageUp / PageDown   previous / next day
 *   T, Esc              back to today (Esc first closes the event form, else the toast)
 *   E / O               new event / write a note for the viewed day (scroll restored after)
 *   M                   wind sound        I    interior (muffled), while the sound is on
 *   F2                  Settings         F1   keyboard shortcuts card (any key closes it)
 *   Up / Down           scroll the page; Ctrl+Home / Ctrl+End top / bottom
 * Time-changing keys scroll the page back to the weather. While another day is viewed the
 * party's own clock is kept (g.party_time) and restored on the way back to today.
 *   In EDIT fields: Esc closes the form / leaves the notes (flushed); Enter in Title or
 *   Ctrl+Enter in Details submits; Tab moves between Title and Details; Ctrl+A selects all.
 *
 * TEST HOOKS (documented in native/README.md)
 *   --state <path>  --seed <text>  --days <n>  --select <offset>  --hour <h>  --minute <m>
 *   --size WxH  --scroll <px>  --open settings|eventform  --shot-ready
 * The window title is the constant "Icewind Dale Almanac" (+ " [ready]" for --shot-ready).
 */
#include "ui.h"
#include "ui_blocks.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

Ui g;

/* ================================================================== private state */

enum { T_TOAST = 1, T_NOTE, T_ARM, T_SAVE, T_THUNDER, T_FADE };
enum { ID_ED_TITLE = 201, ID_ED_DETAILS, ID_ED_NOTES, ID_ED_DAYS };
#define WM_APP_DROP_NOTES (WM_APP + 1)
#define WM_APP_HOOKS      (WM_APP + 2)

static bool     s_dirty = true;          /* back buffer needs view_paint */
static bool     s_persist = true;        /* false: --seed without --state (no file writes) */
static bool     s_save_failed;
static bool     s_save_pending;          /* a debounced save is scheduled (T_SAVE) */
static int      s_durable;               /* > 0: ui_apply saves durably at once */
static bool     s_thunder_timer;
static int32_t  s_last_sel = INT32_MIN;
static bool     s_form_scroll;           /* bring the event form into view after the next paint */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static HWND     s_tip_tool_owner;
static RECT     s_tip_rect;
static HANDLE   s_mutex;
static int      s_last_type = ALM_EV_REMINDER;   /* type preselected for a new event */
static WCHAR    s_bad_name[MAX_PATH];            /* corrupt save kept under this name */
static int      s_boot_notice;                   /* 1 corrupt (renamed), 2 unreadable, 3 rename failed */

/* one-level undo of a move of "today" (not persisted, no timer) */
static struct { bool ok; int32_t cur_abs; double time; } s_undo;
/* one-level undo of an event deletion (a deep copy; the most recent undoable action while
 * ok: a later move of today forgets it) */
static struct { bool ok; AlmEvent ev; int idx; } s_undo_del;

static void undo_del_forget(void)
{
    if (s_undo_del.ok) alm_event_release(&s_undo_del.ev);
    s_undo_del.ok = false;
}

/* test hooks */
static WCHAR   *s_opt_state;
static char    *s_opt_seed;
static int      s_opt_days = -1, s_opt_select = INT32_MIN, s_opt_hour = -1, s_opt_minute = -1;
static int      s_opt_w, s_opt_h, s_opt_scroll = -1;
static int      s_opt_open;              /* 1 settings, 2 eventform */
static bool     s_opt_shot_ready;

static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static bool any_edit_focused(void)
{
    HWND f = GetFocus();
    return f && (f == g.ed_title || f == g.ed_details || f == g.ed_notes || f == g.ed_days);
}

/* ================================================================== redraw */

void ui_redraw(void)
{
    s_dirty = true;
    if (g.hwnd) InvalidateRect(g.hwnd, NULL, FALSE);
}

void ui_relayout(void)
{
    g.layout_dirty = true;
    ui_redraw();
}

/* ================================================================== time, entropy */

double ui_now_ms(void)
{
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)((u.QuadPart - 116444736000000000ull) / 10000ull);
}

static uint64_t entropy(void)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart ^ (GetTickCount64() << 20) ^ ((uint64_t)GetCurrentProcessId() << 40)
           ^ (uint64_t)(ui_now_ms() * 1000.0);
}

/* ================================================================== persistence */

static void path_w(const char *utf8, WCHAR *out, int cap)
{
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) <= 0) out[0] = 0;
}

static void save_write(bool durable)
{
    s_save_pending = false;
    if (g.hwnd) KillTimer(g.hwnd, T_SAVE);
    if (!s_persist || !g.state_path[0]) return;
    if (alm_save_file_ex(&g.a, g.state_path, durable) != ALM_IO_OK) {
        if (!s_save_failed) {
            s_save_failed = true;
            ui_toast("<b>Could not save the almanac.</b> Changes live only in memory until a save works; on closing you can save a backup elsewhere.");
        }
    } else {
        s_save_failed = false;
    }
    if (g.save_failed != s_save_failed) { g.save_failed = s_save_failed; ui_redraw(); }
}

/* explicit / destructive actions: flush now, durably (a pending debounced save is
 * cancelled: this write already contains its state) */
void ui_save_now(void)
{
    save_write(true);
}

/* routine steps: one non-durable save ALM_SAVE_SOON_MS after the last of a burst */
static void save_soon(void)
{
    s_save_pending = true;
    if (g.hwnd) SetTimer(g.hwnd, T_SAVE, ALM_SAVE_SOON_MS, NULL);
    else save_write(false);
}

/* ui_apply with ALM_FX_SAVE written durably at once (explicit actions) */
void ui_apply_durable(AlmResult r)
{
    s_durable++;
    ui_apply(r);
    s_durable--;
}
#define DURABLE(stmt) do { s_durable++; stmt; s_durable--; } while (0)

static bool file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void to_utf8(const WCHAR *w, char *out, int cap)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

/* --state <path>, else a portable state.json next to the exe, else
 * %APPDATA%\IcewindDaleAlmanac\state.json (directory created). */
static void resolve_state_path(void)
{
    WCHAR p[MAX_PATH * 2];
    p[0] = 0;
    if (s_opt_state) {
        if (!GetFullPathNameW(s_opt_state, MAX_PATH * 2, p, NULL)) lstrcpynW(p, s_opt_state, MAX_PATH * 2);
    } else {
        WCHAR exe[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2) {
            WCHAR *s = exe + n;
            while (s > exe && s[-1] != L'\\' && s[-1] != L'/') s--;
            *s = 0;
            lstrcpynW(p, exe, MAX_PATH * 2);
            lstrcatW(p, L"state.json");
            if (!file_exists(p)) p[0] = 0;
        }
        if (!p[0]) {
            WCHAR base[MAX_PATH * 2];
            DWORD k = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH * 2);
            if (k == 0 || k >= MAX_PATH * 2) {
                if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base))) base[0] = 0;
            }
            if (base[0]) {
                char dir[MAX_PATH * 3];
                lstrcpynW(p, base, MAX_PATH * 2);
                lstrcatW(p, L"\\IcewindDaleAlmanac");
                to_utf8(p, dir, sizeof dir);
                alm_fs_mkdirs(dir);
                lstrcatW(p, L"\\state.json");
            } else {
                lstrcpyW(p, L"state.json");
            }
        }
    }
    to_utf8(p, g.state_path, (int)sizeof g.state_path);
}

/* {"v":2,"seed":"..."} so --seed goes through clean() like any loaded state */
static void load_seeded(const char *seed)
{
    size_t n = strlen(seed), i, k = 0;
    char *j = (char *)malloc(n * 6 + 32);
    if (!j) return;
    k += (size_t)sprintf(j, "{\"v\":2,\"seed\":\"");
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)seed[i];
        if (c == '"' || c == '\\') { j[k++] = '\\'; j[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)sprintf(j + k, "\\u%04x", c);
        else j[k++] = (char)c;
    }
    k += (size_t)sprintf(j + k, "\"}");
    alm_load_json(&g.a, j, k);
    free(j);
}

/* ================================================================== toasts */

static void toast_timer(int ms)
{
    if (g.hwnd) SetTimer(g.hwnd, T_TOAST, (UINT)ms, NULL);
}

void ui_toast(const char *text)
{
    if (!text || !*text) return;
    lstrcpynA(g.toast, text, ALM_TOAST_CAP);
    g.toast_day = false;
    g.toast_on = true;
    g.toast_sticky = false;
    g.toast_hint[0] = 0;
    toast_timer(ALM_TOAST_MS);
    ui_redraw();
}

static void day_toast(int32_t abs, const char *lead)
{
    char title[160]; AlmMark mk[2]; int n = 0;
    if (!alm_day_toast(&g.a, abs, lead ? lead : "Today", title, sizeof title, mk, 2, &n)) return;
    g.toast_day = true;
    g.toast_abs = abs;
    lstrcpynA(g.toast_lead, lead ? lead : "Today", (int)sizeof g.toast_lead);
    g.toast[0] = 0;
    g.toast_on = true;
    g.toast_sticky = false;
    g.toast_hint[0] = 0;
    toast_timer(ALM_DAY_TOAST_MS);
    ui_redraw();
}

static void toast_sticky(const char *text)
{
    ui_toast(text);
    KillTimer(g.hwnd, T_TOAST);
    g.toast_sticky = true;
}

static void toast_hide(void)
{
    if (!g.toast_on) return;
    g.toast_on = false;
    g.toast_sticky = false;
    KillTimer(g.hwnd, T_TOAST);
    ui_redraw();
}

/* ================================================================== sound */

static int viewed_event(void)
{
    return alm_event_at(&g.a, g.a.view, alm_hour_now(&g.a));
}

static void thunder_sync(void)
{
    bool want = audio_is_enabled() && alm_has_view(&g.a) && viewed_event() == 10;
    if (want && !s_thunder_timer) {
        SetTimer(g.hwnd, T_THUNDER, 800 + rnd32() % 3500, NULL);
        s_thunder_timer = true;
    } else if (!want && s_thunder_timer) {
        KillTimer(g.hwnd, T_THUNDER);
        s_thunder_timer = false;
    }
}

static void wind_sync(bool force)
{
    int id = viewed_event();
    const IwdLook *lk;
    if (!force && id == g.last_wind_event) { thunder_sync(); return; }
    g.last_wind_event = id;
    lk = iwd_look(id);
    audio_set_wind(lk ? lk->wind : 0.0);
    thunder_sync();
}

void ui_sound_toggle(void)
{
    bool want = !audio_is_enabled();
    bool ok = audio_set_enabled(want);
    if (want && !ok) {
        g.sound_missing = true;
        g.a.native.sound = false;
    } else {
        g.sound_missing = false;
        g.a.native.sound = want;
    }
    wind_sync(true);
    ui_save_now();
    ui_redraw();
}

/* Interior: the sound as heard from inside a building. Sound only; changeable only while
 * the sound is on (the chip is dimmed otherwise), remembered in the native prefs. */
void ui_interior_toggle(void)
{
    if (g.sound_missing || !audio_is_enabled()) return;
    g.a.native.interior = !g.a.native.interior;
    audio_set_interior(g.a.native.interior);
    ui_save_now();
    ui_redraw();
}

/* ================================================================== EDIT controls */

static HMODULE s_uxtheme;
typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);

static void dark_theme(HWND h, const WCHAR *app)
{
    static SetWindowThemeFn fn; static bool tried;
    if (!tried) {
        tried = true;
        s_uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (s_uxtheme) fn = (SetWindowThemeFn)(void (*)(void))GetProcAddress(s_uxtheme, "SetWindowTheme");
    }
    if (fn && h) fn(h, app, NULL);
}

void ui_style_edit(HWND ed, int fnt)
{
    if (!ed) return;
    if (fnt < 0 || fnt >= F_COUNT) fnt = F_BODY;
    SetPropW(ed, L"iwd.font", (HANDLE)(INT_PTR)(fnt + 1));
    SendMessageW(ed, WM_SETFONT, (WPARAM)font(fnt), TRUE);
    SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dp(2), dp(2)));
    if (GetWindowLongPtrW(ed, GWL_STYLE) & ES_MULTILINE) dark_theme(ed, L"DarkMode_Explorer");
}

/* heap WCHAR copy of UTF-8 with \n expanded to \r\n */
static WCHAR *crlf_dup(const char *utf8)
{
    WCHAR *w = wz_dup(utf8 ? utf8 : ""), *o;
    size_t n = 0, i, k = 0;
    if (!w) return NULL;
    for (i = 0; w[i]; i++) n += (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) ? 2 : 1;
    o = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (!o) return w;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) o[k++] = L'\r';
        o[k++] = w[i];
    }
    o[k] = 0;
    free(w);
    return o;
}

char *ui_edit_text(HWND ed)
{
    int n; WCHAR *w; char *u; int i, k = 0;
    if (!ed) return NULL;
    n = GetWindowTextLengthW(ed);
    w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!w) return NULL;
    n = GetWindowTextW(ed, w, n + 1);
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {                           /* \r\n -> \n (textarea value) */
        if (w[i] == L'\r' && i + 1 < n && w[i + 1] == L'\n') continue;
        w[k++] = w[i];
    }
    w[k] = 0;
    u = u8_dup(w);
    free(w);
    return u;
}

static void notes_drop(void);

/* Esc / Tab in the notes: back to the page (and to the weather if a shortcut opened it) */
static void notes_leave(void)
{
    SetFocus(g.hwnd);
    if (g.saved_scroll >= 0) { view_scroll_to(g.saved_scroll); g.saved_scroll = -1; }
}

/* "Lasts" field: current value (0 when empty / not a number) */
static int form_days_read(void)
{
    WCHAR b[16];
    int v = 0, i;
    if (!g.ed_days) return g.form_days;
    GetWindowTextW(g.ed_days, b, 16);
    for (i = 0; b[i]; i++) {
        if (b[i] < L'0' || b[i] > L'9') return 0;
        v = v * 10 + (b[i] - L'0');
        if (v > 1000) return 1000;
    }
    return v;
}

static void form_days_step(int d)
{
    int v = form_days_read();
    WCHAR b[16];
    bool was = g.form_days_invalid;
    v = clampi((v < 1 ? (d > 0 ? 0 : 2) : v) + d, 1, ALM_EVENT_DAYS_MAX);
    g.form_days = v;
    g.form_days_invalid = false;
    if (g.ed_days) {
        wsprintfW(b, L"%d", v);
        SetWindowTextW(g.ed_days, b);
        SendMessageW(g.ed_days, EM_SETSEL, 0, -1);
    }
    if (was) ui_relayout(); else ui_redraw();     /* the error line comes and goes */
}

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, L"iwd.proc");
    HWND parent = GetParent(h);
    bool mine = parent == g.hwnd;
    bool multi = (GetWindowLongPtrW(h, GWL_STYLE) & ES_MULTILINE) != 0;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (!old) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == 'A' && ctrl) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (!mine) {
            /* settings dialog fields: Esc / Tab / Enter (single line) go to the dialog */
            if (wp == VK_ESCAPE || wp == VK_TAB || (wp == VK_RETURN && !multi)) {
                SendMessageW(parent, WM_KEYDOWN, wp, lp);
                return 0;
            }
            break;
        }
        if (wp == VK_ESCAPE) {
            if (h == g.ed_notes) notes_leave();
            else ui_close_form();
            return 0;
        }
        if (wp == VK_TAB) {
            if (h == g.ed_notes) notes_leave();
            else if (h == g.ed_title && (g.ed_days || g.ed_details)) {
                HWND nx = shift ? g.ed_details : g.ed_days ? g.ed_days : g.ed_details;
                if (nx) { SetFocus(nx); if (nx == g.ed_days) SendMessageW(nx, EM_SETSEL, 0, -1); }
            }
            else if (h == g.ed_days) {
                HWND nx = shift ? g.ed_title : g.ed_details;
                if (nx) SetFocus(nx);
            }
            else if (h == g.ed_details && g.ed_title) {
                HWND nx = shift && g.ed_days ? g.ed_days : g.ed_title;
                SetFocus(nx);
                if (nx == g.ed_days) SendMessageW(nx, EM_SETSEL, 0, -1);
            }
            (void)shift;
            return 0;
        }
        if (h == g.ed_days && (wp == VK_UP || wp == VK_DOWN)) {
            form_days_step(wp == VK_UP ? 1 : -1);
            return 0;
        }
        if (wp == VK_RETURN && ((h == g.ed_title) || h == g.ed_days || (h == g.ed_details && ctrl))) {
            ui_form_submit();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == 27 || wp == L'\t') return 0;                 /* no beep */
        if ((wp == L'\r' || wp == L'\n') && (!multi || (mine && h == g.ed_details && ctrl))) return 0;
        if (wp == 1 && ctrl) return 0;                          /* Ctrl+A */
        break;
    case WM_MOUSEWHEEL:
        if (!multi && parent) return SendMessageW(parent, msg, wp, lp);
        break;
    case WM_NCDESTROY:
        RemovePropW(h, L"iwd.proc");
        RemovePropW(h, L"iwd.font");
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
        return CallWindowProcW(old, h, msg, wp, lp);
    default:
        break;
    }
    return CallWindowProcW(old, h, msg, wp, lp);
}

HWND ui_make_edit(HWND parent, int id, bool multiline, int maxlen, int fnt, const char *text_utf8)
{
    DWORD st = WS_CHILD | WS_TABSTOP | (multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : ES_AUTOHSCROLL);
    HWND ed = CreateWindowExW(0, L"EDIT", L"", st, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst, NULL);
    WCHAR *w;
    if (!ed) return NULL;
    SetPropW(ed, L"iwd.proc", (HANDLE)GetWindowLongPtrW(ed, GWLP_WNDPROC));
    SetWindowLongPtrW(ed, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    if (maxlen > 0) SendMessageW(ed, EM_SETLIMITTEXT, (WPARAM)maxlen, 0);
    ui_style_edit(ed, fnt);
    if (text_utf8 && *text_utf8) {
        w = multiline ? crlf_dup(text_utf8) : wz_dup(text_utf8);
        if (w) { SetWindowTextW(ed, w); free(w); }
    }
    return ed;
}

/* multiline EDITs show their scrollbar only when the text overflows */
static void edit_vsb(HWND ed)
{
    RECT r; int lines, vis, lh = fh(F_BODY) > 0 ? fh(F_BODY) : 16;
    bool want, has;
    if (!ed || !(GetWindowLongPtrW(ed, GWL_STYLE) & ES_MULTILINE)) return;
    GetClientRect(ed, &r);
    lines = (int)SendMessageW(ed, EM_GETLINECOUNT, 0, 0);
    vis = (r.bottom - r.top) / lh;
    has = (GetWindowLongPtrW(ed, GWL_STYLE) & WS_VSCROLL) != 0;
    want = lines > (vis > 0 ? vis : 1);
    /* with the bar the text column narrows and may re-wrap: keep it while still needed */
    if (want != has) ShowScrollBar(ed, SB_VERT, want);
}

/* --- placement of the main window's EDITs from the content rects (after each paint) */

typedef struct EdPlace { HWND ed; RECT r; int clip0, clip1; bool shown; } EdPlace;
static EdPlace s_place[4];

static void edit_place(int slot, HWND ed, const RECT *rc)
{
    EdPlace *p = &s_place[slot];
    int hh = view_header_h(), H = g.back.h, y0, y1, w, h, c0, c1;
    RECT nr;
    if (p->ed != ed) { memset(p, 0, sizeof *p); p->ed = ed; }
    if (!ed) return;
    if (IsRectEmpty(rc)) {
        if (p->shown || IsWindowVisible(ed)) ShowWindow(ed, SW_HIDE);
        p->shown = false;
        return;
    }
    y0 = rc->top - g.scroll + hh; y1 = rc->bottom - g.scroll + hh;
    w = rc->right - rc->left; h = y1 - y0;
    c0 = clampi(hh - y0, 0, h); c1 = clampi(H - y0, 0, h);
    if (c1 <= c0 || w <= 0) {
        if (p->shown) ShowWindow(ed, SW_HIDE);
        p->shown = false;
        return;
    }
    SetRect(&nr, rc->left, y0, rc->right, y1);
    if (!EqualRect(&nr, &p->r) || !p->shown) {
        bool resized = nr.right - nr.left != p->r.right - p->r.left;
        bool resized_h = nr.bottom - nr.top != p->r.bottom - p->r.top;
        SetWindowPos(ed, HWND_TOP, nr.left, nr.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (resized && !(GetWindowLongPtrW(ed, GWL_STYLE) & ES_MULTILINE)) {
            /* the EDIT was created 10 px wide and scrolled horizontally to keep the caret
               (end of the selected title) in view; re-apply the selection at the real width
               so the text starts at the left edge again */
            DWORD s0 = 0, s1 = 0;
            SendMessageW(ed, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
            SendMessageW(ed, EM_SETSEL, 0, 0);
            SendMessageW(ed, EM_SCROLLCARET, 0, 0);
            SendMessageW(ed, EM_SETSEL, (WPARAM)s0, (LPARAM)s1);
        }
        p->r = nr;
        p->shown = true;
        if (resized || resized_h) edit_vsb(ed);
    }
    if (c0 != p->clip0 || c1 != p->clip1) {
        if (c0 == 0 && c1 == h) SetWindowRgn(ed, NULL, TRUE);
        else SetWindowRgn(ed, CreateRectRgn(0, c0, w, c1), TRUE);
        p->clip0 = c0; p->clip1 = c1;
    }
}

static void edits_sync(void)
{
    /* a fresh EdPlace starts with clip 0..0, so the first placement always sets the region */
    edit_place(0, g.ed_title, &g.rc_title);
    edit_place(1, g.ed_details, &g.rc_details);
    edit_place(2, g.ed_notes, &g.rc_notes);
    edit_place(3, g.ed_days, &g.rc_days);
}

/* ================================================================== event form */

void ui_open_form(const AlmEvent *ev)
{
    ui_notes_flush();
    if (g.form_open) ui_close_form();
    g.form_open = true;
    g.form_invalid = false;
    if (ev) {
        lstrcpynA(g.form_id, ev->id ? ev->id : "", (int)sizeof g.form_id);
        g.form_type = ev->type;
        g.form_repeat = ev->repeat;
        g.form_days = clampi(ev->days, 1, ALM_EVENT_DAYS_MAX);
    } else {
        g.form_id[0] = 0;
        g.form_type = s_last_type;             /* last used type (first: Reminder) */
        g.form_repeat = IWD_REPEAT_NONE;
        g.form_days = 1;
    }
    g.form_days_invalid = false;
    g.ed_title = ui_make_edit(g.hwnd, ID_ED_TITLE, false, ALM_TITLE_MAX_UNITS, F_BODY, ev ? ev->title : "");
    g.ed_details = ui_make_edit(g.hwnd, ID_ED_DETAILS, true, ALM_DETAILS_MAX_UNITS, F_BODY, ev ? ev->details : "");
    {
        char db[8];
        snprintf(db, sizeof db, "%d", g.form_days);
        g.ed_days = ui_make_edit(g.hwnd, ID_ED_DAYS, false, 2, F_BODY, db);
        if (g.ed_days) SetWindowLongPtrW(g.ed_days, GWL_STYLE, GetWindowLongPtrW(g.ed_days, GWL_STYLE) | ES_NUMBER);
    }
    SetRectEmpty(&g.rc_title);
    SetRectEmpty(&g.rc_details);
    SetRectEmpty(&g.rc_days);
    if (g.ed_title) { SetFocus(g.ed_title); SendMessageW(g.ed_title, EM_SETSEL, 0, -1); }
    s_form_scroll = true;
    ui_relayout();
}

void ui_close_form(void)
{
    bool had_focus = any_edit_focused() && GetFocus() != g.ed_notes;
    if (!g.form_open && !g.ed_title && !g.ed_details && !g.ed_days) return;
    g.form_open = false;
    g.form_id[0] = 0;
    g.form_invalid = false;
    if (had_focus) SetFocus(g.hwnd);
    if (g.ed_title) { DestroyWindow(g.ed_title); g.ed_title = NULL; }
    if (g.ed_details) { DestroyWindow(g.ed_details); g.ed_details = NULL; }
    if (g.ed_days) { DestroyWindow(g.ed_days); g.ed_days = NULL; }
    g.form_days_invalid = false;
    SetRectEmpty(&g.rc_title);
    SetRectEmpty(&g.rc_details);
    SetRectEmpty(&g.rc_days);
    ui_relayout();
    if (g.saved_scroll >= 0) {               /* opened by a shortcut: back to where the DM was */
        view_layout();
        view_scroll_to(g.saved_scroll);
        g.saved_scroll = -1;
    }
}

static bool blank_w(const char *u)
{
    WCHAR *w = wz_dup(u ? u : ""), *p;
    bool blank = true;
    if (!w) return true;
    for (p = w; *p; p++)
        if (!(*p == L' ' || (*p >= 9 && *p <= 13) || *p == 0xA0 || *p == 0xFEFF || *p == 0x1680 ||
              (*p >= 0x2000 && *p <= 0x200A) || *p == 0x2028 || *p == 0x2029 || *p == 0x202F ||
              *p == 0x205F || *p == 0x3000)) { blank = false; break; }
    free(w);
    return blank;
}

void ui_form_submit(void)
{
    char *title, *details;
    bool upd = false;
    AlmResult r;
    int days;
    if (!g.form_open) return;
    days = form_days_read();
    if (days < 1 || days > ALM_EVENT_DAYS_MAX) {   /* "Lasts": 1..30 whole days */
        g.form_days_invalid = true;
        if (g.ed_days) { SetFocus(g.ed_days); SendMessageW(g.ed_days, EM_SETSEL, 0, -1); }
        ui_relayout();
        return;
    }
    g.form_days = days;
    title = ui_edit_text(g.ed_title);
    details = ui_edit_text(g.ed_details);
    if (!title || blank_w(title)) {
        g.form_invalid = true;
        if (g.ed_title) SetFocus(g.ed_title);
        free(title); free(details);
        ui_redraw();
        return;
    }
    r = alm_event_save_span(&g.a, g.form_id[0] ? g.form_id : NULL, g.a.sel_abs, title,
                            (AlmEvType)clampi(g.form_type, 0, ALM_EV_TYPE_COUNT - 1),
                            (IwdRepeat)clampi(g.form_repeat, 0, IWD_REPEAT_COUNT - 1),
                            details ? details : "", days, ui_now_ms(), &upd);
    free(title); free(details);
    if (r.fx & ALM_FX_REJECTED) {
        if (!(r.fx & ALM_FX_TOAST)) { g.form_invalid = true; if (g.ed_title) SetFocus(g.ed_title); }
        ui_apply(r);
        ui_redraw();
        return;
    }
    g.armed_id[0] = 0;
    if (!g.form_id[0]) s_last_type = clampi(g.form_type, 0, ALM_EV_TYPE_COUNT - 1);
    ui_close_form();
    ui_apply_durable(r);
}

/* ================================================================== notes */

static void notes_drop(void)
{
    if (!g.ed_notes) return;
    if (GetFocus() == g.ed_notes) SetFocus(g.hwnd);
    DestroyWindow(g.ed_notes);
    g.ed_notes = NULL;
    ui_redraw();
}

void ui_notes_flush(void)
{
    char *t;
    AlmResult r;
    KillTimer(g.hwnd, T_NOTE);
    if (!g.ed_notes || !g.note_pending) return;
    g.note_pending = false;
    t = ui_edit_text(g.ed_notes);
    r = alm_note_set(&g.a, g.notes_abs, t ? t : "");
    free(t);
    g.note_state = 2;
    g.rev++;
    ui_apply_durable(r);
    ui_redraw();
}

void ui_notes_begin(void)
{
    if (g.ed_notes && g.notes_abs != g.a.sel_abs) { ui_notes_flush(); notes_drop(); }
    if (!g.ed_notes) {
        g.notes_abs = g.a.sel_abs;
        g.note_pending = false;
        g.note_state = 0;
        g.ed_notes = ui_make_edit(g.hwnd, ID_ED_NOTES, true, ALM_MAX_NOTE_BYTES / 4, F_BODY,
                                  alm_note_get(&g.a, g.notes_abs));
        memset(&s_place[2], 0, sizeof s_place[2]);
        edit_place(2, g.ed_notes, &g.rc_notes);
    }
    if (g.ed_notes) {
        int n = GetWindowTextLengthW(g.ed_notes);
        SetFocus(g.ed_notes);
        SendMessageW(g.ed_notes, EM_SETSEL, (WPARAM)n, (LPARAM)n);
    }
    ui_redraw();
}

/* ================================================================== popup choice menu */

/* ui_popup_choice: x, y are SCREEN coordinates of the menu's top-left corner (ui_settings.c
 * passes screen coordinates); w is the minimum width. Returns the chosen index or -1. The
 * owner window must forward WM_MEASUREITEM / WM_DRAWITEM to ui_menu_measure / ui_menu_draw. */

static const char *const *s_pop_labels;
static int s_pop_n, s_pop_sel, s_pop_w;
static HBRUSH s_menu_br;

int ui_popup_choice(HWND owner, int x, int y, int w, const char *const *labels, int n, int sel)
{
    HMENU m; POINT p; int i, r; MENUINFO mi;
    if (!labels || n <= 0) return -1;
    m = CreatePopupMenu();
    if (!m) return -1;
    if (!s_menu_br) s_menu_br = CreateSolidBrush(RGB_OF(C_PANEL2));
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi; mi.fMask = MIM_BACKGROUND; mi.hbrBack = s_menu_br;
    SetMenuInfo(m, &mi);
    s_pop_labels = labels; s_pop_n = n; s_pop_sel = sel; s_pop_w = w;
    for (i = 0; i < n; i++) AppendMenuW(m, MF_OWNERDRAW, (UINT_PTR)(i + 1), (LPCWSTR)(INT_PTR)(i + 1));
    p.x = x; p.y = y;                         /* screen coordinates */
    r = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY | TPM_LEFTBUTTON,
                              p.x, p.y, owner, NULL);
    DestroyMenu(m);
    s_pop_labels = NULL; s_pop_n = 0;
    return r >= 1 && r <= n ? r - 1 : -1;
}

LRESULT ui_menu_measure(MEASUREITEMSTRUCT *mi)
{
    int i, wmax = 0, chk = GetSystemMetrics(SM_CXMENUCHECK);
    if (!mi || mi->CtlType != ODT_MENU || !s_pop_labels) return FALSE;
    for (i = 0; i < s_pop_n; i++) {
        int t = tw(F_BODY, wz(s_pop_labels[i]), -1);
        if (t > wmax) wmax = t;
    }
    wmax += dp(28);
    if (s_pop_w - chk > wmax) wmax = s_pop_w - chk;
    mi->itemWidth = (UINT)(wmax > 0 ? wmax : dp(80));
    mi->itemHeight = (UINT)(fh(F_BODY) + dp(10));
    return TRUE;
}

LRESULT ui_menu_draw(DRAWITEMSTRUCT *di)
{
    int i; bool selnow, cur; RECT r; HDC dc; HGDIOBJ of;
    if (!di || di->CtlType != ODT_MENU || !s_pop_labels) return FALSE;
    i = (int)di->itemData - 1;
    if (i < 0 || i >= s_pop_n) return FALSE;
    dc = di->hDC; r = di->rcItem;
    selnow = (di->itemState & ODS_SELECTED) != 0;
    cur = i == s_pop_sel;
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, RGB_OF(selnow ? mix(C_PANEL2, C_ACCENT, 60) : C_PANEL2));
    PatBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, PATCOPY);
    if (cur) {
        SetDCBrushColor(dc, RGB_OF(C_ACCENT));
        PatBlt(dc, r.left, r.top + dp(4), dp(2) < 2 ? 2 : dp(2), r.bottom - r.top - dp(8), PATCOPY);
    }
    of = SelectObject(dc, font(cur ? F_BODY_B : F_BODY));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB_OF(selnow || cur ? C_BRIGHT : C_TEXT));
    r.left += dp(12); r.right -= dp(8);
    DrawTextW(dc, wz(s_pop_labels[i]), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    return TRUE;
}

/* ================================================================== settings */

void ui_open_settings(void)
{
    ui_notes_flush();
    if (g.ed_notes) notes_drop();
    toast_hide();
    settings_open();
}

static void state_sync(void);

void ui_after_settings(bool changed)
{
    g.rev++;
    if (changed) {
        undo_del_forget();                           /* import / reset: another almanac */
        if (g.form_open && g.form_id[0] && !alm_event_find(&g.a, g.form_id)) ui_close_form();
        if (g.armed_id[0] && !alm_event_find(&g.a, g.armed_id)) g.armed_id[0] = 0;
        if (g.ed_notes) notes_drop();
        view_invalidate_timeline();
        s_last_sel = INT32_MIN;
        state_sync();
        ui_save_now();
    }
    wind_sync(true);
    ui_relayout();
    if (g.hwnd && IsWindowEnabled(g.hwnd)) SetFocus(g.hwnd);
}

/* ================================================================== action results */

/* after any state change: day-bound UI (notes EDIT, new-event form, armed delete) and the
 * wind */
static void state_sync(void)
{
    if (g.a.sel_abs != s_last_sel) {
        s_last_sel = g.a.sel_abs;
        if (g.ed_notes && g.notes_abs != g.a.sel_abs) {
            if (g.note_pending) ui_notes_flush();
            notes_drop();
        }
        if (g.notes_abs != g.a.sel_abs) g.note_state = 0;
        if (g.form_open && !g.form_id[0]) ui_close_form();
        if (g.armed_id[0]) { g.armed_id[0] = 0; KillTimer(g.hwnd, T_ARM); }
    }
    wind_sync(false);
}

void ui_apply(AlmResult r)
{
    unsigned fx = r.fx;
    if (fx & ALM_FX_DAY_TOAST) day_toast(r.toast_abs, r.toast_lead);
    if (fx & ALM_FX_TOAST) ui_toast(r.toast);
    if (fx & ALM_FX_RENDER_DAY) view_invalidate_timeline();
    if (fx & (ALM_FX_RENDER_DAY | ALM_FX_RENDER_CAL | ALM_FX_TOAST | ALM_FX_DAY_TOAST)) g.rev++;
    state_sync();
    if (fx & ALM_FX_SAVE) { if (s_durable > 0) ui_save_now(); else save_soon(); }
    if (fx & ALM_FX_RENDER_ALL) ui_relayout();
    else if (fx & (ALM_FX_TOAST | ALM_FX_DAY_TOAST)) ui_redraw();
}


/* ================================================================== actions */

/* Every user action goes through ACT(call): the state before the call decides the
 * party clock, the undo slot and the "today moved" toast afterwards. */
typedef struct Pre {
    bool    was_today;      /* the party's day was on screen */
    double  t0;             /* clock before the action */
    int32_t cur0, cur_abs0;
    double  party0;         /* the party's clock (today's time, wherever the view is) */
    bool    keep_clock;     /* time stepping: never jump the clock back to the party's */
} Pre;

static double party_clock(void)
{
    if (g.a.view == g.a.current || g.party_time < 0) return g.a.time;
    return g.party_time;
}

static void pre_capture(Pre *p, bool keep_clock)
{
    p->was_today = g.a.current >= 0 && g.a.view == g.a.current;
    p->t0 = g.a.time;
    p->cur0 = g.a.current;
    p->cur_abs0 = g.a.current_abs;
    p->party0 = party_clock();
    p->keep_clock = keep_clock;
}

/* the toast that confirms a move of "today" and offers the undo */
static void today_moved_toast(const Pre *p, AlmResult *r)
{
    char d[IWD_DATE_CAP], t[ALM_TOAST_CAP];
    iwd_format_date(g.a.current_abs, false, d, sizeof d);
    if (p->cur0 < 0) {                                   /* the very first dawn: no undo */
        if (!(r->fx & (ALM_FX_DAY_TOAST | ALM_FX_TOAST))) {
            snprintf(t, sizeof t, "The expedition begins on <b>%s</b>", d);
            lstrcpynA(r->toast, t, ALM_TOAST_CAP);
            r->fx |= ALM_FX_TOAST;
        }
        return;
    }
    if (r->fx & ALM_FX_DAY_TOAST) return;               /* hint added after ui_apply */
    if (r->fx & ALM_FX_TOAST) {
        size_t n = strlen(r->toast);
        if (n + 32 < ALM_TOAST_CAP) lstrcatA(r->toast, " \xC2\xB7 Ctrl+Z steps back");
        return;
    }
    snprintf(t, sizeof t, "Advanced to <b>%s</b> \xC2\xB7 Ctrl+Z steps back", d);
    lstrcpynA(r->toast, t, ALM_TOAST_CAP);
    r->fx |= ALM_FX_TOAST;
}

static void post_apply(const Pre *p, AlmResult r)
{
    bool moved;
    if (r.fx & ALM_FX_REJECTED) { ui_apply(r); return; }
    moved = g.a.current != p->cur0;
    if (moved) {
        g.party_time = -1;
        if (p->cur0 >= 0) {
            s_undo.ok = true; s_undo.cur_abs = p->cur_abs0; s_undo.time = p->party0;
            undo_del_forget();                       /* the day step is now the latest */
        }
        today_moved_toast(p, &r);
        ui_apply(r);
        if ((r.fx & ALM_FX_DAY_TOAST) && p->cur0 >= 0 && g.toast_on && g.toast_day)
            lstrcpynA(g.toast_hint, "Ctrl+Z steps back to the previous day", (int)sizeof g.toast_hint);
        return;
    }
    if (p->was_today && g.a.view != g.a.current) {
        g.party_time = p->t0;                              /* leaving today: keep its clock */
    } else if (!p->was_today && g.a.current >= 0 && g.a.view == g.a.current) {
        if (g.party_time >= 0 && !p->keep_clock) {
            AlmResult t = alm_set_time(&g.a, g.party_time);
            if (!(t.fx & ALM_FX_REJECTED)) r.fx |= t.fx;
        }
        g.party_time = -1;
    }
    ui_apply(r);
}

#define ACT(call)  do { Pre p_; pre_capture(&p_, false); post_apply(&p_, (call)); } while (0)
#define ACTH(call) do { Pre p_; pre_capture(&p_, true);  post_apply(&p_, (call)); } while (0)

/* the live weather is at the top of the page: time-changing actions go back there */
static void to_weather(void)
{
    if (g.scroll > 0) view_scroll_to(0);
}

static void stop_fade(void)
{
    if (g.fade_k) { g.fade_k = 0; KillTimer(g.hwnd, T_FADE); ui_redraw(); }
}

void ui_fade_start(void)
{
    if (g.hwnd && g.fade_k > 0) SetTimer(g.hwnd, T_FADE, 30, NULL);
}

static void do_advance(void)
{
    ui_notes_flush();
    g.fade_req = true;
    ACT(alm_advance(&g.a, IWD_SETTLE_HOUR));
}

/* time step of delta minutes (buttons and arrow keys). A held key (auto-repeat) steps the
 * clock but never crosses midnight: holding a key cannot run through days. */
static void do_step(int delta, bool repeat)
{
    if (repeat && alm_has_view(&g.a)) {
        int n = alm_minute_now(&g.a) + delta;
        if (n < 0 || n >= 1440) return;
    }
    ui_notes_flush();
    g.fade_req = true;
    ACTH(alm_step_minutes(&g.a, delta));
}

static void do_step_day(int dir)
{
    ui_notes_flush();
    g.fade_req = true;
    ACT(alm_step_day(&g.a, dir));
}

static void do_today(void)
{
    ui_notes_flush();
    g.fade_req = true;
    ACT(alm_cal_today(&g.a));
}

static void do_set_current(int32_t abs)
{
    ui_notes_flush();
    g.fade_req = true;
    DURABLE(ACT(alm_set_current(&g.a, abs)));
}

static void do_undo(void)
{
    AlmResult r, t;
    char d[IWD_DATE_CAP], msg[ALM_TOAST_CAP];
    int m;
    if (s_undo_del.ok) {                                 /* the latest action was a deletion */
        r = alm_event_restore(&g.a, &s_undo_del.ev, s_undo_del.idx);
        undo_del_forget();
        if (r.fx & ALM_FX_REJECTED) { ui_toast("The deleted event cannot be restored"); return; }
        ui_apply_durable(r);
        return;
    }
    if (!s_undo.ok) { ui_toast("Nothing to step back to"); return; }
    s_undo.ok = false;
    ui_notes_flush();
    r = alm_set_current(&g.a, s_undo.cur_abs);
    if (r.fx & ALM_FX_REJECTED) { ui_apply(r); return; }
    r.fx &= ~(unsigned)(ALM_FX_TOAST | ALM_FX_DAY_TOAST);
    t = alm_set_time(&g.a, s_undo.time);
    if (!(t.fx & ALM_FX_REJECTED)) r.fx |= t.fx;
    g.party_time = -1;
    g.fade_req = true;
    iwd_format_date(g.a.current_abs, true, d, sizeof d);
    m = alm_minute_now(&g.a);
    snprintf(msg, sizeof msg, "Back to <b>%s</b>, %02d:%02d", d, m / 60, m % 60);
    lstrcpynA(r.toast, msg, ALM_TOAST_CAP);
    r.fx |= ALM_FX_TOAST | ALM_FX_RENDER_ALL | ALM_FX_SAVE;
    ui_apply_durable(r);
    to_weather();
}

static int32_t cell_abs(int arg, bool *ok)
{
    AlmCell c[32]; int n = view_cells(c);
    *ok = true;
    if (arg >= 0 && arg < n) return c[arg].abs;
    *ok = false;
    return 0;
}

static void ev_action(int id, int arg)
{
    AlmMark mk[64]; int n = alm_marks_on(&g.a, g.a.sel_abs, mk, 64);
    const AlmEvent *ev;
    if (n > 64) n = 64;
    if (arg < 0 || arg >= n || mk[arg].builtin || !mk[arg].event) return;
    ev = mk[arg].event;
    if (id == H_EV_EDIT) { ui_open_form(ev); return; }
    if (g.armed_id[0] && ev->id && strcmp(g.armed_id, ev->id) == 0) {
        char idc[64];
        lstrcpynA(idc, ev->id, (int)sizeof idc);
        g.armed_id[0] = 0;
        KillTimer(g.hwnd, T_ARM);
        AlmEvent copy;
        int idx = alm_event_index(&g.a, idc);
        bool have = alm_event_dup(&copy, ev);
        if (g.form_open && strcmp(g.form_id, idc) == 0) ui_close_form();
        DURABLE(ACT(alm_event_delete(&g.a, idc)));
        if (have && !alm_event_find(&g.a, idc)) {
            undo_del_forget();
            s_undo_del.ev = copy;
            s_undo_del.idx = idx;
            s_undo_del.ok = true;
            if (g.toast_on && !g.toast_day)
                lstrcpynA(g.toast_hint, "Ctrl+Z restores it", (int)sizeof g.toast_hint);
        } else if (have) {
            alm_event_release(&copy);
        }
    } else {
        lstrcpynA(g.armed_id, ev->id ? ev->id : "", (int)sizeof g.armed_id);
        SetTimer(g.hwnd, T_ARM, ALM_DELETE_ARM_MS, NULL);
        ui_redraw();
    }
}

static void form_repeat_popup(const RECT *hr)
{
    const char *lab[IWD_REPEAT_COUNT]; int i, k; POINT p;
    for (i = 0; i < IWD_REPEAT_COUNT; i++) lab[i] = iwd_repeat_label((IwdRepeat)i);
    p.x = hr->left; p.y = hr->bottom + dp(2);
    ClientToScreen(g.hwnd, &p);
    k = ui_popup_choice(g.hwnd, p.x, p.y, hr->right - hr->left, lab, IWD_REPEAT_COUNT, g.form_repeat);
    if (k >= 0) { g.form_repeat = k; ui_redraw(); }
}

/* shortcut entry points (E, O): remember where the DM was */
static void open_form_shortcut(void)
{
    if (g.saved_scroll < 0) g.saved_scroll = g.scroll;
    ui_open_form(NULL);
}

static void notes_shortcut(void)
{
    if (!alm_has_view(&g.a) && g.a.sel_abs == INT32_MIN) return;
    if (g.saved_scroll < 0) g.saved_scroll = g.scroll;
    ui_notes_begin();
    {
        RECT r = view_block_rect(BLK_DAY);
        if (!IsRectEmpty(&g.rc_notes)) view_ensure_visible(g.rc_notes.top - dp(40), g.rc_notes.bottom + dp(12));
        else if (r.bottom > r.top) view_ensure_visible(r.bottom - dp(200), r.bottom);
    }
    ui_redraw();
}

static void dispatch(int id, int arg, const RECT *hr)
{
    bool ok;
    stop_fade();
    switch (id) {
    case H_UNIT_F:        DURABLE(ACT(alm_set_unit(&g.a, false))); break;
    case H_UNIT_C:        DURABLE(ACT(alm_set_unit(&g.a, true))); break;
    case H_SOUND:         ui_sound_toggle(); break;
    case H_INTERIOR:      ui_interior_toggle(); break;
    case H_SETTINGS:
    case H_INTRO_SETTINGS: ui_open_settings(); break;
    case H_ADVANCE:       do_advance(); to_weather(); break;
    case H_STEP:          if (arg == -60 || arg == -30 || arg == -5 || arg == 5 || arg == 30 || arg == 60) do_step(arg, false); break;
    case H_DAY_PREV:      do_step_day(-1); break;
    case H_DAY_NEXT:      do_step_day(+1); break;
    case H_LEDGER: {
        g.a.native.ledger = !g.a.native.ledger;
        save_soon();
        ui_relayout();
        if (g.a.native.ledger) {             /* bring it into view, the chip staying on screen */
            RECT f, l;
            view_layout();
            f = view_block_rect(BLK_FOOT); l = view_block_rect(BLK_LEDGER);
            view_ensure_visible(f.top, l.bottom);
        }
        break;
    }
    case H_CAL_PREV:      ui_apply(alm_cal_step(&g.a, -1)); break;
    case H_CAL_NEXT:      ui_apply(alm_cal_step(&g.a, +1)); break;
    case H_CAL_TODAY:
    case H_BACK_TODAY:    do_today(); to_weather(); break;
    case H_HEAD_NOW:      to_weather(); break;
    case H_KEYS:          g.keys_on = !g.keys_on; ui_redraw(); break;
    case H_KEYS_CARD:     g.keys_on = false; ui_redraw(); break;
    case H_SAVE_FAIL: {
        WCHAR m[600];
        ui_export_backup(g.hwnd, m, 600);
        if (m[0]) { char *u = u8_dup(m); if (u) { ui_toast(u); free(u); } }
        break;
    }
    case H_CAL_CELL: {
        int32_t abs = cell_abs(arg, &ok);
        if (!ok) break;
        ui_notes_flush();
        ACT(alm_select(&g.a, abs, true));
        break;
    }
    case H_UPCOMING: {
        ui_notes_flush();
        ACT(alm_select(&g.a, (int32_t)arg, false));
        break;
    }
    case H_SET_CURRENT:   do_set_current(g.a.sel_abs); break;
    case H_EV_EDIT:
    case H_EV_DEL:        ev_action(id, arg); break;
    case H_ADD_EVENT:     ui_open_form(NULL); break;
    case H_FORM_TYPE:
        if (arg >= 0 && arg < ALM_EV_TYPE_COUNT) { g.form_type = arg; ui_redraw(); }
        break;
    case H_FORM_REPEAT:   if (hr) form_repeat_popup(hr); break;
    case H_FORM_DAYS:     form_days_step(arg < 0 ? -1 : 1); break;
    case H_FORM_CANCEL:   ui_close_form(); break;
    case H_FORM_SAVE:     ui_form_submit(); break;
    case H_NOTES:         ui_notes_begin(); break;
    case H_TOAST:         toast_hide(); break;
    default:              break;   /* H_NONE, H_TIP, H_TIMELINE (press), H_SCROLL (press), H_S_* */
    }
}

/* ================================================================== tooltips */

static void tip_init(void)
{
    TOOLINFOW ti;
    INITCOMMONCONTROLSEX ic;
    ic.dwSize = sizeof ic; ic.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&ic);
    g.tip_hwnd = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.hwnd, NULL, g.inst, NULL);
    if (!g.tip_hwnd) return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g.hwnd;
    ti.uId = 1;
    ti.lpszText = (LPWSTR)L"";
    SetRectEmpty(&ti.rect);
    SendMessageW(g.tip_hwnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g.tip_hwnd, TTM_SETMAXTIPWIDTH, 0, dp(380));
    /* dark tips: theming off (so the colours apply on every Windows build), page colours */
    dark_theme(g.tip_hwnd, L"");
    {
        static SetWindowThemeFn fn2; static bool tried2;
        if (!tried2) { tried2 = true; if (s_uxtheme) fn2 = (SetWindowThemeFn)(void (*)(void))GetProcAddress(s_uxtheme, "SetWindowTheme"); }
        if (fn2) fn2(g.tip_hwnd, L"", L"");
    }
    SendMessageW(g.tip_hwnd, TTM_SETTIPBKCOLOR, (WPARAM)RGB_OF(C_PANEL2), 0);
    SendMessageW(g.tip_hwnd, TTM_SETTIPTEXTCOLOR, (WPARAM)RGB_OF(C_TEXT), 0);
    {
        RECT m; SetRect(&m, dp(4), dp(3), dp(4), dp(3));
        SendMessageW(g.tip_hwnd, TTM_SETMARGIN, 0, (LPARAM)&m);
    }
    SendMessageW(g.tip_hwnd, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(g.tip_hwnd, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);
    s_tip_tool_owner = g.hwnd;
}

static void tip_set(const Hot *h)
{
    TOOLINFOW ti;
    const WCHAR *t = h && h->tip[0] ? h->tip : L"";
    RECT r;
    if (!g.tip_hwnd) return;
    if (h && h->tip[0]) r = h->r; else SetRectEmpty(&r);
    if (lstrcmpW(t, g.tip) == 0 && EqualRect(&r, &s_tip_rect)) return;
    lstrcpynW(g.tip, t, TIP_CAP);
    s_tip_rect = r;
    SendMessageW(g.tip_hwnd, TTM_POP, 0, 0);
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.hwnd = s_tip_tool_owner;
    ti.uId = 1;
    ti.rect = r;
    SendMessageW(g.tip_hwnd, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    ti.lpszText = g.tip;
    ti.hinst = NULL;
    SendMessageW(g.tip_hwnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

/* ================================================================== hover */

static void hover_at(int x, int y, bool inside)
{
    const Hot *h = inside ? hot_find(&g.hl, x, y) : NULL;
    int id = h ? h->id : H_NONE, arg = h ? h->arg : 0;
    tip_set(h);
    if (id != g.hot_id || arg != g.hot_arg) {
        g.hot_id = id; g.hot_arg = arg;
        ui_redraw();
    }
}

static void hover_refresh(void)
{
    POINT p; RECT cr;
    if (!g.hwnd || !GetCursorPos(&p)) return;
    ScreenToClient(g.hwnd, &p);
    GetClientRect(g.hwnd, &cr);
    hover_at(p.x, p.y, PtInRect(&cr, p) && GetForegroundWindow() == g.hwnd);
}

static bool clickable(const Hot *h)
{
    return h && h->enabled && h->id != H_NONE && h->id != H_TIP;
}

/* ================================================================== keyboard */

static int line_px(void) { return dp(48); }

static void on_key(WPARAM vk, LPARAM lp)
{
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    bool repeat = (lp & 0x40000000) != 0;
    stop_fade();
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return;
    if (vk == VK_F1) { if (!repeat) { g.keys_on = !g.keys_on; ui_redraw(); } return; }
    if (g.keys_on) {                                    /* the keys card: any key closes it */
        g.keys_on = false;
        ui_redraw();
        if (vk == VK_ESCAPE) return;
    }
    if (vk == VK_F2) { if (!repeat) ui_open_settings(); return; }
    if (ctrl && vk == 'Z') { if (!repeat) do_undo(); return; }
    if (ctrl && vk == VK_HOME) { view_scroll_to(0); return; }
    if (ctrl && vk == VK_END) { view_scroll_to(g.content_h); return; }
    /* the more modifier, the finer the step: none 1 h, Shift 30 min, Ctrl 5 min */
    if (vk == VK_RIGHT || vk == VK_LEFT) {
        int d = ctrl ? 5 : shift ? 30 : 60;
        do_step(vk == VK_RIGHT ? d : -d, repeat);
        to_weather();
        return;
    }
    if (ctrl) return;
    /* one press = one day: auto-repeat only steps the clock and scrolls */
    if (repeat && vk != VK_SPACE && vk != VK_UP && vk != VK_DOWN) return;
    switch (vk) {
    case VK_SPACE: do_step(60, repeat); to_weather(); break;
    case 'N':      do_advance(); to_weather(); break;
    case VK_PRIOR: do_step_day(-1); to_weather(); break;
    case VK_NEXT:  do_step_day(+1); to_weather(); break;
    case VK_UP:    view_scroll_to(g.scroll - line_px()); break;
    case VK_DOWN:  view_scroll_to(g.scroll + line_px()); break;
    case 'T':      do_today(); to_weather(); break;
    case 'M':      ui_sound_toggle(); break;
    case 'I':      ui_interior_toggle(); break;
    case 'E':      if (g.a.sel_abs != INT32_MIN) open_form_shortcut(); break;
    case 'O':      notes_shortcut(); break;
    case VK_ESCAPE:
        if (g.form_open) ui_close_form();
        else if (g.toast_on) toast_hide();
        else if (g.a.current >= 0 && g.a.view != g.a.current) { do_today(); to_weather(); }
        break;
    default: break;
    }
}

/* ================================================================== window helpers */

static UINT query_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn fn = (GetDpiForWindowFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = fn ? fn(hwnd) : 0;
    if (!d) { HDC dc = GetDC(hwnd); d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(hwnd, dc); }
    return d ? d : 96;
}

static void dark_titlebar(HWND hwnd)
{
    typedef HRESULT (WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (m) {
        DwmSetFn fn = (DwmSetFn)(void (*)(void))GetProcAddress(m, "DwmSetWindowAttribute");
        BOOL on = TRUE;
        if (fn && fn(hwnd, 20, &on, sizeof(on)) != S_OK) fn(hwnd, 19, &on, sizeof(on));
        FreeLibrary(m);
    }
}

static void set_client_size(HWND hwnd, int cw, int ch)
{
    RECT wr, cr, wa;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
    cw += (wr.right - wr.left) - cr.right; ch += (wr.bottom - wr.top) - cr.bottom;
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) SetRect(&wa, 0, 0, 1 << 15, 1 << 15);
    if (cw > wa.right - wa.left) cw = wa.right - wa.left;
    if (ch > wa.bottom - wa.top) ch = wa.bottom - wa.top;
    wr.left = wa.left + ((wa.right - wa.left) - cw) / 2;
    wr.top = wa.top + ((wa.bottom - wa.top) - ch) / 2;
    SetWindowPos(hwnd, NULL, wr.left, wr.top, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* exact client size, no clamping (test hook --size) */
static void force_client_size(HWND hwnd, int cw, int ch)
{
    RECT wr, cr;
    GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
    cw += (wr.right - wr.left) - cr.right; ch += (wr.bottom - wr.top) - cr.bottom;
    SetWindowPos(hwnd, NULL, 0, 0, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void store_placement(void)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    if (!g.hwnd || !GetWindowPlacement(g.hwnd, &wp)) return;
    g.a.native.has_rect = true;
    g.a.native.win_x = wp.rcNormalPosition.left;
    g.a.native.win_y = wp.rcNormalPosition.top;
    g.a.native.win_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    g.a.native.win_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    g.a.native.maximized = wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(g.hwnd);
}

static bool restore_placement(int show)
{
    WINDOWPLACEMENT wp; RECT r;
    const AlmNative *n = &g.a.native;
    if (!n->has_rect || n->win_w < 200 || n->win_h < 150) return false;
    SetRect(&r, n->win_x, n->win_y, n->win_x + n->win_w, n->win_y + n->win_h);
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;
    memset(&wp, 0, sizeof wp);
    wp.length = sizeof wp;
    wp.rcNormalPosition = r;
    wp.showCmd = n->maximized ? SW_SHOWMAXIMIZED : (show == SW_SHOWMINIMIZED || show == SW_MINIMIZE ? SW_SHOWNORMAL : show);
    if (wp.showCmd == SW_HIDE || wp.showCmd == 0) wp.showCmd = SW_SHOWNORMAL;
    return SetWindowPlacement(g.hwnd, &wp) != 0;
}

static void on_resize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    surf_resize(&g.back, w, h);
    ui_relayout();
}

static void restyle_all(void)
{
    if (g.ed_title) ui_style_edit(g.ed_title, F_BODY);
    if (g.ed_details) ui_style_edit(g.ed_details, F_BODY);
    if (g.ed_days) ui_style_edit(g.ed_days, F_BODY);
    if (g.ed_notes) ui_style_edit(g.ed_notes, F_BODY);
    if (g.tip_hwnd) SendMessageW(g.tip_hwnd, TTM_SETMAXTIPWIDTH, 0, dp(380));
}

static void close_and_save(void)
{
    ui_notes_flush();
    store_placement();
    ui_save_now();
}

/* ================================================================== test hooks */

static void run_open_hook(void)
{
    SetForegroundWindow(g.hwnd);
    SetFocus(g.hwnd);
    if (s_opt_open == 1) ui_open_settings();
    else if (s_opt_open == 2) ui_open_form(NULL);
    s_opt_open = 0;
}

/* ================================================================== scrubbing */

/* timeline drag: the clock follows the pointer in 5-minute steps (00:00 .. 23:55) */
static void scrub_to(int x, bool first)
{
    int m = (int)floor(view_timeline_time(x) * 12 + 0.5) * 5;
    double t;
    if (m > 1435) m = 1435;
    if (m < 0) m = 0;
    t = m / 60.0;
    if (first || fabs(t - g.a.time) > 1e-9) ui_apply(alm_set_time(&g.a, t));
}

/* ================================================================== window proc */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.dpi = (int)query_dpi(hwnd);
        fonts_make();
        if (!g.br_field) g.br_field = CreateSolidBrush(RGB_OF(C_FIELD));
        if (!g.br_field_dis) g.br_field_dis = CreateSolidBrush(RGB_OF(C_PANEL));
        dark_titlebar(hwnd);
        tip_init();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        if (cr.right > 0 && cr.bottom > 0 && (g.back.w != cr.right || g.back.h != cr.bottom)) {
            surf_resize(&g.back, cr.right, cr.bottom);
            s_dirty = true;
        }
        if (g.back.px && s_dirty) { s_dirty = false; view_paint(); }
        if (g.back.dc)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, g.back.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        edits_sync();
        if (s_form_scroll && !IsRectEmpty(&g.rc_title)) {
            s_form_scroll = false;
            view_ensure_visible(g.rc_title.top - dp(60), IsRectEmpty(&g.rc_details) ? g.rc_title.bottom : g.rc_details.bottom + dp(60));
        }
        if (!g.press_id && !g.scrubbing && !g.sb_drag) hover_refresh();
        if (s_opt_shot_ready) {               /* test hook: the first paint is on screen */
            s_opt_shot_ready = false;
            SetWindowTextW(hwnd, L"Icewind Dale Almanac [ready]");
        }
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) on_resize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp; RECT r;
        int d = g.dpi ? g.dpi : 96;
        SetRect(&r, 0, 0, MulDiv(640, d, 96), MulDiv(560, d, 96));   /* half of a 1366 px laptop screen */
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        mm->ptMinTrackSize.x = r.right - r.left; mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case 0x02E0: {  /* WM_DPICHANGED */
        const RECT *r = (const RECT *)lp;
        g.dpi = HIWORD(wp) ? HIWORD(wp) : 96;
        fonts_make();
        view_free();
        scene_free();
        thumb_free();
        restyle_all();
        settings_fonts_changed();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui_relayout();
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!g.tracking) {
            TRACKMOUSEEVENT t; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = hwnd; t.dwHoverTime = 0;
            g.tracking = TrackMouseEvent(&t) != 0;
        }
        if (g.sb_drag) { view_scroll_to(view_scroll_for_thumb(y)); return 0; }
        if (g.scrubbing) { scrub_to(x, false); return 0; }
        hover_at(x, y, true);
        return 0;
    }

    case WM_MOUSELEAVE:
        g.tracking = false;
        if (!g.scrubbing && !g.press_id && !g.sb_drag) hover_at(0, 0, false);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const Hot *h = hot_find(&g.hl, x, y);
        Hot hc;
        if (!h) { if (GetFocus() != hwnd) SetFocus(hwnd); return 0; }
        hc = *h;
        if (hc.id != H_NOTES && GetFocus() != hwnd && hc.id != H_FORM_TYPE && hc.id != H_FORM_REPEAT) SetFocus(hwnd);
        if (!hc.enabled) return 0;
        if (msg == WM_LBUTTONDBLCLK && hc.id == H_CAL_CELL) {
            bool ok; int32_t abs = cell_abs(hc.arg, &ok);
            if (ok) do_set_current(abs);
            return 0;
        }
        if (hc.id == H_SCROLL) {
            if (hc.arg == 0) {
                g.sb_drag = true; g.sb_grab = y - hc.r.top;
                SetCapture(hwnd);
                ui_redraw();
            } else {
                int vh = g.back.h - view_header_h();
                view_scroll_to(g.scroll + hc.arg * (vh - dp(40)));
            }
            return 0;
        }
        if (hc.id == H_TIMELINE) {
            if (!alm_has_view(&g.a) || g.a.busy) return 0;
            g.scrubbing = true;
            SetCapture(hwnd);
            scrub_to(x, true);
            ui_redraw();
            return 0;
        }
        if (hc.id == H_NONE || hc.id == H_TIP) return 0;
        g.press_id = hc.id; g.press_arg = hc.arg;
        g.hot_id = hc.id; g.hot_arg = hc.arg;
        SetCapture(hwnd);
        ui_redraw();
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.scrubbing) {
            g.scrubbing = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            ui_redraw();
            return 0;
        }
        if (g.sb_drag) {
            g.sb_drag = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            ui_redraw();
            return 0;
        }
        if (g.press_id) {
            int id = g.press_id, arg = g.press_arg;
            const Hot *h = hot_find(&g.hl, x, y);
            Hot hc; bool fire = h && h->id == id && h->arg == arg && h->enabled;
            if (fire) hc = *h;
            g.press_id = 0; g.press_arg = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
            ui_redraw();
            if (fire) dispatch(id, arg, &hc.r);
            hover_refresh();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd) {
            if (g.press_id || g.scrubbing || g.sb_drag) ui_redraw();
            g.press_id = 0; g.scrubbing = false; g.sb_drag = false;
        }
        return 0;

    case WM_MOUSEWHEEL:
        view_scroll_to(g.scroll - GET_WHEEL_DELTA_WPARAM(wp) * dp(60) / WHEEL_DELTA);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) {
            POINT p; const Hot *h;
            GetCursorPos(&p); ScreenToClient(hwnd, &p);
            h = hot_find(&g.hl, p.x, p.y);
            SetCursor(LoadCursorW(NULL, clickable(h) || g.scrubbing ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (!any_edit_focused() && IsWindowEnabled(hwnd)) on_key(wp, lp);
        return 0;

    case WM_SYSKEYDOWN:
        if (wp == VK_F10) return 0;
        break;

    case WM_TIMER:
        switch (wp) {
        case T_TOAST: KillTimer(hwnd, T_TOAST); if (!g.toast_sticky) toast_hide(); break;
        case T_NOTE:  KillTimer(hwnd, T_NOTE); ui_notes_flush(); break;
        case T_ARM:   KillTimer(hwnd, T_ARM); g.armed_id[0] = 0; ui_redraw(); break;
        case T_SAVE:  KillTimer(hwnd, T_SAVE); if (s_save_pending) save_write(false); break;
        case T_FADE:
            if (g.fade_k > 0) g.fade_k--;
            if (g.fade_k <= 0) { g.fade_k = 0; KillTimer(hwnd, T_FADE); }
            ui_redraw();
            break;
        case T_THUNDER:
            KillTimer(hwnd, T_THUNDER);
            s_thunder_timer = false;
            if (audio_is_enabled() && viewed_event() == 10) audio_thunder(0.5 + (rnd32() % 1000) / 2000.0);
            thunder_sync();
            break;
        default: KillTimer(hwnd, wp); break;
        }
        return 0;

    case WM_COMMAND: {
        HWND c = (HWND)lp; int code = HIWORD(wp);
        if (c && c == g.ed_notes) {
            if (code == EN_CHANGE) {
                g.note_pending = true;
                if (g.note_state != 1) { g.note_state = 1; ui_redraw(); }
                SetTimer(hwnd, T_NOTE, ALM_NOTE_DEBOUNCE_MS, NULL);
            } else if (code == EN_KILLFOCUS) {
                ui_notes_flush();
                PostMessageW(hwnd, WM_APP_DROP_NOTES, 0, 0);
            } else if (code == EN_SETFOCUS) {
                ui_redraw();
            }
        } else if (c && c == g.ed_title) {
            if (code == EN_CHANGE && g.form_invalid) { g.form_invalid = false; ui_redraw(); }
            else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) ui_redraw();
        } else if (c && c == g.ed_days) {
            if (code == EN_CHANGE) {          /* "day" / "days" follows the typed number */
                int v = form_days_read();
                bool was = g.form_days_invalid;
                if (v >= 1 && v <= ALM_EVENT_DAYS_MAX) g.form_days = v;
                g.form_days_invalid = false;
                if (was) ui_relayout(); else ui_redraw();
            } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) ui_redraw();
        } else if (c && c == g.ed_details) {
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) ui_redraw();
            else if (code == EN_CHANGE) edit_vsb(c);
        }
        if (c && c == g.ed_notes && code == EN_CHANGE) edit_vsb(c);
        return 0;
    }

    case WM_APP_DROP_NOTES:
        if (g.ed_notes && GetFocus() != g.ed_notes) notes_drop();
        return 0;

    case WM_APP_HOOKS:
        run_open_hook();
        return 0;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, RGB_OF(C_TEXT));
        SetBkColor((HDC)wp, RGB_OF(C_FIELD));
        return (LRESULT)g.br_field;

    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, RGB_OF(C_MUTED));
        SetBkColor((HDC)wp, RGB_OF(C_PANEL));
        return (LRESULT)g.br_field_dis;

    case WM_MEASUREITEM:
        if (ui_menu_measure((MEASUREITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_DRAWITEM:
        if (ui_menu_draw((DRAWITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) hover_at(0, 0, false);
        break;

    case WM_CLOSE:
        close_and_save();
        if (s_save_failed && s_persist) {
            WCHAR p[MAX_PATH * 2], m[MAX_PATH * 2 + 400];
            int k;
            path_w(g.state_path, p, MAX_PATH * 2);
            wsprintfW(m, L"Could not save the almanac to\n%s\n\nYes: save a backup somewhere else first\nNo: close anyway (recent changes are lost)\nCancel: keep the window open", p);
            k = MessageBoxW(hwnd, m, L"Icewind Dale Almanac", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
            if (k == IDCANCEL) return 0;
            if (k == IDYES) {
                WCHAR msg[600];
                ui_export_backup(hwnd, msg, 600);
                if (!msg[0] || wcsncmp(msg, L"Backup saved", 12) != 0) {   /* cancelled / failed: stay */
                    if (msg[0]) { char *u = u8_dup(msg); if (u) { ui_toast(u); free(u); } }
                    return 0;
                }
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
        close_and_save();
        return TRUE;

    case WM_ENDSESSION:
        if (wp) close_and_save();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, T_TOAST); KillTimer(hwnd, T_NOTE); KillTimer(hwnd, T_ARM);
        KillTimer(hwnd, T_SAVE); KillTimer(hwnd, T_THUNDER); KillTimer(hwnd, T_FADE);
        s_thunder_timer = false;
        audio_shutdown();
        if (g.tip_hwnd) { DestroyWindow(g.tip_hwnd); g.tip_hwnd = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================================================================== startup */

static bool parse_int(const WCHAR *s, long long *out, const WCHAR **end)
{
    long long v = 0; int n = 0; bool neg = false;
    if (!s) return false;
    if (*s == L'-' || *s == L'+') { neg = *s == L'-'; s++; }
    while (*s >= L'0' && *s <= L'9') {
        v = v * 10 + (*s - L'0');
        if (v > 2000000000LL) return false;
        s++; n++;
    }
    if (!n) return false;
    *out = neg ? -v : v;
    if (end) *end = s;
    else if (*s) return false;
    return true;
}

static void parse_args(void)
{
    int argc = 0, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL, *end;
        long long v, v2;
        if (!lstrcmpW(a, L"--shot-ready")) { s_opt_shot_ready = true; continue; }
        if (!n) break;
        if (!lstrcmpW(a, L"--state")) { s_opt_state = _wcsdup(n); i++; }
        else if (!lstrcmpW(a, L"--seed")) {
            int k = WideCharToMultiByte(CP_UTF8, 0, n, -1, NULL, 0, NULL, NULL);
            if (k > 0 && (s_opt_seed = (char *)malloc((size_t)k)) != NULL)
                WideCharToMultiByte(CP_UTF8, 0, n, -1, s_opt_seed, k, NULL, NULL);
            i++;
        }
        else if (!lstrcmpW(a, L"--days") && parse_int(n, &v, NULL)) { s_opt_days = (int)(v < 0 ? 0 : v > IWD_MAX_DAYS ? IWD_MAX_DAYS : v); i++; }
        else if (!lstrcmpW(a, L"--select") && parse_int(n, &v, NULL)) { s_opt_select = (int)v; i++; }
        else if (!lstrcmpW(a, L"--hour") && parse_int(n, &v, NULL)) { s_opt_hour = (int)(v < 0 ? 0 : v > 23 ? 23 : v); i++; }
        else if (!lstrcmpW(a, L"--minute") && parse_int(n, &v, NULL)) { s_opt_minute = (int)(v < 0 ? 0 : v > 59 ? 59 : v); i++; }
        else if (!lstrcmpW(a, L"--scroll") && parse_int(n, &v, NULL)) { s_opt_scroll = (int)(v < 0 ? 0 : v); i++; }
        else if (!lstrcmpW(a, L"--size") && parse_int(n, &v, &end) && (*end == L'x' || *end == L'X') &&
                 parse_int(end + 1, &v2, NULL)) {
            s_opt_w = (int)clampi((int)v, 200, 16000); s_opt_h = (int)clampi((int)v2, 150, 16000); i++;
        }
        else if (!lstrcmpW(a, L"--open")) {
            if (!lstrcmpiW(n, L"settings")) s_opt_open = 1;
            else if (!lstrcmpiW(n, L"eventform")) s_opt_open = 2;
            i++;
        }
    }
    if (argv) LocalFree(argv);
}

static void load_state(void)
{
    AlmLoadStatus st;
    if (!alm_init(&g.a, entropy())) {
        MessageBoxW(NULL, L"Out of memory.", L"Icewind Dale Almanac", MB_ICONERROR);
        ExitProcess(1);
    }
    s_rng ^= entropy();
    if (s_opt_seed) {
        s_persist = s_opt_state != NULL;            /* never overwrite the real save from a test run */
        load_seeded(s_opt_seed);
    } else {
        WCHAR p[MAX_PATH * 2];
        int tries;
        DWORD err = 0;
        path_w(g.state_path, p, MAX_PATH * 2);
        /* an existing file that cannot be opened (held by a sync or antivirus tool) is not
           corrupt: wait a little, then run without touching it */
        for (tries = 0; tries < 6; tries++) {
            HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); err = 0; break; }
            err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) break;
            Sleep(250);
        }
        if (err && err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            s_persist = false;
            s_boot_notice = 2;
        } else {
            st = alm_load_file(&g.a, g.state_path);
            if (st == ALM_LOAD_NO_MEMORY) {
                MessageBoxW(NULL, L"Out of memory while loading the almanac.", L"Icewind Dale Almanac", MB_ICONERROR);
                ExitProcess(1);
            }
            if (st == ALM_LOAD_CORRUPT) {
                /* keep the unreadable save under a unique name; never overwrite it */
                SYSTEMTIME t; WCHAR b[MAX_PATH * 2 + 40]; const WCHAR *base;
                GetLocalTime(&t);
                wsprintfW(b, L"%s.bad-%04d%02d%02d-%02d%02d%02d", p, t.wYear, t.wMonth, t.wDay,
                          t.wHour, t.wMinute, t.wSecond);
                base = b + lstrlenW(b);
                while (base > b && base[-1] != L'\\' && base[-1] != L'/') base--;
                lstrcpynW(s_bad_name, base, MAX_PATH);
                if (MoveFileExW(p, b, MOVEFILE_WRITE_THROUGH)) s_boot_notice = 1;
                else { s_persist = false; s_boot_notice = 3; }
            }
        }
    }
    if (s_opt_days > 0) {
        int i;
        for (i = 0; i < s_opt_days; i++) {
            AlmResult r = alm_advance(&g.a, IWD_SETTLE_HOUR);
            if (r.fx & ALM_FX_REJECTED) break;
        }
    }
    if (s_opt_select != INT32_MIN) (void)alm_select(&g.a, g.a.start_abs + s_opt_select, false);
    if ((s_opt_hour >= 0 || s_opt_minute >= 0) && alm_has_view(&g.a)) {
        int h = s_opt_hour >= 0 ? s_opt_hour : alm_hour_now(&g.a);
        (void)alm_set_time(&g.a, (h * 60 + (s_opt_minute >= 0 ? s_opt_minute : 0)) / 60.0);
    }
    g.party_time = -1;
    g.saved_scroll = -1;
    s_last_sel = g.a.sel_abs;
    g.notes_abs = INT32_MIN;
    g.last_wind_event = -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc; MSG msg; HWND hwnd;
    (void)prev; (void)cmdline;

    g.inst = inst;
    parse_args();
    resolve_state_path();
    if (!s_opt_seed || s_opt_state) {
        /* one window per save file: a second launch brings the first forward */
        WCHAR name[80]; uint32_t hsh = 2166136261u; const char *c;
        for (c = g.state_path; *c; c++) {
            unsigned char ch = (unsigned char)*c;
            if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + 32);
            hsh = (hsh ^ ch) * 16777619u;
        }
        wsprintfW(name, L"Local\\IcewindDaleAlmanac-%08x", hsh);
        s_mutex = CreateMutexW(NULL, FALSE, name);
        if (s_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(L"IcewindDaleAlmanac", NULL);
            if (w) {
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
            }
            CloseHandle(s_mutex);
            return 0;
        }
    }
    load_state();
    gdip_up();
    audio_init();
    if (!audio_device_present()) g.sound_missing = true;   /* "No audio" from the first paint */
    settings_register(inst);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"IcewindDaleAlmanac";
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Icewind Dale Almanac", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    if (s_opt_w > 0 && s_opt_h > 0) {
        force_client_size(hwnd, s_opt_w, s_opt_h);
        ShowWindow(hwnd, show == SW_SHOWMAXIMIZED ? SW_SHOWNORMAL : show);
    } else if (!restore_placement(show)) {
        set_client_size(hwnd, dp(1280), dp(860));
        ShowWindow(hwnd, show);
    }

    audio_set_interior(g.a.native.interior);
    if (g.a.native.sound) {
        if (!audio_set_enabled(true)) { g.sound_missing = true; g.a.native.sound = false; }
    }
    wind_sync(true);

    {   /* first layout, then the optional scroll hook */
        RECT cr; GetClientRect(hwnd, &cr);
        if (cr.right > 0 && cr.bottom > 0) surf_resize(&g.back, cr.right, cr.bottom);
        g_tgt = &g.back;
        view_layout();
        if (s_opt_scroll >= 0) view_scroll_to(s_opt_scroll);
    }
    ui_save_now();                       /* once at boot after load + clean (SPEC 6.3) */
    if (s_boot_notice) {
        char t[ALM_TOAST_CAP], n8[MAX_PATH * 3];
        to_utf8(s_bad_name, n8, (int)sizeof n8);
        if (s_boot_notice == 1)
            snprintf(t, sizeof t, "<b>The saved almanac could not be read.</b> It was kept as <b>%.200s</b> next to it; a new almanac was started.", n8);
        else if (s_boot_notice == 2)
            snprintf(t, sizeof t, "<b>The saved almanac is in use by another program</b> and could not be opened. Nothing will be saved this session; close and reopen to try again.");
        else
            snprintf(t, sizeof t, "<b>The saved almanac could not be read</b> and could not be set aside. It was left untouched and nothing will be saved this session.");
        toast_sticky(t);
    }
    ui_relayout();
    UpdateWindow(hwnd);
    SetFocus(hwnd);
    if (s_opt_open) PostMessageW(hwnd, WM_APP_HOOKS, 0, 0);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    view_free();
    scene_free();
    thumb_free();
    img_free_all();
    surf_free(&g.back);
    fonts_free();
    if (g.br_field) DeleteObject(g.br_field);
    if (g.br_field_dis) DeleteObject(g.br_field_dis);
    if (s_menu_br) DeleteObject(s_menu_br);
    gdip_down();
    alm_free(&g.a);
    if (s_uxtheme) FreeLibrary(s_uxtheme);
    if (s_mutex) CloseHandle(s_mutex);
    free(s_opt_state);
    free(s_opt_seed);
    return (int)msg.wParam;
}
