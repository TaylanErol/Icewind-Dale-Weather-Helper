/* ui_view_time.c - the "The Day's Weather" timeline block and the "Dice Ledger" block
 * (SPEC.md 7.5, 7.6), plus view_timeline_time / view_timeline_hit /
 * view_invalidate_timeline from ui.h.
 *
 * Painting model
 *   BLK_TIMELINE: the static part (cycle bands, temperature trace with grid and labels, the
 *   segment bar made of scene-image slices, hour ticks) is composed once into a private
 *   Surface (DIB + GDI+), keyed by (almanac epoch, view, unit, size, dpi). Every paint just
 *   copies it and draws the live parts on top: head row (navigator, play), playhead line,
 *   knob, time label, the chart dot, hover outline and the scrub ring. Moving the playhead
 *   therefore costs one memcpy of the strip plus a few primitives.
 *   BLK_LEDGER: plain rich text, drawn directly (a day has at most ~25 rows).
 *
 * Hit regions registered here (all in back-buffer coordinates, already scrolled):
 *   H_DAY_PREV  arg 0        previous day (alm_step_day(-1)); disabled per alm_controls
 *   H_DAY_NEXT  arg 0        next day (alm_step_day(+1)); disabled per alm_controls
 *   H_PLAY      arg 0        play / pause (alm_toggle_play); tip "Play the day" / "Pause"
 *   H_TIMELINE  arg i >= 0   segment i of the viewed day (index into IwdDay.segments) on
 *                            the bar; tip = the web's segment tooltip
 *               arg -1       the temperature trace / tick area above and below the bar
 *               Both start scrubbing: time = view_timeline_time(mouse x); enabled only
 *               when a day is on screen and nothing is busy.
 *   The ledger registers no hit regions.
 *
 * Deviations from the web (documented for the integrator):
 *   - Segment blocks are slices of the event's scene picture instead of flat colour; the
 *     event colour survives as a 3 px rule along the top of each block. Labels are light
 *     ink with a shadow on every segment (dark ink is unreadable on a photograph).
 *   - Dice are boxed digits (SPEC 7.6), the d12 in the aurora tint.
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

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* single-pixel blend straight into the target (caller did GdiFlush) */
static inline void pblend(int x, int y, DWORD c, int a)
{
    DWORD *p;
    if (a <= 0 || x < 0 || y < 0 || x >= g_tgt->w || y >= g_tgt->h) return;
    p = g_tgt->px + (size_t)y * g_tgt->w + x;
    *p = mix(*p, c, a > 256 ? 256 : a);
}

static const IwdDay *viewed_day(void)
{
    if (!alm_has_view(&g.a) || g.a.view >= g.a.n_days || !g.a.days) return NULL;
    return &g.a.days[g.a.view];
}

static bool tl_interactive(void) { return viewed_day() != NULL && !g.a.busy; }

/* ================================================================== timeline geometry */

typedef struct TlGeom {
    int x, y, w, h;
    int pad, gut;
    int head_y, head_h;
    int cx0, cy0, cw, ch;           /* cached strip rect (back-buffer coords) */
    int tx0, tx1;                   /* 0 h .. 24 h */
    int band_y, band_h;             /* first band row; each row band_h tall */
    int chart_y, chart_h;
    int bar_y, bar_h;
    int tick_y, tick_h;
} TlGeom;

static int gutter_w(void)
{
    int a = tw_caps(F_CAPS_S, L"Twilight"), b = tw(F_TINY, L"−130°", -1);
    return (a > b ? a : b) + dp(12);
}

static int tl_height(void)
{
    return dp(10) + dp(28) + dp(8) + 3 * dp(12) + dp(4) + dp(66) + dp(42) + dp(22) + dp(6);
}

static void tl_geom(int x, int y, int w, TlGeom *G)
{
    int yy;
    G->x = x; G->y = y; G->w = w; G->h = tl_height();
    G->pad = dp(14);
    G->gut = gutter_w();
    if (G->gut > w / 4) G->gut = w / 4;
    G->head_y = y + dp(10); G->head_h = dp(28);
    yy = G->head_y + G->head_h + dp(8);
    G->cx0 = x + G->pad; G->cy0 = yy;
    G->tx0 = G->cx0 + G->gut; G->tx1 = x + w - G->pad;
    if (G->tx1 < G->tx0 + 24) G->tx1 = G->tx0 + 24;
    G->band_y = yy; G->band_h = dp(12);
    yy += 3 * G->band_h + dp(4);
    G->chart_y = yy; G->chart_h = dp(66); yy += G->chart_h;
    G->bar_y = yy; G->bar_h = dp(42); yy += G->bar_h;
    G->tick_y = yy; G->tick_h = dp(22); yy += G->tick_h;
    G->cw = G->tx1 - G->cx0 + dp(4);          /* a little room for the "24" label */
    if (G->cx0 + G->cw > x + w - 1) G->cw = x + w - 1 - G->cx0;
    G->ch = yy - G->cy0;
}

static int hour_x(const TlGeom *G, double h) { return G->tx0 + (int)floor(h / 24.0 * (G->tx1 - G->tx0) + 0.5); }

/* last painted geometry (for view_timeline_time / view_timeline_hit) */
static TlGeom s_last; static bool s_last_ok;

