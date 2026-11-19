#!/usr/bin/env node
/* Store fixtures, JavaScript side.
 * Runs the REAL state code of the web app (reference/app.js, the section from
 * "// ---------- State ----------" up to save(): loadStored, cleanEvent, cleanMap, clean,
 * applyState, snapshot) against a fake localStorage, plus the export / import logic of
 * app.js (exportBackup's JSON.stringify call and importBackup's onload body, copied
 * verbatim below because they are tied to Blob/FileReader).
 *
 *   node store_ref.js > store_cases.txt
 *
 * Output records (hex = UTF-8 bytes as lowercase hex):
 *   CASE <name> <mode>      mode: v2 (localStorage iwd-weather-v2), v1 (iwd-weather-v1 only),
 *                           export (v2 load, then the backup text), import (importBackup)
 *   IN <hex>
 *   OUT <hex>               expected snapshot (export: backup text; import of a non-backup:
 *                           the literal NOT_BACKUP)
 *   NUM <x.toExponential(16)> <String(x)>
 */
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');

globalThis.window = globalThis;
const REF = path.join(__dirname, 'reference');
for (const f of ['engine.js', 'harptos.js', 'calendar.js']) {
  vm.runInThisContext(fs.readFileSync(path.join(REF, f), 'utf8'), { filename: f });
}
const appSrc = fs.readFileSync(path.join(REF, 'app.js'), 'utf8');
const head = appSrc.slice(appSrc.indexOf('  const { EVENTS'), appSrc.indexOf('  const COLORS'));
const state = appSrc.slice(appSrc.indexOf('  // ---------- State ----------'), appSrc.indexOf('  function save()'));
if (head.length < 100 || state.length < 1000) throw new Error('app.js layout changed');

const EXPORT_ISO = '2026-11-19T18:04:05.123Z';
const NOW = 1763575445123;

// a fresh app instance (the app's IIFE state) over the given localStorage contents
const boot = new Function('localStorage', `${head}\n${state}\nreturn { S, snapshot, clean, applyState, days: () => days };`);
function app(store) {
  return boot({ getItem: (k) => (k in store ? store[k] : null) });
}
const hex = (s) => Buffer.from(s, 'utf8').toString('hex');

const cases = [];
function add(name, mode, input) { cases.push({ name, mode, input }); }

// ---------- v2 loads ----------
const rich = {
  v: 2, seed: 'RIME-00FF', started: 1763575445123.5, startAbs: 543858, dayCount: 40, current: 33,
  currentAbs: 543891, view: 30, selAbs: 543900, time: 13.087654321234567, unit: 'C', moonOffset: 0.5,
  yearNames: { 1489: 'Year of the Warrior Princess', 1490: 'Year of the  Ageless One  ', '-12': 'Year Minus' },
  notes: {
    543869: 'Line one\nLine "two"\t\\ tab', 543858: 'Første dag ❄ — Selûne ☾ 𝄞 emoji 🧊',
    '-200': 'before time', 543900: 'ctrl \u0001\u001f del \u007f    end',
  },
  events: [
    { id: 'e1', abs: 543860, title: '  Auril\'s followers attack Bryn Shander  ', type: 'auril', repeat: 'none', details: 'Details\nmultiline' },
    { id: 'e2', abs: 543858, title: 'Tenday market', type: 'reminder', repeat: 'tenday', details: '' },
    { id: 'e3', abs: 543870, title: 'Monthly toll', type: 'unnatural', repeat: 'monthly' },
    { id: 'e4', abs: 543859, title: 'New moon rite', type: 'natural', repeat: 'newmoon', details: 7 },
    { id: 'e5', abs: 543861, title: 'Full moon hunt', type: 'bogus', repeat: 'fullmoon', details: '  keep spaces  ' },
    { id: 'e6', abs: 543862, title: 'Yearly', type: 'natural', repeat: 'yearly', extra: { a: [1, 2, 3] } },
    { id: 'e7', abs: 543863, title: '   ', type: 'natural' },
    { id: 'e8', abs: 543863.5, title: 'fractional abs' },
    { id: '', abs: 543864, title: 'x', type: 'auril', repeat: 'never' },
    null, 5, 'str', [1],
  ],
  ui: { chainNoteHidden: 1, calYear: 1489, calMonth: 2 },
  extraTopLevel: { nested: [[[]]] },
};
rich.events[8].id = 'ezzzzzzzz';      // ids must be deterministic for the comparison
add('rich', 'v2', JSON.stringify(rich));
add('rich_export', 'export', JSON.stringify(rich));
add('rich_pretty', 'v2', JSON.stringify(rich, null, 4));

