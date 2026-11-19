/* ui_view.c - page composition of the main window: layout of the blocks (wide two-column
 * and narrow one-column), vertical scrolling, the sticky header, the live-weather blocks
 * HERO / CONTROLS / STATS / EFFECTS / FOOT, the toast and the slim scrollbar.
 * TIMELINE + LEDGER are painted by ui_view_time.c, CALENDAR + DAY by ui_view_cal.c
 * (see ui_blocks.h).
 *
 * Coordinates: the back buffer is the client area. The header (view_header_h() px) is
 * sticky at the top; below it the content scrolls. Content coordinates are unscrolled with
 * y = 0 at the top of the scrolling area:  screen_y = content_y - g.scroll + header_h.
 *
 * Layout (in-game use, SPEC 10). Wide (client width >= 940 logical px):
 *     left  ~56%:  HERO, TIMELINE, CALENDAR
 *     right ~44%:  CONTROLS, STATS, EFFECTS, LEDGER, DAY
 *     FOOT across the bottom.
 *   At the default size (1280x860) the scene, event name, temperature + tier/DC, the
 *   advance/hour controls, wind, severity, natural light, forecast, the timeline and the
 *   whole effects list are above the fold: the hero is shortened so hero + timeline fit the
 *   viewport, and the effects list chooses 1..3 sub-columns so that it ends inside the
 *   viewport (White Out, 19 items, needs two). The calendar starts below the timeline
 *   (its head is visible). The effects block is the last live block of its column, so its
 *   changing height moves only the ledger and the day panel below it (never the calendar).
 *   The day panel's EDIT controls are re-placed after every paint (edits_sync), so they
 *   follow it. Calendar (left) and the selected day (right) sit side by side when scrolled.
 *   Narrow (below 940 px, e.g. a window snapped to half a laptop screen): HERO,
 *   CONTROLS, STATS, EFFECTS, TIMELINE, CALENDAR, DAY, LEDGER, FOOT.
 *
 * view_paint() re-runs the layout every time (cheap: a few text measurements), so block
 * heights that change with the hour or the day never overlap even when the caller only
 * asked for ui_redraw().
 *
 * HIT REGIONS registered here (id: arg):
 *   H_NONE      : 0   opaque cover behind the header and the toast (blocks the page below)
 *   H_SEED      : 0   seed chip (web: opens Settings with the seed field focused)
 *   H_UNIT_F    : 0   / H_UNIT_C : 0   temperature unit segments
 *   H_SOUND     : 0   sound toggle chip
 *   H_SETTINGS  : 0   header Settings chip
 *   H_ADVANCE   : 0   Advance Day / Begin the Expedition (enabled = alm_controls().advance_enabled)
 *   H_PREV_HOUR : 0   / H_NEXT_HOUR : 0   whole-hour stepping (alm_step_hour(-1 / +1))
 *   H_CHAIN_X   : 0   hide the chained-days note
 *   H_TIP       : 0   tooltip-only regions (tier pill, mark chips, event name, badges)
 *   H_FOOT_SETTINGS : 0   "Campaign settings…" link
 *   H_HEAD_NOW  : 0   header live readout shown while the hero is scrolled away (to top)
 *   H_BACK_TODAY: 0   "Viewing tomorrow · Back to today" ribbon in the controls
 *   H_INTRO_SETTINGS : 0  "Change start date or seed…" link before the first dawn
 *   H_KEYS      : 0   "?" chip in the header (keyboard shortcuts card)
 *   H_KEYS_CARD : 0   the open shortcuts card (click closes)
 *   H_SAVE_FAIL : 0   "Not saved" header chip (Save dialog for a backup)
 *   H_TOAST     : 0   the toast box (click dismisses)
 *   H_SCROLL    : 0 thumb (start a drag; g.sb_grab = mouse_y - thumb top, see
 *                 view_scroll_for_thumb), -1 track above the thumb (page up),
 *                 +1 track below the thumb (page down)
 */
#include "ui_blocks.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <wctype.h>

/* Public helper beyond ui.h (CONTRACT GAP, see report): scroll offset for a thumb drag,
 * from the mouse y in client coordinates and g.sb_grab (offset of the grab inside the
 * thumb). main.c may call it from WM_MOUSEMOVE while g.sb_drag. */

/* ================================================================== state */

static RECT s_blk[BLK_COUNT];        /* content coordinates */
static int  s_fx_avail = INT_MAX;    /* height the effects block should try to stay within */
static bool s_glare;                 /* bright-sky contrast switch (hysteresis) */

/* scrollbar geometry of the last paint (client coords) */
static int  s_sb_top, s_sb_h, s_th_h, s_th_y;
static bool s_sb_on;

static int client_w(void)
{
    RECT r;
    if (g.back.w > 0) return g.back.w;
    if (g.hwnd && GetClientRect(g.hwnd, &r)) return r.right - r.left;
    return dp(1280);
}

static int client_h(void)
{
    RECT r;
    if (g.back.h > 0) return g.back.h;
    if (g.hwnd && GetClientRect(g.hwnd, &r)) return r.bottom - r.top;
    return dp(860);
}

int view_header_h(void) { return dp(42); }

static int view_h(void) { int v = client_h() - view_header_h(); return v > 1 ? v : 1; }

static int max_scroll(void) { int m = g.content_h - view_h(); return m > 0 ? m : 0; }

int view_content_y(int screen_y) { return screen_y - view_header_h() + g.scroll; }

static int screen_y(int content_y) { return content_y - g.scroll + view_header_h(); }

RECT view_block_rect(int which)
{
    RECT z = { 0, 0, 0, 0 };
    if (which < 0 || which >= BLK_COUNT) return z;
    return s_blk[which];
}

void view_scroll_to(int y)
{
    int ny = clampi(y, 0, max_scroll());
    if (ny != g.scroll) {
        g.scroll = ny;
        ui_redraw();
    }
}

void view_ensure_visible(int y0, int y1)
{
    int vh = view_h(), m = dp(12);
    if (y1 - y0 > vh - 2 * m) y1 = y0 + vh - 2 * m;        /* too tall: show its top */
    if (y0 - m < g.scroll) view_scroll_to(y0 - m);
    else if (y1 + m > g.scroll + vh) view_scroll_to(y1 + m - vh);
}

int view_scroll_for_thumb(int mouse_y)
{
    int travel = s_sb_h - s_th_h, top;
    if (!s_sb_on || travel <= 0) return g.scroll;
    top = clampi(mouse_y - g.sb_grab - s_sb_top, 0, travel);
    return (int)((long long)top * max_scroll() / travel);
}

/* ================================================================== small helpers */

static DWORD rgb_u(uint32_t c) { return (DWORD)(c & 0xFFFFFF); }

static bool is_hot(int id) { return g.hot_id == id; }
static bool is_pressed(int id) { return g.press_id == id && g.hot_id == id; }

static void panel_bg(int x, int y, int w, int h)
{
    px_fill(x, y, x + w, y + h, C_PANEL);
    px_frame(x, y, x + w, y + h, 1, C_LINE);
}

static void caps_label(int x, int y, const WCHAR *s, DWORD c) { tx_caps(F_CAPS_S, x, y, s, c); }

