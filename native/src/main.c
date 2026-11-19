/*
 * Icewind Dale Weather Helper - native Win32 build.
 *
 * One window, no child controls, no timers, no animation: everything is painted
 * into a single back-buffer DIB on WM_PAINT and the process sleeps in
 * GetMessage() the rest of the time.
 *
 *   - Scene JPEGs live in the exe as RCDATA 101..112 and are decoded lazily
 *     (GDI+ flat API) into plain 32-bit pixel buffers.
 *   - Scaling is done by a small separable bicubic resampler in this file; the
 *     scaled scene and the composed 24-hour bar are cached, so stepping through
 *     hours or hovering only copies rows and draws a few strings.
 *   - Weather data, progression table and the day generator mirror main.py.
 *
 * Test hooks: --seed <u32> --newday --hour <0-23> --dump <path> --size WxH
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>
#include <shlwapi.h>
#include <string.h>

/* ------------------------------------------------------------------ data */

enum { S_N, S_B, S_I, S_U, S_TITLE, S_COUNT };

typedef struct { const WCHAR *text; BYTE style; } Run;
typedef struct { const WCHAR *name; BYTE dice, sides; const Run *runs; int nruns; } Event;

#include "weather_data.h"

/* --------------------------------------------------------------- palette */
/* Colours are 0xRRGGBB, the same layout as a 32-bit DIB pixel. */
#define C_BG       0x0B111B
#define C_BG_SOFT  0x121B29
#define C_LINE     0x1A2535
#define C_FRAME    0x2B3B53
#define C_TEXT     0xD3DCE8
#define C_BRIGHT   0xFFFFFF
#define C_ITALIC   0xC6D9EE
#define C_TITLE    0x86C5F2
#define C_DIM      0x6C7C93
#define C_ICE      0xEAF6FF
#define C_SHADOW   0x04070C
#define RGB_OF(c)  RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

/* ---------------------------------------------------------------- state */

enum { F_BODY, F_BOLD, F_ITALIC, F_UNDER, F_TITLE, F_NAME, F_LABEL, F_BTN, F_NUM, F_NUMB, F_SMALL, F_COUNT };
enum { D_SCENE = 1, D_BAR = 2, D_TEXT = 4, D_BTN = 8, D_ALL = 15 };
enum { BTN_NEW, BTN_PREV, BTN_NEXT, BTN_COUNT };

typedef struct { DWORD *px; int w, h; } Image;
typedef struct { int x, y; unsigned short off, len; BYTE style; } Frag;

static HINSTANCE g_inst;
static HWND   g_hwnd;
static HANDLE g_heap;
static int    g_dpi = 96;
static HFONT  g_font[F_COUNT];

/* back buffer */
static HDC     g_dc;
static HBITMAP g_bmp, g_bmpOld;
static DWORD  *g_px;
static int     g_w, g_h;
static unsigned g_dirty = D_ALL;

/* schedule */
static BOOL g_has;
static BYTE g_sched[24];
static BYTE g_segEvent[24], g_segRoll[24];
static int  g_nseg;
static int  g_cur;

/* images and caches */
static Image     g_img[13];
static ULONG_PTR g_gdipToken;
static DWORD *g_scenePx; static int g_sceneW, g_sceneH, g_sceneKey = -1;
static DWORD *g_barPx;   static int g_barW, g_barH; static BOOL g_barValid;

/* layout */
static RECT g_rcScene, g_rcBarZone, g_rcBar, g_rcTextZone, g_rcText, g_rcBtnZone, g_rcBtn[BTN_COUNT];
static int  g_numsTop;

/* text */
#define TXT_MAX 4096
#define FRAG_MAX 640
static WCHAR g_txt[TXT_MAX];
static BYTE  g_sty[TXT_MAX];
static int   g_txtLen;
static Frag  g_frag[FRAG_MAX];
static int   g_nfrag, g_textH, g_scroll, g_lineH, g_titleH;

/* interaction */
static int  g_hoverCell = -1, g_hoverBtn = -1, g_pressBtn = -1, g_focusBtn = -1;
static BOOL g_scrub, g_tracking;

/* ------------------------------------------------------------- utilities */

static void *xalloc(SIZE_T n) { return HeapAlloc(g_heap, 0, n ? n : 1); }
static void  xfree(void *p)   { if (p) HeapFree(g_heap, 0, p); }
static int   dp(int v)        { return MulDiv(v, g_dpi, 96); }
static int   clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* xorshift64* seeded through splitmix64; dice use rejection sampling (no modulo bias). */
static unsigned long long g_rng;

static unsigned long long splitmix(unsigned long long *s)
{
    unsigned long long z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed(unsigned long long s)
{
    g_rng = splitmix(&s);
    if (!g_rng) g_rng = 0x2545F4914F6CDD1DULL;
}

static void rng_seed_os(void)
{
    LARGE_INTEGER qpc; FILETIME ft; POINT pt = {0, 0};
    unsigned long long s;
    QueryPerformanceCounter(&qpc);
    GetSystemTimeAsFileTime(&ft);
    GetCursorPos(&pt);
    s = (unsigned long long)qpc.QuadPart;
    s ^= ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) * 0x9E3779B97F4A7C15ULL;
    s ^= (unsigned long long)GetCurrentProcessId() << 40;
    s ^= (unsigned long long)GetCurrentThreadId() << 20;
    s ^= (unsigned long long)GetTickCount() << 7;
    s ^= (unsigned long long)(ULONG_PTR)&qpc << 13;          /* ASLR stack address */
    s ^= ((unsigned long long)(unsigned)pt.x << 48) ^ ((unsigned long long)(unsigned)pt.y << 3);
    rng_seed(s);
}