/* ================================================================== the cached strip */

typedef struct StripKey {
    uint32_t epoch; int32_t view; int32_t n_days; bool celsius; bool has_view;
    int cw, ch, gut, dpi, tw_;
} StripKey;

static Surface  s_strip;
static StripKey s_key;
static bool     s_valid;
static int      s_nseg;
static RECT     s_seg_rc[IWD_MAX_SEGMENTS];        /* bar-relative to the strip origin */
static WCHAR    s_seg_tip[IWD_MAX_SEGMENTS][200];

void view_invalidate_timeline(void) { s_valid = false; }

void blk_time_free(void)
{
    surf_free(&s_strip);
    s_valid = false;
}

static double chart_y_of(const TlGeom *G, double f)
{
    int top = dp(14), bot = dp(4);   /* web: 16 / 6 of 70 px */
    double y = G->chart_y + top + ((IWD_CHART_T_MAX - f) / (double)(IWD_CHART_T_MAX - IWD_CHART_T_MIN)) * (G->chart_h - top - bot);
    return clampd(y, G->chart_y + 1, G->chart_y + G->chart_h - 1);
}

/* spans of hours (0..24) where pred holds, like app.js spans() */
typedef bool (*HourPred)(const IwdDay *d, int h, int k);

static bool pred_window(const IwdDay *d, int h, int k) { (void)d; return iwd_in_window(h, iwd_cycle((IwdCycleKey)k)); }
static bool pred_lit(const IwdDay *d, int h, int k)
{
    const IwdEventDef *ev = iwd_event(d->hour_id[h]);
    if (!ev) return false;
    if (k == IWD_CYCLE_AURORA) return iwd_light_at(ev, h).aurora_visible;
    return iwd_in_window(h, iwd_cycle((IwdCycleKey)k)) && ev->light == IWD_EVLIGHT_DIM;
}

/* draws in strip-local coordinates: (ox, oy) is the strip origin in back-buffer coords */
static void band_spans(const TlGeom *G, int ox, int oy, const IwdDay *d, int k, HourPred pred, bool lit)
{
    static const DWORD lit_a[3] = { 0xDFE9FF, 0xFFD6DE, 0x7DFFCF };
    static const DWORD lit_b[3] = { 0xB8C8FF, 0xFFE9D2, 0x8FB8FF };
    int h, start = -1;
    int ry = G->band_y + k * G->band_h - oy, bh = dp(6) < 4 ? 4 : dp(6);
    int y0 = ry + (G->band_h - bh) / 2, y1 = y0 + bh;
    for (h = 0; h <= 24; h++) {
        bool on = h < 24 && pred(d, h, k);
        if (on && start < 0) start = h;
        if (!on && start >= 0) {
            int x0 = hour_x(G, start) - ox, x1 = hour_x(G, h) - ox, x;
            if (!lit) {
                px_blend(x0, y0, x1, y1, 0xC8EBFF, 26);
                px_frame(x0, y0, x1, y1, 1, mix(C_PANEL, 0xC8EBFF, 40));
            } else {
                for (x = x0; x < x1; x++) {
                    DWORD c = mix(lit_a[k], lit_b[k], x1 - x0 > 1 ? (x - x0) * 256 / (x1 - x0 - 1) : 0);
                    px_fill(x, y0, x + 1, y1, c);
                    px_blend(x, y0 - 2, x + 1, y0, c, 40);      /* soft glow */
                    px_blend(x, y1, x + 1, y1 + 2, c, 40);
                }
            }
            start = -1;
        }
    }
}

