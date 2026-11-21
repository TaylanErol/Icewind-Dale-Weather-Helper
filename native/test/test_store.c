/* Host tests for store.c.
 *   test_store CASES_FILE TMPDIR
 * CASES_FILE comes from store_ref.js (the web app's own state code run in node). Checks:
 *   1. load / v1 migration / export / import parity with the web, byte for byte;
 *   2. ECMAScript Number::toString parity;
 *   3. a rich state built through the API survives save -> load (file shim, atomic write);
 *   4. actions, settings, events, notes and the derived texts;
 *   5. malformed, truncated and mutated inputs (run under ASan/UBSan by test_store.sh).
 */
#include "../src/store.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;

static char *tdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); g_checks++; if (strcmp(g_, w_) != 0) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d:\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__, g_, w_); } } while (0)

static const double NOW = 1763575445123.0;
static const char *ISO = "2026-11-19T18:04:05.123Z";

static char *unhex(const char *h, size_t *len)
{
    size_t n = strlen(h);
    while (n && (h[n - 1] == '\n' || h[n - 1] == '\r')) n--;
    char *o = malloc(n / 2 + 1);
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned v;
        sscanf(h + i, "%2x", &v);
        o[i / 2] = (char)v;
    }
    o[n / 2] = 0;
    *len = n / 2;
    return o;
}

static char *snap(const Almanac *a, bool native)
{
    AlmBuf b = {0};
    if (!alm_snapshot_json(a, native, &b)) { alm_buf_free(&b); return tdup("<oom>"); }
    return b.data;
}

/* "seed":"..." -> "seed":"*" (for cases where both sides roll a random seed) */
static void mask_seed(char *s)
{
    char *p = strstr(s, "\"seed\":\"");
    if (!p) return;
    p += 8;
    char *q = strchr(p, '"');
    if (!q) return;
    *p = '*';
    memmove(p + 1, q, strlen(q) + 1);
}

static void first_diff(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    size_t st = i > 60 ? i - 60 : 0;
    fprintf(stderr, "  first difference at byte %zu\n  got  ...%.160s\n  want ...%.160s\n", i, a + st, b + st);
}

static void run_cases(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); g_fail++; return; }
    size_t cap = 1 << 24;
    char *line = malloc(cap), name[128] = "", mode[16] = "";
    char *in = NULL;
    size_t in_len = 0;
    int ncases = 0, nnums = 0;
    while (fgets(line, (int)cap, f)) {
        if (strncmp(line, "CASE ", 5) == 0) {
            sscanf(line + 5, "%127s %15s", name, mode);
        } else if (strncmp(line, "IN ", 3) == 0) {
            free(in);
            in = unhex(line + 3, &in_len);
        } else if (strncmp(line, "OUT ", 4) == 0) {
            ncases++;
            Almanac a;
            CHECK(alm_init(&a, 42), "init");
            bool not_backup = strncmp(line + 4, "NOT_BACKUP", 10) == 0;
            size_t want_len = 0;
            char *want = not_backup ? tdup("") : unhex(line + 4, &want_len);
            char *got = NULL;
            if (strcmp(mode, "import") == 0) {
                AlmImportStatus st = alm_import_json(&a, in, in_len, NOW);
                if (not_backup) CHECK(st == ALM_IMPORT_NOT_BACKUP, "%s: import status %d", name, (int)st);
                else { CHECK(st == ALM_IMPORT_OK, "%s: import status %d", name, (int)st); got = snap(&a, false); }
            } else {
                AlmLoadStatus st = alm_load_json(&a, in, in_len);
                if (strcmp(mode, "v1") == 0) CHECK(st == ALM_LOAD_MIGRATED_V1, "%s: status %d", name, (int)st);
                else CHECK(st == ALM_LOAD_OK, "%s: status %d", name, (int)st);
                if (strcmp(mode, "export") == 0) {
                    AlmBuf b = {0};
                    CHECK(alm_export_json(&a, ISO, &b), "export");
                    got = b.data;
                } else {
                    got = snap(&a, false);
                }
            }
            if (got) {
                if (strcmp(name, "empty_obj_v2") == 0) { mask_seed(got); mask_seed(want); }
                g_checks++;
                if (strcmp(got, want) != 0) {
                    g_fail++;
                    fprintf(stderr, "FAIL case %s (%s): output differs from the web\n", name, mode);
                    first_diff(got, want);
                } else if (strcmp(mode, "export") != 0 && strcmp(name, "empty_obj_v2") != 0 && strcmp(name, "started_inf") != 0) {
                    /* (started_inf: Infinity is written as null, exactly as the web does) */
                    /* idempotence: loading our own output gives it back */
                    Almanac b;
                    alm_init(&b, 7);
                    CHECK(alm_load_json(&b, got, strlen(got)) == ALM_LOAD_OK, "%s: reload", name);
                    char *again = snap(&b, false);
                    CHECK(strcmp(again, got) == 0, "%s: reload not idempotent", name);
                    free(again);
                    alm_free(&b);
                } else if (strcmp(mode, "export") == 0) {
                    /* export -> import round trip keeps the snapshot */
                    Almanac b;
                    alm_init(&b, 7);
                    CHECK(alm_import_json(&b, got, strlen(got), NOW) == ALM_IMPORT_OK, "%s: import export", name);
                    char *s1 = snap(&a, false), *s2 = snap(&b, false);
                    CHECK(strcmp(s1, s2) == 0, "%s: export/import round trip", name);
                    free(s1); free(s2);
                    alm_free(&b);
                }
            }
            free(got);
            free(want);
            alm_free(&a);
        } else if (strncmp(line, "NUM ", 4) == 0) {
            char e[64], js[64], buf[ALM_NUM_CAP];
            if (sscanf(line + 4, "%63s %63s", e, js) == 2) {
                nnums++;
                double x = strtod(e, NULL);
                alm_fmt_js_number(x, buf, sizeof buf);
                g_checks++;
                if (strcmp(buf, js) != 0) { g_fail++; fprintf(stderr, "FAIL number %s: got %s want %s\n", e, buf, js); }
            }
        }
    }
    free(in);
    free(line);
    fclose(f);
    printf("  web parity: %d state cases, %d numbers\n", ncases, nnums);
    CHECK(ncases > 50 && nnums > 1000, "fixture file incomplete");
}

/* ---------------------------------------------------------------------------------- */

