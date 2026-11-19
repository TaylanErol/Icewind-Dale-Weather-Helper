/* ui_draw.c - drawing primitives of the native UI: back-buffer surfaces, pixel fills,
 * fonts and text (plain, wrapped, rich), antialiased GDI+ shapes, lazy JPEG decoding,
 * a bicubic resampler and the cached hero scene composite with its light overlays.
 * No GDI object is created per paint: fonts exist once per DPI, the GDI+ brush, pen and
 * path are created once and recoloured.
 */
#include "ui.h"

#include <objidl.h>
#include <shlwapi.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

Surface *g_tgt;
HotList *g_hl;

/* ================================================================== GDI+ flat API */

typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupInput;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } GpBitmapData;
typedef struct { INT X, Y, Width, Height; } GpRectI;
typedef struct { float X, Y; } GpPointF;
int  WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
int  WINAPI GdipGetImageWidth(void *image, UINT *width);
int  WINAPI GdipGetImageHeight(void *image, UINT *height);
int  WINAPI GdipBitmapLockBits(void *bitmap, const GpRectI *rect, UINT flags, INT format, GpBitmapData *data);
int  WINAPI GdipBitmapUnlockBits(void *bitmap, GpBitmapData *data);
int  WINAPI GdipDisposeImage(void *image);
int  WINAPI GdipCreateFromHDC(HDC dc, void **g);
int  WINAPI GdipDeleteGraphics(void *g);
int  WINAPI GdipSetSmoothingMode(void *g, int mode);
int  WINAPI GdipSetPixelOffsetMode(void *g, int mode);
int  WINAPI GdipFlush(void *g, int intention);
int  WINAPI GdipCreateSolidFill(DWORD argb, void **brush);
int  WINAPI GdipSetSolidFillColor(void *brush, DWORD argb);
int  WINAPI GdipDeleteBrush(void *brush);
int  WINAPI GdipCreatePen1(DWORD argb, float width, int unit, void **pen);
int  WINAPI GdipSetPenColor(void *pen, DWORD argb);
int  WINAPI GdipSetPenWidth(void *pen, float width);
int  WINAPI GdipDeletePen(void *pen);
int  WINAPI GdipCreatePath(int fill_mode, void **path);
int  WINAPI GdipResetPath(void *path);
int  WINAPI GdipDeletePath(void *path);
int  WINAPI GdipAddPathArc(void *path, float x, float y, float w, float h, float start, float sweep);
int  WINAPI GdipAddPathLine(void *path, float x1, float y1, float x2, float y2);
int  WINAPI GdipAddPathPolygon(void *path, const GpPointF *pts, INT n);
int  WINAPI GdipClosePathFigure(void *path);
int  WINAPI GdipFillPath(void *g, void *brush, void *path);
int  WINAPI GdipFillEllipse(void *g, void *brush, float x, float y, float w, float h);
int  WINAPI GdipDrawEllipse(void *g, void *pen, float x, float y, float w, float h);
int  WINAPI GdipDrawLine(void *g, void *pen, float x1, float y1, float x2, float y2);
#define GP_PIXFMT_32RGB 0x00022009

static ULONG_PTR g_gdip;
static void *g_brush, *g_pen, *g_path;

void gdip_up(void)
{
    if (!g_gdip) {
        GpStartupInput in = { 1, NULL, FALSE, FALSE };
        if (GdiplusStartup(&g_gdip, &in, NULL) != 0) g_gdip = 0;
    }
    if (g_gdip && !g_brush) {
        GdipCreateSolidFill(0xFFFFFFFF, &g_brush);
        GdipCreatePen1(0xFFFFFFFF, 1.0f, 2, &g_pen);
        GdipCreatePath(0, &g_path);
    }
}

void gdip_down(void)
{
    if (g_brush) { GdipDeleteBrush(g_brush); g_brush = NULL; }
    if (g_pen)   { GdipDeletePen(g_pen); g_pen = NULL; }
    if (g_path)  { GdipDeletePath(g_path); g_path = NULL; }
    if (g_gdip)  { GdiplusShutdown(g_gdip); g_gdip = 0; }
}

static DWORD argb(DWORD c, int a) { return ((DWORD)clampi(a, 0, 255) << 24) | (c & 0xFFFFFF); }

/* ================================================================== surfaces */

bool surf_resize(Surface *s, int w, int h)
{
    BITMAPINFO bi; void *bits = NULL; HBITMAP bmp;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (s->bmp && s->w == w && s->h == h) return true;
    if (!s->dc) {
        s->dc = CreateCompatibleDC(NULL);
        SetBkMode(s->dc, TRANSPARENT);
    }
    if (s->gfx) { GdipDeleteGraphics(s->gfx); s->gfx = NULL; }
    if (s->bmp) { SelectObject(s->dc, s->old); DeleteObject(s->bmp); s->bmp = NULL; s->px = NULL; }
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { s->w = s->h = 0; return false; }
    s->bmp = bmp; s->px = (DWORD *)bits; s->w = w; s->h = h;
    s->old = (HBITMAP)SelectObject(s->dc, bmp);
    gdip_up();
    if (g_gdip && GdipCreateFromHDC(s->dc, &s->gfx) == 0) {
        GdipSetSmoothingMode(s->gfx, 4);      /* antialias */
        GdipSetPixelOffsetMode(s->gfx, 4);    /* half */
    } else {
        s->gfx = NULL;
    }
    return true;
}

void surf_free(Surface *s)
{
    if (s->gfx) { GdipDeleteGraphics(s->gfx); s->gfx = NULL; }
    if (s->bmp) { SelectObject(s->dc, s->old); DeleteObject(s->bmp); s->bmp = NULL; s->px = NULL; }
    if (s->dc) { DeleteDC(s->dc); s->dc = NULL; }
    s->w = s->h = 0;
}

/* ================================================================== hit regions */