/* one segment of the bar: a slice of the event's scene, hours start..end of its width */
static void bar_segment(const TlGeom *G, int ox, int oy, const IwdSegment *s, int i, bool hover_name_room)
{
    const IwdEventDef *ev = iwd_event(s->id);
    const Image *im = img_med(s->id);
    int x0 = hour_x(G, s->start) - ox, x1 = hour_x(G, s->end) - ox;
    int y0 = G->bar_y - oy, y1 = y0 + G->bar_h, sw = x1 - x0, x, y;
    DWORD col = iwd_event_rgb(s->id);
    if (sw <= 0) return;
    SetRect(&s_seg_rc[i], x0, y0, x1, y1);
    if (im && im->px && s_strip.px) {
        double sx0 = s->start / 24.0 * im->w, sx1 = s->end / 24.0 * im->w;
        double band = (double)im->w / (G->tx1 - G->tx0) * G->bar_h;     /* same scale both ways */
        double cy = im->h * 0.45, sy0, sy1;
        if (band > im->h) band = im->h;
        sy0 = cy - band / 2;
        if (sy0 < 0) sy0 = 0;
        if (sy0 + band > im->h) sy0 = im->h - band;
        sy1 = sy0 + band;
        GdiFlush();
        if (x0 >= 0 && x1 <= s_strip.w && y0 >= 0 && y1 <= s_strip.h)
            resample_box(im, sx0, sy0, sx1, sy1, s_strip.px + (size_t)y0 * s_strip.w + x0, sw, G->bar_h, s_strip.w);
    } else {
        px_vgrad(x0, y0, x1, y1, mix(col, 0xFFFFFF, 20), mix(col, 0x0B2440, 77));
    }
    /* legibility: darken toward the bottom where the label sits */
    px_vgrad_a(x0, y0 + G->bar_h / 3, x1, y1, 0x020A16, 0, 120);
    /* event colour rule */
    px_fill(x0, y0, x1, y0 + (dp(3) < 2 ? 2 : dp(3)), col);
    /* hatched continuation edges (cont-l / cont-r) */
    if (s->from_prev_day || s->into_next_day) {
        int hw = dp(8) < 6 ? 6 : dp(8), st = dp(3) < 2 ? 2 : dp(3);
        GdiFlush();
        for (y = y0; y < y1; y++) for (x = 0; x < hw && x < sw; x++) {
            if (((x + (y - y0)) / st) & 1) continue;
            if (s->from_prev_day) pblend(x0 + x, y, 0x020A16, 166);
            if (s->into_next_day) pblend(x1 - 1 - x, y, 0x020A16, 166);
        }
    }
    /* separator */
    px_fill(x1 - 1, y0, x1, y1, mix(0x020A16, C_PANEL, 64));
    /* label = event name when the block is >= 64 px wide */
    if (ev && hover_name_room && sw >= dp(64)) {
        int lx = x0 + (s->from_prev_day ? dp(14) : dp(8)), rw = x1 - lx - dp(8);
        int ly = y1 - fh(F_SMALL_B) - dp(5);
        WCHAR name[64];
        w8(ev->name, name, 64);
        if (rw > dp(16)) {
            if (tw(F_SMALL_B, name, -1) <= rw) tx_sh(F_SMALL_B, lx, ly, name, -1, 0xF2FAFF);
            else {
                tx_ellipsis(F_SMALL_B, lx + 1, ly + 1, rw, name, C_SHADOW);
                tx_ellipsis(F_SMALL_B, lx, ly, rw, name, 0xF2FAFF);
            }
        }
    }
    /* tooltip: "<name> · HH:00–HH:00 · Xd4 [r1+r2] = D h[ · continues ...]" */
    {
        char a[8], b[8], rolls[16], t[256];
        iwd_fmt_hour(s->start, a, sizeof a);
        if (s->end >= 24) strcpy(b, "24:00"); else iwd_fmt_hour(s->end, b, sizeof b);
        if (s->n_rolls >= 2) snprintf(rolls, sizeof rolls, "%d+%d", s->rolls[0], s->rolls[1]);
        else snprintf(rolls, sizeof rolls, "%d", s->rolls[0]);
        snprintf(t, sizeof t, "%s \xC2\xB7 %s\xE2\x80\x93%s \xC2\xB7 %dd4 [%s] = %d h%s%s",
                 ev ? ev->name : "?", a, b, ev ? ev->dice : s->n_rolls, rolls, s->duration,
                 s->from_prev_day ? " \xC2\xB7 continues from the previous day" : "",
                 s->into_next_day ? " \xC2\xB7 continues into the next day" : "");
        w8(t, s_seg_tip[i], 200);
    }
}

