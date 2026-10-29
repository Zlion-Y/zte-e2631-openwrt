// Pin down every ZTE upgrade-package verhdr field offset empirically, from the official .bin itself.
// Usage: node pkg_map.mjs <package.bin>
import fs from 'node:fs';
import zlib from 'node:zlib';

const file = process.argv[2];
const b = fs.readFileSync(file);
console.log(`file=${file} size=${b.length} (0x${b.length.toString(16)})`);

const findAll = (needle) => { const out = []; for (let i = b.indexOf(needle); i >= 0; i = b.indexOf(needle, i + 1)) out.push(i); return out; };
const hx = (v) => '0x' + v.toString(16);
const strAt = (o, n) => b.subarray(o, o + n).toString('latin1').replace(/\x00+$/, '');
const printableRuns = (from, to) => {
  const out = []; let cur = '', co = 0;
  for (let i = from; i < to; i++) { const c = b[i]; if (c >= 0x20 && c < 0x7f) { if (!cur) co = i; cur += String.fromCharCode(c); } else { if (cur.length >= 5) out.push(`${hx(co)}:${JSON.stringify(cur)}`); cur = ''; } }
  if (cur.length >= 5) out.push(`${hx(co)}:${JSON.stringify(cur)}`);
  return out.join('  ');
};

console.log('\n--- magic anchors ---');
for (const [label, hex] of [['outer4magic', '999999994444444455555555aaaaaaaa'], ['slotmarker', '33333333cccccccc88888888dddddddd'], ['inner magic', '333333336666666699999999cccccccc'], ['uImage', '27051956'], ['33cc88dd alt', '33333333']]) {
  const hits = findAll(Buffer.from(hex, 'hex')).slice(0, 8);
  console.log(`${label.padEnd(14)} ${hex.padEnd(40)} -> ${hits.map(hx).join(' ')}`);
}

console.log('\n--- known-value scan (LE u32) ---');
const KNOWN = [
  [0x33FDCC, 'kernel segment size (7B5/8B8)'], [0x340000, 'kernel1 part size'], [0x234, 'data_off kernel'],
  [0x340214, 'data_off rootfs'], [0x1100000, 'rootfs seg 6B5'], [0x1220000, 'rootfs seg 7B5/8B8'],
  [0x700000, 'slotA base'], [0x2300000, 'slotB base'], [0x1C00000, 'slot span'], [0x3F00000, 'slotB end'],
  [0x1351, 'upgradeKey1'], [0x5211, 'upgradeKey2'], [0x10, 'serial 7B5'], [0x0f, 'serial 6B5'],
  [0xA40000, 'slotA rootfs abs'], [0x1C60000, 'slotA header abs'], [0x10000, 'block align'], [0x20000, 'erase 128k'],
  [0x0A40000, 'alt'], [0x6E00000, 'UNIFIED'], [0x6F00000, 'UUPlugin'],
];
for (const [v, note] of KNOWN) {
  const hits = [];
  for (let o = 0; o + 4 <= 0x400; o += 4) if (b.readUInt32LE(o) === v) hits.push(hx(o));
  if (hits.length) console.log(`${hx(v).padStart(10)} ${note.padEnd(26)} LE@ ${hits.join(' ')}`);
}

console.log('\n--- first 0x240 printable runs ---');
console.log(printableRuns(0, 0x240));

console.log('\n--- hexdump 0x000..0x240 ---');
for (let o = 0; o < 0x240; o += 16) {
  const row = b.subarray(o, o + 16);
  console.log(o.toString(16).padStart(4, '0') + '  ' + [...row].map(x => x.toString(16).padStart(2, '0')).join(' ') + '   ' + [...row].map(x => (x >= 0x20 && x < 0x7f) ? String.fromCharCode(x) : '.').join(''));
}

console.log('\n--- CRC hypotheses over header regions ---');
const crc = (x) => zlib.crc32(x) >>> 0;
const report = (label, calc, at) => console.log(`${label.padEnd(46)} calc=${hx(calc)}  stored@${hx(at)}=${hx(b.readUInt32LE(at))}  ${calc === b.readUInt32LE(at) ? 'MATCH <<<' : ''}`);
for (const at of [0x24, 0xa4, 0xb8, 0x108, 0x1f4 + 0x14, 0x210, 0x214]) {
  for (const [s, e] of [[0x14, 0xb8], [0x14, 0x210], [0, 0xb8], [0, 0x210], [0x14, 0xa4], [0x24, at], [0x14, at]]) {
    if (e > 0x240) continue;
    const c = crc(b.subarray(s, e));
    for (const probe of [0x24, 0xa4, 0xb8, 0x108, 0x210, 0x214, 0x1f4, 0x1fc, 0x3c, 0x48]) {
      if (probe >= e && probe >= at) continue;
      if (b.readUInt32LE(probe) === c) console.log(`!!! CRC32(b[${hx(s)}..${hx(e)}]) == u32le@${hx(probe)}`);
    }
    if (at === 0xb8 || at === 0x210) report(`CRC32(b[${hx(s)}..${hx(e)}]) vs @${hx(at)}`, c, at);
  }
}