Hot *hot_add(int x0, int y0, int x1, int y1, int id, int arg, bool enabled, const WCHAR *tip)
{
    Hot *h;
    if (!g_hl || g_hl->n >= HOT_MAX) return NULL;
    h = &g_hl->items[g_hl->n++];
    SetRect(&h->r, x0, y0, x1, y1);
    h->id = id; h->arg = arg; h->enabled = enabled;
    if (tip) lstrcpynW(h->tip, tip, (int)(sizeof(h->tip) / sizeof(h->tip[0])));
    else h->tip[0] = 0;
    return h;
}

const Hot *hot_find(const HotList *l, int x, int y)
{
    int i; POINT p; p.x = x; p.y = y;
    for (i = l->n - 1; i >= 0; i--) if (PtInRect(&l->items[i].r, p)) return &l->items[i];
    return NULL;
}

/* ================================================================== pixels */

DWORD mix(DWORD d, DWORD c, int a)
{
    DWORD rb, gg;
    if (a <= 0) return d & 0xFFFFFF;
    if (a >= 256) return c & 0xFFFFFF;
    rb = (((d & 0xFF00FF) * (DWORD)(256 - a) + (c & 0xFF00FF) * (DWORD)a) >> 8) & 0xFF00FF;
    gg = (((d & 0x00FF00) * (DWORD)(256 - a) + (c & 0x00FF00) * (DWORD)a) >> 8) & 0x00FF00;
    return rb | gg;
}

static bool clip(int *x0, int *y0, int *x1, int *y1)
{
    if (!g_tgt || !g_tgt->px) return false;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > g_tgt->w) *x1 = g_tgt->w;
    if (*y1 > g_tgt->h) *y1 = g_tgt->h;
    if (*x0 >= *x1 || *y0 >= *y1) return false;
    GdiFlush();
    return true;
}

void px_fill(int x0, int y0, int x1, int y1, DWORD c)
{
    int x, y;
    if (!clip(&x0, &y0, &x1, &y1)) return;
    c &= 0xFFFFFF;
    for (y = y0; y < y1; y++) { DWORD *p = g_tgt->px + (size_t)y * g_tgt->w; for (x = x0; x < x1; x++) p[x] = c; }
}

void px_blend(int x0, int y0, int x1, int y1, DWORD c, int a)
{
    int x, y;
    if (a <= 0 || !clip(&x0, &y0, &x1, &y1)) return;
    for (y = y0; y < y1; y++) { DWORD *p = g_tgt->px + (size_t)y * g_tgt->w; for (x = x0; x < x1; x++) p[x] = mix(p[x], c, a); }
}

void px_frame(int x0, int y0, int x1, int y1, int t, DWORD c)
{
    px_fill(x0, y0, x1, y0 + t, c); px_fill(x0, y1 - t, x1, y1, c);
    px_fill(x0, y0 + t, x0 + t, y1 - t, c); px_fill(x1 - t, y0 + t, x1, y1 - t, c);
}

void px_vgrad(int x0, int y0, int x1, int y1, DWORD top, DWORD bot)
{
    int y, h = y1 - y0;
    for (y = y0; y < y1; y++) px_fill(x0, y, x1, y + 1, mix(top, bot, h > 1 ? (y - y0) * 256 / (h - 1) : 0));
}

void px_vgrad_a(int x0, int y0, int x1, int y1, DWORD c, int a_top, int a_bot)
{
    int y, h = y1 - y0;
    for (y = y0; y < y1; y++) px_blend(x0, y, x1, y + 1, c, a_top + (h > 1 ? (a_bot - a_top) * (y - y0) / (h - 1) : 0));
}

void px_box(int x0, int y0, int x1, int y1, DWORD fill, DWORD border)
{
    DWORD *p; int w;
    px_fill(x0, y0, x1, y1, fill);
    px_frame(x0, y0, x1, y1, 1, border);
    /* soften the four corner pixels toward the fill */
    if (!g_tgt || !g_tgt->px) return;
    w = g_tgt->w;
    GdiFlush();
#define SOFT(x, y) do { if ((x) >= 0 && (y) >= 0 && (x) < w && (y) < g_tgt->h) { p = g_tgt->px + (size_t)(y) * w + (x); *p = mix(*p, fill, 150); } } while (0)
    SOFT(x0, y0); SOFT(x1 - 1, y0); SOFT(x0, y1 - 1); SOFT(x1 - 1, y1 - 1);
#undef SOFT
}

void px_copy(const DWORD *src, int sw, int sh, int dx, int dy)
{
    int y, x0 = dx, y0 = dy, x1 = dx + sw, y1 = dy + sh;
    if (!src || !clip(&x0, &y0, &x1, &y1)) return;
    for (y = y0; y < y1; y++)
        memcpy(g_tgt->px + (size_t)y * g_tgt->w + x0, src + (size_t)(y - dy) * sw + (x0 - dx), sizeof(DWORD) * (size_t)(x1 - x0));
}

void px_copy_dim(const DWORD *src, int sw, int sh, int dx, int dy, int keep)
{
    int x, y, x0 = dx, y0 = dy, x1 = dx + sw, y1 = dy + sh;
    if (!src || !clip(&x0, &y0, &x1, &y1)) return;
    for (y = y0; y < y1; y++) {
        DWORD *d = g_tgt->px + (size_t)y * g_tgt->w; const DWORD *s = src + (size_t)(y - dy) * sw - dx;
        for (x = x0; x < x1; x++) d[x] = mix(C_BG, s[x], keep);
    }
}

/* ================================================================== fonts */

static int g_ascent[F_COUNT];

