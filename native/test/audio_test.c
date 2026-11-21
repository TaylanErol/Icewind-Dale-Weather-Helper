/* audio_test.c - renders the wind synthesis to WAV files and checks it numerically.
 *
 * Includes ../src/audio.c with AUDIO_SYNTH_ONLY, so the exact block renderer the waveOut
 * thread uses is exercised (same SY_BLOCK chunking, same float->int16 conversion).
 * (-DAUDIO_SRC='"path/audio.c"' measures another revision, e.g. for before/after numbers.)
 *   audio_test [OUTDIR] [MINUTES]
 *     writes OUTDIR/wind_<level>.wav (6 s each), wind_fade.wav, wind_thunder.wav,
 *     wind_interior.wav; MINUTES (default 10) is the stress length per wind level.
 *
 * Checks:
 *   - per level (after the 1.5 s fade-in): RMS strictly increasing with wind, no clipping,
 *     |DC| small, block seams no steeper than the signal itself;
 *   - stress: MINUTES per wind level, then MINUTES of random wind changes and interior
 *     toggles, all rendered in randomly sized chunks (1..SY_BLOCK samples). Dropout
 *     detectors on the int16 output: 20 ms windows more than 10 dB below both their
 *     neighbourhoods (60..300 ms before and after), runs of >= 64 near-zero samples,
 *     clicks (a sample step > 8x the step RMS of its 20 ms window), and the same at chunk
 *     seams. Also the gust trough: the quietest 250 ms against the median, flat and through
 *     a small-speaker model (4th-order high-pass at 400 Hz), and the correlation with the
 *     output 4 s earlier (the web loops a 4 s noise table, which repeats audibly);
 *   - the off-fade reaches exact silence; thunder adds energy without clipping;
 *   - calm air (wind 0): the quietest 250 ms within 3 dB of the median;
 *   - interior: energy above 600 Hz >= 10 dB down and above 2 kHz >= 20 dB down, quieter,
 *     no clipping; through the 400 Hz speaker model a storm inside is not quieter than a
 *     breeze inside; the toggle neither clicks nor dips; settled outside, the path stops.
 * Exit status 0 = pass.
 */
#define AUDIO_SYNTH_ONLY
#ifdef AUDIO_SRC
#include AUDIO_SRC
#else
#include "../src/audio.c"
#endif

#include <stdio.h>

#define SECONDS 6
#define NSAMP   (SY_RATE * SECONDS)
#define WIN     882                     /* 20 ms analysis window */
#define LOOP4S  (SY_RATE * 4 - 2048)    /* where audio.js's 4 s noise table wraps */

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

/* ---------------------------------------------------------------- stress analysis */

static uint32_t t_rng = 0x2545F491u;
static uint32_t trand(void) { t_rng ^= t_rng << 13; t_rng ^= t_rng >> 17; t_rng ^= t_rng << 5; return t_rng; }

