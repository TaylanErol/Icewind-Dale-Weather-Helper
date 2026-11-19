// Reference for upper_check.c: the web's seed normalisation s.trim().toUpperCase().
'use strict';
const parts = [];
for (let c = 1; c <= 0x10FFFF; c++) {
  if (c >= 0xD800 && c <= 0xDFFF) continue;
  parts.push(Buffer.from(('x' + String.fromCodePoint(c) + 'x').trim().toUpperCase(), 'utf8').toString('hex'));
}
process.stdout.write(parts.join('\n') + '\n');