static void test_round_trip(const char *tmpdir)
{
    Almanac a;
    CHECK(alm_init(&a, 1234), "init");
    CHECK(a.current == -1 && a.view == -1 && a.n_days == 0 && a.time == 23, "fresh state");
    CHECK(a.seed && a.seed[0], "fresh seed");
    CHECK(a.ui.cal_year == 1489 && a.ui.cal_month == 0, "fresh calendar");
    free(a.seed);
    a.seed = tdup("RIME-00FF");
    iwd_chain_init(&a.chain, a.seed);
    a.temp_hash = iwd_temp_hash(a.seed);

    AlmResult r = alm_advance(&a, 12);
    CHECK(!(r.fx & ALM_FX_REJECTED) && a.current == 0 && a.view == 0 && a.n_days == 1 && a.time == 12, "begin");
    CHECK(strcmp(r.toast_lead, "The expedition begins") == 0, "lead");
    for (int i = 0; i < 9; i++) alm_advance(&a, 12);
    CHECK(a.current == 9 && a.n_days == 10 && a.current_abs == IWD_DEFAULT_START_ABS + 9, "advance x10");

    bool upd;
    r = alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 3, "  Yeti <attack> & co  ", ALM_EV_UNNATURAL,
                       IWD_REPEAT_TENDAY, "  north pass  ", NOW, &upd);
    CHECK(!upd && (r.fx & ALM_FX_SAVE) && a.n_events == 1, "event add");
    CHECK_STR(r.toast, "<b>Yeti &lt;attack&gt; &amp; co</b> added to the almanac");
    CHECK_STR(a.events[0].title, "Yeti <attack> & co");
    CHECK_STR(a.events[0].details, "north pass");
    char *id = tdup(a.events[0].id);
    CHECK(id[0] == 'e' && strlen(id) > 5, "event id %s", id);
    r = alm_event_save(&a, id, 0, "Yeti attack", ALM_EV_AURIL, IWD_REPEAT_YEARLY, "", NOW, &upd);
    CHECK(upd && a.n_events == 1 && a.events[0].abs == IWD_DEFAULT_START_ABS + 3, "event update keeps anchor");
    CHECK_STR(r.toast, "<b>Yeti attack</b> updated");
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 5, "Second \xF0\x9F\xA7\x8A", ALM_EV_REMINDER, IWD_REPEAT_NONE, NULL, NOW, &upd);
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 5, "Third", ALM_EV_NATURAL, IWD_REPEAT_NONE, "x\ny", NOW, &upd);
    CHECK(a.n_events == 3 && strcmp(a.events[1].id, a.events[2].id) != 0, "unique ids");
    r = alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS, " \t ", ALM_EV_NATURAL, IWD_REPEAT_NONE, NULL, NOW, &upd);
    CHECK(r.fx & ALM_FX_REJECTED, "blank title rejected");
    {
        /* 'a' + 60 x U+1F9CA = 121 UTF-16 units: the cut at 120 drops the split pair whole */
        char longt[256] = "a";
        for (int i = 0; i < 60; i++) strcat(longt, "\xF0\x9F\xA7\x8A");
        alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS, longt, ALM_EV_NATURAL, IWD_REPEAT_NONE, NULL, NOW, &upd);
        CHECK(strlen(a.events[a.n_events - 1].title) == 1 + 59 * 4, "surrogate-safe cut");
        alm_event_delete(&a, a.events[a.n_events - 1].id);
        CHECK(a.n_events == 3, "delete by the event's own id pointer");
    }

    alm_note_set(&a, IWD_DEFAULT_START_ABS + 2, "Rumours of a \"white dragon\"\n\xE2\x9D\x84");
    alm_note_set(&a, -77, "far past");
    alm_note_set(&a, IWD_DEFAULT_START_ABS + 1, "temp");
    alm_note_set(&a, IWD_DEFAULT_START_ABS + 1, "   \n ");
    CHECK(a.notes.count == 2, "note delete on blank");
    CHECK_STR(alm_note_get(&a, IWD_DEFAULT_START_ABS + 1), "");
    CHECK(alm_year_name_set(&a, 1489, "  Year of the Warrior Princess  "), "yn");
    CHECK_STR(alm_year_name(&a, 1489), "Year of the Warrior Princess");
    alm_set_unit(&a, true);
    a.moon_offset = 2.5;
    a.started = NOW - 5000;
    a.native.sound = true;
    a.native.interior = true;
    a.native.has_rect = true;
    a.native.win_x = -8; a.native.win_y = 20; a.native.win_w = 1280; a.native.win_h = 760;
    a.native.maximized = true;
    a.native.ledger = true;
    alm_step_day(&a, -3);
    CHECK(a.view == 6 && a.current == 9, "step day");
    alm_set_time(&a, 7.25);

    char path[2100], dir[1024];
    snprintf(dir, sizeof dir, "%s/deep/er/dir", tmpdir);
    CHECK(alm_fs_mkdirs(dir) == ALM_IO_OK, "mkdirs");
    CHECK(alm_fs_mkdirs(dir) == ALM_IO_OK, "mkdirs again");
    snprintf(path, sizeof path, "%s/almanac.json", dir);
    CHECK(alm_save_file(&a, path) == ALM_IO_OK, "save");
    CHECK(alm_save_file(&a, path) == ALM_IO_OK, "save over existing (atomic replace)");
    CHECK(alm_save_file_ex(&a, path, false) == ALM_IO_OK, "non-durable (debounced) save");
    char tmpf[2200];
    snprintf(tmpf, sizeof tmpf, "%s.tmp", path);
    FILE *tf = fopen(tmpf, "rb");
    CHECK(tf == NULL, "no temp file left behind");
    if (tf) fclose(tf);

    Almanac b;
    alm_init(&b, 99);
    CHECK(alm_load_file(&b, path) == ALM_LOAD_OK, "load");
    char *s1 = snap(&a, true), *s2 = snap(&b, true);
    CHECK_STR(s2, s1);
    CHECK(b.native.sound && b.native.interior && b.native.has_rect && b.native.win_x == -8 && b.native.win_w == 1280 && b.native.maximized &&
          b.native.ledger, "native prefs");
    CHECK(b.n_days == 10 && memcmp(a.days, b.days, sizeof(IwdDay) * 10) == 0, "days rebuilt identically");
    CHECK(b.chain.rng_state == a.chain.rng_state, "chain state");
    free(s1); free(s2);

    /* export never carries "native"; import keeps the importer's native prefs */
    AlmBuf ex = {0};
    CHECK(alm_export_json(&a, ISO, &ex), "export");
    CHECK(strstr(ex.data, "\"native\"") == NULL && strstr(ex.data, "ledger") == NULL &&
          strstr(ex.data, "interior") == NULL, "export has no native");
    CHECK(strncmp(ex.data, "{\n  \"app\": \"icewind-dale-almanac\",\n  \"exported\": \"2026-11-19T18:04:05.123Z\",\n  \"v\": 2,", 80) == 0,
          "export head");
    Almanac c;
    alm_init(&c, 5);
    c.native.ledger = true;
    c.busy = true;
    CHECK(alm_import_json(&c, ex.data, ex.len, NOW) == ALM_IMPORT_BUSY, "import busy");
    c.busy = false;
    CHECK(alm_import_json(&c, ex.data, ex.len, NOW) == ALM_IMPORT_OK, "import");
    CHECK(c.native.ledger && !c.native.sound && !c.native.interior, "import keeps native prefs");
    s1 = snap(&a, false); s2 = snap(&c, false);
    CHECK_STR(s2, s1);
    free(s1); free(s2);
    char toast[ALM_TOAST_CAP];
    alm_fmt_import_toast(&c, toast, sizeof toast);
    CHECK_STR(toast, "Backup restored \xC2\xB7 10 days, 3 events");

    char fn[128];
    free(c.seed);
    c.seed = tdup("A:B/C*\x01?");
    alm_export_filename(&c, 2026, 3, 7, fn, sizeof fn);
    CHECK_STR(fn, "icewind-almanac-A_B_C___-20260307.json");

    /* missing / unreadable files */
    char missing[1100];
    snprintf(missing, sizeof missing, "%s/nope.json", tmpdir);
    CHECK(alm_load_file(&c, missing) == ALM_LOAD_NO_FILE && c.n_days == 0 && c.current == -1, "no file");
    CHECK(alm_import_file(&c, missing, NOW) == ALM_IMPORT_READ_ERROR, "import missing");
    snprintf(missing, sizeof missing, "%s/no/such/dir/x.json", tmpdir);
    CHECK(alm_fs_write_atomic(missing, "{}", 2) == ALM_IO_ERROR, "write into a missing directory fails");
    CHECK(alm_import_file(&c, path, NOW) == ALM_IMPORT_OK && c.n_days == 10, "import save file");

    alm_buf_free(&ex);
    free(id);
    alm_free(&a);
    alm_free(&b);
    alm_free(&c);
}

static Almanac fresh(const char *seed, int days)
{
    Almanac a;
    alm_init(&a, 77);
    char json[256];
    snprintf(json, sizeof json, "{\"v\":2,\"seed\":\"%s\",\"dayCount\":%d}", seed, days);
    alm_load_json(&a, json, strlen(json));
    return a;
}

