// Is the on-flash slot record longer than 512 bytes?
// Reads the 6B5-era programmer dump around a known header location and shows
// the 0x200..0x800 region, plus where the 128 KiB erase-block boundary falls.
import fs from 'node:fs';

const file = process.argv[2];
const base = parseInt(process.argv[3], 16);
const len = parseInt(process.argv[4] || '0x800', 16);
const fd = fs.openSync(file, 'r');
const b = Buffer.alloc(len);
fs.readSync(fd, b, 0, len, base);

const hex = (x) => '0x' + x.toString(16);
const line = (o) => {
  const row = b.subarray(o, o + 16);
  return hex(base + o).padStart(8) + '  ' + [...row].map(x => x.toString(16).padStart(2, '0')).join(' ') +
    '   ' + [...row].map(x => (x >= 0x20 && x < 0x7f) ? String.fromCharCode(x) : '.').join('');
};
const runs = [];
{ let cur = '', co = 0;
  for (let i = 0; i < b.length; i++) { const c = b[i];
    if (c >= 0x20 && c < 0x7f) { if (!cur) co = i; cur += String.fromCharCode(c); }
    else { if (cur.length >= 5) runs.push(`${hex(base + co)}:${JSON.stringify(cur)}`); cur = ''; } }
  if (cur.length >= 5) runs.push(`${hex(base + co)}:${JSON.stringify(cur)}`); }
console.log(`file=${file} window=${hex(base)}..${hex(base + len)}\nprintable runs: ${runs.join('  ')}`);

const rd = (o) => b.readUInt32LE(o);
console.log(`\nheader block start ${hex(base)}; erase block = 0x20000 -> next header candidate at ${hex(base + 0x20000)}`);
console.log('u32 LE at 0x1f0..0x20f :', [...Array(8)].map((_, i) => `${hex(i * 4)}=${hex(rd(0x1f0 + i * 4))}`).join(' '));

console.log('\n--- 0x000..0x030 (fields we already trust) ---');
for (let o = 0; o < 0x40; o += 16) console.log(line(o));
console.log('\n--- 0x1e0..0x2ff (serial / CRC2 / the 0x200 boundary) ---');
for (let o = 0x1e0; o < 0x300; o += 16) console.log(line(o));
console.log('\n--- 0x300..0x540 (is there a second record? SR1010 reportedly uses 0x510) ---');
for (let o = 0x300; o < 0x540; o += 16) console.log(line(o));

let firstNonFF = -1, lastNonFF = -1;
for (let i = 0x200; i < b.length; i++) if (b[i] !== 0xff) { if (firstNonFF < 0) firstNonFF = i; lastNonFF = i; }
console.log(`\n0x200..end: first non-FF at ${firstNonFF < 0 ? 'none (all FF)' : hex(base + firstNonFF)} (rel ${hex(firstNonFF)}), last non-FF at ${lastNonFF < 0 ? '-' : hex(base + lastNonFF)} (rel ${hex(lastNonFF)})`);
console.log(`distinct byte values in 0x200..0x400: ${new Set([...b.subarray(0x200, 0x400)]).size}`);