/* the temperature trace: horizontal-tangent cubic Beziers through h+0.2 / h+0.8 samples */
#define TR_PTS 50
#define TR_SUB 10
static void chart(const TlGeom *G, int ox, int oy, const IwdDay *d)
{
    double px[TR_PTS], py[TR_PTS];
    double dx[(TR_PTS - 1) * TR_SUB + 1], dy[(TR_PTS - 1) * TR_SUB + 1];
    int n = 0, nd = 0, i, k, W = G->tx1 - G->tx0;
    int *ycol; double *ycf;
    uint32_t th = g.a.temp_hash;
    bool cel = g.a.celsius;
    double x0 = G->tx0 - ox;
    int cb = G->chart_y + G->chart_h - oy;       /* chart bottom (strip coords) */

    /* grid + labels */
    {
        static const int gf[3] = { -50, -90, -130 }, gc[3] = { -45, -65, -85 };
        for (i = 0; i < 3; i++) {
            int f = cel ? (gc[i] * 9) / 5 + 32 : gf[i];
            int gy = (int)floor(chart_y_of(G, f) + 0.5) - oy;
            char t[16]; WCHAR wt[16];
            px_blend((int)x0, gy, (int)x0 + W, gy + 1, 0xC8EBFF, 26);
            iwd_fmt_temp(f, cel, t, sizeof t); w8(t, wt, 16);
            tx(F_TINY, (int)x0 - dp(8) - tw(F_TINY, wt, -1), gy - fh(F_TINY) / 2, wt, -1, C_DIM);
        }
    }
    /* sample points */
    px[n] = 0; py[n] = chart_y_of(G, iwd_day_temp_at(d, th, 0)); n++;
    for (i = 0; i < 24; i++) {
        px[n] = (i + 0.2) / 24.0 * W; py[n] = chart_y_of(G, iwd_day_temp_at(d, th, i + 0.2)); n++;
        px[n] = (i + 0.8) / 24.0 * W; py[n] = chart_y_of(G, iwd_day_temp_at(d, th, i + 0.8)); n++;
    }
    px[n] = W; py[n] = chart_y_of(G, iwd_day_temp_at(d, th, 23.999)); n++;
    /* flatten the Bezier chain */
    dx[nd] = px[0]; dy[nd] = py[0]; nd++;
    for (i = 1; i < n; i++) {
        double ax = px[i - 1], ay = py[i - 1], bx = px[i], by = py[i], mx = (ax + bx) / 2;
        for (k = 1; k <= TR_SUB; k++) {
            double t = (double)k / TR_SUB, u = 1 - t;
            double xx = u * u * u * ax + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * bx;
            double yy = u * u * u * ay + 3 * u * u * t * ay + 3 * u * t * t * by + t * t * t * by;
            dx[nd] = xx; dy[nd] = yy; nd++;
        }
    }
    /* per-column curve height -> area fill and glow (pixel exact, no overlap artefacts) */
    ycol = (int *)malloc(sizeof(int) * (size_t)(W + 1));
    ycf = (double *)malloc(sizeof(double) * (size_t)(W + 1));
    if (ycol && ycf) {
        int seg = 0, c, ymin = cb;
        for (c = 0; c <= W; c++) {
            double cxp = c + 0.5, t, v;
            while (seg < nd - 2 && dx[seg + 1] < cxp) seg++;
            t = dx[seg + 1] > dx[seg] ? (cxp - dx[seg]) / (dx[seg + 1] - dx[seg]) : 0;
            t = clampd(t, 0, 1);
            v = dy[seg] + (dy[seg + 1] - dy[seg]) * t - oy;
            ycf[c] = v; ycol[c] = (int)floor(v);
            if (ycol[c] < ymin) ymin = ycol[c];
        }
        GdiFlush();
        for (c = 0; c < W; c++) {
            int X = (int)x0 + c, yy, top = ycol[c] + 1;
            for (yy = top; yy < cb; yy++) {
                /* web: #bfe6fb stop-opacity .55 -> 0 over the area's box, path opacity .55 */
                double a = 0.55 * 0.55 * (1.0 - (double)(yy - ymin) / (cb - ymin > 0 ? cb - ymin : 1));
                pblend(X, yy, 0xBFE6FB, (int)(a * 256));
            }
            for (yy = ycol[c] - dp(4); yy <= ycol[c] + dp(4); yy++) {
                double dd = fabs(yy + 0.5 - ycf[c]) / (dp(4) + 1.0);
                if (dd < 1) pblend(X, yy, 0x96D7FF, (int)(70 * (1 - dd) * (1 - dd)));
            }
        }
    }
    free(ycol); free(ycf);
    /* the line itself, antialiased */
    for (i = 1; i < nd; i++)
        aa_line(x0 + dx[i - 1], dy[i - 1] - oy, x0 + dx[i], dy[i] - oy, 0xD4EEFC, 255, dp(2) > 2 ? dp(2) * 0.9 : 1.8);
    aa_flush();
    /* per-segment labels */
    {
        double last = -1e9;
        for (i = 0; i < d->n_segments; i++) {
            const IwdSegment *s = &d->segments[i];
            double mid, lx, ly, a, b, c;
            int f; char t[16]; WCHAR wt[16];
            if (s->end - s->start < 2) continue;
            mid = (s->start + s->end) / 2.0;
            lx = mid / 24.0 * W;
            if (lx - last < dp(30)) continue;
            last = lx;
            f = d->temp_f[(int)mid < 23 ? (int)mid : 23];
            a = chart_y_of(G, iwd_day_temp_at(d, th, s->start + 0.2));
            b = chart_y_of(G, iwd_day_temp_at(d, th, mid));
            c = chart_y_of(G, iwd_day_temp_at(d, th, s->end - 0.2));
            ly = a < b ? a : b; if (c < ly) ly = c;
            iwd_fmt_temp(f, cel, t, sizeof t); w8(t, wt, 16);
            {
                int ty = (int)(ly - oy) - dp(6) - fh(F_TINY_B) * 4 / 5;
                if (ty < G->chart_y - oy - dp(2)) ty = G->chart_y - oy - dp(2);
                tx(F_TINY_B, (int)(x0 + lx) - tw(F_TINY_B, wt, -1) / 2, ty, wt, -1, C_BRIGHT);
            }
        }
    }
}

static void ticks(const TlGeom *G, int ox, int oy)
{
    int h, y = G->tick_y - oy + dp(2);
    for (h = 0; h <= 24; h += 3) {
        int x = hour_x(G, h) - ox, lw, lx;
        WCHAR t[4];
        int v = (h % 24 == 0 && h) ? 24 : h;
        t[0] = (WCHAR)(L'0' + v / 10); t[1] = (WCHAR)(L'0' + v % 10); t[2] = 0;
        px_blend(x, y, x + 1, y + dp(4), 0xC8EBFF, 77);
        lw = tw(F_TINY, t, 2);
        lx = x - lw / 2;
        if (lx + lw > s_strip.w) lx = s_strip.w - lw;
        tx(F_TINY, lx, y + dp(5), t, 2, C_DIM);
    }
}

