/* Almanac calendar: Harptos month grid with Selûne phases, colour-coded events and reminders,
 * the day detail panel (events, notes) and the upcoming list.
 * State lives in app.js and is reached through the ctx passed to create().
 */
(function () {
  'use strict';

  const H = window.Harptos;

  const TYPES = [
    { key: 'natural',   label: 'Natural' },
    { key: 'unnatural', label: 'Unnatural' },
    { key: 'auril',     label: 'Auril' },
    { key: 'reminder',  label: 'Reminder' },
  ];
  const TYPE = {};
  TYPES.forEach((t) => { TYPE[t.key] = t; });
  const PRIORITY = { auril: 0, unnatural: 1, natural: 2, reminder: 3 };
  const REPEAT = {};
  H.REPEATS.forEach((r) => { REPEAT[r.key] = r; });

  // weather event colours, shared with the timeline
  const COLORS = {
    1: '#a6ecff', 2: '#8ccbeb', 3: '#6d819b', 4: '#46526b', 5: '#dcf3ff', 6: '#a9c5e2',
    7: '#7aa7d8', 8: '#f5fbff', 9: '#b9c7d1', 10: '#b49cff', 11: '#7fd1d9', 12: '#5ee0c8',
  };

  const NEW_MOON = { builtin: true, type: 'auril', title: 'New moon — sacrifice to Auril', short: 'Sacrifice to Auril' };

  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const svgUse = (id, cls) => `<svg${cls ? ` class="${cls}"` : ''} viewBox="0 0 24 24" aria-hidden="true"><use href="#${id}"/></svg>`;
  const pad = (n) => String(n).padStart(2, '0');
  const ORD = ['First', 'Second', 'Third'];

  function dominant(day) {
    const tally = {};
    day.segments.forEach((s) => { tally[s.id] = (tally[s.id] || 0) + (s.end - s.start); });
    return +Object.keys(tally).sort((a, b) => tally[b] - tally[a])[0];
  }

  // Selûne: the lit limb sits on the right while waxing; the terminator is an ellipse whose
  // half-width shrinks to zero at the quarters.
  function moonSvg(m, cls) {
    const k = m.illum;
    let lit = '';
    if (k > 0.985) lit = '<circle cx="10" cy="10" r="8"/>';
    else if (k > 0.015) {
      const rx = (8 * Math.abs(1 - 2 * k)).toFixed(2);
      const outer = m.waxing ? 1 : 0;
      const inner = (k < 0.5) === m.waxing ? 0 : 1;
      lit = `<path d="M10 2A8 8 0 0 ${outer} 10 18A${rx} 8 0 0 ${inner} 10 2z"/>`;
    }
    const state = m.isNew ? ' is-new' : m.isFull ? ' is-full' : '';
    return `<svg class="moon${state}${cls ? ' ' + cls : ''}" viewBox="0 0 20 20" aria-hidden="true">` +
      `<circle class="moon-dark" cx="10" cy="10" r="8"/><g class="moon-lit">${lit}</g><circle class="moon-rim" cx="10" cy="10" r="8"/></svg>`;
  }

  function eventsOn(list, abs, off) {
    return list.filter((e) => H.occursOn(e, abs, off))
      .sort((a, b) => (PRIORITY[a.type] - PRIORITY[b.type]) || (a.abs - b.abs));
  }

  // everything marked on a day: the built-in new-moon sacrifice first, then the user's events
  function allOn(list, abs, off) {
    const m = H.moon(abs, off);
    const evs = eventsOn(list, abs, off);
    return m.isNew ? [NEW_MOON].concat(evs) : evs;
  }

  function chipHtml(e) {
    const mark = e.builtin ? svgUse('i-flake') : '<i aria-hidden="true"></i>';
    return `<span class="ev-chip t-${e.type}${e.builtin ? ' builtin' : ''}" title="${esc(e.title)}">${mark}<span>${esc(e.short || e.title)}</span></span>`;
  }

  // the month a date is shown in: festivals live in the month they follow
  function monthOf(abs) {
    const d = H.fromAbs(abs);
    if (d.festival) return { year: d.year, month: H.FESTIVALS.find((f) => f.key === d.festival).after };
    return { year: d.year, month: d.month };
  }

  function relDays(n) {
    if (n === 0) return 'Today';
    if (n === 1) return 'Tomorrow';
    if (n === -1) return 'Yesterday';
    return n > 0 ? `In ${n} days` : `${-n} days ago`;
  }

  function create(ctx) {
    const $ = (id) => document.getElementById(id);
    const el = {
      prev: $('calPrev'), next: $('calNext'), today: $('calToday'),
      month: $('calMonth'), common: $('calCommon'), year: $('calYear'), yearName: $('calYearName'),
      grid: $('calGrid'), fests: $('calFests'), upList: $('upList'), upNote: $('upNote'),
      dEyebrow: $('dEyebrow'), dDate: $('dDate'), dSub: $('dSub'), dMoon: $('dMoon'), dWeather: $('dWeather'),
      dSetCurrent: $('dSetCurrent'), dEvents: $('dEvents'), dAdd: $('dAdd'),
      form: $('evForm'), formTitle: $('evFormTitle'), title: $('evTitle'), repeat: $('evRepeat'),
      details: $('evDetails'), save: $('evSave'), cancel: $('evCancel'),
      notes: $('dNotes'), noteState: $('dNoteState'),
    };
    const S = ctx.S;
    const days = () => ctx.days();
    const off = () => S.moonOffset;
    const todayAbs = () => (S.current >= 0 ? S.startAbs + S.current : null);

    el.repeat.innerHTML = H.REPEATS.map((r) => `<option value="${r.key}">${esc(r.label)}</option>`).join('');

    function weatherOf(abs) {
      const i = abs - S.startAbs, list = days();
      return i >= 0 && i < list.length ? list[i] : null;
    }

    // ---------- Month grid ----------
    function cellInner(abs, festName) {
      const m = H.moon(abs, off());
      const day = weatherOf(abs);
      const evs = eventsOn(S.events, abs, off());
      const note = S.notes[abs];
      const d = H.fromAbs(abs);
      let wx = '';
      if (day) {
        const id = dominant(day);
        wx = `<span class="cd-wx" style="--c:${COLORS[id]}">${svgUse('ev-' + id)}<span class="cd-temp">${ctx.fmtTemp(day.tempMin)}</span></span>`;
      }
      const marks = (m.isNew ? `<span class="cd-auril" title="New moon — sacrifice to Auril">${svgUse('i-flake')}</span>` : '') +
        (note ? `<span class="cd-note" title="Has notes">${svgUse('i-note')}</span>` : '');
      const chips = evs.slice(0, 2).map(chipHtml).join('') + (evs.length > 2 ? `<span class="cd-more">+${evs.length - 2}</span>` : '');
      const head = festName
        ? `<span class="cd-head"><span class="cd-fest-name">${esc(festName)}</span><span class="cd-marks">${marks}</span>${moonSvg(m)}</span>`
        : `<span class="cd-head"><span class="cd-num">${d.day}</span><span class="cd-marks">${marks}</span>${moonSvg(m)}</span>`;
      return head + wx + `<span class="cd-chips">${chips}</span>`;
    }

    function cellAttrs(abs) {
      const i = abs - S.startAbs;
      const m = H.moon(abs, off());
      const day = weatherOf(abs);
      const evs = allOn(S.events, abs, off());
      const isToday = abs === todayAbs();
      const top = evs.find((e) => !e.builtin);
      const cls = [
        isToday ? 'is-today' : '', abs === S.selAbs ? 'is-sel' : '', i < 0 ? 'is-pre' : '',
        day ? 'has-wx' : '', m.isNew ? 'is-new' : '', i === 0 ? 'is-start' : '', top ? 'has-ev' : '',
      ].filter(Boolean).join(' ');
      const style = top ? ` style="--ec:var(--t-${top.type})"` : '';
      const label = [H.format(abs), m.name, m.isNew ? 'sacrifice to Auril' : '',
        day ? `${window.IWD.EVENTS[dominant(day)].name}, low ${ctx.fmtTemp(day.tempMin)}` : '',
        evs.filter((e) => !e.builtin).map((e) => e.title).join(', '),
        S.notes[abs] ? 'has notes' : '', isToday ? 'current day' : '', i === 0 ? 'expedition start' : '']
        .filter(Boolean).join(' · ');
      const tags = (isToday ? '<span class="cd-tag today">Today</span>' : i === 0 ? '<span class="cd-tag start">Start</span>' : '');
      return { cls, label, tags, isToday, style };
    }

    function dayHtml(abs) {
      const a = cellAttrs(abs);
      return `<button type="button" class="cal-day ${a.cls}" data-abs="${abs}"${a.style} aria-label="${esc(a.label)}" aria-pressed="${abs === S.selAbs}"${a.isToday ? ' aria-current="date"' : ''}>` +
        cellInner(abs) + a.tags + '</button>';
    }

    function festHtml(f) {
      const a = cellAttrs(f.abs);
      return `<button type="button" class="cal-day cal-fest ${a.cls}" data-abs="${f.abs}"${a.style} aria-label="${esc(a.label)}, festival" aria-pressed="${f.abs === S.selAbs}"${a.isToday ? ' aria-current="date"' : ''}>` +
        '<span class="cd-eyebrow">Festival</span>' + cellInner(f.abs, f.name) + a.tags + '</button>';
    }

    let lastMonthKey = '';
    function render() {
      const y = S.ui.calYear, mo = S.ui.calMonth;
      const L = H.monthLayout(y, mo);
      el.month.textContent = L.name;
      el.common.textContent = L.common;
      el.year.textContent = y + ' DR';
      const yn = S.yearNames[y];
      el.yearName.textContent = yn || '';
      el.yearName.hidden = !yn;
      const t = todayAbs();
      el.today.disabled = false;
      el.today.title = t === null ? 'Go to the expedition start' : 'Go to the current day';

      const act = document.activeElement;
      const focusAbs = act && act.classList && act.classList.contains('cal-day') ? act.dataset.abs : null;
      const key = y + ':' + mo;
      el.grid.innerHTML = L.days.map(dayHtml).join('');
      el.fests.innerHTML = L.festivals.map(festHtml).join('');
      el.fests.hidden = !L.festivals.length;
      if (key !== lastMonthKey) {
        el.grid.classList.remove('turn');
        void el.grid.offsetWidth;
        if (lastMonthKey) el.grid.classList.add('turn');
        lastMonthKey = key;
      }
      if (focusAbs !== null) {
        const f = document.querySelector(`.cal-day[data-abs="${focusAbs}"]`);
        if (f) f.focus({ preventScroll: true });
      }
      renderUpcoming();
    }

    function renderUpcoming() {
      const base = todayAbs() !== null ? todayAbs() : S.startAbs;
      const items = [];
      for (let a = base; a <= base + 10; a++) {
        const list = allOn(S.events, a, off());
        if (list.length) items.push({ a, list });
      }
      el.upNote.textContent = todayAbs() !== null ? 'next ten days from today' : 'from the expedition start';
      if (!items.length) {
        el.upList.innerHTML = '<li class="up-empty">Nothing marked for the next ten days.</li>';
        return;
      }
      el.upList.innerHTML = items.map(({ a, list }) => `
        <li><button type="button" class="up-item" data-abs="${a}">
          <span class="up-when"><span class="up-rel">${relDays(a - base)}</span><span class="up-date">${esc(H.format(a, { short: true }))}</span></span>
          <span class="up-what">${list.map(chipHtml).join('')}</span>
        </button></li>`).join('');
    }

    // ---------- Day detail ----------
    let detailAbs = null;
    function renderDetail() {
      const abs = S.selAbs;
      const i = abs - S.startAbs;
      const d = H.fromAbs(abs);
      const m = H.moon(abs, off());
      const t = todayAbs();
      const day = weatherOf(abs);

      if (i < 0) el.dEyebrow.textContent = 'Before the expedition';
      else if (abs === t) el.dEyebrow.textContent = `Today · Day ${i + 1} of the expedition`;
      else el.dEyebrow.textContent = `Day ${i + 1} of the expedition` + (t !== null ? ' · ' + relDays(abs - t).toLowerCase() : '');
      el.dDate.textContent = H.format(abs);
      const yn = S.yearNames[d.year];
      if (d.festival) {
        const after = H.FESTIVALS.find((f) => f.key === d.festival).after;
        el.dSub.textContent = `Festival day · after ${H.MONTHS[after].name}` + (yn ? ' · ' + yn : '');
      } else {
        el.dSub.textContent = `${H.MONTHS[d.month].name} — ${H.MONTHS[d.month].common} · ${ORD[d.tendayIndex]} tenday, day ${d.dayOfTenday}` + (yn ? ' · ' + yn : '');
      }
      el.dMoon.className = 'd-moon' + (m.isNew ? ' is-new' : '');
      el.dMoon.innerHTML = moonSvg(m, 'big') +
        `<span class="d-moon-text"><span class="d-moon-name">Selûne · ${esc(m.name)}</span>` +
        `<span class="d-moon-sub">${m.isNew ? 'New moon — the sacrifice to Auril is due' : `${Math.round(m.illum * 100)}% lit · ${m.waxing ? 'waxing' : 'waning'}`}</span></span>`;

      if (i < 0) {
        el.dWeather.innerHTML = `<p class="d-none">No weather — the expedition begins on ${esc(H.format(S.startAbs))}.</p>`;
      } else if (!day) {
        el.dWeather.innerHTML = i >= ctx.MAX_DAYS
          ? `<p class="d-none">Beyond the almanac: at most ${ctx.MAX_DAYS} days can be recorded.</p>`
          : '<p class="d-none">Weather not rolled yet.</p>';
      } else {
        el.dWeather.innerHTML = weatherHtml(day);
      }

      el.dSetCurrent.hidden = i < 0 || i >= ctx.MAX_DAYS;
      const isToday = abs === t;
      el.dSetCurrent.disabled = isToday;
      el.dSetCurrent.querySelector('span').textContent = isToday ? 'This is the current day' : 'Set as current day';

      renderEvents();
      if (detailAbs !== abs) {
        if (!el.form.hidden && editingId === null) closeForm(false);
        detailAbs = abs;
        el.notes.value = S.notes[abs] || '';
        el.noteState.textContent = '';
      }
    }

    function weatherHtml(day) {
      const EV = window.IWD.EVENTS;
      const id = dominant(day);
      const strip = day.segments.map((s) => {
        const tip = `${EV[s.id].name} · ${pad(s.start)}:00–${pad(s.end)}:00`;
        return `<i style="left:${(s.start / 24) * 100}%;width:${((s.end - s.start) / 24) * 100}%;--c:${COLORS[s.id]}" title="${esc(tip)}"></i>`;
      }).join('');
      const seq = [];
      day.segments.forEach((s) => { if (seq[seq.length - 1] !== s.id) seq.push(s.id); });
      const names = seq.slice(0, 4).map((x) => esc(EV[x].name)).join(' <span class="arrow">→</span> ') + (seq.length > 4 ? ' …' : '');
      const lo = ctx.cold(day.tempMax), hi = ctx.cold(day.tempMin);
      const dc = lo.dc === hi.dc ? `DC ${hi.dc}` : `DC ${lo.dc}–${hi.dc}`;
      const tiers = lo.tier === hi.tier ? hi.tier.name : `${lo.tier.name} → ${hi.tier.name}`;
      return `
        <div class="d-wx-main" style="--c:${COLORS[id]}">${svgUse('ev-' + id)}<span><strong>${esc(EV[id].name)}</strong><span class="d-wx-seq">${names}</span></span></div>
        <div class="d-strip" aria-hidden="true">${strip}</div>
        <div class="d-stats">
          <span><b>Temperature</b>${ctx.fmtTemp(day.tempMin)} to ${ctx.fmtTemp(day.tempMax)}${ctx.unitLabel()}</span>
          <span><b>Cold table</b>${dc} · ${esc(tiers)}</span>
        </div>`;
    }

    function renderEvents() {
      const abs = S.selAbs;
      const list = allOn(S.events, abs, off());
      if (!list.length) {
        el.dEvents.innerHTML = '<li class="d-empty">No events or reminders on this day.</li>';
        return;
      }
      el.dEvents.innerHTML = list.map((e) => {
        if (e.builtin) {
          return `<li class="d-ev t-auril builtin"><span class="d-ev-bar"></span><span class="d-ev-body">
            <span class="d-ev-title">${svgUse('i-flake')}${esc(e.title)}</span>
            <span class="d-ev-meta">Built in · every new moon of Selûne</span></span></li>`;
        }
        const meta = [TYPE[e.type].label, REPEAT[e.repeat].label === 'Never' ? 'One-time' : REPEAT[e.repeat].label];
        if (e.repeat !== 'none' && e.abs !== abs) meta.push('since ' + H.format(e.abs, { short: true }) + ' ' + H.fromAbs(e.abs).year);
        return `<li class="d-ev t-${e.type}" data-id="${esc(e.id)}"><span class="d-ev-bar"></span><span class="d-ev-body">
            <span class="d-ev-title">${esc(e.title)}</span>
            <span class="d-ev-meta">${esc(meta.join(' · '))}</span>
            ${e.details ? `<span class="d-ev-details">${esc(e.details)}</span>` : ''}</span>
          <span class="d-ev-tools">
            <button type="button" class="icon-btn" data-edit="${esc(e.id)}" aria-label="Edit ${esc(e.title)}" title="Edit">${svgUse('i-edit')}</button>
            <button type="button" class="icon-btn danger" data-del="${esc(e.id)}" aria-label="Delete ${esc(e.title)}${e.repeat !== 'none' ? ' (all occurrences)' : ''}" title="Delete${e.repeat !== 'none' ? ' (all occurrences)' : ''}">${svgUse('i-trash')}</button>
          </span></li>`;
      }).join('');
    }

    // ---------- Event form ----------
    let editingId = null;
    function openForm(ev) {
      editingId = ev ? ev.id : null;
      el.formTitle.textContent = ev ? 'Edit event' : 'New event or reminder';
      el.save.textContent = ev ? 'Save changes' : 'Add to the almanac';
      el.title.value = ev ? ev.title : '';
      el.details.value = ev ? ev.details || '' : '';
      el.repeat.value = ev ? ev.repeat : 'none';
      const type = ev ? ev.type : 'natural';
      el.form.querySelectorAll('input[name="evType"]').forEach((r) => { r.checked = r.value === type; });
      el.title.classList.remove('invalid');
      el.form.hidden = false;
      el.dAdd.hidden = true;
      el.title.focus();
    }
    function closeForm(focusBack) {
      editingId = null;
      el.form.hidden = true;
      el.dAdd.hidden = false;
      if (focusBack) el.dAdd.focus();
    }
    el.dAdd.addEventListener('click', () => openForm(null));
    el.cancel.addEventListener('click', () => closeForm(true));
    el.form.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.stopPropagation(); closeForm(true); } });
    el.form.addEventListener('submit', (e) => {
      e.preventDefault();
      const title = el.title.value.trim();
      if (!title) {
        el.title.classList.add('invalid');
        el.title.focus();
        return;
      }
      const picked = el.form.querySelector('input[name="evType"]:checked');
      const old = editingId ? S.events.find((x) => x.id === editingId) : null;
      ctx.saveEvent({
        id: old ? old.id : 'e' + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36),
        abs: old ? old.abs : S.selAbs,
        title: title.slice(0, 120),
        type: picked ? picked.value : 'natural',
        repeat: el.repeat.value in REPEAT ? el.repeat.value : 'none',
        details: el.details.value.trim().slice(0, 2000),
      });
      closeForm(true);
    });
    el.title.addEventListener('input', () => el.title.classList.remove('invalid'));

    let armed = null;
    el.dEvents.addEventListener('click', (e) => {
      const ed = e.target.closest('[data-edit]');
      if (ed) {
        const ev = S.events.find((x) => x.id === ed.dataset.edit);
        if (ev) openForm(ev);
        return;
      }
      const del = e.target.closest('[data-del]');
      if (!del) return;
      // two-step delete: the first press arms the button
      if (armed !== del) {
        if (armed) armed.classList.remove('armed');
        armed = del;
        del.classList.add('armed');
        del.setAttribute('title', 'Press again to delete');
        setTimeout(() => { if (armed === del) { del.classList.remove('armed'); armed = null; } }, 3000);
        return;
      }
      armed = null;
      if (editingId === del.dataset.del) closeForm(false);
      ctx.deleteEvent(del.dataset.del);
      el.dAdd.focus();
    });

    // ---------- Notes (debounced autosave) ----------
    let noteTimer = 0, notePending = false;
    function flushNote() {
      if (!notePending) return;
      clearTimeout(noteTimer);
      notePending = false;
      ctx.setNote(detailAbs, el.notes.value);
      el.noteState.textContent = 'saved';
    }
    el.notes.addEventListener('input', () => {
      notePending = true;
      el.noteState.textContent = 'editing…';
      clearTimeout(noteTimer);
      noteTimer = setTimeout(flushNote, 600);
    });
    el.notes.addEventListener('blur', flushNote);
    // another tab changed the notes: refresh unless the user is typing here
    function syncNotes() {
      if (document.activeElement === el.notes || notePending) return;
      el.notes.value = S.notes[detailAbs] || '';
    }

    // ---------- Clicks: single = select, double = set as current ----------
    let lastClick = null;
    function onDayClick(e) {
      const b = e.target.closest('[data-abs]');
      if (!b) return;
      const abs = +b.dataset.abs;
      const now = performance.now();
      if (e.detail > 0 && lastClick && lastClick.abs === abs && now - lastClick.t < 450) {
        lastClick = null;
        ctx.setCurrent(abs);
        return;
      }
      lastClick = e.detail > 0 ? { abs, t: now } : null;
      ctx.select(abs, { fromCalendar: true });
    }
    function onDayDbl(e) {
      const b = e.target.closest('[data-abs]');
      if (b) ctx.setCurrent(+b.dataset.abs);
    }
    function onDayKey(e) {
      const b = e.target.closest('.cal-day');
      if (!b) return;
      if (e.key === 'Enter' && e.shiftKey) {
        e.preventDefault();
        ctx.setCurrent(+b.dataset.abs);
        return;
      }
      const cells = [...document.querySelectorAll('#calGrid .cal-day, #calFests .cal-day')];
      const k = cells.indexOf(b);
      let n = null;
      if (e.key === 'ArrowRight') n = k + 1;
      else if (e.key === 'ArrowLeft') n = k - 1;
      else if (e.key === 'ArrowDown') n = k < 30 ? Math.min(cells.length - 1, k + 10) : k;
      else if (e.key === 'ArrowUp') n = k >= 30 ? 29 : k - 10;
      else if (e.key === 'Home') n = 0;
      else if (e.key === 'End') n = cells.length - 1;
      if (n !== null && cells[n]) { e.preventDefault(); cells[n].focus(); }
    }
    [el.grid, el.fests].forEach((box) => {
      box.addEventListener('click', onDayClick);
      box.addEventListener('dblclick', onDayDbl);
      box.addEventListener('keydown', onDayKey);
    });
    el.upList.addEventListener('click', (e) => {
      const b = e.target.closest('[data-abs]');
      if (b) ctx.select(+b.dataset.abs);
    });
    el.dSetCurrent.addEventListener('click', () => ctx.setCurrent(S.selAbs));

    function step(dir) {
      let y = S.ui.calYear, mo = S.ui.calMonth + dir;
      if (mo < 0) { mo = 11; y--; }
      if (mo > 11) { mo = 0; y++; }
      S.ui.calYear = y;
      S.ui.calMonth = mo;
      render();
      ctx.saveSoon();
    }
    el.prev.addEventListener('click', () => step(-1));
    el.next.addEventListener('click', () => step(1));
    el.today.addEventListener('click', () => {
      const t = todayAbs();
      ctx.select(t !== null ? t : S.startAbs);
    });

    function showMonthOf(abs) {
      const m = monthOf(abs);
      S.ui.calYear = m.year;
      S.ui.calMonth = m.month;
    }

    return { render, renderDetail, renderUpcoming, flushNote, syncNotes, showMonthOf };
  }

  window.IWDCal = { TYPES, TYPE, COLORS, NEW_MOON, dominant, moonSvg, eventsOn, allOn, chipHtml, monthOf, relDays, create };
})();
