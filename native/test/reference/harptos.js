/* Calendar of Harptos (Forgotten Realms) + the phases of Selûne.
 * Dates are integer absolute day numbers: abs 0 = 1 Hammer 0 DR. Negative years work too.
 * 12 months of 30 days (three tendays each) plus festival days that belong to no month.
 * Shieldmeet follows Midsummer in years divisible by 4, giving 366 days.
 */
(function (root) {
  'use strict';

  const MONTHS = [
    { name: 'Hammer',    common: 'Deepwinter' },
    { name: 'Alturiak',  common: 'The Claw of Winter' },
    { name: 'Ches',      common: 'The Claw of the Sunsets' },
    { name: 'Tarsakh',   common: 'The Claw of the Storms' },
    { name: 'Mirtul',    common: 'The Melting' },
    { name: 'Kythorn',   common: 'The Time of Flowers' },
    { name: 'Flamerule', common: 'Summertide' },
    { name: 'Eleasis',   common: 'Highsun' },
    { name: 'Eleint',    common: 'The Fading' },
    { name: 'Marpenoth', common: 'Leaffall' },
    { name: 'Uktar',     common: 'The Rotting' },
    { name: 'Nightal',   common: 'The Drawing Down' },
  ];

  // after: index of the month the festival follows. Shieldmeet follows Midsummer.
  const FESTIVALS = [
    { key: 'midwinter',      name: 'Midwinter',         after: 0 },
    { key: 'greengrass',     name: 'Greengrass',        after: 3 },
    { key: 'midsummer',      name: 'Midsummer',         after: 6 },
    { key: 'shieldmeet',     name: 'Shieldmeet',        after: 6, leapOnly: true },
    { key: 'highharvestide', name: 'Highharvestide',    after: 8 },
    { key: 'feastofthemoon', name: 'Feast of the Moon', after: 10 },
  ];
  const FEST = {};
  FESTIVALS.forEach((f) => { FEST[f.key] = f; });

  const mod = (a, n) => ((a % n) + n) % n;

  function isLeap(y) { return mod(y, 4) === 0; }
  function daysInYear(y) { return isLeap(y) ? 366 : 365; }
  // abs of 1 Hammer of year y; floor((y + 3) / 4) counts leap years in [0, y) (negative below 0).
  function yearStart(y) { return 365 * y + Math.floor((y + 3) / 4); }

  // Day-of-year tables (0-based index) for common and leap years.
  function buildTable(leap) {
    const list = [];
    const offsets = { month: [], fest: {} };
    for (let m = 0; m < 12; m++) {
      offsets.month[m] = list.length;
      for (let d = 1; d <= 30; d++) list.push({ month: m, day: d, festival: null });
      FESTIVALS.forEach((f) => {
        if (f.after !== m || (f.leapOnly && !leap)) return;
        offsets.fest[f.key] = list.length;
        list.push({ month: null, day: null, festival: f.key });
      });
    }
    return { list, offsets };
  }
  const TABLES = [buildTable(false), buildTable(true)];
  const table = (y) => TABLES[isLeap(y) ? 1 : 0];

  // Returns null for impossible dates (e.g. Shieldmeet in a common year, day 31).
  function toAbs(date) {
    const y = Math.floor(Number(date.year));
    if (!isFinite(y)) return null;
    const t = table(y);
    let off;
    if (date.festival) {
      off = t.offsets.fest[date.festival];
      if (off === undefined) return null;
    } else {
      const m = Number(date.month), d = Number(date.day);
      if (!(m >= 0 && m < 12 && d >= 1 && d <= 30) || m % 1 || d % 1) return null;
      off = t.offsets.month[m] + d - 1;
    }
    return yearStart(y) + off;
  }

  function yearOf(abs) {
    let y = Math.floor(abs / 365.25);
    while (yearStart(y) > abs) y--;
    while (yearStart(y + 1) <= abs) y++;
    return y;
  }

  function fromAbs(abs) {
    abs = Math.floor(abs);
    const year = yearOf(abs);
    const doy = abs - yearStart(year);
    const e = table(year).list[doy];
    const inMonth = e.festival === null;
    return {
      abs, year,
      month: e.month, day: e.day, festival: e.festival,
      dayOfYear: doy + 1,
      tendayIndex: inMonth ? Math.floor((e.day - 1) / 10) : null,
      dayOfTenday: inMonth ? ((e.day - 1) % 10) + 1 : null,
    };
  }

  function format(abs, opts) {
    const short = !!(opts && opts.short);
    const d = fromAbs(abs);
    if (d.festival) return FEST[d.festival].name + (short ? '' : ', ' + d.year + ' DR');
    return d.day + ' ' + MONTHS[d.month].name + (short ? '' : ' ' + d.year + ' DR');
  }

  function festivalName(key) { return FEST[key] ? FEST[key].name : ''; }

  function monthLayout(year, month) {
    const t = table(year);
    const base = yearStart(year) + t.offsets.month[month];
    const days = [];
    for (let i = 0; i < 30; i++) days.push(base + i);
    const festivals = FESTIVALS
      .filter((f) => f.after === month && t.offsets.fest[f.key] !== undefined)
      .map((f) => ({ abs: yearStart(year) + t.offsets.fest[f.key], key: f.key, name: f.name }));
    return { year, month, name: MONTHS[month].name, common: MONTHS[month].common, days, festivals };
  }

  // ---------- Selûne ----------
  // Canon: full at the very start of 1 Hammer 1372 DR, 30 days 10.5 hours per cycle.
  const MOON_PERIOD = 30.4375;
  const FULL_EPOCH = yearStart(1372);
  const NEW_EPOCH = FULL_EPOCH + MOON_PERIOD / 2;
  const PHASES = ['New Moon', 'Waxing Crescent', 'First Quarter', 'Waxing Gibbous',
    'Full Moon', 'Waning Gibbous', 'Last Quarter', 'Waning Crescent'];

  // true if an instant epoch + k * period falls in [abs, abs + 1)
  function hitsDay(epoch, abs) {
    const k = Math.ceil((abs - epoch) / MOON_PERIOD);
    return epoch + k * MOON_PERIOD < abs + 1;
  }

  function moon(abs, offsetDays) {
    abs = Math.floor(abs);
    const off = Number(offsetDays) || 0;
    const age = mod(abs + 0.5 - (NEW_EPOCH + off), MOON_PERIOD);
    const phase = age / MOON_PERIOD;
    const illum = (1 - Math.cos(2 * Math.PI * phase)) / 2;
    const waxing = age < MOON_PERIOD / 2;
    const isNew = hitsDay(NEW_EPOCH + off, abs);
    const isFull = hitsDay(FULL_EPOCH + off, abs);
    let oct = Math.floor(phase * 8 + 0.5) % 8;
    // 'New Moon' / 'Full Moon' belong to the single day holding the instant.
    if (oct === 0 && !isNew) oct = waxing ? 1 : 7;
    if (oct === 4 && !isFull) oct = waxing ? 3 : 5;
    if (isNew) oct = 0;
    if (isFull) oct = 4;
    return { age, phase, illum, waxing, name: PHASES[oct], isNew, isFull };
  }

  // ---------- Recurrence ----------
  const REPEATS = [
    { key: 'none',     label: 'Never' },
    { key: 'newmoon',  label: 'Every new moon' },
    { key: 'fullmoon', label: 'Every full moon' },
    { key: 'tenday',   label: 'Every tenday' },
    { key: 'monthly',  label: 'Every month' },
    { key: 'yearly',   label: 'Every year' },
  ];

  function occursOn(event, abs, moonOffset) {
    const start = Math.floor(event.abs);
    abs = Math.floor(abs);
    if (abs < start) return false;
    // moon repeats fall only on the moon's own days, the anchor day included when it is one
    if (event.repeat === 'newmoon') return moon(abs, moonOffset).isNew;
    if (event.repeat === 'fullmoon') return moon(abs, moonOffset).isFull;
    if (abs === start) return true;
    switch (event.repeat) {
      case 'yearly': {
        const e = fromAbs(start), d = fromAbs(abs);
        return e.festival ? d.festival === e.festival : d.month === e.month && d.day === e.day;
      }
      case 'monthly': {
        const e = fromAbs(start), d = fromAbs(abs);
        return !e.festival && !d.festival && d.day === e.day;
      }
      case 'tenday': {
        const e = fromAbs(start), d = fromAbs(abs);
        return !e.festival && !d.festival && d.dayOfTenday === e.dayOfTenday;
      }
      default: return false;
    }
  }

  // abs days in [from, to] on which the event occurs
  function occurrences(event, from, to, moonOffset) {
    const out = [];
    for (let a = Math.max(Math.floor(from), Math.floor(event.abs)); a <= to; a++) {
      if (occursOn(event, a, moonOffset)) out.push(a);
    }
    return out;
  }

  root.Harptos = {
    MONTHS, FESTIVALS, REPEATS, PHASES, MOON_PERIOD,
    isLeap, daysInYear, yearStart, toAbs, fromAbs, format, festivalName, monthLayout,
    moon, occursOn, occurrences,
  };
})(typeof window !== 'undefined' ? window : globalThis);