static HFONT mkfont(const WCHAR *face, int px, int weight, BOOL italic)
{
    return CreateFontW(-dp(px), 0, 0, 0, weight, (DWORD)italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void fonts_free(void)
{
    int i;
    for (i = 0; i < F_COUNT; i++) if (g.font[i]) { DeleteObject(g.font[i]); g.font[i] = NULL; }
}

void fonts_make(void)
{
    HDC dc = CreateCompatibleDC(NULL); int i;
    fonts_free();
    g.font[F_BODY]    = mkfont(L"Segoe UI", 14, FW_NORMAL, FALSE);
    g.font[F_BODY_B]  = mkfont(L"Segoe UI Semibold", 14, FW_SEMIBOLD, FALSE);
    g.font[F_SMALL]   = mkfont(L"Segoe UI", 13, FW_NORMAL, FALSE);
    g.font[F_SMALL_B] = mkfont(L"Segoe UI Semibold", 13, FW_SEMIBOLD, FALSE);
    g.font[F_SMALL_I] = mkfont(L"Segoe UI", 13, FW_NORMAL, TRUE);
    g.font[F_TINY]    = mkfont(L"Segoe UI", 11, FW_NORMAL, FALSE);
    g.font[F_TINY_B]  = mkfont(L"Segoe UI Semibold", 11, FW_SEMIBOLD, FALSE);
    g.font[F_CAPS]    = mkfont(L"Segoe UI Semibold", 11, FW_SEMIBOLD, FALSE);
    g.font[F_CAPS_S]  = mkfont(L"Segoe UI Semibold", 10, FW_SEMIBOLD, FALSE);
    g.font[F_NAME]    = mkfont(L"Georgia", 38, FW_NORMAL, FALSE);
    g.font[F_DATE]    = mkfont(L"Georgia", 19, FW_NORMAL, FALSE);
    g.font[F_FLAVOR]  = mkfont(L"Georgia", 18, FW_NORMAL, TRUE);
    g.font[F_TEMP]    = mkfont(L"Segoe UI Light", 74, FW_LIGHT, FALSE);
    g.font[F_UNIT]    = mkfont(L"Segoe UI Light", 24, FW_LIGHT, FALSE);
    g.font[F_BTN]     = mkfont(L"Segoe UI Semibold", 13, FW_SEMIBOLD, FALSE);
    g.font[F_BTN_BIG] = mkfont(L"Segoe UI Semibold", 17, FW_SEMIBOLD, FALSE);
    g.font[F_H]       = mkfont(L"Segoe UI Semibold", 12, FW_SEMIBOLD, FALSE);
    g.font[F_MONTH]   = mkfont(L"Georgia", 22, FW_NORMAL, FALSE);
    g.font[F_MONO]    = mkfont(L"Consolas", 13, FW_NORMAL, FALSE);
    g.font[F_VALUE]   = mkfont(L"Segoe UI Semibold", 16, FW_SEMIBOLD, FALSE);
    g.font[F_NUM]     = mkfont(L"Segoe UI Semibold", 13, FW_SEMIBOLD, FALSE);
    g.font[F_TITLE]   = mkfont(L"Georgia", 17, FW_NORMAL, FALSE);
    g.font[F_FX]      = mkfont(L"Segoe UI", 15, FW_NORMAL, FALSE);
    g.font[F_FX_B]    = mkfont(L"Segoe UI Semibold", 15, FW_SEMIBOLD, FALSE);
    g.font[F_CLOCK]   = mkfont(L"Segoe UI Light", 30, FW_LIGHT, FALSE);
    for (i = 0; i < F_COUNT; i++) {
        TEXTMETRICW tm; HGDIOBJ o = SelectObject(dc, g.font[i]);
        GetTextMetricsW(dc, &tm);
        g.font_h[i] = tm.tmHeight + tm.tmExternalLeading;
        g_ascent[i] = tm.tmAscent;
        SelectObject(dc, o);
    }
    DeleteDC(dc);
}

HFONT font(int f) { return g.font[f]; }
int fh(int f) { return g.font_h[f]; }

/* measuring happens on whichever DC is current, or a private one */
static HDC mdc(void)
{
    static HDC dc;
    if (g_tgt && g_tgt->dc) return g_tgt->dc;
    if (!dc) dc = CreateCompatibleDC(NULL);
    return dc;
}

int tw(int f, const WCHAR *s, int n)
{
    SIZE sz; HDC dc = mdc();
    if (n < 0) n = lstrlenW(s);
    if (n <= 0) return 0;
    SelectObject(dc, g.font[f]);
    GetTextExtentPoint32W(dc, s, n, &sz);
    return sz.cx;
}

void tx(int f, int x, int y, const WCHAR *s, int n, DWORD c)
{
    if (!g_tgt) return;
    if (n < 0) n = lstrlenW(s);
    if (n <= 0) return;
    SelectObject(g_tgt->dc, g.font[f]);
    SetTextColor(g_tgt->dc, RGB_OF(c));
    TextOutW(g_tgt->dc, x, y, s, n);
}

void tx_sh(int f, int x, int y, const WCHAR *s, int n, DWORD c)
{
    int o = dp(1) > 0 ? dp(1) : 1;
    tx(f, x + o, y + o, s, n, C_SHADOW);
    tx(f, x, y + o, s, n, C_SHADOW);
    tx(f, x, y, s, n, c);
}

void tx_right(int f, int xr, int y, const WCHAR *s, DWORD c) { tx(f, xr - tw(f, s, -1), y, s, -1, c); }
void tx_center(int f, int cx, int y, const WCHAR *s, DWORD c) { tx(f, cx - tw(f, s, -1) / 2, y, s, -1, c); }

int tx_ellipsis(int f, int x, int y, int w, const WCHAR *s, DWORD c)
{
    RECT r; int full;
    if (w <= 0 || !g_tgt) return 0;
    full = tw(f, s, -1);
    if (full <= w) { tx(f, x, y, s, -1, c); return full; }
    SetRect(&r, x, y, x + w, y + fh(f));
    SelectObject(g_tgt->dc, g.font[f]);
    SetTextColor(g_tgt->dc, RGB_OF(c));
    DrawTextW(g_tgt->dc, s, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_LEFT | DT_TOP);
    return w;
}

static void upper(const WCHAR *s, WCHAR *out, int cap)
{
    lstrcpynW(out, s, cap);
    CharUpperBuffW(out, (DWORD)lstrlenW(out));
}

int tw_caps(int f, const WCHAR *s)
{
    WCHAR b[160]; int w; HDC dc = mdc();
    upper(s, b, 160);
    SetTextCharacterExtra(dc, dp(1));
    w = tw(f, b, -1);
    SetTextCharacterExtra(dc, 0);
    return w;
}

void tx_caps(int f, int x, int y, const WCHAR *s, DWORD c)
{
    WCHAR b[160];
    if (!g_tgt) return;
    upper(s, b, 160);
    SetTextCharacterExtra(g_tgt->dc, dp(1));
    tx(f, x, y, b, -1, c);
    SetTextCharacterExtra(g_tgt->dc, 0);
}

/* ================================================================== UTF-8 <-> UTF-16 */

const WCHAR *wz(const char *s)
{
    static WCHAR *ring[12]; static int cap[12]; static int k;
    int n, i = k++ % 12;
    if (!s) s = "";
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    if (cap[i] < n) {
        WCHAR *p = (WCHAR *)realloc(ring[i], sizeof(WCHAR) * (size_t)(n + 64));
        if (!p) return L"";
        ring[i] = p; cap[i] = n + 64;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, ring[i], cap[i])) ring[i][0] = 0;
    return ring[i];
}

WCHAR *wz_dup(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    WCHAR *w = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n)) w[0] = 0;
    return w;
}

