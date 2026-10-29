// Verify the E2631 package body geometry model:
//   package = [16 outer magic][4 hdr_ofs][512 verhdr][body .. EOF]
//   body is copied verbatim into the target slot at slot_base, skipping bad blocks
//   rel 0x30 = header's in-slot offset = ROUND_UP(body_len, erasesize)
//   kernel  at body offset (H_KERNEL_FILEOFS - 0x214)
//   rootfs  at body offset (H_FS_FILEOFS   - 0x214)
import fs from 'node:fs';

const BLOCK = 0x20000;
const files = process.argv.slice(2);
for (const f of files) {
  const b = fs.readFileSync(f);
  const hdr = b.subarray(0x14, 0x214);
  const bodyLen = b.length - 0x214;
  const rd = (o) => hdr.readUInt32LE(o);
  const fwSize = rd(0x30), hdrPos = rd(0x30);
  const kSize = rd(0x34), kOff = rd(0x38), rSize = rd(0x40), rOff = rd(0x44);
  const slotA = rd(0x4c), spanA = rd(0x50), slotB = rd(0x5c), spanB = rd(0x60);
  const roundUp = (x) => Math.ceil(x / BLOCK) * BLOCK;
  console.log(`\n=== ${f.split('/').pop()} === size=${b.length} (0x${b.length.toString(16)})`);
  console.log(`  body (from 0x214) = ${bodyLen} = 0x${bodyLen.toString(16)}`);
  console.log(`  rel+0x30 = 0x${fwSize.toString(16)}   ROUND_UP(body,0x20000) = 0x${roundUp(bodyLen).toString(16)}  ${fwSize === roundUp(bodyLen) ? 'MATCH (this field is the header in-slot offset)' : fwSize === bodyLen ? '== body len exactly' : 'NO MATCH'}`);
  console.log(`  kernel : file 0x${kOff.toString(16)} size 0x${kSize.toString(16)} -> body ofs 0x${(kOff - 0x214).toString(16)}  slot ofs 0x${(kOff - 0x214).toString(16)}`);
  console.log(`  rootfs : file 0x${rOff.toString(16)} size 0x${rSize.toString(16)} -> body/slot ofs 0x${(rOff - 0x214).toString(16)}`);
  console.log(`  kernel ends at slot ofs 0x${(kOff - 0x214 + kSize).toString(16)}; gap before rootfs = 0x${(rOff - kOff - kSize).toString(16)} bytes`);
  console.log(`  slotA=0x${slotA.toString(16)} span=0x${spanA.toString(16)}  slotB=0x${slotB.toString(16)} span=0x${spanB.toString(16)}`);
  console.log(`  body+slotA end = 0x${(slotA + bodyLen).toString(16)} vs slotA+span = 0x${(slotA + spanA).toString(16)} -> fits: ${slotA + roundUp(bodyLen) + BLOCK <= slotA + spanA}`);
  console.log(`  header strings: ver="${hdr.subarray(0x10, 0x30).toString('latin1').replace(/\0+$/, '')}" prod="${hdr.subarray(0x6c, 0xa0).toString('latin1').replace(/\0+$/, '')}" build="${hdr.subarray(0xa8, 0xb8).toString('latin1').replace(/\0+$/, '')}" hw="${hdr.subarray(0x110, 0x130).toString('latin1').replace(/\0+$/, '')}"`);
  console.log(`  keys: +0x08=0x${rd(0x08).toString(16)} +0x0c=0x${rd(0x0c).toString(16)}  +0x1f0=0x${rd(0x1f0).toString(16)} serial+0x1f4=${rd(0x1f4)} new+0x1f8=${rd(0x1f8)}`);
  const gap = b.subarray(kOff + kSize, rOff);
  console.log(`  inter-extent gap ${gap.length}B: first16=${gap.subarray(0, 16).toString('hex')}`);
  console.log(`    gap == package[0x00:0x14]+verhdr? ${Buffer.compare(gap.subarray(0, 0x214), Buffer.concat([b.subarray(0, 0x14), hdr])) === 0}`);
  console.log(`    gap[0x14:0x214] == verhdr? ${Buffer.compare(gap.subarray(0x14, 0x214), hdr) === 0}`);
  console.log(`    gap unique bytes: ${[...new Set(gap.toString('hex').match(/../g))].length} distinct; allFF=${gap.every(x => x === 0xff)}, allZero=${gap.every(x => x === 0)}`);
}
