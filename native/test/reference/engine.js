/* Icewind Dale weather engine.
 * Rules transcribed from "ICEWIND DALE WEATHER.pdf":
 *   - d12 picks the opening event, then each event rolls xd4 for its duration.
 *   - The duration roll also picks the next event (** table): shorter half -> 1st option,
 *     longer half split between 2nd and 3rd. Wind Vortex (12) always goes to Clear Skies (1).
 * Continuity: one seeded RNG drives the whole campaign. A new day never re-rolls the d12;
 * it resumes the chain exactly where the previous day stopped (mid-event if needed).
 */
(function (root) {
  'use strict';

  // ---------- Seeded RNG ----------
  function hashSeed(str) {
    // cyrb53-style string hash -> 32-bit unsigned
    let h1 = 0xdeadbeef ^ str.length, h2 = 0x41c6ce57 ^ str.length;
    for (let i = 0; i < str.length; i++) {
      const ch = str.charCodeAt(i);
      h1 = Math.imul(h1 ^ ch, 2654435761);
      h2 = Math.imul(h2 ^ ch, 1597334677);
    }
    h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
    h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
    return (h1 ^ h2) >>> 0;
  }

  // mulberry32 with an exposed state so the chain can be resumed.
  function createRng(state) {
    let s = state >>> 0;
    return {
      next() {
        s = (s + 0x6d2b79f5) >>> 0;
        let t = s;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
      },
      die(sides) { return 1 + Math.floor(this.next() * sides); },
      get state() { return s; },
    };
  }

  // ---------- Rules data ----------
  const WIND = {
    none:     { label: 'No Wind',       level: 0 },
    mild:     { label: 'Mild Wind',     level: 1 },
    moderate: { label: 'Moderate Wind', level: 2 },
    severe:   { label: 'Severe Wind',   level: 3 },
  };

  // Effect categories map to icons in the UI.
  // sight | hearing | ranged | attack | cold | travel | flame | concentration | hazard | light
  const NO_LIGHT = [
    { cat: 'sight',  text: 'No natural light: creatures relying on it are Blinded.' },
    { cat: 'sight',  text: 'No [Sight] Perception checks.' },
    { cat: 'attack', text: 'Disadvantage on attack rolls.' },
  ];
  const DIM_SIGHT = { cat: 'sight', text: 'Disadvantage on [Sight] Perception checks.' };
  const COLD_NO_RESIST = { cat: 'cold', text: 'Extreme cold (creatures without natural cold resistance): DC 10 CON save every hour, +1 exhaustion on a fail.' };
  const COLD_EVEN_RESIST = [
    { cat: 'cold', text: 'Extreme cold affects even creatures with natural cold resistance.' },
    { cat: 'cold', text: 'With cold resistance: DC 10 CON save every hour, +1 exhaustion on a fail.' },
    { cat: 'cold', text: 'Without cold resistance: DC 15 CON save every hour, +1 exhaustion on a fail.' },
  ];
  const RANGED = { cat: 'ranged', text: 'Disadvantage on ranged attack rolls.' };
  const HEARING = { cat: 'hearing', text: 'Disadvantage on [Hearing] Perception checks.' };
  const EXTINGUISH = { cat: 'flame', text: 'Extinguishing winds: open flames, fog, tracks in the snow, and non-magical flying are impossible.' };
  const CONCENTRATION = { cat: 'concentration', text: 'At the end of each turn, concentration spells require a DC 10 CON save to maintain.' };

  // light: 'dim' (from moon/aurora/twilight), 'none', or 'lightning'
  // natural: the PDF's asterisk — natural light can reach the party (and Auril's Aurora can be seen).
  const EVENTS = [
    null,
    { id: 1, name: 'Clear Skies', dice: 2, wind: 'none', tempF: -50, light: 'dim', moon: '½', natural: true,
      next: [2, 5, 9], flavor: 'The sky is a frozen lens. Every star burns cold and close.',
      effects: [DIM_SIGHT] },
    { id: 2, name: 'Light Cloud Coverage', dice: 1, wind: 'mild', tempF: -55, light: 'dim', moon: '¾', natural: true,
      next: [1, 3, 5], flavor: 'Thin veils of cloud drift across the stars like breath on glass.',
      effects: [DIM_SIGHT] },
    { id: 3, name: 'Overcast', dice: 1, wind: 'moderate', tempF: -65, light: 'none', natural: false,
      next: [2, 4, 11], flavor: 'A low iron lid of cloud seals the dale. The world goes grey, then black.',
      effects: [RANGED, ...NO_LIGHT] },
    { id: 4, name: 'Storm Clouds', dice: 1, wind: 'severe', tempF: -80, light: 'none', natural: false,
      next: [3, 6, 10], flavor: 'Bruised thunderheads roll off the Spine of the World, howling as they come.',
      effects: [RANGED, HEARING, ...NO_LIGHT, COLD_NO_RESIST] },
    { id: 5, name: 'Light Snow Dusting', dice: 1, wind: 'none', tempF: -60, light: 'dim', moon: 'Full', natural: true,
      next: [2, 6, 10], flavor: 'Fine snow sifts down in perfect silence, glittering where the light finds it.',
      effects: [DIM_SIGHT] },
    { id: 6, name: 'Heavy Snowfall', dice: 1, wind: 'mild', tempF: -70, light: 'none', natural: false,
      next: [3, 7, 11], flavor: 'Fat flakes fall thick as wool, burying tracks within minutes.',
      effects: [...NO_LIGHT, COLD_NO_RESIST] },
    { id: 7, name: 'Blizzard', dice: 2, wind: 'moderate', tempF: -90, light: 'none', natural: false,
      next: [4, 6, 8], flavor: "Auril's fury made manifest: a screaming wall of ice and wind.",
      effects: [
        RANGED, HEARING,
        { cat: 'hearing', text: 'Hearing range reduced to 100 ft.' },
        ...NO_LIGHT,
        { cat: 'sight', text: 'Characters without goggles are Blinded even in lit conditions.' },
        { cat: 'light', text: 'Visibility from unnatural light reduced to 30 ft.' },
        COLD_NO_RESIST, EXTINGUISH,
        { cat: 'travel', text: 'Veering off course: the navigator makes a DC 15 Survival check every hour while travelling.' },
        { cat: 'travel', text: 'Fail with 14–10: the party makes no progress toward its destination.' },
        { cat: 'travel', text: 'Fail with 9 or lower: one random untied party member is lost.' },
        { cat: 'travel', text: 'Finding a lost member: group DC 15 Survival check every hour.' },
        CONCENTRATION,
      ] },
    { id: 8, name: 'White Out', dice: 1, wind: 'severe', tempF: -130, light: 'none', natural: false,
      next: [5, 6, 7], flavor: 'Sky, ground and horizon dissolve into one blinding, killing white.',
      effects: [
        RANGED, HEARING,
        { cat: 'hearing', text: 'Hearing range reduced to 30 ft.' },
        ...NO_LIGHT,
        { cat: 'sight', text: 'Characters without goggles are Blinded even in lit conditions.' },
        { cat: 'light', text: 'Visibility from unnatural light reduced to 10 ft.' },
        ...COLD_EVEN_RESIST, EXTINGUISH,
        { cat: 'travel', text: 'Veering off course: the navigator makes a DC 18 Survival check every hour while travelling.' },
        { cat: 'travel', text: 'Fail with 17–15: the party makes no progress toward its destination.' },
        { cat: 'travel', text: 'Fail with 14–10: one random untied party member is lost.' },
        { cat: 'travel', text: 'Fail with 9 or lower: one random tied party member is lost.' },
        { cat: 'travel', text: 'Finding a lost member: group DC 18 Survival check every hour.' },
        CONCENTRATION,
      ] },
    { id: 9, name: 'Extreme Fog', dice: 1, wind: 'none', tempF: -55, light: 'dim', moon: 'Full', natural: true,
      next: [1, 5, 12], flavor: 'Freezing fog pours across the tundra; lanterns become ghosts at arm’s length.',
      effects: [
        DIM_SIGHT,
        { cat: 'light', text: 'Visibility from unnatural light reduced to 5 ft.' },
        { cat: 'travel', text: 'Veering off course: the navigator makes a DC 12 Survival check every hour while travelling.' },
        { cat: 'travel', text: 'Fail with 11–8: the party makes no progress toward its destination.' },
        { cat: 'travel', text: 'Fail with 7 or lower: one random untied party member is lost.' },
        { cat: 'travel', text: 'Finding a lost member: group DC 12 Survival check every hour.' },
      ] },
    { id: 10, name: 'Lightning Storm', dice: 2, wind: 'mild', tempF: -60, light: 'lightning', natural: false,
      next: [4, 5, 6], flavor: 'Violet lightning stalks the tundra, and the thunder shakes snow from the peaks.',
      effects: [
        { cat: 'sight', text: 'Dim natural light from rapid lightning: disadvantage on [Sight] Perception checks.' },
        { cat: 'hazard', text: 'Every hour, each player caught in the open tundra rolls a d100.' },
        { cat: 'hazard', text: 'On a 001: DC 15 DEX save — 8d8 lightning damage on a fail, half on a success.' },
        { cat: 'hazard', text: 'Each creature within 15 ft of the strike also makes a DC 15 DEX save — half the full damage on a fail, none on a success.' },
        { cat: 'hazard', text: 'Every hour, booming thunder has a 50% chance to trigger an avalanche in the mountains.' },
        HEARING,
      ] },
    { id: 11, name: 'Hail Storm', dice: 1, wind: 'severe', tempF: -80, light: 'none', natural: false,
      next: [3, 6, 7], flavor: 'Stones of ice the size of knuckles hammer down out of the dark.',
      effects: [
        RANGED, HEARING, ...NO_LIGHT, COLD_NO_RESIST,
        { cat: 'hazard', text: 'Icy monstrosity: DC 10 DEX save every hour — 1d12 bludgeoning on a fail, half on a success.' },
      ] },
    { id: 12, name: 'Wind Vortex', dice: 1, wind: 'severe', tempF: -110, light: 'dim', moon: '½', natural: true,
      next: [1, 1, 1], flavor: 'A spinning column of wind tears across the dale, dragging the snow into the sky.',
      effects: [RANGED, HEARING, DIM_SIGHT, ...COLD_EVEN_RESIST, EXTINGUISH] },
  ];

  // ---------- Light cycles (hours, 24h clock) ----------
  const CYCLES = {
    moon:     { label: 'Moon Cycle',     from: 20, to: 6 },
    twilight: { label: 'Twilight',       from: 10, to: 14 },
    aurora:   { label: "Auril's Aurora", from: 22, to: 2 },
  };

  function inWindow(h, w) {
    return w.from < w.to ? h >= w.from && h < w.to : h >= w.from || h < w.to;
  }

  function lightAt(ev, hour) {
    const h = ((Math.floor(hour) % 24) + 24) % 24;
    const moon = inWindow(h, CYCLES.moon);
    const twilight = inWindow(h, CYCLES.twilight);
    const aurora = inWindow(h, CYCLES.aurora);
    const auroraVisible = !!ev.natural && aurora;

    if (ev.light === 'none') {
      return { level: 'none', label: 'No natural light', sources: [], auroraVisible: false, moon, twilight, aurora };
    }
    if (ev.light === 'lightning') {
      return { level: 'dim', label: 'Dim light — rapid lightning', sources: ['Rapid lightning'], auroraVisible: false, moon, twilight, aurora };
    }
    const sources = [];
    if (moon) sources.push(ev.moon + ' Moon');
    if (aurora) sources.push("Auril's Aurora");
    if (twilight) sources.push('Twilight');
    if (!sources.length) {
      return { level: 'dark', label: 'Between light cycles: no natural source', sources, auroraVisible, moon, twilight, aurora };
    }
    return { level: 'dim', label: 'Dim light — ' + sources.join(', '), sources, auroraVisible, moon, twilight, aurora };
  }

  // ---------- Temperature ----------
  // Hourly air temperature wanders around the event's PDF value: smooth value-noise (±6°F,
  // knots every 4 h on the absolute campaign hour) plus a small diurnal swing (−3°F at 02:00,
  // +3°F at 12:00). Separate hash, so the weather chain RNG is never touched.
  const NOISE_AMP = 6, DIURNAL_AMP = 3, KNOT_HOURS = 4;

  function knot(sh, n) {
    let x = Math.imul(n | 0, 0x9e3779b1) ^ sh;
    x = Math.imul(x ^ (x >>> 16), 0x85ebca6b);
    x = Math.imul(x ^ (x >>> 13), 0xc2b2ae35);
    x ^= x >>> 16;
    return ((x >>> 0) / 4294967295) * 2 - 1;
  }

  function tempHash(seedText) { return hashSeed(String(seedText) + '|temp'); }

  function diurnal(hour) {
    const h = ((hour % 24) + 24) % 24;
    if (h >= 2 && h <= 12) return -DIURNAL_AMP * Math.cos(Math.PI * (h - 2) / 10);
    return DIURNAL_AMP * Math.cos(Math.PI * (((h - 12) + 24) % 24) / 14);
  }

  // Offset in °F (float) at a fractional absolute hour: dayIndex * 24 + hour of day.
  function tempOffsetAt(sh, absHour) {
    const k = absHour / KNOT_HOURS, n = Math.floor(k), f = k - n;
    const s = f * f * (3 - 2 * f);
    return NOISE_AMP * (knot(sh, n) * (1 - s) + knot(sh, n + 1) * s) + diurnal(absHour);
  }

  function tempOffset(seedText, dayIndex, hour) { return tempOffsetAt(tempHash(seedText), dayIndex * 24 + hour); }

  // ---------- Cold table (DC ±1 per 10°F away from the tier's anchor) ----------
  const COLD_TIERS = [
    { key: 'cold',    name: 'Cold',         dc: 10, f: 0,    c: -20 },
    { key: 'frigid',  name: 'Frigid Cold',  dc: 15, f: -40,  c: -40 },
    { key: 'severe',  name: 'Severe Cold',  dc: 20, f: -80,  c: -60 },
    { key: 'extreme', name: 'Extreme Cold', dc: 25, f: -110, c: -80 },
    { key: 'deathly', name: 'Deathly Cold', dc: 28, f: -130, c: -100 },
  ];

  function coldDC(tempF) {
    let tier = COLD_TIERS[0];
    for (const t of COLD_TIERS) if (t.f >= tempF) tier = t;
    const dc = tempF > tier.f
      ? Math.max(0, tier.dc - Math.floor((tempF - tier.f) / 10))
      : tier.dc + Math.floor((tier.f - tempF) / 10);
    return { tier, dc, delta: dc - tier.dc };
  }

  // ---------- Transition rule ----------
  // 1d4: 1–2 -> [0], 3 -> [1], 4 -> [2].  2d4: 2–4 -> [0], 5–6 -> [1], 7–8 -> [2].
  function nextIndexFor(dice, duration) {
    if (dice === 1) return duration <= 2 ? 0 : duration === 3 ? 1 : 2;
    return duration <= 4 ? 0 : duration <= 6 ? 1 : 2;
  }

  function startEvent(rng, id, cause, day, hour) {
    const ev = EVENTS[id];
    const rolls = [];
    for (let i = 0; i < ev.dice; i++) rolls.push(rng.die(4));
    const duration = rolls.reduce((a, b) => a + b, 0);
    const nextIdx = nextIndexFor(ev.dice, duration);
    return {
      id, rolls, duration, elapsed: 0,
      nextIdx, next: ev.next[nextIdx],
      cause, startDay: day, startHour: hour,
    };
  }

  // ---------- Chain ----------
  function newChain(seedText) {
    const seed = String(seedText);
    return { seed, rngState: hashSeed(seed), current: null, dayCount: 0 };
  }

  // Generates the next 24h for a chain, mutating it so the following call resumes seamlessly.
  function nextDay(chain) {
    const rng = createRng(chain.rngState);
    const dayNo = chain.dayCount + 1;
    const log = [];
    const carriedIn = chain.current ? { ...chain.current, rolls: chain.current.rolls.slice() } : null;

    let cur = chain.current;
    if (!cur) {
      const d12 = rng.die(12);
      cur = startEvent(rng, d12, { type: 'genesis', d12 }, dayNo, 0);
      log.push({ hour: 0, ...cur });
    } else if (cur.elapsed === 0) {
      // rolled as yesterday ended, begins exactly at this midnight
      log.push({ hour: 0, ...cur });
    }

    const segments = [];
    let hour = 0;
    while (hour < 24) {
      const remaining = cur.duration - cur.elapsed;
      const take = Math.min(remaining, 24 - hour);
      segments.push({
        id: cur.id,
        start: hour,
        end: hour + take,
        duration: cur.duration,
        rolls: cur.rolls,
        next: cur.next,
        nextIdx: cur.nextIdx,
        elapsedBefore: cur.elapsed,
        fromPrevDay: cur.elapsed > 0 && hour === 0,
        intoNextDay: hour + take >= 24 && cur.elapsed + take < cur.duration,
        changeAt: hour + remaining, // hour (relative to this day, may exceed 24) the event ends
        cause: cur.cause,
      });
      hour += take;
      cur.elapsed += take;
      if (cur.elapsed >= cur.duration) {
        const from = cur.id;
        cur = startEvent(rng, cur.next, { type: 'transition', from }, dayNo, hour % 24);
        if (hour < 24) log.push({ hour, ...cur });
        else cur.startDay = dayNo + 1; // begins exactly at the next midnight
      }
    }

    chain.current = cur;
    chain.rngState = rng.state;
    chain.dayCount = dayNo;

    const sh = tempHash(chain.seed);
    const hours = [];
    let tempMin = Infinity, tempMax = -Infinity;
    for (let h = 0; h < 24; h++) {
      const seg = segments.find((s) => h >= s.start && h < s.end);
      const tempF = Math.round(EVENTS[seg.id].tempF + tempOffsetAt(sh, (dayNo - 1) * 24 + h));
      tempMin = Math.min(tempMin, tempF);
      tempMax = Math.max(tempMax, tempF);
      hours.push({ hour: h, id: seg.id, light: lightAt(EVENTS[seg.id], h), tempF });
    }

    return { day: dayNo, seed: chain.seed, segments, hours, tempMin, tempMax, log, carriedIn, carryOut: { ...cur, rolls: cur.rolls.slice() } };
  }

  function rebuild(seedText, count) {
    const chain = newChain(seedText);
    const days = [];
    for (let i = 0; i < count; i++) days.push(nextDay(chain));
    return { chain, days };
  }

  function randomSeed() {
    const words = ['RIME', 'FROST', 'AURIL', 'HOAR', 'SLEET', 'GLACIER', 'TUNDRA', 'BOREAL', 'SPINE', 'MAER', 'DUN', 'CAIRN', 'KELVIN', 'BREMEN', 'TERMALAINE', 'ICEFANG'];
    const buf = new Uint32Array(2);
    if (root.crypto && root.crypto.getRandomValues) root.crypto.getRandomValues(buf);
    else { buf[0] = Math.floor(Math.random() * 2 ** 32); buf[1] = Math.floor(Math.random() * 2 ** 32); }
    return words[buf[0] % words.length] + '-' + (buf[1] % 0x10000).toString(16).toUpperCase().padStart(4, '0');
  }

  const api = { EVENTS, WIND, CYCLES, COLD_TIERS, lightAt, inWindow, nextIndexFor, newChain, nextDay, rebuild, randomSeed, hashSeed, createRng, coldDC, tempOffset };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.IWD = api;
})(typeof window !== 'undefined' ? window : globalThis);