char *u8_dup(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)(n > 0 ? n : 1));
    if (!s) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

/* ================================================================== rich text */

typedef struct Piece { const WCHAR *s; int n; int w; BYTE span; BYTE kind; } Piece;   /* kind 0 word 1 space 2 newline */
static Piece *g_pc; static int g_pc_cap;

static bool pc_push(int *n, Piece p)
{
    if (*n >= g_pc_cap) {
        int nc = g_pc_cap ? g_pc_cap * 2 : 512;
        Piece *q = (Piece *)realloc(g_pc, sizeof(Piece) * (size_t)nc);
        if (!q) return false;
        g_pc = q; g_pc_cap = nc;
    }
    g_pc[(*n)++] = p;
    return true;
}

static int box_pad(void) { return dp(4); }

static void draw_piece(const Span *sp, const Piece *p, int x, int y_line, int base)
{
    const Span *s = &sp[p->span];
    int y = y_line + base - g_ascent[s->font];
    if (s->flags & SP_BOX) {
        int bh = fh(s->font) + dp(2), by = y - dp(1);
        px_box(x, by, x + p->w, by + bh, mix(C_PANEL, s->color, 30), mix(C_PANEL, s->color, 120));
        tx(s->font, x + box_pad(), y, p->s, p->n, s->color);
    } else if (s->flags & SP_SHADOW) {
        tx_sh(s->font, x, y, p->s, p->n, s->color);
    } else {
        tx(s->font, x, y, p->s, p->n, s->color);
    }
}

int rich(const Span *sp, int nsp, int x, int y, int w, int lh, bool draw)
{
    int np = 0, i, base = 0, maxh = 0, lines = 0, line_w = 0, cx, pend_sp = 0, start;
    bool any = false;
    HDC dc = mdc();
    for (i = 0; i < nsp; i++) {
        const WCHAR *s = sp[i].s; int n = sp[i].n < 0 ? lstrlenW(s) : sp[i].n, k = 0;
        if (g_ascent[sp[i].font] > base) base = g_ascent[sp[i].font];
        if (fh(sp[i].font) > maxh) maxh = fh(sp[i].font);
        SelectObject(dc, g.font[sp[i].font]);
        if (sp[i].flags & SP_BOX) {          /* atomic */
            Piece p; SIZE sz;
            GetTextExtentPoint32W(dc, s, n, &sz);
            p.s = s; p.n = n; p.w = sz.cx + 2 * box_pad(); p.span = (BYTE)i; p.kind = 0;
            if (!pc_push(&np, p)) return 0;
            continue;
        }
        while (k < n) {
            Piece p; SIZE sz; int e = k;
            p.span = (BYTE)i; p.s = s + k;
            if (s[k] == L'\n') { p.n = 1; p.w = 0; p.kind = 2; e = k + 1; }
            else if (s[k] == L' ' || s[k] == L'\t') {
                while (e < n && (s[e] == L' ' || s[e] == L'\t')) e++;
                GetTextExtentPoint32W(dc, L" ", 1, &sz);
                p.n = e - k; p.w = sz.cx; p.kind = 1;
            } else {
                while (e < n && s[e] != L' ' && s[e] != L'\t' && s[e] != L'\n') e++;
                if (s[e - 1] == L'\r' && e - 1 > k) e--;   /* CRLF */
                p.n = e - k; p.kind = 0;
                GetTextExtentPoint32W(dc, p.s, p.n, &sz); p.w = sz.cx;
                if (e < n && s[e] == L'\r') e++;
            }
            if (!pc_push(&np, p)) return 0;
            k = e;
        }
    }
    if (lh <= 0) lh = maxh;
    base += (lh - maxh) / 2;
    cx = 0;
    i = 0;
    while (i < np) {
        Piece *p = &g_pc[i];
        int uw = 0, j;
        if (p->kind == 2) { lines++; line_w = 0; pend_sp = 0; any = false; i++; continue; }
        if (p->kind == 1) { if (line_w > 0) pend_sp = p->w; i++; continue; }
        /* a unit = consecutive word pieces */
        start = i;
        for (j = i; j < np && g_pc[j].kind == 0; j++) uw += g_pc[j].w;
        if (line_w > 0 && line_w + pend_sp + uw > w) { lines++; line_w = 0; pend_sp = 0; }
        if (line_w > 0) line_w += pend_sp;
        pend_sp = 0;
        for (j = start; j < np && g_pc[j].kind == 0; j++) {
            Piece *q = &g_pc[j];
            if (line_w > 0 && line_w + q->w > w && q->w > w) { lines++; line_w = 0; }
            if (q->w > w && line_w == 0) {
                /* an over-long word: break it by characters */
                const WCHAR *s = q->s; int left = q->n;
                const Span *ss = &sp[q->span];
                SelectObject(dc, g.font[ss->font]);
                while (left > 0) {
                    int fit = 0; SIZE sz;
                    GetTextExtentExPointW(dc, s, left, w, &fit, NULL, &sz);
                    if (fit <= 0) fit = 1;
                    if (draw) { Piece t = *q; t.s = s; t.n = fit; draw_piece(sp, &t, x, y + lines * lh, base); }
                    s += fit; left -= fit;
                    if (left > 0) lines++;
                    else { GetTextExtentPoint32W(dc, q->s + q->n - fit, fit, &sz); line_w = sz.cx; }
                }
                any = true;
                continue;
            }
            if (draw) draw_piece(sp, q, x + line_w, y + lines * lh, base);
            line_w += q->w;
            any = true;
        }
        i = j;
    }
    (void)cx;
    if (any || lines) lines++;
    return lines * lh;
}

