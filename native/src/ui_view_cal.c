/* ui_view_cal.c - the "Calendar of Harptos" block (month head, 30-day grid in three tendays,
 * festival cells, Upcoming) and the selected-day block (make-today, events & reminders with
 * the inline event form, notes), SPEC.md 8.1-8.3, plus view_cells() from ui.h.
 * The day panel repeats nothing the hero, the timeline or the calendar already show: its
 * date appears only when the selected day is not the day on screen.
 *
 * Painting model: everything is laid out by one function per block that either measures
 * (draw = false) or paints (draw = true) along the same path, so blk_cal_measure and
 * blk_cal_paint can never disagree. Nothing is cached here except the per-DPI width of the
 * month title column and the rendered calendar (cal_cached); the weather marks in the cells
 * come from thumb_get() (ui_draw.c), always requested at one size per layout so its
 * single-size cache never thrashes.
 *
 * Hit regions registered here (back-buffer coordinates, already scrolled):
 *   H_CAL_PREV     arg 0      previous month (alm_cal_step(-1))
 *   H_CAL_NEXT     arg 0      next month (alm_cal_step(+1))
 *   H_CAL_TODAY    arg 0      alm_cal_today; tip "Go to the current day (T)" /
 *                             "Go to the expedition start (T)"
 *   H_CAL_CELL     arg k      cell index k into view_cells() (0..29 month days, 30.. the
 *                             festivals, the same order as alm_month_cells).
 *                             Click = alm_select(cells[k].abs, true); double click =
 *                             alm_set_current(cells[k].abs). Tip = alm_fmt_cell_label.
 *   H_UPCOMING     arg abs    an Upcoming row: alm_select(abs, false)
 *   H_SET_CURRENT  arg 0      "Make this day today": alm_set_current(g.a.sel_abs)
 *   H_EV_EDIT      arg i      index i into alm_marks_on(&g.a, g.a.sel_abs, ...) (only user
 *                             events get the button): ui_open_form(marks[i].event)
 *   H_EV_DEL       arg i      same index: two-step delete. Drawn armed (red, tip "Press
 *                             again to delete") while strcmp(g.armed_id, event->id) == 0;
 *                             main.c arms (copies the id, 3 s timer) / deletes.
 *   H_ADD_EVENT    arg 0      "Add" in the events label row: ui_open_form(NULL) (hidden
 *                             while the form is open)
 *   H_FORM_TYPE    arg t      AlmEvType 0..3 radio choice -> g.form_type = t
 *   H_FORM_REPEAT  arg 0      the Repeat select: main.c pops ui_popup_choice at the hit rect
 *                             with iwd_repeat_label(0..5), selection g.form_repeat
 *   H_FORM_CANCEL  arg 0      ui_close_form()
 *   H_FORM_SAVE    arg 0      ui_form_submit()
 *   H_NOTES        arg 0      the notes box (ui_notes_begin: create / focus the EDIT)
 *
 * EDIT placement: while painting BLK_DAY this file sets g.rc_title / g.rc_details (only when
 * g.form_open, else empty) and g.rc_notes, in content coordinates (x as in the back buffer,
 * y through view_content_y), as the inner text area of the drawn field frames. When an EDIT
 * does not exist the field's content (note text, or the placeholder) is drawn here.
 */
#include "ui.h"
#include "ui_blocks.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== small helpers */

