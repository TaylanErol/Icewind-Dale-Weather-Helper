#!/usr/bin/env node
/* Writes the parity seed list: one seed per line, hex-encoded UTF-8 bytes.
 *   node gen_seeds.js COUNT > seeds.txt
 * Deterministic. Covers the empty seed, the app's WORD-XXXX seeds, plain ASCII, JSON-ish and
 * control characters, accented/CJK/emoji text (surrogate pairs), very long seeds and a few
 * byte strings that are not valid UTF-8 (decoded to U+FFFD on both sides). */
'use strict';
const count = Number(process.argv[2] || 320);
let s = 0x2545f491;
const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const words = ['RIME', 'FROST', 'AURIL', 'HOAR', 'SLEET', 'GLACIER', 'TUNDRA', 'BOREAL', 'SPINE', 'MAER', 'DUN', 'CAIRN', 'KELVIN', 'BREMEN', 'TERMALAINE', 'ICEFANG'];
const uni = ['é', 'ß', 'Ø', 'ñ', 'Selûne', '☃', '❄', '雪', '氷の谷', 'Ж', 'ἀ', '𝄞', '🧊', '🌨️', '👩‍👩‍👧', 'é', '‍', '﻿', ' ', '￿', '\u{10ffff}'];
const out = [];
const bytes = (b) => out.push(Buffer.from(b).toString('hex'));
const str = (t) => out.push(Buffer.from(t, 'utf8').toString('hex'));
['', 'RIME-00FF', 'a', 'A', ' ', '0', '1489', 'Icewind Dale', 'Rime of the Frostmaiden', '"quoted"', 'back\\slash',
  'tab\there', 'line\r\nbreak', '{"json":1}', '\u0001\u001f', 'é☃𝄞', '😀', 'x'.repeat(5000), '雪'.repeat(1200),
  '🧊'.repeat(700), 'RIME-0000', 'ICEFANG-FFFF'].forEach(str);
[[0xff], [0xc3], [0xe0, 0x80, 0x62], [0xed, 0xa0, 0x80], [0xf0, 0x9f, 0x98, 0x63], [0x61, 0xc0, 0xaf, 0x62],
  [0xf4, 0x90, 0x80, 0x80], [0xe2, 0x82], [0x80, 0x80, 0x80]].forEach(bytes);
while (out.length < count) {
  const k = out.length % 6;
  let t = '';
  if (k === 0) t = pick(words) + '-' + Math.floor(rnd() * 65536).toString(16).toUpperCase().padStart(4, '0');
  else if (k === 1) { const n = 1 + Math.floor(rnd() * 40); for (let i = 0; i < n; i++) t += String.fromCharCode(32 + Math.floor(rnd() * 95)); }
  else if (k === 2) { const n = 1 + Math.floor(rnd() * 12); for (let i = 0; i < n; i++) t += rnd() < 0.5 ? pick(uni) : String.fromCharCode(97 + Math.floor(rnd() * 26)); }
  else if (k === 3) t = String(Math.floor(rnd() * 1e9) - 5e8);
  else if (k === 4) { const n = 100 + Math.floor(rnd() * 3000); for (let i = 0; i < n; i++) t += String.fromCharCode(33 + Math.floor(rnd() * 94)); }
  else { const n = 1 + Math.floor(rnd() * 30); for (let i = 0; i < n; i++) { const c = 0x80 + Math.floor(rnd() * 0xd700); t += String.fromCharCode(c); } }
  str(t);
}
process.stdout.write(out.join('\n') + '\n');
