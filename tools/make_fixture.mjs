// Make a small, self-consistent E2631 upgrade package so the C slot tool can be
// tested against it in CI. The real vendor package cannot be committed, and
// "e2631-fwslot check" is the first thing that runs on the device before it
// writes flash, so its parser has to be exercised somewhere.
//
//   node build/tools/make_fixture.mjs [dir]
//     -> <dir>/fixture.bin        accepted by the packer and by e2631-fwslot check
//     -> <dir>/fixture-broken.bin one byte changed in the kernel extent
//
// The package is produced by mkfactory.mjs, the same code the image build uses,
// so the C reader is tested against the real writer rather than a hand-drawn
// guess.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const dir = process.argv[2] || 'out';
fs.mkdirSync(dir, { recursive: true });
const f = (n) => path.join(dir, n);

const crc = (b) => zlib.crc32(b) >>> 0;

// --- version header, as an unsigned package would carry it -------------------
const hdr = Buffer.alloc(0x200);
const put = (o, v) => hdr.writeUInt32LE(v >>> 0, o);
const str = (o, s) => hdr.write(s, o, s.length, 'latin1');
put(0x08, 0x1351); put(0x0c, 0x5211);
str(0x10, 'OpenWrt-FIXTURE');
str(0x6c, 'ZXHN E2631 V1.0.0');
str(0xa8, '20261013000000');
str(0x110, 'V1.0.0');
hdr.set(Buffer.from('333333336666666699999999cccccccc', 'hex'), 0xf4);
put(0x4c, 0x00700000); put(0x50, 0x01c00000);
put(0x5c, 0x02300000); put(0x60, 0x01c00000);
[0x00700000, 0x02300000, 0x02300000, 0x03f00000].forEach((v, i) => put(0x1e0 + i * 4, v));
put(0xa4, crc(hdr.subarray(0, 0xa4)));
put(0x1fc, crc(hdr.subarray(0, 0x1fc)));
fs.writeFileSync(f('template.bin'), Buffer.concat([
  Buffer.from('999999994444444455555555aaaaaaaa', 'hex'), Buffer.alloc(4), hdr, Buffer.alloc(0x20000),
]));

// --- payloads that look like the real ones ----------------------------------
const kernel = Buffer.alloc(0x1000);
kernel.writeUInt32BE(0x27051956, 0);            // uImage magic
kernel.writeUInt32BE(kernel.length - 64, 12);   // data size
kernel.writeUInt32BE(0x40008000, 16);           // load  == entry, both slots use this
kernel.writeUInt32BE(0x40008000, 20);
kernel[28] = 5; kernel[29] = 2; kernel[30] = 2; kernel[31] = 0;  // Linux, ARM, kernel, no compression
kernel.fill(0xa5, 64);
kernel.writeUInt32BE(crc(kernel.subarray(64)), 24);              // data CRC, big endian as uImage stores it
fs.writeFileSync(f('kernel.bin'), kernel);

const rootfs = Buffer.alloc(0x1000);
rootfs.fill(0x5a, 4);
rootfs.write('hsqs', 0, 'latin1');
rootfs.writeUInt32LE(8, 4);              // inode count
rootfs.writeUInt32LE(0x40000, 12);       // block size
rootfs[20] = 1;                          // compression: zlib
rootfs[21] = 18;                         // block log
rootfs.writeBigUInt64LE(BigInt(rootfs.length), 40);  // bytes used
fs.writeFileSync(f('rootfs.bin'), rootfs);

// --- pack it: kernel block, rootfs block, version header block --------------
// The rootfs region needs one erase block *behind* the squashfs as well: that is
// where the kernel puts its rootfs_data split, and the packer refuses a layout
// that leaves no room for it (see mkfactory's rSize rule).
execFileSync(process.execPath, [fileURLToPath(new URL('./mkfactory.mjs', import.meta.url)), 'pack',
  '--template', f('template.bin'), '--kernel', f('kernel.bin'), '--rootfs', f('rootfs.bin'),
  '--rootfs-at', '0x20000', '--header-at', '0x60000', '--out', f('fixture.bin')], { stdio: 'inherit' });

// --- one flipped byte inside the kernel extent ------------------------------
const broken = fs.readFileSync(f('fixture.bin'));
broken[0x240] ^= 0xff;                          // kernel data, past the uImage header
fs.writeFileSync(f('fixture-broken.bin'), broken);
console.log(`fixture: ${fs.statSync(f('fixture.bin')).size} B, broken copy flipped at 0x240`);
