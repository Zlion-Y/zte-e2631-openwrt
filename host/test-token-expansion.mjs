// End-to-end check for the redaction design: a plan that carries ${ZTE_TELNET_PW} must
// send the real value when the variable is set, and nothing literal must leak.
import net from 'node:net';
import { spawn } from 'node:child_process';
import fs from 'node:fs';

const PORT = 2323;
const SECRET = 'unit-test-secret';
const chunks = [];

const server = net.createServer((sock) => {
  sock.on('data', (d) => chunks.push(d.toString('latin1')));
  sock.write('login: ');
  setTimeout(() => sock.write('Password: '), 300);
  setTimeout(() => sock.write('# '), 900);
});
await new Promise((r) => server.listen(PORT, '127.0.0.1', r));

fs.writeFileSync('out/tok-e2e.plan', `1000|root\n2500|\${ZTE_TELNET_PW}\n5000|echo ok\n`);

const child = spawn(process.execPath, ['build/host/telnet_sh.mjs', '127.0.0.1', String(PORT), 'out/tok-e2e.plan', 'out/tok-e2e.log'], {
  env: { ...process.env, ZTE_TELNET_PW: SECRET }, stdio: ['ignore', 'pipe', 'pipe'],
});
let out = '';
child.stdout.on('data', (d) => { out += d; });
child.stderr.on('data', (d) => { out += d; });
await new Promise((r) => child.on('close', r));
server.close();

const wire = chunks.join('');
console.log('bytes seen on the wire:', JSON.stringify(wire));
console.log('child said:', out.split('\n').filter(Boolean).slice(0, 3).join(' | '));

const ok1 = wire.includes(SECRET);
const ok2 = !wire.includes('ZTE_TELNET_PW');
console.log(ok1 ? 'PASS: the value from the environment was sent' : 'FAIL: the password never reached the device');
console.log(ok2 ? 'PASS: no token leaked onto the wire' : 'FAIL: the literal token was typed');
process.exit(ok1 && ok2 ? 0 : 1);
