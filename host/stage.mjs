// Deliver the built images to the router and inspect them there, without
// writing flash. Starts the file server, drives one telnet session, and prints
// what the device reports about both slots and about the package.
//
//   node build/host/stage.mjs put   [--fw out/e2631-fwslot] [--pkg out/e2631-openwrt-factory.bin]
//   node build/host/stage.mjs flash --yes ...same options...
//
// "put" only reads: it copies the two files into /var on the device and runs
// "e2631-fwslot info" and "check <package>". "flash" additionally runs
// "e2631-fwslot flash <package>", which writes the *inactive* slot; it needs
// --yes because that is the one command here that changes the device.
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';

const HERE = path.dirname(fileURLToPath(import.meta.url));

const argv = process.argv.slice(2);
const mode = argv[0];
const arg = (n, d) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : d; };
const HOST = arg('--host', '192.168.5.3');
const PC = arg('--pc', '192.168.5.5');
const PORT = parseInt(arg('--port', '8000'), 10);
const FW = arg('--fw', 'out/e2631-fwslot');
const PKG = arg('--pkg', 'out/e2631-openwrt-factory.bin');
// Passed straight to telnet_sh.mjs: without it a session always runs out its
// fixed schedule, so a flash that finished in 10 s still cost 9 minutes.
const STOP = arg('--stop-on', '');
// Extra arguments appended to the device-side flash command, e.g. --slot 1.
const FARGS = arg('--flash-args', '');
const OUT = `out/stage_${mode}.out.txt`;

if (!['put', 'flash'].includes(mode)) { console.error('mode must be put or flash'); process.exit(2); }
if (mode === 'flash' && !argv.includes('--yes')) {
  console.error('flash writes the inactive slot; re-run with --yes when that is what you want');
  process.exit(2);
}
for (const f of [FW, PKG]) if (!fs.existsSync(f)) { console.error(`missing ${f}`); process.exit(2); }
// The vendor telnet password is not in this repository; set it in the environment.
if (!process.env.ZTE_TELNET_PW) { console.error('export ZTE_TELNET_PW first - the device telnet login needs it'); process.exit(2); }

const files = { 'e2631-fwslot': path.resolve(FW), 'fw.bin': path.resolve(PKG) };
const logFile = 'out/stage-server.log';
const srvLog = (s) => fs.appendFileSync(logFile, `${new Date().toISOString()} ${s}\n`);
const server = http.createServer((req, res) => {
  const rel = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '');
  const file = files[rel];
  srvLog(`${req.method} ${req.url} from ${req.socket.remoteAddress} -> ${file ? path.basename(file) : 'unknown'}`);
  if (!file) { res.writeHead(404).end('no\n'); return; }
  const size = fs.statSync(file).size;
  res.writeHead(200, { 'content-length': size, 'content-type': 'application/octet-stream' });
  const t0 = Date.now();
  const s = fs.createReadStream(file);
  s.pipe(res);
  s.on('end', () => srvLog(`sent ${path.basename(file)} ${size} B in ${Date.now() - t0} ms`));
  res.on('close', () => srvLog(`closed ${path.basename(file)} after ${Date.now() - t0} ms`));
});
// A leftover node process from an earlier session keeps 8000 bound for a while
// after it is killed, and a single EADDRINUSE here aborted a whole flash run
// three times. Walk forward to the next free port instead; the device fetches by
// URL, so the port number travels with the plan.
let usePort = 0;
for (let p = PORT; p < PORT + 16 && !usePort; p++) {
  try {
    await new Promise((res, rej) => {
      const onErr = (e) => { server.removeListener('listening', onOk); rej(e); };
      const onOk = () => { server.removeListener('error', onErr); res(); };
      server.once('error', onErr);
      server.once('listening', onOk);
      server.listen(p, '0.0.0.0');
    });
    usePort = p;
  } catch (e) {
    srvLog(`port ${p}: ${e.message}`);
  }
}
if (!usePort) { console.error(`no free port in ${PORT}..${PORT + 15}`); process.exit(1); }
const P = String(usePort);
console.log(`serving e2631-fwslot (${fs.statSync(files['e2631-fwslot']).size} B) and fw.bin ` +
  `(${fs.statSync(files['fw.bin']).size} B) on http://${PC}:${usePort}/`);