static void test_actions(void)
{
    char buf[512];
    Almanac a = fresh("RIME-00FF", 0);
    AlmHero h;
    AlmControls ct;
    alm_hero(&a, &h);
    CHECK_STR(h.date, "1 Hammer 1489 DR");
    CHECK_STR(h.tag, "Expedition start");
    CHECK_STR(h.day_label, "Before the First Dawn");
    alm_controls(&a, &ct);
    CHECK_STR(ct.main, "Begin the Expedition");
    CHECK(ct.advance_enabled && !ct.prev_enabled && !ct.next_enabled, "intro controls");
    CHECK(alm_fmt_announce(&a, buf, sizeof buf) == 0, "no announce in the intro");
    CHECK(alm_step_minutes(&a, 60).fx & ALM_FX_REJECTED, "no time step in the intro");

    AlmResult r = alm_advance(&a, 12);
    CHECK(a.current == 0 && a.time == 12, "begin");
    alm_controls(&a, &ct);
    CHECK_STR(ct.main, "Advance to 2 Hammer");
    CHECK(!ct.prev_enabled && ct.next_enabled, "day 1 navigator");
    alm_hero(&a, &h);
    CHECK_STR(h.tag, "Today");
    CHECK_STR(h.day_label, "Day 1 of the expedition");
    alm_fmt_announce(&a, buf, sizeof buf);
    CHECK(strncmp(buf, "1 Hammer 1489 DR, day 1, 12:00: ", 32) == 0, "announce %s", buf);

    /* hour stepping */
    r = alm_step_minutes(&a, 60);
    CHECK(a.time == 13 && (r.fx & ALM_FX_SAVE) && (r.fx & ALM_FX_ANNOUNCE), "hour +1");
    alm_set_time(&a, 23);
    r = alm_step_minutes(&a, 60);
    CHECK(a.current == 1 && a.view == 1 && a.time == 0 && a.n_days == 2, "23:00 + 1 h on today advances, lands 00:00");
    CHECK(strcmp(r.toast_lead, "Today") == 0, "lead today");
    r = alm_step_minutes(&a, -60);
    CHECK(a.view == 0 && a.time == 23 && a.current == 1, "00:00 - 1 h goes to the previous day 23:00");
    r = alm_step_minutes(&a, 60);
    CHECK(a.view == 1 && a.time == 0 && a.current == 1 && a.n_days == 2, "23:00 + 1 h on a past day moves on");
    alm_step_day(&a, -1);
    CHECK(a.view == 0, "step day back");
    alm_set_time(&a, 0.5);
    r = alm_step_minutes(&a, -60);
    CHECK((r.fx & ALM_FX_REJECTED) && a.view == 0 && a.time == 0.5, "no hour before day 1");
    CHECK(alm_step_minutes(&a, 0).fx & ALM_FX_REJECTED, "delta 0 rejected");

    /* select / set current */
    r = alm_select(&a, IWD_DEFAULT_START_ABS + 5, true);
    CHECK(a.view == 5 && a.n_days == 6 && a.current == 1 && a.sel_abs == IWD_DEFAULT_START_ABS + 5, "select rolls ahead");
    alm_hero(&a, &h);
    CHECK_STR(h.tag, "In 4 days");
    CHECK(h.tag_kind == ALM_TAG_AHEAD, "ahead");
    r = alm_select(&a, IWD_DEFAULT_START_ABS + 4000, false);
    CHECK_STR(r.toast, "Beyond the almanac: at most 3650 days of weather can be recorded.");
    CHECK(a.view == 5 && a.sel_abs == IWD_DEFAULT_START_ABS + 4000, "beyond keeps the view");
    AlmDayPanel dp;
    alm_day_panel(&a, &dp);
    CHECK_STR(dp.weather_none, "Beyond the almanac: at most 3650 days can be recorded.");
    CHECK(!dp.show_set_current, "no set current beyond");
    r = alm_set_current(&a, IWD_DEFAULT_START_ABS - 1);
    CHECK_STR(r.toast, "The expedition begins on <b>1 Hammer 1489 DR</b>. Change the start date in Settings to begin earlier.");
    r = alm_set_current(&a, IWD_DEFAULT_START_ABS + 3);
    CHECK(a.current == 3 && a.view == 3 && strcmp(r.toast_lead, "Today is now") == 0, "set current");
    CHECK((r.fx & ALM_FX_DAY_TOAST) || strcmp(r.toast, "Today is now <b>4 Hammer 1489 DR</b>") == 0, "set current toast %s", r.toast);
    alm_day_panel(&a, &dp);
    CHECK_STR(dp.eyebrow, "Today \xC2\xB7 Day 4 of the expedition");
    CHECK_STR(dp.set_current_label, "This is the current day");
    CHECK(dp.weather && dp.wx_seq[0] && strncmp(dp.wx_cold, "DC ", 3) == 0, "day weather");
    alm_select(&a, IWD_DEFAULT_START_ABS + 1, false);
    alm_day_panel(&a, &dp);
    CHECK_STR(dp.eyebrow, "Day 2 of the expedition \xC2\xB7 2 days ago");
    CHECK_STR(dp.sub, "Hammer \xE2\x80\x94 Deepwinter \xC2\xB7 First tenday, day 2");
    CHECK(strncmp(dp.moon_name, "Sel\xC3\xBBne \xC2\xB7 ", 10) == 0, "moon name");

    /* festival day panel + year name */
    alm_year_name_set(&a, 1489, "Year of the Warrior Princess");
    alm_select(&a, IWD_DEFAULT_START_ABS + 30, false);   /* Midwinter */
    alm_day_panel(&a, &dp);
    CHECK_STR(dp.date, "Midwinter, 1489 DR");
    CHECK_STR(dp.sub, "Festival day \xC2\xB7 after Hammer \xC2\xB7 Year of the Warrior Princess");
    CHECK(a.ui.cal_year == 1489 && a.ui.cal_month == 0, "festival shown in its month");
    AlmCell cells[32];
    int nc = alm_month_cells(&a, cells);
    CHECK(nc == 31 && cells[30].is_festival && strcmp(cells[30].festival_name, "Midwinter") == 0, "cells %d", nc);
    CHECK(cells[0].is_start && strcmp(cells[0].tag, "Start") == 0 && cells[3].is_today && strcmp(cells[3].tag, "Today") == 0,
          "cell tags");
    CHECK(cells[30].is_sel && cells[0].has_weather && cells[29].has_weather && a.n_days == 31, "cell states (select rolled up to the festival)");
    alm_fmt_cell_label(&a, &cells[30], buf, sizeof buf);
    CHECK(strncmp(buf, "Midwinter, 1489 DR \xC2\xB7 ", 21) == 0 && !strstr(buf, ", festival"), "label %s", buf);

    /* calendar paging */
    for (int i = 0; i < 12; i++) alm_cal_step(&a, 1);
    CHECK(a.ui.cal_year == 1490 && a.ui.cal_month == 0, "page a year");
    alm_cal_step(&a, -1);
    CHECK(a.ui.cal_year == 1489 && a.ui.cal_month == 11, "page back");
    alm_cal_today(&a);
    CHECK(a.sel_abs == IWD_DEFAULT_START_ABS + 3 && a.ui.cal_month == 0, "today button");

    /* events on days, marks, upcoming, meta */
    bool upd;
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 3, "Reminder late", ALM_EV_REMINDER, IWD_REPEAT_NONE, "", NOW, &upd);
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 3, "Auril late", ALM_EV_AURIL, IWD_REPEAT_NONE, "", NOW + 1, &upd);
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS, "Auril early", ALM_EV_AURIL, IWD_REPEAT_TENDAY, "", NOW + 2, &upd);
    const AlmEvent *evs[8];
    int ne = alm_events_on(&a, IWD_DEFAULT_START_ABS + 10, evs, 8);
    CHECK(ne == 1 && strcmp(evs[0]->title, "Auril early") == 0, "tenday repeat");
    ne = alm_events_on(&a, IWD_DEFAULT_START_ABS + 3, evs, 8);
    CHECK(ne == 2 && strcmp(evs[0]->title, "Auril late") == 0 && strcmp(evs[1]->title, "Reminder late") == 0, "priority sort");
    AlmMark mk[8];
    int nm = alm_marks_on(&a, IWD_DEFAULT_START_ABS + 10, mk, 8);
    alm_fmt_mark_meta(&mk[nm - 1], IWD_DEFAULT_START_ABS + 10, buf, sizeof buf);
    CHECK_STR(buf, "Auril \xC2\xB7 Every tenday \xC2\xB7 since 1 Hammer 1489");
    nm = alm_marks_on(&a, IWD_DEFAULT_START_ABS + 3, mk, 8);
    alm_fmt_mark_meta(&mk[nm - 1], IWD_DEFAULT_START_ABS + 3, buf, sizeof buf);
    CHECK_STR(buf, "Reminder \xC2\xB7 One-time");
    AlmUpcoming up[ALM_UPCOMING_DAYS + 1];
    int nu = alm_upcoming(&a, up);
    CHECK(nu >= 2 && up[0].abs == IWD_DEFAULT_START_ABS + 3 && up[0].rel_days == 0, "upcoming");
    /* a new moon day carries the built-in mark first */
    int32_t nmd = 0;
    for (int32_t d = IWD_DEFAULT_START_ABS; d < IWD_DEFAULT_START_ABS + 40; d++)
        if (iwd_moon(d, a.moon_offset).is_new) { nmd = d; break; }
    CHECK(nmd != 0, "new moon found");
    nm = alm_marks_on(&a, nmd, mk, 8);
    CHECK(nm >= 1 && mk[0].builtin && strcmp(mk[0].chip, "Sacrifice to Auril") == 0, "builtin first");
    char title[128];
    int nt = 0;
    CHECK(alm_day_toast(&a, nmd, "Today", title, sizeof title, mk, 8, &nt) && nt == nm, "day toast");
    r = alm_event_delete(&a, evs[0]->id);
    CHECK_STR(r.toast, "<b>Auril late</b> removed");

    /* settings */
    AlmSettingsForm f;
    alm_settings_fill(&a, &f);
    CHECK(f.start_year == 1489 && f.start_option == 0 && f.start_day == 1 && strcmp(f.seed, "RIME-00FF") == 0, "fill");
    CHECK_STR(f.yn_name, "Year of the Warrior Princess");
    f.start_year_valid = false;
    AlmApplyResult ar = alm_settings_apply(&a, &f, false);
    CHECK(ar.status == ALM_APPLY_DATE_ERROR, "no year");
    CHECK_STR(ar.message, "Enter a year.");
    f.start_year_valid = true;
    f.start_day = 31;
    ar = alm_settings_apply(&a, &f, false);
    CHECK_STR(ar.message, "Months have 30 days: pick a day from 1 to 30.");
    f.start_day = 1;
    f.start_option = alm_start_option_index(-1, IWD_FEST_SHIELDMEET);
    CHECK(f.start_option == 10, "shieldmeet option");
    f.start_year = 1489;
    ar = alm_settings_apply(&a, &f, false);
    CHECK_STR(ar.message, "Shieldmeet only falls in leap years; 1489 DR is not one.");
    f.start_option = 0;
    f.start_day = 2;
    snprintf(f.seed, sizeof f.seed, "  frost-<1>  ");
    ar = alm_settings_apply(&a, &f, false);
    CHECK(ar.status == ALM_APPLY_NEEDS_CONFIRM, "confirm");
    CHECK_STR(ar.message, "<b>Regenerate the weather?</b> All recorded weather is replaced and rolled again from "
                          "<b>2 Hammer 1489 DR</b> with seed <code>FROST-&lt;1&gt;</code>, up to the party's current day, "
                          "<b>4 Hammer 1489 DR</b>. Notes and events are kept.");
    CHECK(strcmp(a.seed, "RIME-00FF") == 0, "nothing applied before the confirm");
    f.now_ms = NOW;
    f.celsius = true;
    f.moon_offset = 99;
    snprintf(f.yn_name, sizeof f.yn_name, "  ");
    ar = alm_settings_apply(&a, &f, true);
    CHECK(ar.status == ALM_APPLY_DONE, "apply");
    CHECK_STR(ar.result.toast, "Weather regenerated from <b>2 Hammer 1489 DR</b> \xC2\xB7 seed FROST-&lt;1&gt;");
    CHECK(strcmp(a.seed, "FROST-<1>") == 0 && a.start_abs == IWD_DEFAULT_START_ABS + 1 && a.current == 2 &&
          a.n_days == 3 && a.current_abs == IWD_DEFAULT_START_ABS + 3 && a.started == NOW, "regenerated");
    CHECK(a.celsius && a.moon_offset == 30 && alm_map_get(&a.year_names, 1489) == NULL, "settings applied");
    CHECK(a.n_events == 2 && a.notes.count == 0, "events kept");
    /* new start after today: expedition not begun, currentAbs kept */
    alm_settings_fill(&a, &f);
    f.start_day = 20;
    ar = alm_settings_apply(&a, &f, false);
    CHECK(strstr(ar.message, "The party's current day falls before the new start, so the expedition will begin on "
                             "<b>20 Hammer 1489 DR</b>.") != NULL, "confirm before %s", ar.message);
    ar = alm_settings_apply(&a, &f, true);
    CHECK(a.current == -1 && a.view == -1 && a.n_days == 0 && a.time == 23 && a.has_current_abs &&
          a.current_abs == IWD_DEFAULT_START_ABS + 3 && a.sel_abs == IWD_DEFAULT_START_ABS + 19, "re-roll after today");
    /* no days -> no confirmation */
    alm_settings_fill(&a, &f);
    snprintf(f.seed, sizeof f.seed, "sleet-0001");
    ar = alm_settings_apply(&a, &f, false);
    CHECK(ar.status == ALM_APPLY_DONE && strcmp(a.seed, "SLEET-0001") == 0, "no confirm without days");
    snprintf(f.seed, sizeof f.seed, "   ");
    ar = alm_settings_apply(&a, &f, false);
    CHECK(ar.status == ALM_APPLY_DONE && strlen(a.seed) >= 8 && strchr(a.seed, '-'), "random seed %s", a.seed);

    char up8[64];
    alm_normalize_seed("  \xC3\xA9t\xC3\xA9 \xC3\x9F \xCF\x83 \xD0\xB6 \xC4\x87 ", up8, sizeof up8);
    CHECK_STR(up8, "\xC3\x89T\xC3\x89 SS \xCE\xA3 \xD0\x96 \xC4\x86");
    /* full case mapping like the web's toUpperCase(): expansions and digraphs */
    alm_normalize_seed("stra\xC3\x9F" "e \xEF\xAC\x81re \xC7\x86" "emal \xC5\x89", up8, sizeof up8);
    CHECK_STR(up8, "STRASSE FIRE \xC7\x84" "EMAL \xCA\xBCN");
    snprintf(f.seed, sizeof f.seed, " stra\xC3\x9F" "e ");
    ar = alm_settings_apply(&a, &f, false);
    CHECK(ar.status == ALM_APPLY_DONE && strcmp(a.seed, "STRASSE") == 0, "seed straße -> %s", a.seed);
    {   /* 40 x U+0390 (80 bytes) triples to 240 bytes and must not be cut */
        char big[4 * ALM_SEED_INPUT_MAX + 1] = "";
        for (int i = 0; i < 40; i++) strcat(big, "\xCE\x90");
        snprintf(f.seed, sizeof f.seed, "%s", big);
        ar = alm_settings_apply(&a, &f, false);
        CHECK(ar.status == ALM_APPLY_DONE && strlen(a.seed) == 240, "tripled seed kept whole (%zu)", strlen(a.seed));
    }

    /* the almanac fills up */
    Almanac full = fresh("FULL", 3650);
    CHECK(full.current == 3649, "full");
    alm_controls(&full, &ct);
    CHECK_STR(ct.main, "The Almanac Is Full");
    CHECK(!ct.advance_enabled && !ct.next_enabled, "full controls");
    CHECK(alm_advance(&full, 12).fx & ALM_FX_REJECTED, "cannot advance a full almanac");
    alm_set_time(&full, 23);
    CHECK(alm_step_minutes(&full, 60).fx & ALM_FX_REJECTED, "no hour past the last day");
    alm_set_time(&full, 1420 / 60.0);
    r = alm_step_minutes(&full, 30);
    CHECK((r.fx & ALM_FX_REJECTED) && full.current == 3649 && alm_minute_now(&full) == 1420,
          "full almanac: 23:40 + 30 min rejected, unchanged");
    alm_free(&full);

    alm_free(&a);
}

