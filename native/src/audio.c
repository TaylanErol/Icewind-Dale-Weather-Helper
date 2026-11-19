/* audio.c - wind ambience on winmm waveOut (port of js/audio.js; see audio.h).
 *
 * Layout:
 *   1. Synthesis core (portable C11, no Win32): a looping brown-noise table, a band-pass
 *      biquad, smoothed gain/master, gusts and thunder voices, rendered block by block into
 *      a float buffer. Compiled on its own with -DAUDIO_SYNTH_ONLY for the host test
 *      (native/test/audio_test.c includes this file), so the test exercises exactly the
 *      code the program plays.
 *   2. Device layer (Win32): one worker thread, 4 x 2048-sample 16-bit mono buffers at
 *      44.1 kHz on waveOut with CALLBACK_EVENT. The thread and the device exist only while
 *      the sound is on (and during the fade-out after it is turned off).
 *
 * Deviations from audio.js (inaudible or fixes):
 *   - The noise loop plays len - 2048 samples, so the cross-faded head joins the tail
 *     seamlessly (the web loops the full buffer, leaving a small step at the seam).
 *   - Gusts and parameter targets advance once per 46 ms block (the web: a few times per
 *     second from its frame loop); gain is smoothed per sample, filter per 32 samples.
 *   - A fixed +12 dB output gain: the web graph peaks around -37 dBFS RMS at 44.1 kHz,
 *     too quiet next to other programs on a laptop; levels relative to each other are kept.
 *     A tanh soft knee above -6 dBFS keeps thunder peaks from hard-clipping.
 *   - When the master gain has faded below -46 dB after "off", the last block ramps
 *     linearly to exact zero and the device closes (setTargetAtTime never reaches 0).
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
#define SY_BLOCK     2048                      /* samples per device buffer (~46 ms) */
#define SY_NOISE_LEN (SY_RATE * 4)             /* 4 s noise table, as audio.js        */
#define SY_FADE      2048                      /* loop-seam cross-fade                */
#define SY_LOOP      (SY_NOISE_LEN - SY_FADE)  /* played loop length                  */
#define SY_SUB       32                        /* filter coefficient update interval  */
#define SY_VOICES    4                         /* concurrent thunder claps            */
#define SY_MASTER_ON 0.9
#define SY_OUT_GAIN  4.0                       /* +12 dB makeup (see below)           */
#define SY_SILENT    0.005                     /* master below this after off -> stop */

typedef struct {
    double b0, b1, b2, a1, a2;   /* normalised by a0 */
    double x1, x2, y1, y2;       /* direct form I state */
} SyBiquad;

typedef struct {
    int    active;
    double delay;     /* seconds until t0 */
    double t;         /* seconds since t0 */
    double pos;       /* read position in the noise table (half speed) */
    double strength;
    SyBiquad lp;
} SyVoice;

typedef struct {
    float   *noise;          /* SY_NOISE_LEN samples */
    uint32_t rng;
    long     pos;            /* integer read position, wraps at SY_LOOP */
    SyBiquad bp;
    double   gain, freq, q;  /* smoothed AudioParams */
    double   master, master_target;
    double   gust_phase;
    double   wind;           /* current wind target 0..1 */
    double   k_gain, k_freq, k_q, k_master;  /* per-step smoothing coefficients */
    SyVoice  voice[SY_VOICES];
} Synth;

static uint32_t sy_rand(Synth *s)
{
    uint32_t x = s->rng;            /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s->rng = x;
}

static double sy_uniform(Synth *s) { return (sy_rand(s) >> 8) * (1.0 / 16777216.0); }

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