/* UTF-8 into a caller buffer (for strings that must outlive the wz() ring) */
static int to_w(const char *s, WCHAR *out, int cap)
{
    int n;
    if (cap <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
    if (n <= 0) { out[cap - 1] = 0; n = lstrlenW(out); if (!n) out[0] = 0; return lstrlenW(out); }
    return n - 1;
}

/* A thin pill: dark wash, 1 px tinted border, tinted text. solid = filled with col. */
static int pill_h(int f) { return fh(f) + dp(4); }

static bool s_lg;                    /* hero on a light ground (White Out): dark ink */
#define INK_LG 0x0B1828

static int pill(int f, int x, int y, const WCHAR *s, DWORD col, bool solid, bool draw)
{
    int w = tw(f, s, -1) + dp(12), h = pill_h(f);
    if (draw) {
        if (s_lg && !solid) {
            px_blend(x, y, x + w, y + h, 0xFFFFFF, 140);
            px_frame(x, y, x + w, y + h, 1, mix(INK_LG, col, 90));
            tx(f, x + dp(6), y + dp(2), s, -1, INK_LG);
        } else if (solid) {
            px_box(x, y, x + w, y + h, col, col);
            tx(f, x + dp(6), y + dp(2), s, -1, C_BG);
        } else {
            px_blend(x, y, x + w, y + h, C_SHADOW, 165);
            px_frame(x, y, x + w, y + h, 1, mix(C_SHADOW, col, 150));
            tx(f, x + dp(6), y + dp(2), s, -1, col);
        }
    }
    return w;
}

/* Mark chip (calendar.js chipHtml): type-coloured left rule + text; builtin new-moon
 * mark gets a small frost mark instead. Returns the width. */
static int chip_h(void) { return fh(F_SMALL) + dp(6); }

static int chip(int x, int y, const AlmMark *m, int maxw, bool draw)
{
    const WCHAR *s = wz(m->chip ? m->chip : m->title);
    DWORD col = rgb_u(alm_ev_type_rgb(m->type));
    int lead = m->builtin ? dp(18) : dp(11), w = lead + tw(F_SMALL, s, -1) + dp(8), h = chip_h();
    if (maxw > 0 && w > maxw) w = maxw;
    if (draw) {
        px_blend(x, y, x + w, y + h, C_SHADOW, 170);
        px_frame(x, y, x + w, y + h, 1, mix(C_SHADOW, col, 70));
        if (m->builtin) {
            aa_flake(x + dp(9), y + h / 2.0, dp(5), col, 235);
            aa_flush();
        } else {
            px_fill(x + dp(1), y + dp(1), x + dp(4), y + h - dp(1), col);
        }
        tx_ellipsis(F_SMALL, x + lead, y + dp(3), w - lead - dp(6), s, C_TEXT);
        hot_add(x, y, x + w, y + h, H_TIP, 0, true, wz(m->title));
    }
    return w;
}

/* Text with the store's markup (<b>..</b>, <code>..</code>; &amp; &lt; &gt; escaped):
 * returns the height; *width_out gets the unwrapped single-line width. */
#define MK_CAP (ALM_TOAST_CAP * 2)
static int markup(const char *u8, int x, int y, int w, int f, int fb, DWORD c, DWORD cb,
                  bool draw, int *width_out)
{
    static WCHAR in[MK_CAP], out[MK_CAP];
    Span sp[48];
    int n = to_w(u8, in, MK_CAP), i = 0, o = 0, nsp = 0, start = 0, total = 0;
    bool bold = false;
    while (i < n && nsp < 47) {
        bool tag = false, nb = bold;
        if (in[i] == L'<') {
            if (!wcsncmp(in + i, L"<b>", 3)) { tag = true; nb = true; i += 3; }
            else if (!wcsncmp(in + i, L"</b>", 4)) { tag = true; nb = false; i += 4; }
            else if (!wcsncmp(in + i, L"<code>", 6)) { tag = true; nb = true; i += 6; }
            else if (!wcsncmp(in + i, L"</code>", 7)) { tag = true; nb = false; i += 7; }
        }
        if (tag) {
            if (o > start) {
                sp[nsp].s = out + start; sp[nsp].n = o - start;
                sp[nsp].font = (BYTE)(bold ? fb : f); sp[nsp].flags = 0; sp[nsp].color = bold ? cb : c;
                total += tw(sp[nsp].font, sp[nsp].s, sp[nsp].n);
                nsp++;
            }
            start = o; bold = nb;
            continue;
        }
        if (in[i] == L'&') {
            if (!wcsncmp(in + i, L"&amp;", 5)) { out[o++] = L'&'; i += 5; continue; }
            if (!wcsncmp(in + i, L"&lt;", 4))  { out[o++] = L'<'; i += 4; continue; }
            if (!wcsncmp(in + i, L"&gt;", 4))  { out[o++] = L'>'; i += 4; continue; }
        }
        out[o++] = in[i++];
    }
    if (o > start && nsp < 48) {
        sp[nsp].s = out + start; sp[nsp].n = o - start;
        sp[nsp].font = (BYTE)(bold ? fb : f); sp[nsp].flags = 0; sp[nsp].color = bold ? cb : c;
        total += tw(sp[nsp].font, sp[nsp].s, sp[nsp].n);
        nsp++;
    }
    if (width_out) *width_out = total;
    if (!nsp) return 0;
    return rich(sp, nsp, x, y, w, fh(fb) > fh(f) ? fh(fb) : fh(f), draw);
}

/* button with optional key hint at the right */
static void button(int x0, int y0, int x1, int y1, int id, bool en, const WCHAR *label, int f,
                   const WCHAR *hint, const WCHAR *tip)
{
    bool hot = en && is_hot(id), press = en && is_pressed(id);
    DWORD fill = !en ? mix(C_PANEL, C_PANEL2, 120) : press ? C_FIELD : hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2;
    DWORD border = !en ? C_LINE : hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME;
    DWORD ink = en ? C_BRIGHT : C_DIM;
    int hw = hint ? tw(F_TINY_B, hint, -1) + dp(8) : 0;
    int avail = x1 - x0 - dp(12) - (hw ? hw + dp(4) : 0);
    int lw = tw(f, label, -1);
    int lx = (x0 + x1 - (hw ? hw + dp(6) : 0) - (lw < avail ? lw : avail)) / 2;
    px_box(x0, y0, x1, y1, fill, border);
    if (lx < x0 + dp(6)) lx = x0 + dp(6);
    tx_ellipsis(f, lx, (y0 + y1 - fh(f)) / 2, avail, label, ink);
    if (hw && x1 - x0 > hw + dp(40)) {
        int hx = x1 - dp(6) - hw, hy = (y0 + y1 - fh(F_TINY_B)) / 2 - dp(1);
        px_frame(hx, hy, hx + hw, hy + fh(F_TINY_B) + dp(2), 1, en ? C_FRAME : C_LINE);
        tx(F_TINY_B, hx + dp(4), hy + dp(1), hint, -1, en ? C_MUTED : C_DIM);
    }
    hot_add(x0, y0, x1, y1, id, 0, en, tip);
}

/* small outlined key cap ("T", "PgDn"); returns its width */
static int keycap(int x, int y, const WCHAR *k, DWORD border, DWORD ink, bool draw)
{
    int w = tw(F_TINY_B, k, -1) + dp(8);
    if (draw) {
        px_frame(x, y, x + w, y + fh(F_TINY_B) + dp(2), 1, border);
        tx(F_TINY_B, x + dp(4), y + dp(1), k, -1, ink);
    }
    return w;
}

/* ================================================================== live values */

typedef struct Live {
    bool  intro;
    const IwdDay *day;
    int   hour;
    const IwdEventDef *ev;
    int   temp_f;
    IwdCold cold;
    IwdLight light;
} Live;

static void live_get(Live *L)
{
    memset(L, 0, sizeof *L);
    L->hour = alm_hour_now(&g.a);
    if (!alm_has_view(&g.a) || g.a.view < 0 || g.a.view >= g.a.n_days) {
        L->intro = true;
        L->ev = iwd_event(1);
        return;
    }
    L->day = &g.a.days[g.a.view];
    L->ev = iwd_event(L->day->hour_id[L->hour]);
    if (!L->ev) { L->intro = true; L->day = NULL; L->ev = iwd_event(1); return; }
    L->temp_f = L->day->temp_f[L->hour];
    L->cold = iwd_cold_dc(L->temp_f);
    L->light = iwd_light_at(L->ev, L->hour);
}

static DWORD tier_rgb(int idx)
{
    static const DWORD c[5] = { 0xC2E8FB, 0xA9DCFF, 0x9FBCFF, 0xCDB3FF, 0xFFC2CF };
    return c[clampi(idx, 0, 4)];
}

static DWORD light_rgb(IwdLightLevel lv)
{
    return lv == IWD_LIGHT_DIM ? 0xE0F4FF : lv == IWD_LIGHT_DARK ? 0xC7B9FF : 0xFFB4C2;
}

static DWORD cat_rgb(IwdCat c)
{
    switch (c) {
    case IWD_CAT_COLD: case IWD_CAT_CHILL:      return 0x9FE3FF;
    case IWD_CAT_HAZARD: case IWD_CAT_TRAVEL:   return 0xFFB4C2;
    case IWD_CAT_FLAME:                         return 0xFFC999;
    case IWD_CAT_CONCENTRATION:                 return 0xD5C4FF;
    case IWD_CAT_NATURAL:                       return 0xC7B9FF;
    case IWD_CAT_AURORA:                        return 0x7DFFCF;
    default:                                    return 0xE0F4FF;
    }
}

/* ================================================================== HERO */

/* F_TEMP metrics (ascent, approximate digit height), cached per font handle (per DPI). */
static void temp_metrics(int *ascent, int *digit_h)
{
    static HFONT key; static int asc, dh;
    if (key != font(F_TEMP) || !asc) {
        HDC dc = CreateCompatibleDC(NULL);
        TEXTMETRICW tm;
        HGDIOBJ o = SelectObject(dc, font(F_TEMP));
        if (GetTextMetricsW(dc, &tm)) {
            asc = tm.tmAscent;
            dh = (int)((tm.tmHeight - tm.tmInternalLeading) * 0.72);
        } else {
            asc = fh(F_TEMP) * 4 / 5; dh = fh(F_TEMP) / 2;
        }
        SelectObject(dc, o);
        DeleteDC(dc);
        key = font(F_TEMP);
    }
    *ascent = asc; *digit_h = dh;
}

typedef struct HeroGeo {
    int ip;               /* inner padding */
    int top_h;            /* date + meta + chips */
    int num_w, side_w, temp_w, temp_h;
    int name_w;           /* width of the name/flavour column */
    int name_h, flav_h;
    bool stacked;         /* temperature above the name (narrow hero) */
    int band_h;           /* bottom band */
} HeroGeo;

static const char *k_intro_name = "The Dale Awaits";
static const char *k_intro_flavor = "Begin the expedition to roll the first dawn. After that, every day grows out of the night before. Press N to begin.";

static void temp_texts(const Live *L, WCHAR *num, WCHAR *unit, WCHAR *note, WCHAR *tier_name, WCHAR *tier_dc)
{
    char b[64];
    iwd_fmt_minus(g.a.celsius ? iwd_f_to_c(L->temp_f) : L->temp_f, b, sizeof b);
    to_w(b, num, 32);
    lstrcpyW(unit, g.a.celsius ? L"\x00B0" L"C" : L"\x00B0" L"F");
    to_w(iwd_temp_note(L->temp_f), note, 48);
    to_w(L->cold.tier ? L->cold.tier->name : "", tier_name, 48);
    wsprintfW(tier_dc, L" DC %d", L->cold.dc);
}

static int marks_of_hero(int32_t abs, AlmMark *marks, int cap)
{
    int n = alm_marks_on(&g.a, abs, marks, cap);
    return n < cap ? n : cap;
}

static void hero_geo(const Live *L, int w, int nmarks, HeroGeo *G)
{
    int tw_ = 0;
    memset(G, 0, sizeof *G);
    G->ip = dp(18);
    tw_ = w - 2 * G->ip;
    G->top_h = fh(F_DATE) + dp(6) + pill_h(F_SMALL_B);
    if (nmarks > 0) G->top_h += dp(8) + chip_h();
    if (!L->intro) {
        WCHAR num[32], unit[8], note[48], tn[48], td[24];
        int asc, dh, side_h;
        temp_texts(L, num, unit, note, tn, td);
        temp_metrics(&asc, &dh);
        G->num_w = tw(F_TEMP, num, -1);
        G->side_w = tw(F_UNIT, unit, -1);
        if (tw(F_SMALL, note, -1) > G->side_w) G->side_w = tw(F_SMALL, note, -1);
        {
            int pw = tw(F_SMALL, tn, -1) + tw(F_SMALL_B, td, -1) + dp(12);
            if (pw > G->side_w) G->side_w = pw;
        }
        side_h = fh(F_UNIT) + fh(F_SMALL) + dp(4) + pill_h(F_SMALL_B);
        G->temp_w = G->num_w + dp(10) + G->side_w;
        G->temp_h = side_h > dh ? side_h : dh;
    }
    G->name_w = tw_ - (G->temp_w ? G->temp_w + dp(24) : 0);
    G->stacked = G->temp_w && G->name_w < dp(260);
    if (G->stacked) G->name_w = tw_;
    {
        const char *nm = L->intro ? k_intro_name : L->ev->name;
        const char *fl = L->intro ? k_intro_flavor : L->ev->flavor;
        G->name_h = wrap(F_NAME, 0, 0, G->name_w, wz(nm), C_BRIGHT, false);
        G->flav_h = wrap(F_FLAVOR, 0, 0, G->name_w, wz(fl), C_SOFT, false);
    }
    {
        int nb = G->name_h + dp(2) + G->flav_h;
        G->band_h = G->stacked ? G->temp_h + dp(10) + nb : (nb > G->temp_h ? nb : G->temp_h);
    }
}

static int hero_min_h(int w)
{
    Live L; HeroGeo G; AlmHero hr; AlmMark mk[8];
    live_get(&L);
    alm_hero(&g.a, &hr);
    hero_geo(&L, w, marks_of_hero(hr.abs, mk, 8), &G);
    return G.ip + G.top_h + dp(18) + G.band_h + G.ip;
}

static void scene_look(const Live *L, SceneLook *lk)
{
    double t = g.a.time, cloud;
    IwdLook look;
    memset(lk, 0, sizeof *lk);
    if (L->intro) {
        const IwdLook *l1 = iwd_look(1);
        lk->event_id = 1;
        lk->light_level = -1;
        cloud = l1 ? l1->cloud : 0;
        lk->twilight = iwd_band(t, 10, 14, 1.6) * (1 - 0.8 * cloud);
        lk->aurora = 0.6;
        lk->cold = 0.2;
        s_glare = false;
        return;
    }
    iwd_look_at(alm_event_at_cb, &g.a, t, &look);
    cloud = look.cloud;
    lk->event_id = L->ev->id;
    lk->light_level = (int)L->light.level;
    lk->twilight = iwd_band(t, 10, 14, 1.6) * (1 - 0.8 * cloud);
    lk->aurora = L->ev->natural ? iwd_band(t, 22, 2, 0.8) * (1 - 0.85 * cloud) : 0;
    lk->baked_aurora_subdue = L->ev->id == 1 ? 1 - iwd_band(t, 22, 2, 0.8) : 0;
    lk->dark = L->light.level == IWD_LIGHT_DARK ? 1.0 : 0.0;
    if (L->ev->id == 8) {                 /* White Out: the blinding white is the picture */
        lk->light_ground = true;
        lk->dark = 0;
    }
    {
        double c = (-(double)L->ev->temp_f - 45) / 85;
        lk->cold = c < 0 ? 0 : c > 1 ? 1 : c;
    }
    {
        double gl = look.whiteout + look.fog;
        if (gl > 0.45) s_glare = true;
        else if (gl < 0.38) s_glare = false;
    }
    lk->glare = s_glare && !lk->light_ground;
}

/* the event of the hour before (previous day's 23:00 at midnight); -1 unknown */
static int prev_hour_event(const Live *L)
{
    if (L->intro || !L->day) return -1;
    if (L->hour > 0) return L->day->hour_id[L->hour - 1];
    if (g.a.view > 0 && g.a.view - 1 < g.a.n_days) return g.a.days[g.a.view - 1].hour_id[23];
    return -1;
}

/* hero cross-fade: the previous steady frame of the hero rect */
static DWORD *s_prev; static int s_pw, s_ph, s_prev_id = -1;

static void hero_fade(int x, int y, int w, int h, int id)
{
    int yy;
    bool full = y >= view_header_h() && y + h <= g_tgt->h && x >= 0 && x + w <= g_tgt->w;
    if (!full) { s_prev_id = -1; return; }
    if (g.fade_req && id != s_prev_id && s_prev_id >= 0 && s_prev && s_pw == w && s_ph == h &&
        !g.a.playing && !g.scrubbing) {
        g.fade_k = 8;
        ui_fade_start();
    }
    if (g.fade_k > 0 && s_prev && s_pw == w && s_ph == h) {
        int a = g.fade_k * 256 / 9;
        GdiFlush();
        for (yy = 0; yy < h; yy++) {
            DWORD *d = g_tgt->px + (size_t)(y + yy) * g_tgt->w + x; const DWORD *o = s_prev + (size_t)yy * w;
            int xx;
            for (xx = 0; xx < w; xx++) d[xx] = mix(d[xx], o[xx], a);
        }
        return;
    }
    if (!s_prev || s_pw != w || s_ph != h) {
        free(s_prev);
        s_prev = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
        s_pw = s_prev ? w : 0; s_ph = s_prev ? h : 0;
        if (!s_prev) { s_prev_id = -1; return; }
    }
    GdiFlush();
    for (yy = 0; yy < h; yy++)
        memcpy(s_prev + (size_t)yy * w, g_tgt->px + (size_t)(y + yy) * g_tgt->w + x, sizeof(DWORD) * (size_t)w);
    s_prev_id = id;
}

/* hero text: drop shadow on the scene, flat dark ink on a light ground */
static void htx(int f, int x, int y, const WCHAR *t, DWORD c)
{
    if (s_lg) tx(f, x, y, t, -1, c);
    else tx_sh(f, x, y, t, -1, c);
}

static void hero_paint(int x, int y, int w, int h)
{
    Live L; HeroGeo G; AlmHero hr; AlmMark mk[8]; SceneLook lk;
    const DWORD *sc;
    int nm, ip, tx0, ty, cx, bottom, clk_x;
    WCHAR wb[128];
    DWORD c_bright, c_ice, c_soft, c_flav;
    bool other_day;

    live_get(&L);
    alm_hero(&g.a, &hr);
    nm = marks_of_hero(hr.abs, mk, 8);
    hero_geo(&L, w, nm, &G);
    ip = G.ip;

    /* scene backdrop (cached composite with the light overlays and the text scrims) */
    scene_look(&L, &lk);
    s_lg = lk.light_ground;
    c_bright = s_lg ? INK_LG : C_BRIGHT;
    c_ice    = s_lg ? 0x1C2838 : C_ICE;
    c_soft   = s_lg ? 0x2B3A50 : C_SOFT;
    c_flav   = s_lg ? 0x2B3A50 : 0xDCE8F4;
    sc = scene_get(&lk, w, h);
    if (sc) { GdiFlush(); px_copy(sc, w, h, x, y); }
    else px_fill(x, y, x + w, y + h, C_PANEL2);
    px_frame(x, y, x + w, y + h, 1, C_LINE);
    other_day = hr.tag_kind == ALM_TAG_AHEAD || hr.tag_kind == ALM_TAG_PAST;
    if (other_day) px_fill(x, y, x + w, y + dp(2), 0xFFC999);   /* not the party's day */

    tx0 = x + ip;
    ty = y + ip;

    /* the clock, large, at the top right: the value that changes most often */
    clk_x = x + w - ip;
    if (!L.intro) {
        char clk[16];
        iwd_fmt_clock(g.a.time, clk, sizeof clk);
        to_w(clk, wb, 128);
        clk_x = x + w - ip - tw(F_CLOCK, wb, -1);
        htx(F_CLOCK, clk_x, ty - dp(6), wb, L.intro ? c_soft : c_bright);
        hot_add(clk_x, ty - dp(4), x + w - ip, ty + fh(F_CLOCK) - dp(8), H_TIP, 0, true,
                L"Time of day (\x2190 / \x2192 step an hour, Home / End 00:00 / 23:00)");
        clk_x -= dp(14);
    }

    /* line 1: moon disc + date + tag */
    {
        double r = dp(10);
        int dy = ty + (fh(F_DATE) - (int)(2 * r)) / 2;
        aa_moon(tx0 + r, dy + r, r, &hr.moon);
        aa_flush();
        cx = tx0 + (int)(2 * r) + dp(10);
        to_w(hr.date, wb, 128);
        htx(F_DATE, cx, ty, wb, c_bright);
        cx += tw(F_DATE, wb, -1) + dp(10);
        if (hr.tag_kind != ALM_TAG_NONE && hr.tag[0]) {
            DWORD col = hr.tag_kind == ALM_TAG_TODAY ? C_ICE : hr.tag_kind == ALM_TAG_START ? C_AURORA : 0xFFC999;
            int py = ty + (fh(F_DATE) - pill_h(F_SMALL_B)) / 2;
            bool solid = hr.tag_kind == ALM_TAG_TODAY || other_day;
            if (cx + pill(F_SMALL_B, 0, 0, wz(hr.tag), 0, false, false) <= clk_x)
                pill(F_SMALL_B, cx, py, wz(hr.tag), col, solid, true);
        }
        ty += fh(F_DATE) + dp(6);
    }

    /* line 2: day label · moon phase · light badge · aurora badge · "began this hour" */
    {
        const WCHAR *sep = L"  \x00B7  ";
        int my = ty + (pill_h(F_SMALL_B) - fh(F_SMALL)) / 2, right = x + w - ip;   /* below the clock's digits */
        cx = tx0;
        to_w(hr.day_label, wb, 128);
        htx(F_SMALL_B, cx, my, wb, c_ice); cx += tw(F_SMALL_B, wb, -1);
        htx(F_SMALL, cx, my, sep, c_soft); cx += tw(F_SMALL, sep, -1);
        to_w(hr.moon.name ? hr.moon.name : "", wb, 128);
        htx(F_SMALL, cx, my, wb, c_ice); cx += tw(F_SMALL, wb, -1) + dp(12);
        if (!L.intro) {
            const WCHAR *b = wz(iwd_light_badge(L.light.level));
            int bw = pill(F_SMALL_B, 0, 0, b, 0, false, false);
            if (cx + bw <= right) {
                pill(F_SMALL_B, cx, ty, b, light_rgb(L.light.level), false, true);
                {
                    char lab[IWD_LIGHT_LABEL_CAP];
                    iwd_light_label(L.ev, &L.light, lab, sizeof lab);
                    hot_add(cx, ty, cx + bw, ty + pill_h(F_SMALL_B), H_TIP, 0, true, wz(lab));
                }
                cx += bw + dp(6);
            }
        }
        if (L.intro || L.light.aurora_visible) {
            const WCHAR *b = L"Auril's Aurora visible";
            int bw = pill(F_SMALL_B, 0, 0, b, 0, false, false);
            if (cx + bw <= right) { pill(F_SMALL_B, cx, ty, b, C_AURORA, false, true); cx += bw + dp(6); }
        }
        if (!L.intro) {
            int pe = prev_hour_event(&L);
            if (pe > 0 && pe != L.ev->id) {
                const WCHAR *b = L"Began this hour";
                int bw = pill(F_SMALL_B, 0, 0, b, 0, false, false);
                if (cx + bw <= right) pill(F_SMALL_B, cx, ty, b, C_ACCENT, false, true);
            }
        }
        ty += pill_h(F_SMALL_B);
    }

    /* line 3: marks of the day */
    if (nm > 0) {
        int i, right = x + w - ip;
        ty += dp(8);
        cx = tx0;
        for (i = 0; i < nm; i++) {
            int cw = chip(0, 0, &mk[i], 0, false);
            if (cx + cw > right) {
                if (cx + dp(30) <= right) {
                    WCHAR more[16]; wsprintfW(more, L"+%d", nm - i);
                    htx(F_SMALL_B, cx + dp(2), ty + dp(3), more, c_soft);
                }
                break;
            }
            chip(cx, ty, &mk[i], 0, true);
            cx += cw + dp(6);
        }
    }

    /* bottom band: event name + flavour (left), temperature (right) */
    bottom = y + h - ip;
    {
        const char *nmtxt = L.intro ? k_intro_name : L.ev->name;
        const char *fl = L.intro ? k_intro_flavor : L.ev->flavor;
        int fy = bottom - G.flav_h, ny = fy - dp(2) - G.name_h;
        Span sp;
        sp.s = wz(nmtxt); sp.n = -1; sp.font = F_NAME; sp.flags = s_lg ? 0 : SP_SHADOW; sp.color = c_bright;
        rich(&sp, 1, tx0, ny, G.name_w, fh(F_NAME), true);
        if (!L.intro) {
            char ann[256];
            if (alm_fmt_announce(&g.a, ann, sizeof ann) > 0)
                hot_add(tx0, ny, tx0 + G.name_w, fy, H_TIP, 0, true, wz(ann));
        }
        sp.s = wz(fl); sp.font = F_FLAVOR; sp.color = c_flav;
        rich(&sp, 1, tx0, fy, G.name_w, fh(F_FLAVOR), true);

        if (!L.intro) {
            WCHAR num[32], unit[8], note[48], tn[48], td[24];
            int asc, dh, tx_r, tb, nx, sx, sy;
            temp_texts(&L, num, unit, note, tn, td);
            temp_metrics(&asc, &dh);
            if (G.stacked) { tx_r = tx0 + G.temp_w; tb = ny - dp(10); }
            else           { tx_r = x + w - ip;     tb = bottom; }
            nx = tx_r - G.temp_w;
            /* number: baseline on the band's bottom */
            htx(F_TEMP, nx, tb - asc, num, c_bright);
            /* side column: unit, note, tier pill (bottom aligned) */
            sx = nx + G.num_w + dp(10);
            sy = tb - (fh(F_UNIT) + fh(F_SMALL) + dp(4) + pill_h(F_SMALL_B));
            if (sy > tb - dh - dp(4)) sy = tb - dh - dp(4);
            htx(F_UNIT, sx, sy, unit, c_ice);
            sy += fh(F_UNIT);
            htx(F_SMALL, sx, sy, note, c_soft);
            sy = tb - pill_h(F_SMALL_B);
            {
                DWORD col = tier_rgb(L.cold.tier_index);
                int pw = tw(F_SMALL, tn, -1) + tw(F_SMALL_B, td, -1) + dp(12), ph = pill_h(F_SMALL_B);
                char tip[IWD_TIER_TIP_CAP];
                if (s_lg) {                    /* inverted: solid ink on the white */
                    px_fill(sx, sy, sx + pw, sy + ph, INK_LG);
                    tx(F_SMALL, sx + dp(6), sy + (ph - fh(F_SMALL)) / 2, tn, -1, col);
                } else {
                    px_blend(sx, sy, sx + pw, sy + ph, C_SHADOW, 185);
                    px_frame(sx, sy, sx + pw, sy + ph, 1, mix(C_SHADOW, col, 170));
                    tx(F_SMALL, sx + dp(6), sy + (ph - fh(F_SMALL)) / 2, tn, -1, col);
                }
                tx(F_SMALL_B, sx + dp(6) + tw(F_SMALL, tn, -1), sy + (ph - fh(F_SMALL_B)) / 2, td, -1, C_BRIGHT);
                iwd_fmt_tier_tip(&L.cold, g.a.celsius, tip, sizeof tip);
                hot_add(sx, sy, sx + pw, sy + ph, H_TIP, 0, true, wz(tip));
            }
        }
    }
    s_lg = false;
    hero_fade(x, y, w, h, L.intro ? 0 : L.ev->id);
}

/* ================================================================== CONTROLS */

#define CT_PAD dp(12)

static int chain_note_w(int w) { return w - 2 * CT_PAD - dp(30); }
static const WCHAR *k_chain = L"Each day continues from the last: weather still running at midnight carries over.";

static int adv_h(void) { return fh(F_BTN_BIG) + fh(F_SMALL) + dp(16); }
static int hour_row_h(void) { return fh(F_BTN) + dp(14); }
static int ribbon_h(void) { return dp(28); }

static bool viewing_other_day(void)
{
    return alm_has_view(&g.a) && g.a.current >= 0 && g.a.view != g.a.current;
}

/* the Advance button's second line, phrased as what happens at midnight */
static void adv_sub(const AlmControls *c, char *out, size_t cap)
{
    char d[IWD_DATE_CAP];
    if (g.a.current < 0) {
        iwd_format_date(g.a.start_abs, false, d, sizeof d);
        snprintf(out, cap, "Rolls the first day \xC2\xB7 %s", d);
        return;
    }
    if (g.a.current + 1 < IWD_MAX_DAYS && g.a.current < g.a.n_days) {
        const IwdActive *ca = &g.a.days[g.a.current].carry_out;
        const IwdEventDef *ev = iwd_event(ca->id);
        if (ev && ca->elapsed > 0)
            snprintf(out, cap, "At midnight: %s continues, %d h left", ev->name, (int)ca->duration - (int)ca->elapsed);
        else if (ev)
            snprintf(out, cap, "At midnight: %s rolls in", ev->name);
        else
            lstrcpynA(out, c->sub, (int)cap);
        return;
    }
    lstrcpynA(out, c->sub, (int)cap);
}

static int controls_measure(int w)
{
    int h = CT_PAD + adv_h() + dp(8);
    if (g.a.current < 0) return h + fh(F_SMALL) + dp(6) + fh(F_SMALL) + CT_PAD;
    h += hour_row_h();
    if (viewing_other_day()) h += dp(8) + ribbon_h();
    else if (!g.a.ui.chain_note_hidden) h += dp(8) + wrap(F_SMALL, 0, 0, chain_note_w(w), k_chain, C_MUTED, false);
    return h + CT_PAD;
}

static void controls_paint(int x, int y, int w, int h)
{
    AlmControls c;
    int ip = CT_PAD, x0 = x + ip, x1 = x + w - ip, yy = y + ip;
    char sub[160];
    panel_bg(x, y, w, h);
    alm_controls(&g.a, &c);
    adv_sub(&c, sub, sizeof sub);

    /* Advance Day */
    {
        int y1 = yy + adv_h();
        bool en = c.advance_enabled, hot = en && is_hot(H_ADVANCE), press = en && is_pressed(H_ADVANCE);
        DWORD fill = !en ? mix(C_PANEL, C_PANEL2, 120) : press ? C_FIELD
                   : hot ? mix(C_PANEL2, C_ACCENT, 46) : mix(C_PANEL2, C_ACCENT, 14);
        DWORD border = !en ? C_LINE : (g.beckon || hot) ? C_ACCENT : mix(C_FRAME, C_ACCENT, 90);
        const WCHAR *hint = L"N";
        int hw = tw(F_TINY_B, hint, -1) + dp(10), tx_w = x1 - x0 - dp(28) - hw;
        px_box(x0, yy, x1, y1, fill, border);
        if (g.beckon && en) px_frame(x0 + 1, yy + 1, x1 - 1, y1 - 1, 1, mix(fill, C_ACCENT, 150));
        tx_ellipsis(F_BTN_BIG, x0 + dp(14), yy + dp(8), tx_w, wz(c.main), en ? C_BRIGHT : C_DIM);
        tx_ellipsis(F_SMALL, x0 + dp(14), yy + dp(8) + fh(F_BTN_BIG) + dp(1), tx_w, wz(sub), en ? C_SOFT : C_DIM);
        {
            int hx = x1 - dp(12) - hw, hy = (yy + y1 - fh(F_TINY_B)) / 2 - dp(2);
            px_frame(hx, hy, hx + hw, hy + fh(F_TINY_B) + dp(4), 1, en ? mix(C_FRAME, C_ACCENT, 80) : C_LINE);
            tx(F_TINY_B, hx + dp(5), hy + dp(2), hint, -1, en ? C_SOFT : C_DIM);
        }
        hot_add(x0, yy, x1, y1, H_ADVANCE, 0, en,
                g.a.current < 0 ? L"Roll the first dawn (N, Space or \x2192)"
                                : L"Advance the party to the next day (N) \x00B7 Ctrl+Z steps back");
        yy = y1 + dp(8);
    }

    /* before the first dawn: what will start, and where to change it */
    if (g.a.current < 0) {
        char d[IWD_DATE_CAP], t[200];
        const WCHAR *lk = L"Change start date or seed\x2026 (F2)";
        bool hot = is_hot(H_INTRO_SETTINGS);
        int lw = tw(F_SMALL, lk, -1), ly = yy + fh(F_SMALL) + dp(6);
        iwd_format_date(g.a.start_abs, false, d, sizeof d);
        snprintf(t, sizeof t, "Starts %s \xC2\xB7 seed %.60s", d, g.a.seed ? g.a.seed : "");
        tx_ellipsis(F_SMALL, x0, yy, x1 - x0, wz(t), C_SOFT);
        tx(F_SMALL, x0, ly, lk, -1, hot ? C_BRIGHT : C_ACCENT);
        px_fill(x0, ly + fh(F_SMALL) - dp(1), x0 + lw, ly + fh(F_SMALL), hot ? C_BRIGHT : mix(C_PANEL, C_ACCENT, 110));
        hot_add(x0 - dp(2), ly - dp(2), x0 + lw + dp(2), ly + fh(F_SMALL) + dp(2), H_INTRO_SETTINGS, 0, true,
                L"Choose these before the first dawn: changing them later rolls all weather again");
        return;
    }

    /* hour stepping: [Previous hour] HH:MM [Next hour] */
    {
        bool view = alm_has_view(&g.a) && !g.a.busy;
        int hr = alm_hour_now(&g.a), y1 = yy + hour_row_h();
        bool prev_en = view && !(g.a.view <= 0 && hr == 0);
        bool next_en = view;
        const WCHAR *ntip = L"Next hour (Right arrow or Space)";
        char clk[16];
        int cw, bw;
        WCHAR wc[16];
        if (view && hr == 23) {
            if (g.a.view == g.a.current) {
                next_en = c.advance_enabled;
                ntip = L"Next hour: midnight advances the party to the next day (Right arrow or Space)";
            } else {
                next_en = g.a.view + 1 < IWD_MAX_DAYS;
                ntip = L"Next hour: midnight, the next day (Right arrow or Space)";
            }
        }
        iwd_fmt_clock(g.a.time, clk, sizeof clk);
        to_w(clk, wc, 16);
        cw = tw(F_VALUE, L"00:00", -1) + dp(20);
        bw = (x1 - x0 - cw) / 2;
        button(x0, yy, x0 + bw, y1, H_PREV_HOUR, prev_en,
               bw > tw(F_BTN, L"Previous hour", -1) + dp(46) ? L"Previous hour" : L"Prev", F_BTN,
               L"\x2190", L"Previous hour (Left arrow)");
        tx_center(F_VALUE, x0 + bw + cw / 2, (yy + y1 - fh(F_VALUE)) / 2, wc, view ? C_BRIGHT : C_DIM);
        button(x1 - bw, yy, x1, y1, H_NEXT_HOUR, next_en,
               bw > tw(F_BTN, L"Next hour", -1) + dp(46) ? L"Next hour" : L"Next", F_BTN,
               L"\x2192", ntip);
        yy = y1;
    }

    /* another day on screen: say so, and offer the way back */
    if (viewing_other_day()) {
        AlmHero hr; char t[160], d[IWD_DATE_CAP];
        int rh = ribbon_h(), ry0, ry1, kx, bw;
        bool hot = is_hot(H_BACK_TODAY);
        const WCHAR *back = L"Back to today";
        DWORD amber = 0xFFC999;
        yy += dp(8);
        ry0 = yy; ry1 = yy + rh;
        alm_hero(&g.a, &hr);
        if (hr.tag[0] >= 'A' && hr.tag[0] <= 'Z') hr.tag[0] = (char)(hr.tag[0] + 32);
        iwd_format_date(hr.abs, true, d, sizeof d);
        snprintf(t, sizeof t, "Viewing %s \xC2\xB7 %s", hr.tag[0] ? hr.tag : "another day", d);
        px_fill(x0, ry0, x1, ry1, mix(C_PANEL2, amber, hot ? 60 : 40));
        px_fill(x0, ry0, x0 + dp(2), ry1, amber);
        kx = x1 - dp(8) - keycap(0, 0, L"T", 0, 0, false);
        keycap(kx, (ry0 + ry1 - fh(F_TINY_B)) / 2 - dp(1), L"T", mix(C_PANEL2, amber, 160), C_BRIGHT, true);
        bw = tw(F_SMALL_B, back, -1);
        tx(F_SMALL_B, kx - dp(8) - bw, (ry0 + ry1 - fh(F_SMALL_B)) / 2, back, -1, hot ? C_BRIGHT : amber);
        tx_ellipsis(F_SMALL_B, x0 + dp(10), (ry0 + ry1 - fh(F_SMALL_B)) / 2, kx - dp(20) - bw - (x0 + dp(10)), wz(t), C_BRIGHT);
        hot_add(x0, ry0, x1, ry1, H_BACK_TODAY, 0, true, L"Back to the party's day (T or Esc)");
        return;
    }

    /* chained-days note */
    if (!g.a.ui.chain_note_hidden) {
        int nw = chain_note_w(w), nh;
        yy += dp(8);
        nh = wrap(F_SMALL, x0, yy, nw, k_chain, C_MUTED, true);
        {
            int bx = x1 - dp(22), by = yy - dp(2), s = dp(22);
            bool hot = is_hot(H_CHAIN_X);
            double m = dp(7);
            if (hot) px_box(bx, by, bx + s, by + s, mix(C_PANEL, C_PANEL2, 200), C_FRAME);
            aa_line(bx + m, by + m, bx + s - m, by + s - m, hot ? C_BRIGHT : C_MUTED, 255, 1.4);
            aa_line(bx + s - m, by + m, bx + m, by + s - m, hot ? C_BRIGHT : C_MUTED, 255, 1.4);
            aa_flush();
            hot_add(bx, by, bx + s, by + s, H_CHAIN_X, 0, true, L"Hide this note (bring it back in Settings)");
        }
        (void)nh;
    }
}

/* ================================================================== STATS */

#define ST_PAD dp(12)

static void meter(int x, int y, int n, int lit, DWORD on)
{
    int i, pw = dp(16), ph = dp(4) < 3 ? 3 : dp(4);
    for (i = 0; i < n; i++) {
        int px = x + i * (pw + dp(4));
        px_fill(px, y, px + pw, y + ph, i < lit ? on : C_FRAME);
    }
}

typedef struct StatTexts {
    WCHAR wind[32], sev[32], light[IWD_LIGHT_LABEL_CAP + 24];
    int wind_lit, sev_lit;
    bool lethal;
    DWORD light_col;
    WCHAR fc_name[64], fc_mid[64], fc_paren[80], fc_full[IWD_FORECAST_CAP];
    bool fc_plain;
} StatTexts;

static void stat_texts(const Live *L, StatTexts *T)
{
    memset(T, 0, sizeof *T);
    if (L->intro) {
        lstrcpyW(T->wind, L"\x2014"); lstrcpyW(T->sev, L"\x2014"); lstrcpyW(T->light, L"\x2014");
        lstrcpyW(T->fc_name, L"Not rolled yet");
        T->fc_plain = true;
        T->light_col = C_SOFT;
        return;
    }
    {
        const IwdWind *wd = iwd_wind(L->ev->wind);
        int sv = iwd_severity(L->ev->id);
        char b[IWD_LIGHT_LABEL_CAP + 24];
        to_w(wd ? wd->label : "", T->wind, 32);
        T->wind_lit = iwd_wind_meter(L->ev->wind);
        to_w(iwd_severity_label(sv), T->sev, 32);
        T->sev_lit = sv;
        T->lethal = sv >= 5;
        iwd_fmt_light_value(L->ev, &L->light, b, sizeof b);
        to_w(b, T->light, IWD_LIGHT_LABEL_CAP + 24);
        T->light_col = light_rgb(L->light.level);
    }
    {
        IwdForecast f = iwd_forecast(L->day, L->hour);
        char b[IWD_FORECAST_CAP], *paren;
        size_t nl = 0;
        iwd_fmt_forecast(&f, b, sizeof b, &nl);
        to_w(b, T->fc_full, IWD_FORECAST_CAP);
        if (nl >= sizeof b) nl = 0;
        paren = strstr(b + nl, " (");
        if (paren) {
            /* the dice bookkeeping belongs to the ledger (and the tooltip) */
            char *dot = strstr(paren, " Â· ");
            if (dot) { dot[0] = ')'; dot[1] = 0; }
            to_w(paren, T->fc_paren, 80);
            *paren = 0;
        }
        to_w(b + nl, T->fc_mid, 64);
        b[nl] = 0;
        to_w(b, T->fc_name, 64);
    }
}

static int fc_spans(const StatTexts *T, Span *sp)
{
    int n = 0;
    if (T->fc_plain) {
        sp[0].s = T->fc_name; sp[0].n = -1; sp[0].font = F_BODY; sp[0].flags = 0; sp[0].color = C_SOFT;
        return 1;
    }
    sp[n].s = T->fc_name; sp[n].n = -1; sp[n].font = F_BODY_B; sp[n].flags = 0; sp[n].color = C_BRIGHT; n++;
    sp[n].s = T->fc_mid;  sp[n].n = -1; sp[n].font = F_BODY;   sp[n].flags = 0; sp[n].color = C_ICE; n++;
    if (T->fc_paren[0]) { sp[n].s = T->fc_paren; sp[n].n = -1; sp[n].font = F_SMALL; sp[n].flags = 0; sp[n].color = C_MUTED; n++; }
    return n;
}

static int stat_lab_h(void) { return fh(F_CAPS_S) + dp(2); }
static int stats_row1_h(void) { return stat_lab_h() + fh(F_VALUE) + dp(5) + dp(4); }

static int stats_layout(int x, int y, int w, bool draw)
{
    Live L; StatTexts T; Span sp[3];
    int ip = ST_PAD, x0 = x + ip, iw = w - 2 * ip, yy = y + ip, half = (iw - dp(16)) / 2, nsp, hh;
    live_get(&L);
    stat_texts(&L, &T);

    if (draw) {
        int xs = x0 + half + dp(16);
        caps_label(x0, yy, L"Wind", C_MUTED);
        tx_ellipsis(F_VALUE, x0, yy + stat_lab_h(), half, T.wind, L.intro ? C_DIM : C_BRIGHT);
        meter(x0, yy + stat_lab_h() + fh(F_VALUE) + dp(5), 4, T.wind_lit, C_ACCENT);
        caps_label(xs, yy, L"Severity", C_MUTED);
        tx_ellipsis(F_VALUE, xs, yy + stat_lab_h(), half, T.sev, L.intro ? C_DIM : T.lethal ? C_WARN : C_BRIGHT);
        meter(xs, yy + stat_lab_h() + fh(F_VALUE) + dp(5), 5, T.sev_lit, T.lethal ? C_WARN : C_ACCENT);
    }
    yy += stats_row1_h() + dp(7);
    if (draw) px_fill(x0, yy, x0 + iw, yy + 1, C_LINE);
    yy += dp(7);

    if (draw) caps_label(x0, yy, L"Natural light", C_MUTED);
    yy += stat_lab_h();
    hh = wrap(F_BODY_B, x0, yy, iw, T.light, T.light_col, draw);
    yy += hh + dp(7);
    if (draw) px_fill(x0, yy, x0 + iw, yy + 1, C_LINE);
    yy += dp(7);

    if (draw) caps_label(x0, yy, L"Forecast", C_MUTED);
    yy += stat_lab_h();
    nsp = fc_spans(&T, sp);
    hh = rich(sp, nsp, x0, yy, iw, fh(F_BODY_B), draw);
    if (draw && T.fc_full[0]) hot_add(x0, yy, x0 + iw, yy + hh, H_TIP, 0, true, T.fc_full);
    yy += hh;
    return yy + ip - y;
}

/* ================================================================== EFFECTS */

#define FX_PAD dp(12)

typedef struct FxGroup { int first, count, h; IwdCat cat; } FxGroup;

static IwdHourEffects s_fx;          /* never copied by value (items point into it) */

static int fx_fill(const Live *L)
{
    if (L->intro) { s_fx.count = 0; return 0; }
    iwd_hour_effects(L->ev->id, L->hour, L->temp_f, g.a.celsius, &s_fx);
    if (s_fx.count > IWD_MAX_HOUR_EFFECTS) s_fx.count = IWD_MAX_HOUR_EFFECTS;
    return s_fx.count;
}

/* Two type sizes: 15 px when the hour's effects fit the viewport with it (the usual case,
 * read at arm's length), else the compact 13 px plan (Blizzard, White Out). */
static bool s_fx_big;

static int fx_item_font(IwdCat c)
{
    if (s_fx_big) return c == IWD_CAT_CHILL ? F_FX_B : F_FX;
    return c == IWD_CAT_CHILL ? F_SMALL_B : F_SMALL;
}
static int fx_bold_font(void) { return s_fx_big ? F_FX_B : F_SMALL_B; }
static int fx_indent(void) { return dp(10); }
static int fx_item_gap(void) { return s_fx_big ? dp(3) : dp(2); }
static int fx_group_gap(void) { return s_fx_big ? dp(8) : dp(6); }
static int fx_label_gap(void) { return dp(1); }

/* Item text without a leading repeat of its category label ("Cold table: ..."), with the
 * values a DM hunts for (DC n, Disadvantage, Blinded, impossible) as bold spans. */
#define FX_SP 24
#define IWD_EFFECT_TEXT_W 400
typedef struct FxText { WCHAR buf[IWD_EFFECT_TEXT_W]; Span sp[FX_SP]; int n; } FxText;

static bool word_at(const WCHAR *s, int i, const WCHAR *w)
{
    int k = 0;
    if (i > 0 && iswalpha(s[i - 1])) return false;
    for (; w[k]; k++) if (towlower(s[i + k]) != towlower(w[k])) return false;
    return !iswalpha(s[i + k]);
}

static void fx_text(const IwdEffect *e, FxText *T, DWORD col)
{
    const WCHAR *lab = wz(iwd_cat_label(e->cat));
    WCHAR *t = T->buf;
    int n, i, ll = lstrlenW(lab), start = 0, f = fx_item_font(e->cat), fb = fx_bold_font();
    DWORD cb = C_BRIGHT;
    to_w(e->text, t, IWD_EFFECT_TEXT_W);
    n = lstrlenW(t);
    if (ll > 0 && n > ll + 2 && t[ll] == L':' && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, t, ll, lab, ll) == CSTR_EQUAL) {
        int k = ll + 1;
        while (t[k] == L' ') k++;
        memmove(t, t + k, sizeof(WCHAR) * (size_t)(n - k + 1));
        n -= k;
        if (t[0] >= L'a' && t[0] <= L'z') t[0] = (WCHAR)(t[0] - 32);
    }
    T->n = 0;
    for (i = 0; i < n && T->n < FX_SP - 2;) {
        int e2 = -1;
        if (t[i] == L'D' && t[i + 1] == L'C' && t[i + 2] == L' ' && iswdigit(t[i + 3]) && (i == 0 || !iswalpha(t[i - 1]))) {
            e2 = i + 3;
            while (iswdigit(t[e2])) e2++;
        } else if (word_at(t, i, L"Disadvantage")) e2 = i + 12;
        else if (word_at(t, i, L"Blinded")) e2 = i + 7;
        else if (word_at(t, i, L"impossible")) e2 = i + 10;
        if (e2 < 0) { i++; continue; }
        while (t[e2] == L'.' || t[e2] == L',' || t[e2] == L';' || t[e2] == L')') e2++;   /* no break before it */
        if (i > start) {
            Span *sp = &T->sp[T->n++];
            sp->s = t + start; sp->n = i - start; sp->font = (BYTE)f; sp->flags = 0; sp->color = col;
        }
        {
            Span *sp = &T->sp[T->n++];
            sp->s = t + i; sp->n = e2 - i; sp->font = (BYTE)fb; sp->flags = 0; sp->color = cb;
        }
        i = start = e2;
    }
    if (start < n) {
        Span *sp = &T->sp[T->n++];
        sp->s = t + start; sp->n = n - start; sp->font = (BYTE)f; sp->flags = 0; sp->color = col;
    }
}

