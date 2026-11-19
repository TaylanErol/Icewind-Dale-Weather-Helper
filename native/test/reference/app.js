(function () {
  'use strict';

  const { EVENTS, WIND, CYCLES, inWindow, coldDC } = window.IWD;
  const H = window.Harptos;
  const Cal = window.IWDCal;
  const Scene = window.IWDScene;
  const STORE = 'iwd-weather-v2';
  const STORE_V1 = 'iwd-weather-v1';
  const MAX_DAYS = 3650;
  const SECONDS_PER_HOUR = 1.25;   // playback speed
  const SETTLE_HOUR = 12;          // a fresh day settles mid-twilight (10–14) after the transition
  const DEFAULT_START = H.toAbs({ year: 1489, month: 0, day: 1 });

  const COLORS = Cal.COLORS;
  const SEVERITY = { 1: 1, 2: 1, 5: 1, 9: 2, 3: 2, 10: 3, 6: 3, 4: 3, 11: 3, 12: 4, 7: 4, 8: 5 };
  const SEV_LABEL = ['—', 'Calm', 'Uneasy', 'Dangerous', 'Deadly', 'Lethal'];
  const CAT_LABEL = {
    sight: 'Sight', hearing: 'Hearing', ranged: 'Ranged attacks', attack: 'Attacks', cold: 'Extreme cold',
    travel: 'Travel', flame: 'Extinguishing winds', concentration: 'Concentration', hazard: 'Hazard',
    light: 'Unnatural light', natural: 'Natural light', aurora: "Auril's Aurora", chill: 'Cold table',
  };
  const OPTION_NAME = ['1st', '2nd', '3rd'];

  const $ = (id) => document.getElementById(id);
  const el = {
    seedChip: $('seedChip'), seedValue: $('seedValue'), settingsBtn: $('settingsBtn'), resetBtn: $('resetBtn'),
    soundBtn: $('soundBtn'), soundLabel: $('soundLabel'),
    heroMoon: $('heroMoon'), heroDate: $('heroDate'), heroTag: $('heroTag'), moonLabel: $('moonLabel'), heroEvents: $('heroEvents'),
    dayLabel: $('dayLabel'), clockLabel: $('clockLabel'), lightBadge: $('lightBadge'), auroraBadge: $('auroraBadge'),
    heroGlyph: $('heroGlyph'), eventName: $('eventName'), eventFlavor: $('eventFlavor'),
    tempValue: $('tempValue'), tempUnit: $('tempUnit'), tempNote: $('tempNote'), tempTier: $('tempTier'),
    generateBtn: $('generateBtn'), genMain: $('genMain'), genSub: $('genSub'),
    chainNote: $('continuityNote'), chainHide: $('chainHide'),
    windValue: $('windValue'), windMeter: $('windMeter'), sevValue: $('sevValue'), sevMeter: $('sevMeter'),
    lightValue: $('lightValue'), nextValue: $('nextValue'),
    prevDay: $('prevDay'), nextDay: $('nextDay'), dayNavLabel: $('dayNavLabel'), playBtn: $('playBtn'), playIcon: $('playIcon'),
    timeline: $('timeline'), segments: $('segments'), playhead: $('playhead'), playheadTime: $('playheadTime'),
    ticks: $('ticks'), tempChart: $('tempChart'),
    cycMoon: $('cycMoon'), cycTwilight: $('cycTwilight'), cycAurora: $('cycAurora'),
    effects: $('effects'), effectsNote: $('effectsNote'), ledger: $('ledger'), ledgerNote: $('ledgerNote'),
    srStatus: $('srStatus'), toast: $('toast'),
    settings: $('settings'), setForm: $('setForm'), setClose: $('setClose'), setCancel: $('setCancel'), setApply: $('setApply'),
    setDay: $('setDay'), setMonth: $('setMonth'), setYear: $('setYear'), setDateErr: $('setDateErr'),
    seedInput: $('seedInput'), seedRandom: $('seedRandom'), setYnYear: $('setYnYear'), setYnName: $('setYnName'),
    setChain: $('setChain'), setMoonOff: $('setMoonOff'), setConfirm: $('setConfirm'),
    exportBtn: $('exportBtn'), importBtn: $('importBtn'), importInput: $('importInput'), backupMsg: $('backupMsg'),
  };

  // ---------- State ----------
  function read(key) {
    try { return JSON.parse(localStorage.getItem(key)); } catch (e) { return null; }
  }
  // v2 save, or a v1 save migrated (its days start on the default campaign date)
  function loadStored() {
    const v2 = read(STORE);
    if (v2 && typeof v2 === 'object') return v2;
    const v1 = read(STORE_V1);
    if (v1 && typeof v1 === 'object' && typeof v1.seed === 'string' && v1.seed) {
      const n = Math.max(0, Math.min(MAX_DAYS, v1.dayCount | 0));
      return { seed: v1.seed, started: v1.started, startAbs: DEFAULT_START, dayCount: n, current: n - 1, view: v1.view, time: v1.time, unit: v1.unit };
    }
    return {};
  }

  const isInt = Number.isInteger;
  const VALID_TYPE = { natural: 1, unnatural: 1, auril: 1, reminder: 1 };
  const VALID_REPEAT = {};
  H.REPEATS.forEach((r) => { VALID_REPEAT[r.key] = 1; });

  function cleanEvent(e) {
    if (!e || typeof e !== 'object' || !isInt(e.abs) || typeof e.title !== 'string' || !e.title.trim()) return null;
    return {
      id: typeof e.id === 'string' && e.id ? e.id : 'e' + Math.random().toString(36).slice(2, 10),
      abs: e.abs,
      title: e.title.trim().slice(0, 120),
      type: VALID_TYPE[e.type] ? e.type : 'natural',
      repeat: VALID_REPEAT[e.repeat] ? e.repeat : 'none',
      details: typeof e.details === 'string' ? e.details.slice(0, 2000) : '',
    };
  }
  function cleanMap(m) {
    const out = {};
    if (m && typeof m === 'object') {
      Object.keys(m).forEach((k) => {
        if (isInt(+k) && typeof m[k] === 'string' && m[k].trim()) out[+k] = m[k];
      });
    }
    return out;
  }

  // Any stored shape -> a complete, valid state.
  function clean(s) {
    s = s && typeof s === 'object' ? s : {};
    const c = {};
    c.seed = typeof s.seed === 'string' && s.seed ? s.seed : IWD.randomSeed();
    c.started = typeof s.started === 'number' ? s.started : 0;
    c.startAbs = isInt(s.startAbs) ? s.startAbs : DEFAULT_START;
    c.dayCount = Math.max(0, Math.min(MAX_DAYS, s.dayCount | 0));
    c.current = isInt(s.current) && s.current >= -1 && s.current < c.dayCount ? s.current : c.dayCount - 1;
    c.currentAbs = isInt(s.currentAbs) ? s.currentAbs : c.current >= 0 ? c.startAbs + c.current : null;
    if (c.current >= 0) c.currentAbs = c.startAbs + c.current;
    c.view = isInt(s.view) && s.view >= 0 && s.view < c.dayCount ? s.view : c.current;
    c.selAbs = isInt(s.selAbs) ? s.selAbs : c.startAbs + Math.max(0, c.view);
    c.time = typeof s.time === 'number' && s.time >= 0 && s.time < 24 ? s.time : SETTLE_HOUR;
    c.unit = s.unit === 'C' ? 'C' : 'F';
    c.moonOffset = typeof s.moonOffset === 'number' && isFinite(s.moonOffset) ? Math.max(-30, Math.min(30, s.moonOffset)) : 0;
    c.yearNames = cleanMap(s.yearNames);
    c.notes = cleanMap(s.notes);
    c.events = Array.isArray(s.events) ? s.events.map(cleanEvent).filter(Boolean) : [];
    const ui = s.ui && typeof s.ui === 'object' ? s.ui : {};
    const m = Cal.monthOf(c.selAbs);
    c.ui = {
      chainNoteHidden: !!ui.chainNoteHidden,
      calYear: isInt(ui.calYear) ? ui.calYear : m.year,
      calMonth: isInt(ui.calMonth) && ui.calMonth >= 0 && ui.calMonth < 12 ? ui.calMonth : m.month,
    };
    return c;
  }

  const S = { playing: false, busy: false };
  let chain, days;
  let epoch = 0;   // bumped whenever the day list is replaced, so cached DOM knows to rebuild

  function applyState(c) {
    const b = IWD.rebuild(c.seed, c.dayCount);
    chain = b.chain;
    days = b.days;
    delete c.dayCount;
    Object.assign(S, c);
    if (S.view < 0) S.time = 23;
    epoch++;
  }
  applyState(clean(loadStored()));

  function snapshot() {
    return {
      v: 2, seed: S.seed, started: S.started, startAbs: S.startAbs, dayCount: days.length,
      current: S.current, currentAbs: S.currentAbs, view: S.view, selAbs: S.selAbs, time: S.time, unit: S.unit,
      moonOffset: S.moonOffset, yearNames: S.yearNames, notes: S.notes, events: S.events, ui: S.ui,
    };
  }
  function save() {
    try { localStorage.setItem(STORE, JSON.stringify(snapshot())); } catch (e) { /* storage unavailable: the seed alone can rebuild the weather */ }
  }
  let saveTimer = 0;
  function saveSoon() { clearTimeout(saveTimer); saveTimer = setTimeout(save, 250); }

  const absOf = (i) => S.startAbs + i;
  const todayAbs = () => (S.current >= 0 ? absOf(S.current) : null);
  // the chain only ever grows forward, so this is the same weather a day-by-day roll gives
  function ensureDays(n) {
    n = Math.min(MAX_DAYS, n);
    while (days.length < n) days.push(IWD.nextDay(chain));
  }

  // Another tab changed the almanac. A different expedition is taken over whole; the same one
  // merges in its notes, events, settings and progress (local view and clock stay put).
  let pendingAdopt = false;
  function adoptStored(keepCurrent) {
    pendingAdopt = false;
    const raw = read(STORE);
    if (!raw || typeof raw.seed !== 'string' || !raw.seed) return false;
    const c = clean(raw);
    if (c.seed !== S.seed || c.started !== S.started || c.startAbs !== S.startAbs) {
      applyState(c);
      lastEventId = null;
      setPlaying(false);
      renderAll();
      cal.syncNotes();
      return true;
    }
    S.notes = c.notes;
    S.events = c.events;
    S.yearNames = c.yearNames;
    S.moonOffset = c.moonOffset;
    S.unit = c.unit;
    S.ui.chainNoteHidden = c.ui.chainNoteHidden;
    ensureDays(c.dayCount);
    if (!keepCurrent) {
      S.current = c.current;
      S.currentAbs = c.currentAbs;
    }
    renderAll();
    cal.syncNotes();
    return true;
  }

  // ---------- Helpers ----------
  const pad = (n) => String(n).padStart(2, '0');
  const fmtHour = (h) => pad(((Math.floor(h) % 24) + 24) % 24) + ':00';
  function fmtClock(t) {
    const tt = Math.min(t, 23.999);
    const h = Math.floor(tt), m = Math.floor((tt - h) * 60);
    return pad(h) + ':' + pad(m);
  }
  const toUnit = (f) => (S.unit === 'C' ? Math.round(((f - 32) * 5) / 9) : f);
  const minus = (n) => String(n).replace('-', '−');
  const fmtTemp = (f) => minus(toUnit(f)) + '°';
  const ease = (k) => (k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
  const easeSoft = (k) => 0.5 - 0.5 * Math.cos(Math.PI * k);
  const svgUse = (id, cls) => `<svg${cls ? ` class="${cls}"` : ''} viewBox="0 0 24 24" aria-hidden="true"><use href="#${id}"/></svg>`;
  function inkFor(hex) {
    const n = parseInt(hex.slice(1), 16);
    const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b < 140 ? 'dark-ink' : '';
  }
  function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
  // °C mode shows the table's own °C anchors
  // The table is defined in °F; in °C mode show the exact conversion so the anchor agrees with the shown temperature.
  const tierAnchor = (t) => (S.unit === 'C' ? `${minus(Math.round(((t.f - 32) * 5) / 9))}°C (${minus(t.f)}°F)` : minus(t.f) + '°F');
  const dcStep = () => (S.unit === 'C' ? '±1 per 10°F (≈5.6°C)' : '±1 per 10°F');

  // Event id at any hour relative to day `v`, walking into neighbouring days (or the
  // already-rolled carry-over) so visuals can blend across midnight.
  function eventAt(v, hour) {
    if (!days.length || v < 0) return 1;
    let d = v, h = Math.floor(hour);
    while (h >= 24 && d < days.length - 1) { h -= 24; d++; }
    while (h < 0 && d > 0) { h += 24; d--; }
    if (h < 0) h = 0;
    if (h >= 24) {
      const c = days[days.length - 1].carryOut;
      return h - 24 < c.duration - c.elapsed ? c.id : c.next;
    }
    return days[d].hours[h].id;
  }

  function segmentAt(day, h) {
    return day.segments.find((s) => h >= s.start && h < s.end);
  }

  function hourNow() { return Math.max(0, Math.min(23, Math.floor(S.time))); }

  // ---------- Scene ----------
  const scene = new Scene($('sky'), $('frost'));
  const audio = new window.IWDAudio();
  scene.onThunder = (s) => audio.thunder(s);

  function sceneTarget() {
    if (!days.length || S.view < 0) {
      return Object.assign({}, Scene.LOOKS[1], { aurora: 1, cold: 0.2 });
    }
    const at = (dt) => Scene.paramsFor((h) => eventAt(S.view, h), S.time + dt);
    const look = at(0);
    // cloud builds before precipitation and lingers after it; precipitation keeps inside the cloud
    const ahead = at(0.9), behind = at(-0.9);
    ['cloud', 'cloudDark', 'dark'].forEach((k) => { look[k] = Math.max(look[k], ahead[k], behind[k]); });
    ['stars', 'moon'].forEach((k) => { look[k] = Math.min(look[k], ahead[k], behind[k]); });
    const a2 = at(0.3), b2 = at(-0.3);
    ['snow', 'hail', 'whiteout', 'lightning', 'vortex'].forEach((k) => { look[k] = Math.min(look[k], a2[k], b2[k]); });
    const ev = EVENTS[eventAt(S.view, S.time)];
    look.aurora = ev.natural ? 1 : 0;
    look.cold = Math.max(0, Math.min(1, (-ev.tempF - 45) / 85));
    return look;
  }
  scene.setTarget(sceneTarget());
  scene.setTime(S.time);
  scene.snap();

  // ---------- Calendar ----------
  const cal = Cal.create({
    S, MAX_DAYS,
    days: () => days,
    fmtTemp,
    unitLabel: () => S.unit,
    cold: coldDC,
    select, setCurrent, saveSoon,
    saveEvent(ev) {
      const i = S.events.findIndex((x) => x.id === ev.id);
      if (i >= 0) S.events[i] = ev; else S.events.push(ev);
      renderMarks();
      save();
      toast(`<strong>${esc(ev.title)}</strong> ${i >= 0 ? 'updated' : 'added to the almanac'}`);
    },
    deleteEvent(id) {
      const ev = S.events.find((x) => x.id === id);
      S.events = S.events.filter((x) => x.id !== id);
      renderMarks();
      save();
      if (ev) toast(`<strong>${esc(ev.title)}</strong> removed`);
    },
    setNote(abs, text) {
      if (abs === null) return;
      if (text.trim()) S.notes[abs] = text; else delete S.notes[abs];
      cal.render();
      save();
    },
  });

  // everything that shows events or notes, without touching the weather
  function renderMarks() {
    cal.render();
    cal.renderDetail();
    renderHeroDay();
  }

  // ---------- Toast ----------
  let toastTimer = 0;
  function toast(html, ms) {
    el.toast.innerHTML = html;
    el.toast.hidden = false;
    el.toast.classList.remove('show');
    void el.toast.offsetWidth;
    el.toast.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      el.toast.classList.remove('show');
      toastTimer = setTimeout(() => { el.toast.hidden = true; }, 400);
    }, ms || 4200);
  }
  function dayToast(abs, lead) {
    const list = Cal.allOn(S.events, abs, S.moonOffset);
    if (!list.length) return false;
    toast(`<span class="toast-date">${esc(lead)} · ${esc(H.format(abs))}</span><span class="toast-chips">${list.map(Cal.chipHtml).join('')}</span>`, 7000);
    return true;
  }

  // ---------- Rendering: hour-level ----------
  let lastHourKey = '';
  let lastEventId = null;

  function setMeter(meter, n) {
    [...meter.children].forEach((i, k) => i.classList.toggle('on', k < n));
  }

  function replay(node) {
    node.classList.remove('swap-in');
    void node.offsetWidth;
    node.classList.add('swap-in');
  }

  function renderIntro() {
    el.dayLabel.textContent = 'Before the First Dawn';
    el.clockLabel.textContent = fmtClock(S.time);
    el.lightBadge.hidden = true;
    el.auroraBadge.hidden = false;
    el.heroGlyph.innerHTML = '<use href="#ev-1"/>';
    el.eventName.textContent = 'The Dale Awaits';
    el.eventFlavor.textContent = 'Begin the expedition to roll the first dawn. After that, every day grows out of the night before.';
    el.tempValue.textContent = '';
    document.querySelector('.hero-temp').hidden = true;
    el.tempUnit.textContent = '°' + S.unit;
    el.tempNote.textContent = 'air temperature';
    el.tempTier.hidden = true;
    el.windValue.textContent = '—';
    el.sevValue.textContent = '—';
    setMeter(el.windMeter, 0);
    setMeter(el.sevMeter, 0);
    el.lightValue.textContent = '—';
    el.nextValue.textContent = 'Awaiting the d12';
    el.effectsNote.textContent = 'no weather yet';
    el.effects.innerHTML = '<li class="empty-note">Effects of the current weather will appear here, hour by hour.</li>';
  }

  function renderHour(force) {
    if (!days.length || S.view < 0) {
      if (force || lastHourKey !== 'intro') { renderIntro(); lastHourKey = 'intro'; }
      return;
    }
    const day = days[S.view];
    const h = hourNow();
    const key = S.view + ':' + h + ':' + S.unit;
    if (!force && key === lastHourKey) return;
    lastHourKey = key;

    const hr = day.hours[h];
    const ev = EVENTS[hr.id];
    const seg = segmentAt(day, h);
    const light = hr.light;
    const changed = lastEventId !== ev.id;
    lastEventId = ev.id;

    document.querySelector('.hero-temp').hidden = false;
    if (changed || force) {
      el.heroGlyph.innerHTML = `<use href="#ev-${ev.id}"/>`;
      el.eventName.textContent = ev.name;
      el.eventFlavor.textContent = ev.flavor;
    }
    const cold = coldDC(hr.tempF);
    el.tempValue.textContent = minus(toUnit(hr.tempF));
    el.tempUnit.textContent = '°' + S.unit;
    el.tempNote.textContent = hr.tempF <= -100 ? 'killing cold' : hr.tempF <= -80 ? 'flesh-freezing' : 'air temperature';
    el.tempTier.hidden = false;
    el.tempTier.className = 'temp-tier tier-' + cold.tier.key;
    el.tempTier.innerHTML = `${esc(cold.tier.name)} <b>DC ${cold.dc}</b>`;
    el.tempTier.title = `Cold table: DC ${cold.tier.dc} at ${tierAnchor(cold.tier)}, ${dcStep()}`;
    if (changed) { replay(document.querySelector('.hero-main')); replay(el.tempValue); }

    el.lightBadge.hidden = false;
    el.lightBadge.className = 'badge ' + light.level;
    el.lightBadge.textContent = light.level === 'dim' ? 'Dim light' : light.level === 'dark' ? 'Between light cycles' : 'No natural light';
    el.auroraBadge.hidden = !light.auroraVisible;

    const wind = WIND[ev.wind];
    el.windValue.textContent = wind.label;
    setMeter(el.windMeter, wind.level + (wind.level ? 1 : 0));
    const sev = SEVERITY[ev.id];
    el.sevValue.textContent = SEV_LABEL[sev];
    setMeter(el.sevMeter, sev);
    el.sevMeter.classList.toggle('lethal', sev >= 5);
    el.lightValue.textContent = light.label + (light.level === 'none' ? ' — Blinded' : '');

    // forecast
    const nextEv = EVENTS[seg.next];
    const at = seg.changeAt;
    const left = at - h;
    const when = at >= 24 ? fmtHour(at) + ' tomorrow' : fmtHour(at);
    el.nextValue.innerHTML = `<strong>${esc(nextEv.name)}</strong> at ${when} <span class="muted">(in ${left} h · ${ev.dice}d4 rolled ${seg.duration})</span>`;

    // effects
    const fx = ev.effects.slice();
    const delta = cold.dc - cold.tier.dc;
    fx.unshift({ cat: 'chill', text: `Cold table: ${cold.tier.name} — DC ${cold.dc} at ${minus(toUnit(hr.tempF))}°${S.unit} (DC ${cold.tier.dc} at ${tierAnchor(cold.tier)}, ${dcStep()}` +
      (delta ? `; ${delta > 0 ? '+' : '−'}${Math.abs(delta)} here)` : ')') });
    if (light.level === 'dark') {
      fx.unshift({ cat: 'natural', text: 'Between light cycles: no moon, twilight or aurora this hour. The rules name no natural light source here; the [Sight] effects below still apply (DM may rule it darkness).' });
    }
    if (light.auroraVisible) {
      fx.unshift({ cat: 'aurora', text: "Auril's Aurora is visible. Characters who see it during a rest are magically healed: −1 exhaustion level and +1d4 HP per hour." });
    }
    el.effectsNote.textContent = fx.length + ' in play · ' + ev.name;
    el.effects.innerHTML = fx.map((f, i) => `
      <li class="effect cat-${f.cat}" style="--i:${i}">
        <span class="fx-icon">${svgUse('fx-' + f.cat)}</span>
        <span><span class="fx-cat">${esc(CAT_LABEL[f.cat] || f.cat)}</span><br><span class="fx-text">${esc(f.text)}</span></span>
      </li>`).join('');
  }

  // spoken summary for screen readers, only on deliberate steps (not every hour of playback)
  function announce() {
    if (!days.length || S.view < 0) return;
    const day = days[S.view], h = hourNow();
    const hr = day.hours[h], ev = EVENTS[hr.id];
    el.srStatus.textContent = `${H.format(absOf(S.view))}, day ${day.day}, ${fmtHour(h)}: ${ev.name}, ${minus(toUnit(hr.tempF))}°${S.unit}, ${coldDC(hr.tempF).tier.name} DC ${coldDC(hr.tempF).dc}, ${WIND[ev.wind].label}`;
  }

  // ---------- Rendering: day-level ----------
  function renderControls() {
    el.seedValue.textContent = S.seed;
    if (S.current < 0) {
      el.genMain.textContent = 'Begin the Expedition';
      el.genSub.textContent = 'd12 opens ' + H.format(S.startAbs);
    } else if (S.current + 1 >= MAX_DAYS) {
      el.genMain.textContent = 'The Almanac Is Full';
      el.genSub.textContent = MAX_DAYS + ' days recorded';
    } else {
      el.genMain.textContent = 'Advance to ' + H.format(absOf(S.current + 1), { short: true });
      const c = days[S.current].carryOut;
      const rem = c.duration - c.elapsed;
      el.genSub.textContent = c.elapsed > 0
        ? `${EVENTS[c.id].name} carries on · ${rem} h left`
        : `${EVENTS[c.id].name} rolls in at midnight`;
    }
    document.querySelectorAll('.tools .toggle-btn').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.unit === S.unit)));
    el.chainNote.hidden = !!S.ui.chainNoteHidden;
    // day arrows
    const v = S.view;
    el.prevDay.disabled = S.busy || v <= 0;
    el.nextDay.disabled = S.busy || v < 0 || v + 1 >= MAX_DAYS;
    el.dayNavLabel.textContent = v >= 0 ? `${H.format(absOf(v), { short: true })} · Day ${v + 1}` : 'No days yet';
  }

  function heroChips(abs) {
    const list = Cal.allOn(S.events, abs, S.moonOffset);
    el.heroEvents.hidden = !list.length;
    el.heroEvents.innerHTML = list.map(Cal.chipHtml).join('');
  }

  function renderHeroDay() {
    const abs = S.view >= 0 ? absOf(S.view) : S.startAbs;
    const m = H.moon(abs, S.moonOffset);
    el.heroMoon.innerHTML = Cal.moonSvg(m);
    el.heroDate.textContent = H.format(abs);
    el.moonLabel.textContent = m.name;
    heroChips(abs);
    let tag = '', cls = '';
    if (S.view < 0) { tag = 'Expedition start'; cls = 'start'; }
    else {
      el.dayLabel.textContent = `Day ${S.view + 1} of the expedition`;
      const t = todayAbs();
      if (abs === t) { tag = 'Today'; cls = 'today'; }
      else if (t !== null) { tag = Cal.relDays(abs - t); cls = abs > t ? 'ahead' : 'past'; }
    }
    el.heroTag.hidden = !tag;
    el.heroTag.textContent = tag;
    el.heroTag.className = 'hd-tag ' + cls;
  }

  function renderTimeline() {
    if (!days.length || S.view < 0) {
      el.segments.innerHTML = '';
      el.timeline.classList.add('empty');
      el.segments.innerHTML = '<span style="position:absolute;inset:0;display:grid;place-items:center">No days recorded yet — begin the expedition.</span>';
      [el.cycMoon, el.cycTwilight, el.cycAurora].forEach((t) => (t.innerHTML = ''));
      renderCycleWindows();
      el.tempChart.innerHTML = '';
      chartGeom = null;
      el.playhead.hidden = true;
      return;
    }
    el.timeline.classList.remove('empty');
    el.playhead.hidden = false;
    const day = days[S.view];
    const pxPerH = (el.timeline.clientWidth || 600) / 24;
    el.segments.innerHTML = day.segments.map((s, i) => {
      const ev = EVENTS[s.id];
      const left = (s.start / 24) * 100, width = ((s.end - s.start) / 24) * 100;
      const cls = ['seg', inkFor(COLORS[s.id]), s.fromPrevDay ? 'cont-l' : '', s.intoNextDay ? 'cont-r' : ''].join(' ');
      const tip = `${ev.name} · ${fmtHour(s.start)}–${s.end >= 24 ? '24:00' : fmtHour(s.end)} · ${ev.dice}d4 [${s.rolls.join('+')}] = ${s.duration} h` +
        (s.fromPrevDay ? ' · continues from the previous day' : '') + (s.intoNextDay ? ' · continues into the next day' : '');
      const label = (s.end - s.start) * pxPerH >= 64 ? `<span class="seg-name">${esc(ev.name)}</span>` : '';
      return `<div class="${cls}" style="left:${left}%;width:${width}%;--c:${COLORS[s.id]};--i:${i}" title="${esc(tip)}">${svgUse('ev-' + s.id)}${label}</div>`;
    }).join('');
    renderCycles(day);
    renderChart(day);
  }

  function spans(pred) {
    const out = [];
    let start = null;
    for (let h = 0; h <= 24; h++) {
      const on = h < 24 && pred(h);
      if (on && start === null) start = h;
      if (!on && start !== null) { out.push([start, h]); start = null; }
    }
    return out;
  }
  const spanHtml = (cls, list) => list.map(([a, b]) => `<i class="${cls}" style="left:${(a / 24) * 100}%;width:${((b - a) / 24) * 100}%"></i>`).join('');

  function renderCycleWindows() {
    el.cycMoon.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.moon)));
    el.cycTwilight.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.twilight)));
    el.cycAurora.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.aurora)));
  }

  function renderCycles(day) {
    const reach = (h) => EVENTS[day.hours[h].id].light === 'dim';
    el.cycMoon.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.moon))) +
      spanHtml('lit', spans((h) => inWindow(h, CYCLES.moon) && reach(h)));
    el.cycTwilight.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.twilight))) +
      spanHtml('lit', spans((h) => inWindow(h, CYCLES.twilight) && reach(h)));
    el.cycAurora.innerHTML = spanHtml('win', spans((h) => inWindow(h, CYCLES.aurora))) +
      spanHtml('lit', spans((h) => day.hours[h].light.auroraVisible));
  }

  function renderTicks() {
    let html = '';
    for (let h = 0; h <= 24; h += 3) html += `<span style="left:${(h / 24) * 100}%">${pad(h % 24 === 0 && h ? 24 : h)}</span>`;
    el.ticks.innerHTML = html;
  }

  // temperature trace: the hourly air temperature (event value + smooth drift), labelled per segment
  const T_MIN = -142, T_MAX = -38;
  let chartGeom = null;
  function tempAt(day, t) {
    const tt = Math.max(0, Math.min(23.999, t));
    return EVENTS[day.hours[Math.floor(tt)].id].tempF + IWD.tempOffset(S.seed, day.day - 1, tt);
  }
  function renderChart(day) {
    const W = Math.max(200, el.tempChart.clientWidth || 600), H = 70;
    const top = 16, bot = 6;
    const y = (f) => top + ((T_MAX - f) / (T_MAX - T_MIN)) * (H - top - bot);
    const x = (h) => (h / 24) * W;
    const pts = [];
    // follow the drift inside each hour, easing across event changes at the hour marks
    for (let h = 0; h < 24; h++) {
      pts.push([x(h + 0.2), y(tempAt(day, h + 0.2))], [x(h + 0.8), y(tempAt(day, h + 0.8))]);
    }
    pts.unshift([0, y(tempAt(day, 0))]);
    pts.push([W, y(tempAt(day, 23.999))]);
    let d = `M${pts[0][0].toFixed(1)},${pts[0][1].toFixed(1)}`;
    for (let i = 1; i < pts.length; i++) {
      const [x0, y0] = pts[i - 1], [x1, y1] = pts[i];
      const mx = (x0 + x1) / 2;
      d += ` C${mx.toFixed(1)},${y0.toFixed(1)} ${mx.toFixed(1)},${y1.toFixed(1)} ${x1.toFixed(1)},${y1.toFixed(1)}`;
    }
    const area = d + ` L${W},${H} L0,${H} Z`;
    const gridF = S.unit === 'C' ? [-45, -65, -85].map((c) => (c * 9) / 5 + 32) : [-50, -90, -130];
    const grid = gridF.map((f) =>
      `<line class="t-grid" x1="0" x2="${W}" y1="${y(f).toFixed(1)}" y2="${y(f).toFixed(1)}"/>` +
      `<text class="t-label" text-anchor="end" x="-8" y="${(y(f) + 3).toFixed(1)}">${fmtTemp(f)}</text>`).join('');
    let lastX = -Infinity;
    const labels = day.segments.filter((s) => s.end - s.start >= 2).map((s) => {
      const mid = (s.start + s.end) / 2;
      const lx = x(mid);
      if (lx - lastX < 30) return '';
      lastX = lx;
      const f = day.hours[Math.min(23, Math.floor(mid))].tempF;
      const ly = Math.min(...[s.start + 0.2, mid, s.end - 0.2].map((t) => y(tempAt(day, t))));
      return `<text class="t-label t-label-strong" text-anchor="middle" x="${lx.toFixed(1)}" y="${(ly - 6).toFixed(1)}">${fmtTemp(f)}</text>`;
    }).join('');
    el.tempChart.setAttribute('viewBox', `0 0 ${W} ${H}`);
    el.tempChart.innerHTML = `
      <defs><linearGradient id="tGrad" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#bfe6fb" stop-opacity=".55"/><stop offset="1" stop-color="#bfe6fb" stop-opacity="0"/>
      </linearGradient></defs>
      ${grid}<path class="t-area" d="${area}"/><path class="t-line" d="${d}"/>${labels}
      <circle id="tDot" r="4.5" fill="#fff" style="filter:drop-shadow(0 0 6px #bfe6fb)"/>`;
    chartGeom = { day, x, y };
    placeChartDot();
  }

  function placeChartDot() {
    const dot = document.getElementById('tDot');
    if (!dot || !chartGeom) return;
    const t = Math.min(S.time, 23.999);
    dot.setAttribute('cx', chartGeom.x(t).toFixed(1));
    dot.setAttribute('cy', chartGeom.y(tempAt(chartGeom.day, t)).toFixed(1));
  }

  // The ledger shows the viewed day's rolls only.
  const dice = (rolls) => rolls.map((r) => `<span class="die">${r}</span>`).join('');
  function ledgerDayHtml(i) {
    const day = days[i];
    const out = [`<li class="day-break">Day ${day.day} · ${esc(H.format(absOf(i)))}</li>`];
    if (day.segments[0].fromPrevDay) {
      const c = day.carriedIn;
      out.push(`<li class="carry"><span class="l-time">00:00</span><span class="l-body">
        <span class="l-event">${esc(EVENTS[c.id].name)} continues</span>
        <span class="l-rolls">carried over from Day ${day.day - 1} · ${c.duration - c.elapsed} of ${c.duration} h remain <span class="arrow">→</span> ${esc(EVENTS[c.next].name)}</span>
      </span></li>`);
    }
    day.log.forEach((e) => {
      const ev = EVENTS[e.id];
      const opener = e.cause.type === 'genesis'
        ? `<span class="die d12">${e.cause.d12}</span> d12 opens with`
        : `after ${esc(EVENTS[e.cause.from].name)}:`;
      const nx = ev.next;
      const nextLabel = nx[0] === nx[1] && nx[1] === nx[2] ? 'always' : OPTION_NAME[e.nextIdx] + ' option:';
      out.push(`<li><span class="l-time">${fmtHour(e.hour)}</span><span class="l-body">
        <span class="l-event">${opener} ${esc(ev.name)}</span>
        <span class="l-rolls">${ev.dice}d4 ${dice(e.rolls)} = ${e.duration} h <span class="arrow">→</span> ${nextLabel} ${esc(EVENTS[e.next].name)}</span>
      </span></li>`);
    });
    return out.join('');
  }

  let ledgerKey = '';
  function renderLedger() {
    const key = epoch + ':' + S.view;
    if (key === ledgerKey) return;
    ledgerKey = key;
    el.ledger.innerHTML = S.view >= 0 && days[S.view] ? ledgerDayHtml(S.view) : '<li class="day-break">No rolls yet</li>';
    el.ledger.scrollTop = 0;
  }

  function renderDay() {
    renderControls();
    renderTimeline();
    renderLedger();
    renderHeroDay();
    renderHour(true);
  }
  function renderAll() {
    renderDay();
    cal.render();
    cal.renderDetail();
  }

  // ---------- Per-frame updates ----------
  let tween = null;
  function tweenTime(to, secs, curve) {
    return new Promise((resolve) => { tween = { from: S.time, to, dur: Math.max(0.01, secs), t: 0, curve: curve || ease, resolve }; });
  }

  let audioClock = 0;
  let brightSky = false;
  let last = performance.now();
  function frame(now) {
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;

    if (tween) {
      tween.t += dt;
      const k = Math.min(1, tween.t / tween.dur);
      S.time = tween.from + (tween.to - tween.from) * tween.curve(k);
      if (k >= 1) { const r = tween.resolve; tween = null; r(); }
    } else if (S.playing && days.length && S.view >= 0) {
      S.time += dt / SECONDS_PER_HOUR;
      if (S.time >= 24) {
        // play on through recorded days, but stop at the end of today: tomorrow needs Advance Day
        const limit = S.view <= S.current ? S.current : days.length - 1;
        if (S.view < limit) {
          S.time -= 24;
          S.view++;
          S.selAbs = absOf(S.view);
          cal.flushNote();
          cal.showMonthOf(S.selAbs);
          renderAll();
        } else {
          S.time = 23.99;
          setPlaying(false);
          if (S.view === S.current) el.generateBtn.classList.add('beckon');
        }
      }
    }

    // fast time needs a fast sky, or short events wash out in the low-pass
    scene.tau = S.playing || tween ? 0.32 : 1.6;
    scene.setTarget(sceneTarget());
    scene.setTime(S.time);

    // white-out and fog skies are bright: switch the UI to its high-contrast inks
    const glare = (scene.params.whiteout || 0) + (scene.params.fog || 0);
    const bright = glare > (brightSky ? 0.38 : 0.45);
    if (bright !== brightSky) { brightSky = bright; document.body.classList.toggle('bright-sky', bright); }

    audioClock += dt;
    if (audioClock > 0.2) { audio.update(scene.params.wind, audioClock); audioClock = 0; }

    el.clockLabel.textContent = fmtClock(S.time);
    if (days.length && S.view >= 0) {
      const pct = (Math.min(S.time, 24) / 24) * 100;
      el.playhead.style.left = pct + '%';
      el.playheadTime.textContent = fmtClock(S.time);
      el.playheadTime.style.transform = pct > 88 ? 'translateX(calc(-100% - 16px))' : '';
      el.timeline.setAttribute('aria-valuenow', String(hourNow()));
      el.timeline.setAttribute('aria-valuetext', fmtHour(hourNow()) + ', ' + EVENTS[days[S.view].hours[hourNow()].id].name);
      placeChartDot();
    }
    renderHour(false);
    requestAnimationFrame(frame);
  }

  // ---------- Actions ----------
  function setPlaying(on) {
    S.playing = on && days.length > 0 && S.view >= 0;
    el.playIcon.setAttribute('href', S.playing ? '#i-pause' : '#i-play');
    el.playBtn.setAttribute('aria-label', S.playing ? 'Pause' : 'Play the day');
    el.playBtn.title = S.playing ? 'Pause' : 'Play the day';
    if (!S.playing) save();
  }

  function frostBurst(btn) {
    const r = btn.getBoundingClientRect();
    const b = document.createElement('span');
    b.className = 'frost-burst';
    b.style.left = r.left + r.width * 0.15 + 'px';
    b.style.top = r.top + r.height / 2 + 'px';
    document.body.appendChild(b);
    setTimeout(() => b.remove(), 1000);
  }

  // Advance Day: the party's current day moves on by one (Begin the Expedition the first time).
  async function advance() {
    const first = S.current < 0;
    const target = first ? 0 : S.current + 1;
    if (S.busy || target >= MAX_DAYS) return;
    S.busy = true;
    setPlaying(false);
    cal.flushNote();
    // aria-disabled rather than disabled, so keyboard focus stays on the button
    el.generateBtn.setAttribute('aria-disabled', 'true');
    el.generateBtn.classList.remove('beckon');
    el.generateBtn.classList.add('rolling');
    frostBurst(el.generateBtn);

    if (!first && S.view !== S.current) {
      // back to today, so the night that's ending is the one on screen
      S.view = S.current;
      S.selAbs = absOf(S.current);
      cal.showMonthOf(S.selAbs);
      renderAll();
    }
    ensureDays(target + 1);
    renderControls();
    el.genMain.textContent = first ? 'Rolling the First Dawn…' : 'Night Runs Out…';
    el.genSub.textContent = 'continuing the same chain';

    const land = () => {
      S.current = target;
      S.currentAbs = absOf(target);
      S.view = target;
      S.selAbs = S.currentAbs;
      S.time = 0;
      cal.showMonthOf(S.selAbs);
      renderAll();
      el.genMain.textContent = first ? 'Rolling the First Dawn…' : 'Dawn Breaks…';
      el.genSub.textContent = 'continuing the same chain';
      dayToast(S.currentAbs, first ? 'The expedition begins' : 'Today');
    };
    if (first) {
      land();
      await tweenTime(SETTLE_HOUR, 2.6);
    } else {
      // let the old night run out (visuals already know what tomorrow brings), then roll into the new day
      await tweenTime(24, Math.max(1.2, Math.min(4, (24 - S.time) * 0.25)), easeSoft);
      land();
      await tweenTime(SETTLE_HOUR, 2.4);
    }

    el.generateBtn.classList.remove('rolling');
    el.generateBtn.removeAttribute('aria-disabled');
    S.busy = false;
    // a write from another tab during the transition was held back: merge it, keeping our new day
    if (pendingAdopt) adoptStored(true);
    renderControls();
    save();
    announce();
  }

  // Single click on a day: view it. Days on/after the start are rolled up to it first.
  function select(abs, opts) {
    opts = opts || {};
    const i = abs - S.startAbs;
    // days past the cap can still hold notes and events, they just get no weather
    if (i >= MAX_DAYS) toast(`Beyond the almanac: at most ${MAX_DAYS} days of weather can be recorded.`);
    cal.flushNote();
    const prevView = S.view;
    S.selAbs = abs;
    if (i >= 0 && i < MAX_DAYS && !S.busy) {
      ensureDays(i + 1);
      if (i !== S.view) {
        if (S.view < 0) S.time = SETTLE_HOUR;
        S.view = i;
      }
    }
    if (!opts.fromCalendar) cal.showMonthOf(abs);
    else {
      const m = Cal.monthOf(abs);
      if (m.year !== S.ui.calYear || m.month !== S.ui.calMonth) cal.showMonthOf(abs);
    }
    renderAll();
    save();
    if (S.view !== prevView) announce();
  }

  // Double click / "Set as current day": the party's today jumps there.
  function setCurrent(abs) {
    if (S.busy) return;
    const i = abs - S.startAbs;
    if (i < 0) {
      toast(`The expedition begins on <strong>${esc(H.format(S.startAbs))}</strong>. Change the start date in Settings to begin earlier.`);
      return;
    }
    if (i >= MAX_DAYS) { toast(`Beyond the almanac: at most ${MAX_DAYS} days can be recorded.`); return; }
    if (i === S.current) {
      if (S.selAbs !== abs) select(abs);
      return;
    }
    cal.flushNote();
    ensureDays(i + 1);
    if (S.view < 0) S.time = SETTLE_HOUR;
    S.current = i;
    S.currentAbs = abs;
    S.view = i;
    S.selAbs = abs;
    el.generateBtn.classList.remove('beckon');
    cal.showMonthOf(abs);
    renderAll();
    save();
    if (!dayToast(abs, 'Today is now')) toast(`Today is now <strong>${esc(H.format(abs))}</strong>`);
    announce();
  }

  function stepDay(dir) {
    if (S.busy || S.view < 0) return;
    const i = S.view + dir;
    if (i < 0 || i >= MAX_DAYS) return;
    select(absOf(i));
  }

  function setTimeFromPointer(evt) {
    const r = el.timeline.getBoundingClientRect();
    const k = Math.max(0, Math.min(0.9999, (evt.clientX - r.left) / r.width));
    S.time = k * 24;
  }

  // Start date or seed changed: re-roll from the new start up to the party's calendar date.
  function regenerate(startAbs, seed) {
    const target = S.currentAbs;
    setPlaying(false);
    S.startAbs = startAbs;
    S.seed = seed;
    S.started = Date.now();
    chain = IWD.newChain(seed);
    days = [];
    epoch++;
    lastEventId = null;
    const i = target === null ? -1 : target - startAbs;
    if (i >= 0) {
      ensureDays(i + 1);
      S.current = days.length - 1;
      S.currentAbs = absOf(S.current);
      S.view = S.current;
      S.selAbs = S.currentAbs;
    } else {
      S.current = -1;
      S.view = -1;
      S.time = 23;
      if (S.selAbs < startAbs) S.selAbs = startAbs;
    }
    cal.showMonthOf(S.view >= 0 ? S.selAbs : startAbs);
  }

  // ---------- Settings ----------
  const startOptions = [];
  H.MONTHS.forEach((m, i) => {
    startOptions.push({ value: 'm' + i, label: `${m.name} — ${m.common}` });
    H.FESTIVALS.filter((f) => f.after === i).forEach((f) => {
      startOptions.push({ value: 'f' + f.key, label: `${f.name} (festival${f.leapOnly ? ', leap years' : ''})` });
    });
  });
  el.setMonth.innerHTML = startOptions.map((o) => `<option value="${o.value}">${esc(o.label)}</option>`).join('');

  let confirmArmed = false;
  function readStart() {
    const y = parseInt(el.setYear.value, 10);
    const sel = el.setMonth.value;
    const fest = sel[0] === 'f' ? sel.slice(1) : null;
    const d = parseInt(el.setDay.value, 10);
    if (!isFinite(y)) return { err: 'Enter a year.' };
    if (!fest && !(d >= 1 && d <= 30)) return { err: 'Months have 30 days: pick a day from 1 to 30.' };
    const abs = H.toAbs(fest ? { year: y, festival: fest } : { year: y, month: +sel.slice(1), day: d });
    if (abs === null) return { err: fest === 'shieldmeet' ? `Shieldmeet only falls in leap years; ${y} DR is not one.` : 'That date does not exist.' };
    return { abs };
  }
  function syncDayField() {
    const fest = el.setMonth.value[0] === 'f';
    el.setDay.disabled = fest;
    el.setDay.closest('.field').classList.toggle('off', fest);
  }
  function disarm() {
    confirmArmed = false;
    el.setConfirm.hidden = true;
    el.setApply.textContent = 'Apply';
    el.setApply.classList.remove('warn');
  }
  function fillYearName() { el.setYnName.value = S.yearNames[parseInt(el.setYnYear.value, 10)] || ''; }

  let settingsReturn = null;
  function openSettings(focus) {
    cal.flushNote();
    settingsReturn = document.activeElement;
    const d = H.fromAbs(S.startAbs);
    el.setYear.value = d.year;
    el.setMonth.value = d.festival ? 'f' + d.festival : 'm' + d.month;
    el.setDay.value = d.day || 1;
    syncDayField();
    el.seedInput.value = S.seed;
    el.setYnYear.value = S.ui.calYear;
    fillYearName();
    el.setForm.querySelectorAll('input[name="setUnit"]').forEach((r) => { r.checked = r.value === S.unit; });
    el.setChain.checked = !S.ui.chainNoteHidden;
    el.setMoonOff.value = S.moonOffset;
    el.setDateErr.hidden = true;
    el.backupMsg.textContent = 'Everything in one JSON file: seed, dates, notes, events and settings.';
    disarm();
    if (!el.settings.open) el.settings.showModal();
    const f = focus === 'seed' ? el.seedInput : el.setDay.disabled ? el.setMonth : el.setDay;
    f.focus();
    if (focus === 'seed') el.seedInput.select();
  }
  function closeSettings() {
    if (el.settings.open) el.settings.close();
  }
  el.settings.addEventListener('close', () => {
    disarm();
    if (settingsReturn && document.contains(settingsReturn)) settingsReturn.focus();
    settingsReturn = null;
  });

  function applySettings() {
    if (S.busy) return;
    const st = readStart();
    if (st.err) {
      el.setDateErr.textContent = st.err;
      el.setDateErr.hidden = false;
      return;
    }
    el.setDateErr.hidden = true;
    const seed = el.seedInput.value.trim().toUpperCase() || IWD.randomSeed();
    const reroll = st.abs !== S.startAbs || seed !== S.seed;
    if (reroll && days.length && !confirmArmed) {
      const t = S.currentAbs;
      const upTo = t !== null && t >= st.abs
        ? `, up to the party's current day, <strong>${esc(H.format(t))}</strong>.`
        : `. The party's current day falls before the new start, so the expedition will begin on <strong>${esc(H.format(st.abs))}</strong>.`;
      el.setConfirm.innerHTML = `<strong>Regenerate the weather?</strong> All recorded weather is replaced and rolled again from <strong>${esc(H.format(st.abs))}</strong> with seed <code>${esc(seed)}</code>` +
        upTo + ' Notes and events are kept.';
      el.setConfirm.hidden = false;
      el.setApply.textContent = 'Regenerate weather';
      el.setApply.classList.add('warn');
      confirmArmed = true;
      el.setApply.focus();
      return;
    }
    const unitPick = el.setForm.querySelector('input[name="setUnit"]:checked');
    if (unitPick) S.unit = unitPick.value;
    S.ui.chainNoteHidden = !el.setChain.checked;
    const off = parseFloat(el.setMoonOff.value);
    S.moonOffset = isFinite(off) ? Math.max(-30, Math.min(30, off)) : 0;
    const yy = parseInt(el.setYnYear.value, 10);
    if (isFinite(yy)) {
      const name = el.setYnName.value.trim();
      if (name) S.yearNames[yy] = name.slice(0, 80); else delete S.yearNames[yy];
    }
    if (reroll) regenerate(st.abs, seed);
    closeSettings();
    renderAll();
    save();
    if (reroll) toast(`Weather regenerated from <strong>${esc(H.format(S.startAbs))}</strong> · seed ${esc(S.seed)}`);
  }

  function exportBackup() {
    const data = JSON.stringify(Object.assign({ app: 'icewind-dale-almanac', exported: new Date().toISOString() }, snapshot()), null, 2);
    const blob = new Blob([data], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    const d = new Date();
    a.href = url;
    a.download = `icewind-almanac-${S.seed}-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}.json`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 5000);
    el.backupMsg.textContent = 'Backup saved as ' + a.download;
    return data;
  }

  function importBackup(file) {
    const r = new FileReader();
    r.onload = () => {
      let obj = null;
      try { obj = JSON.parse(r.result); } catch (e) { obj = null; }
      if (!obj || typeof obj !== 'object' || typeof obj.seed !== 'string' || !obj.seed) {
        el.backupMsg.textContent = 'That file is not an almanac backup.';
        return;
      }
      if (S.busy) return;
      setPlaying(false);
      applyState(clean(obj));
      S.started = typeof obj.started === 'number' && obj.started ? obj.started : Date.now();
      lastEventId = null;
      closeSettings();
      renderAll();
      cal.syncNotes();
      save();
      toast(`Backup restored · ${days.length} day${days.length === 1 ? '' : 's'}, ${S.events.length} event${S.events.length === 1 ? '' : 's'}`);
    };
    r.onerror = () => { el.backupMsg.textContent = 'Could not read that file.'; };
    r.readAsText(file);
  }

  // ---------- Wiring ----------
  el.generateBtn.addEventListener('click', advance);
  el.playBtn.addEventListener('click', () => {
    if (S.busy || !days.length || S.view < 0) return;
    if (!S.playing && S.time >= 23.9) S.time = 0;
    setPlaying(!S.playing);
  });
  el.prevDay.addEventListener('click', () => stepDay(-1));
  el.nextDay.addEventListener('click', () => stepDay(1));

  let dragging = false;
  el.timeline.addEventListener('pointerdown', (e) => {
    if (S.busy || !days.length || S.view < 0) return;
    dragging = true;
    el.timeline.setPointerCapture(e.pointerId);
    setPlaying(false);
    setTimeFromPointer(e);
  });
  el.timeline.addEventListener('pointermove', (e) => { if (dragging) setTimeFromPointer(e); });
  const endDrag = () => { if (dragging) { dragging = false; save(); } };
  el.timeline.addEventListener('pointerup', endDrag);
  el.timeline.addEventListener('pointercancel', endDrag);
  el.timeline.addEventListener('keydown', (e) => {
    if (S.busy || !days.length || S.view < 0) return;
    const h = hourNow();
    let n = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowUp') n = Math.min(23, h + 1);
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') n = Math.max(0, h - 1);
    else if (e.key === 'Home') n = 0;
    else if (e.key === 'End') n = 23;
    else if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); el.playBtn.click(); return; }
    if (n !== null) { e.preventDefault(); setPlaying(false); S.time = n + 0.5; save(); announce(); }
  });

  function setUnit(u) {
    S.unit = u;
    renderAll();
    save();
  }
  document.querySelectorAll('.tools .toggle-btn').forEach((b) => b.addEventListener('click', () => setUnit(b.dataset.unit)));

  el.soundBtn.addEventListener('click', () => {
    const on = !audio.enabled;
    if (!audio.setEnabled(on)) { el.soundLabel.textContent = 'No audio'; return; }
    el.soundBtn.setAttribute('aria-pressed', String(on));
    el.soundLabel.textContent = on ? 'Sound on' : 'Sound off';
  });

  el.chainHide.addEventListener('click', () => {
    S.ui.chainNoteHidden = true;
    renderControls();
    save();
    el.generateBtn.focus();
  });

  el.settingsBtn.addEventListener('click', () => openSettings());
  el.seedChip.addEventListener('click', () => openSettings('seed'));
  el.resetBtn.addEventListener('click', () => openSettings());
  el.setClose.addEventListener('click', closeSettings);
  el.setCancel.addEventListener('click', closeSettings);
  el.setForm.addEventListener('submit', (e) => { e.preventDefault(); applySettings(); });
  el.setMonth.addEventListener('change', syncDayField);
  [el.setYear, el.setMonth, el.setDay, el.seedInput].forEach((n) => n.addEventListener('input', disarm));
  el.setYnYear.addEventListener('input', fillYearName);
  el.seedRandom.addEventListener('click', () => { el.seedInput.value = IWD.randomSeed(); disarm(); el.seedInput.focus(); });
  el.exportBtn.addEventListener('click', exportBackup);
  el.importBtn.addEventListener('click', () => el.importInput.click());
  el.importInput.addEventListener('change', () => {
    const f = el.importInput.files && el.importInput.files[0];
    if (f) importBackup(f);
    el.importInput.value = '';
  });
  // clicking the backdrop closes the dialog
  el.settings.addEventListener('pointerdown', (e) => { if (e.target === el.settings) closeSettings(); });

  let resizeTimer = 0;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(renderTimeline, 120);
  });
  window.addEventListener('storage', (e) => {
    if (e.key !== STORE) return;
    if (S.busy) pendingAdopt = true;
    else adoptStored(false);
  });
  window.addEventListener('beforeunload', () => { cal.flushNote(); save(); });

  // test / console hooks
  window.IWDApp = { state: S, days: () => days, select, setCurrent, advance, exportBackup, save };

  // ---------- Boot ----------
  renderTicks();
  renderAll();
  save();
  requestAnimationFrame(frame);
})();
