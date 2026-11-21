/* audio_device_test.c - runs the real waveOut layer of src/audio.c (worker thread, event
 * wake-ups, buffer ring, fade-out, shutdown) against a fake waveOut device, in real time,
 * under wine or Windows. Build (see test_audio.sh, AUDIO_DEVICE_TEST=1):
 *   x86_64-w64-mingw32-gcc -std=c11 -O2 -municode audio_device_test.c -o adt.exe -lwinmm
 * (-DAUDIO_SRC='"path/audio.c"' measures another revision.)
 *
 * The fake device behaves like the Windows audio engine: a player thread pulls 10 ms
 * periods at the real-time rate from the queue of written buffers, marks a buffer
 * WHDR_DONE and signals the CALLBACK_EVENT when it is used up, and plays silence (an
 * underrun) when the queue is empty. Everything it plays is captured. While it runs, the
 * main thread behaves like a busy UI (wind changes, interior toggles, 300 ms CPU bursts),
 * and the worker is stalled on purpose inside waveOutWrite to model a thread that the
 * system did not schedule in time.
 *   adt.exe SECONDS STALL_MS EVERY_MS [OUT.wav]   STALL_MS < 0: random 0..-STALL_MS
 * Prints: underrun periods, gaps (runs of >= 64 zero samples while playing), and exits 1
 * when any underrun happened.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>

static MMRESULT fk_open(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT fk_prepare(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT fk_unprepare(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT fk_write(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT fk_reset(HWAVEOUT);
static MMRESULT fk_close(HWAVEOUT);
static UINT fk_numdevs(void);
#define waveOutOpen fk_open
#define waveOutPrepareHeader fk_prepare
#define waveOutUnprepareHeader fk_unprepare
#define waveOutWrite fk_write
#define waveOutReset fk_reset
#define waveOutClose fk_close
#define waveOutGetNumDevs fk_numdevs

#ifdef AUDIO_SRC
#include AUDIO_SRC
#else
#include "../src/audio.c"
#endif

#define PERIOD   441                         /* 10 ms engine period */
#define CAP_MAX  (SY_RATE * 200)

static CRITICAL_SECTION q_lock;
static WAVEHDR *q[64];
static int q_head, q_n;
static DWORD q_off;                          /* bytes of q[q_head] already played */
static HANDLE q_event, q_thread;
static volatile LONG q_stop, q_started, counting = 1;
static int16_t *cap;
static long cap_n;
static long underrun_periods, played_periods;
static volatile LONG stall_ms = 0, stall_every = 1000, stall_rand = 0;
static double next_stall_at;                 /* seconds of playback */

static double now_s(void)
{
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static DWORD WINAPI player(LPVOID arg)
{
    (void)arg;
    double t0 = now_s();
    long periods = 0;
    while (!q_stop) {
        double due = t0 + (periods + 1) * (double)PERIOD / SY_RATE;
        while (now_s() < due && !q_stop) Sleep(1);
        periods++;
        EnterCriticalSection(&q_lock);
        int need = PERIOD, signal = 0;
        while (need > 0 && q_n > 0) {
            WAVEHDR *h = q[q_head];
            int avail = (int)((h->dwBufferLength - q_off) / 2);
            int take = avail < need ? avail : need;
            if (cap_n + take <= CAP_MAX) memcpy(cap + cap_n, h->lpData + q_off, (size_t)take * 2);
            cap_n += take;
            q_off += (DWORD)take * 2;
            need -= take;
            if (q_off >= h->dwBufferLength) {
                h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE;
                q_head = (q_head + 1) % 64;
                q_n--;
                q_off = 0;
                signal = 1;
            }
        }
        if (q_started && counting) {
            played_periods++;
            if (need > 0) {
                underrun_periods++;
                for (int i = 0; i < need && cap_n < CAP_MAX; i++) cap[cap_n++] = 0;
            }
        }
        LeaveCriticalSection(&q_lock);
        if (signal) SetEvent(q_event);
    }
    return 0;
}

static MMRESULT fk_open(LPHWAVEOUT pwo, UINT dev, LPCWAVEFORMATEX fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)dev; (void)inst;
    if (fmt->nSamplesPerSec != SY_RATE || !(flags & CALLBACK_EVENT)) return MMSYSERR_INVALPARAM;
    q_event = (HANDLE)cb;
    q_head = q_n = 0; q_off = 0; q_stop = 0; q_started = 0;
    q_thread = CreateThread(NULL, 0, player, NULL, 0, NULL);
    *pwo = (HWAVEOUT)(uintptr_t)1;
    SetEvent(q_event);                       /* WOM_OPEN */
    return MMSYSERR_NOERROR;
}

static MMRESULT fk_prepare(HWAVEOUT wo, LPWAVEHDR h, UINT n) { (void)wo; (void)n; h->dwFlags |= WHDR_PREPARED; return 0; }
static MMRESULT fk_unprepare(HWAVEOUT wo, LPWAVEHDR h, UINT n) { (void)wo; (void)n; h->dwFlags &= ~(DWORD)WHDR_PREPARED; return 0; }