static int fx_item_h(const IwdEffect *e, int w, int x, int y, DWORD col, bool draw)
{
    static FxText T;
    int f = fx_item_font(e->cat), lh = fh(f) > fh(fx_bold_font()) ? fh(f) : fh(fx_bold_font());
    fx_text(e, &T, col);
    if (!T.n) return fh(f);
    return rich(T.sp, T.n, x, y, w, lh, draw);
}

/* consecutive runs of one category; heights for a column of width colw */
static int fx_groups(FxGroup *gr, int colw)
{
    int i, n = 0;
    for (i = 0; i < s_fx.count; i++) {
        IwdCat c = s_fx.items[i].cat;
        int th = fx_item_h(&s_fx.items[i], colw - fx_indent(), 0, 0, C_TEXT, false);
        if (n > 0 && gr[n - 1].cat == c) {
            gr[n - 1].count++;
            gr[n - 1].h += fx_item_gap() + th;
        } else {
            gr[n].first = i; gr[n].count = 1; gr[n].cat = c;
            gr[n].h = fh(F_CAPS_S) + fx_label_gap() + th;
            n++;
        }
    }
    return n;
}

/* columns needed for limit L (sequential packing) */
static int fx_pack(const FxGroup *gr, int n, int lim, int *col_of)
{
    int i, col = 0, cur = 0;
    for (i = 0; i < n; i++) {
        int add = (cur > 0 ? fx_group_gap() : 0) + gr[i].h;
        if (cur > 0 && cur + add > lim) { col++; cur = gr[i].h; }
        else cur += add;
        if (col_of) col_of[i] = col;
    }
    return n ? col + 1 : 0;
}

