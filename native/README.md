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

- a toast on screen
- an armed delete button
- the notes autosave delay
- the debounced save (250 ms after the last routine step: time / day steps including a
  step past midnight, scrubbing, calendar paging, selecting a day, the dice-ledger chip;
  explicit actions, undo and closing save at once)
- the short hero cross-fade when a step changes the weather
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
| `src/ui_view.c` | Layout, scrolling, header, hero, Advance + time-step controls, stats, effects, footer (dice chip), toast, keys card |
| `src/ui_view_time.c` | 24-hour timeline and the dice ledger (opened from the footer's dice chip) |
| `src/ui_view_cal.c` | Calendar, upcoming, day panel with the event form and notes |
| `src/ui_settings.c` | Settings dialog, including backup export and import |
| `src/main.c` | Window, message loop, input, EDIT controls, popup menu, timers, persistence, sound, test hooks |
| `test/` | Host tests: C engine parity with the JS engine in node, store round trips, audio synthesis and dropout stress (`test/run.sh`); `AUDIO_DEVICE_TEST=1` also runs the waveOut layer against a fake real-time device under wine |

## Build

    native/build.sh        # needs x86_64-w64-mingw32-gcc and windres

The output is `native/build/IcewindDaleWeatherHelper.exe` (about 1.4 MB, almost all of it
the twelve JPEG scenes).
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
holds the window position, the sound and Interior settings and whether the dice ledger is open. The
clock is stored as minutes / 60 (a plain number the web app accepts). **Settings → Export backup** writes a
backup file that the web app can import, and the native app can import the web app's
backups.

## Controls

The live weather is always at the top of the window: scene, temperature, cold DC, wind,
light, severity, forecast and the effects of the current hour. Every fact has one place
on the screen; every function one visible control and at most one key. Hover a control to
see its key; F1 (or the "?" chip) shows them all.

| Input | Action |
|---|---|
| Right arrow, Space, "+1h" | forward 1 hour |
| Left arrow, "−1h" | back 1 hour |
| Shift+Right / Shift+Left, "+30m" / "−30m" | forward / back 30 minutes |
| Ctrl+Right / Ctrl+Left, "+5m" / "−5m" | forward / back 5 minutes |
| (past midnight) | forward on today advances the party to the next day (Ctrl+Z steps back); back past 00:00 views the previous day, today stays. A held key never crosses midnight |
| N, "Advance to …" | Advance Day (lands at 12:00) |
| Ctrl+Z | undo the last day step (advance, midnight step or make-today, with its clock) or, if it came later, the last event deletion |
| PageUp / PageDown, "‹ Previous day" / "Next day ›" | view the previous / next day |
| T, Esc, "Back to today" (hero), "Today" (calendar) | back to the party's day and clock |
| Click / drag the timeline | scrub the time (5-minute steps) |
| Click a calendar day | view it |
| Double-click a calendar day, "Make this day today" | make it the current day |
| E / O, "Add" | new event or reminder / write a note for the viewed day |
| (event form) "Lasts", − / +, Up / Down in the field | how many days the event lasts (1 to 30); every day of a span links to the same event and note |
| M, Sound | wind sound on / off ("No audio" if no device) |
| I, Interior | while the sound is on: muffle it as heard from inside a building (sound only; remembered) |
| °F / °C (header) | switch the temperature unit |
| F2, Settings | Settings (start date, seed, year names, moon, backup) |
| F1, "?" | keyboard shortcuts card |
| Dice chip (footer, left) | show / hide the dice ledger (remembered) |
| Wheel, Up / Down, Ctrl+Home / Ctrl+End | scroll the page |
| Esc | close the event form, else hide the toast, else back to today |

In text fields:

- Esc leaves the field. It cancels the event form, or saves the notes.
- Enter in Title or Lasts submits the form, and so does Ctrl+Enter in Details.
- Tab moves Title → Lasts → Details (Shift+Tab back).
- Up / Down in Lasts make the event one day longer / shorter.
- Ctrl+A selects all.

## Test hooks

    --state <path>     use this save file (loaded, then saved after every change)
    --seed <text>      start a fresh almanac with this seed instead of loading. Nothing is
                       written unless --state is also given, so a test run never
                       overwrites the real save
    --days <n>         advance n days at start-up (n = 0..3650)
    --select <offset>  view the day start + offset (0 = weather day 1)
    --hour <0-23>      set the clock of the viewed day (whole hour)
    --minute <0-59>    minutes of that clock (with --hour, or on the loaded hour), e.g.
                       --hour 20 --minute 35 for 20:35
    --size WxH         exact initial client size in pixels (else 1280x860, DPI scaled,
                       or the remembered window)
    --scroll <px>      initial page scroll
    --open settings|eventform   open the Settings dialog / a new event form at start-up
    --shot-ready       after the first paint the window title becomes "Icewind Dale
                       Almanac [ready]" (otherwise it is always "Icewind Dale Almanac");
                       screenshot scripts can wait for it

Example screenshot under wine (unset TMPDIR first):

    xvfb-run -a -s "-screen 0 1600x1100x24" bash -c \
      'wine native/build/IcewindDaleWeatherHelper.exe --state Z:/tmp/s.json \
         --seed RIME-00FF --days 3 --hour 14 & sleep 8; import -window root out.png; wineserver -k'