/* minute stepping (SPEC 6.2): exact minutes / 60.0, midnight in both directions */
static void test_minutes(const char *tmpdir)
{
    Almanac a = fresh("MINUTES", 3);
    AlmResult r;
    CHECK(a.current == 2 && a.view == 2 && a.time == 12 && alm_minute_now(&a) == 720, "fresh at 12:00");
    r = alm_step_minutes(&a, 5);
    CHECK(a.time == 725 / 60.0 && alm_minute_now(&a) == 725 && (r.fx & ALM_FX_SAVE) && (r.fx & ALM_FX_RENDER_HOUR),
          "12:00 + 5 min (%.17g)", a.time);
    r = alm_step_minutes(&a, 30);
    CHECK(a.time == 755 / 60.0 && alm_minute_now(&a) == 755, "+ 30 min");
    r = alm_step_minutes(&a, -5);
    CHECK(a.time == 750 / 60.0 && alm_minute_now(&a) == 750 && alm_hour_now(&a) == 12, "- 5 min");
    for (int m = 0; m < 1440; m++) {          /* every stored minute reads back as itself */
        alm_set_time(&a, m / 60.0);
        if (alm_minute_now(&a) != m || alm_hour_now(&a) != m / 60) { CHECK(0, "minute %d reads %d", m, alm_minute_now(&a)); break; }
    }

    /* forward past midnight on today: an advance landing at the carried-over minute */
    alm_set_time(&a, 1420 / 60.0);
    r = alm_step_minutes(&a, 30);
    CHECK(a.current == 3 && a.view == 3 && a.n_days == 4 && alm_minute_now(&a) == 10 && a.time == 10 / 60.0,
          "23:40 + 30 on today -> next day 00:10");
    CHECK(r.toast_lead && strcmp(r.toast_lead, "Today") == 0 && (r.fx & ALM_FX_SAVE), "advance result");
    CHECK(a.sel_abs == alm_abs_of(&a, 3) && a.current_abs == alm_abs_of(&a, 3), "today and the selection moved");

    /* backward past midnight on today: only the view changes */
    r = alm_step_minutes(&a, -30);
    CHECK(a.view == 2 && a.current == 3 && alm_minute_now(&a) == 1420 && a.time == 1420 / 60.0,
          "00:10 - 30 on today -> yesterday 23:40, today unchanged");
    CHECK(a.sel_abs == alm_abs_of(&a, 2), "selection follows the view");

    /* forward past midnight on a past day: on to the next day, today unchanged */
    r = alm_step_minutes(&a, 30);
    CHECK(a.view == 3 && a.current == 3 && a.n_days == 4 && alm_minute_now(&a) == 10, "23:40 + 30 on a past day");
    alm_select(&a, alm_abs_of(&a, 1), false);
    alm_set_time(&a, 1380 / 60.0);
    r = alm_step_minutes(&a, 60);
    CHECK(a.view == 2 && a.current == 3 && a.time == 0, "23:00 + 1 h on a past day lands 00:00");

    /* day 1 cannot go back before 00:00 */
    alm_select(&a, a.start_abs, false);
    alm_set_time(&a, 10 / 60.0);
    r = alm_step_minutes(&a, -30);
    CHECK((r.fx & ALM_FX_REJECTED) && a.view == 0 && a.current == 3 && alm_minute_now(&a) == 10,
          "day 1 00:10 - 30 rejected, unchanged");
    r = alm_step_minutes(&a, -5);
    CHECK(!(r.fx & ALM_FX_REJECTED) && alm_minute_now(&a) == 5, "day 1 00:10 - 5 stays on the day");
    CHECK(alm_step_minutes(&a, 1440).fx & ALM_FX_REJECTED, "a whole day is not a step");

    /* a legacy fractional time from a web backup */
    alm_set_time(&a, 13.7283);
    CHECK(alm_minute_now(&a) == 823 && alm_hour_now(&a) == 13, "13.7283 reads 13:43 (%d)", alm_minute_now(&a));
    r = alm_step_minutes(&a, 5);
    CHECK(alm_minute_now(&a) == 828 && a.time == 828 / 60.0, "13:43 + 5 -> 13:48 exactly");
    alm_set_time(&a, 23.9999);
    CHECK(alm_minute_now(&a) == 1439, "23.9999 reads 23:59");

    /* busy */
    a.busy = true;
    CHECK(alm_step_minutes(&a, 5).fx & ALM_FX_REJECTED, "busy rejects");
    a.busy = false;

    /* save / load keeps the exact minute and the ledger preference; "tab" is gone */
    alm_set_time(&a, 725 / 60.0);
    a.native.ledger = true;
    char path[1100];
    snprintf(path, sizeof path, "%s/minutes.json", tmpdir);
    CHECK(alm_save_file(&a, path) == ALM_IO_OK, "save minutes");
    Almanac b;
    alm_init(&b, 4);
    CHECK(alm_load_file(&b, path) == ALM_LOAD_OK, "load minutes");
    CHECK(b.time == 725 / 60.0 && alm_minute_now(&b) == 725 && b.native.ledger, "time 725/60 and ledger survive");
    AlmBuf sb = {0};
    alm_snapshot_json(&b, true, &sb);
    CHECK(strstr(sb.data, "\"ledger\":true") != NULL && strstr(sb.data, "\"tab\"") == NULL, "native.ledger written");
    alm_buf_free(&sb);
    AlmBuf ex = {0};
    alm_export_json(&b, ISO, &ex);
    CHECK(strstr(ex.data, "\"native\"") == NULL && strstr(ex.data, "ledger") == NULL, "ledger never exported");
    alm_buf_free(&ex);
    alm_free(&b);
    const char *old = "{\"v\":2,\"seed\":\"X\",\"dayCount\":1,\"native\":{\"sound\":true,\"tab\":3}}";
    alm_init(&b, 4);
    CHECK(alm_load_json(&b, old, strlen(old)) == ALM_LOAD_OK && !b.native.ledger && b.native.sound, "old tab key ignored");
    alm_free(&b);
    const char *on = "{\"v\":2,\"seed\":\"X\",\"native\":{\"ledger\":1}}";
    alm_init(&b, 4);
    CHECK(alm_load_json(&b, on, strlen(on)) == ALM_LOAD_OK && !b.native.ledger, "ledger must be true, not truthy");
    alm_free(&b);
    alm_free(&a);
}

