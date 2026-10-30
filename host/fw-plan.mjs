#!/usr/bin/env node
// Parse MT7916 vendor firmware images the way mt7916.ko's parse_fw_v3 /
// parse_patch_v1 do. Offline only: prints the download plan the MCU would
// receive (AddressLen + FwScatter + FwStart), no device access.
//
//   node fw-plan.mjs <file> [--patch] [--wa]
//
// Firmware format 2 (WIFI_RAM_CODE_MT7916.bin, 7916_WACPU_...bin):
//   [region0 payload][region1 payload]...[padding]
//   [40-byte region table x n_region][36-byte trailer]
//   region: decomp_crc, decomp_len, decomp_blk_sz, rsv[4], addr, len,
//           feature_set, type, rsv1[14]
//   trailer: chip_id, eco, n_region, format_ver, format_flag, rsv[2],
//            fw_ver[10], build_date[15], crc
// Patch format (mt7916_patch_e1_hdr.bin):
//   30-byte header (build_date[16], platform[4], hw_sw_ver, patch_ver,
//   vendor) followed by the image that is downloaded to 0x04000000.
import fs from 'node:fs';

const args = process.argv.slice(2);
const file = args[0];
const isPatch = args.includes('--patch');
const isWa = args.includes('--wa');
if (!file) {
  console.error('usage: fw-plan.mjs <file> [--patch] [--wa]');
  process.exit(2);
}
const b = fs.readFileSync(file);
const u32 = (o) => b.readUInt32LE(o);
const hex = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');

if (isPatch) {
  if (b.length < 30) throw new Error('too short');
  const build = b.toString('latin1', 0, 16).replace(/\0+$/, '');
  const platform = b.toString('latin1', 16, 20);
  const hwsw = b.readUInt32BE(20).toString(16);
  const len = b.length - 30;
  console.log(`patch ${file}`);
  console.log(`  build_date "${build}" platform "${platform}" hw_sw_ver 0x${hwsw}`);
  console.log(`  AddressLen addr=0x04000000 len=${len} mode=0x80000000 type=5`);
  console.log(`  FwScatter off=30 len=${len}`);
  console.log(`  PatchFinish`);
  process.exit(0);
}

if (b.length < 36) throw new Error('too short for trailer');
const t = b.length - 36;
const chip = b[t], eco = b[t + 1], n = b[t + 2], fmt = b[t + 3], flag = b[t + 4];
const ver = b.toString('latin1', t + 7, t + 17).replace(/\0+$/, '');
const date = b.toString('latin1', t + 17, t + 32).replace(/\0+$/, '');
const crc = u32(t + 32);
console.log(`${file}`);
console.log(`  size ${b.length} chip 0x${chip.toString(16)} eco ${eco} n_region ${n} fmt ${fmt} flag ${flag}`);
console.log(`  fw_ver "${ver}" build "${date}" crc ${hex(crc)}`);
if (fmt !== 2) throw new Error(`unexpected format_ver ${fmt}`);
if (b.length < 36 + n * 40) throw new Error('region table does not fit');
const tab = t - n * 40;
let off = 0, override = 0, opt = 0;
const lines = [];
for (let i = 0; i < n; i++) {
  const e = tab + i * 40;
  const addr = u32(e + 16), len = u32(e + 20), fs = b[e + 24];
  let mode = 0x80000000 | (fs & 0x7);
  if (fs & 0x01) mode |= 0x08;
  if (fs & 0x10) mode |= 0x40;
  if (isWa) mode |= 0x10;
  const skip = !!(fs & 0x40);
  if (fs & 0x20) override = addr;
  if (off + len > b.length) throw new Error(`region ${i} beyond file`);
  console.log(`  [${i}] addr ${hex(addr)} len ${String(len).padStart(7)} off ${String(off).padStart(7)} feature 0x${fs.toString(16).padStart(2, '0')}${skip ? ' (skip/non-dl)' : ''} mode ${hex(mode)}`);
  if (!skip) lines.push(`${addr.toString(16)} ${len.toString(16)} ${off.toString(16)} ${mode.toString(16)}`);
  off += len;
}
if (override) opt |= 0x1;
if (isWa) opt |= 0x4;
console.log(`  payload span ${off}, table at ${tab}, gap ${tab - off}`);
console.log(`  FwStart option ${hex(opt)} override ${hex(override)}`);
console.log(`  manifest: ${lines.join(',')}`);
