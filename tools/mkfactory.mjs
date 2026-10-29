// E2631 factory-package packer (ZX279128S / ZXHN E2631 A/B slot upgrade format).
//
//   layout: [16 outer magic][4 hdr_ofs=0][512 verhdr][body .. EOF]
//   body  : [32B slot marker][kernel extent][0x00 pad to 128K][rootfs extent][0xFF pad to 128K]
//   verhdr offsets are RELATIVE to the header start (file offset = relative + 0x14)
//
// The invariant cspboot enforces (read off the serial console, and true of every
// official package): rFileOff == 0x14 + 0x200 + ceil(kSize / erasesize), i.e. the
// rootfs begins one erase block past the *declared* kernel extent, and the header's
// own value is just that position written as a file offset. Declaring a padded
// kSize is therefore the only way to pin the rootfs anywhere else.
//
// Second rule, learned from the first image that actually booted: rSize must stop
// at the end of the squashfs (rounded to an erase block), because the kernel turns
// the rest of the "rootfs" partition into the writable "rootfs_data" - a checksum
// window over that tail makes every config change break the boot.
//
// Self-test mode reproduces its own template byte-for-byte, which is the proof
// that the geometry + CRC model is right.
//
// Usage:
//   node mkfactory.mjs selftest   --template <official.bin>
//   node mkfactory.mjs info       --template <official.bin>
//   node mkfactory.mjs pack       --template <official.bin> --kernel <k> --rootfs <r> --out <o> [opts]
// options: --version <s> --build <s> --product <s> --key1 <hex> --key2 <hex>
//          --rootfs-at <ofs>  start the rootfs extent at this slot offset
//          --header-at <ofs>  end the body here, so the version header block
//                             always lands on this slot offset (pads rootfs)
//          --note --keep-crc2
import fs from 'node:fs';
import zlib from 'node:zlib';

const BLOCK = 0x20000, VER_HDR = 0x200, OUTER = 0x14, MARKER = 32;
const M = {
  key1: 0x08, key2: 0x0c, version: 0x10, bodySize: 0x30,
  kSize: 0x34, kFileOff: 0x38, kCrc: 0x3c,
  rSize: 0x40, rFileOff: 0x44, rCrc: 0x48,
  slotABase: 0x4c, slotASpan: 0x50, slotBBase: 0x5c, slotBSpan: 0x60,
  product: 0x6c, bootIncl: 0x8c, ubootCrc: 0x98,
  crc1: 0xa4, build: 0xa8, integrity: 0xb8,
  inner: 0xf4, hw: 0x110, slotTab: 0x1e0, slotStart: 0x1f0, serial: 0x1f4, isNew: 0x1f8, crc2: 0x1fc,
};
const OUTER_MAGIC = Buffer.from('999999994444444455555555aaaaaaaa', 'hex');
const INNER_MAGIC = Buffer.from('333333336666666699999999cccccccc', 'hex');
const SLOT_MARKER = Buffer.concat([Buffer.from('33333333cccccccc88888888dddddddd', 'hex'), Buffer.alloc(16, 0xff)]);
const crc32 = (b) => zlib.crc32(b) >>> 0;
const up = (x, a) => Math.ceil(x / a) * a;

const args = (() => { const a = process.argv.slice(2); const cmd = a.shift(); const o = {};
  for (let i = 0; i < a.length; i++) if (a[i].startsWith('--')) { const key = a[i].slice(2).replace(/-([a-z])/g, (m, c) => c.toUpperCase()); o[key] = (i + 1 < a.length && !a[i + 1].startsWith('--')) ? a[++i] : true; }
  return { cmd, o }; })();
const die = (m) => { console.error('mkfactory: ' + m); process.exit(1); };

function readTemplate(path) {
  if (!path) die('no --template <official.bin>: pack copies the 512-byte version header from it. seed/vendor-verhdr-8b8.bin ships here and is enough for pack (BUILD.md, "the template")');
  if (!fs.existsSync(path)) die(`template not found: ${path}`);
  const data = fs.readFileSync(path);
  if (!data.subarray(0, 16).equals(OUTER_MAGIC)) die('template lacks the outer magic');
  const hdrOfs = OUTER + data.readUInt32LE(OUTER - 4);
  if (data.readUInt32LE(0x10) !== 0) console.error(`note: template hdr_ofs=0x${data.readUInt32LE(0x10).toString(16)} (signed package); this tool only emits hdr_ofs=0`);
  const hdr = Buffer.from(data.subarray(hdrOfs, hdrOfs + VER_HDR));
  if (!hdr.subarray(M.inner, M.inner + 16).equals(INNER_MAGIC)) die('template verhdr inner magic mismatch');
  if (crc32(hdr.subarray(0, M.crc1)) !== hdr.readUInt32LE(M.crc1)) die('template CRC1 does not verify');
  if (crc32(hdr.subarray(0, M.crc2)) !== hdr.readUInt32LE(M.crc2)) die('template CRC2 does not verify');
  const body = data.subarray(hdrOfs + VER_HDR);
  return { data, hdr, body, hdrOfs };
}

