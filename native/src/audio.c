/* audio.c - wind ambience on winmm waveOut (port of js/audio.js; see audio.h).
 *
 * Layout:
 *   1. Synthesis core (portable C11, no Win32): live brown noise, a band-pass biquad,
 *      smoothed gain/master, gusts, thunder voices and the "interior" muffling, rendered
 *      block by block into a float buffer. Compiled on its own with -DAUDIO_SYNTH_ONLY for
 *      the host test (native/test/audio_test.c includes this file), so the test exercises
 *      exactly the code the program plays.
 *   2. Device layer (Win32): one worker thread woken by the waveOut CALLBACK_EVENT, 8 x 1536
 *      sample 16-bit mono buffers at 44.1 kHz (279 ms queued), refilled strictly in
 *      submission order. The thread runs at THREAD_PRIORITY_HIGHEST, joins the MMCSS "Audio"
 *      class when avrt.dll is present and opts out of Windows' execution-speed power
 *      throttling (both loaded at run time: no extra import). It never waits on the UI
 *      thread. The thread and the device exist only while the sound is on (and during the
 *      fade-out after it is turned off).
 *
 * Deviations from audio.js (inaudible or fixes):
 *   - The noise is generated live with the same recursion instead of looping a 4 s table:
 *     no 4 s repetition, and no -3 dB dip every 4 s where the web's linear loop cross-fade
 *     mixes two uncorrelated stretches.
 *   - Gentler gusts that never sink the band into the speaker's dead zone: the gust depth
 *     grows with the wind at a constant mean (0.63..0.79 in calm air, 0.42..1 at full wind;
 *     web 0.3..1 at every wind), the band starts at 300 Hz (web 220 Hz) and moves less with
 *     the gust (230 Hz, web 260 Hz), and the calm floor gain is 0.065 (web 0.03), so "No
 *     Wind" is a quiet steady bed instead of dropping out on small speakers.
 *   - Gusts and parameter targets advance once per 35 ms block (the web: a few times per
 *     second from its frame loop); gain is smoothed per sample, filter per 32 samples.
 *   - A fixed +12 dB output gain: the web graph peaks around -37 dBFS RMS at 44.1 kHz,
 *     too quiet next to other programs on a laptop; levels relative to each other are kept.
 *     A tanh soft knee above -6 dBFS keeps thunder peaks from hard-clipping.
 *   - When the master gain has faded below -46 dB after "off", the last block ramps
 *     linearly to exact zero and the device closes (setTargetAtTime never reaches 0).
 *   - Interior (native only): the wind and thunder pass one overdamped low-pass (700 Hz,
 *     Q 0.32: about -6 dB/oct from 250 Hz and -12 dB/oct above 2 kHz, like a wall) at
 *     -1.9 dB (5..9 dB quieter in all, the howl still faintly audible), with 40 % less gust
 *     depth and a faint low rumble, cross-faded in and out with a 0.18 s time constant
 *     (settled within ~0.8 s). The path is skipped while settled outside.
 */
#include "audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- synthesis core */

#define SY_RATE      44100
#define SY_BLOCK     2048                      /* largest render call (device: 1536)  */
#define SY_SUB       32                        /* filter coefficient update interval  */
#define SY_VOICES    4                         /* concurrent thunder claps            */
#define SY_MASTER_ON 0.9
#define SY_OUT_GAIN  4.0                       /* +12 dB makeup (see above)           */
#define SY_SILENT    0.005                     /* master below this after off -> stop */
#define SY_IN_LP     700.0                     /* interior low-pass corner, Hz        */
#define SY_IN_Q      0.32                      /* overdamped: poles near 250 Hz, 2 kHz */
#define SY_IN_GAIN   0.80                      /* interior level (-1.9 dB)            */
#define SY_IN_GUST   0.4                       /* interior: share of gust depth removed */
#define SY_IN_TAU    0.18                      /* interior cross-fade time constant, s */
#define SY_DECLICK   220                       /* fade-in after a device underrun (5 ms) */

typedef struct {
    double b0, b1, b2, a1, a2;   /* normalised by a0 */
    double x1, x2, y1, y2;       /* direct form I state */
} SyBiquad;