static unsigned rng_u32(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (unsigned)((g_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static int roll_die(int sides)
{
    unsigned n = (unsigned)sides, limit = 0xFFFFFFFFu - (0xFFFFFFFFu % n + 1u) % n, r;
    do r = rng_u32(); while (r > limit);
    return (int)(r % n) + 1;
}

/* ------------------------------------------------------------ new_day() */
/* Same algorithm as main.py: d12 picks the first event, each event rolls its
   duration dice, the duration is clamped to the hours left in the day and the
   (unclamped) roll picks the next event; no match falls back to Clear Skies. */
static void new_day(void)
{
    int ev = roll_die(12), total = 0;
    g_nseg = 0;
    while (total < 24) {
        const Event *e = &EVENTS[ev];
        int roll = 0, dur, i, next = 0;
        for (i = 0; i < e->dice; i++) roll += roll_die(e->sides);
        dur = roll < 24 - total ? roll : 24 - total;
        for (i = 0; i < dur; i++) g_sched[total + i] = (BYTE)ev;
        g_segEvent[g_nseg] = (BYTE)ev; g_segRoll[g_nseg] = (BYTE)roll; g_nseg++;
        total += dur;
        if (total >= 24) break;
        for (i = 0; i < 3; i++) {
            const BYTE *p = PROGRESSION[ev][i];
            if (p[0] && p[0] <= roll && roll <= p[1]) { next = p[2]; break; }
        }
        ev = next ? next : 1;
    }
    g_cur = 0;
    g_has = TRUE;
}

static BOOL dump_schedule(const WCHAR *path)
{
    char buf[2048]; int n = 0, i; DWORD wr; HANDLE f;
    for (i = 0; i < g_nseg; i++)
        n += wsprintfA(buf + n, i ? " %d:%d" : "%d:%d", g_segEvent[i], g_segRoll[i]);
    buf[n++] = '\n';
    for (i = 0; i < 24; i++) {
        n += wsprintfA(buf + n, "%d\t%d\t", i, g_sched[i]);
        n += WideCharToMultiByte(CP_UTF8, 0, EVENTS[g_sched[i]].name, -1, buf + n, 64, NULL, NULL) - 1;
        buf[n++] = '\n';
    }
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    WriteFile(f, buf, (DWORD)n, &wr, NULL);
    CloseHandle(f);
    return wr == (DWORD)n;
}

/* ------------------------------------------------------- pixel helpers */

static DWORD mix(DWORD d, DWORD c, int a)   /* a = 0..256 weight of c */
{
    DWORD rb = (((d & 0xFF00FF) * (256 - a) + (c & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    DWORD g  = (((d & 0x00FF00) * (256 - a) + (c & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}

static BOOL clip_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > g_w) *x1 = g_w;
    if (*y1 > g_h) *y1 = g_h;
    return g_px && *x0 < *x1 && *y0 < *y1;
}

static void px_fill(int x0, int y0, int x1, int y1, DWORD c)
{
    int x, y;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    for (y = y0; y < y1; y++) { DWORD *p = g_px + (SIZE_T)y * g_w; for (x = x0; x < x1; x++) p[x] = c; }
}

static void px_blend(int x0, int y0, int x1, int y1, DWORD c, int a)
{
    int x, y;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    for (y = y0; y < y1; y++) { DWORD *p = g_px + (SIZE_T)y * g_w; for (x = x0; x < x1; x++) p[x] = mix(p[x], c, a); }
}

static void px_vgrad(int x0, int y0, int x1, int y1, DWORD top, DWORD bot)
{
    int y, h = y1 - y0, oy = y0;
    for (y = oy; y < y1; y++)
        px_fill(x0, y, x1, y + 1, mix(top, bot, h > 1 ? (y - oy) * 256 / (h - 1) : 0));
}

static void px_frame(int x0, int y0, int x1, int y1, int t, DWORD c)
{
    px_fill(x0, y0, x1, y0 + t, c); px_fill(x0, y1 - t, x1, y1, c);
    px_fill(x0, y0 + t, x0 + t, y1 - t, c); px_fill(x1 - t, y0 + t, x1, y1 - t, c);
}

/* --------------------------------------------------- bicubic resampler */
/* Separable Catmull-Rom filter whose support widens when shrinking, so the
   same routine gives sharp enlargements and alias-free reductions. */

typedef struct { int *bounds; int *k; int ksize; } Coeffs;

static double cubic(double x)
{
    if (x < 0) x = -x;
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

static BOOL coeffs_make(Coeffs *c, int in, double in0, double in1, int out)
{
    double scale = (in1 - in0) / out, fs = scale < 1.0 ? 1.0 : scale, support = 2.0 * fs;
    int ksize = (int)support * 2 + 3, xx, i;
    double *w = (double *)xalloc(sizeof(double) * ksize);
    c->ksize  = ksize;
    c->bounds = (int *)xalloc(sizeof(int) * 2 * out);
    c->k      = (int *)xalloc(sizeof(int) * ksize * out);
    if (!w || !c->bounds || !c->k) { xfree(w); xfree(c->bounds); xfree(c->k); return FALSE; }
    for (xx = 0; xx < out; xx++) {
        double center = in0 + (xx + 0.5) * scale, sum = 0;
        int xmin = (int)(center - support + 0.5), xmax = (int)(center + support + 0.5), n;
        if (center - support + 0.5 < 0 || xmin < 0) xmin = 0;
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
    xfree(w);
    return TRUE;
}

static int clamp8(int v) { v = (v + 8192) >> 14; return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Resamples the source box (sx0,sy0)-(sx1,sy1) into dst (dw x dh, row pitch dpitch pixels). */
static void resample(const DWORD *src, int sw, int sh, double sx0, double sy0, double sx1, double sy1,
                     DWORD *dst, int dw, int dh, int dpitch)
{
    Coeffs cx, cy; DWORD *tmp; int ymin, ymax, x, y, i, rows;
    if (dw <= 0 || dh <= 0) return;
    if (!coeffs_make(&cx, sw, sx0, sx1, dw)) return;
    if (!coeffs_make(&cy, sh, sy0, sy1, dh)) { xfree(cx.bounds); xfree(cx.k); return; }
    ymin = cy.bounds[0]; ymax = cy.bounds[(dh - 1) * 2] + cy.bounds[(dh - 1) * 2 + 1];
    rows = ymax - ymin;
    tmp = (DWORD *)xalloc(sizeof(DWORD) * (SIZE_T)dw * rows);
    if (tmp) {
        for (y = 0; y < rows; y++) {                       /* horizontal pass */
            const DWORD *s = src + (SIZE_T)(y + ymin) * sw; DWORD *t = tmp + (SIZE_T)y * dw;
            for (x = 0; x < dw; x++) {
                const int *k = cx.k + x * cx.ksize; const DWORD *p = s + cx.bounds[x * 2];
                int n = cx.bounds[x * 2 + 1], r = 0, g = 0, b = 0;
                for (i = 0; i < n; i++) { DWORD v = p[i]; int kk = k[i];
                    r += (int)((v >> 16) & 255) * kk; g += (int)((v >> 8) & 255) * kk; b += (int)(v & 255) * kk; }
                t[x] = (DWORD)(clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b));
            }
        }
        for (y = 0; y < dh; y++) {                         /* vertical pass */
            const int *k = cy.k + y * cy.ksize; int n = cy.bounds[y * 2 + 1];
            const DWORD *base = tmp + (SIZE_T)(cy.bounds[y * 2] - ymin) * dw; DWORD *d = dst + (SIZE_T)y * dpitch;
            for (x = 0; x < dw; x++) {
                int r = 0, g = 0, b = 0;
                for (i = 0; i < n; i++) { DWORD v = base[(SIZE_T)i * dw + x]; int kk = k[i];
                    r += (int)((v >> 16) & 255) * kk; g += (int)((v >> 8) & 255) * kk; b += (int)(v & 255) * kk; }
                d[x] = (DWORD)(clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b));
            }
        }
        xfree(tmp);
    }
    xfree(cx.bounds); xfree(cx.k); xfree(cy.bounds); xfree(cy.k);
}

/* Cover-fit: fills dw x dh completely, cropping the source around its centre. */
static void resample_cover(const Image *im, DWORD *dst, int dw, int dh, int dpitch)
{
    double x0 = 0, y0 = 0, x1 = im->w, y1 = im->h;
    if ((double)dw * im->h > (double)dh * im->w) {
        double ch = (double)im->w * dh / dw; y0 = (im->h - ch) / 2; y1 = y0 + ch;
    } else {
        double cw = (double)im->h * dw / dh; x0 = (im->w - cw) / 2; x1 = x0 + cw;
    }
    resample(im->px, im->w, im->h, x0, y0, x1, y1, dst, dw, dh, dpitch);
}

/* ---------------------------------------------- JPEG decoding (GDI+ flat) */

typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupInput;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } GpBitmapData;
typedef struct { INT X, Y, Width, Height; } GpRectI;
int  WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
int  WINAPI GdipGetImageWidth(void *image, UINT *width);
int  WINAPI GdipGetImageHeight(void *image, UINT *height);
int  WINAPI GdipBitmapLockBits(void *bitmap, const GpRectI *rect, UINT flags, INT format, GpBitmapData *data);
int  WINAPI GdipBitmapUnlockBits(void *bitmap, GpBitmapData *data);
int  WINAPI GdipDisposeImage(void *image);
#define GP_PIXFMT_32RGB 0x00022009

static BOOL decode_jpeg(int id, Image *out)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(100 + id), (LPCWSTR)RT_RCDATA);
    HGLOBAL hg = res ? LoadResource(NULL, res) : NULL;
    const BYTE *data = hg ? (const BYTE *)LockResource(hg) : NULL;
    DWORD size = res ? SizeofResource(NULL, res) : 0;
    IStream *stm; void *bmp = NULL; BOOL ok = FALSE;
    if (!data || !size) return FALSE;
    if (!g_gdipToken) {
        GpStartupInput in = { 1, NULL, FALSE, FALSE };
        if (GdiplusStartup(&g_gdipToken, &in, NULL) != 0) { g_gdipToken = 0; return FALSE; }
    }
    stm = SHCreateMemStream(data, size);
    if (!stm) return FALSE;
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        UINT w = 0, h = 0; GpBitmapData bd; GpRectI rc;
        GdipGetImageWidth(bmp, &w); GdipGetImageHeight(bmp, &h);
        rc.X = 0; rc.Y = 0; rc.Width = (INT)w; rc.Height = (INT)h;
        if (w && h && w <= 8192 && h <= 8192 && GdipBitmapLockBits(bmp, &rc, 1, GP_PIXFMT_32RGB, &bd) == 0) {
            DWORD *px = (DWORD *)xalloc(sizeof(DWORD) * (SIZE_T)w * h);
            if (px) {
                UINT x, y;
                for (y = 0; y < h; y++) {
                    const DWORD *s = (const DWORD *)((const BYTE *)bd.Scan0 + (INT_PTR)y * bd.Stride);
                    DWORD *d = px + (SIZE_T)y * w;
                    for (x = 0; x < w; x++) d[x] = s[x] & 0xFFFFFF;
                }
                out->px = px; out->w = (int)w; out->h = (int)h; ok = TRUE;
            }
            GdipBitmapUnlockBits(bmp, &bd);
        }
        GdipDisposeImage(bmp);
    }
    stm->lpVtbl->Release(stm);
    return ok;
}

static const Image *get_image(int id)
{
    Image *im = &g_img[id];
    if (!im->px && !decode_jpeg(id, im)) {
        /* Decoder unavailable: fall back to a plain night gradient so the app stays usable. */
        int x, y;
        im->w = 96; im->h = 32;
        im->px = (DWORD *)xalloc(sizeof(DWORD) * 96 * 32);
        if (!im->px) return NULL;
        for (y = 0; y < 32; y++) for (x = 0; x < 96; x++) im->px[y * 96 + x] = mix(0x0E1830, 0x3B5374, y * 8);
    }
    return im;
}

static void free_unused_images(void)
{
    BOOL used[13] = {0}; int i;
    for (i = 0; i < 24; i++) used[g_sched[i]] = TRUE;
    for (i = 1; i <= 12; i++) if (!used[i] && g_img[i].px) { xfree(g_img[i].px); g_img[i].px = NULL; }
}

/* ---------------------------------------------------------------- fonts */

static HFONT mkfont(const WCHAR *face, int px, int weight, BOOL italic, BOOL underline)
{
    return CreateFontW(-dp(px), 0, 0, 0, weight, (DWORD)italic, (DWORD)underline, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void fonts_free(void)
{
    int i;
    for (i = 0; i < F_COUNT; i++) if (g_font[i]) { DeleteObject(g_font[i]); g_font[i] = NULL; }
}

static void fonts_make(void)
{
    TEXTMETRICW tm; HGDIOBJ old;
    fonts_free();
    g_font[F_BODY]   = mkfont(L"Segoe UI", 15, FW_NORMAL, FALSE, FALSE);
    g_font[F_BOLD]   = mkfont(L"Segoe UI", 15, FW_BOLD, FALSE, FALSE);
    g_font[F_ITALIC] = mkfont(L"Segoe UI", 15, FW_NORMAL, TRUE, FALSE);
    g_font[F_UNDER]  = mkfont(L"Segoe UI", 15, FW_NORMAL, FALSE, TRUE);
    g_font[F_TITLE]  = mkfont(L"Segoe UI Semibold", 18, FW_SEMIBOLD, FALSE, FALSE);
    g_font[F_NAME]   = mkfont(L"Georgia", 34, FW_NORMAL, FALSE, FALSE);
    g_font[F_LABEL]  = mkfont(L"Segoe UI Semibold", 12, FW_SEMIBOLD, FALSE, FALSE);
    g_font[F_BTN]    = mkfont(L"Segoe UI Semibold", 14, FW_SEMIBOLD, FALSE, FALSE);
    g_font[F_NUM]    = mkfont(L"Segoe UI", 11, FW_NORMAL, FALSE, FALSE);
    g_font[F_NUMB]   = mkfont(L"Segoe UI", 11, FW_BOLD, FALSE, FALSE);
    g_font[F_SMALL]  = mkfont(L"Segoe UI", 12, FW_NORMAL, FALSE, FALSE);
    old = SelectObject(g_dc, g_font[F_BODY]);
    GetTextMetricsW(g_dc, &tm); g_lineH = tm.tmHeight + tm.tmExternalLeading + dp(3);
    SelectObject(g_dc, g_font[F_TITLE]);
    GetTextMetricsW(g_dc, &tm); g_titleH = tm.tmHeight + tm.tmExternalLeading + dp(2);
    SelectObject(g_dc, old);
}

static HFONT style_font(int s)
{
    static const BYTE map[S_COUNT] = { F_BODY, F_BOLD, F_ITALIC, F_UNDER, F_TITLE };
    return g_font[map[s]];
}

/* ------------------------------------------------------ rich text layout */

static void txt_add(const WCHAR *s, int style)
{
    while (*s && g_txtLen < TXT_MAX - 1) { g_txt[g_txtLen] = *s++; g_sty[g_txtLen++] = (BYTE)style; }
}

static void text_build(void)
{
    g_txtLen = 0;
    if (g_has) {
        const Event *e = &EVENTS[g_sched[g_cur]]; WCHAR t[96]; int i;
        wsprintfW(t, L"Hour %d: Weather Event: %s\n\n", g_cur, e->name);
        txt_add(t, S_TITLE);
        for (i = 0; i < e->nruns; i++) txt_add(e->runs[i].text, e->runs[i].style);
    } else {
        txt_add(L"No weather rolled yet\n\n", S_TITLE);
        txt_add(L"Press ", S_N); txt_add(L"New Day ", S_B);
        txt_add(L"to roll twenty-four hours of weather for the Dale.\n", S_N);
        txt_add(L"Then click an hour on the bar, or use the ", S_N); txt_add(L"Left ", S_B);
        txt_add(L"and ", S_N); txt_add(L"Right ", S_B); txt_add(L"arrow keys, to step through the day.", S_N);
    }
}

static int txt_measure(int a, int b)
{
    int w = 0;
    while (a < b) {
        int e = a + 1; SIZE sz;
        while (e < b && g_sty[e] == g_sty[a]) e++;
        SelectObject(g_dc, style_font(g_sty[a]));
        GetTextExtentPoint32W(g_dc, g_txt + a, e - a, &sz);
        w += sz.cx; a = e;
    }
    return w;
}

static void txt_emit(int a, int b, int x, int y)
{
    while (b > a && g_txt[b - 1] == L' ') b--;
    while (a < b && g_nfrag < FRAG_MAX) {
        int e = a + 1; SIZE sz; Frag *f = &g_frag[g_nfrag++];
        while (e < b && g_sty[e] == g_sty[a]) e++;
        SelectObject(g_dc, style_font(g_sty[a]));
        GetTextExtentPoint32W(g_dc, g_txt + a, e - a, &sz);
        f->x = x; f->y = y; f->off = (unsigned short)a; f->len = (unsigned short)(e - a); f->style = g_sty[a];
        x += sz.cx; a = e;
    }
}

static int txt_trim(int a, int b) { while (b > a && g_txt[b - 1] == L' ') b--; return b; }

/* Greedy word wrap of one logical line; continuation lines start at `hang`. */
static void txt_wrap(int a, int b, int width, int indent, int hang, int lh, int *y)
{
    int p = a; BOOL first = TRUE;
    while (p < b) {
        int x0 = first ? indent : hang, avail = width - x0, best = -1, q = p;
        if (txt_measure(p, txt_trim(p, b)) <= avail) { txt_emit(p, b, x0, *y); *y += lh; return; }
        while (q < b) {
            int e = q;
            while (e < b && g_txt[e] != L' ') e++;
            if (txt_measure(p, e) > avail) { if (best < 0) best = e; break; }   /* an over-long word stays whole */
            best = e;
            while (e < b && g_txt[e] == L' ') e++;
            q = e;
        }
        if (best <= p) best = b;
        txt_emit(p, best, x0, *y); *y += lh;
        p = best;
        while (p < b && g_txt[p] == L' ') p++;
        first = FALSE;
    }
}

static void text_layout(void)
{
    HGDIOBJ old = SelectObject(g_dc, g_font[F_BODY]);
    int width = g_rcText.right - g_rcText.left - dp(10), pos = 0, y = 0;
    if (width < dp(120)) width = dp(120);
    g_nfrag = 0;
    while (pos < g_txtLen) {
        int b = pos;
        while (b < g_txtLen && g_txt[b] != L'\n') b++;
        if (b == pos) {
            y += dp(9);                                        /* blank line: a small gap */
        } else if (g_sty[pos] == S_TITLE) {
            txt_wrap(pos, b, width, 0, 0, g_titleH, &y);
        } else {
            /* Leading spaces become a real indent; "a)", "ii)" and bullet markers get a hanging indent. */
            int a = pos, lead = 0, indent, hang, e;
            while (a < b && g_txt[a] == L' ') { a++; lead++; }
            indent = lead * dp(6); hang = indent;
            e = a;
            while (e < b && g_txt[e] != L' ') e++;
            if (e > a && e < b && (g_txt[e - 1] == L')' || (e - a == 1 && g_txt[a] == 0x25CF))) {
                while (e < b && g_txt[e] == L' ') e++;
                hang = indent + txt_measure(a, e);
            }
            if (hang > width / 2) hang = indent;
            txt_wrap(a, b, width, indent, hang, g_lineH, &y);
        }
        pos = b + 1;
    }
    g_textH = y;
    SelectObject(g_dc, old);
}

static int text_view_h(void) { int h = g_rcText.bottom - g_rcText.top; return h > 0 ? h : 0; }
static int text_max_scroll(void) { int m = g_textH - text_view_h(); return m > 0 ? m : 0; }

/* --------------------------------------------------------------- layout */

static int cell_x(int i) { return g_rcBar.left + MulDiv(g_rcBar.right - g_rcBar.left, i, 24); }

/* The content never gets wider than this: the scene pictures are 720 px wide and
   must not be blown up into mush in a maximised window. Wider windows get margins. */
static int column_w(int W) { int m = dp(960); return W < m ? W : m; }

static void layout(void)
{
    int W = g_w, H = g_h, M = dp(24), CW = column_w(W), X0 = (W - CW) / 2, X1 = X0 + CW;
    int sceneH = clampi(CW / 3, dp(150), dp(320)), strip = dp(62), bh = dp(36), by;
    if (sceneH > H / 2) sceneH = H / 2;
    SetRect(&g_rcScene, X0, 0, X1, sceneH);
    SetRect(&g_rcBar, X0 + M, sceneH + dp(16), X1 - M, sceneH + dp(16) + dp(54));
    g_numsTop = g_rcBar.bottom + dp(6);
    SetRect(&g_rcBarZone, 0, sceneH, W, g_numsTop + dp(16) + dp(8));
    SetRect(&g_rcBtnZone, 0, H - strip, W, H);
    SetRect(&g_rcTextZone, 0, g_rcBarZone.bottom, W, g_rcBtnZone.top);
    SetRect(&g_rcText, X0 + dp(28), g_rcTextZone.top + dp(6), X1 - dp(22), g_rcTextZone.bottom - dp(8));
    by = g_rcBtnZone.top + (strip - bh) / 2;
    SetRect(&g_rcBtn[BTN_NEW], X0 + M, by, X0 + M + dp(128), by + bh);
    SetRect(&g_rcBtn[BTN_NEXT], X1 - M - dp(128), by, X1 - M, by + bh);
    SetRect(&g_rcBtn[BTN_PREV], g_rcBtn[BTN_NEXT].left - dp(10) - dp(148), by, g_rcBtn[BTN_NEXT].left - dp(10), by + bh);
}

/* Fixed (non-text) vertical space of the layout for a given client width. */
static int chrome_height(int W)
{
    return clampi(column_w(W) / 3, dp(150), dp(320)) + dp(16) + dp(54) + dp(6) + dp(16) + dp(8) + dp(6) + dp(8) + dp(62);
}

/* Like the Python adjust_window_size(): if the description does not fit, grow the
   window (never past the monitor work area). Beyond that the wheel scrolls. */
static void ensure_fit(void)
{
    int need = g_textH - text_view_h();
    RECT wr; MONITORINFO mi; int h, maxH, top;
    if (need <= 0 || !g_hwnd || IsZoomed(g_hwnd) || IsIconic(g_hwnd)) return;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    GetWindowRect(g_hwnd, &wr);
    h = wr.bottom - wr.top; maxH = mi.rcWork.bottom - mi.rcWork.top;
    if (h >= maxH) return;
    h = h + need > maxH ? maxH : h + need;
    top = wr.top;
    if (top + h > mi.rcWork.bottom) top = mi.rcWork.bottom - h;
    if (top < mi.rcWork.top) top = mi.rcWork.top;
    SetWindowPos(g_hwnd, NULL, wr.left, top, wr.right - wr.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------- painting */

static void text_out(int x, int y, const WCHAR *s, int n, DWORD c)
{
    SetTextColor(g_dc, RGB_OF(c));
    TextOutW(g_dc, x, y, s, n);
}

static void text_shadowed(int x, int y, const WCHAR *s, int n, DWORD c)
{
    int o = dp(1) > 0 ? dp(1) : 1;
    text_out(x + o, y + o, s, n, C_SHADOW);
    text_out(x, y, s, n, c);
}

static void scene_cache(int event, BOOL dim)
{
    int w = g_rcScene.right - g_rcScene.left, h = g_rcScene.bottom, x, y;
    BOOL edges = w < g_w;                                     /* margins beside the picture */
    int key = event | (dim ? 64 : 0) | (edges ? 128 : 0);
    const Image *im;
    if (g_scenePx && g_sceneW == w && g_sceneH == h && g_sceneKey == key) return;
    if (!g_scenePx || g_sceneW != w || g_sceneH != h) {
        xfree(g_scenePx);
        g_scenePx = (DWORD *)xalloc(sizeof(DWORD) * (SIZE_T)w * h);
        g_sceneW = w; g_sceneH = h;
    }
    g_sceneKey = -1;
    if (!g_scenePx || !(im = get_image(event))) return;
    resample_cover(im, g_scenePx, w, h, w);
    /* Melt the lower part of the picture into the window background. */
    for (y = 0; y < h; y++) {
        DWORD *p = g_scenePx + (SIZE_T)y * w;
        int t = (y * 1024 / h - 380) * 1024 / 644, a;
        t = clampi(t, 0, 1024);
        a = (t * t / 1024) * (3072 - 2 * t) / 1024;          /* smoothstep, 0..1024 */
        a = a * 250 / 1024;
        if (dim) a = 120 + a * (256 - 120) / 256;
        if (a > 0) for (x = 0; x < w; x++) p[x] = mix(p[x], C_BG, a);
    }
    if (edges) {                                              /* and its sides into the margins */
        int f = dp(72);
        if (f > w / 3) f = w / 3;
        for (x = 0; x < f; x++) {
            int t = x * 1024 / f, a = 256 - ((t * t / 1024) * (3072 - 2 * t) / 1024) / 4;
            for (y = 0; y < h; y++) {
                DWORD *p = g_scenePx + (SIZE_T)y * w;
                p[x] = mix(p[x], C_BG, a); p[w - 1 - x] = mix(p[w - 1 - x], C_BG, a);
            }
        }
    }
    g_sceneKey = key;
}

static void draw_scene(void)
{
    int x0 = g_rcScene.left, w = g_rcScene.right - x0, h = g_rcScene.bottom, y, M = dp(24);
    HGDIOBJ old; SIZE sz; WCHAR buf[96]; int n, nameY, labelY;
    if (h <= 0 || w <= 0) return;
    scene_cache(g_has ? g_sched[g_cur] : 1, !g_has);
    GdiFlush();
    if (x0 > 0) px_fill(0, 0, x0, h, C_BG);
    if (x0 + w < g_w) px_fill(x0 + w, 0, g_w, h, C_BG);
    if (g_scenePx && g_sceneKey >= 0)
        for (y = 0; y < h; y++) memcpy(g_px + (SIZE_T)y * g_w + x0, g_scenePx + (SIZE_T)y * w, sizeof(DWORD) * w);
    else
        px_fill(x0, 0, x0 + w, h, C_BG_SOFT);

    old = SelectObject(g_dc, g_font[F_NAME]);
    SetBkMode(g_dc, TRANSPARENT);
    {
        const WCHAR *name = g_has ? EVENTS[g_sched[g_cur]].name : L"Press New Day to roll the weather";
        int len = lstrlenW(name);
        if (!g_has) SelectObject(g_dc, g_font[F_TITLE]);
        GetTextExtentPoint32W(g_dc, name, len, &sz);
        nameY = h - dp(12) - sz.cy;
        text_shadowed(x0 + M - dp(1), nameY, name, len, C_BRIGHT);
    }
    SelectObject(g_dc, g_font[F_LABEL]);
    SetTextCharacterExtra(g_dc, dp(2));
    n = g_has ? wsprintfW(buf, L"HOUR %d", g_cur) : wsprintfW(buf, L"ICEWIND DALE");
    GetTextExtentPoint32W(g_dc, buf, n, &sz);
    labelY = nameY - sz.cy - dp(1);
    text_shadowed(x0 + M, labelY, buf, n, C_TITLE);
    SetTextCharacterExtra(g_dc, 0);
    if (g_has) {
        /* Extent of the current stretch of weather, right-aligned. */
        int s = g_cur, e = g_cur;
        while (s > 0 && g_sched[s - 1] == g_sched[g_cur]) s--;
        while (e < 23 && g_sched[e + 1] == g_sched[g_cur]) e++;
        SelectObject(g_dc, g_font[F_SMALL]);
        if (s == e) n = wsprintfW(buf, L"1 hour  ·  hour %d only", s);
        else        n = wsprintfW(buf, L"%d hours  ·  hour %d to %d", e - s + 1, s, e);
        GetTextExtentPoint32W(g_dc, buf, n, &sz);
        if (sz.cx < w / 2 - M) text_shadowed(x0 + w - M - sz.cx, h - dp(17) - sz.cy, buf, n, C_TEXT);
    }
    SelectObject(g_dc, old);
}

static void bar_cache(void)
{
    int w = g_rcBar.right - g_rcBar.left, h = g_rcBar.bottom - g_rcBar.top, s, x, y;
    if (g_barValid && g_barPx && g_barW == w && g_barH == h) return;
    if (!g_barPx || g_barW != w || g_barH != h) {
        xfree(g_barPx);
        g_barPx = (DWORD *)xalloc(sizeof(DWORD) * (SIZE_T)w * h);
        g_barW = w; g_barH = h;
    }
    g_barValid = FALSE;
    if (!g_barPx || w <= 0 || h <= 0) return;
    for (s = 0; s < 24; ) {
        /* A run of k equal hours is one continuous k-cell-wide crop of that scene. */
        int e = s, x0, x1; const Image *im;
        while (e < 23 && g_sched[e + 1] == g_sched[s]) e++;
        x0 = cell_x(s) - g_rcBar.left; x1 = cell_x(e + 1) - g_rcBar.left;
        im = get_image(g_sched[s]);
        if (im && x1 > x0) resample_cover(im, g_barPx + x0, x1 - x0, h, w);
        for (x = s + 1; x <= e; x++) {                        /* faint hour ticks inside a run */
            int cx = cell_x(x) - g_rcBar.left;
            for (y = 0; y < h; y++) { DWORD *p = g_barPx + (SIZE_T)y * w + cx; *p = mix(*p, C_BG, 70); }
        }
        if (e < 23) {                                         /* separator between different events */
            int cx = x1 - 1, t = dp(2) > 1 ? dp(2) : 2, i;
            for (i = 0; i < t; i++) if (cx + i >= 0 && cx + i < w)
                for (y = 0; y < h; y++) g_barPx[(SIZE_T)y * w + cx + i] = C_BG;
        }
        s = e + 1;
    }
    for (x = 0; x < w; x++) {                                 /* soft top light / bottom shade */
        DWORD *p = g_barPx + x;
        if (*p != C_BG) *p = mix(*p, 0xFFFFFF, 34);
        p = g_barPx + (SIZE_T)(h - 1) * w + x;
        if (*p != C_BG) *p = mix(*p, 0x000000, 70);
    }
    g_barValid = TRUE;
}

static void draw_bar(void)
{
    RECT z = g_rcBarZone, b = g_rcBar; int w = b.right - b.left, h = b.bottom - b.top, i, y;
    HGDIOBJ old;
    GdiFlush();
    px_fill(z.left, z.top, z.right, z.bottom, C_BG);
    if (w <= 0 || h <= 0) return;
    px_frame(b.left - 1, b.top - 1, b.right + 1, b.bottom + 1, 1, C_FRAME);
    if (g_has) {
        bar_cache();
        if (g_barValid && b.right <= g_w && b.bottom <= g_h)
            for (y = 0; y < h; y++) memcpy(g_px + (SIZE_T)(b.top + y) * g_w + b.left, g_barPx + (SIZE_T)y * w, sizeof(DWORD) * w);
        for (i = 0; i < 24; i++) {
            int x0 = cell_x(i), x1 = cell_x(i + 1);
            if (i == g_cur) continue;
            if (i == g_hoverCell) px_blend(x0, b.top, x1, b.bottom, 0xFFFFFF, 26);
            else                  px_blend(x0, b.top, x1, b.bottom, C_BG, 72);   /* the current hour stays lit */
        }
        {   /* current hour: dark-light-dark frame (reads on snow and on night alike) + marker */
            int x0 = cell_x(g_cur), x1 = cell_x(g_cur + 1), t = dp(2) > 1 ? dp(2) : 2, m = dp(5), cx = (x0 + x1) / 2, r;
            px_frame(x0 - t - 1, b.top - t - 1, x1 + t + 1, b.bottom + t + 1, 1, C_SHADOW);
            px_frame(x0 - t, b.top - t, x1 + t, b.bottom + t, t, C_ICE);
            px_frame(x0, b.top, x1, b.bottom, 1, C_SHADOW);
            for (r = 0; r < m; r++)                           /* small downward marker above the cell */
                px_fill(cx - (m - 1 - r), b.top - t - dp(3) - m + r, cx + (m - r), b.top - t - dp(3) - m + r + 1, C_ICE);
        }
    } else {
        px_fill(b.left, b.top, b.right, b.bottom, C_BG_SOFT);
        for (i = 1; i < 24; i++) px_fill(cell_x(i), b.top, cell_x(i) + 1, b.bottom, C_LINE);
    }
    old = SelectObject(g_dc, g_font[F_NUM]);
    SetBkMode(g_dc, TRANSPARENT);
    for (i = 0; i < 24; i++) {
        WCHAR t[4]; SIZE sz; int n = wsprintfW(t, L"%d", i), cx = (cell_x(i) + cell_x(i + 1)) / 2;
        BOOL cur = g_has && i == g_cur;
        SelectObject(g_dc, g_font[cur ? F_NUMB : F_NUM]);
        GetTextExtentPoint32W(g_dc, t, n, &sz);
        text_out(cx - sz.cx / 2, g_numsTop, t, n, cur ? C_BRIGHT : (g_has && i == g_hoverCell) ? C_TEXT : C_DIM);
    }
    SelectObject(g_dc, old);
}

static void draw_text(void)
{
    static const DWORD col[S_COUNT] = { C_TEXT, C_BRIGHT, C_ITALIC, C_TEXT, C_TITLE };
    RECT z = g_rcTextZone, r = g_rcText; int i, vh = text_view_h(), maxs = text_max_scroll(), saved;
    HGDIOBJ old;
    GdiFlush();
    px_fill(z.left, z.top, z.right, z.bottom, C_BG);
    if (vh <= 0) return;
    saved = SaveDC(g_dc);
    IntersectClipRect(g_dc, r.left, r.top, r.right, r.bottom);
    SetBkMode(g_dc, TRANSPARENT);
    old = SelectObject(g_dc, g_font[F_BODY]);
    for (i = 0; i < g_nfrag; i++) {
        const Frag *f = &g_frag[i]; int y = f->y - g_scroll;
        if (y > vh || y + g_titleH + g_lineH < 0) continue;
        SelectObject(g_dc, style_font(f->style));
        text_out(r.left + f->x, r.top + y, g_txt + f->off, f->len, col[f->style]);
    }
    SelectObject(g_dc, old);
    RestoreDC(g_dc, saved);
    if (maxs > 0) {                                           /* soft edges where text continues + slim scroll indicator */
        int tw = dp(3), x = r.right + dp(12), th = vh * vh / g_textH, ty, fade = dp(14), k;
        GdiFlush();
        for (k = 0; k < fade && fade > 0; k++) {
            int a = 256 - (k + 1) * 256 / (fade + 1);
            if (g_scroll > 0)    px_blend(r.left, r.top + k, r.right, r.top + k + 1, C_BG, a);
            if (g_scroll < maxs) px_blend(r.left, r.bottom - 1 - k, r.right, r.bottom - k, C_BG, a);
        }
        if (th < dp(24)) th = dp(24);
        if (th > vh) th = vh;
        ty = r.top + MulDiv(vh - th, g_scroll, maxs);
        px_fill(x, r.top, x + tw, r.bottom, C_BG_SOFT);
        px_fill(x, ty, x + tw, ty + th, 0x4C6485);
    }
}

static BOOL btn_enabled(int i) { return i == BTN_NEW || g_has; }

static void draw_buttons(void)
{
    static const WCHAR *label[BTN_COUNT] = { L"New Day", L"‹   Previous hour", L"Next hour   ›" };
    RECT z = g_rcBtnZone; int i; HGDIOBJ old;
    GdiFlush();
    px_fill(z.left, z.top, z.right, z.bottom, C_BG);
    px_fill(z.left, z.top, z.right, z.top + 1, C_LINE);
    for (i = 0; i < BTN_COUNT; i++) {
        RECT r = g_rcBtn[i];
        BOOL en = btn_enabled(i), hov = en && g_hoverBtn == i && (g_pressBtn < 0 || g_pressBtn == i);
        BOOL down = en && g_pressBtn == i && g_hoverBtn == i;
        DWORD top, bot, border;
        if (i == BTN_NEW) {
            top = down ? 0x1A3F69 : hov ? 0x3A7DBD : 0x2F6AA3; bot = down ? 0x245688 : hov ? 0x245A8E : 0x1C4472;
            border = hov ? 0xB5DCF7 : 0x7FBDEB;
        } else if (!en) {
            top = bot = 0x101722; border = 0x1D2838;
        } else {
            top = down ? 0x111925 : hov ? 0x26364E : 0x1B2638; bot = down ? 0x1A2538 : hov ? 0x1A2639 : 0x131B29;
            border = hov ? 0x6C8BB0 : 0x35465F;
        }
        px_vgrad(r.left, r.top, r.right, r.bottom, top, bot);
        px_frame(r.left, r.top, r.right, r.bottom, 1, border);
        if (en && !down) px_blend(r.left + 1, r.top + 1, r.right - 1, r.top + 2, 0xFFFFFF, 34);
        px_fill(r.left, r.top, r.left + 1, r.top + 1, C_BG);       px_fill(r.right - 1, r.top, r.right, r.top + 1, C_BG);
        px_fill(r.left, r.bottom - 1, r.left + 1, r.bottom, C_BG); px_fill(r.right - 1, r.bottom - 1, r.right, r.bottom, C_BG);
        if (g_focusBtn == i) px_frame(r.left - dp(3), r.top - dp(3), r.right + dp(3), r.bottom + dp(3), dp(1) > 0 ? dp(1) : 1, C_ICE);
    }
    old = SelectObject(g_dc, g_font[F_BTN]);
    SetBkMode(g_dc, TRANSPARENT);
    for (i = 0; i < BTN_COUNT; i++) {
        RECT r = g_rcBtn[i]; SIZE sz; int n = lstrlenW(label[i]);
        BOOL en = btn_enabled(i), down = en && g_pressBtn == i && g_hoverBtn == i;
        GetTextExtentPoint32W(g_dc, label[i], n, &sz);
        text_out((r.left + r.right - sz.cx) / 2, (r.top + r.bottom - sz.cy) / 2 - dp(1) + (down ? 1 : 0), label[i], n,
                 !en ? 0x475468 : i == BTN_NEW ? C_BRIGHT : (g_hoverBtn == i ? C_BRIGHT : C_TEXT));
    }
    {   /* key hint between the buttons, only when there is room for it */
        static const WCHAR hint[] = L"← →  hour     N  new day";
        SIZE sz; int n = (int)(sizeof(hint) / sizeof(hint[0])) - 1;
        int l = g_rcBtn[BTN_NEW].right + dp(16), r = g_rcBtn[BTN_PREV].left - dp(16);
        SelectObject(g_dc, g_font[F_SMALL]);
        GetTextExtentPoint32W(g_dc, hint, n, &sz);
        if (sz.cx <= r - l) text_out(l, (g_rcBtn[BTN_NEW].top + g_rcBtn[BTN_NEW].bottom - sz.cy) / 2, hint, n, 0x53627A);
    }
    SelectObject(g_dc, old);
}

static void render(void)
{
    if (!g_px) return;
    if (g_dirty & D_SCENE) draw_scene();
    if (g_dirty & D_BAR)   draw_bar();
    if (g_dirty & D_TEXT)  draw_text();
    if (g_dirty & D_BTN)   draw_buttons();
    g_dirty = 0;
    GdiFlush();
}

static void mark(unsigned what)
{
    g_dirty |= what;
    if (!g_hwnd) return;
    if (what & D_SCENE) InvalidateRect(g_hwnd, &g_rcScene, FALSE);
    if (what & D_BAR)   InvalidateRect(g_hwnd, &g_rcBarZone, FALSE);
    if (what & D_TEXT)  InvalidateRect(g_hwnd, &g_rcTextZone, FALSE);
    if (what & D_BTN)   InvalidateRect(g_hwnd, &g_rcBtnZone, FALSE);
}

/* ------------------------------------------------------------ back buffer */

static void backbuffer_resize(int w, int h)
{
    BITMAPINFO bi; HBITMAP bmp; void *bits = NULL;
    if (w == g_w && h == g_h && g_bmp) return;
    if (g_bmp) { SelectObject(g_dc, g_bmpOld); DeleteObject(g_bmp); g_bmp = NULL; g_px = NULL; }
    g_w = w; g_h = h;
    if (w <= 0 || h <= 0) return;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;       /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return;
    g_bmp = bmp; g_px = (DWORD *)bits;
    g_bmpOld = (HBITMAP)SelectObject(g_dc, bmp);
}

/* ------------------------------------------------------------- actions */

static void refresh_text(BOOL fit)
{
    text_build();
    text_layout();
    g_scroll = 0;
    if (fit) ensure_fit();                                     /* may resize -> WM_SIZE -> relayout */
}

static void set_hour(int h)
{
    if (!g_has) return;
    h = clampi(h, 0, 23);
    if (h == g_cur) return;
    g_cur = h;
    refresh_text(TRUE);
    mark(D_SCENE | D_BAR | D_TEXT);
}

static void do_new_day(void)
{
    new_day();
    free_unused_images();
    g_barValid = FALSE;
    refresh_text(TRUE);
    mark(D_ALL);
}

static void scroll_to(int s)
{
    s = clampi(s, 0, text_max_scroll());
    if (s != g_scroll) { g_scroll = s; mark(D_TEXT); }
}

static int hit_btn(int x, int y)
{
    POINT p; int i;
    p.x = x; p.y = y;
    for (i = 0; i < BTN_COUNT; i++) if (PtInRect(&g_rcBtn[i], p)) return i;
    return -1;
}

static int hit_cell(int x, int y)
{
    int w = g_rcBar.right - g_rcBar.left;
    if (!g_has || w <= 0 || x < g_rcBar.left || x >= g_rcBar.right) return -1;
    if (y < g_rcBar.top - dp(6) || y >= g_numsTop + dp(16)) return -1;
    return clampi((x - g_rcBar.left) * 24 / w, 0, 23);
}

static void set_hover(int cell, int btn)
{
    if (cell != g_hoverCell) { g_hoverCell = cell; mark(D_BAR); }
    if (btn != g_hoverBtn)   { g_hoverBtn = btn;   mark(D_BTN); }
}

static void press_button(int i)
{
    if (i == BTN_NEW) do_new_day();
    else if (g_has) set_hour((g_cur + (i == BTN_NEXT ? 1 : 23)) % 24);
}

/* Tab / Shift+Tab walk a keyboard focus ring over the enabled buttons. */
static void focus_step(int dir)
{
    int i = g_focusBtn, k;
    for (k = 0; k < BTN_COUNT; k++) {
        i = i < 0 ? (dir > 0 ? 0 : BTN_COUNT - 1) : (i + dir + BTN_COUNT) % BTN_COUNT;
        if (btn_enabled(i)) break;
    }
    if (i != g_focusBtn) { g_focusBtn = i; mark(D_BTN); }
}

/* Ctrl+C: the description shown on screen, as plain text. */
static void copy_text(HWND hwnd)
{
    HGLOBAL hg; WCHAR *d; int i, n = 0;
    for (i = 0; i < g_txtLen; i++) n += g_txt[i] == L'\n' ? 2 : 1;
    if (!n || !OpenClipboard(hwnd)) return;
    hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(WCHAR) * ((SIZE_T)n + 1));
    d = hg ? (WCHAR *)GlobalLock(hg) : NULL;
    if (d) {
        for (i = 0; i < g_txtLen; i++) { if (g_txt[i] == L'\n') *d++ = L'\r'; *d++ = g_txt[i]; }
        *d = 0;
        GlobalUnlock(hg);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
    } else if (hg) GlobalFree(hg);
    CloseClipboard();
}

/* --------------------------------------------------------- window proc */

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
    GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
    cw += (wr.right - wr.left) - cr.right; ch += (wr.bottom - wr.top) - cr.bottom;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        if (cw > wa.right - wa.left) cw = wa.right - wa.left;
        if (ch > wa.bottom - wa.top) ch = wa.bottom - wa.top;
        if (wr.left + cw > wa.right) wr.left = wa.right - cw;
        if (wr.top + ch > wa.bottom) wr.top = wa.bottom - ch;
        if (wr.left < wa.left) wr.left = wa.left;
        if (wr.top < wa.top) wr.top = wa.top;
    }
    SetWindowPos(hwnd, NULL, wr.left, wr.top, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void on_size(int w, int h)
{
    backbuffer_resize(w, h);
    layout();
    text_layout();
    g_scroll = clampi(g_scroll, 0, text_max_scroll());
    g_dirty = D_ALL;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_dpi = (int)query_dpi(hwnd);
        g_dc = CreateCompatibleDC(NULL);
        fonts_make();
        text_build();
        dark_titlebar(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        if (g_px) {
            if (g_dirty) render();
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, g_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) on_size(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp; RECT r;
        SetRect(&r, 0, 0, dp(640), dp(480));
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        mm->ptMinTrackSize.x = r.right - r.left; mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case 0x02E0: {  /* WM_DPICHANGED */
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        fonts_make();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        on_size(g_w, g_h);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!g_tracking) {
            TRACKMOUSEEVENT t; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = hwnd; t.dwHoverTime = 0;
            g_tracking = TrackMouseEvent(&t);
        }
        if (g_scrub) {
            int w = g_rcBar.right - g_rcBar.left, c = w > 0 ? clampi((x - g_rcBar.left) * 24 / w, 0, 23) : 0;
            if (x < g_rcBar.left) c = 0;
            set_hover(c, -1);
            set_hour(c);
        } else {
            set_hover(g_pressBtn >= 0 ? -1 : hit_cell(x, y), hit_btn(x, y));
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (!g_scrub && g_pressBtn < 0) set_hover(-1, -1);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), c = hit_cell(x, y), b = hit_btn(x, y);
        if (g_focusBtn >= 0) { g_focusBtn = -1; mark(D_BTN); }   /* the mouse takes over */
        if (b >= 0 && btn_enabled(b)) {
            g_pressBtn = b; g_hoverBtn = b; SetCapture(hwnd); mark(D_BTN);
        } else if (c >= 0) {
            g_scrub = TRUE; SetCapture(hwnd); set_hover(c, -1); set_hour(c);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), b = g_pressBtn;
        BOOL fire = b >= 0 && hit_btn(x, y) == b;
        g_pressBtn = -1; g_scrub = FALSE;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (b >= 0) mark(D_BTN);
        if (fire) press_button(b);
        set_hover(hit_cell(x, y), hit_btn(x, y));
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (g_pressBtn >= 0) { g_pressBtn = -1; mark(D_BTN); }
        g_scrub = FALSE;
        return 0;

    case WM_MOUSEWHEEL:
        scroll_to(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * g_lineH * 3 / WHEEL_DELTA);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; int b;
            GetCursorPos(&p); ScreenToClient(hwnd, &p);
            b = hit_btn(p.x, p.y);
            SetCursor(LoadCursorW(NULL, (b >= 0 && btn_enabled(b)) || hit_cell(p.x, p.y) >= 0 ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  if (g_has) set_hour((g_cur + 23) % 24); break;
        case VK_RIGHT: if (g_has) set_hour((g_cur + 1) % 24); break;
        case VK_HOME:  set_hour(0); break;
        case VK_END:   set_hour(23); break;
        case VK_UP:    scroll_to(g_scroll - g_lineH); break;
        case VK_DOWN:  scroll_to(g_scroll + g_lineH); break;
        case VK_PRIOR: scroll_to(g_scroll - text_view_h()); break;
        case VK_NEXT:  scroll_to(g_scroll + text_view_h()); break;
        case 'N': case VK_F5: do_new_day(); break;
        case VK_TAB:   focus_step(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); break;
        case VK_SPACE: case VK_RETURN:
            if (g_focusBtn >= 0 && btn_enabled(g_focusBtn)) press_button(g_focusBtn);
            break;
        case 'C': case VK_INSERT: if (GetKeyState(VK_CONTROL) < 0) copy_text(hwnd); break;
        default: break;                                       /* Esc and everything else: nothing */
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void cleanup(void)
{
    int i;
    fonts_free();
    if (g_bmp) { SelectObject(g_dc, g_bmpOld); DeleteObject(g_bmp); g_bmp = NULL; g_px = NULL; }
    if (g_dc) { DeleteDC(g_dc); g_dc = NULL; }
    for (i = 1; i <= 12; i++) { xfree(g_img[i].px); g_img[i].px = NULL; }
    xfree(g_scenePx); g_scenePx = NULL;
    xfree(g_barPx); g_barPx = NULL;
    if (g_gdipToken) { GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
}

/* ------------------------------------------------------------- startup */

static BOOL parse_u32(const WCHAR *s, unsigned *out, const WCHAR **end)
{
    unsigned long long v = 0; int n = 0;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (unsigned)(*s - L'0'); if (v > 0xFFFFFFFFull) return FALSE; s++; n++; }
    if (!n) return FALSE;
    *out = (unsigned)v;
    if (end) *end = s;
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc; MSG msg; HWND hwnd; LPWSTR *argv; int argc = 0, i;
    BOOL optNewDay = FALSE, haveSeed = FALSE; unsigned seed = 0, v;
    int optHour = -1, optW = 0, optH = 0; const WCHAR *optDump = NULL, *end;
    (void)prev; (void)cmdline;

    g_inst = inst;
    g_heap = GetProcessHeap();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!lstrcmpW(a, L"--newday")) optNewDay = TRUE;
        else if (!lstrcmpW(a, L"--seed") && n && parse_u32(n, &v, NULL)) { seed = v; haveSeed = TRUE; i++; }
        else if (!lstrcmpW(a, L"--hour") && n && parse_u32(n, &v, NULL)) { optHour = v > 23 ? 23 : (int)v; i++; }
        else if (!lstrcmpW(a, L"--dump") && n) { optDump = n; i++; }
        else if (!lstrcmpW(a, L"--size") && n && parse_u32(n, &v, &end) && (*end == L'x' || *end == L'X')) {
            unsigned h2;
            if (parse_u32(end + 1, &h2, NULL)) { optW = (int)(v > 16000 ? 16000 : v); optH = (int)(h2 > 16000 ? 16000 : h2); }
            i++;
        }
    }
    if (haveSeed) rng_seed(seed); else rng_seed_os();

    if (optDump) {                                             /* headless test hook */
        BOOL ok;
        new_day();
        ok = dump_schedule(optDump);
        LocalFree(argv);
        return ok ? 0 : 1;
    }

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
    wc.lpszClassName = L"IcewindDaleWeatherHelper";
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Icewind Dale Weather Helper", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 800, 640, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    if (optW > 0 && optH > 0) set_client_size(hwnd, optW, optH);
    else                      set_client_size(hwnd, dp(780), chrome_height(dp(780)) + dp(210));

    if (optNewDay) {
        new_day();
        if (optHour >= 0) g_cur = optHour;
        g_barValid = FALSE;
        refresh_text(TRUE);
        mark(D_ALL);
    }
    if (argv) LocalFree(argv);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    cleanup();
    return (int)msg.wParam;
}
