# Icewind Dale Almanac (native) - behaviour spec

Source of truth: the web app "Icewind Dale - Almanac of the Everlasting Rime"
(`index.html`, `js/engine.js`, `js/harptos.js`, `js/app.js`, `js/calendar.js`,
`js/audio.js`, `js/scene.js`, `css/*`, rules PDF). Reference copies of `engine.js` and
`harptos.js` are in `native/test/reference/`. The old `main.py` / `weather_data.h` text
is superseded. Where this spec says "web", it describes what the web app does; "native"
marks a deliberate difference (all listed again in section 12).

All strings below are exact, UTF-8. `—` is U+2014, `−` is U+2212 (used for every
displayed negative number), `·` U+00B7, `→` U+2192, `…` U+2026, `±` U+00B1, `≈` U+2248,
`½` U+00BD, `¾` U+00BE, `û` in "Selûne".

## 1. Architecture

| File | Role |
|---|---|
| `src/engine.h/.c` | Pure C11 port of engine.js + harptos.js + the pure per-hour derivations of app.js (effects list, forecast, labels). No Win32, no allocation. |
| `src/store.h/.c` | State (`Almanac`), clean/migrate, JSON reader/writer, save/load, backup import/export, all app actions, calendar/day-panel/hero derivations. Pure C11 + a file shim. |
| `src/audio.h/.c` | Wind ambience on winmm waveOut (port of audio.js). |
| `src/main.c` (+ `ui_*.c`) | Window, layout, painting (GDI + GDI+ flat API), input, dialogs, timers. |
| `src/app.rc`, `app.manifest`, `build.sh` | Resources: icon, manifest, the 12 JPEG scenes as RCDATA 101..112. |
| `test/` | Host tests: C engine vs node running the reference JS (parity), store round trips; `test/run.sh`. |

Build: `x86_64-w64-mingw32-gcc -std=c11 -Os -s -municode -mwindows -Wall -Wextra`, zero
warnings, `engine.c` additionally with `-ffp-contract=off` (never `-ffast-math`), links
only system DLLs (user32 gdi32 gdiplus comdlg32 shell32 shlwapi winmm kernel32 ole32 as
needed). Host tests: `gcc -std=c11 -O2 -ffp-contract=off` on `engine.c store.c` + tests.

Performance contract (unchanged from the current native app): no timers or animation loops
while idle; paint only on `WM_PAINT` from a cached back buffer; scaled scene and timeline
cached; days rebuilt from the seed (3650 days take < 5 ms). Timers exist only while
something is in motion: playback, a toast on screen, an armed delete button, the optional
day transition, the notes autosave debounce.

## 2. Reproducibility

`engine.h` lists the JS number pitfalls (Math.imul / `>>>` / uint32 arithmetic, UTF-16
string hashing, Math.round halves toward +inf, floor division on negatives, fmod, operation
order, Math.cos). Integers, strings and doubles must all match the JS exactly, bit for bit:
V8's Math.cos is fdlibm's, so engine.c carries an exact fdlibm port (`iwd_js_cos`) instead of
calling the platform cos(). Verified anchors (node 22):

* `hashSeed("") = 1382371643`, `hashSeed("RIME-00FF") = 2105444690`,
  `hashSeed("RIME-00FF|temp") = 4064753795`, `hashSeed("é☃𝄞") = 503696356`
* `toAbs(1 Hammer 1489) = 543858` (default start), `yearStart(1372) = 501123`
* `moon(543858, 0)`: age 16.46875, "Waning Gibbous", illum 0.9834464644695761
* `rebuild("RIME-00FF", 1)` day 1 segments `[id,start,end,dur]`:
  `[5,0,1,1] [2,1,5,4] [5,5,6,1] [2,6,8,2] [1,8,16,8] [9,16,19,3] [5,19,22,3] [6,22,24,4]`,
  tempMin −72, tempMax −45
* coldDC: −50 Frigid 16, −79 Frigid 18, −80 Severe 20, −139 Deathly 28, −200 Deathly 35,
  5 Cold 10, 15 Cold 9, 100 Cold 0

## 3. Rules data (engine.js)