const plan = [
  '1000|root',
  '3000|' + (process.env.ZTE_TELNET_PW || ''),
  '9000|',
  '12000|echo S1; rm -f /var/tmp/e2631-fwslot /var/tmp/fw.bin /var/tmp/flash.log; df -k /var /tmp; ' +
  'mount | grep -E " /var| /tmp"; which setsid nohup curl; cat /proc/mtd; ls -l /dev/mtd* 2>/dev/null',
  '18000|echo S2; curl -f --connect-timeout 5 --max-time 240 -o /var/tmp/e2631-fwslot http://' + PC + ':' + P + '/e2631-fwslot; echo rc=$?; chmod +x /var/tmp/e2631-fwslot; ls -l /var/tmp/e2631-fwslot',
  // The transfers are not the slow part: the device pulled the 20 MB package at
  // 16 MB/s over the LAN (one second). The old timetable waited until 180 s for
  // the next step anyway, which is where the "flashing takes ten minutes" came
  // from. Only the flash itself needs slack, and that is polled below.
  '30000|echo S3; curl -f --connect-timeout 5 --max-time 240 -o /var/tmp/fw.bin http://' + PC + ':' + P + '/fw.bin; echo rc=$?; ls -l /var/tmp/fw.bin',
  '46000|echo S4; /var/tmp/e2631-fwslot info',
  '58000|echo S5; /var/tmp/e2631-fwslot check /var/tmp/fw.bin',
];
if (mode === 'flash') {
  // Detached on purpose: erasing 28 MiB and writing the payload takes minutes,
  // and if the shell killed its child because the telnet socket closed, the
  // flash would stop half way through. Start it in the background and poll a log.
  plan.push('72000|echo S6; rm -f /var/tmp/flash.log;' +
    ' (setsid /var/tmp/e2631-fwslot flash /var/tmp/fw.bin ' + FARGS + ' > /var/tmp/flash.log 2>&1 &); echo started');
  for (let t = 84000, i = 0; t <= 324000; t += 12000, i++)
    plan.push(`${t}|echo S6.${i}; cat /var/tmp/flash.log`);
} else plan.push('72000|echo S6; echo "no write performed"');

// This telnetd drops a session that goes quiet for about a minute (measured:
// the next send after ~110 s idle never arrived), and a dropped session during a
// flash is the one thing that must not happen - keep talking to it.
const last = parseInt(plan[plan.length - 1], 10);
for (let t = 24000; t < last; t += 12000) plan.push(`${t}|:`);
plan.sort((a, b) => parseInt(a, 10) - parseInt(b, 10));
fs.writeFileSync(path.join('out','stage.plan'), plan.join('\n') + '\n');

for (const [n, p] of Object.entries(files))
  console.log(`  ${n.padEnd(14)} ${fs.statSync(p).size} B  sha256 ${crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex').slice(0, 16)}…`);

// telnet_sh.mjs must run *async*: the file server lives in this same process,
// and a synchronous child would freeze it, so the device curl would hang.
const code = await new Promise((resolve) => {
  const stop = STOP || (mode === 'flash'
    ? 'ok: slot \\d+ now holds|does not read back|refusing to|ERROR|failed to build' : '');
  const tail = stop ? ['--stop-on', stop] : [];
  const c = spawn(process.execPath, [path.join(HERE, 'telnet_sh.mjs'), HOST, '23', path.join('out', 'stage.plan'), OUT, ...tail], { stdio: 'inherit' });
  c.on('close', resolve);
});
server.close();
srvLog(`session exited ${code}`);
const tail = fs.readFileSync(OUT, 'utf8').split('echo S1')[1] || fs.readFileSync(OUT, 'utf8');
console.log(`\n--- device said ---\n${tail.slice(0, 4000)}`);