// info and selftest read the vendor payload the header points at, so they need the
// whole package; pack only ever needs the 512-byte header. Saying this beats printing
// a confident "CRC BAD" about a file that simply has no payload in it.
function needPayload(t, what) {
  const end = Math.max(t.hdr.readUInt32LE(M.kFileOff) + t.hdr.readUInt32LE(M.kSize),
    t.hdr.readUInt32LE(M.rFileOff) + t.hdr.readUInt32LE(M.rSize));
  if (t.data.length < end)
    die(`${what} needs a complete official package: this file is 0x${t.data.length.toString(16)} bytes ` +
      `but the header points at 0x${end.toString(16)}. Use a vendor .bin you own; pack works with the ` +
      `shipped seed/vendor-verhdr-8b8.bin.`);
}

function show(hdr, label) {
  const u = (k) => hdr.readUInt32LE(M[k]), s = (k, n) => hdr.subarray(M[k], M[k] + n).toString('latin1').replace(/\0+$/, '');
  const hx = (v) => '0x' + v.toString(16);
  console.log(`${label}  key=${hx(u('key1'))}/${hx(u('key2'))}  version=${JSON.stringify(s('version', 32))}  product=${JSON.stringify(s('product', 52))}`);
  console.log(`         hw=${JSON.stringify(s('hw', 16))}  build=${JSON.stringify(s('build', 16))}  bodySize=${hx(u('bodySize'))}`);
  console.log(`         kernel size=${hx(u('kSize'))} fileOff=${hx(u('kFileOff'))} crc=${hx(u('kCrc'))}`);
  console.log(`         rootfs size=${hx(u('rSize'))} fileOff=${hx(u('rFileOff'))} crc=${hx(u('rCrc'))}`);
  console.log(`         slotA=${hx(u('slotABase'))}+${hx(u('slotASpan'))} slotB=${hx(u('slotBBase'))}+${hx(u('slotBSpan'))}`);
  console.log(`         slotTab=${[0, 1, 2, 3].map(i => hx(hdr.readUInt32LE(M.slotTab + i * 4))).join(',')}`);
  console.log(`         bootIncl=${u('bootIncl')} integrity=${u('integrity')} slotStart=${hx(u('slotStart'))} serial=${u('serial')} isNew=${u('isNew')}`);
}

