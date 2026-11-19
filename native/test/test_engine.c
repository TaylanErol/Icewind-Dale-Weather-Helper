/* Engine checks the JS parity run cannot make: native-only limits, buffer truncation, the
 * header's test vectors. Built and run by run.sh. */
#include "../src/engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    /* header test vectors */
    CHECK(iwd_hash_seed("") == 1382371643u);
    CHECK(iwd_hash_seed("RIME-00FF") == 2105444690u);
    CHECK(iwd_hash_seed("RIME-00FF|temp") == 4064753795u);
    CHECK(iwd_temp_hash("RIME-00FF") == 4064753795u);
    CHECK(iwd_hash_seed("\xC3\xA9\xE2\x98\x83\xF0\x9D\x84\x9E") == 503696356u);
    uint16_t u[8];
    CHECK(iwd_utf8_to_utf16("\xC3\xA9\xE2\x98\x83\xF0\x9D\x84\x9E", u, 8) == 4);
    CHECK(u[0] == 0xE9 && u[1] == 0x2603 && u[2] == 0xD834 && u[3] == 0xDD1E);
    CHECK(iwd_hash_seed_utf16(u, 4) == 503696356u);
    CHECK(iwd_utf8_to_utf16(NULL, NULL, 0) == 0);

    /* dates */
    int32_t a;
    CHECK(iwd_to_abs(1489, 0, 1, IWD_FEST_NONE, &a) && a == IWD_DEFAULT_START_ABS);
    CHECK(iwd_year_start(1372) == IWD_FULL_EPOCH);
    CHECK(iwd_year_start(IWD_YEAR_MIN) == IWD_ABS_MIN);
    CHECK(iwd_year_start(IWD_YEAR_MAX + 1) - 1 == IWD_ABS_MAX);
    CHECK(!iwd_to_abs(IWD_YEAR_MAX + 1, 0, 1, IWD_FEST_NONE, &a));
    CHECK(!iwd_to_abs(IWD_YEAR_MIN - 1, 0, 1, IWD_FEST_NONE, &a));
    CHECK(!iwd_to_abs(1489, 0, 0, IWD_FEST_SHIELDMEET, &a));
    CHECK(iwd_to_abs(1488, 0, 0, IWD_FEST_SHIELDMEET, &a));
    CHECK(!iwd_to_abs(1489, 0, 0, (IwdFestival)99, &a));
    CHECK(iwd_from_abs(IWD_ABS_MAX + 5).abs == IWD_ABS_MAX);
    CHECK(iwd_from_abs(INT32_MIN).abs == IWD_ABS_MIN);
    CHECK(iwd_festival_from_key("shieldmeet") == IWD_FEST_SHIELDMEET);
    CHECK(iwd_festival_from_key("nope") == IWD_FEST_NONE && iwd_festival_from_key(NULL) == IWD_FEST_NONE);
    CHECK(iwd_repeat_from_key("yearly") == IWD_REPEAT_YEARLY && iwd_repeat_from_key("x") == IWD_REPEAT_COUNT);

    /* moon check value from the header */
    IwdMoon m = iwd_moon(543858, 0);
    CHECK(m.age == 16.46875 && m.phase == 0.5410677618069816 && !m.waxing && strcmp(m.name, "Waning Gibbous") == 0);
    CHECK(iwd_moon(543858, NAN).age == m.age);

    /* cold examples from the header */
    CHECK(iwd_cold_dc(-50).dc == 16 && iwd_cold_dc(-50).tier_index == 1 && iwd_cold_dc(-50).delta == 1);
    CHECK(iwd_cold_dc(-79).dc == 18 && iwd_cold_dc(-80).dc == 20 && iwd_cold_dc(-80).tier_index == 2);
    CHECK(iwd_cold_dc(-139).dc == 28 && iwd_cold_dc(-139).tier_index == 4);
    CHECK(iwd_cold_dc(5).dc == 10 && iwd_cold_dc(500).dc == 0);

    /* js round */
    CHECK(iwd_js_round(-2.5) == -2 && iwd_js_round(-50.5) == -50 && iwd_js_round(2.5) == 3);
    CHECK(iwd_js_round(0.49999999999999994) == 0 && signbit(iwd_js_round(-0.2)));
    CHECK(iwd_floor_div(-1, 4) == -1 && iwd_floor_div(-4, 4) == -1 && iwd_floor_div(3, 4) == 0);
    CHECK(iwd_mod(-1, 24) == 23 && iwd_mod(24, 24) == 0);

    /* buffer writers truncate on a UTF-8 boundary and always terminate */
    char b[8];
    memset(b, 'x', sizeof b);
    size_t n = iwd_fmt_minus(-123456, b, 4);      /* "−" is 3 bytes: only it fits */
    CHECK(n == 3 && strcmp(b, "\xE2\x88\x92") == 0);
    n = iwd_fmt_minus(-1, b, 3);                  /* no room for the 3-byte sign */
    CHECK(n == 0 && b[0] == 0);
    CHECK(iwd_fmt_hour(7, b, 0) == 0);
    CHECK(iwd_fmt_temp(-50, true, b, sizeof b) > 0 && strcmp(b, "\xE2\x88\x92" "46\xC2\xB0") == 0);
    char big[IWD_FORECAST_CAP];
    IwdForecast f = { 8, 27, 5, true, 2, 6 };
    size_t nl = 0;
    iwd_fmt_forecast(&f, big, sizeof big, &nl);
    CHECK(strcmp(big, "White Out at 03:00 tomorrow (in 5 h \xC2\xB7 2d4 rolled 6)") == 0 && nl == 9);
    char rs[IWD_RANDOM_SEED_CAP];
    CHECK(iwd_random_seed(14, 0xffff, rs, sizeof rs) == 15 && strcmp(rs, "TERMALAINE-FFFF") == 0);

    /* every hour's effects fit; the longest chill line fits its buffer */
    for (int id = 1; id <= 12; id++) {
        for (int h = 0; h < 24; h++) {
            IwdHourEffects fx;
            iwd_hour_effects(id, h, -260, true, &fx);
            CHECK(fx.count == iwd_event(id)->n_effects + 1 + (iwd_light_at(iwd_event(id), h).aurora_visible ||
                                                               iwd_light_at(iwd_event(id), h).level == IWD_LIGHT_DARK));
            CHECK(fx.count <= IWD_MAX_HOUR_EFFECTS);
            CHECK(strlen(fx.chill_text) < IWD_CHILL_TEXT_CAP - 1);
        }
    }

    /* chain invariants over a long run */
    IwdChain c;
    iwd_chain_init(&c, "RIME-00FF");
    IwdDay d, prev;
    for (int i = 0; i < IWD_MAX_DAYS; i++) {
        iwd_next_day(&c, &d);
        CHECK(d.day == i + 1 && d.n_segments >= 3 && d.n_segments <= IWD_MAX_SEGMENTS);
        CHECK(d.segments[0].start == 0 && d.segments[d.n_segments - 1].end == 24);
        for (int s = 1; s < d.n_segments; s++) CHECK(d.segments[s].start == d.segments[s - 1].end);
        if (i) CHECK(memcmp(&d.carried_in, &prev.carry_out, sizeof d.carried_in) == 0);
        prev = d;
    }
    printf("engine unit checks: %s (sizeof(IwdDay) = %zu bytes)\n", failures ? "FAILED" : "OK", sizeof(IwdDay));
    return failures ? 1 : 0;
}