static void strip_build(const TlGeom *G, const IwdDay *d)
{
    Surface *save = g_tgt;
    int ox = G->cx0, oy = G->cy0, k;
    int bx0 = G->tx0 - ox, bx1 = G->tx1 - ox, by0 = G->bar_y - oy, by1 = by0 + G->bar_h;
    static const WCHAR *names[3] = { L"Moon", L"Twilight", L"Aurora" };
    s_nseg = 0;
    if (!surf_resize(&s_strip, G->cw, G->ch)) { s_valid = false; return; }
    g_tgt = &s_strip;
    GdiFlush();
    px_fill(0, 0, s_strip.w, s_strip.h, C_PANEL);
    /* cycle bands */
    for (k = 0; k < 3; k++) {
        int ry = G->band_y + k * G->band_h - oy;
        tx_caps(F_CAPS_S, bx0 - dp(8) - tw_caps(F_CAPS_S, names[k]), ry + (G->band_h - fh(F_CAPS_S)) / 2, names[k], C_DIM);
        band_spans(G, ox, oy, d, k, pred_window, false);
        if (d) band_spans(G, ox, oy, d, k, pred_lit, true);
    }
    /* bar background + frame */
    px_fill(bx0, by0, bx1, by1, mix(C_PANEL, 0x020A16, 128));
    if (d) {
        int i;
        chart(G, ox, oy, d);
        s_nseg = d->n_segments;
        for (i = 0; i < d->n_segments; i++) bar_segment(G, ox, oy, &d->segments[i], i, true);
    } else {
        const WCHAR *e = L"No days recorded yet — begin the expedition.";
        int ew = tw(F_SMALL, e, -1);
        if (ew <= bx1 - bx0) tx(F_SMALL, (bx0 + bx1 - ew) / 2, (by0 + by1 - fh(F_SMALL)) / 2, e, -1, C_DIM);
        else tx_ellipsis(F_SMALL, bx0 + dp(6), (by0 + by1 - fh(F_SMALL)) / 2, bx1 - bx0 - dp(12), e, C_DIM);
    }
    px_frame(bx0, by0, bx1, by1, 1, mix(C_PANEL, 0xC8EBFF, 41));
    ticks(G, ox, oy);
    aa_flush();
    GdiFlush();
    g_tgt = save;
    s_valid = true;
}

static void strip_ensure(const TlGeom *G)
{
    const IwdDay *d = viewed_day();
    StripKey k;
    memset(&k, 0, sizeof k);
    k.epoch = g.a.epoch; k.view = g.a.view; k.n_days = g.a.n_days; k.celsius = g.a.celsius;
    k.has_view = d != NULL; k.cw = G->cw; k.ch = G->ch; k.gut = G->gut; k.dpi = g.dpi;
    k.tw_ = G->tx1 - G->tx0;
    if (s_valid && s_strip.px && memcmp(&k, &s_key, sizeof k) == 0) return;
    s_key = k;
    strip_build(G, d);
}

/* ================================================================== timeline: live parts */

static void button(int x0, int y0, int x1, int y1, int id, bool en, const WCHAR *label, const WCHAR *tip, int glyph)
{
    bool hot = g.hot_id == id, press = hot && g.press_id == id && en;
    DWORD fill = !en ? mix(C_PANEL, C_PANEL2, 120) : press ? C_FIELD : hot ? mix(C_PANEL2, C_ACCENT, 34) : C_PANEL2;
    DWORD border = !en ? C_LINE : hot ? mix(C_FRAME, C_ACCENT, 140) : C_FRAME;
    DWORD ink = en ? C_BRIGHT : C_DIM;
    int lw = label ? tw(F_BTN, label, -1) : 0, gw = glyph ? dp(10) : 0, gap = (glyph && label) ? dp(6) : 0;
    int cx = (x0 + x1 - (lw + gw + gap)) / 2, cy = (y0 + y1) / 2;
    px_box(x0, y0, x1, y1, fill, border);
    if (glyph == 1) {                       /* play */
        double xy[6];
        xy[0] = cx; xy[1] = cy - dp(5); xy[2] = cx + dp(9); xy[3] = cy; xy[4] = cx; xy[5] = cy + dp(5);
        aa_poly(xy, 3, ink, 255);
        aa_flush();
    } else if (glyph == 2) {                /* pause */
        int bw = dp(3) < 2 ? 2 : dp(3);
        px_fill(cx + dp(1), cy - dp(5), cx + dp(1) + bw, cy + dp(5), ink);
        px_fill(cx + dp(6), cy - dp(5), cx + dp(6) + bw, cy + dp(5), ink);
    }
    if (label) tx(F_BTN, cx + gw + gap, cy - fh(F_BTN) / 2, label, -1, ink);
    hot_add(x0, y0, x1, y1, id, 0, en, tip);
}

