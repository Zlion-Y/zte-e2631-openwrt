// Decode a live slot version header that was read off the device as a hex
// stream, and check it against the two CRCs the packer and cspboot use.
//   node build/host/check_hdr_dump.mjs build/board/pass20.out.txt N1
// Capture it with: busybox hexdump -v -e '16/1 "%02x"' -s 0x3860000 -n 512 /dev/mtdblock0
import fs from 'node:fs';
import zlib from 'node:zlib';

const [, , file, tag] = process.argv;
if (!file || !tag) { console.error('usage: check_hdr_dump.mjs <capture> <N1|N2|N3>'); process.exit(2); }

const lines = fs.readFileSync(file, 'latin1').split(/\r?\n/);
const at = lines.findIndex((l) => l.trim() === tag);
if (at < 0) { console.error(`${tag} not in ${file}`); process.exit(2); }
// The dump is the first line after the tag that is only hex digits.
const dump = lines.slice(at + 1).find((l) => /^[0-9a-f]{64,}$/.test(l.trim()));
if (!dump) { console.error('no hex line after the tag'); process.exit(2); }

const h = Buffer.from(dump.trim(), 'hex');
const u32 = (o) => h.readUInt32LE(o);
const s = (o, n) => h.subarray(o, o + n).toString('latin1').replace(/\0.*$/, '');
const hx = (v) => '0x' + (v >>> 0).toString(16);
const crc = (b) => zlib.crc32(b);
const F = { key1: 0x08, key2: 0x0c, version: 0x10, bodySize: 0x30, kSize: 0x34, kFileOff: 0x38, kCrc: 0x3c,
  rSize: 0x40, rFileOff: 0x44, rCrc: 0x48, slotA: 0x4c, spanA: 0x50, slotB: 0x5c, spanB: 0x60, product: 0x6c,
  bootIncl: 0x8c, ubootCrc: 0x98, crc1: 0xa4, build: 0xa8, inner: 0xf4, hw: 0x110, slotTab: 0x1e0,
  slotStart: 0x1f0, serial: 0x1f4, isNew: 0x1f8, crc2: 0x1fc };

console.log(`${tag}: ${h.length} bytes`);
if (h.length < 0x200) { console.error('short dump'); process.exit(1); }
console.log(`  version=${JSON.stringify(s(F.version, 32))}`);
console.log(`  product=${JSON.stringify(s(F.product, 52))}  hw=${JSON.stringify(s(F.hw, 16))}  build=${JSON.stringify(s(F.build, 16))}`);
console.log(`  key=${hx(u32(F.key1))}/${hx(u32(F.key2))}  body=${hx(u32(F.bodySize))}`);
console.log(`  kernel=${hx(u32(F.kSize))}@${hx(u32(F.kFileOff))} crc=${hx(u32(F.kCrc))}`);
console.log(`  rootfs=${hx(u32(F.rSize))}@${hx(u32(F.rFileOff))} crc=${hx(u32(F.rCrc))}`);
console.log(`  slotA=${hx(u32(F.slotA))}+${hx(u32(F.spanA))}  slotB=${hx(u32(F.slotB))}+${hx(u32(F.spanB))}`);
console.log(`  tab=[${[0, 1, 2, 3].map((i) => hx(u32(F.slotTab + i * 4))).join(', ')}]`);
console.log(`  slotStart=${hx(u32(F.slotStart))} serial=${u32(F.serial)} isNew=${u32(F.isNew)} bootIncl=${hx(u32(F.bootIncl))} ubootCrc=${hx(u32(F.ubootCrc))}`);
console.log(`  inner magic@0xf4 = ${h.subarray(F.inner, F.inner + 16).toString('hex')}`);
const c1 = crc(h.subarray(0, F.crc1)) >>> 0, c2 = crc(h.subarray(0, F.crc2)) >>> 0;
console.log(`  CRC1 stored ${hx(u32(F.crc1))} computed ${hx(c1)} ${c1 === u32(F.crc1) ? 'MATCH' : 'DIFFER'}`);
console.log(`  CRC2 stored ${hx(u32(F.crc2))} computed ${hx(c2)} ${c2 === u32(F.crc2) ? 'MATCH' : 'DIFFER'}`);
if (h.length > 0x200)
  console.log(`  past the header: ${h.subarray(0x200).toString('hex').replace(/(.{64})/g, '$1\n    ').trim()}`);
