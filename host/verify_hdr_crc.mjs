// Reconstruct the two live slot headers captured by `hexdump -C` and verify the CRC scheme.
// One-off evidence reader: it parses the transcripts under board/ that are on this disk
// but not in git, so it is documentation of how the scheme was proven, not a tool to run.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
import zlib from 'node:zlib';

const txt = fs.readFileSync(process.argv[2] || path.join(HERE, '..', 'board', 'pass5.out.txt'), 'utf8').replace(/\r/g, '');
function grab(marker) {
  const i = txt.indexOf(marker);
  const seg = txt.slice(i, i + 4000);
  const b = Buffer.alloc(512);
  let have = 0, lastAddr = -16;
  for (const line of seg.split('\n')) {
    const m = /^([0-9a-f]{8})\s{2}((?:[0-9a-f]{2} ){1,8})(?: (?:[0-9a-f]{2} ){0,8})?\|/.exec(line.trim());
    if (!m) continue;
    let addr = parseInt(m[1], 16) & 0xfff;
    const bytes = m[2].trim().split(/\s+/).map(x => parseInt(x, 16));
    for (let j = 0; j < bytes.length && addr + j < 512; j++) { b[addr + j] = bytes[j]; have++; }
    lastAddr = addr;
  }
  return { b, have };
}
const crc = (x) => zlib.crc32(x) >>> 0;
for (const [name, marker, slotBase] of [['slotA @0x1c60000', 'U1_SLOTB_NEWHEAD', 0x2300000], ['slotB @0x3860000', 'U1_SLOTB_NEWHEAD', 0x2300000]]) void 0;

const specs = [
  ['HDR @0x3860000 (slot B, written by syn_version)', '03860000', 0x3860000],
];
// slot A header lives in pass3 output
const files = { pass3: fs.readFileSync(path.join(HERE, '..', 'board', 'pass3.out.txt'), 'utf8').replace(/\r/g, '') };
function grabFrom(text, startAddr) {
  const b = Buffer.alloc(512);
  let filled = 0;
  for (const line of text.split('\n')) {
    const bar = line.indexOf('|');
    if (bar < 0) continue;
    const toks = line.slice(0, bar).trim().split(/\s+/);
    if (!/^[0-9a-f]{8}$/.test(toks[0])) continue;
    const addr = parseInt(toks[0], 16);
    if (addr < startAddr || addr >= startAddr + 512) continue;
    toks.slice(1).forEach((t, j) => {
      if (/^[0-9a-f]{2}$/.test(t)) { b[addr - startAddr + j] = parseInt(t, 16); filled++; }
    });
  }
  return b;
}
const hdrs = [
  ['slot A hdr (flash 0x1c60000, boot-time original)', grabFrom(files.pass3, 0x1c60000)],
  ['slot B hdr (flash 0x3860000, syn_version copy)', grabFrom(txt, 0x3860000)],
];
const PKGF = process.argv[3] || process.env.E2631_TEMPLATE;
if (!PKGF) { console.error('usage: verify_hdr_crc.mjs <transcript> <official-package.bin>  (or set E2631_TEMPLATE)'); process.exit(2); }
const pkg = fs.readFileSync(PKGF);
hdrs.push(['7B5 package verhdr (file 0x14)', pkg.subarray(0x14, 0x214)]);

for (const [name, b] of hdrs) {
  console.log(`\n=== ${name} ===  nonzero bytes=${b.filter(x => x).length}`);
  const crc1 = b.readUInt32LE(0xa4), crc2 = b.readUInt32LE(0x1fc);
  console.log(`  +0x1f0 slotBase = 0x${b.readUInt32LE(0x1f0).toString(16)}`);
  console.log(`  +0x1f4 serial    = 0x${b.readUInt32LE(0x1f4).toString(16)} (${b.readUInt32LE(0x1f4)})`);
  console.log(`  +0x1f8 newflag   = 0x${b.readUInt32LE(0x1f8).toString(16)}`);
  console.log(`  CRC1 stored=0x${crc1.toString(16)}  CRC32(h[0x000:0x0a4])=0x${crc(b.subarray(0, 0xa4)).toString(16)}  ${crc1 === crc(b.subarray(0, 0xa4)) ? 'MATCH' : 'no'}`);
  console.log(`  CRC2 stored=0x${crc2.toString(16)}  CRC32(h[0x000:0x1fc])=0x${crc(b.subarray(0, 0x1fc)).toString(16)}  ${crc2 === crc(b.subarray(0, 0x1fc)) ? 'MATCH' : 'no'}`);
  console.log(`  body sizes: +0x30=${hex(b, 0x30)} +0x34=${hex(b, 0x34)} +0x38=${hex(b, 0x38)} +0x3c=${hex(b, 0x3c)} +0x40=${hex(b, 0x40)} +0x44=${hex(b, 0x44)} +0x48=${hex(b, 0x48)}`);
  console.log(`  ver=${JSON.stringify(str(b, 0x10, 28))} prod=${JSON.stringify(str(b, 0x6c, 32))} hw=${JSON.stringify(str(b, 0x110, 16))} build=${JSON.stringify(str(b, 0xa8, 20))}`);
  console.log(`  slot table +0x1e0..0x1ec = ${[0x1e0, 0x1e4, 0x1e8, 0x1ec].map(o => '0x' + b.readUInt32LE(o).toString(16)).join(', ')}`);
}
function hex(b, o) { return '0x' + b.readUInt32LE(o).toString(16); }
function str(b, o, n) { return b.subarray(o, o + n).toString('latin1').replace(/\x00+$/, ''); }

const a = hdrs[0][1], bb = hdrs[1][1];
let diff = [];
for (let i = 0; i < 512; i++) if (a[i] !== bb[i]) diff.push(i);
console.log(`\nslotA-vs-slotB header differing byte offsets (relative): ${diff.map(d => '0x' + d.toString(16)).join(' ')}`);
const p = hdrs[2][1];
diff = [];
for (let i = 0; i < 512; i++) if (p[i] !== a[i]) diff.push(i);
console.log(`packageVerhdr-vs-slotA differing offsets: ${diff.map(d => '0x' + d.toString(16)).join(' ')}`);

// payload CRC verification against the package body
const kSize = p.readUInt32LE(0x34), kOff = p.readUInt32LE(0x38), kCrc = p.readUInt32LE(0x3c);
const rSize = p.readUInt32LE(0x40), rOff = p.readUInt32LE(0x44), rCrc = p.readUInt32LE(0x48);
console.log(`\nkernel: off=0x${kOff.toString(16)} size=0x${kSize.toString(16)} storedCRC=0x${kCrc.toString(16)} calcCRC32=0x${crc(pkg.subarray(kOff, kOff + kSize)).toString(16)} ${kCrc === crc(pkg.subarray(kOff, kOff + kSize)) ? 'MATCH' : 'no'}`);
console.log(`rootfs: off=0x${rOff.toString(16)} size=0x${rSize.toString(16)} storedCRC=0x${rCrc.toString(16)} calcCRC32(AS-IS/cipher)=0x${crc(pkg.subarray(rOff, rOff + rSize)).toString(16)} ${rCrc === crc(pkg.subarray(rOff, rOff + rSize)) ? 'MATCH' : 'no'}`);
const d = hdrs[0][1]; void d;