static void tl_head(const TlGeom *G)
{
    AlmControls c;
    WCHAR nav[96];
    int bh = dp(26), by = G->head_y + (G->head_h - bh) / 2;
    int xr = G->x + G->w - G->pad, play_w = dp(96), ab = dp(28), lab_w, xl;
    bool can_play = tl_interactive();
    alm_controls(&g.a, &c);
    w8(c.day_nav, nav, 96);
    lab_w = tw(F_SMALL_B, nav, -1) + dp(16);
    if (lab_w < dp(150)) lab_w = dp(150);
    /* play / pause */
    button(xr - play_w, by, xr, by + bh, H_PLAY, can_play,
           g.a.playing ? L"Pause" : L"Play", g.a.playing ? L"Pause (P)" : L"Play the day (P)", g.a.playing ? 2 : 1);
    {   /* key cap at the button's right end */
        int kw = tw(F_TINY_B, L"P", -1) + dp(8), kx = xr - dp(6) - kw, ky = by + (bh - fh(F_TINY_B)) / 2 - dp(1);
        px_frame(kx, ky, kx + kw, ky + fh(F_TINY_B) + dp(2), 1, can_play ? C_FRAME : C_LINE);
        tx(F_TINY_B, kx + dp(4), ky + dp(1), L"P", -1, can_play ? C_MUTED : C_DIM);
    }
    xr -= play_w + dp(10);
    button(xr - ab, by, xr, by + bh, H_DAY_NEXT, c.next_enabled, L"›", L"Next day (PgDn)", 0);
    xr -= ab;
    xl = xr - lab_w;
    {
        int nw = tw(F_SMALL_B, nav, -1);
        if (nw <= lab_w - dp(8)) tx(F_SMALL_B, xl + (lab_w - nw) / 2, G->head_y + (G->head_h - fh(F_SMALL_B)) / 2, nav, -1, C_ICE);
        else tx_ellipsis(F_SMALL_B, xl + dp(4), G->head_y + (G->head_h - fh(F_SMALL_B)) / 2, lab_w - dp(8), nav, C_ICE);
    }
    button(xl - ab, by, xl, by + bh, H_DAY_PREV, c.prev_enabled, L"‹", L"Previous day (PgUp)", 0);
    /* title, if it fits */
    {
        const WCHAR *t = L"The Day's Weather";
        int room = xl - ab - dp(12) - (G->x + G->pad);
        if (tw_caps(F_CAPS, t) <= room) tx_caps(F_CAPS, G->x + G->pad, G->head_y + (G->head_h - fh(F_CAPS)) / 2, t, C_SOFT);
    }
}

static void tl_live(const TlGeom *G)
{
    const IwdDay *d = viewed_day();
    bool en = tl_interactive();
    int i, ox = G->cx0, oy = G->cy0;
    /* hit regions: whole strip area first (arg -1), then the bar segments on top */
    hot_add(G->tx0, G->chart_y, G->tx1, G->tick_y + G->tick_h, H_TIMELINE, -1, en, NULL);
    if (d) {
        for (i = 0; i < s_nseg && i < d->n_segments; i++) {
            RECT r = s_seg_rc[i];
            hot_add(r.left + ox, r.top + oy, r.right + ox, r.bottom + oy, H_TIMELINE, i, en, s_seg_tip[i]);
        }
    }
    /* hover outline on the segment under the mouse */
    if (d && g.hot_id == H_TIMELINE && g.hot_arg >= 0 && g.hot_arg < s_nseg && !g.scrubbing) {
        RECT r = s_seg_rc[g.hot_arg];
        int x0 = r.left + ox, x1 = r.right + ox, y0 = r.top + oy, y1 = r.bottom + oy;
        px_blend(x0, y0, x1, y0 + 1, 0xFFFFFF, 200); px_blend(x0, y1 - 1, x1, y1, 0xFFFFFF, 200);
        px_blend(x0, y0 + 1, x0 + 1, y1 - 1, 0xFFFFFF, 200); px_blend(x1 - 1, y0 + 1, x1, y1 - 1, 0xFFFFFF, 200);
    }
    /* focus ring while scrubbing */
    if (g.scrubbing && d) {
        int o = dp(2);
        px_frame(G->tx0 - o, G->bar_y - o, G->tx1 + o, G->bar_y + G->bar_h + o, 1, C_ACCENT);
    }
    if (!d) return;
    /* playhead */
    {
        double t = g.a.time < 23.999 ? g.a.time : 23.999, k = t / 24.0;
        int W = G->tx1 - G->tx0, x = G->tx0 + (int)floor(k * W + 0.5);
        int y0 = G->bar_y - dp(4), y1 = G->bar_y + G->bar_h + dp(4), lw = dp(2) < 2 ? 2 : dp(2);
        char ck[8]; WCHAR wc[8];
        int tw_, th, bx, by;
        double cy = chart_y_of(G, iwd_day_temp_at(d, g.a.temp_hash, t));
        px_blend(x - lw / 2 - 2, y0, x - lw / 2, y1, 0xFFFFFF, 60);
        px_blend(x - lw / 2 + lw, y0, x - lw / 2 + lw + 2, y1, 0xFFFFFF, 60);
        px_fill(x - lw / 2, y0, x - lw / 2 + lw, y1, 0xFFFFFF);
        aa_circle(x, y0 + dp(1), dp(7), 0xC8F0FF, 70);
        aa_circle(x, y0 + dp(1), dp(4), 0xFFFFFF, 255);
        /* chart dot */
        aa_circle(G->tx0 + k * W, cy, dp(8), 0xBFE6FB, 60);
        aa_circle(G->tx0 + k * W, cy, dp(4) + 0.5, 0xFFFFFF, 255);
        aa_flush();
        /* time label (top of the bar), flips to the left past 88% */
        iwd_fmt_clock(g.a.time, ck, sizeof ck); w8(ck, wc, 8);
        tw_ = tw(F_TINY_B, wc, -1) + dp(12); th = fh(F_TINY_B) + dp(2);
        by = G->bar_y + dp(6);            /* top of the bar: segment names sit at the bottom */
        bx = k > 0.88 ? x - dp(8) - tw_ : x + dp(8);
        px_blend(bx, by, bx + tw_, by + th, 0x020A16, 205);
        tx(F_TINY_B, bx + dp(6), by + dp(1), wc, -1, C_BRIGHT);
    }
    (void)ox; (void)oy;
}