function assemble(hdr, kernelBuf, rootfsBuf, opts = {}) {
  let k = Buffer.from(kernelBuf); const r = Buffer.from(rootfsBuf);
  // OpenWrt's zte-slot-marker step may already have prepended the 32-byte marker.
  if (k.subarray(0, 32).equals(SLOT_MARKER)) k = k.subarray(32);
  if (opts.version) putStr(hdr, M.version, 32, opts.version);
  if (opts.build) putStr(hdr, M.build, 16, opts.build);
  if (opts.product) putStr(hdr, M.product, 52, opts.product);
  if (opts.key1 !== undefined) hdr.writeUInt32LE(parseInt(opts.key1, 16) >>> 0, M.key1);
  if (opts.key2 !== undefined) hdr.writeUInt32LE(parseInt(opts.key2, 16) >>> 0, M.key2);
  // cspboot picks the slot with the higher serial. Stamping one in the package
  // means the flashed slot wins the contest without the flasher having to read
  // the other slot correctly - which it cannot do reliably from the vendor
  // firmware, whose 4.1.25 MTD stack mis-reads blocks our kernel wrote (seen
  // 2026-10-13: max_serial() returned 16 there while the real serial was 18).
  if (opts.serial !== undefined) hdr.writeUInt32LE(parseInt(opts.serial, 10) >>> 0, M.serial);

  // Rootfs placement: the vendor packages happen to be block aligned; we can also
  // force a fixed landing offset so the board DTS partition table can be static.
  const minBody = opts.rootfsAt ? Math.max(1, parseInt(opts.rootfsAt, 16) || parseInt(opts.rootfsAt, 10)) : 0;
  const bodyLen = Math.max(up(MARKER + k.length, BLOCK), up(minBody, BLOCK));
  if (bodyLen < MARKER + k.length) die(`kernel (0x${k.length.toString(16)}) overruns the requested rootfs offset`);
  if (opts.rootfsAt && bodyLen % BLOCK) die(`rootfs offset 0x${bodyLen.toString(16)} is not erase-block aligned`);
  const body = Buffer.alloc(bodyLen, 0x00);
  SLOT_MARKER.copy(body, 0); k.copy(body, MARKER);

  // What the header declares as the kernel extent.
  //
  // cspboot does not use the header's rootfs file offset: on the serial console
  // it was seen locating the rootfs at  slot + ceil(kSize / erasesize)  and then
  // CRC-ing rSize bytes from there, which is how our first image got rejected
  // (declared kSize 0x34bdf8 => it looked at slot+0x360000, our squashfs lived at
  // slot+0x800000). So when --rootfs-at pins the rootfs, the declared kernel
  // extent has to be the whole padded region: ceil((rootfsAt - 32) / block) is
  // exactly rootfsAt, the derivation and the real layout agree, and the padding
  // belongs to the stored kernel, so its CRC covers it as written.
  const kExtent = opts.rootfsAt ? bodyLen - MARKER : k.length;
  const rFileBase = body.length;
  const rp = Buffer.from(r);
  // --header-at pins the end of the body, so the version header block always
  // lands on the slot offset the board DTS says it is on. The stored rootfs
  // extent then covers the whole padded region: the header's rSize/rCrc must
  // describe the bytes as stored, exactly as in the official packages.
  const headerAt = opts.headerAt ? Math.max(1, parseInt(opts.headerAt, 16) || parseInt(opts.headerAt, 10)) : 0;
  let rt = up(rp.length, BLOCK);
  if (headerAt) {
    if (bodyLen >= headerAt) die(`kernel region (0x${bodyLen.toString(16)}) is not below the header block at 0x${headerAt.toString(16)}`);
    if (headerAt - bodyLen < rp.length) die(`rootfs (0x${rp.length.toString(16)}) does not fit in 0x${(headerAt - bodyLen).toString(16)} before the header block`);
    rt = headerAt - bodyLen;
  }
  const rpad = Buffer.alloc(rt, 0xff); rp.copy(rpad);
  const full = Buffer.concat([body, rpad]);

  // How much of that stored rootfs the version header declares as checksummed.
  //
  // The kernel does not just mount the "rootfs" partition: ofpart's squashfs
  // split registers the erase blocks *after* the squashfs inside that partition
  // as "rootfs_data", and fstools puts the JFFS2 overlay there (measured on the
  // first boot: rootfs = slot+0x400000 size 0x800000, squashfs 0x43dc00, so
  // mtd8 "rootfs_data" = slot+0x840000 size 0x3c0000). If the declared rootfs
  // extent covered that tail, every config change would rewrite bytes inside
  // cspboot's rootfs CRC window and the next boot would reject the slot -- which
  // is what happened to the first image that booted at all. So declare exactly
  // the squashfs rounded up to an erase block: the checksummed region ends where
  // the writable region begins. A reassembled vendor package keeps its own rSize
  // (their rootfs runs right up to the header block and their config lives in a
  // partition of its own), so this only applies to our own pinned layout.
  const rCrcLen = opts.rootfsAt ? Math.max(up(rp.length, BLOCK), BLOCK) : rpad.length;
  if (rCrcLen > rpad.length) die(`rootfs extent 0x${rCrcLen.toString(16)} exceeds the 0x${rpad.length.toString(16)} in front of the header block`);
  if (opts.rootfsAt && rpad.length - rCrcLen < BLOCK) die(`no erase block left for the kernel's rootfs_data split: nothing to hold the overlay`);

  const w = (key, v) => hdr.writeUInt32LE(v >>> 0, M[key]);
  w('bodySize', full.length);
  w('kSize', kExtent); w('kFileOff', OUTER + VER_HDR + MARKER); w('kCrc', crc32(body.subarray(MARKER, MARKER + kExtent)));
  w('rSize', rCrcLen); w('rFileOff', OUTER + VER_HDR + rFileBase); w('rCrc', crc32(rpad.subarray(0, rCrcLen)));
  w('bootIncl', 0); w('ubootCrc', 0);
  hdr.writeUInt32LE(crc32(hdr.subarray(0, M.crc1)) >>> 0, M.crc1);
  if (!opts.keepCrc2) hdr.writeUInt32LE(crc32(hdr.subarray(0, M.crc2)) >>> 0, M.crc2);

  const out = Buffer.alloc(OUTER + VER_HDR + full.length);
  OUTER_MAGIC.copy(out, 0); out.writeUInt32LE(0, 0x10); hdr.copy(out, OUTER); full.copy(out, OUTER + VER_HDR);
  // The one rule the bootloader actually enforces, asserted on every package.
  if (up(kExtent, BLOCK) !== rFileBase)
    die(`cspboot would look for the rootfs at slot+0x${up(kExtent, BLOCK).toString(16)}, it is stored at slot+0x${rFileBase.toString(16)}`);
  return { out, hdr, bodyKernelOff: OUTER + VER_HDR + MARKER, bodyRootOff: OUTER + VER_HDR + rFileBase,
    rootfsStored: rpad, kExtent, rootfsSlotOffset: rFileBase, rCrcLen, overlaySlotOffset: rFileBase + rCrcLen };
}