int wrap(int f, int x, int y, int w, const WCHAR *s, DWORD c, bool draw)
{
    Span sp; sp.s = s; sp.n = -1; sp.font = (BYTE)f; sp.flags = 0; sp.color = c;
    return rich(&sp, 1, x, y, w, fh(f), draw);
}

/* ================================================================== antialiased shapes */

static bool aa_ok(void) { return g_tgt && g_tgt->gfx && g_brush && g_pen && g_path; }
void aa_flush(void) { if (g_tgt && g_tgt->gfx) GdipFlush(g_tgt->gfx, 1); }

void aa_circle(double cx, double cy, double r, DWORD fill, int alpha)
{
    if (!aa_ok()) return;
    GdiFlush();
    GdipSetSolidFillColor(g_brush, argb(fill, alpha));
    GdipFillEllipse(g_tgt->gfx, g_brush, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r));
}

void aa_ring(double cx, double cy, double r, DWORD c, int alpha, double width)
{
    if (!aa_ok()) return;
    GdiFlush();
    GdipSetPenColor(g_pen, argb(c, alpha));
    GdipSetPenWidth(g_pen, (float)width);
    GdipDrawEllipse(g_tgt->gfx, g_pen, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r));
}

void aa_line(double x0, double y0, double x1, double y1, DWORD c, int alpha, double width)
{
    if (!aa_ok()) return;
    GdiFlush();
    GdipSetPenColor(g_pen, argb(c, alpha));
    GdipSetPenWidth(g_pen, (float)width);
    GdipDrawLine(g_tgt->gfx, g_pen, (float)x0, (float)y0, (float)x1, (float)y1);
}

void aa_poly(const double *xy, int n, DWORD c, int alpha)
{
    GpPointF pts[64]; int i;
    if (!aa_ok() || n < 3 || n > 64) return;
    GdiFlush();
    for (i = 0; i < n; i++) { pts[i].X = (float)xy[2 * i]; pts[i].Y = (float)xy[2 * i + 1]; }
    GdipResetPath(g_path);
    GdipAddPathPolygon(g_path, pts, n);
    GdipSetSolidFillColor(g_brush, argb(c, alpha));
    GdipFillPath(g_tgt->gfx, g_brush, g_path);
}

/* A six-armed frost mark (new moon / sacrifice to Auril): three thin crossing strokes. */
void aa_flake(double cx, double cy, double r, DWORD c, int alpha)
{
    int k;
    for (k = 0; k < 3; k++) {
        double a = IWD_PI / 2 + k * IWD_PI / 3, dx = cos(a) * r, dy = sin(a) * r;
        aa_line(cx - dx, cy - dy, cx + dx, cy + dy, c, alpha, r > 5 ? 1.3 : 1.0);
    }
}

/* SPEC 5: dark disc, lit limb bounded by the outer half circle and a terminator
 * half-ellipse of half-width r*|1 - 2 illum|; waxing lit on the right. */
void aa_moon(double cx, double cy, double r, const IwdMoon *m)
{
    double il = m->illum, tw_ = r * fabs(1 - 2 * il);
    float x = (float)(cx - r), y = (float)(cy - r), d = (float)(2 * r);
    if (!aa_ok()) return;
    GdiFlush();
    if (m->is_new) aa_circle(cx, cy, r + dp(2), 0x9FD8FF, 40);
    aa_circle(cx, cy, r, m->is_new ? 0x1E3250 : 0x081426, 235);
    if (il > 0.985) {
        aa_circle(cx, cy, r, 0xEEF7FF, 255);
    } else if (il > 0.015) {
        float start = m->waxing ? -90.0f : 90.0f;        /* lit half circle: right when waxing */
        GdipResetPath(g_path);
        GdipAddPathArc(g_path, x, y, d, d, start, 180.0f);
        /* terminator from the bottom (waxing) back to the top */
        if (tw_ > 0.05) {
            float ex = (float)(cx - tw_), ew = (float)(2 * tw_);
            bool gibbous = il > 0.5;
            /* waxing: path is at the bottom now (90 deg). gibbous bulges left (into the dark half):
             * sweep +180 from 90 through 180; crescent bulges right: sweep -180 from 90 through 0. */
            if (m->waxing) GdipAddPathArc(g_path, ex, y, ew, d, 90.0f, gibbous ? 180.0f : -180.0f);
            else           GdipAddPathArc(g_path, ex, y, ew, d, 270.0f, gibbous ? 180.0f : -180.0f);
        }
        GdipClosePathFigure(g_path);
        GdipSetSolidFillColor(g_brush, argb(0xEEF7FF, 255));
        GdipFillPath(g_tgt->gfx, g_brush, g_path);
    }
    if (m->is_new) aa_ring(cx, cy, r, 0xD9F0FF, 255, 1.3);
    else           aa_ring(cx, cy, r, 0xD2EBFF, 107, 1.0);
}