/* ================================================================== ledger */

typedef struct LgCtx { int x, y, w; bool draw; } LgCtx;

#define LG_SP 12
typedef struct LgLine { Span sp[LG_SP]; WCHAR buf[LG_SP][200]; int n; } LgLine;

static void lg_add(LgLine *L, const char *utf8, int fnt, int flags, DWORD c)
{
    if (L->n >= LG_SP) return;
    w8(utf8, L->buf[L->n], 200);
    L->sp[L->n].s = L->buf[L->n]; L->sp[L->n].n = -1;
    L->sp[L->n].font = (BYTE)fnt; L->sp[L->n].flags = (BYTE)flags; L->sp[L->n].color = c;
    L->n++;
}


/* width of a span line laid out on one row (boxes as rich() draws them) */
static int lg_width(const LgLine *L)
{
    int i, w = 0;
    for (i = 0; i < L->n; i++) {
        w += tw(L->sp[i].font, L->sp[i].s, -1);
        if (L->sp[i].flags & SP_BOX) w += 2 * dp(4);
    }
    return w;
}

/* One ledger row, unboxed: time | "Event → Next" | the roll, right-aligned. Falls back to
 * two lines (roll below) when the column is too narrow. A 1 px rule separates rows; the
 * carried-over first row gets a dashed aurora rule on its left instead. */
static int lg_row(LgCtx *C, int y, const char *time, LgLine *ev, LgLine *rolls, bool carry)
{
    int tcol = dp(46), gap = dp(10), bx = C->x + tcol, bw = C->w - tcol;
    int lh = fh(F_SMALL) + dp(10), ew = lg_width(ev), rw = lg_width(rolls), h, he = 0;
    bool one = ew + gap + rw <= bw;
    if (one) h = lh;
    else {
        he = rich(ev->sp, ev->n, bx, 0, bw, fh(F_SMALL_B), false);
        h = dp(5) + he + rich(rolls->sp, rolls->n, bx, 0, bw, fh(F_SMALL), false) + dp(6);
    }
    if (C->draw && g_tgt && y < g_tgt->h && y + h > 0) {
        WCHAR wt[8];
        int ty = one ? y + (lh - fh(F_SMALL_B)) / 2 : y + dp(5);
        px_fill(C->x, y + h - 1, C->x + C->w, y + h, C_LINE);
        if (carry) {
            int d = dp(3) < 2 ? 2 : dp(3), yy;
            for (yy = y + dp(2); yy < y + h - dp(2); yy += 2 * d)
                px_fill(C->x, yy, C->x + dp(2), yy + d < y + h - dp(2) ? yy + d : y + h - dp(2), C_AURORA);
        }
        w8(time, wt, 8);
        tx(F_MONO, C->x + (carry ? dp(8) : 0), ty + (fh(F_SMALL_B) - fh(F_MONO)) / 2, wt, -1, C_MUTED);
        rich(ev->sp, ev->n, bx, ty, bw, fh(F_SMALL_B), true);
        if (one) rich(rolls->sp, rolls->n, C->x + C->w - rw, ty, rw + dp(2), fh(F_SMALL_B), true);
        else rich(rolls->sp, rolls->n, bx, ty + he, bw, fh(F_SMALL), true);
    }
    return h;
}

#define DIE_RGB 0xBFE4F6