/* RBJ low-pass; WebAudio's lowpass Q is in dB (default 1 dB). */
static void sy_lowpass(SyBiquad *f, double hz, double q_db, double rate)
{
    double w0 = 2.0 * M_PI * hz / rate;
    double q = pow(10.0, q_db / 20.0);
    double alpha = sin(w0) / (2.0 * q);
    double c = cos(w0);
    double a0 = 1.0 + alpha;
    f->b0 = (1.0 - c) / 2.0 / a0;
    f->b1 = (1.0 - c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0 * c / a0;
    f->a2 = (1.0 - alpha) / a0;
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

/* Allocates the noise table; returns 0 on out-of-memory. */
static int synth_init(Synth *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->noise = (float *)malloc(sizeof(float) * SY_NOISE_LEN);
    if (!s->noise) return 0;
    s->rng = seed ? seed : 0x9E3779B9u;
    double last = 0.0;
    for (long i = 0; i < SY_NOISE_LEN; i++) {
        double w = sy_uniform(s) * 2.0 - 1.0;
        last = (last + 0.02 * w) / 1.02;
        s->noise[i] = (float)(last * 3.5);
    }
    for (long i = 0; i < SY_FADE; i++) {
        double k = (double)i / SY_FADE;
        s->noise[i] = (float)(s->noise[i] * k + s->noise[SY_NOISE_LEN - SY_FADE + i] * (1.0 - k));
    }
    s->freq = 400.0;                 /* audio.js initial filter values */
    s->q = 0.8;
    sy_bandpass(&s->bp, s->freq, s->q);
    s->k_gain = sy_k(1.0 / SY_RATE, 0.5);
    s->k_freq = sy_k((double)SY_SUB / SY_RATE, 0.6);
    s->k_q    = sy_k((double)SY_SUB / SY_RATE, 0.8);
    s->k_master = sy_k(1.0 / SY_RATE, 0.4);
    return 1;
}

static void synth_free(Synth *s)
{
    free(s->noise);
    s->noise = NULL;
}

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
        v->pos = sy_uniform(s) * 2.0 * SY_RATE;   /* src.start(t0, random * 2 s) */
        /* the source plays at half speed, so the 140 Hz cut is relative to the output rate */
        sy_lowpass(&v->lp, 140.0, 1.0, SY_RATE);
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

/* Renders n (<= SY_BLOCK) samples into out[]. Returns 1 while sound is still audible or
 * wanted, 0 once the fade-out after "off" has completed (the block then ends in silence). */
static int synth_render(Synth *s, float *out, int n)
{
    const double dt = (double)n / SY_RATE;
    double wind = s->wind;

    /* WindAudio.update(wind, dt) */
    s->gust_phase += dt * (0.25 + wind * 0.6);
    if (s->gust_phase > 2000.0 * M_PI) s->gust_phase -= 2000.0 * M_PI; /* keep precision */
    double gust = 0.65 + 0.35 * sin(s->gust_phase) * sin(s->gust_phase * 0.37 + 1.3);
    double gain_t = (0.03 + wind * 0.42) * gust;
    double freq_t = 220.0 + wind * 800.0 + gust * 260.0;
    double q_t = 0.6 + wind * 1.4;

    int ending = s->master_target == 0.0 && s->master < SY_SILENT;
    double ramp0 = s->master;

    for (int i = 0; i < n; i++) {
        if ((i % SY_SUB) == 0) {
            s->freq += (freq_t - s->freq) * s->k_freq;
            s->q += (q_t - s->q) * s->k_q;
            sy_bandpass(&s->bp, s->freq, s->q);
        }
        s->gain += (gain_t - s->gain) * s->k_gain;
        if (ending)
            s->master = ramp0 * (double)(n - 1 - i) / (double)(n > 1 ? n - 1 : 1);
        else
            s->master += (s->master_target - s->master) * s->k_master;

        double x = s->noise[s->pos];
        if (++s->pos >= SY_LOOP) s->pos = 0;
        double y = sy_biquad(&s->bp, x) * s->gain;

        for (int k = 0; k < SY_VOICES; k++) {
            SyVoice *v = &s->voice[k];
            if (!v->active) continue;
            if (v->delay > 0.0) { v->delay -= 1.0 / SY_RATE; continue; }
            long i0 = (long)v->pos;
            double fr = v->pos - (double)i0;
            long i1 = i0 + 1 >= SY_LOOP ? 0 : i0 + 1;
            double src = s->noise[i0] * (1.0 - fr) + s->noise[i1] * fr;
            v->pos += 0.5;
            if (v->pos >= SY_LOOP) v->pos -= SY_LOOP;
            y += sy_biquad(&v->lp, src) * sy_thunder_env(v->t, v->strength);
            v->t += 1.0 / SY_RATE;
            if (v->t >= 3.4) v->active = 0;   /* src.stop(t0 + 3.4) */
        }

        y *= s->master * SY_OUT_GAIN;
        /* soft knee above -6 dBFS (only thunder peaks get there): never hard-clips */
        if (y > 0.5) y = 0.5 + 0.5 * tanh((y - 0.5) * 2.0);
        else if (y < -0.5) y = -0.5 - 0.5 * tanh((-y - 0.5) * 2.0);
        out[i] = (float)(y * 0.98);
    }
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

#define AU_NBUF 4

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
static volatile LONG g_thunder_u;    /* pending thunder strength * 1e6, 0 = none */
static HANDLE g_event;               /* waveOut CALLBACK_EVENT, owned with g_thread */

static DWORD WINAPI au_thread(LPVOID arg)
{
    HWAVEOUT wo = (HWAVEOUT)arg;
    HANDLE ev = g_event;
    Synth *s = (Synth *)malloc(sizeof(Synth));
    WAVEHDR hdr[AU_NBUF];
    int16_t *pcm = NULL;
    float fbuf[SY_BLOCK];
    int ok = s && synth_init(s, (uint32_t)GetTickCount() ^ (uint32_t)(uintptr_t)s);
    if (ok) {
        pcm = (int16_t *)malloc(sizeof(int16_t) * SY_BLOCK * AU_NBUF);
        ok = pcm != NULL;
    }
    memset(hdr, 0, sizeof hdr);
    if (ok) {
        s->wind = g_wind_u / 1e6;
        s->master_target = SY_MASTER_ON;
        for (int i = 0; i < AU_NBUF; i++) {
            hdr[i].lpData = (LPSTR)(pcm + i * SY_BLOCK);
            hdr[i].dwBufferLength = SY_BLOCK * sizeof(int16_t);
            waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            hdr[i].dwFlags |= WHDR_DONE;   /* mark free for the first fill */
        }
    }

    int alive = ok;
    while (alive && !g_quit) {
        int queued = 0;
        for (int i = 0; i < AU_NBUF && alive; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) { queued++; continue; }
            s->wind = g_wind_u / 1e6;
            s->master_target = g_enabled ? SY_MASTER_ON : 0.0;
            LONG th = InterlockedExchange(&g_thunder_u, 0);
            if (th > 0 && g_enabled) synth_thunder(s, th / 1e6);
            int more = synth_render(s, fbuf, SY_BLOCK);
            int16_t *p = (int16_t *)hdr[i].lpData;
            for (int k = 0; k < SY_BLOCK; k++) p[k] = (int16_t)lrintf(fbuf[k] * 32767.0f);
            hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
            if (waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                hdr[i].dwFlags |= WHDR_DONE;
                alive = 0;
                break;
            }
            queued++;
            if (!more) {
                /* faded out: exit unless the sound was switched back on meanwhile */
                EnterCriticalSection(&g_lock);
                if (!g_enabled) { InterlockedExchange(&g_running, 0); alive = 0; }
                LeaveCriticalSection(&g_lock);
            }
        }
        if (alive) WaitForSingleObject(ev, queued ? 200 : 20);
    }

    /* let the tail (already silent or faded) play out unless quitting, then close */
    if (ok && !g_quit) {
        for (int tries = 0; tries < 25; tries++) {
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
    free(pcm);
    if (s) synth_free(s);
    free(s);

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
        WaitForSingleObject(th, INFINITE);   /* bounded: <= one 200 ms wait + reset */
        CloseHandle(th);
    }
    if (ev) CloseHandle(ev);
    InterlockedExchange(&g_running, 0);
}
#endif /* AUDIO_SYNTH_ONLY */