/* smallest column height that packs into at most c columns */
static int fx_min_height(const FxGroup *gr, int n, int c)
{
    int lo = 0, hi = 0, i;
    for (i = 0; i < n; i++) { if (gr[i].h > lo) lo = gr[i].h; hi += gr[i].h + fx_group_gap(); }
    if (hi < lo) hi = lo;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (fx_pack(gr, n, mid, NULL) <= c) hi = mid; else lo = mid + 1;
    }
    return lo;
}

static int fx_head_h(void) { return fh(F_CAPS) + dp(9); }

/* chooses the type size and the column count; returns the block height */
static int fx_plan(int w, int *cols_out, int *colw_out, int *colh_out, FxGroup *gr, int *ngr)
{
    int iw = w - 2 * FX_PAD, cg = dp(16), maxc, c, best_c = 1, best_h = INT_MAX, best_w = iw, pass;
    int fixed = FX_PAD + fx_head_h() + FX_PAD;
    maxc = clampi((iw + cg) / dp(196), 1, 3);   /* sub-columns >= ~180 px (min window: 2) */
    *ngr = 0;
    s_fx_big = false;
    if (s_fx.count == 0) {
        int ph = wrap(F_SMALL_I, 0, 0, iw, L"Effects of the current weather will appear here, hour by hour.", C_MUTED, false);
        *cols_out = 1; *colw_out = iw; *colh_out = ph;
        return fixed + ph;
    }
    /* pass 0: large type, accepted only if it fits; pass 1: compact (unchanged plan) */
    for (pass = 0; pass < 2; pass++) {
        bool fit = false;
        s_fx_big = pass == 0;
        best_c = 1; best_h = INT_MAX; best_w = iw;
        for (c = 1; c <= maxc; c++) {
            int colw = (iw - (c - 1) * cg) / c, n = fx_groups(gr, colw), hgt;
            if (c > n && c > 1) break;
            hgt = fx_min_height(gr, n, c);
            if (hgt < best_h) { best_h = hgt; best_c = c; best_w = colw; }
            if (fixed + hgt <= s_fx_avail) { best_h = hgt; best_c = c; best_w = colw; fit = true; break; }
        }
        if (fit || pass == 1) break;
    }
    *ngr = fx_groups(gr, best_w);
    *cols_out = best_c; *colw_out = best_w; *colh_out = best_h;
    return fixed + best_h;
}

