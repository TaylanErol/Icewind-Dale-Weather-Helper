#!/usr/bin/env node
/* Parity harness, JavaScript side.
 * Loads the reference web code (native/test/reference/*.js) and prints canonical lines
 * that native/test/parity.c must reproduce byte for byte.
 *
 *   node parity.js SEEDS_FILE DAYS LONG_DAYS > js.txt
 *
 * SEEDS_FILE: one seed per line, hex encoded UTF-8 (an empty line is the empty seed).
 * Every double is printed as the 16 hex digits of its IEEE-754 bits, next to the value
 * the UI shows (rounded the way the UI rounds it).
 *
 * The few app.js helpers used below cannot be loaded from app.js (it needs a DOM), so they
 * are copied verbatim from js/app.js and marked "app.js".
 */
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

globalThis.window = globalThis;
const REF = path.join(__dirname, 'reference');
for (const f of ['engine.js', 'harptos.js', 'scene.js', 'calendar.js']) {
  vm.runInThisContext(fs.readFileSync(path.join(REF, f), 'utf8'), { filename: f });
}
const { EVENTS, WIND } = IWD;
const H = Harptos;
const Cal = IWDCal;

const [seedFile, daysArg, longArg] = process.argv.slice(2);
const DAYS = Number(daysArg || 60);
const LONG_DAYS = Number(longArg || 0);

const lines = [];
const out = (a) => { lines.push(JSON.stringify(a)); if (lines.length >= 4096) flush(); };
function flush() { if (lines.length) { fs.writeSync(1, lines.join('\n') + '\n'); lines.length = 0; } }

const hb = Buffer.alloc(8);
const hx = (x) => { hb.writeDoubleBE(x); return hb.toString('hex'); };

// ---------- app.js helpers (verbatim, with S.unit passed in as `unit`) ----------
const pad = (n) => String(n).padStart(2, '0');
const fmtHour = (h) => pad(((Math.floor(h) % 24) + 24) % 24) + ':00';
function fmtClock(t) {
  const tt = Math.min(t, 23.999);
  const h = Math.floor(tt), m = Math.floor((tt - h) * 60);
  return pad(h) + ':' + pad(m);
}
const toUnit = (f, unit) => (unit === 'C' ? Math.round(((f - 32) * 5) / 9) : f);
const minus = (n) => String(n).replace('-', '−');
const fmtTemp = (f, unit) => minus(toUnit(f, unit)) + '°';
const tierAnchor = (t, unit) => (unit === 'C' ? `${minus(Math.round(((t.f - 32) * 5) / 9))}°C (${minus(t.f)}°F)` : minus(t.f) + '°F');
const dcStep = (unit) => (unit === 'C' ? '±1 per 10°F (≈5.6°C)' : '±1 per 10°F');
function inkFor(hex) {
  const n = parseInt(hex.slice(1), 16);
  const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b < 140 ? 'dark-ink' : '';
}
const SEVERITY = { 1: 1, 2: 1, 5: 1, 9: 2, 3: 2, 10: 3, 6: 3, 4: 3, 11: 3, 12: 4, 7: 4, 8: 5 };
const SEV_LABEL = ['—', 'Calm', 'Uneasy', 'Dangerous', 'Deadly', 'Lethal'];
const CAT_LABEL = {
  sight: 'Sight', hearing: 'Hearing', ranged: 'Ranged attacks', attack: 'Attacks', cold: 'Extreme cold',
  travel: 'Travel', flame: 'Extinguishing winds', concentration: 'Concentration', hazard: 'Hazard',
  light: 'Unnatural light', natural: 'Natural light', aurora: "Auril's Aurora", chill: 'Cold table',
};
const OPTION_NAME = ['1st', '2nd', '3rd'];
function segmentAt(day, h) { return day.segments.find((s) => h >= s.start && h < s.end); }
function tempAt(seed, day, t) {
  const tt = Math.max(0, Math.min(23.999, t));
  return EVENTS[day.hours[Math.floor(tt)].id].tempF + IWD.tempOffset(seed, day.day - 1, tt);
}
// renderHour(): the effects list, the forecast and the light stat, as plain text
function effectsFor(hr, unit) {
  const ev = EVENTS[hr.id];
  const light = hr.light;
  const cold = IWD.coldDC(hr.tempF);
  const fx = ev.effects.slice();
  const delta = cold.dc - cold.tier.dc;
  fx.unshift({ cat: 'chill', text: `Cold table: ${cold.tier.name} — DC ${cold.dc} at ${minus(toUnit(hr.tempF, unit))}°${unit} (DC ${cold.tier.dc} at ${tierAnchor(cold.tier, unit)}, ${dcStep(unit)}` +
    (delta ? `; ${delta > 0 ? '+' : '−'}${Math.abs(delta)} here)` : ')') });
  if (light.level === 'dark') {
    fx.unshift({ cat: 'natural', text: 'Between light cycles: no moon, twilight or aurora this hour. The rules name no natural light source here; the [Sight] effects below still apply (DM may rule it darkness).' });
  }
  if (light.auroraVisible) {
    fx.unshift({ cat: 'aurora', text: "Auril's Aurora is visible. Characters who see it during a rest are magically healed: −1 exhaustion level and +1d4 HP per hour." });
  }
  return fx;
}
function forecastText(day, h) {
  const seg = segmentAt(day, h);
  const ev = EVENTS[seg.id];
  const nextEv = EVENTS[seg.next];
  const at = seg.changeAt;
  const left = at - h;
  const when = at >= 24 ? fmtHour(at) + ' tomorrow' : fmtHour(at);
  return `${nextEv.name} at ${when} (in ${left} h · ${ev.dice}d4 rolled ${seg.duration})`;
}
const lightValue = (light) => light.label + (light.level === 'none' ? ' — Blinded' : '');
const lightBadge = (light) => (light.level === 'dim' ? 'Dim light' : light.level === 'dark' ? 'Between light cycles' : 'No natural light');
// scene.js band() is internal to the scene module: verbatim copy
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const smooth = (t) => t * t * (3 - 2 * t);
function band(h, from, to, fade) {
  const len = (((to - from) % 24) + 24) % 24 || 24;
  const x = (((h - from) % 24) + 24) % 24;
  const d = x < len ? Math.min(x, len - x) : -Math.min(x - len, 24 - x);
  return smooth(clamp(0.5 + d / fade, 0, 1));
}
const LOOK_KEYS = ['cloud', 'cloudDark', 'snow', 'flake', 'wind', 'hail', 'fog', 'whiteout',
  'lightning', 'vortex', 'stars', 'moon', 'dark', 'extinguish'];

