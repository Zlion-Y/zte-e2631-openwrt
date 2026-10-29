// Timed blind telnet session: sends lines on a schedule, records ALL received bytes.
// Always releases the device-side session with "exit" before closing; the router
// counts half-open shells against TelnetCfg.Max_Con_Num and otherwise wedges.
// Usage: node telnet_sh.mjs <host> <port> <planfile> <outfile> [--stop-on <regex>]
// planfile lines: "<delay_ms>|<text>"   (newline appended unless the line has a CR)
// --stop-on ends the session as soon as the device output matches, so a job that
// finished in 10 s does not have to wait out the whole polling schedule.
import net from 'node:net';
import fs from 'node:fs';

const argv = process.argv.slice(2);
let stopOn = '';
{
  const i = argv.indexOf('--stop-on');
  if (i >= 0) { stopOn = argv[i + 1]; argv.splice(i, 2); }
}
const [host, port, planPath, outPath] = argv;
const stopRe = stopOn ? new RegExp(stopOn) : null;
const plan = fs.readFileSync(planPath, 'utf8').split(/\r?\n/).filter(l => l.trim())
  // The vendor telnet password is never committed. Plans carry ${ZTE_TELNET_PW} and the
  // value is taken from the environment at run time; a plan that needs it and finds no
  // value stops here rather than typing an empty line at the device.
  .map((l) => {
    if (!l.includes('\${ZTE_TELNET_PW}')) return l;
    if (!process.env.ZTE_TELNET_PW) {
      console.error('plan needs ${ZTE_TELNET_PW}: export ZTE_TELNET_PW first');
      process.exit(2);
    }
    return l.split('\${ZTE_TELNET_PW}').join(process.env.ZTE_TELNET_PW);
  })
  .map(l => { const i = l.indexOf('|'); return i < 0 ? { d: parseInt(l, 10), s: '' } : { d: parseInt(l.slice(0, i), 10), s: /\r/.test(l.slice(i + 1)) ? l.slice(i + 1) : l.slice(i + 1) + '\r\n' }; });
let last = 0;
for (const p of plan) { last = p.d; }
plan.push({ d: last + 1500, s: 'exit\r\n' });

const raw = [];      // all received bytes, un-decoded
let clean = Buffer.alloc(0);
const wf = fs.createWriteStream(outPath);
// A reboot kills the socket mid-schedule, so the close/error handlers can end the
// file while a scheduled send is still pending; writing then throws and the
// captured transcript is lost.
let finished = false;
const log = (s) => { if (!finished) wf.write(s); };

// Telnet IAC state machine across chunk boundaries.
let pending = Buffer.alloc(0);
function decode(chunk) {
  raw.push(chunk);
  const b = Buffer.concat([pending, chunk]);
  pending = Buffer.alloc(0);
  const out = [];
  for (let i = 0; i < b.length; i++) {
    if (b[i] !== 255) { out.push(b[i]); continue; }
    if (i + 1 >= b.length) { pending = b.subarray(i); break; }
    const cmd = b[i + 1];
    if (cmd === 255) { out.push(255); i++; continue; }
    if (cmd >= 250 && cmd <= 254) { // WILL/WONT/DO/DONT (+opt) and SB
      if (cmd === 250) { while (i + 1 < b.length && !(b[i] === 255 && b[i + 1] === 240)) i++; i++; continue; }
      if (i + 2 >= b.length) { pending = b.subarray(i); break; }
      const opt = b[i + 2];
      // refuse everything: reply DONT to DO, WONT to WILL
      if (cmd === 253) schedReply(Buffer.from([255, 254, opt]));
      if (cmd === 251) schedReply(Buffer.from([255, 252, opt]));
      i += 2; continue;
    }
    i += 1; // unknown 2-byte command
  }
  return Buffer.from(out);
}
const replyQ = [];
function schedReply(b) { replyQ.push(b); if (sock && sock.writable) { sock.write(b); replyQ.shift(); } }

const sock = net.connect(parseInt(port, 10), host);
sock.setTimeout(Math.max(20000, last + 15000));
sock.on('connect', () => {
  log(`### connected ${host}:${port}\n`);
  for (const p of plan) {
    setTimeout(() => { if (p.s) { sock.write(p.s); log(`\n=== [send @${p.d}ms] ${JSON.stringify(p.s)}\n`); } }, p.d);
  }
});
sock.on('data', (d) => {
  clean = Buffer.concat([clean, decode(d)]);
  flush();
  if (stopRe && !ended && stopRe.test(clean.toString('utf8'))) {
    log('\n### --stop-on matched, ending early\n');
    end();
  }
});
sock.on('timeout', () => { log('\n### TIMEOUT\n'); end(); });
sock.on('error', (e) => { log(`\n### ERROR ${e.code}\n`); end(); });
sock.on('close', () => { log('\n### CLOSED\n'); end(); });

let flushed = 0;
function flush() {
  log(clean.subarray(flushed).toString('utf8'));
  flushed = clean.length;
}
let ended = false;
function end() {
  if (ended) return;
  ended = true;
  try { if (sock.writable) sock.write('exit\r\n'); } catch {}
  flush();
  finished = true;
  wf.end();
  setTimeout(() => { try { sock.destroy(); } catch {} }, 400);
}
