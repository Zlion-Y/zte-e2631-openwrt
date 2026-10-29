// ZTE router config.bin container (AES-256-CBC + zlib chunk chain).
// Reimplemented from the user's own Go tool `zte-config-decryption` (main.go) so it
// runs in this workspace with no Go toolchain. Round-trip is byte-faithful:
// outer header tail and inner header tail are preserved verbatim.
//
//   outer : [0x3c header][4B innerLen][4B cipherLen][4B 0][ciphertext(zero-padded to 16)]
//   inner : [0x3c header][per chunk: 4B inflated, 4B packed, 4B nextAbsOffset | zlib stream]
//   inner header: 0=magic 4=type(0) 8=xmlLen 12=LAST-chunk-offset 16=chunkSize
//                 20=crc32(all packed bytes) 24=crc32(header[0:24]) 28..=tail
//   key = sha256(keySeed)[:32]   iv = sha256(ivSeed)[:16]
//
// CLI:
//   node zteconfig.mjs decrypt <config.bin> <keySeed> <ivSeed>       -> prints tables it finds
//   node zteconfig.mjs dump    <config.bin> <keySeed> <ivSeed> <out.xml>
//   node zteconfig.mjs pack    <in.xml> <orig.bin> <keySeed> <ivSeed> <out.bin>
import fs from 'node:fs';
import crypto from 'node:crypto';
import zlib from 'node:zlib';

const MAGIC = 0x01020304, HDR = 0x3c, INNER_TYPE = 0, DEFAULT_CHUNK = 0x10000;
const sha256 = (b) => crypto.createHash('sha256').update(b).digest();
const crc32 = (b) => zlib.crc32(b) >>> 0;
const be32 = (b, o) => b.readUInt32BE(o);
const die = (m) => { console.error('zteconfig: ' + m); process.exit(1); };

function derive(keySeed, ivSeed, type) {
  const ks = sha256(Buffer.from(keySeed, 'utf8'));
  if (type === 2) return { key: ks.subarray(0, 16), iv: null, ecb: true };
  if (type === 3) return { key: ks, iv: ks.subarray(0, 16), ecb: false };
  if (type === 4) return { key: ks, iv: sha256(Buffer.from((ivSeed || keySeed), 'utf8')).subarray(0, 16), ecb: false };
  die(`unsupported payload type ${type}`);
}
function aes(payload, { key, iv, ecb }, enc) {
  const pad = (16 - (payload.length % 16)) % 16;
  const data = pad ? Buffer.concat([payload, Buffer.alloc(pad)]) : Buffer.from(payload);
  const out = Buffer.alloc(data.length);
  if (ecb) {
    const c = crypto.createCipheriv(`aes-${key.length * 8}-ecb`, key, null); c.setAutoPadding(false);
    return Buffer.concat([c.update(data), c.final()]);
  }
  const op = enc ? crypto.createCipheriv(`aes-${key.length * 8}-cbc`, key, iv) : crypto.createDecipheriv(`aes-${key.length * 8}-cbc`, key, iv);
  op.setAutoPadding(false);
  return Buffer.concat([op.update(data), op.final()]);
}
function parseChunks(buf, start) {
  const out = []; let off = start;
  while (off + 12 <= buf.length) {
    const inflated = be32(buf, off), packed = be32(buf, off + 4), next = be32(buf, off + 8);
    const pStart = off + 12, pEnd = pStart + packed;
    if (!packed || pEnd > buf.length) die(`bad chunk at 0x${off.toString(16)}`);
    out.push({ off, inflated, packed, next, payload: buf.subarray(pStart, pEnd) });
    off = pEnd;
    if (!next) break;
  }
  return out;
}

export function decryptConfig(input, keySeed, ivSeed) {
  if (input.length < HDR) die('file too small');
  if (be32(input, 0) !== MAGIC) die('bad outer magic');
  const type = be32(input, 4);
  const k = derive(keySeed, ivSeed, type);
  const outer = parseChunks(input, HDR);
  // outer "inflated" field carries the plaintext inner length; CBC output is zero-padded
  const inner = Buffer.concat(outer.map((c) => {
    const d = aes(c.payload, k, false);
    return c.inflated ? d.subarray(0, c.inflated) : d;
  }));
  if (be32(inner, 0) !== MAGIC) die('inner magic mismatch (wrong key?)');
  if (be32(inner, 4) !== INNER_TYPE) die(`inner type ${be32(inner, 4)} != 0`);
  if (be32(inner, 24) !== crc32(inner.subarray(0, 24))) die('inner header CRC32 mismatch');
  const chunks = parseChunks(inner, HDR);
  const packedAll = Buffer.concat(chunks.map((c) => c.payload));
  if (be32(inner, 20) !== crc32(packedAll)) die('compressed data CRC32 mismatch');
  const xml = Buffer.concat(chunks.map((c) => {
    const inf = zlib.inflateSync(c.payload);
    if (inf.length !== c.inflated) die('chunk inflated size mismatch');
    return inf;
  }));
  if (be32(inner, 8) !== xml.length) die(`XML size ${xml.length} != header ${be32(inner, 8)}`);
  if (!/<XTM-Acl|X_ZTE-CFG|<Tbl\s+name=/.test(xml.toString('utf8').slice(0, 4000))) die('does not look like a ZTE config XML');
  return { xml, outerType: type, outerTail: Buffer.from(input.subarray(8, HDR)), innerTail: Buffer.from(inner.subarray(28, HDR)), chunkSize: be32(inner, 16) || DEFAULT_CHUNK, outerChunks: outer.length, innerChunks: chunks.length };
}

