// Parse the live slot version-header blobs captured from /proc/zte/verinfo/cspversionheader
// and from `busybox hexdump -C -s OFF -n 512 /dev/mtdblock0`.
// Two input modes:
//   node parse_verhdr.mjs proc   <pass2.out.txt>       ; long hex string after the cspversionheader marker
//   node parse_verhdr.mjs hexdmp <mtdblock0_dump.txt>  ; `hexdump -C` text of a 512-byte window
import fs from 'node:fs';
import zlib from 'node:zlib';

const mode = process.argv[2], file = process.argv[3];
let blobs = [];

if (mode === 'proc') {
  const txt = fs.readFileSync(file, 'utf8');
  const i = txt.indexOf('cspversionheader');
  const seg = txt.slice(i, i + 6000).replace(/\r?\n/g, '');
  const m = /([0-9a-fA-F]{200,})/.exec(seg);
  if (!m) { console.error('no long hex string found'); process.exit(1); }
  const hex = m[1];
  const bytes = Buffer.from(hex.slice(0, hex.length - (hex.length % 2)), 'hex');
  console.log(`raw hex chars=${hex.length} bytes=${bytes.length}`);
  const anchor = Buffer.from('333333336666666699999999cccccccc', 'hex');
  for (let k = bytes.indexOf(anchor); k >= 0; k = bytes.indexOf(anchor, k + 1)) {
    const start = k - 0xf4;
    console.log(`inner magic at ${k} -> header start candidate 0x${start.toString(16)}`);
  }
  for (let off = 0; off + 0x200 <= bytes.length; off += 0x200) blobs.push({ name: `block@+${off.toString(16)}`, buf: bytes.subarray(off, off + 0x200) });
} else {
  const txt = fs.readFileSync(file, 'utf8');
  const bytes = Buffer.alloc(512);
  let seen = 0;
  for (const line of txt.split(/\r?\n/)) {
    const m = /^([0-9a-f]{6,8})\s+((?:[0-9a-f]{2}\s?){1,16})/i.exec(line.trim());
    if (!m) continue;
    const addr = parseInt(m[1], 16);
    for (const [j, hh] of m[2].trim().split(/\s+/).entries()) {
      if (addr + j < 512) { bytes[addr + j] = parseInt(hh, 16); seen++; }
    }
  }
  console.log(`assembled ${seen} bytes`);
  blobs.push({ name: 'window', buf: bytes });
}

const u32le = (b, o) => b.readUInt32LE(o);
const u32be = (b, o) => b.readUInt32BE(o);
const str = (b, o, n) => b.subarray(o, o + n).toString('latin1').replace(/\x00+$/, '').replace(/[^\x20-\x7e]/g, '');
const printableRuns = (b) => {
  const out = [];
  let cur = '';
  let curOff = 0;
  for (let i = 0; i < b.length; i++) {
    const c = b[i];
    if (c >= 0x20 && c < 0x7f) { if (!cur) curOff = i; cur += String.fromCharCode(c); }
    else { if (cur.length >= 4) out.push(`0x${curOff.toString(16)}:${cur}`); cur = ''; }
  }
  if (cur.length >= 4) out.push(`0x${curOff.toString(16)}:${cur}`);
  return out;
};

for (const { name, buf: b } of blobs) {
  console.log(`\n================ ${name} (${b.length} bytes) ================`);
  console.log('printable runs: ' + printableRuns(b).join('  '));
  console.log(`\ninner magic @0xf4 = ${b.subarray(0xf4, 0xfc).toString('hex')}`);
  const hdrs = [];
  for (const be of [false, true]) {
    const R = be ? u32be : u32le;
    const tag = be ? 'BE' : 'LE';
    const rows = [];
    for (const [off, label] of [[0x1c, ''], [0x20, ''], [0x2c, 'extent#1?'], [0x30, ''], [0x34, 'kernel size'], [0x38, 'kernel off'], [0x3c, 'kernel crc'], [0x40, 'rootfs size'], [0x44, 'rootfs off'], [0x48, 'rootfs crc'], [0x4c, 'slotA base'], [0x50, 'slotA span'], [0x54, 'slotA x'], [0x58, 'slotA align'], [0x5c, 'slotB base'], [0x60, 'slotB span'], [0x64, 'slotB x'], [0x68, 'slotB align'], [0xa4, 'CRC1'], [0xb8, 'CRC(outer?)'], [0x1e0, 'slottab0'], [0x1e4, 'slottab1'], [0x1e8, 'slottab2'], [0x1ec, 'slottab3'], [0x1f0, ''], [0x1f4, 'SERIAL'], [0x1f8, ''], [0x1fc, 'CRC2']]) {
      if (off + 4 > b.length) continue;
      rows.push(`${tag} +0x${off.toString(16).padStart(3, '0')} ${label.padEnd(13)} = 0x${R(b, off).toString(16).padStart(8, '0')}`);
    }
    hdrs.push(rows);
  }
  hdrs[0].forEach((l, i) => console.log(l.padEnd(42) + ' | ' + hdrs[1][i]));
  console.log('strings: +0x10=' + JSON.stringify(str(b, 0x10, 32)) + '  +0x6c=' + JSON.stringify(str(b, 0x6c, 32)) + '  +0xa8=' + JSON.stringify(str(b, 0xa8, 32)) + '  +0x110=' + JSON.stringify(str(b, 0x110, 20)) + '  +0x15c=' + JSON.stringify(str(b, 0x15c, 32)));

  const crc32 = (x) => zlib.crc32(x) >>> 0;
  const cands = [
    ['CRC32 b[0:0xa4] -> @0xa4', 0, 0xa4, 0xa4],
    ['CRC32 b[0:0xa8] excl', 0, 0xa8, 0xa4],
    ['CRC32 b[4:0xa4]', 4, 0xa4, 0xa4],
    ['CRC32 b[0x14:0xb8] -> @0xb8', 0x14, 0xb8, 0xb8],
    ['CRC32 b[0:0xb8] -> @0xb8', 0, 0xb8, 0xb8],
    ['CRC32 b[0:0x1fc] -> @0x1fc', 0, 0x1fc, 0x1fc],
    ['CRC32 b[0:0x200] -> @0x1fc', 0, 0x200, 0x1fc],
    ['CRC32 b[4:0x1fc] -> @0x1fc', 4, 0x1fc, 0x1fc],
    ['CRC32 b[0x14:0x210] (pkg view)', 0, 0x1fc, 0xa4],
  ];
  for (const [label, s, e, at] of cands) {
    const got = crc32(b.subarray(s, e));
    const want = u32le(b, at);
    console.log(`${label.padEnd(34)} calc=0x${got.toString(16)} stored=0x${want.toString(16)} ${got === want ? 'MATCH <<<' : ''}`);
  }
}
