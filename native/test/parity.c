/* Parity harness, C side: prints exactly what native/test/parity.js prints, computed with
 * native/src/engine.c. Build with host gcc (see run.sh):
 *
 *   ./parity SEEDS_FILE DAYS LONG_DAYS > c.txt
 *
 * Every line is the JSON.stringify() text of an array; doubles are the 16 hex digits of
 * their IEEE-754 bits. Keep the line order and the field order identical to parity.js. */
#include "../src/engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* ---------- a tiny JSON.stringify-compatible writer ---------- */

static int depth;
static int first[16];

static void sep(void)
{
    if (!first[depth]) putchar(',');
    first[depth] = 0;
}
static void arr_open(void) { sep(); putchar('['); first[++depth] = 1; }
static void arr_close(void) { putchar(']'); depth--; }
static void line_open(void) { depth = 0; first[0] = 1; putchar('['); first[++depth] = 1; }
static void line_close(void) { putchar(']'); putchar('\n'); depth = 0; }

static void j_int(long long v) { sep(); printf("%lld", v); }
static void j_bool(bool b) { sep(); fputs(b ? "true" : "false", stdout); }
static void j_null(void) { sep(); fputs("null", stdout); }
static void j_str(const char *s)
{
    sep();
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned c = *p;
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20) printf("\\u%04x", c);
            else putchar((int)c);
        }
    }
    putchar('"');
}
static void j_hex(double x)
{
    uint64_t b;
    memcpy(&b, &x, sizeof b);
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%016llx", (unsigned long long)b);
    j_str(tmp);
}
/* Number -> string for the few non-integer numbers printed (they are short and exact). */
static void j_num(double x)
{
    sep();
    if (x == floor(x) && fabs(x) < 1e15) printf("%.0f", x == 0 ? 0.0 : x);
    else printf("%.17g", x);
}

/* ---------- canonical encodings ---------- */

static void j_rolls(int n, const uint8_t *r)
{
    arr_open();
    for (int i = 0; i < n; i++) j_int(r[i]);
    arr_close();
}

static void j_act_fields(const IwdActive *a)
{
    j_int(a->id);
    j_rolls(a->n_rolls, a->rolls);
    j_int(a->duration);
    j_int(a->elapsed);
    j_int(a->next_idx);
    j_int(a->next);
    j_int(a->cause_type);
    j_int(a->cause_arg);
    j_int(a->start_day);
    j_int(a->start_hour);
}
static void j_act(const IwdActive *a) { arr_open(); j_act_fields(a); arr_close(); }