static int effects_measure(int w)
{
    Live L; FxGroup gr[IWD_MAX_HOUR_EFFECTS]; int c, cw, ch, n;
    live_get(&L);
    fx_fill(&L);
    return fx_plan(w, &c, &cw, &ch, gr, &n);
}

static void effects_paint(int x, int y, int w, int h)
{
    Live L; FxGroup gr[IWD_MAX_HOUR_EFFECTS]; int col_of[IWD_MAX_HOUR_EFFECTS];
    int cols, colw, colh, n, i, x0 = x + FX_PAD, yy = y + FX_PAD, cg = dp(16);
    int cur_col = -1, cy = 0;
    WCHAR note[96];

    live_get(&L);
    fx_fill(&L);
    fx_plan(w, &cols, &colw, &colh, gr, &n);
    panel_bg(x, y, w, h);

    tx_caps(F_CAPS, x0, yy, L"Effects in play", C_SOFT);
    if (L.intro) lstrcpyW(note, L"no weather yet");
    else {
        WCHAR nm[64];
        to_w(L.ev->name, nm, 64);
        wsprintfW(note, L"%d in play \x00B7 %s", s_fx.count, nm);
    }
    {
        int cw = tw_caps(F_CAPS, L"Effects in play") + dp(14);
        int nx = x + w - FX_PAD - tw(F_SMALL, note, -1);
        if (nx < x0 + cw) nx = x0 + cw;
        tx_ellipsis(F_SMALL, nx, yy + (fh(F_CAPS) - fh(F_SMALL)) / 2, x + w - FX_PAD - nx, note, C_MUTED);
    }
    yy += fx_head_h();

    if (s_fx.count == 0) {
        wrap(F_SMALL_I, x0, yy, w - 2 * FX_PAD, L"Effects of the current weather will appear here, hour by hour.", C_MUTED, true);
        return;
    }
    fx_pack(gr, n, colh, col_of);
    for (i = 0; i < n; i++) {
        const FxGroup *G = &gr[i];
        DWORD col = cat_rgb(G->cat);
        int gx, gy, k, ty;
        bool chill = G->cat == IWD_CAT_CHILL;
        if (col_of[i] != cur_col) { cur_col = col_of[i]; cy = yy; }
        else cy += fx_group_gap();
        gx = x0 + cur_col * (colw + cg);
        gy = cy;
        if (gy < g_tgt->h && gy + G->h > 0) {
            if (chill) px_fill(gx, gy - dp(2), gx + colw, gy + G->h + dp(2), mix(C_PANEL, C_ACCENT, 20));
            px_fill(gx, gy + dp(1), gx + dp(2), gy + G->h, mix(C_PANEL, col, chill ? 230 : 150));
            tx_caps(F_CAPS_S, gx + fx_indent(), gy, wz(iwd_cat_label(G->cat)), mix(C_PANEL, col, 225));
            ty = gy + fh(F_CAPS_S) + fx_label_gap();
            for (k = 0; k < G->count; k++) {
                const IwdEffect *e = &s_fx.items[G->first + k];
                if (k > 0) ty += fx_item_gap();
                ty += fx_item_h(e, colw - fx_indent(), gx + fx_indent(), ty, chill ? C_BRIGHT : C_TEXT, true);
            }
        }
        cy += G->h;
    }
}