const long120 = '🧊'.repeat(59) + 'ab' + 'tail beyond the limit';   // 118 + 2 = 120 units exactly
add('long_title', 'v2', JSON.stringify({ v: 2, seed: 'S', dayCount: 1, events: [{ id: 'a', abs: 543858, title: long120, details: 'd'.repeat(2500) }] }));

add('empty_obj_v2', 'v2', '{"v":2}');
add('defaults_seeded', 'v2', '{"v":2,"seed":"GLACIER-0A3F"}');
add('daycount_big', 'v2', '{"v":2,"seed":"X","dayCount":1e10}');
add('daycount_neg', 'v2', '{"v":2,"seed":"X","dayCount":-5}');
add('daycount_str', 'v2', '{"v":2,"seed":"X","dayCount":"12"}');
add('daycount_str_ws', 'v2', '{"v":2,"seed":"X","dayCount":" \\t0x1F\\n"}');
add('daycount_arr', 'v2', '{"v":2,"seed":"X","dayCount":[["7"]]}');
add('daycount_arr2', 'v2', '{"v":2,"seed":"X","dayCount":[1,2]}');
add('daycount_bool', 'v2', '{"v":2,"seed":"X","dayCount":true,"current":0}');
add('daycount_frac', 'v2', '{"v":2,"seed":"X","dayCount":4.9}');
add('daycount_huge_neg', 'v2', '{"v":2,"seed":"X","dayCount":-4294967290}');
add('daycount_inf', 'v2', '{"v":2,"seed":"X","dayCount":1e400}');
add('current_out', 'v2', '{"v":2,"seed":"X","dayCount":3,"current":7,"view":-1}');
add('current_neg1', 'v2', '{"v":2,"seed":"X","dayCount":3,"current":-1,"currentAbs":543870,"view":2}');
add('current_neg1_noabs', 'v2', '{"v":2,"seed":"X","dayCount":3,"current":-1,"currentAbs":null}');
add('current_neg1_view_neg', 'v2', '{"v":2,"seed":"X","dayCount":3,"current":-1,"view":-1,"time":5}');
add('currentabs_mismatch', 'v2', '{"v":2,"seed":"X","dayCount":3,"current":1,"currentAbs":1}');
add('time_24', 'v2', '{"v":2,"seed":"X","dayCount":1,"time":24}');
add('time_neg', 'v2', '{"v":2,"seed":"X","dayCount":1,"time":-0.5}');
add('time_str', 'v2', '{"v":2,"seed":"X","dayCount":1,"time":"5"}');
add('time_negzero', 'v2', '{"v":2,"seed":"X","dayCount":1,"time":-0}');
add('time_tiny', 'v2', '{"v":2,"seed":"X","dayCount":1,"time":1e-7}');
add('unit_c_lower', 'v2', '{"v":2,"seed":"X","unit":"c"}');
add('moon_99', 'v2', '{"v":2,"seed":"X","moonOffset":99}');
add('moon_neg', 'v2', '{"v":2,"seed":"X","moonOffset":-30.25}');
add('moon_inf', 'v2', '{"v":2,"seed":"X","moonOffset":-1e999}');
add('moon_str', 'v2', '{"v":2,"seed":"X","moonOffset":"3"}');
add('started_float', 'v2', '{"v":2,"seed":"X","started":1.5e-300}');
add('started_inf', 'v2', '{"v":2,"seed":"X","started":1e309}');
add('started_str', 'v2', '{"v":2,"seed":"X","started":"1700000000000"}');
add('startabs_frac', 'v2', '{"v":2,"seed":"X","startAbs":5.5,"dayCount":2}');
add('startabs_neg', 'v2', '{"v":2,"seed":"X","startAbs":-500,"dayCount":2,"selAbs":-1000}');
add('startabs_exp', 'v2', '{"v":2,"seed":"X","startAbs":5.43858e5,"dayCount":2}');
add('selabs_missing_view', 'v2', '{"v":2,"seed":"X","startAbs":1000,"dayCount":5,"view":3}');
add('keys_weird', 'v2', JSON.stringify({ v: 2, seed: 'K', notes: { '012': 'twelve', ' 7 ': 'seven', x: 'nan', '1e2': 'hundred', '0x10': 'sixteen', '': 'empty key', '-0': 'minus zero', '-3': 'm3', '-1': 'm1', '3': '  ', 5: 'five', '1.0': 'one', '.5': 'half', 'Infinity': 'inf', '0b11': 'three', '+4': 'plus four' } }));
add('keys_dupe', 'v2', '{"v":2,"seed":"K","notes":{"05":"a","5":"b","-2":"c"," -2":"d","5":"e","-2":"f"}}');
add('keys_order_neg', 'v2', '{"v":2,"seed":"K","yearNames":{"-5":"a","3":"b","-1":"c","1":"d","-3":"e"}}');
add('notes_array', 'v2', '{"v":2,"seed":"K","notes":["zero",null,"two","   ",5]}');
add('notes_str', 'v2', '{"v":2,"seed":"K","notes":"nope","yearNames":7}');
add('escapes', 'v2', '{"v":2,"seed":"\\u0041\\u00e9\\ud83e\\uddca","notes":{"1":"lone \\ud800 high","2":"lone \\udc00 low","3":"nul \\u0000 inside","4":"pair \\ud83d\\ude00 ok","5":"rev \\udc00\\ud800","6":"\\/\\b\\f\\n\\r\\t\\"\\\\"}}');
add('raw_invalid_utf8', 'v2', '{"v":2,"seed":"S\xff","notes":{"1":"bad \xc0\xaf and \xed\xa0\x80 and \xf4\x90\x80\x80 trunc \xe2\x82"}}');
add('ui_odd', 'v2', '{"v":2,"seed":"U","ui":{"chainNoteHidden":"false","calYear":12.5,"calMonth":12}}');
add('ui_array', 'v2', '{"v":2,"seed":"U","ui":[1,2]}');
add('ui_falsy', 'v2', '{"v":2,"seed":"U","ui":{"chainNoteHidden":0,"calYear":-40,"calMonth":11}}');
add('seed_empty', 'v2', '{"v":2,"seed":"","dayCount":0}'.replace('""', '"Z"'));
add('v_other', 'v2', '{"v":"two","seed":"VV","dayCount":2}');
add('array_root_events', 'v2', '{"v":2,"seed":"A","events":{"0":{"id":"q","abs":1,"title":"t"}}}');
add('bom', 'v2', '﻿{"v":2,"seed":"BOM","dayCount":1}');
add('whitespace', 'v2', ' \r\n\t{ "v" : 2 , "seed" : "WS" , "dayCount" : 2 } \n');
add('full_3650', 'v2', '{"v":2,"seed":"FULL","dayCount":3650,"current":3649}');
add('festival_sel', 'v2', '{"v":2,"seed":"F","startAbs":544037,"dayCount":3,"selAbs":544040}');

