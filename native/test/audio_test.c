/* audio_test.c - renders the wind synthesis to WAV files and checks it numerically.
 *
 * Includes ../src/audio.c with AUDIO_SYNTH_ONLY, so the exact block renderer the waveOut
 * thread uses is exercised (same SY_BLOCK chunking, same float->int16 conversion).
 *   audio_test [OUTDIR]     writes OUTDIR/wind_<level>.wav (6 s each) and wind_fade.wav
 * Checks per level (after the 1.5 s fade-in): RMS strictly increasing with wind, no
 * clipping, |DC| small, and the largest sample step across block seams and the noise loop
 * seam is no bigger than the largest step inside blocks. Also checks the off-fade reaches
 * exact silence and that thunder adds energy. Exit status 0 = pass.
 */
#define AUDIO_SYNTH_ONLY
#include "../src/audio.c"

#include <stdio.h>

#define SECONDS 6
#define NSAMP   (SY_RATE * SECONDS)

static int16_t to_pcm(float f) { return (int16_t)lrintf(f * 32767.0f); }

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 255), f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

static int write_wav(const char *path, const int16_t *pcm, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + n * 2)); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, SY_RATE); put32(f, SY_RATE * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)(n * 2));
    for (long i = 0; i < n; i++) put16(f, (uint16_t)pcm[i]);
    return fclose(f) == 0;
}

/* Renders n samples in SY_BLOCK chunks; returns the number of samples rendered before the
 * synth reported the end of a fade-out (n if it never did). */