/* ---------------------------------------------------------------------------------- */

static uint64_t rs = 0x1234567;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

static void check_valid(const Almanac *a, const char *what)
{
    bool ok = a->seed && a->seed[0] && a->n_days >= 0 && a->n_days <= IWD_MAX_DAYS && a->current >= -1 &&
              a->current < a->n_days && a->view >= -1 && a->view < a->n_days && a->time >= 0 && a->time < 24 &&
              a->ui.cal_month >= 0 && a->ui.cal_month < 12 && a->moon_offset >= -30 && a->moon_offset <= 30;
    for (int i = 0; ok && i < a->n_events; i++)
        ok = a->events[i].id && a->events[i].id[0] && a->events[i].title && a->events[i].title[0] && a->events[i].details;
    g_checks++;
    if (!ok) { g_fail++; fprintf(stderr, "FAIL invalid state after %s\n", what); }
}

static void exercise(Almanac *a)
{
    /* every derived view must be safe on any loaded state */
    char buf[1024];
    AlmHero h; AlmControls c; AlmDayPanel d; AlmCell cells[32]; AlmUpcoming up[ALM_UPCOMING_DAYS + 1];
    alm_hero(a, &h);
    alm_controls(a, &c);
    alm_day_panel(a, &d);
    int n = alm_month_cells(a, cells);
    for (int i = 0; i < n; i++) alm_fmt_cell_label(a, &cells[i], buf, sizeof buf);
    alm_upcoming(a, up);
    alm_fmt_announce(a, buf, sizeof buf);
    AlmBuf b = {0};
    alm_snapshot_json(a, true, &b);
    Almanac x;
    alm_init(&x, 3);
    alm_load_json(&x, b.data, b.len);
    AlmBuf b2 = {0};
    alm_snapshot_json(&x, true, &b2);
    g_checks++;
    if (strcmp(b.data, b2.data) != 0) { g_fail++; fprintf(stderr, "FAIL fuzz: save/load not stable\n"); first_diff(b2.data, b.data); }
    alm_buf_free(&b);
    alm_buf_free(&b2);
    alm_free(&x);
}

static void test_malformed(void)
{
    static const char *bad[] = {
        "", " ", "{", "}", "[", "{\"v\":2,", "{\"v\":2,\"seed\":\"x\"", "{\"v\":2 \"seed\":1}", "{\"v\":2,}",
        "{'v':2}", "{\"v\":2,\"seed\":\"a\nb\"}", "{\"v\":2,\"seed\":\"\\x\"}", "{\"v\":2,\"seed\":\"\\u12\"}",
        "{\"v\":01}", "{\"v\":-}", "{\"v\":1.}", "{\"v\":.5}", "{\"v\":1e}", "{\"v\":+1}", "{\"v\":NaN}",
        "{\"v\":Infinity}", "{\"v\":tru}", "{\"v\":nul}", "{\"v\":2}x", "{\"v\":2}{}", "null", "true", "5", "\"str\"",
        "[]", "[{\"v\":2,\"seed\":\"A\"}]", "\xEF\xBB\xBF", "\xFF\xFE", "{\"v\":2,\"seed\":\"x\"}\x00",
        "{\"a\":[1,2,]}", "{\"a\":[,1]}", "{\"a\" 1}", "{1:2}", "{\"v\":2,\"seed\":\"\x01\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Almanac a;
        alm_init(&a, i);
        size_t len = strlen(bad[i]);
        if (i == sizeof bad / sizeof bad[0] - 6) len++;   /* the trailing NUL case */
        AlmLoadStatus st = alm_load_json(&a, bad[i], len);
        CHECK(st == ALM_LOAD_CORRUPT, "bad[%zu] status %d", i, (int)st);
        check_valid(&a, "bad json");
        CHECK(alm_import_json(&a, bad[i], len, NOW) == ALM_IMPORT_NOT_BACKUP, "bad[%zu] import", i);
        alm_free(&a);
    }
    /* deep nesting: rejected (or accepted when shallow enough), never a crash */
    for (int depth = 200; depth <= 200000; depth *= 10) {
        size_t n = (size_t)depth * 2 + 64;
        char *s = malloc(n);
        size_t k = (size_t)snprintf(s, n, "{\"v\":2,\"seed\":\"D\",\"x\":");
        for (int i = 0; i < depth; i++) s[k++] = '[';
        for (int i = 0; i < depth; i++) s[k++] = ']';
        s[k++] = '}';
        Almanac a;
        alm_init(&a, 1);
        AlmLoadStatus st = alm_load_json(&a, s, k);
        CHECK(depth <= 250 ? st == ALM_LOAD_OK : st == ALM_LOAD_CORRUPT, "depth %d status %d", depth, (int)st);
        check_valid(&a, "deep");
        alm_free(&a);
        free(s);
    }
    /* huge note is capped, long seed kept */
    {
        size_t n = 600000;
        char *s = malloc(n + 200);
        size_t k = (size_t)snprintf(s, 200, "{\"v\":2,\"seed\":\"");
        for (size_t i = 0; i < 300; i++) s[k++] = 'S';
        k += (size_t)snprintf(s + k, 200, "\",\"notes\":{\"5\":\"");
        for (size_t i = 0; i < 400000; i++) s[k++] = (i % 3) ? 'a' : 'b';
        k += (size_t)snprintf(s + k, 40, "\"}}");
        Almanac a;
        alm_init(&a, 1);
        CHECK(alm_load_json(&a, s, k) == ALM_LOAD_OK, "huge");
        CHECK(strlen(alm_note_get(&a, 5)) == ALM_MAX_NOTE_BYTES && strlen(a.seed) == 300, "caps");
        alm_free(&a);
        free(s);
    }
    /* UTF-16 LE file with BOM */
    {
        const char *t = "{\"v\":2,\"seed\":\"W\xC3\xA9\"}";
        unsigned char u[128];
        size_t k = 0;
        u[k++] = 0xFF; u[k++] = 0xFE;
        for (const char *p = "{\"v\":2,\"seed\":\"W"; *p; p++) { u[k++] = (unsigned char)*p; u[k++] = 0; }
        u[k++] = 0xE9; u[k++] = 0;
        u[k++] = '"'; u[k++] = 0; u[k++] = '}'; u[k++] = 0;
        Almanac a;
        alm_init(&a, 1);
        CHECK(alm_load_json(&a, (const char *)u, k) == ALM_LOAD_OK && strcmp(a.seed, "W\xC3\xA9") == 0, "utf16 %s", t);
        alm_free(&a);
    }
}

