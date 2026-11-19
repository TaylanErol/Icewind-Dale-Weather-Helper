# Icewind Dale Almanac - native build

A native Win32 program in C11 that carries every feature and rule of the web app
"Icewind Dale - Almanac of the Everlasting Rime". It shows them over the realistic
raster scene images instead of vector art. No framework and no runtime to install:
the exe links only DLLs that ship with Windows. Those are user32, gdi32, gdiplus,
comctl32, comdlg32, shell32, shlwapi, ole32, winmm, kernel32 and msvcrt. The twelve
JPEG scenes and the icon are built into the exe as resources.

It does nothing while idle. There are no timers or animation loops at rest. Painting
happens only on `WM_PAINT`, from a cached back buffer. Timers run only while something
is moving:

- playback
- a toast on screen
- an armed delete button
- the notes autosave delay
- the debounced save (250 ms after the last routine step: hour / day steps, scrubbing,
  calendar paging, selecting a day; explicit actions and closing save at once)
- random thunder, only while the sound is on during a Lightning Storm

The behaviour is specified in [SPEC.md](SPEC.md).

## Sources

| File | Role |
|---|---|
| `src/engine.c/.h` | Pure C11 port of `engine.js` + `harptos.js` (weather chain, calendar, moon, cold table, effects, forecast) |
| `src/store.c/.h` | Almanac state, JSON load/save, backup import/export, every app action |
| `src/audio.c/.h` | Wind ambience on winmm waveOut, a port of `audio.js` |
| `src/ui.h`, `src/ui_blocks.h` | Shared UI contract: palette, fonts, surfaces, hit regions, the `Ui` state |
| `src/ui_draw.c` | Pixels, text, GDI+ shapes, JPEG decoding, scene composite, thumbnails |
| `src/ui_view.c` | Layout, scrolling, header, hero, advance/hour controls, stats, effects, toast |
| `src/ui_view_time.c` | 24-hour timeline and dice ledger |
| `src/ui_view_cal.c` | Calendar, upcoming, day panel with the event form and notes |
| `src/ui_settings.c` | Settings dialog, including backup export and import |
| `src/main.c` | Window, message loop, input, EDIT controls, popup menu, timers, persistence, sound, test hooks |
| `test/` | Host tests: C engine parity with the JS engine in node, store round trips, audio (`test/run.sh`) |

## Build

    native/build.sh        # needs x86_64-w64-mingw32-gcc and windres

The output is `native/build/IcewindDaleWeatherHelper.exe` (about 1.3 MB).
`build.sh` compiles every `src/*.c`. It compiles `engine.c` with
`-ffp-contract=off` so the numbers match the web app exactly.
`tools/gen_assets.py` regenerates the scene images in `native/assets/`.

Host tests:

    native/test/run.sh

## Save file

The almanac is saved after every change. Saving is atomic: a temp file is written and
then swapped in. The file is:

1. `--state <path>`, if given;
2. otherwise `state.json` next to the exe, if that file exists (portable mode);
3. otherwise `%APPDATA%\IcewindDaleAlmanac\state.json`. The folder is created if it is
   missing.

If the file is corrupt, it is renamed to `<file>.bad` and a fresh almanac is started.
The JSON is the same one the web app keeps in localStorage, plus a `"native"` key that
holds the window position and the sound setting. **Settings → Export backup** writes a
backup file that the web app can import, and the native app can import the web app's
backups.

## Controls

The live weather is always at the top of the window: scene, temperature, cold DC, wind,
light, severity, forecast and the effects of the current hour.

| Input | Action |
|---|---|
| Right arrow, Space, "Next hour" | +1 hour. On today, going past 23:00 advances the day and lands at 00:00 |
| Left arrow, "Previous hour" | −1 hour |
| Home / End | 00:00 / 23:00 |
| N, Enter, "Advance to …" | Advance Day (lands at 12:00) |
| PageUp / PageDown, ‹ › | view the previous / next day |
| Click / drag the timeline | scrub the time |
| P, Play | play the day (1.25 s per game hour) |
| U, °F / °C | switch the temperature unit |
| M, Sound | wind sound on / off ("No audio" if no device) |
| T, Today | calendar: go to the current day |
| F2, Ctrl+, , Settings, seed chip | Settings |
| Ctrl+C | copy the seed |
| Tab | keyboard focus into the calendar grid. In the grid: arrows, Home/End, Enter or Space selects, Shift+Enter makes the day today, Esc/Tab leaves |
| Click a calendar day | view it |
| Double-click a calendar day | make it the current day |
| Wheel, Up / Down, Ctrl+Home / Ctrl+End | scroll the page |
| Esc | close the event form, else hide the toast |

In text fields:

- Esc leaves the field. It cancels the event form, or saves the notes.
- Enter in Title submits the form, and so does Ctrl+Enter in Details.
- Tab moves between Title and Details.
- Ctrl+A selects all.

## Test hooks

    --state <path>     use this save file (loaded, then saved after every change)
    --seed <text>      start a fresh almanac with this seed instead of loading. Nothing is
                       written unless --state is also given, so a test run never
                       overwrites the real save
    --days <n>         advance n days at start-up (n = 0..3650)
    --select <offset>  view the day start + offset (0 = weather day 1)
    --hour <0-23>      set the clock of the viewed day
    --size WxH         exact initial client size in pixels (else 1280x860, DPI scaled,
                       or the remembered window)
    --scroll <px>      initial page scroll
    --open settings|eventform   open the Settings dialog / a new event form at start-up
    --shot-ready       after the first paint " [ready]" is appended to the window title
                       (live, e.g. "5 Hammer · 14:00 · Clear Skies −51°F — Icewind Dale
                       Almanac [ready]"); screenshot scripts can wait for it

Example screenshot under wine (unset TMPDIR first):

    xvfb-run -a -s "-screen 0 1600x1100x24" bash -c \
      'wine native/build/IcewindDaleWeatherHelper.exe --state Z:/tmp/s.json \
         --seed RIME-00FF --days 3 --hour 14 & sleep 8; import -window root out.png; wineserver -k'