// ---------- canonical encodings ----------
const cause = (c) => (c.type === 'genesis' ? [0, c.d12] : [1, c.from]);
const act = (a) => [a.id, a.rolls, a.duration, a.elapsed, a.nextIdx, a.next, ...cause(a.cause), a.startDay, a.startHour];

// ---------- seeds ----------
const seeds = fs.readFileSync(seedFile, 'utf8').split('\n');
if (seeds.length && seeds[seeds.length - 1] === '') seeds.pop();
const seedText = seeds.map((h) => Buffer.from(h, 'hex').toString('utf8'));

seedText.forEach((seed, si) => {
  out(['K', si, IWD.hashSeed(seed), IWD.hashSeed(String(seed) + '|temp'), seed.length]);
  const chain = IWD.newChain(seed);
  const days = [];
  const states = [];
  for (let i = 0; i < DAYS; i++) { days.push(IWD.nextDay(chain)); states.push(chain.rngState); }
  days.forEach((d, di) => {
    out(['D', si, d.day, states[di], d.tempMin, d.tempMax, Cal.dominant(d),
      d.carriedIn ? act(d.carriedIn) : null, act(d.carryOut)]);
    d.segments.forEach((s) => out(['S', si, d.day, s.id, s.start, s.end, s.duration, s.rolls, s.next, s.nextIdx,
      s.elapsedBefore, s.fromPrevDay, s.intoNextDay, s.changeAt, ...cause(s.cause)]));
    d.log.forEach((e) => out(['L', si, d.day, e.hour, ...act(e)]));
    d.hours.forEach((hr) => {
      const off = IWD.tempOffset(seed, d.day - 1, hr.hour);
      out(['H', si, d.day, hr.hour, hr.id, hr.light.level, hr.light.label, hr.light.auroraVisible,
        hr.light.moon, hr.light.twilight, hr.light.aurora, hr.tempF, hx(off), forecastText(d, hr.hour),
        lightValue(hr.light), lightBadge(hr.light)]);
    });
    if (di < 3) {
      const ts = [];
      for (let k = -2; k <= 100; k++) {
        const t = k / 4 + 0.07;
        const v = tempAt(seed, d, t);
        ts.push(hx(v), Math.round(v));
      }
      out(['T', si, d.day, ts]);
    }
  });
  // scene looks blended across day boundaries (eventAt walks into neighbouring days)
  if (si < 40) {
    const eventAt = (v, hour) => {
      let dd = v, h = Math.floor(hour);
      while (h < 0) { dd--; h += 24; }
      while (h >= 24) { dd++; h -= 24; }
      if (dd < 0) return 1;
      if (dd >= days.length) return days[days.length - 1].carryOut.id;
      return days[dd].hours[h].id;
    };
    for (let v = 0; v < Math.min(4, days.length); v++) {
      for (let k = 0; k < 240; k += 3) {
        const t = k / 10;
        const p = IWDScene.paramsFor((h) => eventAt(v, h), t);
        out(['P', si, v, k, LOOK_KEYS.map((key) => hx(p[key]))]);
      }
    }
  }
  if (si < 6 && LONG_DAYS > 0) {
    const r = IWD.rebuild(seed, LONG_DAYS);
    const sh = [];
    r.days.forEach((d) => {
      sh.push(d.segments.length, Cal.dominant(d), d.tempMin, d.tempMax, d.hours.map((x) => x.id).join(','),
        d.hours.map((x) => x.tempF).join(','));
    });
    out(['X', si, r.chain.rngState, r.chain.dayCount, act(r.chain.current), sh.join(';')]);
  }
});
flush();