static void test_fuzz(const char *cases_path)
{
    /* base documents: a rich state through the API, its export, and a fixture */
    Almanac a = fresh("FUZZ-0001", 12);
    bool upd;
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 2, "T\xF0\x9F\xA7\x8A \"q\"", ALM_EV_AURIL, IWD_REPEAT_NEWMOON, "d\n", NOW, &upd);
    alm_event_save(&a, NULL, IWD_DEFAULT_START_ABS + 4, "Yearly", ALM_EV_NATURAL, IWD_REPEAT_YEARLY, "", NOW, &upd);
    alm_note_set(&a, IWD_DEFAULT_START_ABS + 3, "note \\ \x01 \xE2\x80\xA8");
    alm_note_set(&a, -9, "neg");
    alm_year_name_set(&a, 1489, "Year");
    AlmBuf docs[3] = { {0}, {0}, {0} };
    alm_snapshot_json(&a, true, &docs[0]);
    alm_export_json(&a, ISO, &docs[1]);
    {
        AlmBuf f = {0};
        if (alm_fs_read(cases_path, &f) == ALM_IO_OK) {
            const char *p = strstr(f.data, "CASE rich v2\nIN ");
            if (p) {
                p += 16;
                const char *e = strchr(p, '\n');
                char *h = malloc((size_t)(e - p) + 1);
                memcpy(h, p, (size_t)(e - p));
                h[e - p] = 0;
                size_t len;
                char *raw = unhex(h, &len);
                docs[2].data = raw; docs[2].len = len; docs[2].cap = len + 1;
                free(h);
            }
        }
        alm_buf_free(&f);
    }
    alm_free(&a);
    int runs = 0;
    for (int d = 0; d < 3; d++) {
        if (!docs[d].data) continue;
        size_t n = docs[d].len;
        /* every truncation */
        for (size_t cut = 0; cut <= n; cut += (n > 4000 ? 3 : 1)) {
            char *t = malloc(cut + 1);
            memcpy(t, docs[d].data, cut);
            t[cut] = 0;
            Almanac x;
            alm_init(&x, cut);
            alm_load_json(&x, t, cut);
            check_valid(&x, "truncation");
            if (cut % 97 == 0) exercise(&x);
            alm_import_json(&x, t, cut, NOW);
            check_valid(&x, "truncated import");
            alm_free(&x);
            free(t);
            runs++;
        }
        /* random mutations */
        static const char pool[] = "{}[],:\"\\0123456789-+.eEtrufalsn \x80\xC3\xED\xF0\xFF";
        for (int it = 0; it < 3000; it++) {
            char *t = malloc(n + 16);
            memcpy(t, docs[d].data, n);
            size_t len = n;
            int muts = 1 + (int)(rnd() % 6);
            for (int m = 0; m < muts; m++) {
                size_t pos = len ? rnd() % len : 0;
                switch (rnd() % 3) {
                case 0: t[pos] = pool[rnd() % (sizeof pool - 1)]; break;
                case 1: if (len) { memmove(t + pos, t + pos + 1, len - pos - 1); len--; } break;
                default: if (len < n + 15) { memmove(t + pos + 1, t + pos, len - pos); t[pos] = pool[rnd() % (sizeof pool - 1)]; len++; } break;
                }
            }
            Almanac x;
            alm_init(&x, (uint64_t)it);
            AlmLoadStatus st = alm_load_json(&x, t, len);
            check_valid(&x, "mutation");
            if (st == ALM_LOAD_OK && it % 7 == 0) {
                exercise(&x);
                alm_step_minutes(&x, 60);
                alm_advance(&x, 12);
                alm_step_minutes(&x, -30);
                alm_step_minutes(&x, 5);
                check_valid(&x, "actions after mutation");
            }
            alm_free(&x);
            free(t);
            runs++;
        }
    }
    for (int d = 0; d < 3; d++) alm_buf_free(&docs[d]);
    printf("  fuzz: %d documents\n", runs);
}

/* ------------------------------------------------------------------ multi-day spans */

static int32_t A(int32_t y, int m, int d)
{
    int32_t v = 0;
    if (!iwd_to_abs(y, m, d, IWD_FEST_NONE, &v)) { fprintf(stderr, "bad date\n"); exit(2); }
    return v;
}
static int32_t F(int32_t y, IwdFestival f)
{
    int32_t v = 0;
    if (!iwd_to_abs(y, 0, 1, f, &v)) { fprintf(stderr, "bad festival\n"); exit(2); }
    return v;
}
/* days in [from, to] the event is on, as a bitstring "1100..." */
static void on_string(const Almanac *a, const AlmEvent *e, int32_t from, int32_t to, char *out)
{
    int k = 0;
    for (int32_t d = from; d <= to; d++) out[k++] = alm_event_span_day(e, d, a->moon_offset, NULL) >= 0 ? '1' : '0';
    out[k] = 0;
}
static const AlmEvent *last_ev(const Almanac *a) { return &a->events[a->n_events - 1]; }

