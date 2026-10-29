// Minimal Telnet client with option negotiation + expect-style runner.
// Usage: node telnet_run.js <host> <port> <scriptfile> [outfile]
// scriptfile lines:  "EXPECT <regex>"  /  "SEND <literal>"  /  "WAIT <ms>"  /  "#" comment
import net from 'node:net';
import fs from 'node:fs';

const [host, portStr, scriptPath, outPath] = process.argv.slice(2);
const port = parseInt(portStr || '23', 10);
const out = outPath ? fs.createWriteStream(outPath) : process.stdout;
const IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240;

const steps = [];
for (const raw of fs.readFileSync(scriptPath, 'utf8').split(/\r?\n/)) {
  const line = raw.replace(/\r$/, '');
  if (!line.trim() || line.trim().startsWith('#')) continue;
  const m = /^(EXPECT|SEND|WAIT|SENDLN)\s([\s\S]*)$/.exec(line);
  if (!m) { console.error('bad script line: ' + raw); process.exit(2); }
  steps.push({ op: m[1], arg: m[2] });
}

let buf = Buffer.alloc(0);
let log = '';
const write = (s) => { log += s; out.write(s); };

function stripIac(b) {
  const o = [];
  for (let i = 0; i < b.length; i++) {
    if (b[i] === IAC && i + 2 < b.length) {
      const cmd = b[i + 1];
      if (cmd === WILL || cmd === WONT || cmd === DO || cmd === DONT) { i += 2; continue; }
      if (cmd === SB) { while (i + 1 < b.length && !(b[i] === IAC && b[i + 1] === SE)) i++; i++; continue; }
      if (cmd === IAC) { o.push(IAC); i++; continue; }
    }
    o.push(b[i]);
  }
  return Buffer.from(o);
}

function replyNeg(b) {
  const r = [];
  for (let i = 0; i < b.length - 2; i++) {
    if (b[i] === IAC && (b[i + 1] === DO || b[i + 1] === WILL)) r.push(IAC, b[i + 1] === DO ? DONT : WONT, b[i + 2]);
  }
  return r.length ? Buffer.from(r) : null;
}

const sock = net.connect(port, host);
sock.setTimeout(30000);
let done = false;

sock.on('connect', () => write(`[connected ${host}:${port}]\n`));
sock.on('data', (d) => {
  const neg = replyNeg(d);
  if (neg) sock.write(neg);
  buf = Buffer.concat([buf, stripIac(d)]);
  pump();
});
sock.on('timeout', () => { write('\n[TIMEOUT]\n'); finish(3); });
sock.on('error', (e) => { write(`\n[ERROR ${e.code}]\n`); finish(4); });
sock.on('close', () => { write('\n[socket closed]\n'); finish(0); });

function finish(code) {
  if (done) return;
  done = true;
  try { sock.destroy(); } catch {}
  if (outPath) out.end();
  process.exitCode = code;
}

function pump() {
  for (;;) {
    if (!steps.length) { finish(0); return; }
    const st = steps[0];
    if (st.op === 'WAIT') {
      const ms = parseInt(st.arg, 10) || 1000;
      steps.shift();
      setTimeout(pump, ms);
      return;
    }
    if (st.op === 'SEND' || st.op === 'SENDLN') {
      sock.write(st.op === 'SENDLN' ? st.arg + '\r\n' : st.arg);
      write(`> ${st.arg.replace(/\r?\n/g, '\\n')}\n`);
      steps.shift();
      continue;
    }
    let pat = st.arg, flags = 'gs';
    if (pat.startsWith('(?i)')) { pat = pat.slice(4); flags = 'gis'; }
    const re = new RegExp(pat, flags);
    const mm = re.exec(buf.toString('latin1'));
    if (mm) {
      const consumed = buf.subarray(0, Buffer.byteLength(mm[0], 'latin1'));
      write(consumed.toString('utf8'));
      buf = buf.subarray(consumed.length);
      steps.shift();
      continue;
    }
    if (buf.length > 262144) { write(buf.toString('utf8')); buf = Buffer.alloc(0); }
    return;
  }
}