| id | name | dice | wind | °F | light | moon | natural (*) | next (1st/2nd/3rd) | severity | colour |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Clear Skies | 2 | none | −50 | dim | ½ | yes | 2/5/9 | 1 Calm | #a6ecff |
| 2 | Light Cloud Coverage | 1 | mild | −55 | dim | ¾ | yes | 1/3/5 | 1 Calm | #8ccbeb |
| 3 | Overcast | 1 | moderate | −65 | none | - | no | 2/4/11 | 2 Uneasy | #6d819b |
| 4 | Storm Clouds | 1 | severe | −80 | none | - | no | 3/6/10 | 3 Dangerous | #46526b |
| 5 | Light Snow Dusting | 1 | none | −60 | dim | Full | yes | 2/6/10 | 1 Calm | #dcf3ff |
| 6 | Heavy Snowfall | 1 | mild | −70 | none | - | no | 3/7/11 | 3 Dangerous | #a9c5e2 |
| 7 | Blizzard | 2 | moderate | −90 | none | - | no | 4/6/8 | 4 Deadly | #7aa7d8 |
| 8 | White Out | 1 | severe | −130 | none | - | no | 5/6/7 | 5 Lethal | #f5fbff |
| 9 | Extreme Fog | 1 | none | −55 | dim | Full | yes | 1/5/12 | 2 Uneasy | #b9c7d1 |
| 10 | Lightning Storm | 2 | mild | −60 | lightning | - | no | 4/5/6 | 3 Dangerous | #b49cff |
| 11 | Hail Storm | 1 | severe | −80 | none | - | no | 3/6/7 | 3 Dangerous | #7fd1d9 |
| 12 | Wind Vortex | 1 | severe | −110 | dim | ½ | yes | 1/1/1 | 4 Deadly | #5ee0c8 |

Flavour texts and the effect lists (category + text, in order, with the shared constants
NO_LIGHT, DIM_SIGHT, COLD_NO_RESIST, COLD_EVEN_RESIST, RANGED, HEARING, EXTINGUISH,
CONCENTRATION expanded in place) are copied verbatim from `test/reference/engine.js`; the
parity test diffs them. Wind labels: "No Wind" 0, "Mild Wind" 1, "Moderate Wind" 2,
"Severe Wind" 3. Severity labels: 0 "—", "Calm", "Uneasy", "Dangerous", "Deadly", "Lethal".

Effect category labels (CAT_LABEL): sight "Sight", hearing "Hearing", ranged "Ranged
attacks", attack "Attacks", cold "Extreme cold", travel "Travel", flame "Extinguishing
winds", concentration "Concentration", hazard "Hazard", light "Unnatural light", natural
"Natural light", aurora "Auril's Aurora", chill "Cold table". Category accent colours
(web CSS, use for a thin left rule or label tint, never cartoon icons): cold/chill
#9fe3ff, hazard/travel #ffb4c2, flame #ffc999, concentration #d5c4ff, natural #c7b9ff,
aurora #7dffcf, others #e0f4ff.

Cold tiers: Cold DC 10 @ 0°F, Frigid Cold 15 @ −40, Severe Cold 20 @ −80, Extreme Cold
25 @ −110, Deathly Cold 28 @ −130 (DC ±1 per 10°F). Tier accent colours: cold #c2e8fb,
frigid #a9dcff, severe #9fbcff, extreme #cdb3ff, deathly #ffc2cf.

## 4. Engine behaviour (summary; signatures in engine.h)

* **Chain**: `newChain(seed)` = `{rngState: hashSeed(seed), current: null, dayCount: 0}`.
  `nextDay` resumes the RNG at `rngState`. Day 1: d12 picks the opening event (cause
  genesis), then its x d4. When an event's hours run out, the next event (from `next[]`
  chosen by the duration roll: 1d4 1-2/3/4 -> 0/1/2, 2d4 2-4/5-6/7-8 -> 0/1/2) is rolled
  immediately, even at hour 24 (it then opens the next day at 00:00 with elapsed 0). A
  new day never re-rolls the d12. One seed drives the whole campaign; days are
  regenerated from the seed on load (only `dayCount` is stored).
* **Segments** per day, **log** = segments whose event starts that day, **carriedIn** =
  the event in progress at midnight (copy), **carryOut** = the event after the day.
* **Temperature**: hour h of day d (1-based) is
  `round(EVENTS[id].tempF + tempOffsetAt(hashSeed(seed+"|temp"), (d-1)*24 + h))` where the
  offset is ±6°F smooth value noise (knots every 4 absolute hours, smoothstep) plus a
  diurnal ±3°F (−3 at 02:00, +3 at 12:00). The trace (`tempAt`) uses fractional hours.
* **Light** at hour h (`lightAt`): windows Moon 20-06, Twilight 10-14, Aurora 22-02 (end
  exclusive). Event light none -> "No natural light"; lightning -> "Dim light — rapid
  lightning"; dim -> sources in order "<moon> Moon", "Auril's Aurora", "Twilight" ->
  "Dim light — <sources joined by ', '>", or none open -> level dark "Between light
  cycles: no natural source". `auroraVisible = natural && aurora window` (false for
  none/lightning events).