static void hp_init(SyBiquad *f, double hz, double q)
{
    double w0 = 2.0 * M_PI * hz / SY_RATE, alpha = sin(w0) / (2.0 * q), c = cos(w0), a0 = 1.0 + alpha;
    memset(f, 0, sizeof *f);
    f->b0 = (1.0 + c) / 2.0 / a0;
    f->b1 = -(1.0 + c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0 * c / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(const double *v, int n, double *tmp)
{
    memcpy(tmp, v, sizeof(double) * n);
    qsort(tmp, n, sizeof(double), cmp_d);
    return tmp[n / 2];
}

typedef struct {
    long   dips, zero_runs, clicks, seam_clicks;
    double trough_flat, trough_spk;   /* quietest 250 ms vs median, dB */
    double loop_corr;                 /* correlation with the signal 4 s earlier (looping) */
    double rms_db;
    int    peak;
} Stress;

/* Renders `seconds` of audio in random chunks. wind < 0: random wind changes and interior
 * toggles. Analyses everything after the first 2 s. */
static Stress stress(double wind, int interior, double seconds, uint32_t seed)
{
    Stress r;
    memset(&r, 0, sizeof r);
    Synth s;
    synth_init(&s, seed);
    s.wind = wind < 0 ? 0.4 : wind;
    s.master_target = SY_MASTER_ON;
#ifdef SY_IN_LP
    s.interior = s.imix = interior;
#else
    (void)interior;
#endif
    long total = (long)(seconds * SY_RATE), skip = 2L * SY_RATE, nwin = (total - skip) / WIN;
    double *rf = malloc(sizeof(double) * nwin), *rs = malloc(sizeof(double) * nwin);
    double *tmp = malloc(sizeof(double) * (nwin + 16));
    SyBiquad h1, h2;
    hp_init(&h1, 400.0, 0.54119610);
    hp_init(&h2, 400.0, 1.30656296);
    static const double looks[] = { 0, 0.02, 0.03, 0.2, 0.22, 0.42, 0.68, 0.75, 0.82, 1 };
    long next_change = 0, next_toggle = 0;
    float buf[SY_BLOCK];
    long done = 0, w = 0, wpos = 0, zrun = 0;
    double ef = 0, es = 0, ed = 0, sum2 = 0;
    int prev = 0, maxd = 0, seam_d = 0, have_prev = 0;
    /* ring of the last LOOP4S output samples for the 4 s lag correlation */
    int16_t *ring = calloc(LOOP4S, sizeof(int16_t));
    double cxy = 0, cxx = 0, cyy = 0;
    while (done < total) {
        if (wind < 0 && done >= next_change) {
            s.wind = looks[trand() % 10];
            next_change = done + (long)(2 + trand() % 29) * SY_RATE;
        }
#ifdef SY_IN_LP
        if (wind < 0 && done >= next_toggle) {
            s.interior = s.interior > 0.5 ? 0.0 : 1.0;
            next_toggle = done + (long)(5 + trand() % 56) * SY_RATE;
        }
#endif
        int m = 1 + (int)(trand() % SY_BLOCK);
        if (m > total - done) m = (int)(total - done);
        synth_render(&s, buf, m);
        for (int i = 0; i < m; i++) {
            int v = to_pcm(buf[i]);
            long idx = done + i;
            int d = have_prev ? abs(v - prev) : 0;
            prev = v;
            have_prev = 1;
            double hs = sy_biquad(&h2, sy_biquad(&h1, v));
            if (idx < skip) continue;
            if (abs(v) > r.peak) r.peak = abs(v);
            if (abs(v) <= 1) { if (++zrun == 64) r.zero_runs++; } else zrun = 0;
            ef += (double)v * v;
            es += hs * hs;
            ed += (double)d * d;
            sum2 += (double)v * v;
            if (d > maxd) maxd = d;
            if (i == 0 && d > seam_d) seam_d = d;
            /* the same sample one noise-table length earlier */
            {
                long ph = idx % LOOP4S;
                if (idx >= skip + LOOP4S) {
                    double y0 = ring[ph];
                    cxy += y0 * v; cxx += y0 * y0; cyy += (double)v * v;
                }
                ring[ph] = (int16_t)v;
            }
            if (++wpos == WIN) {
                if (w < nwin) {
                    double drms = sqrt(ed / WIN);
                    rf[w] = sqrt(ef / WIN);
                    rs[w] = sqrt(es / WIN);
                    if (maxd > 8.0 * drms && maxd > 64) r.clicks++;
                    if (seam_d > 8.0 * drms && seam_d > 64) r.seam_clicks++;
                    w++;
                }
                wpos = 0; ef = es = ed = 0; maxd = seam_d = 0;
            }
        }
        done += m;
    }
    nwin = w;
    /* short dropouts: a 20 ms window far below both neighbourhoods */
    for (long i = 15; i + 15 < nwin; i++) {
        double a = median(rf + i - 15, 13, tmp), b = median(rf + i + 3, 13, tmp);
        double ref = a < b ? a : b;
        if (rf[i] < ref * 0.316) r.dips++;
    }
    /* gust trough: quietest 250 ms (12 windows) vs the median 250 ms */
    {
        long n12 = nwin / 12;
        double *bf = malloc(sizeof(double) * n12), *bs = malloc(sizeof(double) * n12);
        for (long k = 0; k < n12; k++) {
            double a = 0, b = 0;
            for (int j = 0; j < 12; j++) { a += rf[k * 12 + j] * rf[k * 12 + j]; b += rs[k * 12 + j] * rs[k * 12 + j]; }
            bf[k] = sqrt(a / 12); bs[k] = sqrt(b / 12);
        }
        double mf = median(bf, (int)n12, tmp), ms = median(bs, (int)n12, tmp);
        double lf = 1e30, ls = 1e30;
        for (long k = 0; k < n12; k++) { if (bf[k] < lf) lf = bf[k]; if (bs[k] < ls) ls = bs[k]; }
        r.trough_flat = 20 * log10((lf + 1e-9) / mf);
        r.trough_spk = 20 * log10((ls + 1e-9) / ms);
        free(bf); free(bs);
    }
    r.loop_corr = cxy / sqrt(cxx * cyy + 1e-9);
    free(ring);
    r.rms_db = 10 * log10(sum2 / (double)(total - skip) / (32767.0 * 32767.0) + 1e-20);
    free(rf); free(rs); free(tmp);
    synth_free(&s);
    return r;
}

/* Energy of x above `hz` (4th-order Butterworth high-pass) relative to its total, dB. */
static double hf_share_db(const int16_t *x, long n, double hz)
{
    SyBiquad h1, h2;
    hp_init(&h1, hz, 0.54119610);
    hp_init(&h2, hz, 1.30656296);
    double e = 0, eh = 0;
    for (long i = 0; i < n; i++) {
        double y = sy_biquad(&h2, sy_biquad(&h1, x[i]));
        if (i < SY_RATE) continue;
        e += (double)x[i] * x[i];
        eh += y * y;
    }
    return 10 * log10(eh / e);
}

static double energy_above(const int16_t *x, long n, double hz)
{
    SyBiquad h1, h2;
    hp_init(&h1, hz, 0.54119610);
    hp_init(&h2, hz, 1.30656296);
    double eh = 0;
    for (long i = 0; i < n; i++) {
        double y = sy_biquad(&h2, sy_biquad(&h1, x[i]));
        if (i >= SY_RATE) eh += y * y;
    }
    return eh;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    double minutes = argc > 2 ? atof(argv[2]) : 10.0;
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
                if ((i % SY_BLOCK) == 0) { if (d > max_seam) max_seam = d; }
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

    /* stress: long renders in random chunks, dropout detectors */
    printf("stress (%.1f min each, random chunks 1..%d):\n", minutes, SY_BLOCK);
    printf("  wind  rms dBFS  dips  zero-runs  clicks  seam-clicks  trough flat / speaker  4s-repeat\n");
    for (int L = -1; L < nlev; L++) {
        double wl = L < 0 ? -1.0 : levels[L];
        Stress r = stress(wl, 0, minutes * 60.0 + 2.0, 1000u + (uint32_t)L * 77u);
        if (L < 0) printf("  chg ");
        else printf("  %.2f", wl);
        printf("  %7.1f  %4ld  %9ld  %6ld  %11ld  %8.1f / %6.1f dB  %+5.2f\n",
               r.rms_db, r.dips, r.zero_runs, r.clicks, r.seam_clicks, r.trough_flat, r.trough_spk, r.loop_corr);
        CHECK(r.dips == 0, "%ld dropouts at wind %.2f", r.dips, wl);
        CHECK(r.zero_runs == 0, "%ld zero runs at wind %.2f", r.zero_runs, wl);
        CHECK(r.clicks == 0 && r.seam_clicks == 0, "clicks at wind %.2f (%ld, seams %ld)", wl, r.clicks, r.seam_clicks);
        CHECK(r.peak < 32767, "clipping in stress at wind %.2f", wl);
        if (L >= 0) {
            /* gentle gusts: never more than ~11 dB under the median, even on small speakers */
            CHECK(r.trough_flat > -11.0 && r.trough_spk > -11.0,
                  "gust trough too deep at wind %.2f (%.1f / %.1f dB)", wl, r.trough_flat, r.trough_spk);
            /* calm air gusts shallowly: the quietest bed must not sound like a cut */
            if (wl == 0.0)
                CHECK(r.trough_flat > -3.0 && r.trough_spk > -3.0,
                      "calm-air gust trough too deep (%.1f / %.1f dB)", r.trough_flat, r.trough_spk);
            CHECK(fabs(r.loop_corr) < 0.05, "the noise repeats every 4 s at wind %.2f (correlation %.2f)", wl, r.loop_corr);
        }
    }
#ifdef SY_IN_LP
    {
        Stress r = stress(0.75, 1, minutes * 60.0 + 2.0, 4321u);
        printf("  int.  %7.1f  %4ld  %9ld  %6ld  %11ld  %8.1f / %6.1f dB  (interior, wind 0.75)\n",
               r.rms_db, r.dips, r.zero_runs, r.clicks, r.seam_clicks, r.trough_flat, r.trough_spk);
        CHECK(r.dips == 0 && r.zero_runs == 0 && r.clicks == 0 && r.seam_clicks == 0,
              "dropouts or clicks with interior on");
    }
#endif

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

#ifdef SY_IN_LP
    /* interior: same seed and wind, muffled; then a thunder clap inside; then the toggle */
    {
        static int16_t po[NSAMP], pi[NSAMP];
        double worst = 0.0;
        for (int L = 0; L < nlev; L++) {
            Synth a, b;
            synth_init(&a, 555u);
            synth_init(&b, 555u);
            a.wind = b.wind = levels[L];
            a.master_target = b.master_target = SY_MASTER_ON;
            b.interior = b.imix = 1.0;
            render(&a, po, NSAMP);
            render(&b, pi, NSAMP);
            double eo = 0, ei = 0;
            int clips = 0;
            for (long i = SY_RATE; i < NSAMP; i++) {
                eo += (double)po[i] * po[i];
                ei += (double)pi[i] * pi[i];
                if (pi[i] >= 32767 || pi[i] <= -32767) clips++;
            }
            double hi = 10 * log10(energy_above(pi, NSAMP, 600.0) / energy_above(po, NSAMP, 600.0));
            double hi2 = 10 * log10(energy_above(pi, NSAMP, 2000.0) / energy_above(po, NSAMP, 2000.0));
            double lvl = 10 * log10(ei / eo);
            if (L == 0 || hi > worst) worst = hi;
            printf("interior wind %.2f: level %+5.1f dB, energy above 600 Hz %+5.1f dB, above 2 kHz %+5.1f dB "
                   "(share %5.1f -> %5.1f dB)\n", levels[L], lvl, hi, hi2,
                   hf_share_db(po, NSAMP, 600.0), hf_share_db(pi, NSAMP, 600.0));
            CHECK(hi < -10.0, "interior keeps too much above 600 Hz at wind %.2f (%.1f dB)", levels[L], hi);
            CHECK(hi2 < -20.0, "interior keeps too much above 2 kHz at wind %.2f (%.1f dB)", levels[L], hi2);
            CHECK(lvl < -2.0, "interior not quieter at wind %.2f", levels[L]);
            CHECK(clips == 0, "interior clips at wind %.2f", levels[L]);
            synth_free(&a);
            synth_free(&b);
        }
        /* inside, through a small-speaker model (400 Hz high-pass): a storm must not be
         * quieter than a breeze (the howl stays faintly audible through the walls) */
        {
            double spk[2];
            const double wv[2] = { 0.2, 1.0 };
            for (int j = 0; j < 2; j++) {
                Synth b;
                synth_init(&b, 777u);
                b.wind = wv[j];
                b.master_target = SY_MASTER_ON;
                b.interior = b.imix = 1.0;
                render(&b, pi, NSAMP);
                spk[j] = 10 * log10(energy_above(pi, NSAMP, 400.0) / (NSAMP - SY_RATE) / (32767.0 * 32767.0) + 1e-20);
                synth_free(&b);
            }
            printf("interior through a 400 Hz speaker model: wind 0.20 %.1f dBFS, wind 1.00 %.1f dBFS\n", spk[0], spk[1]);
            CHECK(spk[1] >= spk[0], "inside, a storm is quieter than a breeze on small speakers (%.1f < %.1f dBFS)", spk[1], spk[0]);
        }
        /* toggle cross-fade: 2 s outside, 2 s inside, 2 s outside; no click, no dip */
        Synth s;
        synth_init(&s, 31337u);
        s.wind = 0.75;
        s.master_target = SY_MASTER_ON;
        render(&s, pcm, 2 * SY_RATE);
        s.interior = 1.0;
        render(&s, pcm + 2 * SY_RATE, 2 * SY_RATE);
        s.interior = 0.0;
        render(&s, pcm + 4 * SY_RATE, 2 * SY_RATE);
        synth_thunder(&s, 1.0);
        snprintf(path, sizeof path, "%s/wind_interior.wav", dir);
        write_wav(path, pcm, NSAMP);
        /* 50 ms RMS through the toggles: monotone-ish, no window under -12 dB of the
         * quieter steady state */
        double steady_in = 0, steady_out = 0, minr = 1e30;
        for (long i = 3 * SY_RATE; i < 4 * SY_RATE; i++) steady_in += (double)pcm[i] * pcm[i];
        for (long i = 1 * SY_RATE; i < 2 * SY_RATE; i++) steady_out += (double)pcm[i] * pcm[i];
        steady_in = sqrt(steady_in / SY_RATE);
        steady_out = sqrt(steady_out / SY_RATE);
        for (long w0 = SY_RATE; w0 + 2205 <= NSAMP; w0 += 2205) {
            double e = 0;
            for (long i = w0; i < w0 + 2205; i++) e += (double)pcm[i] * pcm[i];
            e = sqrt(e / 2205);
            if (e < minr) minr = e;
        }
        printf("interior toggle: steady outside %.0f, inside %.0f, quietest 50 ms %.0f (%.1f dB under inside)\n",
               steady_out, steady_in, minr, 20 * log10(minr / steady_in));
        CHECK(minr > steady_in * 0.25, "interior toggle dips");
        render(&s, pcm, NSAMP);
        CHECK(s.imix == 0.0, "interior mix does not settle to exactly 0 (denormals)");
        CHECK(s.in_active == 0, "interior path still running while settled outside");
        synth_free(&s);
    }
#endif

    printf(fails ? "audio synthesis: FAILED (%d)\n" : "audio synthesis: OK\n", fails);
    return fails ? 1 : 0;
}