/* ---------- seeds ---------- */

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(2); }
    b[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int hexv(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static IwdDay *g_days;
static int g_ndays;

static int event_at(void *user, int hour)
{
    int v = *(int *)user;
    int d = v, h = hour;
    while (h < 0) { d--; h += 24; }
    while (h >= 24) { d++; h -= 24; }
    if (d < 0) return 1;
    if (d >= g_ndays) return g_days[g_ndays - 1].carry_out.id;
    return g_days[d].hour_id[h];
}

static void run_seed(int si, const char *seed, int ndays, int long_days)
{
    line_open();
    j_str("K"); j_int(si); j_int(iwd_hash_seed(seed)); j_int(iwd_temp_hash(seed));
    j_int((long long)iwd_utf8_to_utf16(seed, NULL, 0));
    line_close();

    IwdChain chain;
    iwd_chain_init(&chain, seed);
    IwdDay *days = calloc((size_t)(ndays > 0 ? ndays : 1), sizeof *days);
    char buf[256], buf2[256];
    for (int di = 0; di < ndays; di++) {
        IwdDay *d = &days[di];
        iwd_next_day(&chain, d);
        line_open();
        j_str("D"); j_int(si); j_int(d->day); j_int(chain.rng_state); j_int(d->temp_min); j_int(d->temp_max);
        j_int(iwd_day_dominant(d));
        if (d->has_carried_in) j_act(&d->carried_in); else j_null();
        j_act(&d->carry_out);
        line_close();
        for (int i = 0; i < d->n_segments; i++) {
            const IwdSegment *s = &d->segments[i];
            line_open();
            j_str("S"); j_int(si); j_int(d->day); j_int(s->id); j_int(s->start); j_int(s->end); j_int(s->duration);
            j_rolls(s->n_rolls, s->rolls); j_int(s->next); j_int(s->next_idx); j_int(s->elapsed_before);
            j_bool(s->from_prev_day); j_bool(s->into_next_day); j_int(s->change_at); j_int(s->cause_type); j_int(s->cause_arg);
            line_close();
        }
        const IwdSegment *log[IWD_MAX_SEGMENTS];
        int nl = iwd_day_log(d, log);
        for (int i = 0; i < nl; i++) {
            const IwdSegment *s = log[i];
            line_open();
            j_str("L"); j_int(si); j_int(d->day); j_int(s->start);
            j_int(s->id); j_rolls(s->n_rolls, s->rolls); j_int(s->duration); j_int(0); j_int(s->next_idx); j_int(s->next);
            j_int(s->cause_type); j_int(s->cause_arg); j_int(d->day); j_int(s->start);
            line_close();
        }
        for (int h = 0; h < 24; h++) {
            const IwdEventDef *ev = iwd_event(d->hour_id[h]);
            IwdLight L = iwd_light_at(ev, h);
            double off = iwd_temp_offset(seed, d->day - 1, h);
            line_open();
            j_str("H"); j_int(si); j_int(d->day); j_int(h); j_int(d->hour_id[h]);
            j_str(iwd_light_level_key(L.level));
            iwd_light_label(ev, &L, buf, sizeof buf); j_str(buf);
            j_bool(L.aurora_visible); j_bool(L.moon); j_bool(L.twilight); j_bool(L.aurora);
            j_int(d->temp_f[h]); j_hex(off);
            IwdForecast f = iwd_forecast(d, h);
            iwd_fmt_forecast(&f, buf, sizeof buf, NULL); j_str(buf);
            iwd_fmt_light_value(ev, &L, buf2, sizeof buf2); j_str(buf2);
            j_str(iwd_light_badge(L.level));
            line_close();
        }
        if (di < 3) {
            line_open();
            j_str("T"); j_int(si); j_int(d->day);
            arr_open();
            for (int k = -2; k <= 100; k++) {
                double t = k / 4.0 + 0.07;
                double v = iwd_day_temp_at(d, chain.temp_hash, t);
                j_hex(v); j_int((long long)iwd_js_round(v));
            }
            arr_close();
            line_close();
        }
    }
    if (si < 40) {
        g_days = days; g_ndays = ndays;
        for (int v = 0; v < (ndays < 4 ? ndays : 4); v++) {
            for (int k = 0; k < 240; k += 3) {
                IwdLook p;
                iwd_look_at(event_at, &v, k / 10.0, &p);
                const double *pv = &p.cloud;
                line_open();
                j_str("P"); j_int(si); j_int(v); j_int(k);
                arr_open();
                for (int i = 0; i < 14; i++) j_hex(pv[i]);
                arr_close();
                line_close();
            }
        }
    }
    if (si < 6 && long_days > 0) {
        IwdChain c2;
        iwd_chain_init(&c2, seed);
        IwdDay d;
        line_open();
        j_str("X"); j_int(si);
        /* the summary string is built first, like the JS join */
        size_t cap = (size_t)long_days * 256 + 64, len = 0;
        char *sb = malloc(cap);
        sb[0] = 0;
        for (int i = 0; i < long_days; i++) {
            iwd_next_day(&c2, &d);
            if (i) sb[len++] = ';';
            len += (size_t)sprintf(sb + len, "%d;%d;%d;%d;", d.n_segments, iwd_day_dominant(&d), d.temp_min, d.temp_max);
            for (int h = 0; h < 24; h++) len += (size_t)sprintf(sb + len, h ? ",%d" : "%d", d.hour_id[h]);
            sb[len++] = ';';
            for (int h = 0; h < 24; h++) len += (size_t)sprintf(sb + len, h ? ",%d" : "%d", d.temp_f[h]);
            sb[len] = 0;
        }
        j_int(c2.rng_state); j_int(c2.day_count); j_act(&c2.current);
        j_str(sb);
        free(sb);
        line_close();
    }
    free(days);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) { fprintf(stderr, "usage: parity SEEDS_FILE [DAYS] [LONG_DAYS]\n"); return 2; }
    int ndays = argc > 2 ? atoi(argv[2]) : 60;
    int long_days = argc > 3 ? atoi(argv[3]) : 0;
    size_t flen;
    char *file = read_all(argv[1], &flen);
    static char seed[1 << 16];
    int si = 0;
    char *p = file;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t k = 0;
        for (size_t i = 0; i + 1 < n && k + 1 < sizeof seed; i += 2)
            seed[k++] = (char)(hexv(p[i]) * 16 + hexv(p[i + 1]));
        seed[k] = 0;
        run_seed(si++, seed, ndays, long_days);
        if (!e) break;
        p = e + 1;
    }
    free(file);
    char buf[512];

    /* ---------- effects / event data / cold line ---------- */
    for (int id = 1; id <= 12; id++) {
        const IwdEventDef *ev = iwd_event(id);
        const IwdWind *w = iwd_wind(ev->wind);
        line_open();
        j_str("V"); j_int(id); j_str(ev->name); j_int(ev->dice); j_str(w->key); j_str(w->label);
        j_int(iwd_wind_meter(ev->wind)); j_int(ev->temp_f);
        j_str(ev->light == IWD_EVLIGHT_NONE ? "none" : ev->light == IWD_EVLIGHT_DIM ? "dim" : "lightning");
        if (ev->moon) j_str(ev->moon); else j_null();
        j_bool(ev->natural);
        arr_open(); for (int i = 0; i < 3; i++) j_int(ev->next[i]); arr_close();
        j_str(ev->flavor); j_int(iwd_severity(id)); j_str(iwd_severity_label(iwd_severity(id)));
        snprintf(buf, sizeof buf, "#%06x", (unsigned)iwd_event_rgb(id)); j_str(buf);
        j_bool(iwd_event_rgb_is_dark(id));
        arr_open(); for (int i = 0; i < 3; i++) j_str(iwd_next_option_label(ev, i)); arr_close();
        const IwdLook *lk = iwd_look(id);
        arr_open(); for (int i = 0; i < 14; i++) j_hex((&lk->cloud)[i]); arr_close();
        line_close();
        for (int h = 0; h < 24; h++) {
            for (int u = 0; u < 2; u++) {
                IwdHourEffects fx;
                iwd_hour_effects(id, h, -50, u == 1, &fx);
                line_open();
                j_str("E"); j_int(id); j_int(h); j_str(u ? "C" : "F");
                arr_open();
                for (int i = 0; i < fx.count; i++) {
                    arr_open(); j_str(iwd_cat_key(fx.items[i].cat)); j_str(iwd_cat_label(fx.items[i].cat));
                    j_str(fx.items[i].text); arr_close();
                }
                arr_close();
                snprintf(buf, sizeof buf, "%d in play \xC2\xB7 %s", fx.count, ev->name); j_str(buf);
                line_close();
            }
        }
    }
    for (int t = -260; t <= 90; t++) {
        for (int u = 0; u < 2; u++) {
            bool c = u == 1;
            IwdHourEffects fx;
            iwd_hour_effects(3, 12, t, c, &fx);
            IwdCold cold = iwd_cold_dc(t);
            line_open();
            j_str("C"); j_int(t); j_str(c ? "C" : "F"); j_str(fx.items[0].text);
            j_str(cold.tier->key); j_str(cold.tier->name); j_int(cold.dc); j_int(cold.delta);
            iwd_fmt_tier_tip(&cold, c, buf, sizeof buf); j_str(buf);
            j_str(iwd_temp_note(t));
            iwd_fmt_temp(t, c, buf, sizeof buf); j_str(buf);
            j_int(c ? iwd_f_to_c(t) : t);
            iwd_fmt_minus(t, buf, sizeof buf); j_str(buf);
            line_close();
        }
    }

    /* ---------- light at awkward hours ---------- */
    static const char *const HS[] = { "-49.5", "-24", "-1", "-0.5", "0", "0.99", "1.5", "5.999", "6", "13.7",
                                      "21.2", "23.99", "24", "25.5", "47", "1000000.5" };
    for (int id = 1; id <= 12; id++) {
        for (size_t i = 0; i < sizeof HS / sizeof HS[0]; i++) {
            const IwdEventDef *ev = iwd_event(id);
            IwdLight L = iwd_light_at(ev, strtod(HS[i], NULL));
            line_open();
            j_str("G"); j_int(id); j_str(HS[i]); j_str(iwd_light_level_key(L.level));
            iwd_light_label(ev, &L, buf, sizeof buf); j_str(buf);
            j_bool(L.aurora_visible); j_bool(L.moon); j_bool(L.twilight); j_bool(L.aurora);
            line_close();
        }
    }

    /* ---------- temperature model ---------- */
    {
        static const char *const S3[] = { "", "RIME-00FF", "x" };
        static const int DS[] = { -3, -1, 0, 1, 7, 3649, 100000 };
        static const double HH[] = { 0, 0.5, 1.999, 2, 2.25, 11.5, 12, 12.001, 18.75, 23, 23.999 };
        for (int i = 0; i < 3; i++) {
            line_open();
            j_str("O"); j_int(i);
            arr_open();
            for (int a = 0; a < 7; a++)
                for (int b = 0; b < 11; b++) j_hex(iwd_temp_offset(S3[i], DS[a], HH[b]));
            arr_close();
            line_close();
        }
    }

    /* ---------- Math.round / °C ---------- */
    {
        static const double XS[] = { 0, -0.5, 0.5, -1.5, 1.5, -2.5, 2.5, -50.5, -50.49999999999999,
            0.49999999999999994, -0.49999999999999994, 4503599627370495.5, -4503599627370495.5,
            4503599627370497, 1e300, -1e300, 123.456, -123.456, 2.5000000000000004, -2.4999999999999996 };
        line_open();
        j_str("R");
        arr_open();
        for (size_t i = 0; i < sizeof XS / sizeof XS[0]; i++) { arr_open(); j_hex(XS[i]); j_hex(iwd_js_round(XS[i])); arr_close(); }
        arr_close();
        line_close();
        line_open();
        j_str("FC");
        arr_open();
        for (int f = -400; f <= 300; f++) j_int(iwd_f_to_c(f));
        arr_close();
        line_close();
    }

    /* ---------- RNG ---------- */
    {
        static const uint32_t ST[] = { 0, 1, 0xdeadbeefu, 0xffffffffu, 0x6d2b79f5u, 2105444690u };
        static const int SIDES[5] = { 2, 4, 12, 100, 20 };
        for (size_t s = 0; s < 6; s++) {
            IwdRng r, r2;
            iwd_rng_init(&r, ST[s]);
            iwd_rng_init(&r2, ST[s]);
            line_open();
            j_str("N"); j_int(ST[s]);
            arr_open(); for (int i = 0; i < 64; i++) j_hex(iwd_rng_next(&r)); arr_close();
            arr_open(); for (int i = 0; i < 64; i++) j_int(iwd_rng_die(&r2, SIDES[i % 5])); arr_close();
            j_int(r2.state);
            line_close();
        }
        static const uint32_t RS[][2] = { { 0, 0 }, { 15, 65535 }, { 16, 65536 }, { 0xffffffffu, 0xffffffffu },
                                          { 123456789, 987654321 }, { 7, 0xabc } };
        line_open();
        j_str("W");
        arr_open();
        for (int i = 0; i < 6; i++) {
            char sb[IWD_RANDOM_SEED_CAP];
            iwd_random_seed(RS[i][0], RS[i][1], sb, sizeof sb);
            j_str(sb);
        }
        arr_close();
        line_close();
    }

    /* ---------- calendar ---------- */
    {
        uint32_t s = 12345;
        const int64_t SPAN = (int64_t)IWD_ABS_MAX - IWD_ABS_MIN + 1;
        int32_t *abs_list = malloc(sizeof(int32_t) * 40000);
        int na = 0;
        for (int a = -3000; a <= 3000; a++) abs_list[na++] = a;
        for (int a = 543858 - 800; a <= 543858 + 800; a++) abs_list[na++] = a;
        for (int a = IWD_ABS_MIN; a < IWD_ABS_MIN + 400; a++) abs_list[na++] = a;
        for (int a = IWD_ABS_MAX - 400; a <= IWD_ABS_MAX; a++) abs_list[na++] = a;
        for (int i = 0; i < 20000; i++) {
            s = s * 1103515245u + 12345u;
            abs_list[na++] = (int32_t)(IWD_ABS_MIN + (int64_t)(s % (uint64_t)SPAN));
        }
        for (int i = 0; i < na; i++) {
            int32_t a = abs_list[i];
            IwdDate d = iwd_from_abs(a);
            int32_t my; int mm;
            iwd_month_of(a, &my, &mm);
            line_open();
            j_str("A"); j_int(a); j_int(d.year);
            if (d.month >= 0) j_int(d.month); else j_null();
            if (d.day) j_int(d.day); else j_null();
            if (d.festival != IWD_FEST_NONE) j_str(iwd_festival(d.festival)->key); else j_null();
            j_int(d.day_of_year);
            if (d.tenday_index >= 0) j_int(d.tenday_index); else j_null();
            if (d.day_of_tenday) j_int(d.day_of_tenday); else j_null();
            iwd_format_date(a, false, buf, sizeof buf); j_str(buf);
            iwd_format_date(a, true, buf, sizeof buf); j_str(buf);
            j_int(my); j_int(mm); j_bool(iwd_is_leap(d.year)); j_int(iwd_year_start(d.year)); j_int(iwd_days_in_year(d.year));
            line_close();
        }
        free(abs_list);

        int years[80], ny = 0;
        for (int y = -405; y <= -395; y++) years[ny++] = y;
        for (int y = -9; y <= 9; y++) years[ny++] = y;
        static const int MORE[] = { 1370, 1371, 1372, 1488, 1489, 1490, 1491, 1492, 99996, 99997, 99998, 99999,
                                    -99999, -99998, -99997, -99996 };
        for (size_t i = 0; i < sizeof MORE / sizeof MORE[0]; i++) years[ny++] = MORE[i];
        static const int DD[] = { -1, 0, 1, 10, 11, 15, 30, 31 };
        for (int yi = 0; yi < ny; yi++) {
            int y = years[yi];
            int32_t r;
            line_open();
            j_str("Y"); j_int(y);
            arr_open();
            for (int m = -1; m <= 12; m++)
                for (int k = 0; k < 8; k++) {
                    if (iwd_to_abs(y, m, DD[k], IWD_FEST_NONE, &r)) j_int(r); else j_null();
                }
            for (int f = 0; f < IWD_FEST_COUNT; f++) {
                if (iwd_to_abs(y, 0, 0, (IwdFestival)f, &r)) j_int(r); else j_null();
            }
            if (iwd_festival_from_key("bogus") != IWD_FEST_NONE && iwd_to_abs(y, 0, 0, iwd_festival_from_key("bogus"), &r)) j_int(r);
            else j_null();
            arr_close();
            line_close();
            for (int m = 0; m < 12; m++) {
                IwdMonthLayout L;
                iwd_month_layout(y, m, &L);
                line_open();
                j_str("M"); j_int(y); j_int(m); j_str(L.name); j_str(L.common); j_int(L.first_abs); j_int(L.first_abs + 29);
                arr_open();
                for (int i = 0; i < L.n_festivals; i++) {
                    arr_open(); j_int(L.festivals[i].abs); j_str(iwd_festival(L.festivals[i].festival)->key);
                    j_str(iwd_festival(L.festivals[i].festival)->name); arr_close();
                }
                arr_close();
                line_close();
            }
        }

        const double OFFS[9] = { 0, 0.5, -3.25, 15.21875, 30.4375, -100.1, 1e-9, 7.77, NAN };
        int32_t *ma = malloc(sizeof(int32_t) * 6000);
        int nm = 0;
        for (int a = 543858 - 400; a <= 543858 + 400; a++) ma[nm++] = a;
        for (int a = -3000; a <= 3000; a += 7) ma[nm++] = a;
        for (int i = 0; i < 2000; i++) {
            s = s * 1103515245u + 12345u;
            ma[nm++] = (int32_t)(IWD_ABS_MIN + (int64_t)(s % (uint64_t)SPAN));
        }
        ma[nm++] = IWD_ABS_MIN; ma[nm++] = IWD_ABS_MAX; ma[nm++] = 501123; ma[nm++] = 501138; ma[nm++] = 501139;
        for (int i = 0; i < nm; i++) {
            int32_t a = ma[i];
            line_open();
            j_str("Q"); j_int(a);
            arr_open();
            for (int oi = 0; oi < 9; oi++) {
                if (oi > 2 && (a % 5)) continue;
                IwdMoon m = iwd_moon(a, OFFS[oi]);
                arr_open();
                j_hex(m.age); j_hex(m.phase); j_hex(m.illum); j_int(iwd_moon_percent(&m)); j_bool(m.waxing);
                j_str(m.name); j_bool(m.is_new); j_bool(m.is_full);
                arr_close();
            }
            arr_close();
            line_close();
        }
        free(ma);

        int32_t anchors[13];
        anchors[0] = 543858; anchors[1] = 543858 + 30; anchors[2] = 543858 + 31;
        iwd_to_abs(1489, 0, 0, IWD_FEST_MIDWINTER, &anchors[3]);
        iwd_to_abs(1492, 0, 0, IWD_FEST_SHIELDMEET, &anchors[4]);
        iwd_to_abs(1492, 0, 0, IWD_FEST_MIDSUMMER, &anchors[5]);
        iwd_to_abs(1489, 11, 30, IWD_FEST_NONE, &anchors[6]);
        iwd_to_abs(1489, 4, 10, IWD_FEST_NONE, &anchors[7]);
        anchors[8] = -400; anchors[9] = 0; anchors[10] = -1;
        iwd_to_abs(-4, 0, 0, IWD_FEST_SHIELDMEET, &anchors[11]);
        iwd_to_abs(1490, 6, 20, IWD_FEST_NONE, &anchors[12]);
        static const double OO[3] = { 0, 2.5, -14 };
        int32_t *occ = malloc(sizeof(int32_t) * 4000);
        for (int ai = 0; ai < 13; ai++) {
            for (int rp = 0; rp < IWD_REPEAT_COUNT; rp++) {
                for (int oi = 0; oi < 3; oi++) {
                    int32_t an = anchors[ai];
                    int32_t to = rp == IWD_REPEAT_YEARLY ? an + 3000 : an + 900;
                    int n = iwd_occurrences(an, (IwdRepeat)rp, an - 60, to, OO[oi], occ, 4000);
                    line_open();
                    j_str("U"); j_int(an); j_str(iwd_repeat_key((IwdRepeat)rp)); j_num(OO[oi]);
                    arr_open(); for (int i = 0; i < n; i++) j_int(occ[i]); arr_close();
                    j_bool(iwd_occurs_on(an, (IwdRepeat)rp, an - 1, OO[oi]));
                    j_bool(iwd_occurs_on(an, (IwdRepeat)rp, an, OO[oi]));
                    line_close();
                }
            }
        }
        free(occ);
        line_open();
        j_str("B");
        arr_open();
        for (int r = 0; r < IWD_REPEAT_COUNT; r++) { arr_open(); j_str(iwd_repeat_key((IwdRepeat)r)); j_str(iwd_repeat_label((IwdRepeat)r)); arr_close(); }
        arr_close();
        arr_open();
        for (int m = 0; m < 12; m++) { arr_open(); j_str(iwd_month(m)->name); j_str(iwd_month(m)->common); arr_close(); }
        arr_close();
        arr_open();
        for (int f = 0; f < IWD_FEST_COUNT; f++) {
            const IwdFestivalDef *fd = iwd_festival((IwdFestival)f);
            arr_open(); j_str(fd->key); j_str(fd->name); j_int(fd->after); j_bool(fd->leap_only); arr_close();
        }
        arr_close();
        line_close();
    }

    /* ---------- small text helpers ---------- */
    {
        line_open();
        j_str("Z");
        arr_open(); for (int h = -50; h <= 60; h++) { iwd_fmt_hour(h, buf, sizeof buf); j_str(buf); } arr_close();
        arr_open();
        for (int k = 0; k <= 2000; k++) { iwd_fmt_clock(k * 0.0125, buf, sizeof buf); j_str(buf); }
        static const double CL[] = { 23.999, 24, 30, 12.5, 0.016666666666666666 };
        for (int i = 0; i < 5; i++) { iwd_fmt_clock(CL[i], buf, sizeof buf); j_str(buf); }
        arr_close();
        arr_open();
        for (int n = -40; n <= 40; n++) { iwd_rel_days(n, buf, sizeof buf); j_str(buf); }
        iwd_rel_days(1000000, buf, sizeof buf); j_str(buf);
        iwd_rel_days(-1000000, buf, sizeof buf); j_str(buf);
        arr_close();
        arr_open();
        static const int MN[] = { 0, 5, -5, -2147483647, 2147483647, 100, -100 };
        for (int i = 0; i < 7; i++) { iwd_fmt_minus(MN[i], buf, sizeof buf); j_str(buf); }
        arr_close();
        arr_open(); for (int n = 0; n <= 120; n++) { iwd_fmt_pad2(n, buf, sizeof buf); j_str(buf); } arr_close();
        line_close();

        static const double FADES[4] = { 1.6, 1.2, 0.8, 0.3 };
        static const double WINS[6][2] = { { 10, 14 }, { 20, 6 }, { 22, 2 }, { 0, 24 }, { 5, 5 }, { -3, 30 } };
        line_open();
        j_str("BAND");
        arr_open();
        for (int w = 0; w < 6; w++)
            for (int f = 0; f < 4; f++)
                for (int k = -10; k <= 250; k += 3) j_hex(iwd_band(k / 10.0, WINS[w][0], WINS[w][1], FADES[f]));
        arr_close();
        line_close();
    }
    return 0;
}