/* ================================================================== FOOT */

static const WCHAR *k_foot1 = L"Rules from Icewind Dale: Weather Events and Effects. Duration rolls decide the next event (shorter half improves, longer half shifts).";
static const WCHAR *k_foot_keys = L"Keys (F1 for all): \x2192 / Space next hour \x00B7 \x2190 previous hour \x00B7 N advance day \x00B7 Ctrl+Z step back \x00B7 PgUp / PgDn view day \x00B7 T today \x00B7 E new event \x00B7 O note \x00B7 P play \x00B7 U \x00B0" L"F/\x00B0" L"C \x00B7 M sound \x00B7 F2 settings";
static const WCHAR *k_foot_link = L"Campaign settings\x2026";

static int foot_layout(int x, int y, int w, bool draw)
{
    int yy = y + dp(4), lw = tw(F_SMALL, k_foot_link, -1), tw1 = w - lw - dp(24), h1;
    if (tw1 < dp(200)) tw1 = w;
    h1 = wrap(F_SMALL, x, yy, tw1, k_foot1, C_DIM, draw);
    if (draw) {
        int lx = tw1 == w ? x : x + w - lw, ly = tw1 == w ? yy + h1 + dp(4) : yy;
        bool hot = is_hot(H_FOOT_SETTINGS);
        tx(F_SMALL, lx, ly, k_foot_link, -1, hot ? C_BRIGHT : C_ACCENT);
        px_fill(lx, ly + fh(F_SMALL) - dp(1), lx + lw, ly + fh(F_SMALL), hot ? C_BRIGHT : mix(C_BG, C_ACCENT, 110));
        hot_add(lx - dp(2), ly - dp(2), lx + lw + dp(2), ly + fh(F_SMALL) + dp(2), H_FOOT_SETTINGS, 0, true, L"Open Settings (F2)");
    }
    yy += h1 + (tw1 == w ? fh(F_SMALL) + dp(4) : 0) + dp(6);
    yy += wrap(F_TINY, x, yy, w, k_foot_keys, C_DIM, draw);
    return yy - y + dp(4);
}

/* ================================================================== measure / paint dispatch */

static int blk_measure(int which, int w)
{
    switch (which) {
    case BLK_HERO:     return hero_min_h(w);
    case BLK_CONTROLS: return controls_measure(w);
    case BLK_STATS:    return stats_layout(0, 0, w, false);
    case BLK_EFFECTS:  return effects_measure(w);
    case BLK_FOOT:     return foot_layout(0, 0, w, false);
    case BLK_TIMELINE: case BLK_LEDGER:  return blk_time_measure(which, w);
    case BLK_CALENDAR: case BLK_DAY:     return blk_cal_measure(which, w);
    default: return 0;
    }
}

static void blk_paint(int which, int x, int y, int w, int h)
{
    bool vis = y < g_tgt->h && y + h > view_header_h();
    switch (which) {
    case BLK_HERO:     if (vis) hero_paint(x, y, w, h); break;
    case BLK_CONTROLS: if (vis) controls_paint(x, y, w, h); break;
    case BLK_STATS:    if (vis) { panel_bg(x, y, w, h); stats_layout(x, y, w, true); } break;
    case BLK_EFFECTS:  if (vis) effects_paint(x, y, w, h); break;
    case BLK_FOOT:     if (vis) foot_layout(x, y, w, true); break;
    case BLK_TIMELINE: case BLK_LEDGER:  blk_time_paint(which, x, y, w, h); break;
    case BLK_CALENDAR: case BLK_DAY:     blk_cal_paint(which, x, y, w, h); break;
    default: break;
    }
}

/* ================================================================== layout */

static void put(int which, int x, int y, int w, int h)
{
    SetRect(&s_blk[which], x, y, x + w, y + (h > 0 ? h : 0));
}