static MMRESULT fk_write(HWAVEOUT wo, LPWAVEHDR h, UINT n)
{
    (void)wo; (void)n;
    double t = (double)cap_n / SY_RATE;
    if (q_started && stall_ms != 0 && t >= next_stall_at) {
        /* the worker is not scheduled for a while */
        int ms = stall_rand ? (int)(rand() % (-stall_ms + 1)) : stall_ms;
        next_stall_at = t + stall_every / 1000.0;
        Sleep((DWORD)ms);
    }
    EnterCriticalSection(&q_lock);
    h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_DONE) | WHDR_INQUEUE;
    q[(q_head + q_n) % 64] = h;
    q_n++;
    q_started = 1;
    LeaveCriticalSection(&q_lock);
    return MMSYSERR_NOERROR;
}

static MMRESULT fk_reset(HWAVEOUT wo)
{
    (void)wo;
    EnterCriticalSection(&q_lock);
    while (q_n > 0) {
        q[q_head]->dwFlags = (q[q_head]->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE;
        q_head = (q_head + 1) % 64;
        q_n--;
    }
    q_off = 0;
    q_started = 0;
    LeaveCriticalSection(&q_lock);
    SetEvent(q_event);
    return 0;
}

static MMRESULT fk_close(HWAVEOUT wo)
{
    (void)wo;
    InterlockedExchange(&q_stop, 1);
    WaitForSingleObject(q_thread, INFINITE);
    CloseHandle(q_thread);
    return 0;
}

static UINT fk_numdevs(void) { return 1; }

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 255), f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int wmain(int argc, wchar_t **argv)
{
    double seconds = argc > 1 ? _wtof(argv[1]) : 20.0;
    int st = argc > 2 ? _wtoi(argv[2]) : 0;
    stall_every = argc > 3 ? _wtoi(argv[3]) : 1000;
    if (st < 0) stall_rand = 1;
    stall_ms = st;
    next_stall_at = 1.0;
    srand(1234);
    InitializeCriticalSection(&q_lock);
    cap = (int16_t *)calloc(CAP_MAX, sizeof(int16_t));

    audio_init();
    audio_set_wind(0.42);
    if (!audio_set_enabled(true)) { printf("cannot enable\n"); return 2; }
    static const double looks[] = { 0, 0.02, 0.2, 0.42, 0.68, 0.82, 1 };
    double t0 = now_s(), next = 1.0;
    int k = 0;
    while (now_s() - t0 < seconds) {
        if (now_s() - t0 > next) {
            audio_set_wind(looks[k % 7]);
#ifdef SY_IN_LP
            if (k % 3 == 2) audio_set_interior(!audio_interior());
#endif
            if (k % 4 == 1) audio_thunder(0.8);
            /* a busy UI thread: 300 ms of CPU */
            double b = now_s();
            volatile double x = 0;
            while (now_s() - b < 0.3) x += 1.0;
            k++;
            next += 1.7;
        }
        Sleep(20);
    }
    long play_end = cap_n;
    InterlockedExchange(&counting, 0);
    audio_set_enabled(false);
    Sleep(3000);
    audio_shutdown();

    /* gaps: runs of >= 64 zero samples between the first sound and the switch-off */
    long first = 0, gaps = 0, run = 0, gap_samples = 0;
    while (first < play_end && cap[first] == 0) first++;
    first += SY_RATE / 5;                    /* the fade-in rounds to zero at first */
    for (long i = first; i < play_end; i++) {
        if (cap[i] == 0) { if (++run == 64) gaps++; }
        else { if (run >= 64) gap_samples += run; run = 0; }
    }
    int fade_ok = cap_n > 0 && cap[cap_n - 1] == 0;
    printf("%.0f s, stall %s%d ms every %d ms: played %ld periods, underruns %ld, gaps %ld (%.0f ms), "
           "fade-out ends silent %s, worker underrun counter %u\n",
           seconds, stall_rand ? "0.." : "", stall_rand ? -st : st, (int)stall_every, played_periods,
           underrun_periods, gaps, gap_samples * 1000.0 / SY_RATE, fade_ok ? "yes" : "NO",
#ifdef SY_IN_LP
           audio_underruns()
#else
           0u
#endif
           );
    if (argc > 4) {
        FILE *f = _wfopen(argv[4], L"wb");
        if (f) {
            long n = cap_n < CAP_MAX ? cap_n : CAP_MAX;
            fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + n * 2)); fwrite("WAVE", 1, 4, f);
            fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
            put32(f, SY_RATE); put32(f, SY_RATE * 2); put16(f, 2); put16(f, 16);
            fwrite("data", 1, 4, f); put32(f, (uint32_t)(n * 2));
            fwrite(cap, 2, (size_t)n, f);
            fclose(f);
        }
    }
    return (underrun_periods || gaps || !fade_ok) ? 1 : 0;
}