// ---------- effects for every event x hour, and the cold table line for every temperature ----------
for (let id = 1; id <= 12; id++) {
  const ev = EVENTS[id];
  out(['V', id, ev.name, ev.dice, ev.wind, WIND[ev.wind].label, WIND[ev.wind].level + (WIND[ev.wind].level ? 1 : 0),
    ev.tempF, ev.light, ev.moon === undefined ? null : ev.moon, ev.natural, ev.next, ev.flavor, SEVERITY[id],
    SEV_LABEL[SEVERITY[id]], Cal.COLORS[id], inkFor(Cal.COLORS[id]) !== '',
    [0, 1, 2].map((i) => (ev.next[0] === ev.next[1] && ev.next[1] === ev.next[2] ? 'always' : OPTION_NAME[i] + ' option:')),
    IWDScene.LOOKS[id] ? LOOK_KEYS.map((k) => hx(IWDScene.LOOKS[id][k])) : null]);
  for (let h = 0; h < 24; h++) {
    for (const unit of ['F', 'C']) {
      const hr = { hour: h, id, light: IWD.lightAt(ev, h), tempF: -50 };
      const fx = effectsFor(hr, unit);
      out(['E', id, h, unit, fx.map((f) => [f.cat, CAT_LABEL[f.cat], f.text]), fx.length + ' in play · ' + ev.name]);
    }
  }
}
for (let t = -260; t <= 90; t++) {
  for (const unit of ['F', 'C']) {
    const cold = IWD.coldDC(t);
    const fx = effectsFor({ hour: 12, id: 3, light: IWD.lightAt(EVENTS[3], 12), tempF: t }, unit)[0];
    out(['C', t, unit, fx.text, cold.tier.key, cold.tier.name, cold.dc, cold.delta,
      `Cold table: DC ${cold.tier.dc} at ${tierAnchor(cold.tier, unit)}, ${dcStep(unit)}`,
      t <= -100 ? 'killing cold' : t <= -80 ? 'flesh-freezing' : 'air temperature',
      fmtTemp(t, unit), toUnit(t, unit), minus(t)]);
  }
}
flush();

// ---------- light at fractional / negative / huge hours ----------
for (let id = 1; id <= 12; id++) {
  for (const h of [-49.5, -24, -1, -0.5, 0, 0.99, 1.5, 5.999, 6, 13.7, 21.2, 23.99, 24, 25.5, 47, 1e6 + 0.5]) {
    const l = IWD.lightAt(EVENTS[id], h);
    out(['G', id, String(h), l.level, l.label, l.auroraVisible, l.moon, l.twilight, l.aurora]);
  }
}