/* ================================================================== JPEG decoding */

static Image g_full[13], g_med[13];
static unsigned g_full_use[13], g_use_clock;

static bool decode_res(int resid, Image *out)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(resid), (LPCWSTR)RT_RCDATA);
    HGLOBAL hg = res ? LoadResource(NULL, res) : NULL;
    const BYTE *data = hg ? (const BYTE *)LockResource(hg) : NULL;
    DWORD size = res ? SizeofResource(NULL, res) : 0;
    IStream *stm; void *bmp = NULL; bool ok = false;
    if (!data || !size) return false;
    gdip_up();
    if (!g_gdip) return false;
    stm = SHCreateMemStream(data, size);
    if (!stm) return false;
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        UINT w = 0, h = 0; GpBitmapData bd; GpRectI rc;
        GdipGetImageWidth(bmp, &w); GdipGetImageHeight(bmp, &h);
        rc.X = 0; rc.Y = 0; rc.Width = (INT)w; rc.Height = (INT)h;
        if (w && h && w <= 8192 && h <= 8192 && GdipBitmapLockBits(bmp, &rc, 1, GP_PIXFMT_32RGB, &bd) == 0) {
            DWORD *px = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
            if (px) {
                UINT xx, yy;
                for (yy = 0; yy < h; yy++) {
                    const DWORD *s = (const DWORD *)((const BYTE *)bd.Scan0 + (INT_PTR)yy * bd.Stride);
                    DWORD *d = px + (size_t)yy * w;
                    for (xx = 0; xx < w; xx++) d[xx] = s[xx] & 0xFFFFFF;
                }
                out->px = px; out->w = (int)w; out->h = (int)h; ok = true;
            }
            GdipBitmapUnlockBits(bmp, &bd);
        }
        GdipDisposeImage(bmp);
    }
    stm->lpVtbl->Release(stm);
    return ok;
}

static void fallback_image(Image *im)
{
    int x, y;
    im->w = 96; im->h = 32;
    im->px = (DWORD *)malloc(sizeof(DWORD) * 96 * 32);
    if (!im->px) { im->w = im->h = 0; return; }
    for (y = 0; y < 32; y++) for (x = 0; x < 96; x++) im->px[y * 96 + x] = mix(0x0E1830, 0x3B5374, y * 8);
}

const Image *img_full(int id)
{
    int i, live = 0, lru = -1;
    if (id < 1 || id > 12) id = 1;
    g_full_use[id] = ++g_use_clock;
    if (g_full[id].px) return &g_full[id];
    for (i = 1; i <= 12; i++) if (g_full[i].px) {
        live++;
        if (lru < 0 || g_full_use[i] < g_full_use[lru]) lru = i;
    }
    if (live >= 3 && lru > 0) { free(g_full[lru].px); g_full[lru].px = NULL; }
    if (!decode_res(100 + id, &g_full[id])) fallback_image(&g_full[id]);
    return g_full[id].px ? &g_full[id] : NULL;
}

const Image *img_med(int id)
{
    const Image *f;
    if (id < 1 || id > 12) id = 1;
    if (g_med[id].px) return &g_med[id];
    /* pre-scaled copy embedded in the exe: no full decode for strips and thumbnails */
    if (decode_res(200 + id, &g_med[id])) {
        if (g_med[id].w == 360 && g_med[id].h == 120) return &g_med[id];
        free(g_med[id].px); g_med[id].px = NULL;
    }
    f = img_full(id);
    if (!f) return NULL;
    g_med[id].w = 360; g_med[id].h = 120;
    g_med[id].px = (DWORD *)malloc(sizeof(DWORD) * 360 * 120);
    if (!g_med[id].px) return NULL;
    resample_cover(f, g_med[id].px, 360, 120, 360);
    return &g_med[id];
}

void img_free_all(void)
{
    int i;
    for (i = 0; i < 13; i++) {
        free(g_full[i].px); g_full[i].px = NULL;
        free(g_med[i].px); g_med[i].px = NULL;
    }
}

/* ================================================================== bicubic resampler */
/* Separable Catmull-Rom whose support widens when shrinking (sharp enlargements,
 * alias-free reductions). Carried over from the first native build. */

typedef struct { int *bounds; int *k; int ksize; } Coeffs;

