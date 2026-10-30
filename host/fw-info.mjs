import fs from 'node:fs';

function u32le(b, o) { return b.readUInt32LE(o); }
function hex(v, n = 8) { return '0x' + v.toString(16).padStart(n, '0'); }

for (const f of process.argv.slice(2)) {
  const b = fs.readFileSync(f);
  console.log(`=== ${f} (${b.length} bytes)`);
  if (b.length >= 0x40 && u32le(b, 0x20) === 0x11223344) {
    console.log(`  patch: date=${b.subarray(0, 16).toString('latin1').trim()}` +
      ` platform=${b.subarray(0x10, 0x14).toString('latin1')}` +
      ` magic=11223344 dst=${hex(u32le(b, 0x24))}` +
      ` field28=${hex(u32le(b, 0x28))}` +
      ` field2c=${hex(u32le(b, 0x2c))}` +
      ` payload_off=0x40 payload_len=${b.length - 0x40}`);
    continue;
  }
  if (b.length >= 36) {
    const t = b.subarray(b.length - 36);
    console.log(`  trailer: chip_id=${hex(t[0], 2)} eco=${hex(t[1], 2)}` +
      ` n_region=${t[2]} format=${t[3]} flag=${t[4]}` +
      ` fw_ver=${t.subarray(7, 17).toString('latin1')}` +
      ` build=${t.subarray(17, 32).toString('latin1')}` +
      ` crc=${hex(t.readUInt32LE(32))}`);
  } else {
    console.log('  no patch header and no trailer');
  }
}