// ---------- temperature model at awkward hours ----------
{
  const shs = ['', 'RIME-00FF', 'x'].map((s) => s);
  shs.forEach((s, i) => {
    const vals = [];
    for (const d of [-3, -1, 0, 1, 7, 3649, 100000]) {
      for (const h of [0, 0.5, 1.999, 2, 2.25, 11.5, 12, 12.001, 18.75, 23, 23.999]) {
        vals.push(hx(IWD.tempOffset(s, d, h)));
      }
    }
    out(['O', i, vals]);
  });
}

// ---------- Math.round and the °C conversion ----------
{
  const xs = [0, -0.5, 0.5, -1.5, 1.5, -2.5, 2.5, -50.5, -50.49999999999999, 0.49999999999999994, -0.49999999999999994,
    4503599627370495.5, -4503599627370495.5, 4503599627370497, 1e300, -1e300, 123.456, -123.456, 2.5000000000000004, -2.4999999999999996];
  out(['R', xs.map((x) => [hx(x), hx(Math.round(x))])]);
  const cs = [];
  for (let f = -400; f <= 300; f++) cs.push(toUnit(f, 'C'));
  out(['FC', cs]);
}

// ---------- RNG ----------
{
  const st = [0, 1, 0xdeadbeef, 0xffffffff, 0x6d2b79f5, 2105444690];
  st.forEach((s) => {
    const r = IWD.createRng(s);
    const v = [];
    for (let i = 0; i < 64; i++) v.push(hx(r.next()));
    const r2 = IWD.createRng(s);
    const dice = [];
    for (let i = 0; i < 64; i++) dice.push(r2.die([2, 4, 12, 100, 20][i % 5]));
    out(['N', s, v, dice, r2.state]);
  });
  // randomSeed(), with the entropy made explicit
  const words = ['RIME', 'FROST', 'AURIL', 'HOAR', 'SLEET', 'GLACIER', 'TUNDRA', 'BOREAL', 'SPINE', 'MAER', 'DUN', 'CAIRN', 'KELVIN', 'BREMEN', 'TERMALAINE', 'ICEFANG'];
  const rs = [];
  for (const [a, b] of [[0, 0], [15, 65535], [16, 65536], [0xffffffff, 0xffffffff], [123456789, 987654321], [7, 0xabc]]) {
    rs.push(words[a % words.length] + '-' + (b % 0x10000).toString(16).toUpperCase().padStart(4, '0'));
  }
  out(['W', rs]);
}