static void test_spans(void)
{
    Almanac a;
    bool upd;
    char s[512], buf[512];
    alm_init(&a, 77);

    /* span range texts (form line, day panel meta) */
    alm_fmt_span_range(A(1489, 0, 5), 1, buf, sizeof buf);
    CHECK_STR(buf, "5\xC2\xA0" "Hammer");
    alm_fmt_span_range(A(1489, 0, 5), 5, buf, sizeof buf);
    CHECK_STR(buf, "5\xE2\x80\x93" "9\xC2\xA0" "Hammer");
    alm_fmt_span_range(A(1489, 0, 30), 3, buf, sizeof buf);
    CHECK_STR(buf, "30\xC2\xA0" "Hammer \xE2\x80\x93 1\xC2\xA0" "Alturiak");
    alm_fmt_span_range(A(1489, 0, 30), 2, buf, sizeof buf);
    CHECK_STR(buf, "30\xC2\xA0" "Hammer \xE2\x80\x93 Midwinter");
    alm_fmt_span_range(F(1489, IWD_FEST_MIDWINTER), 1, buf, sizeof buf);
    CHECK_STR(buf, "Midwinter");
    alm_fmt_span_range(A(1489, 10, 30), 2, buf, sizeof buf);   /* "Feast of the Moon": no-break spaces */
    CHECK(strstr(buf, "Moon") && strstr(buf, "Feast") && !strstr(buf, "of the"), "festival name keeps no-break spaces: %s", buf);
    alm_fmt_span_range(A(1489, 0, 5), 99, buf, sizeof buf);    /* clamped to 30 days */
    CHECK_STR(buf, "5\xC2\xA0" "Hammer \xE2\x80\x93 3\xC2\xA0" "Alturiak");
    CHECK(alm_fmt_span_range(IWD_ABS_MAX, 5, buf, sizeof buf) > 0 && !strstr(buf, "\xE2\x80\x93"), "range at the edge: %s", buf);
    CHECK(alm_fmt_span_range(IWD_ABS_MAX - 1, 5, buf, sizeof buf) > 0 && strstr(buf, "\xE2\x80\x93"), "range near the edge: %s", buf);

    /* undo of a deletion: the copy comes back with its id, anchor, details and days, in place */
    {
        Almanac u;
        AlmEvent cp;
        alm_init(&u, 5);
        alm_event_save_span(&u, NULL, A(1489, 0, 3), "First", ALM_EV_AURIL, IWD_REPEAT_YEARLY, "", 1, NOW, &upd);
        alm_event_save_span(&u, NULL, A(1489, 0, 5), "Fair", ALM_EV_NATURAL, IWD_REPEAT_YEARLY, "Stalls on the ice", 5, NOW + 1, &upd);
        alm_event_save_span(&u, NULL, A(1489, 0, 7), "Last", ALM_EV_REMINDER, IWD_REPEAT_NONE, "", 1, NOW + 2, &upd);
        char id[64];
        snprintf(id, sizeof id, "%s", u.events[1].id);
        CHECK(alm_event_index(&u, id) == 1 && alm_event_index(&u, "nope") == -1, "event index");
        CHECK(alm_event_dup(&cp, &u.events[1]), "dup");
        AlmResult r = alm_event_delete(&u, id);
        CHECK(u.n_events == 2 && !alm_event_find(&u, id) && strstr(r.toast, "all 5 days"), "deleted: %s", r.toast);
        r = alm_event_restore(&u, &cp, 1);
        CHECK(!(r.fx & ALM_FX_REJECTED) && (r.fx & ALM_FX_SAVE) && strstr(r.toast, "<b>Fair</b> restored"), "restored: %s", r.toast);
        CHECK(u.n_events == 3 && strcmp(u.events[1].id, id) == 0 && u.events[1].abs == A(1489, 0, 5) &&
              u.events[1].days == 5 && u.events[1].repeat == IWD_REPEAT_YEARLY && u.events[1].type == ALM_EV_NATURAL &&
              strcmp(u.events[1].details, "Stalls on the ice") == 0 && strcmp(u.events[2].title, "Last") == 0,
              "restored in place with everything");
        CHECK(alm_event_span_day(&u.events[1], A(1490, 0, 9), 0, NULL) == 4, "restored span works");
        r = alm_event_restore(&u, &cp, 0);
        CHECK((r.fx & ALM_FX_REJECTED) && u.n_events == 3, "restoring twice is rejected");
        alm_event_release(&cp);
        CHECK(cp.id == NULL && cp.title == NULL, "release clears");
        alm_free(&u);
    }

    /* one-time 5-day festival: days 1..5 of the same event, nothing before or after */
    int32_t s0 = A(1489, 2, 10);
    alm_event_save_span(&a, NULL, s0, "Festival of Ice", ALM_EV_NATURAL, IWD_REPEAT_NONE, "Lanterns on the lake", 5, NOW, &upd);
    CHECK(a.n_events == 1 && a.events[0].days == 5, "span saved");
    on_string(&a, &a.events[0], s0 - 2, s0 + 7, s);
    CHECK_STR(s, "0011111000");
    for (int k = 0; k < 5; k++) {
        AlmMark mk[4]; int32_t st = 0;
        int n = alm_marks_on(&a, s0 + k, mk, 4);
        int j = n > 0 && mk[0].builtin ? 1 : 0;
        CHECK(n >= 1 && mk[j].event == &a.events[0] && mk[j].span_day == k && mk[j].span_len == 5 && mk[j].span_start == s0,
              "mark day %d", k);
        CHECK(alm_event_span_day(&a.events[0], s0 + k, 0, &st) == k && st == s0, "span day %d", k);
    }
    {
        AlmMark mk[4];
        int n = alm_marks_on(&a, s0 + 1, mk, 4), j = mk[0].builtin ? 1 : 0;
        CHECK(n >= 1, "marks");
        alm_fmt_mark_meta(&mk[j], s0 + 1, buf, sizeof buf);
        CHECK_STR(buf, "Natural \xC2\xB7 One-time \xC2\xB7 Day 2 of 5 \xC2\xB7 10\xE2\x80\x93" "14\xC2\xA0" "Ches");
        alm_fmt_mark_chip(&mk[j], false, buf, sizeof buf);
        CHECK_STR(buf, "Festival of Ice \xC2\xB7 day 2 of 5");
        alm_fmt_mark_chip(&mk[j], true, buf, sizeof buf);   /* not the first day: still "day k of n" */
        CHECK_STR(buf, "Festival of Ice \xC2\xB7 day 2 of 5");
    }

    /* JSON: key written after details only when > 1; single-day events byte-identical */
    {
        char *j1 = snap(&a, false);
        CHECK(strstr(j1, "\"details\":\"Lanterns on the lake\",\"days\":5}") != NULL, "days key written last");
        free(j1);
        alm_event_save(&a, NULL, s0, "One day", ALM_EV_REMINDER, IWD_REPEAT_NONE, "", NOW, &upd);
        CHECK(last_ev(&a)->days == 1, "alm_event_save defaults to 1 day");
        j1 = snap(&a, false);
        CHECK(strstr(j1, "\"title\":\"One day\",\"type\":\"reminder\",\"repeat\":\"none\",\"details\":\"\"}") != NULL,
              "no days key at 1");
        Almanac b; alm_init(&b, 3);
        CHECK(alm_load_json(&b, j1, strlen(j1)) == ALM_LOAD_OK && b.n_events == 2 && b.events[0].days == 5 && b.events[1].days == 1,
              "span round trip");
        char *j2 = snap(&b, false);
        CHECK(strcmp(j1, j2) == 0, "span snapshot byte-identical after reload");
        free(j2);
        AlmBuf ex = {0};
        CHECK(alm_export_json(&b, ISO, &ex) && strstr(ex.data, "\"days\": 5") != NULL, "export keeps days");
        Almanac c; alm_init(&c, 4);
        CHECK(alm_import_json(&c, ex.data, ex.len, NOW) == ALM_IMPORT_OK && c.n_events == 2 && c.events[0].days == 5, "import days");
        alm_buf_free(&ex);
        alm_free(&c);
        alm_free(&b);
        free(j1);
        alm_event_delete(&a, last_ev(&a)->id);
    }

    /* editing from a middle day changes the one event; days kept by alm_event_save */
    {
        char id[64];
        snprintf(id, sizeof id, "%s", a.events[0].id);
        alm_select(&a, s0 + 2, true);
        alm_event_save_span(&a, id, a.sel_abs, "Festival of Ice", ALM_EV_NATURAL, IWD_REPEAT_NONE, "Moved to the shore", 0, NOW, &upd);
        CHECK(upd && a.n_events == 1 && a.events[0].abs == s0 && a.events[0].days == 5, "edit from day 3 keeps anchor and length");
        for (int k = 0; k < 5; k++) {
            const AlmEvent *ev[2];
            CHECK(alm_events_on(&a, s0 + k, ev, 2) == 1 && strcmp(ev[0]->details, "Moved to the shore") == 0, "shared details %d", k);
        }
        alm_event_save(&a, id, 0, "Festival of Ice", ALM_EV_NATURAL, IWD_REPEAT_NONE, "x", NOW, &upd);
        CHECK(a.events[0].days == 5, "plain save keeps days");
        alm_event_save_span(&a, id, 0, "Festival of Ice", ALM_EV_NATURAL, IWD_REPEAT_NONE, "x", 3, NOW, &upd);
        on_string(&a, &a.events[0], s0, s0 + 5, s);
        CHECK_STR(s, "111000");
        alm_event_save_span(&a, id, 0, "Festival of Ice", ALM_EV_NATURAL, IWD_REPEAT_NONE, "x", 5, NOW, &upd);
        /* delete from day 4: every day clears */
        const AlmEvent *ev[2];
        CHECK(alm_events_on(&a, s0 + 3, ev, 2) == 1, "on day 4");
        AlmResult r = alm_event_delete(&a, ev[0]->id);
        CHECK(strstr(r.toast, "removed \xC2\xB7 all 5 days") != NULL, "delete toast names the span: %s", r.toast);
        for (int k = 0; k < 5; k++) CHECK(alm_events_on(&a, s0 + k, NULL, 0) == 0, "cleared day %d", k);
    }

    /* limits: clamp in the API */
    alm_event_save_span(&a, NULL, s0, "Big", ALM_EV_NATURAL, IWD_REPEAT_NONE, "", 999, NOW, &upd);
    CHECK(last_ev(&a)->days == ALM_EVENT_DAYS_MAX, "api clamps to max");
    alm_event_save_span(&a, NULL, s0, "Neg", ALM_EV_NATURAL, IWD_REPEAT_NONE, "", -4, NOW, &upd);
    CHECK(last_ev(&a)->days == 1, "api: negative -> 1");
    while (a.n_events) alm_event_delete(&a, a.events[0].id);

    /* across month end into a festival, and across the year end */
    {
        int32_t m0 = A(1489, 0, 29);           /* 29 Hammer, 30 Hammer, Midwinter, 1 Alturiak, 2 Alturiak */
        alm_event_save_span(&a, NULL, m0, "Thaw", ALM_EV_NATURAL, IWD_REPEAT_NONE, "", 5, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), F(1489, IWD_FEST_MIDWINTER), 0, NULL) == 2, "festival is day 3");
        CHECK(alm_event_span_day(last_ev(&a), A(1489, 1, 2), 0, NULL) == 4, "2 Alturiak is day 5");
        CHECK(alm_event_span_day(last_ev(&a), A(1489, 1, 3), 0, NULL) < 0, "3 Alturiak not");
        int32_t y0 = A(1489, 11, 29);          /* 29, 30 Nightal, 1, 2 Hammer 1490 */
        alm_event_save_span(&a, NULL, y0, "Year turn", ALM_EV_AURIL, IWD_REPEAT_NONE, "", 4, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), A(1490, 0, 2), 0, NULL) == 3, "year end crossing");
        AlmMark mk[4];
        int n = alm_marks_on(&a, A(1490, 0, 1), mk, 4), j = n > 0 && mk[0].builtin ? 1 : 0;
        CHECK(n >= 1, "marks");
        alm_fmt_mark_meta(&mk[j], A(1490, 0, 1), buf, sizeof buf);
        CHECK_STR(buf, "Auril \xC2\xB7 One-time \xC2\xB7 Day 3 of 4 \xC2\xB7 29\xC2\xA0" "Nightal \xE2\x80\x93 2\xC2\xA0" "Hammer");
        while (a.n_events) alm_event_delete(&a, a.events[0].id);
    }

    /* yearly repeat: 5 days every year, also when anchored on a festival / Shieldmeet */
    {
        alm_event_save_span(&a, NULL, s0, "Yearly", ALM_EV_NATURAL, IWD_REPEAT_YEARLY, "", 5, NOW, &upd);
        for (int32_t y = 1489; y <= 1493; y++) {
            on_string(&a, last_ev(&a), A(y, 2, 9), A(y, 2, 15), s);
            CHECK_STR(s, "0111110");
        }
        on_string(&a, last_ev(&a), A(1488, 2, 9), A(1488, 2, 15), s);
        CHECK_STR(s, "0000000");              /* never before the anchor */
        int32_t mw = F(1489, IWD_FEST_MIDWINTER);
        alm_event_save_span(&a, NULL, mw, "Midwinter fair", ALM_EV_REMINDER, IWD_REPEAT_YEARLY, "", 3, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), A(1491, 1, 2), 0, NULL) == 2 &&
              alm_event_span_day(last_ev(&a), A(1491, 1, 3), 0, NULL) < 0 &&
              alm_event_span_day(last_ev(&a), A(1491, 0, 30), 0, NULL) < 0, "festival-anchored yearly span");
        int32_t sm = F(1492, IWD_FEST_SHIELDMEET);
        alm_event_save_span(&a, NULL, sm, "Shieldmeet games", ALM_EV_REMINDER, IWD_REPEAT_YEARLY, "", 2, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), A(1492, 7, 1), 0, NULL) == 1, "Shieldmeet + 1 Eleasis");
        CHECK(alm_event_span_day(last_ev(&a), A(1493, 7, 1), 0, NULL) < 0, "no Shieldmeet in 1493: no span");
        CHECK(alm_event_span_day(last_ev(&a), A(1496, 7, 1), 0, NULL) == 1, "next leap year");
        while (a.n_events) alm_event_delete(&a, a.events[0].id);
    }

    /* monthly and tenday repeats; a span longer than the interval: latest occurrence wins */
    {
        alm_event_save_span(&a, NULL, A(1489, 0, 28), "Monthly", ALM_EV_NATURAL, IWD_REPEAT_MONTHLY, "", 4, NOW, &upd);
        /* 28 Hammer .. Midwinter .. 1 Alturiak; then 28 Alturiak .. 1 Ches */
        CHECK(alm_event_span_day(last_ev(&a), F(1489, IWD_FEST_MIDWINTER), 0, NULL) == 3 &&
              alm_event_span_day(last_ev(&a), A(1489, 1, 1), 0, NULL) < 0 &&
              alm_event_span_day(last_ev(&a), A(1489, 2, 1), 0, NULL) == 3, "monthly span over a festival");
        alm_event_save_span(&a, NULL, A(1489, 0, 1), "Tenday x3", ALM_EV_NATURAL, IWD_REPEAT_TENDAY, "", 3, NOW, &upd);
        on_string(&a, last_ev(&a), A(1489, 0, 1), A(1489, 0, 20), s);
        CHECK_STR(s, "11100000001110000000");
        alm_event_save_span(&a, NULL, A(1489, 0, 1), "Tenday x15", ALM_EV_NATURAL, IWD_REPEAT_TENDAY, "", 15, NOW, &upd);
        const AlmEvent *e = last_ev(&a);
        CHECK(alm_event_span_day(e, A(1489, 0, 11), 0, NULL) == 0 && alm_event_span_day(e, A(1489, 0, 15), 0, NULL) == 4,
              "overlap restarts at the newer occurrence");
        CHECK(alm_event_span_day(e, F(1489, IWD_FEST_MIDWINTER), 0, NULL) == 10, "festival: day 11 of the 21 Hammer occurrence");
        {
            const AlmEvent *ev[8];
            int n = alm_events_on(&a, A(1489, 0, 15), ev, 8), cnt = 0;
            for (int i = 0; i < n; i++) if (ev[i] == e) cnt++;
            CHECK(cnt == 1, "overlapping occurrences show once per day");
        }
        while (a.n_events) alm_event_delete(&a, a.events[0].id);
    }

    /* new-moon repeat: every new moon starts a span */
    {
        int32_t nm = 0;
        for (int32_t d = A(1489, 0, 1); d < A(1489, 0, 1) + 40; d++) if (iwd_moon(d, 0).is_new) { nm = d; break; }
        CHECK(nm != 0, "new moon found");
        alm_event_save_span(&a, NULL, nm, "Dark nights", ALM_EV_AURIL, IWD_REPEAT_NEWMOON, "", 3, NOW, &upd);
        int32_t next = 0;
        for (int32_t d = nm + 20; d < nm + 40; d++) if (iwd_moon(d, 0).is_new) { next = d; break; }
        CHECK(alm_event_span_day(last_ev(&a), next + 2, 0, NULL) == 2 && alm_event_span_day(last_ev(&a), next + 3, 0, NULL) < 0,
              "new-moon span");
        while (a.n_events) alm_event_delete(&a, a.events[0].id);
    }

    /* Upcoming: a span shows once, where it starts; days with only continuing spans have no row */
    {
        Almanac u; alm_init(&u, 5);
        alm_advance(&u, 12);                     /* begin: today = start */
        int32_t today = 0;
        CHECK(alm_today_abs(&u, &today), "today");
        alm_event_save_span(&u, NULL, today + 2, "Fair", ALM_EV_REMINDER, IWD_REPEAT_NONE, "", 5, NOW, &upd);
        alm_event_save_span(&u, NULL, today - 1, "Ongoing", ALM_EV_NATURAL, IWD_REPEAT_NONE, "", 4, NOW, &upd);
        AlmUpcoming up[ALM_UPCOMING_DAYS + 1];
        int n = alm_upcoming(&u, up), fair = 0, ongoing = 0;
        for (int i = 0; i < n; i++) {
            AlmMark mk[16];
            int m = alm_upcoming_marks(&u, &up[i], mk, 16);
            CHECK(m == up[i].n_marks, "n_marks matches");
            for (int k = 0; k < m && k < 16; k++) {
                if (mk[k].builtin) continue;
                if (strcmp(mk[k].title, "Fair") == 0) {
                    fair++;
                    CHECK(up[i].abs == today + 2 && mk[k].span_day == 0, "fair on its first day");
                    alm_fmt_mark_chip(&mk[k], true, buf, sizeof buf);
                    CHECK_STR(buf, "Fair \xC2\xB7 5 days");
                }
                if (strcmp(mk[k].title, "Ongoing") == 0) {
                    ongoing++;
                    CHECK(up[i].rel_days == 0 && mk[k].span_day == 1, "in-progress span only on the base row");
                    alm_fmt_mark_chip(&mk[k], true, buf, sizeof buf);
                    CHECK_STR(buf, "Ongoing \xC2\xB7 day 2 of 4");
                }
            }
        }
        CHECK(fair == 1 && ongoing == 1, "upcoming de-duplicated (fair %d, ongoing %d)", fair, ongoing);
        /* the day toast and cell label of a middle day */
        AlmCell cells[32];
        int nc = alm_month_cells(&u, cells);
        for (int i = 0; i < nc; i++)
            if (cells[i].abs == today + 3) {
                alm_fmt_cell_label(&u, &cells[i], buf, sizeof buf);
                CHECK(strstr(buf, "Fair (day 2 of 5)") != NULL, "cell label: %s", buf);
            }
        alm_free(&u);
    }

    /* JSON hostile values for "days" */
    {
        static const struct { const char *v; int want; } H[] = {
            { "0", 1 }, { "-3", 1 }, { "1", 1 }, { "2", 2 }, { "30", 30 }, { "31", 30 }, { "1e300", 30 },
            { "-1e300", 1 }, { "2.5", 1 }, { "\"5\"", 1 }, { "true", 1 }, { "null", 1 }, { "[5]", 1 }, { "{}", 1 },
            { "5.0", 5 }, { "4294967301", 30 },
        };
        for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
            char doc[512];
            snprintf(doc, sizeof doc, "{\"v\":2,\"seed\":\"X\",\"events\":[{\"id\":\"e1\",\"abs\":%ld,\"title\":\"T\","
                     "\"days\":%s}]}", (long)s0, H[i].v);
            Almanac b; alm_init(&b, 9);
            CHECK(alm_load_json(&b, doc, strlen(doc)) == ALM_LOAD_OK && b.n_events == 1 && b.events[0].days == H[i].want,
                  "days %s -> %d (got %d)", H[i].v, H[i].want, b.n_events ? b.events[0].days : -99);
            char *j = snap(&b, false);
            CHECK((H[i].want > 1) == (strstr(j, "\"days\"") != NULL), "days key presence for %s", H[i].v);
            free(j);
            alm_free(&b);
        }
        /* an old file without the key: one-day events */
        char doc[256];
        snprintf(doc, sizeof doc, "{\"v\":2,\"seed\":\"X\",\"events\":[{\"id\":\"e1\",\"abs\":%ld,\"title\":\"T\"}]}", (long)s0);
        Almanac b; alm_init(&b, 9);
        CHECK(alm_load_json(&b, doc, strlen(doc)) == ALM_LOAD_OK && b.events[0].days == 1 &&
              alm_events_on(&b, s0 + 1, NULL, 0) == 0, "old file: one day");
        alm_free(&b);
    }
    /* near the abs limits a span never overflows */
    {
        alm_event_save_span(&a, NULL, IWD_ABS_MAX - 2, "Edge", ALM_EV_NATURAL, IWD_REPEAT_NONE, "", 30, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), IWD_ABS_MAX, 0, NULL) == 2, "edge max");
        AlmMark mk[4];
        int n = alm_marks_on(&a, IWD_ABS_MAX, mk, 4), j = n > 0 && mk[0].builtin ? 1 : 0;
        if (n > j) alm_fmt_mark_meta(&mk[j], IWD_ABS_MAX, buf, sizeof buf);
        alm_event_save_span(&a, NULL, IWD_ABS_MIN, "Edge2", ALM_EV_NATURAL, IWD_REPEAT_YEARLY, "", 30, NOW, &upd);
        CHECK(alm_event_span_day(last_ev(&a), IWD_ABS_MIN + 29, 0, NULL) == 29, "edge min");
    }
    alm_free(&a);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_store CASES_FILE TMPDIR\n"); return 2; }
    run_cases(argv[1]);
    test_round_trip(argv[2]);
    test_actions();
    test_minutes(argv[2]);
    test_spans();
    test_malformed();
    test_fuzz(argv[1]);
    if (g_fail) { fprintf(stderr, "store tests: %d of %d checks FAILED\n", g_fail, g_checks); return 1; }
    printf("store tests: OK (%d checks)\n", g_checks);
    return 0;
}