void view_layout(void)
{
    int W = client_w(), vh = view_h();
    int pad = dp(12), gap = dp(12), rm = dp(16);
    int xl = pad, total = W - rm - xl, y, yl, yr, h;
    Surface *prev = g_tgt;
    if (!g_tgt && g.back.dc) g_tgt = &g.back;       /* measure on the back buffer's DC */
    if (total < dp(200)) total = dp(200);
    g.wide = W >= dp(940);
    memset(s_blk, 0, sizeof s_blk);

    if (g.wide) {
        int wl = (total - gap) * 56 / 100, wr = total - gap - wl, xr = xl + wl + gap;
        int tl_h, hero_h, lim, hmin;
        /* right column: live controls, stats, effects (kept inside the viewport), ledger */
        yr = pad;
        h = blk_measure(BLK_CONTROLS, wr); put(BLK_CONTROLS, xr, yr, wr, h); yr += h + gap;
        h = blk_measure(BLK_STATS, wr);    put(BLK_STATS, xr, yr, wr, h);    yr += h + gap;
        s_fx_avail = vh - yr - pad;
        if (s_fx_avail < dp(180)) s_fx_avail = vh - 2 * pad;
        h = blk_measure(BLK_EFFECTS, wr);  put(BLK_EFFECTS, xr, yr, wr, h);  yr += h + gap;
        h = blk_measure(BLK_LEDGER, wr);   put(BLK_LEDGER, xr, yr, wr, h);   yr += h + gap;
        /* the selected day's panel under the ledger, beside the calendar (below the fold) */
        h = blk_measure(BLK_DAY, wr);      put(BLK_DAY, xr, yr, wr, h);      yr += h;
        /* left column: hero sized so hero + timeline fit the viewport */
        tl_h = blk_measure(BLK_TIMELINE, wl);
        hmin = blk_measure(BLK_HERO, wl);
        hero_h = clampi(wl * 42 / 100, dp(280), dp(400));
        lim = vh - 2 * pad - gap - tl_h;
        if (hero_h > lim) hero_h = lim > dp(240) ? lim : dp(240);
        if (hero_h < hmin) hero_h = hmin;
        yl = pad;
        put(BLK_HERO, xl, yl, wl, hero_h);      yl += hero_h + gap;
        put(BLK_TIMELINE, xl, yl, wl, tl_h);    yl += tl_h + gap;
        h = blk_measure(BLK_CALENDAR, wl); put(BLK_CALENDAR, xl, yl, wl, h); yl += h;
        y = (yl > yr ? yl : yr) + gap * 2;
    } else {
        int w = total, hero_h, hmin;
        y = pad;
        hmin = blk_measure(BLK_HERO, w);
        hero_h = clampi(w * 38 / 100, dp(240), dp(340));
        if (hero_h < hmin) hero_h = hmin;
        put(BLK_HERO, xl, y, w, hero_h); y += hero_h + gap;
        h = blk_measure(BLK_CONTROLS, w); put(BLK_CONTROLS, xl, y, w, h); y += h + gap;
        h = blk_measure(BLK_STATS, w);    put(BLK_STATS, xl, y, w, h);    y += h + gap;
        /* the rules in play before the timeline: they are what the DM reads out */
        s_fx_avail = vh - 2 * pad;
        h = blk_measure(BLK_EFFECTS, w);  put(BLK_EFFECTS, xl, y, w, h);  y += h + gap;
        h = blk_measure(BLK_TIMELINE, w); put(BLK_TIMELINE, xl, y, w, h); y += h + gap;
        h = blk_measure(BLK_CALENDAR, w); put(BLK_CALENDAR, xl, y, w, h); y += h + gap;
        h = blk_measure(BLK_DAY, w);      put(BLK_DAY, xl, y, w, h);      y += h + gap;
        h = blk_measure(BLK_LEDGER, w);   put(BLK_LEDGER, xl, y, w, h);   y += h;
        y += gap * 2;
    }
    h = blk_measure(BLK_FOOT, total);
    put(BLK_FOOT, xl, y, total, h);
    g.content_h = y + h + pad;
    g.scroll = clampi(g.scroll, 0, max_scroll());
    g.layout_dirty = false;
    g_tgt = prev;
}

/* ================================================================== header */

static void header_paint(int W)
{
    int hh = view_header_h(), x = dp(16), cy = hh / 2, ch = dp(26), cy0 = cy - ch / 2, cy1 = cy0 + ch;
    int right = W - dp(16), rx;
    px_fill(0, 0, W, hh, C_BG);
    px_fill(0, hh - 1, W, hh, C_LINE);
    hot_add(0, 0, W, hh, H_NONE, 0, false, NULL);           /* covers the page under the header */

    /* tools, right to left: Settings, Sound, F/C, Seed */
    rx = right;
    {
        const WCHAR *s = L"Settings";
        int w = tw(F_BTN, s, -1) + dp(20);
        button(rx - w, cy0, rx, cy1, H_SETTINGS, true, s, F_BTN, NULL, L"Settings (F2)");
        rx -= w + dp(8);
    }
    {
        const WCHAR *s = g.sound_missing ? L"No audio" : audio_is_enabled() ? L"Sound on" : L"Sound off";
        int w = tw(F_BTN, L"Sound off", -1) + dp(28);
        bool on = !g.sound_missing && audio_is_enabled(), hot = is_hot(H_SOUND);
        DWORD fill = hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2;
        px_box(rx - w, cy0, rx, cy1, fill, on ? mix(C_FRAME, C_ACCENT, 150) : hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME);
        /* level mark: a small filled dot when on, ring when off */
        if (on) aa_circle(rx - w + dp(11), cy, dp(3), C_ACCENT, 255);
        else    aa_ring(rx - w + dp(11), cy, dp(3), C_MUTED, 255, 1.0);
        aa_flush();
        tx(F_BTN, rx - w + dp(19), cy - fh(F_BTN) / 2, s, -1, g.sound_missing ? C_DIM : on ? C_BRIGHT : C_SOFT);
        hot_add(rx - w, cy0, rx, cy1, H_SOUND, 0, true, L"Howling wind ambience (M)");
        rx -= w + dp(8);
    }
    {
        int sw = tw(F_BTN, L"\x00B0" L"F", -1) + dp(18), w = 2 * sw, x0 = rx - w, i;
        px_box(x0, cy0, rx, cy1, C_FIELD, C_FRAME);
        for (i = 0; i < 2; i++) {
            bool act = (i == 1) == g.a.celsius, hot = is_hot(i ? H_UNIT_C : H_UNIT_F);
            int a0 = x0 + i * sw, a1 = a0 + sw;
            if (act) px_fill(a0 + 2, cy0 + 2, a1 - 2, cy1 - 2, mix(C_PANEL2, C_ACCENT, 70));
            else if (hot) px_fill(a0 + 2, cy0 + 2, a1 - 2, cy1 - 2, C_PANEL2);
            tx_center(F_BTN, (a0 + a1) / 2, cy - fh(F_BTN) / 2, i ? L"\x00B0" L"C" : L"\x00B0" L"F",
                      act ? C_BRIGHT : hot ? C_TEXT : C_MUTED);
            hot_add(a0, cy0, a1, cy1, i ? H_UNIT_C : H_UNIT_F, 0, true,
                    i ? L"Show temperatures in \x00B0" L"C (U)" : L"Show temperatures in \x00B0" L"F (U)");
        }
        rx = x0 - dp(8);
    }
    {
        const WCHAR *lab = L"SEED";
        WCHAR seed[96];
        int lw, vw, w, maxw, x0;
        bool hot = is_hot(H_SEED);
        to_w(g.a.seed ? g.a.seed : "", seed, 96);
        lw = tw(F_CAPS_S, lab, -1) + dp(1) * 4;
        vw = tw(F_MONO, seed, -1);
        maxw = rx - x - dp(260);
        if (maxw > dp(260)) maxw = dp(260);
        w = dp(10) + lw + dp(8) + vw + dp(10);
        if (w > maxw) w = maxw;
        if (w > lw + dp(48)) {
            x0 = rx - w;
            px_box(x0, cy0, rx, cy1, hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2, hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME);
            tx_caps(F_CAPS_S, x0 + dp(10), cy - fh(F_CAPS_S) / 2, lab, C_MUTED);
            tx_ellipsis(F_MONO, x0 + dp(10) + lw + dp(8), cy - fh(F_MONO) / 2, w - lw - dp(28), seed, C_ICE);
            hot_add(x0, cy0, rx, cy1, H_SEED, 0, true, L"Expedition seed (opens Settings)");
            rx = x0 - dp(8);
        }
    }
    {   /* keyboard shortcuts */
        int s = ch, x0 = rx - s;
        bool hot = is_hot(H_KEYS) || g.keys_on;
        px_box(x0, cy0, rx, cy1, hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2, hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME);
        tx_center(F_BTN, x0 + s / 2, cy - fh(F_BTN) / 2, L"?", hot ? C_BRIGHT : C_SOFT);
        hot_add(x0, cy0, rx, cy1, H_KEYS, 0, true, L"Keyboard shortcuts (F1)");
        rx = x0 - dp(8);
    }
    if (g.save_failed) {
        const WCHAR *t = L"Not saved";
        int w = tw(F_BTN, t, -1) + dp(20), x0 = rx - w;
        bool hot = is_hot(H_SAVE_FAIL);
        px_box(x0, cy0, rx, cy1, mix(C_PANEL2, C_WARN, hot ? 70 : 40), C_WARN);
        tx_center(F_BTN, x0 + w / 2, cy - fh(F_BTN) / 2, t, C_BRIGHT);
        hot_add(x0, cy0, rx, cy1, H_SAVE_FAIL, 0, true,
                L"The almanac could not be written to its file. Click to save a backup elsewhere.");
        rx = x0 - dp(8);
    }

    /* the hero is scrolled away: a one-line live readout replaces the title */
    if (alm_has_view(&g.a) && g.scroll > s_blk[BLK_HERO].bottom - dp(60)) {
        Live L; char b[96]; WCHAR wd[IWD_DATE_CAP], wt[32], wn[64], wtier[64];
        int cx = x, room, i;
        AlmHero hr;
        struct { const WCHAR *s; int f; DWORD c; } part[9];
        int np = 0;
        live_get(&L);
        if (!L.intro) {
            WCHAR clk[16];
            alm_hero(&g.a, &hr);
            iwd_format_date(hr.abs, true, b, sizeof b); to_w(b, wd, IWD_DATE_CAP);
            iwd_fmt_clock(g.a.time, b, sizeof b); to_w(b, clk, 16);
            to_w(L.ev->name, wn, 64);
            iwd_fmt_temp(L.temp_f, g.a.celsius, b, sizeof b); to_w(b, wt, 30);
            if (lstrlenW(wt) > 0 && wt[lstrlenW(wt) - 1] == 0x00B0) lstrcatW(wt, g.a.celsius ? L"C" : L"F");
            snprintf(b, sizeof b, "%s DC %d", L.cold.tier ? L.cold.tier->name : "", L.cold.dc); to_w(b, wtier, 64);
            part[np].s = wd;  part[np].f = F_BODY;   part[np++].c = C_SOFT;
            part[np].s = clk; part[np].f = F_BODY;   part[np++].c = C_SOFT;
            part[np].s = wn;  part[np].f = F_BODY_B; part[np++].c = C_BRIGHT;
            part[np].s = wt;  part[np].f = F_BODY_B; part[np++].c = C_BRIGHT;
            part[np].s = wtier; part[np].f = F_BODY_B; part[np++].c = C_BRIGHT;
            room = rx - dp(8);
            if (viewing_other_day() && room - cx > dp(160)) {
                pill(F_SMALL_B, cx, cy - pill_h(F_SMALL_B) / 2, wz(hr.tag), 0xFFC999, true, true);
                cx += pill(F_SMALL_B, 0, 0, wz(hr.tag), 0, false, false) + dp(8);
            }
            for (i = 0; i < np && cx < room; i++) {
                const WCHAR *sep = L"  \x00B7  ";
                if (i) { tx(F_BODY, cx, cy - fh(F_BODY) / 2, sep, -1, C_DIM); cx += tw(F_BODY, sep, -1); }
                if (cx >= room) break;
                cx += tx_ellipsis(part[i].f, cx, cy - fh(part[i].f) / 2, room - cx, part[i].s, part[i].c);
            }
            hot_add(x - dp(4), cy0, (cx < room ? cx : room) + dp(4), cy1, H_HEAD_NOW, 0, true, L"Back to the weather (Ctrl+Home)");
            return;
        }
    }

    /* title */
    {
        const WCHAR *t1 = L"Icewind Dale", *t2 = L"Almanac of the Everlasting Rime";
        int w1 = tw_caps(F_CAPS, t1), ty = cy - fh(F_CAPS) / 2;
        if (x + w1 < rx) {
            tx_caps(F_CAPS, x, ty, t1, C_ICE);
            if (x + w1 + dp(14) + tw(F_SMALL_I, t2, -1) < rx)
                tx(F_SMALL_I, x + w1 + dp(14), cy - fh(F_SMALL_I) / 2, t2, -1, C_MUTED);
        }
    }
}