static int ledger_layout(int x, int y, int w, bool draw)
{
    LgCtx C;
    int pad = dp(14), yy = y + dp(10), gap = 0;
    const IwdDay *d = viewed_day();
    static LgLine e, r;            /* large; not re-entrant (UI thread only) */
    C.x = x + pad; C.w = w - 2 * pad; C.draw = draw; C.y = y;
    if (C.w < dp(120)) C.w = dp(120);
    /* head: "Dice Ledger" + note */
    if (draw) {
        const WCHAR *t = L"Dice Ledger", *nt = L"every roll of the viewed day";
        int hy = yy + (dp(28) - fh(F_CAPS)) / 2, tw0 = tw_caps(F_CAPS, t);
        tx_caps(F_CAPS, C.x, hy, t, C_SOFT);
        if (tw0 + dp(16) + tw(F_SMALL_I, nt, -1) <= C.w)
            tx_right(F_SMALL_I, C.x + C.w, yy + (dp(28) - fh(F_SMALL_I)) / 2, nt, C_DIM);
    }
    yy += dp(28);
    if (!d) {
        if (draw) tx_caps(F_CAPS, C.x + dp(2), yy + dp(4), L"No rolls yet", C_SOFT);
        yy += fh(F_CAPS) + dp(10);
        return yy - y + dp(6);
    }
    /* day break */
    {
        char date[IWD_DATE_CAP], t[128]; WCHAR wt[128];
        iwd_format_date(alm_abs_of(&g.a, g.a.view), false, date, sizeof date);
        snprintf(t, sizeof t, "Day %d \xC2\xB7 %s", (int)d->day, date);
        w8(t, wt, 128);
        if (draw) tx_caps(F_CAPS_S, C.x, yy + dp(2), wt, 0xA9CDE6);
        yy += fh(F_CAPS_S) + dp(6);
        if (draw) px_fill(C.x, yy - 1, C.x + C.w, yy, C_LINE);
    }
    /* carried in */
    if (d->n_segments > 0 && d->segments[0].from_prev_day && d->has_carried_in) {
        const IwdActive *c = &d->carried_in;
        const IwdEventDef *ev = iwd_event(c->id), *nx = iwd_event(c->next);
        char t[200];
        e.n = 0; r.n = 0;
        snprintf(t, sizeof t, "%s continues", ev ? ev->name : "?");
        lg_add(&e, t, F_SMALL_B, 0, C_TEXT);
        snprintf(t, sizeof t, " \xE2\x86\x92 %s", nx ? nx->name : "?");
        lg_add(&e, t, F_SMALL, 0, C_SOFT);
        snprintf(t, sizeof t, "carried over \xC2\xB7 %d of %d h remain", c->duration - c->elapsed, c->duration);
        lg_add(&r, t, F_SMALL, 0, C_MUTED);
        yy += lg_row(&C, yy, "00:00", &e, &r, true) + gap;
    }
    /* log entries */
    {
        const IwdSegment *log[IWD_MAX_SEGMENTS];
        int n = iwd_day_log(d, log), i, k;
        for (i = 0; i < n; i++) {
            const IwdSegment *s = log[i];
            const IwdEventDef *ev = iwd_event(s->id), *nx = iwd_event(s->next);
            char t[200], hh[8], num[8], opt[32];
            size_t ol;
            e.n = 0; r.n = 0;
            if (!ev) continue;
            lg_add(&e, ev->name, F_SMALL_B, 0, C_TEXT);
            snprintf(t, sizeof t, " \xE2\x86\x92 %s", nx ? nx->name : "?");
            lg_add(&e, t, F_SMALL, 0, C_SOFT);
            if (s->cause_type == IWD_CAUSE_GENESIS) {
                lg_add(&r, "d12 ", F_SMALL, 0, C_MUTED);
                snprintf(num, sizeof num, "%d", s->cause_arg);
                lg_add(&r, num, F_NUM, SP_BOX, C_AURORA);
                lg_add(&r, " \xC2\xB7 ", F_SMALL, 0, C_DIM);
            }
            snprintf(t, sizeof t, "%dd4 ", ev->dice);
            lg_add(&r, t, F_SMALL, 0, C_MUTED);
            for (k = 0; k < s->n_rolls && k < 2; k++) {
                if (k) lg_add(&r, " ", F_SMALL, 0, C_MUTED);
                snprintf(num, sizeof num, "%d", s->rolls[k]);
                lg_add(&r, num, F_NUM, SP_BOX, DIE_RGB);
            }
            lstrcpynA(opt, iwd_next_option_label(ev, s->next_idx), (int)sizeof opt);
            ol = strlen(opt);
            if (ol && opt[ol - 1] == ':') opt[ol - 1] = 0;
            snprintf(t, sizeof t, " = %d h \xC2\xB7 %s", s->duration, opt);
            lg_add(&r, t, F_SMALL, 0, C_MUTED);
            iwd_fmt_hour(s->start, hh, sizeof hh);
            yy += lg_row(&C, yy, hh, &e, &r, false) + gap;
        }
    }
    return yy - y + dp(10);
}

/* ================================================================== block entry points */

int blk_time_measure(int which, int w)
{
    if (which == BLK_TIMELINE) return tl_height();
    if (which == BLK_LEDGER) return ledger_layout(0, 0, w, false);
    return 0;
}

static void panel_bg(int x, int y, int w, int h)
{
    px_fill(x, y, x + w, y + h, C_PANEL);
    px_frame(x, y, x + w, y + h, 1, C_LINE);
}

void blk_time_paint(int which, int x, int y, int w, int h)
{
    if (!g_tgt || w <= 0 || h <= 0) return;
    if (which == BLK_TIMELINE) {
        TlGeom G;
        tl_geom(x, y, w, &G);
        s_last = G; s_last_ok = true;
        if (y >= g_tgt->h || y + h <= 0) return;           /* off-screen: nothing to show */
        panel_bg(x, y, w, h);
        strip_ensure(&G);
        if (s_valid && s_strip.px) {
            GdiFlush();
            px_copy(s_strip.px, s_strip.w, s_strip.h, G.cx0, G.cy0);
        }
        tl_head(&G);
        tl_live(&G);
    } else if (which == BLK_LEDGER) {
        if (y >= g_tgt->h || y + h <= 0) return;
        panel_bg(x, y, w, h);
        ledger_layout(x, y, w, true);
    }
}

/* ================================================================== ui.h queries */

double view_timeline_time(int x)
{
    double k;
    int W = s_last.tx1 - s_last.tx0;
    if (!s_last_ok || W <= 0) return g.a.time;
    k = (double)(x - s_last.tx0) / W;
    k = clampd(k, 0.0, 0.9999);            /* app.js setTimeFromPointer */
    return k * 24.0;
}

bool view_timeline_hit(int x, int y)
{
    if (!s_last_ok || !viewed_day()) return false;
    return x >= s_last.tx0 && x < s_last.tx1 && y >= s_last.chart_y && y < s_last.tick_y + s_last.tick_h;
}