static long render(Synth *s, int16_t *pcm, long n)
{
    float buf[SY_BLOCK];
    long done = 0;
    while (done < n) {
        int m = n - done < SY_BLOCK ? (int)(n - done) : SY_BLOCK;
        int more = synth_render(s, buf, m);
        for (int i = 0; i < m; i++) pcm[done + i] = to_pcm(buf[i]);
        done += m;
        if (!more) return done;
    }
    return done;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static int16_t pcm[NSAMP];
    char path[1024];
    double prev_rms = -1.0;
    const double levels[] = { 0.0, 0.15, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0 };
    const int nlev = (int)(sizeof levels / sizeof levels[0]);

    for (int L = 0; L < nlev; L++) {
        Synth s;
        if (!synth_init(&s, 12345u)) { printf("FAIL: out of memory\n"); return 1; }
        s.wind = levels[L];
        s.master_target = SY_MASTER_ON;
        render(&s, pcm, NSAMP);
        synth_free(&s);

        long from = (long)(1.5 * SY_RATE);
        double sum = 0, sum2 = 0;
        int peak = 0, clips = 0;
        int max_in = 0, max_seam = 0;
        for (long i = 0; i < NSAMP; i++) {
            int v = pcm[i];
            if (v >= 32767 || v <= -32767) clips++;
            if (abs(v) > peak) peak = abs(v);
            if (i >= from) { sum += v; sum2 += (double)v * v; }
            if (i > 0) {
                int d = abs(v - pcm[i - 1]);
                int seam = (i % SY_BLOCK) == 0 || (i % SY_LOOP) == 0;
                if (seam) { if (d > max_seam) max_seam = d; }
                else if (d > max_in) max_in = d;
            }
        }
        long cnt = NSAMP - from;
        double mean = sum / cnt, rms = sqrt(sum2 / cnt - mean * mean);
        printf("wind %.2f: rms %7.1f (%.1f dBFS)  peak %5d  dc %+6.2f  step in-block %4d seam %4d\n",
               levels[L], rms, 20 * log10(rms / 32767.0), peak, mean, max_in, max_seam);
        CHECK(rms > prev_rms * 1.01, "rms not increasing at wind %.2f", levels[L]);
        CHECK(clips == 0, "clipping at wind %.2f (%d samples)", levels[L], clips);
        CHECK(fabs(mean) < 0.02 * rms + 2.0, "DC offset %.2f at wind %.2f", mean, levels[L]);
        CHECK(max_seam <= max_in, "seam step %d > in-block max %d at wind %.2f",
              max_seam, max_in, levels[L]);
        CHECK(pcm[0] == 0 && abs(pcm[1]) < 4, "start not silent at wind %.2f", levels[L]);
        prev_rms = rms;

        snprintf(path, sizeof path, "%s/wind_%03d.wav", dir, (int)lrint(levels[L] * 100));
        CHECK(write_wav(path, pcm, NSAMP), "cannot write %s", path);
    }

    /* noise loop seam itself: table[SY_LOOP-1] -> table[0] must be an ordinary step */
    {
        Synth s;
        synth_init(&s, 777u);
        double seam = fabs(s.noise[0] - s.noise[SY_LOOP - 1]), maxd = 0;
        for (long i = 1; i < SY_LOOP; i++) {
            double d = fabs(s.noise[i] - s.noise[i - 1]);
            if (d > maxd) maxd = d;
        }
        printf("noise table: loop seam step %.5f, max in-table step %.5f\n", seam, maxd);
        CHECK(seam <= maxd, "noise loop seam step too large");
        synth_free(&s);
    }

    /* on -> off: fade reaches exact zero within ~3 s and ends without a jump */
    {
        Synth s;
        synth_init(&s, 4242u);
        s.wind = 0.8;
        s.master_target = SY_MASTER_ON;
        render(&s, pcm, 2 * SY_RATE);
        s.master_target = 0.0;
        long n = render(&s, pcm + 2 * SY_RATE, NSAMP - 2 * SY_RATE);
        long end = 2 * SY_RATE + n;
        printf("fade-out: silent after %.2f s, last sample %d\n", n / (double)SY_RATE, pcm[end - 1]);
        CHECK(n < NSAMP - 2 * SY_RATE, "fade-out never completed");
        CHECK(n / (double)SY_RATE < 3.0, "fade-out too slow");
        CHECK(pcm[end - 1] == 0, "fade-out does not end at zero");
        int tail = 0;
        for (long i = end - SY_BLOCK; i < end; i++) if (abs(pcm[i]) > tail) tail = abs(pcm[i]);
        CHECK(tail < 200, "last block still loud (%d)", tail);
        for (long i = end; i < NSAMP; i++) pcm[i] = 0;
        snprintf(path, sizeof path, "%s/wind_fade.wav", dir);
        write_wav(path, pcm, NSAMP);
        synth_free(&s);
    }

    /* thunder adds low-frequency energy after its delay */
    {
        Synth a, b;
        synth_init(&a, 99u);
        synth_init(&b, 99u);
        a.wind = b.wind = 0.5;
        a.master_target = b.master_target = SY_MASTER_ON;
        static int16_t pa[NSAMP], pb[NSAMP];
        render(&a, pa, 2 * SY_RATE);
        render(&b, pb, 2 * SY_RATE);
        synth_thunder(&b, 1.0);
        render(&a, pa + 2 * SY_RATE, NSAMP - 2 * SY_RATE);
        render(&b, pb + 2 * SY_RATE, NSAMP - 2 * SY_RATE);
        double ea = 0, eb = 0;
        int clips = 0;
        for (long i = 2 * SY_RATE; i < NSAMP; i++) {
            ea += (double)pa[i] * pa[i];
            eb += (double)pb[i] * pb[i];
            if (pb[i] >= 32767 || pb[i] <= -32767) clips++;
        }
        int pk = 0;
        for (long i = 0; i < NSAMP; i++) if (abs(pb[i]) > pk) pk = abs(pb[i]);
        printf("thunder: energy ratio %.2f, peak %d, clips %d\n", eb / ea, pk, clips);
        CHECK(eb > ea * 1.5, "thunder inaudible");
        CHECK(clips == 0, "thunder clips");
        snprintf(path, sizeof path, "%s/wind_thunder.wav", dir);
        write_wav(path, pb, NSAMP);
        synth_free(&a);
        synth_free(&b);
    }

    printf(fails ? "audio synthesis: FAILED (%d)\n" : "audio synthesis: OK\n", fails);
    return fails ? 1 : 0;
}