* **Cold** `coldDC(tempF)`: last tier with anchor ≥ tempF (Cold if warmer than 0°F);
  colder than anchor: dc + floor((anchor − t)/10); warmer (only above 0°F):
  max(0, dc − floor((t − anchor)/10)).
* **°C**: `round((f − 32) * 5 / 9)` with JS rounding. All displayed temperatures go
  through `minus()` (U+2212).

## 5. Calendar of Harptos and Selûne (harptos.js / calendar.js)

* abs 0 = 1 Hammer 0 DR. 12 months × 30 days (three tendays). Festivals after a month:
  Midwinter (after Hammer), Greengrass (Tarsakh), Midsummer (Flamerule), Shieldmeet (after
  Midsummer, leap years = year mod 4 == 0), Highharvestide (Eleint), Feast of the Moon
  (Uktar). Month common names: Deepwinter, The Claw of Winter, The Claw of the Sunsets, The
  Claw of the Storms, The Melting, The Time of Flowers, Summertide, Highsun, The Fading,
  Leaffall, The Rotting, The Drawing Down.
* format: "1 Hammer 1489 DR" / short "1 Hammer"; festival "Midwinter, 1489 DR" / short
  "Midwinter".
* monthOf(abs): a festival is shown on the page of the month it follows.
* Moon: period 30.4375 d, full at the first instant of 1 Hammer 1372 DR, offset (Settings,
  −30..30, step 0.5) shifts phases later. "New Moon"/"Full Moon" belong only to the single
  day holding the instant; other days get the octant name (Waxing Crescent, First Quarter,
  Waxing Gibbous, Waning Gibbous, Last Quarter, Waning Crescent). `illum` drives the disc.
* Moon disc (moonSvg, 20×20 box, r=8): dark disc fill rgba(8,20,38,.9) (new moon:
  rgba(30,50,80,.9)), lit part #eef7ff, rim rgba(210,235,255,.42) 1px (new moon: #d9f0ff
  1.3px + glow). illum > 0.985 full circle; > 0.015: lit limb on the right while waxing,
  left while waning, bounded by the outer half circle and a terminator half-ellipse of
  half-width `8·|1 − 2·illum|` (bulging into the dark half for gibbous, into the lit half
  for crescent); otherwise no lit part. Native: draw with GDI+ antialiased paths.
* Every new moon carries the built-in mark "New moon — sacrifice to Auril" (chip
  "Sacrifice to Auril", type auril, flake mark), always first in a day's marks.
* Recurrence (`occursOn`): never before the anchor; newmoon/fullmoon repeats fall only on
  new/full-moon days (the anchor only if it is one); yearly = same month+day, or same
  festival; monthly = same day of month (never festivals); tenday = same day of tenday
  (never festivals); none = anchor day only. Repeat labels: Never, Every new moon, Every
  full moon, Every tenday, Every month, Every year.
* Event types: Natural #56e6b6, Unnatural #b897ff, Auril #e6f7ff, Reminder #ffc46b;
  sort priority auril, unnatural, natural, reminder, then anchor abs.

## 6. State, actions and persistence