typedef struct {
    int    active;
    double delay;     /* seconds until t0 */
    double t;         /* seconds since t0 */
    double nz;        /* own brown-noise state (the web plays the table at half speed) */
    double strength;
    SyBiquad lp;
} SyVoice;

typedef struct {
    uint32_t rng;
    double   nz;             /* brown-noise state of the wind source */
    double   rz;             /* brown-noise state of the interior rumble */
    SyBiquad bp;
    double   gain, freq, q;  /* smoothed AudioParams */
    double   master, master_target;
    double   gust_phase;
    double   wind;           /* current wind target 0..1 */
    double   interior;       /* interior target 0 or 1 */
    double   imix;           /* smoothed interior mix 0..1 */
    double   rum;            /* smoothed interior rumble level (relative to gain) */
    SyBiquad in1;            /* interior low-pass, one overdamped section (wall) */
    SyBiquad rlp;            /* rumble low-pass */
    int      fadein;         /* samples left of a de-click fade-in (device underrun) */
    int      in_active;      /* interior path ran in the previous block */
    double   k_gain, k_freq, k_q, k_master, k_imix;  /* per-step smoothing coefficients */
    SyVoice  voice[SY_VOICES];
} Synth;

static uint32_t sy_rand(Synth *s)
{
    uint32_t x = s->rng;            /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s->rng = x;
}

static double sy_uniform(Synth *s) { return (sy_rand(s) >> 8) * (1.0 / 16777216.0); }

/* audio.js brown noise: last = (last + 0.02 * white) / 1.02; sample = last * 3.5 */
static double sy_brown(Synth *s, double *state)
{
    double w = sy_uniform(s) * 2.0 - 1.0;
    *state = (*state + 0.02 * w) / 1.02;
    return *state * 3.5;
}

/* The same noise played at half speed (thunder): pole sqrt(1/1.02) per output sample,
 * input scaled so the RMS matches the full-speed noise. */
#define SY_HALF_A 0.99014754297667      /* sqrt(1 / 1.02) */
#define SY_HALF_B 0.0139333076031824    /* b * sqrt((1 - a_half^2) / (1 - a^2)) */
static double sy_brown_half(Synth *s, double *state)
{
    double w = sy_uniform(s) * 2.0 - 1.0;
    *state = *state * SY_HALF_A + SY_HALF_B * w;
    return *state * 3.5;
}