// ---------- v1 migration ----------
add('v1_basic', 'v1', '{"seed":"OLD-1","started":1600000000000,"dayCount":5,"view":2,"time":7.5,"unit":"C","notes":{"1":"dropped"}}');
add('v1_daycount_str', 'v1', '{"seed":"OLD-2","dayCount":"3","view":9,"time":30}');
add('v1_v1', 'v1', '{"v":1,"seed":"OLD-3","dayCount":2}');
add('v1_big', 'v1', '{"seed":"OLD-4","dayCount":99999}');

// ---------- import ----------
add('import_rich', 'import', JSON.stringify(Object.assign({ app: 'icewind-dale-almanac', exported: EXPORT_ISO }, rich), null, 2));
add('import_started_zero', 'import', '{"seed":"I0","started":0,"dayCount":2}');
add('import_started_missing', 'import', '{"seed":"I1"}');
add('import_no_v', 'import', '{"seed":"I2","dayCount":3,"current":1,"view":0}');
add('import_no_seed', 'import', '{"dayCount":3}');
add('import_seed_empty', 'import', '{"seed":""}');
add('import_seed_num', 'import', '{"seed":5}');
add('import_array', 'import', '[{"seed":"A"}]');
add('import_bad_json', 'import', '{"seed":"A",}');
add('import_bom', 'import', '﻿{"seed":"B"}');