The state struct, clean() rules, JSON schema, key order, v1 migration and import/export are
specified in `store.h` (header comment). Summary of the state: seed, started, startAbs,
dayCount, current (party's today, −1 = not begun), currentAbs, view (day on screen),
selAbs (calendar selection), time (0..24), unit, moonOffset, yearNames, notes, events,
ui {chainNoteHidden, calYear, calMonth}. Runtime: playing, busy.

### 6.1 Actions (web)

| Action | Effect |
|---|---|
| Advance Day / Begin the Expedition | see `alm_advance`. Web animation: old night tweens to 24:00 over clamp((24−t)·0.25, 1.2, 4) s (cosine ease), lands at 00:00 on the new day, then tweens to 12:00 over 2.4 s (first day: lands then 2.6 s). Button shows "Night Runs Out…" / "Dawn Breaks…" (first: "Rolling the First Dawn…"), sub "continuing the same chain". Then day toast "Today · <date>" (first: "The expedition begins · <date>") if the day has marks, save, announce. |
| Click calendar day | select(abs, fromCalendar) |
| Double-click calendar day / Shift+Enter / "Set as current day" | setCurrent(abs) |
| Prev/next day arrows | stepDay(∓1) |
| Timeline click/drag | stop playback, time = x/width·24 (clamped < 24), save on release |
| Timeline keys (focused) | Right/Up +1 h, Left/Down −1 h (clamped 0..23), Home 0, End 23 → time = h + 0.5; Space/Enter = play/pause |
| Play/Pause | 1.25 s per game hour; plays on through recorded days up to today (or to the last generated day when viewing beyond today), then stops at 23:59 and the Advance button pulses if the viewed day is today |
| °F / °C toggle | unit, re-render, save |
| Sound toggle | on/off; label "Sound on"/"Sound off"; "No audio" if unavailable. Not persisted (native: persisted) |
| Chain note X | hide note (Settings can bring it back) |
| Calendar ‹ › | page months (wrapping years), debounced save |
| Calendar "Today" | select(today ?? start); tooltip "Go to the current day" / "Go to the expedition start" |
| Upcoming item | select(abs) |
| Seed chip / Settings / "Campaign settings…" | open Settings (seed chip focuses + selects the seed field) |

### 6.2 Native hour/day stepping (in-game requirement)

* **Right arrow, Space, "Next hour" button**: +1 hour on the viewed day (time becomes the
  whole hour, clock "HH:00"). At 23:00 it crosses midnight: viewing a day other than today
  -> the next day at 00:00; viewing today -> Advance Day, landing at 00:00 (not 12:00)
  because the DM is stepping hour by hour. `alm_step_hour`.
* **Left arrow**: −1 hour; at 00:00 goes to the previous day at 23:00 (if any).
* **Home / End**: 00:00 / 23:00.
* **N, Enter (when no text field has focus), "Advance day" button**: Advance Day
  (lands at 12:00 like the web). No confirmation; the transition is instant or a ≤ 400 ms
  cosmetic clock sweep that never blocks input (input during it completes it at once).
* **PageUp / PageDown**: view previous / next day (stepDay). **T**: calendar Today.
* **P**: play/pause. **U**: toggle °F/°C. **M**: sound on/off. **F2** or **Ctrl+,**:
  Settings. **Esc**: close dialog/form; when a text field has focus, Esc returns focus to
  the main view (notes are flushed). Keys are ignored by the main view while a text
  field (notes, event form) has focus.
* Hour/day steps stop playback, save immediately, never show a dialog.

### 6.3 When to save

After every state-changing action (web saves synchronously; calendar paging after 250 ms;
notes after 600 ms idle or on blur; on pause; on exit (`WM_CLOSE`/`WM_ENDSESSION`); once at
boot after load+clean). Native save file: `%APPDATA%\IcewindDaleAlmanac\state.json`
(UTF-8, compact JSON + "native" key, atomic replace). A corrupt file is renamed to
`state.json.bad` before the fresh state is saved. Portable mode: if
`state.json` exists next to the exe, use that instead. The web's multi-tab merge
(`storage` event / adoptStored) has no native equivalent.

### 6.4 Toasts (exact; `<b>` = bold)

Default 4.2 s, day toasts 7 s, bottom centre, never modal, never steals focus; a new toast
replaces the old one; click dismisses.

* Day toast: "<lead> · <date>" + chips of the day's marks; leads "The expedition begins",
  "Today", "Today is now".
* "Today is now <b>DATE</b>"
* "The expedition begins on <b>DATE</b>. Change the start date in Settings to begin earlier."
* "Beyond the almanac: at most 3650 days can be recorded." (setCurrent / day panel)
* "Beyond the almanac: at most 3650 days of weather can be recorded." (select)
* "<b>TITLE</b> added to the almanac" / "<b>TITLE</b> updated" / "<b>TITLE</b> removed"
* "Weather regenerated from <b>DATE</b> · seed SEED"
* "Backup restored · N day(s), M event(s)" (singular "day"/"event" for 1)

## 7. Main view: derived values (exact strings)

### 7.1 Hero (read-aloud area)

* Date line: moon disc (24 px) + "1 Hammer 1489 DR" + tag pill: "Today" (bright pill),
  "Tomorrow"/"In 3 days" (ahead), "Yesterday"/"3 days ago" (past), "Expedition start"
  (intro). Abs shown = view day, or start in the intro.
* Marks of that day as chips (built-in first).
* Meta line: "Day N of the expedition" (intro: "Before the First Dawn") · moon phase name ·
  clock "HH:MM" (`fmtClock`, minutes floored) · light badge "Dim light" / "Between light
  cycles" / "No natural light" (colours: dim #e0f4ff, dark #c7b9ff, none #ffb4c2) ·
  aurora badge "Auril's Aurora visible" (#7dffcf) when visible.
* Event name (largest text), flavour (italic, secondary).
* Temperature: big number `minus(toUnit(tempF))`, unit "°F"/"°C", note "killing cold"
  (≤ −100°F) / "flesh-freezing" (≤ −80°F) / "air temperature", tier pill
  "<Tier name> DC <dc>" (DC bold), tooltip "Cold table: DC <tier dc> at <anchor>, <step>".
* Intro (no days or view < 0): name "The Dale Awaits", flavour "Begin the expedition to
  roll the first dawn. After that, every day grows out of the night before.", no
  temperature, wind/severity/light "—", forecast "Awaiting the d12", effects note "no
  weather yet", effects placeholder "Effects of the current weather will appear here, hour
  by hour.", scene = Clear Skies look with aurora.

### 7.2 Stats

* Wind: label + 4-pip thin meter, lit pips = level + (level ? 1 : 0) (0, 2, 3, 4).
* Severity: label + 5-pip meter lit = severity; at 5 (Lethal) lit pips turn #ff9fb2.
* Natural light: light label + " — Blinded" when level none.
* Forecast: "<b>Next name</b> at HH:00[ tomorrow] (in N h · Xd4 rolled D)" (parenthesis
  muted). `iwd_fmt_forecast`.

### 7.3 Effects in play

Header note "<count> in play · <event name>". Items (category label small caps above the
text) in order: Auril's Aurora effect if visible, or the dark-hour Natural light effect;
then "Cold table" (chill); then the event's effects. Texts in `engine.h`
(`iwd_hour_effects`). The chill item is visually emphasised (it changes every hour). The
whole list must be visible without scrolling at the default size (max 19 items, White
Out): single-column, compact rows; if space runs out, use two columns before scrolling.

### 7.4 Advance control

`alm_controls`: main/sub texts (see store.h), chained note "Days are chained: tomorrow
continues whatever tonight leaves behind. No re-seed." with a close X (tooltip "Hide this
note (bring it back in Settings)"). Beckon = subtle pulse/highlight (native: a static
accent border until the next action; no animation timer required).

### 7.5 Timeline "The Day's Weather"

* Day navigator "<short date> · Day N" / "No days yet", ‹ › buttons, play/pause button
  (tooltip "Play the day" / "Pause").
* Segment bar 0-24 h: one block per segment in the event colour (vertical gradient from
  colour+white 8% to colour darkened toward #0b2440 30% is fine, or flat), 1px dark
  separators, label = event name when the block is ≥ 64 px wide; dark ink #04182b on light
  colours, light ink #e9f6ff on ids 3 and 4. Hatched 8 px edge (`cont-l`/`cont-r`) when the
  segment continues from the previous / into the next day. Tooltip:
  "<name> · HH:00–HH:00 · Xd4 [r1+r2] = D h[ · continues from the previous day][ ·
  continues into the next day]" (end 24 shows "24:00").
* Playhead: 2 px white line + time label "HH:MM", label flips to the left past 88%.
* Ticks every 3 h: "00 03 06 … 21 24".
* Cycle bands (three thin tracks labelled Moon, Twilight, Aurora): the window
  (rgba(200,235,255,.1)) and, on top, the lit part: Moon/Twilight where the hour's event
  light is 'dim' (lightning does not count); Aurora where auroraVisible. Lit colours:
  moon #dfe9ff→#b8c8ff, twilight #ffd6de→#ffe9d2, aurora #7dffcf→#8fb8ff.
* Temperature trace above the bar (70 px): y maps −38°F (top margin 16) .. −142°F
  (bottom margin 6); sampled at h+0.2 and h+0.8 for each hour plus both ends, joined with
  horizontal-tangent cubic Béziers (control x = midpoint), soft area fill below
  (#bfe6fb, 55% → 0); grid lines at −50/−90/−130°F (°C mode: −45/−65/−85°C converted)
  labelled with fmtTemp at the left; per-segment labels (segments ≥ 2 h, ≥ 30 px apart)
  showing fmtTemp of the hour at the segment middle, placed 6 px above the highest of
  three samples; a dot at the current time. Native: GDI+ antialiased 1.5-2 px line, cached
  bitmap per (day, unit, width).
* Empty: "No days recorded yet — begin the expedition."

### 7.6 Dice ledger (viewed day only)

* Header "Day N · <date>".
* If the day starts mid-event: time "00:00", "<event> continues", "carried over from Day
  N−1 · R of D h remain → <next>" (R = duration − elapsed of carriedIn).
* Each log entry: time HH:00; genesis "[d12 die value] d12 opens with <event>",
  transition "after <previous>: <event>"; rolls line "Xd4 [r][r] = D h → <label> <next>"
  where label is "always" for Wind Vortex, else "1st option:"/"2nd option:"/"3rd option:".
  Dice drawn as small numbered marks (plain boxed digits; no cartoon die art).
* Empty: "No rolls yet".

## 8. Secondary panes: calendar, day, notes, ledger

Secondary content lives below or beside the live weather (tabs "Calendar", "Day & notes",
"Ledger" are fine) and never covers it.

### 8.1 Calendar of Harptos

* Head: month name, common name, year "1489 DR", year name (italic) if set, ‹ › and
  "Today".
* Grid: 10 columns × 3 rows (tendays), then festival cells (eyebrow "Festival", festival
  name instead of the number). `alm_month_cells`.
* Cell content: day number, marks (new-moon flake, note glyph), moon disc; if weather
  exists: dominant event (colour swatch/line in its colour, not an icon) + min temp
  fmtTemp; up to 2 event chips (type colour left rule + title) and "+N". States: today
  (strong bright border + "Today" tag), selected (bright border), before start (40%
  opacity), new moon (lighter top glow), start (aurora-green left edge + "Start" tag),
  has event (2 px underline in the first user event's type colour).
* Click selects, double-click (or Shift+Enter) sets today. Keyboard in the grid:
  arrows move (±1, ±10, down from the third row goes to festivals), Home/End.
* Legend: Natural, Unnatural, Auril, Reminder chips, "New moon · sacrifice to Auril",
  hint "Click a day to view it · double-click to make it today".

### 8.2 Upcoming

"Upcoming" + note ("next ten days from today" / "from the expedition start"); items with
rel ("Today", "Tomorrow", "In 3 days") + short date + chips; empty "Nothing marked for the
next ten days.". Click selects.

### 8.3 Day panel (selected day)

`alm_day_panel` gives eyebrow, date, sub, moon line, weather summary (dominant name, event
sequence, colour strip, temperature range, cold DC range) or the "no weather" text, and the
"Set as current day" button state. Then:

* "Events & reminders": marks list; user events show title, meta (`alm_fmt_mark_meta`),
  details, Edit and Delete (tooltip "Delete" or "Delete (all occurrences)" for repeats).
  Delete is two-step: first press arms (red, tooltip "Press again to delete"), auto-disarms
  after 3 s. Empty: "No events or reminders on this day.". Button "Add event / reminder".
* Event form (inline, not modal): title "New event or reminder" / "Edit event"; Title
  (max 120, placeholder "e.g. Auril's followers attack Bryn Shander", required: empty
  title marks the field invalid and refocuses); Type radio Natural/Unnatural/Auril/Reminder
  (default Natural); Repeat select (labels above, default Never); Details (optional,
  multiline, max 2000); buttons "Cancel" and "Add to the almanac" / "Save changes". Esc
  cancels. New events anchor on selAbs; edits keep their anchor and id. Selecting another
  day closes an unsaved *new* event form.
* Notes: label "Notes" + state "editing…" / "saved"; multiline, placeholder "What happened
  on this day? Rumours, debts, oaths, frostbite…"; autosave 600 ms after typing stops and
  on blur / day change / settings / advance; blank text deletes the note.

## 9. Settings dialog (modal; the only modal besides file pickers)

Title "Settings". Groups (exact labels):

1. "Campaign start" - help "The first day of the expedition: weather day 1 and the
   starting point of the seeded chain." Fields Day (1-30; disabled for festivals), "Month
   or festival" (18 options, `alm_start_option`), "Year (DR)". Error line (red #ffb4c2).
2. "Expedition seed" - help "One seed drives the whole campaign. Each new day continues
   the same chain, and the weather carries over from the previous night." Field Seed
   (monospace, max 40) + "Random" button (fills a random seed, disarms).
3. "Year names" - "Year (DR)" (initially the calendar's year; changing it reloads the
   name) + "Name (optional)" (max 80, placeholder "e.g. Year of the Warrior Princess").
4. "Temperature" °F/°C and switch "Show the chained-days note".
5. "Advanced" (collapsed) - "Selûne offset (days)" (−30..30 step 0.5) + help "Shifts the
   moon's phases. Positive values make them arrive later. Canon: full at the first
   midnight of 1 Hammer 1372 DR, 30 days 10½ hours per cycle."
6. "Backup" - "Export backup", "Import backup", message line (default "Everything in one
   JSON file: seed, dates, notes, events and settings."; after export "Backup saved as
   <file>"; import errors "That file is not an almanac backup." / "Could not read that
   file.").

Buttons "Cancel" / "Apply". Apply validates the date (`alm_settings_apply`); when the start
date or seed changes and weather exists, the first Apply shows the red confirmation box and
turns the button into "Regenerate weather" (warning style); editing the date/seed fields or
pressing Random disarms it; the second Apply regenerates. Seed is trimmed + upper-cased;
empty = random. Apply also stores unit, chain-note visibility, moon offset (invalid -> 0,
clamped) and the year name (empty deletes). Esc/Cancel/close discard. Export writes the
backup via a Save As dialog (default name `icewind-almanac-<SEED>-<YYYYMMDD>.json`); import
uses an Open dialog (`*.json`) and replaces the whole state immediately (no confirmation in
the web; native: same, it is user-initiated through a file dialog).

## 10. Native layout (in-game use)

Default client size 1280×860 logical px (raised from 760 so a 19-effect White Out fits
without scrolling) (scaled by DPI, clamped to the monitor work area;
remember the last window rect in "native"). Minimum 960×640. Without scrolling, at the
default size, the DM must see: the scene, event name, temperature + tier/DC, wind,
natural light, severity, forecast and the current hour's effects.

Suggested arrangement (top to bottom):

1. Thin title bar row: app name (small caps, restrained), seed (click -> Settings), °F/°C,
   Sound, Settings. ~36 px.
2. Live band (~60% of height): left ~60%: the scene image (cover-fit, 3:1 source) with
   the hero text drawn over a dark gradient at its bottom-left (date line, event name
   ~34-40 px, flavour, big temperature ~72 px with unit, tier pill). Right ~40%: Advance
   button ("Advance to 2 Hammer" + sub), "Next hour" button, then Wind / Severity /
   Natural light / Forecast rows with thin meters, then the effects list.
3. Timeline strip (~150 px): day navigator, play, cycle bands, temperature trace, bar.
4. Secondary area (rest, may scroll internally): tabs Calendar | Day & notes | Ledger
   (+ Upcoming within Calendar).

Visual language: dark slate background (#0b111b family as in the current native app),
restrained typography (Segoe UI / Segoe UI Semibold, a serif such as Georgia or
Cambria only for the event name and date if desired), high-contrast white for read-aloud
items, muted blue-grey for labels, thin 1-2 px rules, small radii (≤ 4 px) or square
corners, no glassmorphism, no pastel rounded cards, no emoji or cartoon vector icons.
Colour is carried by thin bars, rules and text tints. Antialiased GDI+ only where curves
appear (moon discs, temperature trace, meters' ends). Respect the bright-sky rule below
for text over the scene.

## 11. Scene: what the web draws and how to approximate it

The web's sky canvas, frost canvas, particle snow/hail, vortex motes, lightning bolts,
huts, fog strips, sigil spin, shimmer, icicles, frost-burst and swap-in animations are
purely decorative and are replaced by the raster scenes `assets/wNN.jpg` (event NN,
720×240, already night-lit; w01 has a baked aurora and moon, w02/w05 a moon, w10 a bolt,
w12 the vortex). Static image per viewed hour's event; no animation while idle.

How scene.js varies the look (use as cheap overlays on the cached scaled image, recomputed
only when the hour/event changes):

* Per-event params `LOOKS` (`iwd_look`): cloud, cloudDark, snow, flake, wind, hail, fog,
  whiteout, lightning, vortex, stars, moon (PDF moon fraction 0.5/0.75/1), dark,
  extinguish. Around event changes the web blends: the next event builds in during the last
  40% of an hour, the previous echoes through the first 25% (`iwd_look_at`); cloud/dark
  take the max and stars/moon the min of ±0.9 h, precipitation the min of ±0.3 h. Native:
  blending is optional (whole-hour stepping lands on .0 where only the echo applies);
  a 150-250 ms cross-fade between two scaled images on event change is acceptable only if
  it uses a timer that stops afterwards.
* Time of day: twilight `band(h,10,14,1.6)` lifts and warms the sky (night top #030610
  -> #1c3454, horizon #0a1a34 -> #a8c4d6 at 85%) and adds a rose glow (255,190,200) at the
  horizon with alpha 0.22·twi·(1 − 0.8·cloud). Native: add a soft warm horizon gradient
  and +10-15% brightness scaled by `twi·(1 − 0.8·cloud)`.
* Moon: visible alpha `moon·band(h,20,6,1.2)·(1 − 0.85·cloud)`, arcs east (20:00) to west
  (06:00): x = W·(0.85 − 0.7·prog), y = H·(0.42 − 0.3·sin(π·prog)), prog = ((h−20+24)%24)/10.
  Native: images with a baked moon keep it; optionally dim the baked moon region outside
  20-06 by a dark radial overlay.
* Aurora: alpha `natural·band(h,22,2,0.8)·(1 − 0.85·cloud)`; green/cyan/violet curtains
  in the top third. Native: for natural events add a faint additive green→violet
  top gradient (≤ 20% alpha) when the aurora badge shows; for w01 outside the aurora
  window overlay a sky-coloured top gradient (~60%) to subdue the baked aurora (or ship a
  13th aurora-free variant from tools/gen_assets.py; +~50 KB).
* Darkness: overall dim `1 − 0.55·dark` and vignette alpha 0.55 + 0.25·dark. Native:
  multiply by ~(1 − 0.35·dark) when level is "dark" between light cycles, plus a cached
  vignette bitmap.
* Lightning: flashes and thunder at random 0.8-4.3 s intervals when lightning > 0.25.
  Native: static (w10 has a bolt); thunder audio optional while sound is on.
* Cold frost on the glass: opacity 0.3 + 0.75·cold, cold = clamp((−tempF(event) − 45)/85).
  Native: optional static frosted-edge vignette scaled by cold.
* Bright sky: glare = whiteout + fog of the current look; > 0.45 (on) / < 0.38 (off,
  hysteresis) switches UI inks to high contrast: darker panels behind text
  (rgba(4,13,26,.74)), labels #d6e6f3 / #bcd0e2, text shadows. In practice: Blizzard,
  White Out, Extreme Fog. Native: when glare > 0.45 draw the hero text on a stronger dark
  gradient/scrim.

Wind sound: the wind level is the look's `wind` of the viewed hour (eased); see audio.h.

## 12. Ambiguities resolved / native deviations

1. **Hour stepping** (not in the web, required at the table): Right/Space step whole
   hours (time = h, clock HH:00) instead of the web's h + 0.5; crossing 23→00 on today
   advances the day and lands at 00:00; other days just move on (6.2). Advance Day itself
   lands at 12:00 as in the web.
2. **Transitions** are instant (or a short non-blocking sweep) instead of the web's
   2.6-6.4 s tween, so "busy" is normally never true; the API keeps it for an optional
   animation.
3. **Playback** may step once per game hour (1.25 s) instead of continuously; the
   playhead jumps hourly. The stop time is 23.99 as in the web.
4. **Save file** instead of localStorage; v1 detection by content (`v` absent or 1),
   extra "native" key (ignored by the web, never exported); sound preference persisted.
5. **Seed upper-casing** uses 1:1 case mapping (no 'ß' -> "SS"); seeds imported from a
   backup are kept verbatim, so parity holds for imported campaigns.
6. **Date range**: abs/year limited to years −99999..99999; out-of-range stored values
   fall back as non-integers would.
7. **UTF-16 slicing** of title/details/year name never leaves half a surrogate pair.
8. **Export file name** sanitises characters Windows forbids.
9. **Notes**: native caps a single note at 256 KiB (web unlimited); events capped at
   20000.
10. **Multi-tab merge** (web `storage` event) is not applicable.
11. **Screen-reader announcement** (`announce`) may be exposed via the window's accessible
    name or simply used as the tooltip of the hero; optional.
12. Scene moon in the image is the PDF moon fraction (½/¾/Full), matching the light label;
    the calendar/hero discs show the real Selûne phase - as in the web.
13. Dominant-event ties go to the lowest id (JS key order + stable sort).

## 13. Tests (native/test)

* `test/reference/engine.js`, `harptos.js`, `scene.js`, `calendar.js`: unmodified copies of
  the web files (scene.js and calendar.js load in node without a DOM and supply paramsFor,
  LOOKS, dominant, monthOf and relDays to the parity script).
* Parity driver: node script prints, for several seeds (ASCII, empty, non-BMP) and
  1000+ days, every segment, hour id, temperature, min/max, carry state, light label,
  coldDC, effects (incl. chill text in F and C), forecast; and for abs ranges spanning
  negative years, leap years and festivals: fromAbs/toAbs/format/monthLayout, moon
  (several offsets incl. 0.5 and −30), occursOn for every repeat; the C test prints the
  same and the outputs are compared byte for byte (every double as its IEEE-754 bits).
  Implemented as `test/parity.js` + `test/parity.c` + `test/gen_seeds.js`, run by
  `test/run.sh` (default 320 seeds x 64 days, plus 6 seeds x 3650 days).
* Store: load a JSON produced by the web's snapshot()/export (fixture made in node by
  running app.js's clean+snapshot logic), check clean() edge cases (dayCount 1e10, -5,
  "12", NaN; current out of range; currentAbs null; time 24; unit "c"; moonOffset 99;
  note keys "012", "-3", "x"; events with bad type/repeat, blank title, long title with
  emoji), re-serialise and compare with JSON.stringify output byte for byte; export ->
  import round trip; v1 migration.