/* RBJ band-pass with constant 0 dB peak gain (WebAudio "bandpass"), Q linear. */
static void sy_bandpass(SyBiquad *f, double hz, double q)
{
    double w0 = 2.0 * M_PI * hz / SY_RATE;
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0;
    f->b2 = -alpha / a0;
    f->a1 = -2.0 * cos(w0) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

/* RBJ low-pass, Q linear. */
static void sy_lowpass_q(SyBiquad *f, double hz, double q)
{
    double w0 = 2.0 * M_PI * hz / SY_RATE;
    double alpha = sin(w0) / (2.0 * q);
    double c = cos(w0);
    double a0 = 1.0 + alpha;
    f->b0 = (1.0 - c) / 2.0 / a0;
    f->b1 = (1.0 - c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0 * c / a0;
    f->a2 = (1.0 - alpha) / a0;
}

/* WebAudio's lowpass Q is in dB (default 1 dB). */
static void sy_lowpass(SyBiquad *f, double hz, double q_db)
{
    sy_lowpass_q(f, hz, pow(10.0, q_db / 20.0));
}

static double sy_biquad(SyBiquad *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

/* setTargetAtTime step coefficient for an interval dt and time constant tau. */
static double sy_k(double dt, double tau) { return 1.0 - exp(-dt / tau); }

/* Always succeeds (no allocation); returns 1 for the callers that test it. */
static int synth_init(Synth *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = seed ? seed : 0x9E3779B9u;
    /* start the noise in its stationary state (no slow drift in from 0) */
    for (int i = 0; i < 4096; i++) sy_brown(s, &s->nz);
    for (int i = 0; i < 4096; i++) sy_brown(s, &s->rz);
    s->freq = 400.0;                 /* audio.js initial filter values */
    s->q = 0.8;
    sy_bandpass(&s->bp, s->freq, s->q);
    sy_lowpass_q(&s->in1, SY_IN_LP, SY_IN_Q);
    sy_lowpass_q(&s->rlp, 90.0, 0.7071);
    s->k_gain = sy_k(1.0 / SY_RATE, 0.5);
    s->k_freq = sy_k((double)SY_SUB / SY_RATE, 0.6);
    s->k_q    = sy_k((double)SY_SUB / SY_RATE, 0.8);
    s->k_master = sy_k(1.0 / SY_RATE, 0.4);
    s->k_imix = sy_k(1.0 / SY_RATE, SY_IN_TAU);
    return 1;
}

static void synth_free(Synth *s) { (void)s; }

static void synth_thunder(Synth *s, double strength)
{
    if (strength < 0.5) strength = 0.5;
    if (strength > 1.0) strength = 1.0;
    for (int i = 0; i < SY_VOICES; i++) {
        SyVoice *v = &s->voice[i];
        if (v->active) continue;
        memset(v, 0, sizeof *v);
        v->active = 1;
        v->strength = strength;
        v->delay = 0.3 + (1.0 - strength) * 1.4;
        for (int k = 0; k < 512; k++) sy_brown_half(s, &v->nz);   /* settle the source */
        /* the source plays at half speed, so the 140 Hz cut is relative to the output rate */
        sy_lowpass(&v->lp, 140.0, 1.0);
        return;
    }
}

/* Thunder gain envelope at t seconds after t0 (audio.js automation). */
static double sy_thunder_env(double t, double s)
{
    if (t < 0.0) return 0.0;
    if (t < 0.08) return 1.4 * s * (t / 0.08);
    if (t < 0.7) return 1.4 * s * pow(0.4 / 1.4, (t - 0.08) / 0.62);
    if (t < 3.2) return 0.4 * s * pow(0.001 / (0.4 * s), (t - 0.7) / 2.5);
    return 0.001;
}

/* Gust envelope: audio.js's shape (0.65 + 0.35 * sin p * sin(0.37 p + 1.3)), lifted to a
 * constant mean of 0.71 with a depth that grows with the wind: 0.63..0.79 in calm air,
 * 0.42..1 at full wind. */
static double sy_gust(double p, double wind)
{ return 0.71 + (0.08 + 0.21 * wind) * sin(p) * sin(p * 0.37 + 1.3); }

/* Renders n (<= SY_BLOCK) samples into out[]. Returns 1 while sound is still audible or
 * wanted, 0 once the fade-out after "off" has completed (the block then ends in silence). */
static int synth_render(Synth *s, float *out, int n)
{
    const double dt = (double)n / SY_RATE;
    double wind = s->wind;

    /* WindAudio.update(wind, dt) */
    s->gust_phase += dt * (0.25 + wind * 0.6);
    if (s->gust_phase > 2000.0 * M_PI) s->gust_phase -= 2000.0 * M_PI; /* keep precision */
    double gust = sy_gust(s->gust_phase, wind);
    /* interior: shallower gusts (the walls even them out) */
    gust = 1.0 - (1.0 - gust) * (1.0 - SY_IN_GUST * s->imix);
    double gain_t = (0.065 + wind * 0.40) * gust;
    double freq_t = 300.0 + wind * 740.0 + gust * 230.0;
    double q_t = 0.6 + wind * 1.4;
    double rum_t = 0.03 + wind * 0.09;    /* interior rumble level (before imix) */

    int ending = s->master_target == 0.0 && s->master < SY_SILENT;
    double ramp0 = s->master;
    int inside = !(s->interior == 0.0 && s->imix == 0.0);
    if (inside && !s->in_active) {
        /* switched on from settled outside: start the wet path from rest (its output is
         * still mixed in at ~0 while imix rises, so a cold filter is inaudible) */
        s->in1.x1 = s->in1.x2 = s->in1.y1 = s->in1.y2 = 0.0;
        s->rlp.x1 = s->rlp.x2 = s->rlp.y1 = s->rlp.y2 = 0.0;
    }
    s->in_active = inside;

    for (int i = 0; i < n; i++) {
        if ((i % SY_SUB) == 0) {
            s->freq += (freq_t - s->freq) * s->k_freq;
            s->q += (q_t - s->q) * s->k_q;
            sy_bandpass(&s->bp, s->freq, s->q);
        }
        s->gain += (gain_t - s->gain) * s->k_gain;
        s->imix += (s->interior - s->imix) * s->k_imix;
        s->rum += (rum_t - s->rum) * s->k_gain;
        if (ending)
            s->master = ramp0 * (double)(n - 1 - i) / (double)(n > 1 ? n - 1 : 1);
        else
            s->master += (s->master_target - s->master) * s->k_master;

        double y = sy_biquad(&s->bp, sy_brown(s, &s->nz)) * s->gain;

        for (int k = 0; k < SY_VOICES; k++) {
            SyVoice *v = &s->voice[k];
            if (!v->active) continue;
            if (v->delay > 0.0) { v->delay -= 1.0 / SY_RATE; continue; }
            double src = sy_brown_half(s, &v->nz);
            y += sy_biquad(&v->lp, src) * sy_thunder_env(v->t, v->strength);
            v->t += 1.0 / SY_RATE;
            if (v->t >= 3.4) v->active = 0;   /* src.stop(t0 + 3.4) */
        }

        /* interior: skipped while settled outside; filter states restart from zero
         * when it is switched on (see below), then the mix cross-fades in */
        if (inside) {
            double wet = sy_biquad(&s->in1, y) * SY_IN_GAIN
                       + sy_biquad(&s->rlp, sy_brown(s, &s->rz)) * s->rum * s->gain;
            y += (wet - y) * s->imix;
        }

        y *= s->master * SY_OUT_GAIN;
        if (s->fadein > 0) {
            y *= (double)(SY_DECLICK - s->fadein) / SY_DECLICK;
            s->fadein--;
        }
        /* soft knee above -6 dBFS (only thunder peaks get there): never hard-clips */
        if (y > 0.5) y = 0.5 + 0.5 * tanh((y - 0.5) * 2.0);
        else if (y < -0.5) y = -0.5 - 0.5 * tanh((-y - 0.5) * 2.0);
        out[i] = (float)(y * 0.98);
    }
    /* settled: snap, so the mix never creeps into denormals (slow on x86) */
    if (fabs(s->imix - s->interior) < 1e-6) s->imix = s->interior;
    if (ending) {
        s->master = 0.0;
        for (int k = 0; k < SY_VOICES; k++) s->voice[k].active = 0;
        return 0;
    }
    return 1;
}

#ifndef AUDIO_SYNTH_ONLY
/* ---------------------------------------------------------------- device layer */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#define AU_NBUF 8                  /* 8 buffers ...                                     */
#define AU_LEN  1536               /* ... of 34.8 ms = 279 ms queued, 244 ms of slack   */

static double sy_clamp01(double v)
{
    if (!(v > 0.0)) return 0.0;   /* also catches NaN */
    return v > 1.0 ? 1.0 : v;
}

static CRITICAL_SECTION g_lock;
static volatile LONG g_inited;
static HANDLE g_thread;              /* worker; non-NULL until joined */
static volatile LONG g_running;      /* worker alive and not yet committed to exit */
static volatile LONG g_enabled;      /* user wants sound */
static volatile LONG g_quit;         /* shutdown: stop immediately */
static volatile LONG g_wind_u;       /* wind * 1e6 */
static volatile LONG g_interior;     /* muffled as heard indoors */
static volatile LONG g_thunder_u;    /* pending thunder strength * 1e6, 0 = none */
static volatile LONG g_underruns;    /* queue ran dry (diagnostics) */
static HANDLE g_event;               /* waveOut CALLBACK_EVENT, owned with g_thread */

/* Optional scheduling helpers, resolved at run time so the exe imports nothing new. */
typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *AvRevertFn)(HANDLE);
typedef struct { ULONG Version, ControlMask, StateMask; } AuThrottle;   /* THREAD_POWER_THROTTLING_STATE */
typedef BOOL (WINAPI *SetThreadInfoFn)(HANDLE, int, LPVOID, DWORD);

static HANDLE au_boost(HMODULE *avrt)
{
    HANDLE task = NULL;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    /* no EcoQoS / execution-speed throttling while the window is in the background */
    SetThreadInfoFn sti = (SetThreadInfoFn)(void (*)(void))
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
    if (sti) {
        AuThrottle t = { 1, 1, 0 };   /* THREAD_POWER_THROTTLING_EXECUTION_SPEED, off */
        sti(GetCurrentThread(), 3 /* ThreadPowerThrottling */, &t, sizeof t);
    }
    *avrt = LoadLibraryW(L"avrt.dll");
    if (*avrt) {
        AvSetFn set = (AvSetFn)(void (*)(void))GetProcAddress(*avrt, "AvSetMmThreadCharacteristicsW");
        DWORD idx = 0;
        if (set) task = set(L"Audio", &idx);
    }
    return task;
}

static void au_unboost(HMODULE avrt, HANDLE task)
{
    if (avrt) {
        AvRevertFn rev = (AvRevertFn)(void (*)(void))GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        if (task && rev) rev(task);
        FreeLibrary(avrt);
    }
}

static DWORD WINAPI au_thread(LPVOID arg)
{
    HWAVEOUT wo = (HWAVEOUT)arg;
    HANDLE ev = g_event;
    HMODULE avrt = NULL;
    HANDLE task = au_boost(&avrt);
    Synth *s = (Synth *)malloc(sizeof(Synth));
    WAVEHDR hdr[AU_NBUF];
    int16_t *pcm = NULL;
    float *fbuf = NULL;
    int ok = s && synth_init(s, (uint32_t)GetTickCount() ^ (uint32_t)(uintptr_t)s);
    if (ok) {
        pcm = (int16_t *)malloc(sizeof(int16_t) * AU_LEN * AU_NBUF);
        fbuf = (float *)malloc(sizeof(float) * AU_LEN);
        ok = pcm && fbuf;
    }
    memset(hdr, 0, sizeof hdr);
    if (ok) {
        s->wind = g_wind_u / 1e6;
        s->interior = s->imix = g_interior ? 1.0 : 0.0;
        s->master_target = SY_MASTER_ON;
        for (int i = 0; i < AU_NBUF; i++) {
            hdr[i].lpData = (LPSTR)(pcm + i * AU_LEN);
            hdr[i].dwBufferLength = AU_LEN * sizeof(int16_t);
            waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            hdr[i].dwFlags |= WHDR_DONE;   /* mark free for the first fill */
        }
    }

    /* Buffers are refilled strictly in submission order (next), so the queue always plays
     * the synthesis in sequence; the event wakes the thread whenever one finishes. */
    int alive = ok, next = 0, started = 0;
    while (alive && !g_quit) {
        int free_n = 0;
        for (int i = 0; i < AU_NBUF; i++) if (hdr[i].dwFlags & WHDR_DONE) free_n++;
        if (started && free_n == AU_NBUF) {
            /* the queue ran dry (the thread was starved): the device restarts from silence,
             * so fade the next block in instead of starting it with a click */
            InterlockedIncrement(&g_underruns);
            s->fadein = SY_DECLICK;
        }
        while (alive && (hdr[next].dwFlags & WHDR_DONE)) {
            WAVEHDR *h = &hdr[next];
            s->wind = g_wind_u / 1e6;
            s->interior = g_interior ? 1.0 : 0.0;
            s->master_target = g_enabled ? SY_MASTER_ON : 0.0;
            LONG th = InterlockedExchange(&g_thunder_u, 0);
            if (th > 0 && g_enabled) synth_thunder(s, th / 1e6);
            int more = synth_render(s, fbuf, AU_LEN);
            int16_t *p = (int16_t *)h->lpData;
            for (int k = 0; k < AU_LEN; k++) p[k] = (int16_t)lrintf(fbuf[k] * 32767.0f);
            h->dwFlags &= ~(DWORD)WHDR_DONE;
            if (waveOutWrite(wo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                h->dwFlags |= WHDR_DONE;
                alive = 0;
                break;
            }
            started = 1;
            next = (next + 1) % AU_NBUF;
            if (!more) {
                /* faded out: exit unless the sound was switched back on meanwhile */
                EnterCriticalSection(&g_lock);
                if (!g_enabled) { InterlockedExchange(&g_running, 0); alive = 0; }
                LeaveCriticalSection(&g_lock);
            }
        }
        /* the timeout only guards against a lost event; normally a buffer end wakes us */
        if (alive) WaitForSingleObject(ev, 100);
    }

    /* let the tail (already silent or faded) play out unless quitting, then close */
    if (ok && !g_quit) {
        for (int tries = 0; tries < 40; tries++) {
            int busy = 0;
            for (int i = 0; i < AU_NBUF; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) busy = 1;
            if (!busy) break;
            WaitForSingleObject(ev, 20);
        }
    }
    waveOutReset(wo);
    if (ok)
        for (int i = 0; i < AU_NBUF; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(wo);
    free(fbuf);
    free(pcm);
    if (s) synth_free(s);
    free(s);
    au_unboost(avrt, task);

    /* device error / out of memory: report "off" (no lock here: a joiner may hold it) */
    if (g_running) {
        InterlockedExchange(&g_enabled, 0);
        InterlockedExchange(&g_running, 0);
    }
    return 0;
}

void audio_init(void)
{
    if (InterlockedCompareExchange(&g_inited, 1, 0) != 0) return;
    InitializeCriticalSection(&g_lock);
    g_wind_u = 0;
}

/* Joins a worker that has committed to exit (lock held; the worker no longer takes the
 * lock once g_running is 0). */
static void au_join_locked(void)
{
    if (g_thread && !g_running) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
        if (g_event) { CloseHandle(g_event); g_event = NULL; }
    }
}

bool audio_set_enabled(bool on)
{
    audio_init();
    bool result = true;
    EnterCriticalSection(&g_lock);
    if (!on) {
        InterlockedExchange(&g_enabled, 0);   /* worker fades out and exits by itself */
    } else if (g_running) {
        InterlockedExchange(&g_enabled, 1);   /* cancel a fade-out in progress */
    } else {
        au_join_locked();
        g_quit = 0;
        g_thunder_u = 0;
        HWAVEOUT wo = NULL;
        g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        WAVEFORMATEX fmt;
        memset(&fmt, 0, sizeof fmt);
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = SY_RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 2;
        fmt.nAvgBytesPerSec = SY_RATE * 2;
        if (!g_event ||
            waveOutOpen(&wo, WAVE_MAPPER, &fmt, (DWORD_PTR)g_event, 0, CALLBACK_EVENT)
                != MMSYSERR_NOERROR) {
            if (g_event) { CloseHandle(g_event); g_event = NULL; }
            result = false;
        } else {
            InterlockedExchange(&g_enabled, 1);
            InterlockedExchange(&g_running, 1);
            g_thread = CreateThread(NULL, 64 * 1024, au_thread, (LPVOID)wo, 0, NULL);
            if (!g_thread) {
                InterlockedExchange(&g_running, 0);
                InterlockedExchange(&g_enabled, 0);
                waveOutClose(wo);
                CloseHandle(g_event);
                g_event = NULL;
                result = false;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

bool audio_device_present(void)
{
    return waveOutGetNumDevs() > 0;
}

bool audio_is_enabled(void)
{
    return g_enabled != 0;
}

void audio_set_wind(double wind)
{
    InterlockedExchange(&g_wind_u, (LONG)lrint(sy_clamp01(wind) * 1e6));
}

void audio_set_interior(bool on)
{
    InterlockedExchange(&g_interior, on ? 1 : 0);
}

bool audio_interior(void)
{
    return g_interior != 0;
}

unsigned audio_underruns(void)
{
    return (unsigned)g_underruns;
}

void audio_thunder(double strength)
{
    if (!g_enabled) return;
    if (!(strength >= 0.5)) strength = 0.5;
    if (strength > 1.0) strength = 1.0;
    InterlockedExchange(&g_thunder_u, (LONG)lrint(strength * 1e6));
}

void audio_shutdown(void)
{
    if (!g_inited) return;
    EnterCriticalSection(&g_lock);
    InterlockedExchange(&g_enabled, 0);
    InterlockedExchange(&g_quit, 1);
    HANDLE th = g_thread, ev = g_event;
    g_thread = NULL;
    g_event = NULL;
    LeaveCriticalSection(&g_lock);
    /* joined outside the lock: the worker may be about to take it to commit its exit */
    if (th) {
        if (ev) SetEvent(ev);
        WaitForSingleObject(th, INFINITE);   /* bounded: <= one 100 ms wait + reset */
        CloseHandle(th);
    }
    if (ev) CloseHandle(ev);
    InterlockedExchange(&g_running, 0);
}
#endif /* AUDIO_SYNTH_ONLY */