function putStr(hdr, ofs, size, text) {
  const raw = Buffer.from(text, 'ascii');
  if (raw.length >= size) die(`value too long for ${size}-byte field: ${text}`);
  hdr.fill(0, ofs, ofs + size); raw.copy(hdr, ofs);
}

/* ---------- commands ---------- */
if (args.cmd === 'info') {
  const t = readTemplate(args.o.template);
  needPayload(t, 'info');
  show(t.hdr, 'template verhdr');
  const k = t.data.subarray(t.hdr.readUInt32LE(M.kFileOff), t.hdr.readUInt32LE(M.kFileOff) + t.hdr.readUInt32LE(M.kSize));
  const r = t.data.subarray(t.hdr.readUInt32LE(M.rFileOff), t.hdr.readUInt32LE(M.rFileOff) + t.hdr.readUInt32LE(M.rSize));
  console.log(`         kernel magic=${k.subarray(0, 4).toString('hex')} rootfs magic=${r.subarray(0, 4).toString('hex')}`);
  console.log(`         kernel CRC ${crc32(k) === t.hdr.readUInt32LE(M.kCrc) ? 'OK' : 'BAD'}, rootfs CRC(as-stored) ${crc32(r) === t.hdr.readUInt32LE(M.rCrc) ? 'OK' : 'BAD'}`);
  console.log(`         body=0x${(t.data.length - (t.hdrOfs + VER_HDR)).toString(16)}, file=0x${t.data.length.toString(16)}`);
  process.exit(0);
}

if (args.cmd === 'selftest') {
  const t = readTemplate(args.o.template);
  needPayload(t, 'selftest');
  const kOff = t.hdr.readUInt32LE(M.kFileOff), kLen = t.hdr.readUInt32LE(M.kSize);
  const rOff = t.hdr.readUInt32LE(M.rFileOff), rLen = t.hdr.readUInt32LE(M.rSize);
  const kernel = t.data.subarray(kOff, kOff + kLen);
  const rootfsStored = t.data.subarray(rOff, rOff + rLen);
  if (kernel.subarray(0, 4).toString('hex') !== '27051956') console.error('note: kernel extent is not a plaintext uImage');
  const r = assemble(Buffer.from(t.hdr), kernel, rootfsStored, {});
  const same = Buffer.compare(r.out, t.data) === 0;
  console.log(`template    : 0x${t.data.length.toString(16)} bytes`);
  console.log(`rebuilt     : 0x${r.out.length.toString(16)} bytes  (kernel 0x${kLen.toString(16)}, rootfs-stored 0x${rLen.toString(16)})`);
  console.log(`byte-exact  : ${same ? 'YES — geometry + CRC model proven' : 'NO'}`);
  if (!same) {
    let n = 0;
    for (let i = 0; i < Math.min(r.out.length, t.data.length); i++) if (r.out[i] !== t.data[i]) { if (n < 20) console.log(`  diff @0x${i.toString(16)}: tmpl=${t.data[i].toString(16)} ours=${r.out[i].toString(16)}`); n++; }
    console.log(`  total differing bytes: ${n}`);
    process.exit(1);
  }
  console.log(`self-check  : CRC1 ${crc32(r.hdr.subarray(0, M.crc1)) === r.hdr.readUInt32LE(M.crc1) ? 'OK' : 'BAD'}, CRC2 ${crc32(r.hdr.subarray(0, M.crc2)) === r.hdr.readUInt32LE(M.crc2) ? 'OK' : 'BAD'}`);
  process.exit(0);
}