// ---------- calendar ----------
{
  const absList = [];
  for (let a = -3000; a <= 3000; a++) absList.push(a);
  for (let a = 543858 - 800; a <= 543858 + 800; a++) absList.push(a);
  for (let a = -36524634; a < -36524634 + 400; a++) absList.push(a);
  for (let a = 36524999 - 400; a <= 36524999; a++) absList.push(a);
  let s = 12345;
  for (let i = 0; i < 20000; i++) {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    absList.push(-36524634 + (s % (36524999 + 36524634 + 1)));
  }
  absList.forEach((a) => {
    const d = H.fromAbs(a);
    const mo = Cal.monthOf(a);
    out(['A', a, d.year, d.month, d.day, d.festival, d.dayOfYear, d.tendayIndex, d.dayOfTenday,
      H.format(a), H.format(a, { short: true }), mo.year, mo.month, H.isLeap(d.year), H.yearStart(d.year), H.daysInYear(d.year)]);
  });
  flush();
  const years = [];
  for (let y = -405; y <= -395; y++) years.push(y);
  for (let y = -9; y <= 9; y++) years.push(y);
  years.push(1370, 1371, 1372, 1488, 1489, 1490, 1491, 1492, 99996, 99997, 99998, 99999, -99999, -99998, -99997, -99996);
  years.forEach((y) => {
    const r = [];
    for (let m = -1; m <= 12; m++) for (const dd of [-1, 0, 1, 10, 11, 15, 30, 31]) {
      r.push(H.toAbs({ year: y, month: m, day: dd }));
    }
    H.FESTIVALS.forEach((f) => r.push(H.toAbs({ year: y, festival: f.key })));
    r.push(H.toAbs({ year: y, festival: 'bogus' }));
    out(['Y', y, r]);
    for (let m = 0; m < 12; m++) {
      const L = H.monthLayout(y, m);
      out(['M', y, m, L.name, L.common, L.days[0], L.days[29], L.festivals.map((f) => [f.abs, f.key, f.name])]);
    }
  });
  // out-of-range years are rejected natively (the web would accept them)
  // moon
  const offs = [0, 0.5, -3.25, 15.21875, 30.4375, -100.1, 1e-9, 7.77, NaN];
  const moonAbs = [];
  for (let a = 543858 - 400; a <= 543858 + 400; a++) moonAbs.push(a);
  for (let a = -3000; a <= 3000; a += 7) moonAbs.push(a);
  for (let i = 0; i < 2000; i++) {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    moonAbs.push(-36524634 + (s % (36524999 + 36524634 + 1)));
  }
  moonAbs.push(-36524634, 36524999, 501123, 501138, 501139);
  moonAbs.forEach((a) => {
    const r = [];
    offs.forEach((o, oi) => {
      if (oi > 2 && (a % 5)) return;
      const m = H.moon(a, o);
      r.push([hx(m.age), hx(m.phase), hx(m.illum), Math.round(m.illum * 100), m.waxing, m.name, m.isNew, m.isFull]);
    });
    out(['Q', a, r]);
  });
  flush();
  // recurrence
  const anchors = [543858, 543858 + 30, 543858 + 31, H.toAbs({ year: 1489, festival: 'midwinter' }),
    H.toAbs({ year: 1492, festival: 'shieldmeet' }), H.toAbs({ year: 1492, festival: 'midsummer' }),
    H.toAbs({ year: 1489, month: 11, day: 30 }), H.toAbs({ year: 1489, month: 4, day: 10 }), -400, 0, -1,
    H.toAbs({ year: -4, festival: 'shieldmeet' }), H.toAbs({ year: 1490, month: 6, day: 20 })];
  const reps = ['none', 'newmoon', 'fullmoon', 'tenday', 'monthly', 'yearly'];
  anchors.forEach((an) => {
    reps.forEach((rp) => {
      [0, 2.5, -14].forEach((o) => {
        const ev = { abs: an, repeat: rp };
        const occ = H.occurrences(ev, an - 60, rp === 'yearly' ? an + 3000 : an + 900, o);
        out(['U', an, rp, o, occ, H.occursOn(ev, an - 1, o), H.occursOn(ev, an, o)]);
      });
    });
  });
  out(['B', H.REPEATS.map((r) => [r.key, r.label]), H.MONTHS.map((m) => [m.name, m.common]),
    H.FESTIVALS.map((f) => [f.key, f.name, f.after, !!f.leapOnly])]);
}
flush();

// ---------- small text helpers ----------
{
  const hs = [];
  for (let h = -50; h <= 60; h++) hs.push(fmtHour(h));
  const cl = [];
  for (let k = 0; k <= 2000; k++) cl.push(fmtClock(k * 0.0125));
  cl.push(fmtClock(23.999), fmtClock(24), fmtClock(30), fmtClock(12.5), fmtClock(0.016666666666666666));
  const rd = [];
  for (let n = -40; n <= 40; n++) rd.push(Cal.relDays(n));
  rd.push(Cal.relDays(1000000), Cal.relDays(-1000000));
  const mn = [];
  for (const n of [0, 5, -5, -2147483647, 2147483647, 100, -100]) mn.push(minus(n));
  const pd = [];
  for (let n = 0; n <= 120; n++) pd.push(pad(n));
  out(['Z', hs, cl, rd, mn, pd]);
  const bs = [];
  const fades = [1.6, 1.2, 0.8, 0.3];
  const wins = [[10, 14], [20, 6], [22, 2], [0, 24], [5, 5], [-3, 30]];
  for (const [f, t] of wins) for (const fd of fades) for (let k = -10; k <= 250; k += 3) bs.push(hx(band(k / 10, f, t, fd)));
  out(['BAND', bs]);
}
flush();
