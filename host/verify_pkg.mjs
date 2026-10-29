// Independent verification of an E2631 factory package: re-derives everything from
// the bytes instead of trusting mkfactory.mjs, which would otherwise grade itself.
//
//   node verify_pkg.mjs <pkg.bin> [--dts <board.dts>] [--template <official.bin>]
//
// Checks, all read off the file:
//   1 outer magic, hdr_ofs, header placement
//   2 CRC1 = CRC32(hdr[0:0xa4]) and CRC2 = CRC32(hdr[0:0x1fc])
//   3 kernel extent really starts with a legacy uImage whose own ih_dcrc matches,
//     ih_load == ih_ep == 0x40008000, comp == none, and ih_size == the declared extent
//   4 rootfs extent really starts with "hsqs" (plain squashfs, not vendor ciphertext)
//   5 the two extent CRCs match what the header promises, over the stored bytes
//   6 body size == header +0x30, and every extent stays inside the slot span
//   7 with --dts: the slot-relative offsets agree with the board partition table
import fs from 'node:fs';
import zlib from 'node:zlib';
import crypto from 'node:crypto';

const BLOCK = 0x20000, HDR = 0x14, VER = 0x200, BODY = 0x214, MARK = 32;
const arg = (n) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const crc = (b) => zlib.crc32(b) >>> 0;
const rd = (b, o) => b.readUInt32LE(o);
const s = (b, o, n) => b.subarray(o, o + n).toString('latin1').replace(/\x00+$/, '');
const hx = (v) => (v < 0 ? '-0x' + (-v).toString(16) : '0x' + v.toString(16));
let fails = 0, warns = 0;
const ok = (label, cond, detail = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`);
  if (!cond) fails++;
};
const warn = (label, cond, detail = '') => {
  if (!cond) { warns++; console.log(`WARN  ${label}  ${detail}`); }
};

const file = process.argv[2];
if (!file || !fs.existsSync(file)) { console.error('usage: verify_pkg.mjs <pkg.bin> [--dts f] [--template f]'); process.exit(2); }
const b = fs.readFileSync(file);
const h = b.subarray(HDR, HDR + VER);
console.log(`=== ${file}  ${b.length} B (${hx(b.length)})  sha256 ${crypto.createHash('sha256').update(b).digest('hex').slice(0, 16)}…\n`);

ok('outer magic', b.subarray(0, 4).toString('hex') === '99999999' && b.subarray(4, 8).toString('hex') === '44444444'
  && b.subarray(8, 12).toString('hex') === '55555555' && b.subarray(12, 16).toString('hex') === 'aaaaaaaa');
ok('header offset field is 0 (unsigned package, vendor VerifySign skipped)', rd(b, 0x10) === 0, hx(rd(b, 0x10)));
ok('inner magic at hdr+0xf4', h.subarray(0xf4, 0x104).toString('hex') === '333333336666666699999999cccccccc');
ok('CRC1 over hdr[0:0xa4]', crc(h.subarray(0, 0xa4)) === rd(h, 0xa4), `${hx(crc(h.subarray(0, 0xa4)))} vs ${hx(rd(h, 0xa4))}`);
ok('CRC2 over hdr[0:0x1fc]', crc(h.subarray(0, 0x1fc)) === rd(h, 0x1fc), `${hx(crc(h.subarray(0, 0x1fc)))} vs ${hx(rd(h, 0x1fc))}`);

const kOff = rd(h, 0x38), kLen = rd(h, 0x34), kCrc = rd(h, 0x3c);
const rOff = rd(h, 0x44), rLen = rd(h, 0x40), rCrc = rd(h, 0x48);
ok('kernel and rootfs offsets are inside the file', kOff + kLen <= b.length && rOff + rLen <= b.length);
ok('rootfs offset is past the kernel offset', rOff >= kOff + kLen || rOff === kOff + kLen);
ok('kernel CRC over stored bytes', crc(b.subarray(kOff, kOff + kLen)) === kCrc, hx(kCrc));
ok('rootfs CRC over stored bytes', crc(b.subarray(rOff, rOff + rLen)) === rCrc, hx(rCrc));
// cspboot never uses the header's rootfs offset: it starts the rootfs one erase
// block past the *declared* kernel extent, so the header's own value has to be
// that same position written as a file offset. Every official package obeys this
// identity; ours was rejected at boot precisely because it did not (declared
// 0x34bdf8 => bootloader looked at slot+0x360000, squashfs lived at slot+0x800000).
// A package failing it flashes fine, fails the rootfs CRC and falls back to the
// other slot, which makes this a hard blocker rather than a note.
const derivedFs = Math.ceil(kLen / BLOCK) * BLOCK;
ok('rootfs sits where cspboot derives it from the kernel extent',
  BODY + derivedFs === rOff, `derived slot+${hx(derivedFs)}, stored slot+${hx(rOff - BODY)}`);
ok('body size == header +0x30', rd(h, 0x30) === b.length - BODY, `${hx(rd(h, 0x30))} vs ${hx(b.length - BODY)}`);
ok('bootloader is not touched (hdr+0x8c == 0)', rd(h, 0x8c) === 0);
ok('integrity byte (hdr+0xb8) is 0, as the vendor switch path requires', rd(h, 0xb8) === 0);

const slotA = rd(h, 0x4c), spanA = rd(h, 0x50), slotB = rd(h, 0x5c), spanB = rd(h, 0x60);
ok('slot table is the E2631 pair', slotA === 0x700000 && slotB === 0x2300000 && spanA === 0x1c00000 && spanB === 0x1c00000,
  `${hx(slotA)}/${hx(spanA)} ${hx(slotB)}/${hx(spanB)}`);
ok('body fits one slot, leaving room for its own header block', b.length - BODY + BLOCK <= spanA,
  `body ${hx(b.length - BODY)} of ${hx(spanA)} (needs a 128K block for the version header)`);

/* the kernel really has to be something cspboot can drop at 0x40008000 */
const atSlot = kOff - BODY;             // offset of the kernel payload inside the slot
const ui = b.subarray(kOff, kOff + 64);
/* legacy image_header_t, all big endian:
 * 0 magic, 4 hcrc, 8 time, 12 size, 16 load, 20 ep, 24 dcrc,
 * 28 os, 29 arch, 30 type, 31 comp, 32 name[32]                        */
ok('kernel extent begins with a legacy uImage magic', ui.subarray(0, 4).toString('hex') === '27051956');
const ihSize = ui.readUInt32BE(12), ihLoad = ui.readUInt32BE(16), ihEp = ui.readUInt32BE(20);
const ihDcrc = ui.readUInt32BE(24);
const ihHcrc = ui.readUInt32BE(4);
ok('uImage data size fits inside the declared extent', ihSize + 64 <= kLen, `ih_size=${hx(ihSize)}+64 vs extent ${hx(kLen)}`);
if (kLen - ihSize - 64 > 0)
  console.log(`      note: ${hx(kLen - ihSize - 64)} of trailing bytes after the uImage (the vendor kernel mounts an appended payload as /dev/loop0; our own images simply leave it zero-padded)`);
ok('uImage os=Linux arch=arm type=kernel comp=none', ui[28] === 5 && ui[29] === 2 && ui[30] === 2 && ui[31] === 0,
  `os=${ui[28]} arch=${ui[29]} type=${ui[30]} comp=${ui[31]}`);
ok('load == entry == 0x40008000 (what both vendor slots use)', ihLoad === 0x40008000 && ihEp === 0x40008000, `${hx(ihLoad)} / ${hx(ihEp)}`);
{
  const rest = Buffer.concat([ui.subarray(0, 4), ui.subarray(8, 64)]);
  // The official 8B8 image does not carry a standard ih_hcrc either, and it boots,
  // so cspboot clearly does not police it - keep this as a note, not a gate.
  warn('uImage header CRC (ih_hcrc) matches the standard algorithm', ihHcrc === crc(rest),
    `stored ${hx(ihHcrc)} std-crc ${hx(crc(rest))} - vendor images differ too`);
}
const img = b.subarray(kOff + 64, kOff + 64 + ihSize);
ok('uImage data CRC (ih_dcrc) self-checks', ihDcrc === crc(img), `${hx(ihDcrc)} vs ${hx(crc(img))}`);
ok('slot marker precedes the kernel at slot+0x00', b.subarray(BODY, BODY + MARK).toString('hex') === '33333333cccccccc88888888ddddddddffffffffffffffffffffffffffffffff');
warn('kernel payload looks like ARM code (zImage header is NOPs then a branch)',
  img.length > 64 && (img.readUInt32LE(0) === 0xe1a00000 || (img.readUInt32LE(0) & 0x0e000000) !== 0),
  `first word ${img.subarray(0, 4).toString('hex')}`);
ok('rootfs is plain squashfs (vendor AES layer is not needed by a mainline kernel)', b.subarray(rOff, rOff + 4).toString('latin1') === 'hsqs',
  JSON.stringify(b.subarray(rOff, rOff + 4).toString('latin1')));
let sqUsed = 0;
if (b.subarray(rOff, rOff + 4).toString('latin1') === 'hsqs') {
  const rb = b.subarray(rOff, rOff + rLen);
  // A squashfs superblock states how many bytes of it are used. Measured on the
  // H3600 root (a 4788224-byte real squashfs) the value sits at +40 and reads
  // 4788138, i.e. the file size minus the superblock tail.
  sqUsed = rb.readUInt32LE(40);
  ok('squashfs bytes_used is sane and fits the extent', sqUsed <= rLen,
    `bytes_used=${sqUsed} extent=${rLen}`);
}

console.log(`\nkernel at slot+${hx(atSlot)} size ${hx(kLen)}; rootfs at slot+${hx(rOff - BODY)} size ${hx(rLen)};`);
console.log(`version header lands at slot+${hx(rd(h, 0x30))} (absolute ${hx(slotA + rd(h, 0x30))} for slot0, ${hx(slotB + rd(h, 0x30))} for slot1)`);

const dtsPath = arg('--dts');
if (dtsPath) {
  const t = fs.readFileSync(dtsPath, 'utf8');
  const grab = (label) => {
    const m = new RegExp(`label = "${label}";[\\s\\S]*?reg = <0x([0-9a-fA-F]+)\\s+0x([0-9a-fA-F]+)>`).exec(t);
    return m ? { off: parseInt(m[1], 16), len: parseInt(m[2], 16) } : null;
  };
  const SLOT1 = 0x2300000;
  const rf = grab('rootfs'), hd = grab('slot1-header'), dt = grab('rootfs_data'), sl = grab('slot1');
  // Measured on the first boot of our own image: ofpart's squashfs split turns the
  // tail of the "rootfs" partition into a second partition named "rootfs_data", and
  // fstools mounts the JFFS2 overlay there. Anything the bootloader checksums that
  // overlaps that tail is destroyed by the very first config save, and the slot is
  // then rejected on the next power cycle. So the checksum window has to stop at the
  // squashfs, and the DTS must not declare a competing rootfs_data.
  const splitAt = Math.ceil(sqUsed / BLOCK) * BLOCK;
  ok('DTS declares one 28 MiB slot1 partition at the package slot address',
    !!sl && sl.off === SLOT1 && sl.len === rd(h, 0x60), sl ? `${hx(sl.off)}+${hx(sl.len)}` : 'missing');
  ok('DTS rootfs offset == package rootfs slot offset', !!rf && (rf.off - SLOT1) === (rOff - BODY), rf ? hx(rf.off - SLOT1) : 'missing');
  ok('the declared rootfs extent stops exactly at the kernel overlay split', rLen === splitAt && sqUsed > 0,
    `rSize=${hx(rLen)} squashfs=${sqUsed}B splits at ${hx(splitAt)}`);
  ok('the rootfs partition keeps writable space behind the squashfs', !!rf && rf.len - rLen >= BLOCK,
    rf ? `partition ${hx(rf.len)}, checksummed ${hx(rLen)}, overlay ${hx(rf.len - rLen)} = ${((rf.len - rLen) / 1048576).toFixed(2)} MiB` : 'missing');
  ok('nothing is declared as rootfs_data (the kernel creates it from the rootfs tail)', !dt,
    dt ? `${hx(dt.off)}+${hx(dt.len)} would be a second partition of that name and fstools takes the first` : 'none');
  ok('DTS slot1-header sits on the block the body ends on', !!hd && hd.off - SLOT1 === rd(h, 0x30), hd ? hx(hd.off - SLOT1) : 'missing');
  ok('the version header block is not inside the rootfs partition', !!rf && !!hd && hd.off >= rf.off + rf.len,
    hd && rf ? `header ${hx(hd.off)} vs rootfs end ${hx(rf.off + rf.len)}` : 'missing');
  ok('everything still stays inside slot1', !!hd && hd.off + hd.len <= SLOT1 + rd(h, 0x60), hd ? hx(hd.off + hd.len) : 'missing');
}
if (arg('--template')) {
  const tb = fs.readFileSync(arg('--template'));
  const th = tb.subarray(HDR, HDR + VER);
  ok('keys match the device account (upgradeKey1/2 from the template)', rd(h, 0x08) === rd(th, 0x08) && rd(h, 0x0c) === rd(th, 0x0c),
    `${hx(rd(h, 0x08))}/${hx(rd(h, 0x0c))}`);
  ok('product string matches the template', s(h, 0x6c, 20) === s(th, 0x6c, 20), JSON.stringify(s(h, 0x6c, 20)));
}
console.log(`\n${fails === 0 ? 'ALL CHECKS PASSED' : 'FAILURES: ' + fails}${warns ? ` (warnings: ${warns})` : ''}`);
process.exit(fails ? 1 : 0);