export function buildConfig(xml, info, keySeed, ivSeed) {
  const cs = info.chunkSize > 0 && info.chunkSize <= (1 << 20) ? info.chunkSize : DEFAULT_CHUNK;
  const pieces = [];
  for (let i = 0; i < xml.length; i += cs) {
    const part = xml.subarray(i, Math.min(i + cs, xml.length));
    pieces.push({ src: part.length, packed: zlib.deflateSync(part, { level: 9 }) });
  }
  const offs = []; let pos = HDR;
  for (const p of pieces) { offs.push(pos); pos += 12 + p.packed.length; }
  const body = Buffer.concat(pieces.map((p, i) => {
    const h = Buffer.alloc(12);
    h.writeUInt32BE(p.src, 0); h.writeUInt32BE(p.packed.length, 4);
    h.writeUInt32BE(i + 1 < pieces.length ? offs[i + 1] : 0, 8);
    return Buffer.concat([h, p.packed]);
  }));
  const inner = Buffer.alloc(HDR);
  inner.writeUInt32BE(MAGIC, 0); inner.writeUInt32BE(INNER_TYPE, 4);
  inner.writeUInt32BE(xml.length, 8); inner.writeUInt32BE(offs[offs.length - 1], 12);
  inner.writeUInt32BE(cs, 16);
  inner.writeUInt32BE(crc32(Buffer.concat(pieces.map((p) => p.packed))), 20);
  inner.writeUInt32BE(crc32(inner.subarray(0, 24)), 24);
  (info.innerTail || Buffer.alloc(32)).copy(inner, 28);
  const innerFull = Buffer.concat([inner, body]);
  const k = derive(keySeed, ivSeed, info.outerType);
  const ct = aes(innerFull, k, true);
  const head = Buffer.alloc(HDR);
  head.writeUInt32BE(MAGIC, 0); head.writeUInt32BE(info.outerType, 4);
  (info.outerTail || Buffer.alloc(HDR - 8)).copy(head, 8);
  const ch = Buffer.alloc(12);
  ch.writeUInt32BE(innerFull.length, 0); ch.writeUInt32BE(ct.length, 4); ch.writeUInt32BE(0, 8);
  return Buffer.concat([head, ch, ct]);
}

if (process.argv[1]?.includes('zteconfig.mjs')) {
  const [cmd, ...rest] = process.argv.slice(2);
  const opt = {}; for (let i = 0; i < rest.length; i++) if (rest[i].startsWith('--')) opt[rest[i].slice(2)] = rest[++i];
  const { file, key, iv, out, bin } = opt;
  if (cmd === 'dump') {
    const info = decryptConfig(fs.readFileSync(file), key, iv);
    fs.writeFileSync(out, info.xml);
    console.log(`outer type ${info.outerType}  chunks outer=${info.outerChunks} inner=${info.innerChunks}  chunkSize=0x${info.chunkSize.toString(16)}  xml=${info.xml.length} B -> ${out}`);
  } else if (cmd === 'pack') {
    const xml = fs.readFileSync(file);
    const layout = decryptConfig(fs.readFileSync(bin), key, iv);   // reuse original layout params
    const blob = buildConfig(xml, layout, key, iv);
    fs.writeFileSync(out, blob);
    const back = decryptConfig(fs.readFileSync(out), key, iv);
    if (!back.xml.equals(xml)) die('round-trip FAILED');
    if (blob.length !== fs.statSync(out).size) die('size mismatch after write');
    console.log(`round-trip OK  xml=${xml.length} B -> ${out} (${blob.length} B)`);
  } else die('usage: node zteconfig.mjs dump|pack --file <f> --key <keySeed> --iv <ivSeed> [--bin <orig.bin>] [--out <path>]');
}