static int w8(const char *s, WCHAR *out, int cap)
{
    int n;
    if (cap <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

static int px1(int v) { int d = dp(v); return d < 1 ? 1 : d; }

static bool vis(int y0, int y1) { return g_tgt && y1 > 0 && y0 < g_tgt->h; }

static DWORD type_rgb(AlmEvType t) { return (DWORD)alm_ev_type_rgb(t) & 0xFFFFFF; }

/* text clipped and wrapped into a rectangle (DrawText clips without DT_NOCLIP) */
static void tx_box(int f, int x0, int y0, int x1, int y1, const WCHAR *s, DWORD c)
{
    RECT r;
    if (!g_tgt || !s || !*s || x1 <= x0 || y1 <= y0) return;
    SetRect(&r, x0, y0, x1, y1);
    SelectObject(g_tgt->dc, font(f));
    SetTextColor(g_tgt->dc, RGB_OF(c));
    DrawTextW(g_tgt->dc, s, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT | DT_TOP);
}

static void chevron(int cx, int cy, bool left, DWORD c)
{
    double s = dp(4), lw = dp(16) / 10.0;
    if (left) {
        aa_line(cx + s * 0.5, cy - s, cx - s * 0.5, cy, c, 255, lw);
        aa_line(cx - s * 0.5, cy, cx + s * 0.5, cy + s, c, 255, lw);
    } else {
        aa_line(cx - s * 0.5, cy - s, cx + s * 0.5, cy, c, 255, lw);
        aa_line(cx + s * 0.5, cy, cx - s * 0.5, cy + s, c, 255, lw);
    }
    aa_flush();
}

static void chevron_down(int cx, int cy, DWORD c)
{
    double s = dp(4), lw = dp(15) / 10.0;
    aa_line(cx - s, cy - s * 0.5, cx, cy + s * 0.5, c, 255, lw);
    aa_line(cx, cy + s * 0.5, cx + s, cy - s * 0.5, c, 255, lw);
    aa_flush();
}

/* minimal page mark for "has notes": an outlined sheet with two text lines */
static void note_mark(int x, int y, DWORD c)
{
    int w = dp(7), h = dp(9);
    px_frame(x, y, x + w, y + h, 1, c);
    px_fill(x + dp(2), y + dp(3), x + w - dp(2), y + dp(3) + 1, c);
    px_fill(x + dp(2), y + dp(5) + 1, x + w - dp(2), y + dp(5) + 2, c);
}

enum { BTS_GHOST = 0, BTS_SOLID, BTS_DANGER };
enum { BG_NONE = 0, BG_LEFT, BG_RIGHT };

static bool is_hot(int id, int arg) { return g.hot_id == id && g.hot_arg == arg; }

static void btn(int x0, int y0, int x1, int y1, int id, int arg, bool en, const WCHAR *label,
                const WCHAR *tip, int style, int glyph)
{
    bool hot = en && is_hot(id, arg), press = hot && g.press_id == id && g.press_arg == arg;
    DWORD fill, border, ink;
    if (style == BTS_SOLID) {
        /* primary: the Advance button's recipe, no filled pastel */
        fill = press ? C_FIELD : mix(C_PANEL2, C_ACCENT, hot ? 46 : 14);
        border = C_ACCENT;
        ink = C_BRIGHT;
    } else if (style == BTS_DANGER) {
        fill = hot ? mix(C_PANEL2, C_DANGER, 90) : mix(C_PANEL2, C_DANGER, 60);
        border = C_DANGER;
        ink = C_BRIGHT;
    } else {
        fill = !en ? mix(C_PANEL, C_PANEL2, 120) : press ? C_FIELD : hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2;
        border = !en ? C_LINE : hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME;
        ink = en ? C_BRIGHT : C_DIM;
    }
    if (!en && style == BTS_SOLID) { fill = mix(C_PANEL, C_PANEL2, 120); border = C_LINE; ink = C_DIM; }
    px_box(x0, y0, x1, y1, fill, border);
    if (glyph) chevron((x0 + x1) / 2, (y0 + y1) / 2, glyph == BG_LEFT, ink);
    if (label && *label) {
        int lw = tw(F_BTN, label, -1), room = x1 - x0 - dp(12);
        int ty = (y0 + y1 - fh(F_BTN)) / 2;
        if (lw <= room) tx(F_BTN, (x0 + x1 - lw) / 2, ty, label, -1, ink);
        else tx_ellipsis(F_BTN, x0 + dp(6), ty, room, label, ink);
    }
    hot_add(x0, y0, x1, y1, id, arg, en, tip);
}

static int btn_w(const WCHAR *label) { return tw(F_BTN, label, -1) + dp(26); }
static int btn_h(void) { return fh(F_BTN) + dp(12); }

/* ================================================================== chips */

/* Mark chip: thin type-colour rule on the left, title text; the built-in new-moon mark gets
 * a frost flake. Returns the chip width for the given max width. */
static int chip_width(const AlmMark *m, const char *text, int f, int maxw)
{
    int w = dp(3) + dp(6) + tw(f, wz(text), -1) + dp(8);
    if (m->builtin) w += dp(12);
    return w > maxw ? maxw : w;
}

static void chip_draw(const AlmMark *m, const char *text, int f, int x, int y, int w, int h)
{
    DWORD col = type_rgb(m->type);
    int tx0 = x + dp(3) + dp(6);
    px_box(x, y, x + w, y + h, mix(C_PANEL, col, 22), mix(C_PANEL, col, 80));
    px_fill(x, y, x + px1(3), y + h, col);
    if (m->builtin) {
        aa_flake(tx0 + dp(4), y + h / 2.0, dp(4), col, 230);
        aa_flush();
        tx0 += dp(12);
    }
    tx_ellipsis(f, tx0, y + (h - fh(f)) / 2, x + w - dp(6) - tx0, wz(text), C_TEXT);
}

/* ================================================================== calendar geometry */

static int s_title_w, s_title_dpi;

static int month_title_w(void)
{
    int i;
    if (s_title_w && s_title_dpi == g.dpi) return s_title_w;
    s_title_w = 0;
    for (i = 0; i < 12; i++) {
        const IwdMonth *m = iwd_month(i);
        int a = tw(F_MONTH, wz(m->name), -1), b = tw(F_SMALL_I, wz(m->common), -1);
        if (a > s_title_w) s_title_w = a;
        if (b > s_title_w) s_title_w = b;
    }
    s_title_dpi = g.dpi;
    return s_title_w;
}

typedef struct CalGeom {
    int pad, gap, rgap;
    int cw, ch, fw, fch;           /* day cell, festival cell */
    int thumb_w, thumb_h;
    bool text_chips;
} CalGeom;

static void cal_geom(int w, CalGeom *G)
{
    int inner;
    G->pad = dp(14);
    G->gap = dp(5);
    G->rgap = dp(9);
    inner = w - 2 * G->pad;
    G->cw = (inner - 9 * G->gap) / 10;
    if (G->cw < dp(24)) G->cw = dp(24);
    G->text_chips = G->cw >= dp(54);
    G->thumb_w = G->cw - 2 * dp(4);
    if (G->thumb_w < 4) G->thumb_w = 4;
    G->thumb_h = dp(18);
    G->ch = dp(5) + fh(F_SMALL_B) + dp(3) + G->thumb_h + dp(4) +
            (G->text_chips ? 2 * (fh(F_TINY) + dp(1)) : 2 * dp(5)) + dp(5);
    G->fw = 2 * G->cw + G->gap;
    if (G->fw < dp(120) && 4 * G->cw + 3 * G->gap >= dp(120)) G->fw = 3 * G->cw + 2 * G->gap;
    G->fch = G->ch + fh(F_CAPS_S) + dp(2);
}

/* ================================================================== span lanes */

/* A multi-day event reads as one bar across the cells of a tenday row: every event shown
 * in a row gets one lane (its vertical slot) for all the cells it covers there, so a span
 * keeps its slot from cell to cell. Lanes are handed out to spans first (longer runs first),
 * then to one-day events in eventsOn order (type priority); the cell shows lanes 0 and 1,
 * the rest count as "+N". The title is drawn once per row, on the run's first cell, across the whole run;
 * a span that goes on beyond the row (before or after) gets a small chevron at that end. */
#define LANE_VIS 2
#define LANE_MAX 64
typedef struct LaneSeg {
    const AlmEvent *e;
    int  span_day, span_len;
    bool join_l, join_r;          /* the same occurrence continues in the cell left / right */
    bool cont_l, cont_r;          /* ... and beyond this row's edge (or into a festival) */
    bool title;                   /* first cell of the run in this row */
    int  run_x1;                  /* right end of the run (for the title), cell-relative to x0 */
} LaneSeg;
typedef struct CellLanes { LaneSeg lane[LANE_VIS]; int more; } CellLanes;
static const CellLanes *s_lanes;  /* lanes of the cell being painted (NULL: none) */

typedef struct LaneItem {
    const AlmEvent *e;
    int32_t occ;
    int first, last, day0, prio, lane;
} LaneItem;

static int cmp_lane_item(const void *pa, const void *pb)
{
    const LaneItem *a = (const LaneItem *)pa, *b = (const LaneItem *)pb;
    int la = a->last - a->first, lb = b->last - b->first;
    bool sa = a->e->days > 1, sb = b->e->days > 1;
    if (sa != sb) return sa ? -1 : 1;                   /* spans take the top lanes */
    if (la != lb) return la > lb ? -1 : 1;              /* longer runs first */
    if (a->prio != b->prio) return a->prio < b->prio ? -1 : 1;
    if (a->first != b->first) return a->first < b->first ? -1 : 1;
    if (a->occ != b->occ) return a->occ < b->occ ? -1 : 1;
    return a->e < b->e ? -1 : a->e > b->e;
}

/* cells: m consecutive days (m <= 10) at x positions xs[] with width cw */
static void lanes_for_row(const AlmCell *cells, int m, const int *xs, int cw, CellLanes *out)
{
    static LaneItem it[10 * LANE_MAX];
    static bool used[10][LANE_MAX];
    int ni = 0, c, i, L;
    memset(out, 0, (size_t)m * sizeof *out);
    memset(used, 0, sizeof used);
    for (c = 0; c < m; c++) {
        const AlmEvent *evs[LANE_MAX];
        int n = alm_events_on(&g.a, cells[c].abs, evs, LANE_MAX), j;
        if (n > LANE_MAX) n = LANE_MAX;
        for (j = 0; j < n; j++) {
            int32_t occ = cells[c].abs;
            int k = evs[j]->days > 1 ? alm_event_span_day(evs[j], cells[c].abs, g.a.moon_offset, &occ) : 0;
            if (k < 0) { k = 0; occ = cells[c].abs; }
            for (i = 0; i < ni; i++)
                if (it[i].e == evs[j] && it[i].occ == occ && it[i].last == c - 1) break;
            if (i < ni && evs[j]->days > 1) { it[i].last = c; continue; }
            if (ni >= (int)(sizeof it / sizeof it[0])) continue;
            it[ni].e = evs[j]; it[ni].occ = occ; it[ni].first = it[ni].last = c; it[ni].day0 = k;
            it[ni].prio = alm_ev_type_priority(evs[j]->type); it[ni].lane = -1;
            ni++;
        }
    }
    if (ni > 1) qsort(it, (size_t)ni, sizeof *it, cmp_lane_item);
    for (i = 0; i < ni; i++) {
        for (L = 0; L < LANE_MAX; L++) {
            bool free_ = true;
            for (c = it[i].first; c <= it[i].last; c++) if (used[c][L]) { free_ = false; break; }
            if (free_) break;
        }
        if (L >= LANE_MAX) continue;
        it[i].lane = L;
        for (c = it[i].first; c <= it[i].last; c++) used[c][L] = true;
    }
    for (i = 0; i < ni; i++) {
        const LaneItem *q = &it[i];
        int len = q->e->days > 1 ? q->e->days : 1;
        for (c = q->first; c <= q->last; c++) {
            LaneSeg *sg;
            if (q->lane < 0 || q->lane >= LANE_VIS) { out[c].more++; continue; }
            sg = &out[c].lane[q->lane];
            sg->e = q->e;
            sg->span_len = len;
            sg->span_day = q->day0 + (c - q->first);
            sg->join_l = c > q->first;
            sg->join_r = c < q->last;
            sg->cont_l = c == q->first && sg->span_day > 0;
            sg->cont_r = c == q->last && sg->span_day < len - 1;
            sg->title = c == q->first;
            sg->run_x1 = xs[q->last] + cw - xs[c];
        }
    }
}

/* tiny chevron for a span that goes on beyond the row */
static void cont_mark(int cx, int cy, bool left, DWORD c)
{
    double s = dp(3), lw = dp(13) / 10.0;
    if (left) {
        aa_line(cx + s * 0.4, cy - s, cx - s * 0.4, cy, c, 255, lw);
        aa_line(cx - s * 0.4, cy, cx + s * 0.4, cy + s, c, 255, lw);
    } else {
        aa_line(cx - s * 0.4, cy - s, cx + s * 0.4, cy, c, 255, lw);
        aa_line(cx + s * 0.4, cy, cx - s * 0.4, cy + s, c, 255, lw);
    }
    aa_flush();
}

/* ================================================================== one calendar cell */

static int tag_pill_w(bool today)
{
    return tw_caps(F_CAPS_S, today ? L"Today" : L"Start") + dp(10);
}

/* the "Today"/"Start" notch: centred on cx, its bottom ybot (it sits on the cell's top
   edge, almost entirely in the gap above, so it never covers the day number) */
static void tag_pill(int cx, int ybot, bool today)
{
    const WCHAR *s = today ? L"Today" : L"Start";
    int w = tag_pill_w(today), h = fh(F_CAPS_S) + dp(2);
    int x0 = cx - w / 2, y0 = ybot - h;
    if (today) {
        px_box(x0, y0, x0 + w, y0 + h, C_ICE, C_BRIGHT);
        tx_caps(F_CAPS_S, x0 + dp(5), y0 + dp(1), s, 0x0B1828);
    } else {
        px_box(x0, y0, x0 + w, y0 + h, mix(C_PANEL, C_AURORA, 70), C_AURORA);
        tx_caps(F_CAPS_S, x0 + dp(5), y0 + dp(1), s, C_AURORA);
    }
}

static bool s_fest_cell;              /* cell_paint called by fest_paint: it draws the tag */

static void cell_paint(const AlmCell *c, int k, int x0, int y0, int x1, int y1, const CalGeom *G)
{
    bool hot = is_hot(H_CAL_CELL, k);
    DWORD fill = C_PANEL2, border = hot ? C_FRAME : C_LINE;
    int p = dp(4), yy = y0 + dp(5), x, i, th = px1(2);
    WCHAR tip[200]; char lab[512];

    if (c->is_sel) { fill = mix(C_PANEL2, C_ACCENT, hot ? 54 : 42); border = C_ACCENT; }
    else if (hot) fill = mix(C_PANEL2, C_ACCENT, 18);
    px_box(x0, y0, x1, y1, fill, border);
    if (c->is_new_moon) px_vgrad_a(x0 + 1, y0 + 1, x1 - 1, y0 + dp(16), 0xBFE6FF, 46, 0);

    /* head: number (or festival name), marks, moon disc */
    {
        int mr = dp(5), mx = x1 - p - mr, hy = yy + fh(F_SMALL_B) / 2;
        int nx = x0 + p + (c->is_start ? dp(2) : 0), nw;
        DWORD ink = c->is_today ? C_BRIGHT : c->is_sel ? C_ICE : C_TEXT;
        if (c->is_festival) {
            nw = tx_ellipsis(F_TINY_B, nx, yy + (fh(F_SMALL_B) - fh(F_TINY_B)) / 2,
                             mx - mr - dp(4) - nx - (c->is_new_moon ? dp(11) : 0) - (c->has_note ? dp(10) : 0),
                             wz(c->festival_name), ink);
        } else {
            WCHAR num[8];
            wsprintfW(num, L"%d", c->date.day);
            tx(F_SMALL_B, nx, yy, num, -1, ink);
            nw = tw(F_SMALL_B, num, -1);
        }
        x = nx + nw + dp(4);
        if (c->is_new_moon && x + dp(9) < mx - mr) {
            aa_flake(x + dp(4), hy, dp(4), type_rgb(ALM_EV_AURIL), 235);
            aa_flush();
            x += dp(11);
        }
        if (c->has_note && x + dp(7) < mx - mr) { note_mark(x, hy - dp(4), C_SOFT); x += dp(10); }
        if (s_lanes && s_lanes->more > 0) {    /* events beyond the two lanes: "+N" by the moon */
            WCHAR mw[12];
            int ww;
            wsprintfW(mw, L"+%d", s_lanes->more);
            ww = tw(F_TINY_B, mw, -1);
            if (mx - mr - dp(4) - ww >= x) tx(F_TINY_B, mx - mr - dp(4) - ww, hy - fh(F_TINY_B) / 2, mw, -1, C_SOFT);
        }
        if (nx + nw + dp(2) <= mx - mr) {      /* narrow cells: the number wins over the disc */
            aa_moon(mx, hy, mr, &c->moon);
            aa_flush();
        }
    }
    yy += fh(F_SMALL_B) + dp(3);

    /* weather: a crop of the dominant event's scene + its colour rule + low temperature */
    if (c->has_weather) {
        const DWORD *t = thumb_get(c->dominant_id, G->thumb_w, G->thumb_h);
        int tx0 = x0 + p, ty1 = yy + G->thumb_h;
        char tb[24]; WCHAR wt[24];
        if (t) px_copy(t, G->thumb_w, G->thumb_h, tx0, yy);
        else px_fill(tx0, yy, tx0 + G->thumb_w, ty1, mix(C_PANEL2, iwd_event_rgb(c->dominant_id), 90));
        px_fill(tx0, ty1 - th, tx0 + G->thumb_w, ty1, iwd_event_rgb(c->dominant_id) & 0xFFFFFF);
        iwd_fmt_temp(c->temp_min_f, g.a.celsius, tb, sizeof tb);
        if (G->cw >= dp(84)) { char lb[32]; snprintf(lb, sizeof lb, "low %s", tb); lstrcpynA(tb, lb, (int)sizeof tb); }
        w8(tb, wt, 24);
        {
            int ww = tw(F_TINY_B, wt, -1), bx1, bx0;
            if (ww > G->thumb_w - dp(4)) {     /* too narrow: drop the degree sign */
                int L = lstrlenW(wt);
                if (L > 1 && wt[L - 1] == 0x00B0) { wt[L - 1] = 0; ww = tw(F_TINY_B, wt, -1); }
            }
            bx1 = tx0 + G->thumb_w; bx0 = bx1 - ww - dp(6);
            if (bx0 < tx0) bx0 = tx0;
            px_blend(bx0, yy, bx1, ty1 - th, 0x020A16, 150);
            tx(F_TINY_B, bx0 + dp(3), yy + (G->thumb_h - th - fh(F_TINY_B)) / 2, wt, -1, C_BRIGHT);
        }
    }
    yy += G->thumb_h + dp(4);

    /* the today frame goes under the lanes, so a span's bar runs on through it */
    if (c->is_today && s_lanes) px_frame(x0, y0, x1, y1, th, C_BRIGHT);

    /* event lanes (spans as joined bars) */
    if (s_lanes) {
        int cx0 = x0 + p, cx1 = x1 - p, gapr = G->gap;
        for (i = 0; i < LANE_VIS; i++) {
            const LaneSeg *sg = &s_lanes->lane[i];
            DWORD col;
            bool span;
            int lh = G->text_chips ? fh(F_TINY) + dp(1) : dp(5);
            int cy = yy + i * lh, right = cx1;
            if (!sg->e) continue;
            col = type_rgb(sg->e->type);
            span = sg->span_len > 1;
            if (G->text_chips) {
                if (!span) {
                    px_fill(cx0, cy + dp(2), cx0 + th, cy + lh - dp(2), col);
                    tx_ellipsis(F_TINY, cx0 + th + dp(3), cy, right - cx0 - th - dp(3), wz(sg->e->title), C_SOFT);
                } else {
                    /* the bar: flush with the cell edge where it joins or goes on, and across
                       the gap to the right neighbour (painted first: cells paint right to left) */
                    int sx0 = sg->join_l || sg->cont_l ? x0 : cx0;
                    int sx1 = sg->join_r ? x1 + gapr : sg->cont_r ? x1 : right;
                    int by0 = cy + dp(1), by1 = cy + lh - dp(1), tx0, tx1;
                    px_fill(sx0, by0, sx1, by1, mix(C_PANEL2, col, 64));
                    if (sg->span_day == 0) px_fill(sx0, by0, sx0 + th, by1, col);
                    if (sg->cont_l) cont_mark(sx0 + dp(4), (by0 + by1) / 2, true, col);
                    if (sg->cont_r) cont_mark(sx1 - dp(4), (by0 + by1) / 2, false, col);
                    if (sg->title) {
                        tx0 = sx0 + (sg->cont_l ? dp(9) : th + dp(3));
                        tx1 = x0 + sg->run_x1 - (sg->cont_r ? dp(9) : dp(3));
                        if (tx1 > tx0 + dp(6)) tx_ellipsis(F_TINY, tx0, cy, tx1 - tx0, wz(sg->e->title), C_TEXT);
                    }
                }
            } else {
                int sx0 = span && (sg->join_l || sg->cont_l) ? x0 : cx0;
                int sx1 = span && sg->join_r ? x1 + gapr : span && sg->cont_r ? x1 : right;
                px_fill(sx0, cy, sx1, cy + px1(3), col);
            }
        }
    } else {
        int n = c->n_events < 2 ? c->n_events : 2, more = c->n_events - 2;
        int cx0 = x0 + p, cx1 = x1 - p;
        for (i = 0; i < n; i++) {
            const AlmEvent *e = c->chips[i];
            DWORD col;
            int right = cx1;
            if (!e) continue;
            col = type_rgb(e->type);
            if (G->text_chips) {
                int lh = fh(F_TINY) + dp(1), cy = yy + i * lh;
                WCHAR mw[12];
                if (i == 1 && more > 0) {
                    wsprintfW(mw, L"+%d", more);
                    right = cx1 - tw(F_TINY_B, mw, -1) - dp(3);
                    tx(F_TINY_B, right + dp(3), cy, mw, -1, C_MUTED);
                }
                px_fill(cx0, cy + dp(2), cx0 + th, cy + lh - dp(2), col);
                tx_ellipsis(F_TINY, cx0 + th + dp(3), cy, right - cx0 - th - dp(3), wz(e->title), C_SOFT);
            } else {
                int cy = yy + i * dp(5);
                if (i == 1 && more > 0) right = cx1 - dp(5);
                px_fill(cx0, cy, right, cy + px1(3), col);
                if (i == 1 && more > 0) px_fill(right + dp(2), cy, cx1, cy + px1(3), C_MUTED);
            }
        }
    }

    /* edges and states */
    if (c->is_start) px_fill(x0 + 1, y0 + 1, x0 + 1 + th, y1 - 1, C_AURORA);
    if (c->is_today && !s_lanes) px_frame(x0, y0, x1, y1, th, C_BRIGHT);
    if (c->is_pre) px_blend(x0, y0, x1, y1, C_PANEL, 150);
    if (c->tag && !s_fest_cell) tag_pill((x0 + x1) / 2, y0 + dp(3), c->is_today);

    alm_fmt_cell_label(&g.a, c, lab, sizeof lab);
    w8(lab, tip, 200);
    hot_add(x0, y0, x1, y1, H_CAL_CELL, k, true, tip);
}

static void fest_paint(const AlmCell *c, int k, int x0, int y0, int x1, int y1, const CalGeom *G)
{
    int eh = fh(F_CAPS_S) + dp(2);
    /* eyebrow strip above an ordinary cell body */
    px_fill(x0, y0, x1, y0 + eh, mix(C_PANEL, C_PANEL2, 150));
    tx_caps(F_CAPS_S, x0 + dp(5), y0 + dp(2), L"Festival", 0xA9CDE6);
    s_fest_cell = true;
    cell_paint(c, k, x0, y0 + eh, x1, y1, G);
    s_fest_cell = false;
    /* widen the hit region and the frame to include the eyebrow */
    px_frame(x0, y0, x1, y0 + eh, 1, c->is_sel ? C_ACCENT : C_LINE);
    /* tag at the right end of the eyebrow, clear of the "Festival" label */
    if (c->tag) tag_pill(x1 - dp(4) - tag_pill_w(c->is_today) / 2, y0 + eh - dp(1), c->is_today);
    if (g_hl && g_hl->n > 0 && g_hl->items[g_hl->n - 1].id == H_CAL_CELL) g_hl->items[g_hl->n - 1].r.top = y0;
}

/* ================================================================== calendar block */

static int cal_head(int x, int y, int w, bool draw)
{
    int pad = dp(14), bs = dp(28);
    int lh = fh(F_MONTH) + fh(F_SMALL_I);
    int h = (lh > bs ? lh : bs) + dp(6);
    if (draw && vis(y, y + h)) {
        const IwdMonth *mo = iwd_month(g.a.ui.cal_month);
        int tw0 = month_title_w(), xl = x + pad, cy = y + h / 2, by = cy - bs / 2;
        int today_w = btn_w(L"Today"), xr = x + w - pad, tbx0 = xr - today_w;
        int title_x, title_w, ty;
        int32_t t;
        bool has_today = alm_today_abs(&g.a, &t);
        char yb[32]; WCHAR wy[32];
        const char *yn = alm_map_get(&g.a.year_names, g.a.ui.cal_year);
        int room;
        /* left group: prev, title, next */
        btn(xl, by, xl + bs, by + bs, H_CAL_PREV, 0, true, NULL, L"Previous month", BTS_GHOST, BG_LEFT);
        title_x = xl + bs + dp(10);
        title_w = tw0;
        room = tbx0 - dp(12) - dp(90) - (title_x + bs + dp(10));
        if (title_w > room) title_w = room > dp(60) ? room : dp(60);
        ty = cy - lh / 2;
        if (mo) {
            tx_ellipsis(F_MONTH, title_x, ty, title_w, wz(mo->name), C_BRIGHT);
            tx_ellipsis(F_SMALL_I, title_x, ty + fh(F_MONTH), title_w, wz(mo->common), C_MUTED);
        }
        btn(title_x + title_w + dp(10), by, title_x + title_w + dp(10) + bs, by + bs, H_CAL_NEXT, 0, true, NULL,
            L"Next month", BTS_GHOST, BG_RIGHT);
        /* right: Today button, year box */
        btn(tbx0, by, xr, by + bs, H_CAL_TODAY, 0, true, L"Today",
            has_today ? L"Go to the current day (T)" : L"Go to the expedition start (T)", BTS_GHOST, BG_NONE);
        snprintf(yb, sizeof yb, "%ld DR", (long)g.a.ui.cal_year);
        w8(yb, wy, 32);
        {
            int yx1 = tbx0 - dp(12), yx0 = title_x + title_w + dp(10) + bs + dp(12), ww = yx1 - yx0;
            if (ww > dp(20)) {
                if (yn && *yn) {
                    int ytw = tw(F_BODY_B, wy, -1);
                    tx(F_BODY_B, yx1 - (ytw < ww ? ytw : ww), cy - (fh(F_BODY_B) + fh(F_SMALL_I)) / 2, wy, -1, C_TEXT);
                    {
                        const WCHAR *wn = wz(yn);
                        int nw = tw(F_SMALL_I, wn, -1);
                        if (nw <= ww) tx(F_SMALL_I, yx1 - nw, cy - (fh(F_BODY_B) + fh(F_SMALL_I)) / 2 + fh(F_BODY_B), wn, -1, 0xC9D6F2);
                        else tx_ellipsis(F_SMALL_I, yx0, cy - (fh(F_BODY_B) + fh(F_SMALL_I)) / 2 + fh(F_BODY_B), ww, wn, 0xC9D6F2);
                    }
                } else {
                    int ytw = tw(F_BODY_B, wy, -1);
                    tx(F_BODY_B, yx1 - (ytw < ww ? ytw : ww), cy - fh(F_BODY_B) / 2, wy, -1, C_TEXT);
                }
            }
        }
    }
    return h;
}

static int cal_upcoming(int x, int y, int w, bool draw)
{
    AlmUpcoming up[ALM_UPCOMING_DAYS + 1];
    int n = alm_upcoming(&g.a, up), i, k0;
    int pad = dp(14), x0 = x + pad, x1 = x + w - pad, yy = y;
    int32_t today;
    bool has_today = alm_today_abs(&g.a, &today);
    int headh = dp(26);
    /* today's marks are on the hero and in the day panel: Upcoming starts tomorrow */
    for (i = k0 = 0; i < n; i++) if (up[i].rel_days != 0) up[k0++] = up[i];
    n = k0;
    if (draw && vis(yy, yy + headh)) {
        const WCHAR *t = L"Upcoming", *note = has_today ? L"next ten days from today" : L"from the expedition start";
        int tw0 = tw_caps(F_CAPS, t);
        px_fill(x0, yy, x1, yy + 1, C_LINE);
        tx_caps(F_CAPS, x0, yy + (headh - fh(F_CAPS)) / 2 + dp(2), t, C_SOFT);
        if (tw0 + dp(16) + tw(F_SMALL_I, note, -1) <= x1 - x0)
            tx_right(F_SMALL_I, x1, yy + (headh - fh(F_SMALL_I)) / 2 + dp(2), note, C_DIM);
    }
    yy += headh + dp(4);
    if (n <= 0) {
        yy += wrap(F_SMALL_I, x0, yy, x1 - x0, L"Nothing marked for the next ten days.", C_MUTED, draw && vis(yy, yy + 2 * fh(F_SMALL_I)));
        return yy - y + dp(4);
    }
    for (i = 0; i < n; i++) {
        AlmMark marks[12];
        char ct[12][200];
        int nm = alm_upcoming_marks(&g.a, &up[i], marks, 12), k;
        int lw = dp(118), rp = dp(8), chh = fh(F_SMALL) + dp(6), cgap = dp(4);
        int bx0, bx1, cx, cy, rowh, lefth = fh(F_SMALL_B) + fh(F_SMALL);
        if (nm > 12) nm = 12;
        /* a span is listed once, where it starts: "Fair · 5 days" (or "day 2 of 5" when it is
           already under way on the first row) */
        for (k = 0; k < nm; k++) alm_fmt_mark_chip(&marks[k], true, ct[k], sizeof ct[k]);
        if (lw > (x1 - x0) / 3) lw = (x1 - x0) / 3;
        bx0 = x0 + rp + lw; bx1 = x1 - rp;
        /* measure the chip flow */
        cx = bx0; cy = 0;
        for (k = 0; k < nm; k++) {
            int cw = chip_width(&marks[k], ct[k], F_SMALL, bx1 - bx0);
            if (cx > bx0 && cx + cw > bx1) { cx = bx0; cy += chh + cgap; }
            cx += cw + cgap;
        }
        rowh = cy + chh;
        if (rowh < lefth) rowh = lefth;
        rowh += 2 * dp(6);
        if (draw && vis(yy, yy + rowh)) {
            char rel[32], dt[IWD_DATE_CAP]; WCHAR wr[32], wd[IWD_DATE_CAP];
            bool hot = is_hot(H_UPCOMING, up[i].abs), sel = up[i].abs == g.a.sel_abs;
            int oy = yy + dp(6) + (rowh - 2 * dp(6) - (cy + chh)) / 2;
            px_box(x0, yy, x1, yy + rowh, hot ? mix(C_PANEL2, C_ACCENT, 26) : sel ? mix(C_PANEL, C_PANEL2, 200) : mix(C_PANEL, C_PANEL2, 120),
                   sel ? mix(C_FRAME, C_ACCENT, 120) : hot ? C_FRAME : C_LINE);
            iwd_rel_days(up[i].rel_days, rel, sizeof rel);
            iwd_format_date(up[i].abs, true, dt, sizeof dt);
            w8(rel, wr, 32); w8(dt, wd, IWD_DATE_CAP);
            {
                int ly = yy + (rowh - lefth) / 2;
                tx_ellipsis(F_SMALL_B, x0 + rp, ly, lw - dp(6), wr, C_TEXT);
                tx_ellipsis(F_SMALL, x0 + rp, ly + fh(F_SMALL_B), lw - dp(6), wd, C_MUTED);
            }
            cx = bx0; cy = 0;
            for (k = 0; k < nm; k++) {
                int cw = chip_width(&marks[k], ct[k], F_SMALL, bx1 - bx0);
                if (cx > bx0 && cx + cw > bx1) { cx = bx0; cy += chh + cgap; }
                chip_draw(&marks[k], ct[k], F_SMALL, cx, oy + cy, cw, chh);
                cx += cw + cgap;
            }
            hot_add(x0, yy, x1, yy + rowh, H_UPCOMING, up[i].abs, true, NULL);
        }
        yy += rowh + dp(5);
    }
    return yy - y;
}

static int cal_layout(int x, int y, int w, bool draw)
{
    CalGeom G;
    AlmCell cells[32];
    int n = 0, i, yy = y + dp(10), gx0, rows_h;
    cal_geom(w, &G);
    yy += cal_head(x, yy, w, draw);
    yy += dp(12);                                  /* room for the tag notch of the first row */
    gx0 = x + G.pad;
    n = alm_month_cells(&g.a, cells);
    {
        /* a tenday with no weather, events or tag needs only the number and the moon */
        int rh[3], ry[3], r;
        rows_h = 0;
        for (r = 0; r < 3; r++) {
            bool busy = false;
            for (i = r * 10; i < r * 10 + 10 && i < n; i++)
                if (cells[i].has_weather || cells[i].n_events > 0 || cells[i].tag) { busy = true; break; }
            rh[r] = busy ? G.ch : dp(5) + fh(F_SMALL_B) + dp(7);
            ry[r] = yy + rows_h;
            rows_h += rh[r] + (r < 2 ? G.rgap : 0);
        }
        if (draw && vis(yy - dp(10), yy + rows_h + dp(4))) {
            for (r = 0; r < 3 && r * 10 < n; r++) {
                CellLanes ln[10];
                int xs[10], m = n - r * 10 < 10 ? n - r * 10 : 10, c;
                if (!vis(ry[r] - dp(8), ry[r] + rh[r] + dp(4))) continue;
                for (c = 0; c < m; c++) xs[c] = gx0 + c * (G.cw + G.gap);
                lanes_for_row(&cells[r * 10], m, xs, G.cw, ln);
                /* right to left: a bar's title may run on over the cells after its first */
                for (c = m - 1; c >= 0; c--) {
                    i = r * 10 + c;
                    s_lanes = &ln[c];
                    cell_paint(&cells[i], i, xs[c], ry[r], xs[c] + G.cw, ry[r] + rh[r], &G);
                }
                s_lanes = NULL;
            }
        }
    }
    yy += rows_h;
    if (n > 30) {
        int fx = gx0, maxx = x + w - G.pad, fch = G.fch;
        bool busy = false;
        for (i = 30; i < n; i++)
            if (cells[i].has_weather || cells[i].n_events > 0 || cells[i].tag) busy = true;
        if (!busy) fch = fh(F_CAPS_S) + dp(2) + dp(5) + fh(F_SMALL_B) + dp(7);
        yy += G.rgap + dp(2);
        for (i = 30; i < n; i++) {
            if (fx > gx0 && fx + G.fw > maxx) { fx = gx0; yy += fch + G.rgap; }
            if (draw && vis(yy - dp(8), yy + fch + dp(4))) {
                CellLanes ln[1];
                int xs[1] = { fx + G.pad };    /* any x: a festival cell is a row of one */
                int L;
                lanes_for_row(&cells[i], 1, xs, G.fw, ln);
                for (L = 0; L < LANE_VIS; L++) ln[0].lane[L].run_x1 = G.fw;   /* the run is the cell */
                s_lanes = &ln[0];
                fest_paint(&cells[i], i, fx, yy, fx + G.fw, yy + fch, &G);
                s_lanes = NULL;
            }
            fx += G.fw + G.gap;
        }
        yy += fch;
    }
    yy += dp(16);
    yy += cal_upcoming(x, yy, w, draw);
    return yy - y + dp(10);
}

/* ================================================================== day block */

static int section_label(int x, int y, const WCHAR *t, const WCHAR *state, bool draw)
{
    int h = fh(F_CAPS) + dp(8);
    if (draw && vis(y, y + h)) {
        tx_caps(F_CAPS, x, y + dp(2), t, C_SOFT);
        if (state && *state) tx(F_SMALL_I, x + tw_caps(F_CAPS, t) + dp(10), y + dp(2) + (fh(F_CAPS) - fh(F_SMALL_I)) / 2, state, -1, C_DIM);
    }
    return h;
}

static void set_rc(RECT *r, int x0, int y0, int x1, int y1)
{
    r->left = x0; r->right = x1;
    r->top = view_content_y(y0); r->bottom = view_content_y(y1);
}

/* one field frame; returns its height */
static int field(int x0, int y, int x1, int h, bool draw, bool focused, bool invalid)
{
    if (draw && vis(y, y + h))
        px_box(x0, y, x1, y + h, C_FIELD, invalid ? C_DANGER : focused ? mix(C_FRAME, C_ACCENT, 170) : C_FRAME);
    return h;
}

static bool has_focus(HWND h) { return h && GetFocus() == h; }

/* "Day 3 of 5 · changes apply to the whole event" while editing a span (else empty) */
static bool form_span_hint(WCHAR *out, int cap)
{
    const AlmEvent *e = g.form_id[0] ? alm_event_find(&g.a, g.form_id) : NULL;
    char b[160];
    int k;
    out[0] = 0;
    if (!e || e->days <= 1) return false;
    k = alm_event_span_day(e, g.a.sel_abs, g.a.moon_offset, NULL);
    if (k >= 0) snprintf(b, sizeof b, "Day %d of %d \xC2\xB7 changes apply to the whole event", k + 1, e->days);
    else snprintf(b, sizeof b, "Changes apply to the whole event");
    w8(b, out, cap);
    return true;
}

/* The first day of the span the form edits: the selected day for a new event, else the
 * start of the occurrence the selected day belongs to (the anchor as a fallback). */
static int32_t form_span_start(void)
{
    const AlmEvent *e = g.form_id[0] ? alm_event_find(&g.a, g.form_id) : NULL;
    int32_t st = g.a.sel_abs;
    if (e && alm_event_span_day(e, g.a.sel_abs, g.a.moon_offset, &st) < 0) st = e->abs;
    return st;
}

/* Repeat select and the "Lasts [n] days" stepper: one row when they fit, else two.
 * The line under the stepper is always reserved: the days covered ("5 Hammer",
 * "5–9 Hammer", "30 Hammer – 1 Alturiak") or, invalid, "1 to 30 days" in red.
 * Measures (draw false) and paints along the same path; returns the height. */
static int form_repeat_lasts(int ix0, int ix1, int y, bool draw)
{
    int lab = fh(F_SMALL) + dp(4), fhgt = fh(F_BODY) + dp(12), yy = y;
    int sw = ix1 - ix0, bs = fhgt, numw = dp(40);
    int gw = bs + dp(4) + numw + dp(4) + bs + dp(8) + tw(F_BODY, L"days", -1);
    int lx, ly;
    bool one_row;
    if (sw > dp(240)) sw = dp(240);
    one_row = sw + dp(16) + gw <= ix1 - ix0;
    if (draw) {
        int hot = is_hot(H_FORM_REPEAT, 0);
        int r = g.form_repeat >= 0 && g.form_repeat < IWD_REPEAT_COUNT ? g.form_repeat : 0;
        tx(F_SMALL, ix0, yy, L"Repeat", -1, C_SOFT);
        px_box(ix0, yy + lab, ix0 + sw, yy + lab + fhgt, hot ? mix(C_FIELD, C_ACCENT, 20) : C_FIELD, hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME);
        tx_ellipsis(F_BODY, ix0 + dp(8), yy + lab + (fhgt - fh(F_BODY)) / 2, sw - dp(30), wz(iwd_repeat_label((IwdRepeat)r)), C_TEXT);
        chevron_down(ix0 + sw - dp(13), yy + lab + fhgt / 2, C_SOFT);
        hot_add(ix0, yy + lab, ix0 + sw, yy + lab + fhgt, H_FORM_REPEAT, 0, true, NULL);
    }
    if (one_row) { lx = ix0 + sw + dp(16); ly = yy; }
    else { lx = ix0; ly = yy + lab + fhgt + dp(10); }
    if (draw) {
        int fy = ly + lab, nx0 = lx + bs + dp(4), nx1 = nx0 + numw;
        WCHAR num[8];
        tx(F_SMALL, lx, ly, L"Lasts", -1, C_SOFT);
        btn(lx, fy, lx + bs, fy + fhgt, H_FORM_DAYS, -1, true, L"\x2212", L"One day shorter (Down)", BTS_GHOST, BG_NONE);
        field(nx0, fy, nx1, fhgt, true, has_focus(g.ed_days), g.form_days_invalid);
        {
            int ih = fh(F_BODY) + dp(2), iy = fy + (fhgt - ih) / 2;
            set_rc(&g.rc_days, nx0 + dp(7), iy, nx1 - dp(5), iy + ih);
            if (!g.ed_days) { wsprintfW(num, L"%d", g.form_days); tx(F_BODY, nx0 + dp(7), iy + dp(1), num, -1, C_TEXT); }
        }
        btn(nx1 + dp(4), fy, nx1 + dp(4) + bs, fy + fhgt, H_FORM_DAYS, 1, true, L"+",
            L"One day longer (Up) \x2014 or type 1 to 30", BTS_GHOST, BG_NONE);
        tx(F_BODY, nx1 + dp(4) + bs + dp(8), fy + (fhgt - fh(F_BODY)) / 2, g.form_days == 1 ? L"day" : L"days", -1, C_SOFT);
        if (g.form_days_invalid)
            tx(F_SMALL, lx, fy + fhgt + dp(3), L"1 to 30 days", -1, C_DANGER);
        else {
            char rg[96];
            alm_fmt_span_range(form_span_start(), g.form_days, rg, sizeof rg);
            tx_ellipsis(F_SMALL, lx, fy + fhgt + dp(3), ix1 - lx, wz(rg), C_MUTED);
        }
    }
    yy = ly + lab + fhgt + fh(F_SMALL) + dp(3);
    return yy - y;
}

static int day_form(int x0, int y, int x1, bool draw)
{
    int pad = dp(12), ix0 = x0 + pad, ix1 = x1 - pad, yy = y + pad, lab = fh(F_SMALL) + dp(4);
    bool editing = g.form_id[0] != 0;
    int fhgt = fh(F_BODY) + dp(12), dh = 3 * fh(F_BODY) + dp(12), t;
    int y_start = y;
    /* frame drawn first (full height computed below) - so measure first */
    int total;
    {
        int my = yy;
        my += fh(F_BODY_B) + dp(8);
        {
            WCHAR hint[160];
            if (form_span_hint(hint, 160)) my += wrap(F_SMALL_I, ix0, 0, ix1 - ix0, hint, C_MUTED, false) + dp(8);
        }
        my += lab + fhgt + dp(10);
        {
            int cx = ix0, ph = fh(F_SMALL) + dp(10);
            my += lab;
            for (t = 0; t < ALM_EV_TYPE_COUNT; t++) {
                int pw = dp(12) + dp(8) + tw(F_SMALL, wz(alm_ev_type_label((AlmEvType)t)), -1) + dp(10);
                if (cx > ix0 && cx + pw > ix1) { cx = ix0; my += ph + dp(6); }
                cx += pw + dp(6);
            }
            my += ph + dp(10);
        }
        my += form_repeat_lasts(ix0, ix1, my, false) + dp(10);
        my += lab + dh + dp(12);
        my += btn_h();
        total = my - y + pad;
    }
    if (draw && vis(y, y + total)) px_box(x0, y, x1, y + total, mix(C_PANEL, C_PANEL2, 170), C_FRAME);
    if (!draw) return total;

    tx(F_BODY_B, ix0, yy, editing ? L"Edit event" : L"New event or reminder", -1, C_BRIGHT);
    yy += fh(F_BODY_B) + dp(8);
    {
        WCHAR hint[160];
        if (form_span_hint(hint, 160)) yy += wrap(F_SMALL_I, ix0, yy, ix1 - ix0, hint, C_MUTED, true) + dp(8);
    }

    /* Title */
    tx(F_SMALL, ix0, yy, L"Title", -1, C_SOFT);
    yy += lab;
    field(ix0, yy, ix1, fhgt, true, has_focus(g.ed_title), g.form_invalid);
    {
        int ih = fh(F_BODY) + dp(2), iy = yy + (fhgt - ih) / 2;
        set_rc(&g.rc_title, ix0 + dp(7), iy, ix1 - dp(7), iy + ih);
        if (!g.ed_title) tx_ellipsis(F_BODY, ix0 + dp(7), iy + dp(1), ix1 - ix0 - dp(14), L"e.g. Auril's followers attack Bryn Shander", C_DIM);
    }
    yy += fhgt + dp(10);

    /* Type */
    tx(F_SMALL, ix0, yy, L"Type", -1, C_SOFT);
    yy += lab;
    {
        int cx = ix0, ph = fh(F_SMALL) + dp(10);
        for (t = 0; t < ALM_EV_TYPE_COUNT; t++) {
            const WCHAR *s = wz(alm_ev_type_label((AlmEvType)t));
            int pw = dp(12) + dp(8) + tw(F_SMALL, s, -1) + dp(10);
            bool on = g.form_type == t, hot = is_hot(H_FORM_TYPE, t);
            DWORD col = type_rgb((AlmEvType)t);
            if (cx > ix0 && cx + pw > ix1) { cx = ix0; yy += ph + dp(6); }
            px_box(cx, yy, cx + pw, yy + ph,
                   on ? mix(C_PANEL2, col, 46) : hot ? mix(C_PANEL2, col, 18) : C_PANEL2,
                   on ? col : hot ? mix(C_FRAME, col, 110) : C_FRAME);
            {   /* radio: ring, filled when chosen */
                double rcx = cx + dp(10), rcy = yy + ph / 2.0;
                aa_ring(rcx, rcy, dp(4), on ? col : C_MUTED, 255, 1.2);
                if (on) aa_circle(rcx, rcy, dp(2), col, 255);
                aa_flush();
            }
            tx(F_SMALL, cx + dp(20), yy + (ph - fh(F_SMALL)) / 2, s, -1, on ? C_BRIGHT : C_SOFT);
            {
                static const WCHAR *const tips[ALM_EV_TYPE_COUNT] = {
                    L"Natural: something the Dale itself does (beasts, ice, thaw, travellers)",
                    L"Unnatural: magic, monsters or the uncanny",
                    L"Auril: the Frostmaiden, her cult and her sacrifices (shown first on the calendar)",
                    L"Reminder: a note for the DM (deadlines, debts, meetings)" };
                hot_add(cx, yy, cx + pw, yy + ph, H_FORM_TYPE, t, true, tips[t]);
            }
            cx += pw + dp(6);
        }
        yy += ph + dp(10);
    }

    /* Repeat, Lasts */
    yy += form_repeat_lasts(ix0, ix1, yy, true) + dp(10);

    /* Details (optional) */
    tx(F_SMALL, ix0, yy, L"Details", -1, C_SOFT);
    tx(F_SMALL_I, ix0 + tw(F_SMALL, L"Details ", -1), yy, L"(optional)", -1, C_DIM);
    yy += lab;
    field(ix0, yy, ix1, dh, true, has_focus(g.ed_details), false);
    set_rc(&g.rc_details, ix0 + dp(7), yy + dp(6), ix1 - dp(7), yy + dh - dp(6));
    yy += dh + dp(12);

    /* buttons, right-aligned */
    {
        const WCHAR *sv = editing ? L"Save changes" : L"Add to the almanac";
        int sw = btn_w(sv), cw = btn_w(L"Cancel"), bh = btn_h();
        btn(ix1 - sw, yy, ix1, yy + bh, H_FORM_SAVE, 0, true, sv, NULL, BTS_SOLID, BG_NONE);
        btn(ix1 - sw - dp(8) - cw, yy, ix1 - sw - dp(8), yy + bh, H_FORM_CANCEL, 0, true, L"Cancel", L"Cancel (Esc)", BTS_GHOST, BG_NONE);
    }
    (void)y_start;
    return total;
}

static int day_events(int x0, int y, int x1, bool draw)
{
    AlmMark marks[64];
    int n = alm_marks_on(&g.a, g.a.sel_abs, marks, 64), i, yy = y;
    if (n > 64) n = 64;
    if (n <= 0) {
        yy += wrap(F_SMALL_I, x0, yy, x1 - x0, L"None on this day.", C_MUTED, draw && vis(yy, yy + 2 * fh(F_SMALL_I)));
        return yy - y + dp(8);
    }
    for (i = 0; i < n; i++) {
        const AlmMark *m = &marks[i];
        char meta[256]; WCHAR wm[256];
        const WCHAR *title;
        WCHAR *details = NULL;
        int pad = dp(9), bx0 = x0 + dp(3) + dp(10), tools = 0, bx1, h, th, mh, dh = 0;
        int ew = btn_w(L"Edit"), dw = btn_w(L"Delete"), bh = fh(F_BTN) + dp(8);
        bool user = !m->builtin && m->event;
        if (user) tools = ew + dp(6) + dw + dp(4);
        bx1 = x1 - pad - tools;
        if (bx1 < bx0 + dp(60)) bx1 = bx0 + dp(60);
        alm_fmt_mark_meta(m, g.a.sel_abs, meta, sizeof meta);
        w8(meta, wm, 256);
        title = wz(m->title);
        th = wrap(F_BODY_B, bx0 + (m->builtin ? dp(14) : 0), 0, bx1 - bx0 - (m->builtin ? dp(14) : 0), title, C_BRIGHT, false);
        mh = wrap(F_SMALL, bx0, 0, bx1 - bx0, wm, C_MUTED, false);
        if (user && m->event->details && m->event->details[0]) {
            details = wz_dup(m->event->details);
            if (details) dh = wrap(F_SMALL, bx0, 0, bx1 - bx0, details, C_SOFT, false) + dp(3);
        }
        h = pad + th + dp(1) + mh + (dh ? dp(3) + dh : 0) + pad;
        if (user && h < 2 * pad + bh) h = 2 * pad + bh;
        if (draw && vis(yy, yy + h)) {
            DWORD col = type_rgb(m->type);
            int ty = yy + pad;
            px_box(x0, yy, x1, yy + h, mix(C_PANEL, 0x041020, 110), mix(C_PANEL, 0xC8EBFF, 22));
            px_fill(x0, yy, x0 + px1(3), yy + h, col);
            if (m->builtin) {
                aa_flake(bx0 + dp(5), ty + fh(F_BODY_B) / 2.0, dp(5), col, 240);
                aa_flush();
            }
            wrap(F_BODY_B, bx0 + (m->builtin ? dp(14) : 0), ty, bx1 - bx0 - (m->builtin ? dp(14) : 0), title, C_BRIGHT, true);
            ty += th + dp(1);
            wrap(F_SMALL, bx0, ty, bx1 - bx0, wm, C_MUTED, true);
            ty += mh;
            if (dh && details) wrap(F_SMALL, bx0, ty + dp(3), bx1 - bx0, details, C_SOFT, true);
            if (user) {
                bool armed = g.armed_id[0] && strcmp(g.armed_id, m->event->id) == 0;
                bool rep = m->event->repeat != IWD_REPEAT_NONE, span = m->span_len > 1;
                int by = yy + pad, xr = x1 - pad;
                WCHAR dtip[96], etip[96];
                if (span) {
                    wsprintfW(dtip, armed ? L"Press again to delete all %d days%s" : L"Delete (all %d days%s)", m->span_len,
                              rep ? L", every occurrence" : L"");
                    wsprintfW(etip, L"Edit (all %d days)", m->span_len);
                } else {
                    lstrcpyW(dtip, armed ? L"Press again to delete" : rep ? L"Delete (all occurrences)" : L"Delete");
                    lstrcpyW(etip, L"Edit");
                }
                if (armed && span) {
                    /* armed span: one wide red button over Edit + Delete that says what goes */
                    WCHAR al[48];
                    int aw = ew + dp(6) + dw;
                    wsprintfW(al, L"Delete all %d days?", m->span_len);
                    if (tw(F_BTN, al, -1) > aw - dp(12)) wsprintfW(al, L"All %d days?", m->span_len);
                    btn(xr - aw, by, xr, by + bh, H_EV_DEL, i, true, al, dtip, BTS_DANGER, BG_NONE);
                } else {
                    btn(xr - dw, by, xr, by + bh, H_EV_DEL, i, true, armed ? L"Delete?" : L"Delete", dtip,
                        armed ? BTS_DANGER : BTS_GHOST, BG_NONE);
                    xr -= dw + dp(6);
                    btn(xr - ew, by, xr, by + bh, H_EV_EDIT, i, true, L"Edit", etip, BTS_GHOST, BG_NONE);
                }
            }
        }
        free(details);
        yy += h + dp(6);
    }
    return yy - y + dp(2);
}

static int day_layout(int x, int y, int w, bool draw)
{
    static AlmDayPanel p;                 /* large-ish; UI thread only */
    AlmHero hr;
    int32_t today;
    bool has_today, is_today;
    int pad = dp(14), x0 = x + pad, x1 = x + w - pad, yy = y + dp(12), iw;
    if (x1 < x0 + dp(120)) x1 = x0 + dp(120);
    iw = x1 - x0;
    alm_day_panel(&g.a, &p);
    alm_hero(&g.a, &hr);
    has_today = alm_today_abs(&g.a, &today);
    is_today = has_today && today == g.a.sel_abs;

    /* the selected day is not the one on screen (before the start, beyond the almanac):
       name it, and say why it has no weather */
    if (g.a.sel_abs != hr.abs) {
        yy += wrap(F_BODY_B, x0, yy, iw, wz(p.date), C_BRIGHT, draw && vis(yy, yy + 2 * fh(F_BODY_B)));
        if (!p.weather) {
            yy += dp(2);
            yy += wrap(F_SMALL_I, x0, yy, iw, wz(p.weather_none), C_MUTED, draw && vis(yy, yy + 3 * fh(F_SMALL_I)));
        }
        yy += dp(12);
    }

    /* the one visible way to move today */
    if (p.show_set_current && !is_today && !(g.a.current < 0 && g.a.sel_abs == g.a.start_abs)) {
        const WCHAR *lab = L"Make this day today";
        int bw = btn_w(lab), bh = btn_h() + dp(4);
        if (bw > iw) bw = iw;
        if (draw && vis(yy, yy + bh))
            btn(x0, yy, x0 + bw, yy + bh, H_SET_CURRENT, 0, true, lab,
                L"Make this the party's current day (or double-click the day in the calendar)",
                BTS_GHOST, BG_NONE);
        yy += bh + dp(14);
    }

    /* events & reminders, "Add" at the right of the label row */
    {
        const WCHAR *al = L"Add";
        int bw = btn_w(al), bh = fh(F_BTN) + dp(8), lh = fh(F_CAPS) + dp(8), rh = bh + dp(6) > lh ? bh + dp(6) : lh;
        section_label(x0, yy + (rh - lh) / 2, L"Events & reminders", NULL, draw);
        if (!g.form_open && draw && vis(yy, yy + rh))
            btn(x1 - bw, yy + (rh - bh) / 2, x1, yy + (rh - bh) / 2 + bh, H_ADD_EVENT, 0, true, al, L"New event or reminder on this day (E)",
                BTS_GHOST, BG_NONE);
        yy += rh;
    }
    yy += day_events(x0, yy, x1, draw);
    if (g.form_open) {
        yy += day_form(x0, yy, x1, draw) + dp(12);
    } else {
        if (draw) {
            SetRectEmpty(&g.rc_title);
            SetRectEmpty(&g.rc_details);
            SetRectEmpty(&g.rc_days);
        }
        yy += dp(8);
    }

    /* notes */
    {
        const WCHAR *state = NULL;
        int nh = dp(124);
        if (g.notes_abs == g.a.sel_abs) state = g.note_state == 1 ? L"editing\x2026" : g.note_state == 2 ? L"saved" : NULL;
        yy += section_label(x0, yy, L"Notes", state, draw);
        if (draw) {
            int ix0 = x0 + dp(8), iy0 = yy + dp(6), ix1 = x1 - dp(8), iy1 = yy + nh - dp(6);
            field(x0, yy, x1, nh, true, has_focus(g.ed_notes), false);
            set_rc(&g.rc_notes, ix0, iy0, ix1, iy1);
            if (!g.ed_notes && vis(yy, yy + nh)) {
                const char *note = alm_note_get(&g.a, g.a.sel_abs);
                if (note && *note) {
                    WCHAR *wn = wz_dup(note);
                    if (wn) { tx_box(F_BODY, ix0 + dp(2), iy0 + dp(1), ix1, iy1, wn, C_TEXT); free(wn); }
                } else {
                    tx_box(F_BODY, ix0 + dp(2), iy0 + dp(1), ix1, iy1,
                           L"What happened on this day? Rumours, debts, oaths, frostbite\x2026", C_DIM);
                }
            }
            if (vis(yy, yy + nh)) hot_add(x0, yy, x1, yy + nh, H_NOTES, 0, true, NULL);
        }
        yy += nh;
    }
    return yy - y + dp(14);
}

/* ================================================================== block entry points */

static void panel_bg(int x, int y, int w, int h)
{
    px_fill(x, y, x + w, y + h, C_PANEL);
    px_frame(x, y, x + w, y + h, 1, C_LINE);
}

int blk_cal_measure(int which, int w)
{
    if (which == BLK_CALENDAR) return cal_layout(0, 0, w, false);
    if (which == BLK_DAY) return day_layout(0, 0, w, false);
    return 0;
}

/* The calendar is the costliest block (30 cells, moon discs, thumbnails) and changes
 * rarely, so it is rendered into its own surface and re-blitted while its inputs stay the
 * same; time steps, scrubbing and hovering elsewhere then cost one copy. The hit regions
 * of the render are kept with it (block-relative) and re-registered on every paint. */
typedef struct CalKey {
    int w, h, dpi;
    uint32_t epoch, rev;
    int32_t n_days, current, view, sel_abs, start_abs, cal_year;
    int cal_month, n_events, hot_id, hot_arg, press_id, press_arg;
    bool celsius, has_today;
    double moon_offset;
} CalKey;
static Surface s_cal;
static CalKey s_ck;
static bool s_cal_ok;
#define CAL_HOT_MAX 120
static Hot s_cal_hot[CAL_HOT_MAX];
static int s_cal_nhot;

static bool cal_hot_id(int id)
{
    return id == H_CAL_PREV || id == H_CAL_NEXT || id == H_CAL_TODAY || id == H_CAL_CELL || id == H_UPCOMING;
}

static void cal_cached(int x, int y, int w, int h)
{
    CalKey k; int i;
    int32_t t;
    memset(&k, 0, sizeof k);
    k.w = w; k.h = h; k.dpi = g.dpi; k.epoch = g.a.epoch; k.rev = g.rev;
    k.n_days = g.a.n_days; k.current = g.a.current; k.view = g.a.view; k.sel_abs = g.a.sel_abs;
    k.start_abs = g.a.start_abs; k.cal_year = g.a.ui.cal_year; k.cal_month = g.a.ui.cal_month;
    k.n_events = g.a.n_events; k.celsius = g.a.celsius; k.moon_offset = g.a.moon_offset;
    k.has_today = alm_today_abs(&g.a, &t);
    if (cal_hot_id(g.hot_id)) { k.hot_id = g.hot_id; k.hot_arg = g.hot_arg; }
    if (cal_hot_id(g.press_id)) { k.press_id = g.press_id; k.press_arg = g.press_arg; }
    if (!s_cal_ok || !s_cal.px || memcmp(&k, &s_ck, sizeof k) != 0) {
        Surface *save = g_tgt;
        HotList *hl = g_hl;
        int n0;
        if (!surf_resize(&s_cal, w, h)) { panel_bg(x, y, w, h); cal_layout(x, y, w, true); return; }
        g_tgt = &s_cal;
        n0 = hl ? hl->n : 0;
        panel_bg(0, 0, w, h);
        cal_layout(0, 0, w, true);
        aa_flush();
        GdiFlush();
        g_tgt = save;
        s_cal_nhot = 0;
        if (hl) {
            for (i = n0; i < hl->n && s_cal_nhot < CAL_HOT_MAX; i++) s_cal_hot[s_cal_nhot++] = hl->items[i];
            hl->n = n0;
        }
        s_ck = k;
        s_cal_ok = true;
    }
    px_copy(s_cal.px, w, h, x, y);
    for (i = 0; i < s_cal_nhot; i++) {
        const Hot *o = &s_cal_hot[i];
        Hot *n = hot_add(o->r.left + x, o->r.top + y, o->r.right + x, o->r.bottom + y, o->id, o->arg, o->enabled, NULL);
        if (n) lstrcpynW(n->tip, o->tip, (int)(sizeof n->tip / sizeof n->tip[0]));
    }
}

void blk_cal_paint(int which, int x, int y, int w, int h)
{
    if (!g_tgt || w <= 0 || h <= 0) return;
    if (which == BLK_CALENDAR) {
        if (!vis(y, y + h)) return;
        cal_cached(x, y, w, h);
    } else if (which == BLK_DAY) {
        /* always run the layout: it also places the EDIT rectangles (content coordinates) */
        if (vis(y, y + h)) panel_bg(x, y, w, h);
        day_layout(x, y, w, true);
    }
}

void blk_cal_free(void)
{
    surf_free(&s_cal);
    s_cal_ok = false;
    thumb_free();
    s_title_w = 0; s_title_dpi = 0;
}

/* ================================================================== ui.h query */

int view_cells(AlmCell out[32])
{
    return alm_month_cells(&g.a, out);
}