static double cubic(double x)
{
    if (x < 0) x = -x;
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

static bool coeffs_make(Coeffs *c, int in, double in0, double in1, int out)
{
    double scale = (in1 - in0) / out, fs = scale < 1.0 ? 1.0 : scale, support = 2.0 * fs;
    int ksize = (int)support * 2 + 3, xx, i;
    double *w = (double *)malloc(sizeof(double) * (size_t)ksize);
    c->ksize  = ksize;
    c->bounds = (int *)malloc(sizeof(int) * 2 * (size_t)out);
    c->k      = (int *)malloc(sizeof(int) * (size_t)ksize * (size_t)out);
    if (!w || !c->bounds || !c->k) { free(w); free(c->bounds); free(c->k); return false; }
    for (xx = 0; xx < out; xx++) {
        double center = in0 + (xx + 0.5) * scale, sum = 0;
        int xmin = (int)floor(center - support + 0.5), xmax = (int)floor(center + support + 0.5), n;
        if (xmin < 0) xmin = 0;
        if (xmax > in) xmax = in;
        if (xmin > in - 1) xmin = in - 1;
        n = xmax - xmin;
        if (n < 1) n = 1;
        if (n > ksize) n = ksize;
        for (i = 0; i < n; i++) { w[i] = cubic((i + xmin - center + 0.5) / fs); sum += w[i]; }
        if (sum == 0) { w[0] = 1; sum = 1; }
        for (i = 0; i < n; i++) {
            double v = w[i] / sum * 16384.0;
            c->k[xx * ksize + i] = (int)(v < 0 ? v - 0.5 : v + 0.5);
        }
        c->bounds[xx * 2] = xmin; c->bounds[xx * 2 + 1] = n;
    }
    free(w);
    return true;
}

static int clamp8(int v) { v = (v + 8192) >> 14; return v < 0 ? 0 : v > 255 ? 255 : v; }

void resample_box(const Image *im, double sx0, double sy0, double sx1, double sy1,
                  DWORD *dst, int dw, int dh, int dpitch)
{
    Coeffs cx, cy; DWORD *tmp; int ymin, ymax, x, y, i, rows;
    const DWORD *src = im->px; int sw = im->w, sh = im->h;
    if (dw <= 0 || dh <= 0 || !src) return;
    if (!coeffs_make(&cx, sw, sx0, sx1, dw)) return;
    if (!coeffs_make(&cy, sh, sy0, sy1, dh)) { free(cx.bounds); free(cx.k); return; }
    ymin = cy.bounds[0]; ymax = cy.bounds[(dh - 1) * 2] + cy.bounds[(dh - 1) * 2 + 1];
    rows = ymax - ymin;
    tmp = (DWORD *)malloc(sizeof(DWORD) * (size_t)dw * (size_t)rows);
    if (tmp) {
        for (y = 0; y < rows; y++) {
            const DWORD *s = src + (size_t)(y + ymin) * sw; DWORD *t = tmp + (size_t)y * dw;
            for (x = 0; x < dw; x++) {
                const int *k = cx.k + x * cx.ksize; const DWORD *p = s + cx.bounds[x * 2];
                int n = cx.bounds[x * 2 + 1], r = 0, gg = 0, b = 0;
                for (i = 0; i < n; i++) { DWORD v = p[i]; int kk = k[i];
                    r += (int)((v >> 16) & 255) * kk; gg += (int)((v >> 8) & 255) * kk; b += (int)(v & 255) * kk; }
                t[x] = (DWORD)(clamp8(r) << 16 | clamp8(gg) << 8 | clamp8(b));
            }
        }
        for (y = 0; y < dh; y++) {
            const int *k = cy.k + y * cy.ksize; int n = cy.bounds[y * 2 + 1];
            const DWORD *base = tmp + (size_t)(cy.bounds[y * 2] - ymin) * dw; DWORD *d = dst + (size_t)y * dpitch;
            for (x = 0; x < dw; x++) {
                int r = 0, gg = 0, b = 0;
                for (i = 0; i < n; i++) { DWORD v = base[(size_t)i * dw + x]; int kk = k[i];
                    r += (int)((v >> 16) & 255) * kk; gg += (int)((v >> 8) & 255) * kk; b += (int)(v & 255) * kk; }
                d[x] = (DWORD)(clamp8(r) << 16 | clamp8(gg) << 8 | clamp8(b));
            }
        }
        free(tmp);
    }
    free(cx.bounds); free(cx.k); free(cy.bounds); free(cy.k);
}

void resample_cover(const Image *im, DWORD *dst, int dw, int dh, int dpitch)
{
    double x0 = 0, y0 = 0, x1 = im->w, y1 = im->h;
    if ((double)dw * im->h > (double)dh * im->w) {
        double ch = (double)im->w * dh / dw; y0 = (im->h - ch) / 2; y1 = y0 + ch;
    } else {
        double cw = (double)im->h * dw / dh; x0 = (im->w - cw) / 2; x1 = x0 + cw;
    }
    resample_box(im, x0, y0, x1, y1, dst, dw, dh, dpitch);
}

/* ================================================================== hero scene */

static DWORD *s_base; static int s_bw, s_bh, s_bid = -1;
static DWORD *s_comp; static int s_cw, s_ch; static SceneLook s_key; static bool s_valid;

static int c8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5); }

static bool look_eq(const SceneLook *a, const SceneLook *b)
{
    return a->event_id == b->event_id && a->light_level == b->light_level &&
           fabs(a->twilight - b->twilight) < 0.02 && fabs(a->aurora - b->aurora) < 0.02 &&
           fabs(a->baked_aurora_subdue - b->baked_aurora_subdue) < 0.02 &&
           fabs(a->dark - b->dark) < 0.02 && fabs(a->cold - b->cold) < 0.02 && a->glare == b->glare &&
           a->light_ground == b->light_ground;
}