/* ================================================================== toast */

static void toast_paint(int W, int H)
{
    int maxw = W - 2 * dp(24), pad = dp(14), bw, bh, x0, y0, iw;
    if (maxw > dp(620)) maxw = dp(620);
    if (maxw < dp(160)) return;
    iw = maxw - 2 * pad;
    if (g.toast_day) {
        char title[160]; AlmMark mk[8]; int n = 0, i, cw_sum = 0, rows = 1, cx;
        WCHAR wt[160];
        if (!alm_day_toast(&g.a, g.toast_abs, g.toast_lead, title, sizeof title, mk, 8, &n)) return;
        to_w(title, wt, 160);
        for (i = 0; i < n; i++) {
            int cw = chip(0, 0, &mk[i], iw, false);
            if (cw_sum > 0 && cw_sum + cw > iw) { rows++; cw_sum = 0; }
            cw_sum += cw + dp(6);
        }
        bw = tw(F_BODY_B, wt, -1);
        {
            int row_w = 0, cur = 0;
            for (i = 0; i < n; i++) {
                int cw = chip(0, 0, &mk[i], iw, false);
                if (cur > 0 && cur + cw > iw) { if (cur > row_w) row_w = cur; cur = 0; }
                cur += cw + dp(6);
            }
            if (cur > row_w) row_w = cur;
            if (row_w > bw) bw = row_w;
        }
        if (g.toast_hint[0]) { int hw = tw(F_SMALL, wz(g.toast_hint), -1); if (hw > bw) bw = hw; }
        if (bw > iw) bw = iw;
        bw += 2 * pad;
        bh = pad + fh(F_BODY_B) + (n ? dp(8) + rows * (chip_h() + dp(6)) - dp(6) : 0) + pad;
        if (g.toast_hint[0]) bh += dp(6) + fh(F_SMALL);
        x0 = (W - bw) / 2; y0 = H - dp(24) - bh;
        hot_add(x0, y0, x0 + bw, y0 + bh, H_NONE, 0, false, NULL);
        px_box(x0, y0, x0 + bw, y0 + bh, C_PANEL2, C_FRAME);
        px_fill(x0, y0, x0 + bw, y0 + dp(2), C_ACCENT);
        tx_ellipsis(F_BODY_B, x0 + pad, y0 + pad, bw - 2 * pad, wt, C_BRIGHT);
        cx = x0 + pad;
        {
            int cy = y0 + pad + fh(F_BODY_B) + dp(8);
            for (i = 0; i < n; i++) {
                int cw = chip(0, 0, &mk[i], iw, false);
                if (cx > x0 + pad && cx + cw > x0 + pad + iw) { cx = x0 + pad; cy += chip_h() + dp(6); }
                chip(cx, cy, &mk[i], iw, true);
                cx += cw + dp(6);
            }
            if (g.toast_hint[0])
                tx_ellipsis(F_SMALL, x0 + pad, y0 + bh - pad - fh(F_SMALL), bw - 2 * pad, wz(g.toast_hint), C_MUTED);
        }
        hot_add(x0, y0, x0 + bw, y0 + bh, H_TOAST, 0, true, NULL);
        return;
    }
    {
        int one = 0, th;
        th = markup(g.toast, 0, 0, iw, F_BODY, F_BODY_B, C_TEXT, C_BRIGHT, false, &one);
        if (th <= 0) return;
        bw = (one + dp(2) < iw ? one + dp(2) : iw) + 2 * pad;
        if (one + dp(2) < iw) th = markup(g.toast, 0, 0, one + dp(2), F_BODY, F_BODY_B, C_TEXT, C_BRIGHT, false, NULL);
        bh = th + 2 * pad;
        x0 = (W - bw) / 2; y0 = H - dp(24) - bh;
        px_box(x0, y0, x0 + bw, y0 + bh, C_PANEL2, C_FRAME);
        px_fill(x0, y0, x0 + bw, y0 + dp(2), C_ACCENT);
        markup(g.toast, x0 + pad, y0 + pad, bw - 2 * pad, F_BODY, F_BODY_B, C_TEXT, C_BRIGHT, true, NULL);
        hot_add(x0, y0, x0 + bw, y0 + bh, H_TOAST, 0, true, NULL);
    }
}

/* ================================================================== keys card */

static void keys_paint(int W, int H)
{
    static const WCHAR *const k[][2] = {
        { L"\x2192  Space", L"Next hour (midnight on today advances the day)" },
        { L"\x2190", L"Previous hour" },
        { L"Home  End", L"00:00 / 23:00" },
        { L"N", L"Advance the party to the next day" },
        { L"Ctrl+Z", L"Step back (undo the last advance or make-today)" },
        { L"PgUp  PgDn", L"View the previous / next day" },
        { L"T  Esc", L"Back to today" },
        { L"P", L"Play / pause the day" },
        { L"E", L"New event or reminder on the viewed day" },
        { L"O", L"Write a note for the viewed day" },
        { L"Tab", L"Calendar grid: arrows move, Enter views, Shift+Enter makes it today" },
        { L"U", L"\x00B0" L"F / \x00B0" L"C" },
        { L"M", L"Wind sound" },
        { L"F2", L"Settings" },
        { L"Ctrl+C", L"Copy the seed" },
        { L"F1  ?", L"This card" },
    };
    int n = (int)(sizeof k / sizeof k[0]), i, pad = dp(18), rowh = fh(F_SMALL) + dp(8);
    int kw = 0, aw = 0, cols, rows, bw, bh, x0, y0, colw;
    for (i = 0; i < n; i++) {
        int a = tw(F_TINY_B, k[i][0], -1) + dp(10), b = tw(F_SMALL, k[i][1], -1);
        if (a > kw) kw = a;
        if (b > aw) aw = b;
    }
    colw = kw + dp(12) + aw;
    cols = (W - 2 * dp(24) - 2 * pad + dp(28)) / (colw + dp(28)) >= 2 ? 2 : 1;
    if (cols == 1 && colw > W - 2 * dp(24) - 2 * pad) colw = W - 2 * dp(24) - 2 * pad;
    rows = (n + cols - 1) / cols;
    bw = 2 * pad + cols * colw + (cols - 1) * dp(28);
    bh = pad + fh(F_BODY_B) + dp(12) + rows * rowh + dp(4) + fh(F_TINY) + pad;
    x0 = (W - bw) / 2; y0 = (H - bh) / 2;
    if (y0 < view_header_h() + dp(8)) y0 = view_header_h() + dp(8);
    px_blend(0, view_header_h(), W, H, C_SHADOW, 120);
    px_box(x0, y0, x0 + bw, y0 + bh, C_PANEL2, C_FRAME);
    px_fill(x0, y0, x0 + bw, y0 + dp(2), C_ACCENT);
    tx(F_BODY_B, x0 + pad, y0 + pad, L"Keyboard shortcuts", -1, C_BRIGHT);
    for (i = 0; i < n; i++) {
        int c = i / rows, r = i % rows;
        int cx = x0 + pad + c * (colw + dp(28)), cy = y0 + pad + fh(F_BODY_B) + dp(12) + r * rowh;
        int kx = cx;
        const WCHAR *ks = k[i][0];
        /* each key of the cell in its own cap */
        {
            WCHAR one[24]; int a = 0, b;
            while (ks[a]) {
                b = a;
                while (ks[b] && !(ks[b] == L' ' && ks[b + 1] == L' ')) b++;
                lstrcpynW(one, ks + a, b - a + 1 < 24 ? b - a + 1 : 24);
                kx += keycap(kx, cy + (rowh - fh(F_TINY_B)) / 2 - dp(2), one, C_FRAME, C_TEXT, true) + dp(4);
                a = b; while (ks[a] == L' ') a++;
            }
        }
        tx_ellipsis(F_SMALL, cx + kw + dp(12), cy + (rowh - fh(F_SMALL)) / 2 - dp(1), colw - kw - dp(12), k[i][1], C_TEXT);
    }
    tx(F_TINY, x0 + pad, y0 + bh - pad - fh(F_TINY), L"Right-click a calendar day for more \x00B7 any key or click closes this card", -1, C_MUTED);
    hot_add(0, view_header_h(), W, H, H_KEYS_CARD, 0, true, NULL);
}

/* ================================================================== scrollbar */

static void scrollbar_paint(int W, int H)
{
    int hh = view_header_h(), vh = H - hh, ms = max_scroll();
    int track_top = hh + dp(4), track_h = vh - dp(8), x1 = W - dp(3), bw, x0, ty;
    bool act;
    s_sb_on = false;
    if (ms <= 0 || track_h < dp(40)) return;
    s_th_h = (int)((long long)track_h * vh / (g.content_h > 0 ? g.content_h : 1));
    if (s_th_h < dp(28)) s_th_h = dp(28);
    if (s_th_h > track_h) s_th_h = track_h;
    ty = track_top + (int)((long long)(track_h - s_th_h) * g.scroll / ms);
    s_sb_top = track_top; s_sb_h = track_h; s_th_y = ty; s_sb_on = true;
    act = g.sb_drag || is_hot(H_SCROLL);
    bw = act ? dp(7) : dp(4);
    x0 = x1 - bw;
    px_blend(x0, track_top, x1, track_top + track_h, C_FRAME, act ? 70 : 30);
    px_fill(x0, ty, x1, ty + s_th_h, act ? C_MUTED : C_FRAME);
    /* hit regions: generous width at the window edge */
    hot_add(W - dp(14), track_top, W, ty, H_SCROLL, -1, true, NULL);
    hot_add(W - dp(14), ty + s_th_h, W, track_top + track_h, H_SCROLL, 1, true, NULL);
    hot_add(W - dp(14), ty, W, ty + s_th_h, H_SCROLL, 0, true, NULL);
}

/* ================================================================== paint */

void view_paint(void)
{
    int W, H, i;
    if (!g.back.dc || !g.back.px) return;
    g_tgt = &g.back;
    g_hl = &g.hl;
    g.hl.n = 0;
    W = g.back.w; H = g.back.h;

    view_layout();
    g_tgt = &g.back;

    px_fill(0, 0, W, H, C_BG);
    for (i = 0; i < BLK_COUNT; i++) {
        RECT r = s_blk[i];
        int h = r.bottom - r.top, w = r.right - r.left;
        if (w <= 0 || h <= 0) continue;
        blk_paint(i, r.left, screen_y(r.top), w, h);
    }
    /* blocks may have drawn into the header strip; the header covers it */
    header_paint(W);
    scrollbar_paint(W, H);
    if (g.toast_on) toast_paint(W, H);
    if (g.keys_on) keys_paint(W, H);
    aa_flush();
    GdiFlush();
    g.fade_req = false;
}

void view_free(void)
{
    free(s_prev); s_prev = NULL; s_pw = s_ph = 0; s_prev_id = -1;
    blk_time_free();
    blk_cal_free();
    memset(s_blk, 0, sizeof s_blk);
    s_sb_on = false;
}