if (args.cmd === 'pack') {
  const t = readTemplate(args.o.template);
  let kernel, rootfs;
  if (args.o.payloadFromTemplate) {
    const kO = t.hdr.readUInt32LE(M.kFileOff), kL = t.hdr.readUInt32LE(M.kSize);
    const rO = t.hdr.readUInt32LE(M.rFileOff), rL = t.hdr.readUInt32LE(M.rSize);
    kernel = t.data.subarray(kO, kO + kL); rootfs = t.data.subarray(rO, rO + rL);
    console.log('payload  : reused verbatim from the template (as-stored bytes)');
  } else {
    kernel = fs.readFileSync(args.o.kernel); rootfs = fs.readFileSync(args.o.rootfs);
  }
  const r = assemble(Buffer.from(t.hdr), kernel, rootfs, {
    version: args.o.version, build: args.o.build, product: args.o.product,
    key1: args.o.key1, key2: args.o.key2, keepCrc2: args.o.keepCrc2,
    rootfsAt: args.o.rootfsAt, headerAt: args.o.headerAt });
  const slot = t.hdr.readUInt32LE(M.slotABase), span = t.hdr.readUInt32LE(M.slotASpan);
  const bodyLen = r.out.length - (OUTER + VER_HDR);
  console.log(`out      : ${args.o.out}  0x${r.out.length.toString(16)} bytes (body 0x${bodyLen.toString(16)})`);
  console.log(`kernel   : 0x${kernel.length.toString(16)} at file 0x${r.bodyKernelOff.toString(16)}, magic ${r.out.subarray(r.bodyKernelOff, r.bodyKernelOff + 4).toString('hex')}`);
  console.log(`declared : kSize 0x${r.kExtent.toString(16)} (kernel + pad) => cspboot looks for the rootfs at slot + 0x${r.rootfsSlotOffset.toString(16)}`);
  console.log(`rootfs   : 0x${rootfs.length.toString(16)} stored 0x${r.rootfsStored.length.toString(16)} at file 0x${r.bodyRootOff.toString(16)}`);
  console.log(`           checksummed 0x${r.rCrcLen.toString(16)}, kernel overlay split starts at slot + 0x${r.overlaySlotOffset.toString(16)}`);
  console.log(`slot fit : body 0x${bodyLen.toString(16)} vs slot span 0x${span.toString(16)} -> ${bodyLen + BLOCK <= span ? 'FITS' : 'TOO BIG'} (free 0x${(span - bodyLen - BLOCK).toString(16)})`);
  console.log(`header   : will land at slot_base + 0x${bodyLen.toString(16)} (i.e. 0x${(slot + bodyLen).toString(16)})`);
  if (bodyLen + BLOCK > span) die('image does not fit a firmware slot');
  fs.writeFileSync(args.o.out, r.out);
  console.log(`post-write verify: ${readTemplateWrite(args.o.out)}`);
  process.exit(0);
}

function readTemplateWrite(path, expected) {
  const d = fs.readFileSync(path);
  const h = d.subarray(OUTER, OUTER + VER_HDR);
  const ok1 = crc32(h.subarray(0, M.crc1)) === h.readUInt32LE(M.crc1);
  const ok2 = crc32(h.subarray(0, M.crc2)) === h.readUInt32LE(M.crc2);
  const kOff = h.readUInt32LE(M.kFileOff), kLen = h.readUInt32LE(M.kSize);
  const rOff = h.readUInt32LE(M.rFileOff), rLen = h.readUInt32LE(M.rSize);
  const okK = crc32(d.subarray(kOff, kOff + kLen)) === h.readUInt32LE(M.kCrc);
  const okR = crc32(d.subarray(rOff, rOff + rLen)) === h.readUInt32LE(M.rCrc);
  const okS = h.readUInt32LE(M.bodySize) === d.length - (OUTER + VER_HDR);
  const okE = kOff + kLen <= rOff;
  const okD = OUTER + VER_HDR + up(kLen, BLOCK) === rOff;   // cspboot's own rootfs derivation
  const okM = d.subarray(OUTER + VER_HDR, OUTER + VER_HDR + MARKER).equals(SLOT_MARKER);
  return [ `CRC1 ${ok1 ? 'ok' : 'FAIL'}`, `CRC2 ${ok2 ? 'ok' : 'FAIL'}`, `kernelCRC ${okK ? 'ok' : 'FAIL'}`,
    `rootfsCRC ${okR ? 'ok' : 'FAIL'}`, `bodySize ${okS ? 'ok' : 'FAIL'}`, `layout ${okE ? 'ok' : 'FAIL'}`,
    `derivedFsOff ${okD ? 'ok' : 'FAIL'}`, `slotMarker ${okM ? 'ok' : 'FAIL'}`,
    `innerMagic ${h.subarray(M.inner, M.inner + 16).equals(INNER_MAGIC) ? 'ok' : 'FAIL'}` ].join(' ');
}

die('unknown command; use selftest | info | pack');
