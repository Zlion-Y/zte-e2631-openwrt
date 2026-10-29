// Offline device-tree cross-reference check: no dtc on this box, and a missing
// label costs a 20 minute CI cycle. Resolves #include lines against the kernel
// dts files extracted from the target's patch series, then diffs the labels the
// board file references against the labels that are actually defined.
//
//   node build/tools/dts_lint.mjs <board.dts> <patches-dir> [...more dirs]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const [board, ...dirs] = process.argv.slice(2);
if (!board || !dirs.length) { console.error('usage: dts_lint.mjs <board.dts> <patches dir>'); process.exit(2); }

const readAdded = (file) => {
  // Pull the added lines out of a .patch so we can see files the PR creates.
  const out = [];
  let cur = null, keep = false;
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const h = /^(?:--- a\/|\+\+\+ b\/)(.*)$/.exec(line);
    if (line.startsWith('diff --git ')) { cur = (line.split(' b/')[1] || '').trim(); keep = false; continue; }
    if (h && cur && h[1] === cur) { keep = true; continue; }
    if (keep && line.startsWith('+') && !line.startsWith('+++')) out.push(line.slice(1));
    if (keep && line.startsWith('-') && !line.startsWith('---')) { /* deletion: irrelevant here */ }
  }
  return cur ? { path: cur, text: out.join('\n') } : null;
};

// index every .dts/.dtsi we can see, from the filesystem and from patches
const byBase = new Map();
const add = (p, text) => {
  const b = path.basename(p);
  if (!byBase.has(b)) byBase.set(b, { path: p, text });
};
const walk = (d) => {
  if (!fs.existsSync(d)) return;
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (/\.(dts|dtsi)$/.test(e.name)) add(e.name, fs.readFileSync(p, 'utf8'));
    else if (/\.patch$/.test(e.name)) { const r = readAdded(p); if (r && /\.(dts|dtsi)$/.test(path.basename(r.path))) add(path.basename(r.path), r.text); }
  }
};
for (const d of dirs) walk(d);
if (board) add(path.basename(board), fs.readFileSync(board, 'utf8'));

const labelsDefined = new Map();   // label -> file
const collect = (name, text) => {
  for (const m of text.matchAll(/^\s*([A-Za-z_][A-Za-z0-9_]*):\s*[A-Za-z_][A-Za-z0-9_,@#-]*\s*\{/gm))
    if (!labelsDefined.has(m[1])) labelsDefined.set(m[1], name);
};
for (const [b, v] of byBase) collect(b, v.text);

const includesOf = (text) => [...text.matchAll(/#include\s+"([^"]+)"/g)].map((m) => m[1]);

// walk the include chain from the board file
const seen = new Set();
const queue = [[path.basename(board), fs.readFileSync(board, 'utf8')]];
const chain = [];
const missingIncludes = [];
while (queue.length) {
  const [from, text] = queue.shift();
  if (seen.has(from)) continue;
  seen.add(from);
  chain.push(from);
  for (const inc of includesOf(text)) {
    const base = path.basename(inc);
    const hit = byBase.get(base);
    if (!hit) { missingIncludes.push(`${from} -> ${inc}`); continue; }
    queue.push([base, hit.text]);
  }
}

const refs = new Set([...fs.readFileSync(board, 'utf8').matchAll(/&([A-Za-z_][A-Za-z0-9_]*)/g)].map((m) => m[1]));
const unresolved = [...refs].filter((r) => !labelsDefined.has(r));

console.log(`board        : ${path.basename(board)}`);
console.log(`include chain: ${chain.join(' -> ')}`);
console.log(`files indexed: ${byBase.size} (from ${dirs.join(', ')})`);
console.log(`labels seen  : ${labelsDefined.size}`);
console.log(`&refs used    : ${refs.size}`);
let bad = 0;
if (missingIncludes.length) { bad++; console.log('\nUNRESOLVED INCLUDES:'); missingIncludes.forEach((m) => console.log('  ' + m)); }
if (unresolved.length) { bad++; console.log('\nUNDEFINED LABELS:'); unresolved.forEach((r) => console.log(`  &${r}`)); }
for (const [b, v] of byBase) {
  const open = (v.text.match(/\{/g) || []).length, close = (v.text.match(/\}/g) || []).length;
  if (open !== close) { bad++; console.log(`\nUNBALANCED BRACES in ${b}: ${open} { vs ${close} }`); }
}
const dup = [];
for (const r of refs) if (labelsDefined.has(r)) { /* fine */ }
// a node overridden twice in the same board file is usually a copy/paste slip
const own = fs.readFileSync(board, 'utf8');
for (const m of own.matchAll(/^&([A-Za-z_][A-Za-z0-9_]*)\s*\{/gm)) {
  const n = [...own.matchAll(new RegExp('^&' + m[1] + '\\s*\\{', 'gm'))].length;
  if (n > 1 && !dup.includes(m[1])) dup.push(m[1]);
}
if (dup.length) console.log(`\nWARN: &node overridden more than once in the board file: ${dup.join(', ')}`);

// dt-binding macro names: a wrong one is a dtc "Lexical error: Unexpected
// 'LED_FUNCTION_INET'", and dtc only runs inside the 40 minute build, so the
// symbols are checked here against cached headers. Refresh the cache with
// curl against include/dt-bindings/{leds/common.h,gpio/gpio.h,input/linux-event-codes.h}.
{
  // scripts/env.sh exports E2631_WORK/E2631_OUT; the fallback is the same default
  // it uses (the kit's parent), so the cache is found from either root.
  const WORK = process.env.E2631_WORK
    || path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
  const dir = path.join(process.env.E2631_OUT || path.join(WORK, 'out'), 'hdrs');
  let cached = [];
  try { cached = fs.readdirSync(dir).filter((f) => f.endsWith('.h')); } catch { /* no cache */ }
  const used = [...new Set([...own.matchAll(/\b(?:LED_|GPIO_|KEY_|INPUT_)[A-Z0-9_]+\b/g)].map((m) => m[0]))];
  if (!cached.length) console.log(`\nNOTE: no dt-binding headers in ${dir}: macro names NOT checked`);
  else {
    const known = new Set();
    for (const f of cached)
      for (const m of fs.readFileSync(dir + '/' + f, 'utf8').matchAll(/^#define\s+([A-Z][A-Z_0-9]*)\b/gm)) known.add(m[1]);
    const unknown = used.filter((s) => !known.has(s));
    console.log(`\ndt-binding macros used (${used.length}): ${used.join(' ')}`);
    if (unknown.length) { bad++; console.log('UNKNOWN MACROS: ' + unknown.join(', ') + '  (dtc would stop here)'); }
  }
}

console.log(bad ? `\nRESULT: ${bad} problem group(s)` : '\nRESULT: labels, includes and braces all resolve');
process.exit(bad ? 1 : 0);