// ---------- run ----------
const out = [];
for (const c of cases) {
  let result;
  if (c.mode === 'import') {
    const a = app({});
    const S = a.S;
    // importBackup's r.onload body (app.js), DOM calls removed
    let obj = null;
    try { obj = JSON.parse(c.input.replace(/^﻿/, '')); } catch (e) { obj = null; }
    if (!obj || typeof obj !== 'object' || typeof obj.seed !== 'string' || !obj.seed) {
      result = 'NOT_BACKUP';
    } else {
      a.applyState(a.clean(obj));
      S.started = typeof obj.started === 'number' && obj.started ? obj.started : NOW;
      result = JSON.stringify(a.snapshot());
    }
  } else {
    const store = {};
    // the browser's UTF-8 decoding of the stored text: localStorage holds a JS string;
    // raw bytes reach it through TextDecoder (U+FFFD for invalid sequences), and
    // JSON.parse does not skip a BOM, but the native app reads files, whose BOM FileReader drops
    const text = new TextDecoder('utf-8').decode(Buffer.from(c.input, 'latin1').length && /[^\x00-\xff]/.test(c.input) ? Buffer.from(c.input, 'utf8') : Buffer.from(c.input, 'latin1'));
    store[c.mode === 'v1' ? 'iwd-weather-v1' : 'iwd-weather-v2'] = text;
    const a = app(store);
    if (c.mode === 'export') {
      // exportBackup's data (app.js)
      result = JSON.stringify(Object.assign({ app: 'icewind-dale-almanac', exported: EXPORT_ISO }, a.snapshot()), null, 2);
    } else {
      result = JSON.stringify(a.snapshot());
    }
  }
  const inBytes = /[^\x00-\xff]/.test(c.input) ? Buffer.from(c.input, 'utf8') : Buffer.from(c.input, 'latin1');
  out.push(`CASE ${c.name} ${c.mode}`, `IN ${inBytes.toString('hex')}`, `OUT ${result === 'NOT_BACKUP' ? 'NOT_BACKUP' : hex(result)}`);
}

// ---------- numbers: ECMAScript Number::toString ----------
const nums = [0.1, 0.2, 0.30000000000000004, 1 / 3, 2 / 3, 1e21, 1e20, 123456789012345680000, 1e-6, 1e-7, 1.5e-7,
  5e-324, 1.7976931348623157e308, 2 ** 53, 2 ** 53 + 2, -1.25, 100, 1234.5, 0.000001234, 13.087654321234567,
  1763575445123, 1763575445123.5, 23.99, 23.9999, 9.5, -30, 4.35, 1e300 * 10, 123e-20, 0.5, 12.000000000000002];
let s = 0x9e3779b9;
const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s; };
const dv = new DataView(new ArrayBuffer(8));
for (let i = 0; i < 4000; i++) {
  dv.setUint32(0, rnd()); dv.setUint32(4, rnd());
  const x = dv.getFloat64(0);
  if (isFinite(x)) nums.push(x);
}
for (let i = 0; i < 2000; i++) nums.push((rnd() % 2400000) / 100000, rnd() * 1000 + rnd() / 4294967296);
for (const x of nums) out.push(`NUM ${x.toExponential(16)} ${String(x)}`);

process.stdout.write(out.join('\n') + '\n');