const DWORD *scene_get(const SceneLook *lk, int w, int h)
{
    int x, y;
    double *colv, *rowv;
    if (w <= 0 || h <= 0) return NULL;
    if (s_comp && s_valid && s_cw == w && s_ch == h && look_eq(lk, &s_key)) return s_comp;
    if (!s_base || s_bw != w || s_bh != h || s_bid != lk->event_id) {
        const Image *im = img_full(lk->event_id);
        free(s_base);
        s_base = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
        s_bid = -1;
        if (!s_base || !im) return NULL;
        resample_cover(im, s_base, w, h, w);
        s_bw = w; s_bh = h; s_bid = lk->event_id;
    }
    if (!s_comp || s_cw != w || s_ch != h) {
        free(s_comp);
        s_comp = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
        if (!s_comp) return NULL;
        s_cw = w; s_ch = h;
    }
    colv = (double *)malloc(sizeof(double) * (size_t)w);
    rowv = (double *)malloc(sizeof(double) * (size_t)h);
    if (!colv || !rowv) { free(colv); free(rowv); memcpy(s_comp, s_base, sizeof(DWORD) * (size_t)w * h); return s_comp; }
    for (x = 0; x < w; x++) { double u = (x + 0.5) / w * 2 - 1; colv[x] = u * u; }
    for (y = 0; y < h; y++) { double v = (y + 0.5) / h * 2 - 1; rowv[y] = v * v; }
    {
        double twi = lk->twilight, aur = lk->aurora, sub = lk->baked_aurora_subdue;
        double dark = 1.0 - 0.35 * lk->dark, vig = 0.20 + 0.14 * lk->dark;
        double cold = lk->cold;
        double scrim_b = lk->glare ? 0.86 : 0.74, scrim_t = lk->glare ? 0.66 : 0.50;
        bool lg = lk->light_ground;
        if (lg) { scrim_b = scrim_t = 0; vig = 0.10; }
        int frost = (int)(0.06 * (w < h ? w : h)) + 1;
        for (y = 0; y < h; y++) {
            const DWORD *s = s_base + (size_t)y * w; DWORD *d = s_comp + (size_t)y * w;
            double fy = (double)y / h;
            /* sky band weights */
            double aur_y = fy < 0.36 ? pow(1 - fy / 0.36, 1.3) : 0;
            double glow = exp(-pow((fy - 0.58) / 0.12, 2));               /* twilight horizon glow */
            double lift = 1 + 0.14 * twi * (fy < 0.7 ? 1 - fy / 0.7 : 0);
            double sb = fy > 0.42 ? (fy - 0.42) / 0.58 : 0;
            /* top scrim behind the date/meta lines; on a bright sky (white out, fog,
               blizzard) it reaches further down and falls off linearly so the meta line
               (~20% down) stays legible */
            double st = lk->glare ? (fy < 0.38 ? 1 - fy / 0.38 : 0) : (fy < 0.30 ? 1 - fy / 0.30 : 0);
            double scrim = scrim_b * sb * sb * (3 - 2 * sb) + scrim_t * (lk->glare ? st : st * st);
            for (x = 0; x < w; x++) {
                DWORD p = s[x];
                double r = (p >> 16) & 255, gg = (p >> 8) & 255, b = p & 255, fx = (double)x / w, a, v;
                if (sub > 0 && fy < 0.62) {                             /* hide w01's baked aurora by day */
                    /* hue-targeted: only the green excess goes, stars and ridgelines stay */
                    double m = r > b ? r : b, gn = (gg - m) / 14.0;
                    if (gn > 0) {
                        if (gn > 1) gn = 1;
                        a = sub * gn;
                        gg = m + (1 - a) * (gg - m);
                        r *= 1 - 0.35 * a; gg *= 1 - 0.35 * a; b *= 1 - 0.35 * a;
                    }
                }
                if (aur > 0 && aur_y > 0) {                             /* curtains: green -> cyan -> violet */
                    double wave = 0.55 + 0.45 * sin(fx * 9.0 + sin(fx * 3.1) * 1.7);
                    double ar = 0x7D + (0xB4 - 0x7D) * fx, ag = 0xFF + (0x8F - 0xFF) * fx, ab = 0xCF + (0xFF - 0xCF) * fx;
                    a = 0.22 * aur * aur_y * wave;
                    r += ar * a; gg += ag * a; b += ab * a;
                }
                if (twi > 0) {
                    r *= lift; gg *= lift; b *= lift;
                    a = 0.24 * twi * glow;
                    r += (255 - r) * a * 1.0; gg += (190 - gg) * a * 0.8; b += (200 - b) * a * 0.8;
                }
                r *= dark; gg *= dark; b *= dark;
                v = (colv[x] * 0.7 + rowv[y] * 0.5) * vig;              /* vignette */
                if (v > 0) { r *= 1 - v; gg *= 1 - v; b *= 1 - v; }
                if (cold > 0) {                                          /* frost creeping in from the edges */
                    int e = x < y ? x : y, e2 = (w - 1 - x) < (h - 1 - y) ? (w - 1 - x) : (h - 1 - y);
                    if (e2 < e) e = e2;
                    if (e < frost) {
                        a = 0.16 * cold * (1 - (double)e / frost);
                        r += (215 - r) * a; gg += (236 - gg) * a; b += (250 - b) * a;
                    }
                }
                if (scrim > 0) { r += (4 - r) * scrim; gg += (8 - gg) * scrim; b += (15 - b) * scrim; }
                if (lg) { r += (236 - r) * 0.25; gg += (240 - gg) * 0.25; b += (244 - b) * 0.25; }
                d[x] = (DWORD)(c8(r) << 16 | c8(gg) << 8 | c8(b));
            }
        }
    }
    free(colv); free(rowv);
    s_key = *lk; s_valid = true;
    return s_comp;
}

void scene_free(void)
{
    free(s_base); s_base = NULL; s_bid = -1;
    free(s_comp); s_comp = NULL; s_valid = false;
}

/* ================================================================== thumbnails */

static DWORD *t_px[13]; static int t_w, t_h;

const DWORD *thumb_get(int id, int w, int h)
{
    const Image *m;
    if (id < 1 || id > 12 || w <= 0 || h <= 0) return NULL;
    if (w != t_w || h != t_h) { thumb_free(); t_w = w; t_h = h; }
    if (t_px[id]) return t_px[id];
    m = img_med(id);
    if (!m) return NULL;
    t_px[id] = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
    if (!t_px[id]) return NULL;
    /* the horizon band of the scene: a centred crop at the image's own scale */
    {
        double cw = m->w, ch = cw * h / w, y0 = m->h * 0.40 - ch / 2;
        if (ch > m->h) { ch = m->h; cw = ch * w / h; }
        if (y0 < 0) y0 = 0;
        if (y0 + ch > m->h) y0 = m->h - ch;
        resample_box(m, (m->w - cw) / 2, y0, (m->w + cw) / 2, y0 + ch, t_px[id], w, h, w);
    }
    return t_px[id];
}

void thumb_free(void)
{
    int i;
    for (i = 0; i < 13; i++) { free(t_px[i]); t_px[i] = NULL; }
}
