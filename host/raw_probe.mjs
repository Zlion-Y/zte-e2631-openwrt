// Raw TCP probe: connect, log every byte with timestamps, optionally pre-negotiate.
// Usage: node raw_probe.mjs <host> <port> <seconds> [preiac]
import net from 'node:net';
const [host, port, secs, pre] = process.argv.slice(2);
const t0 = Date.now();
const sock = net.connect(parseInt(port, 10), host);
const log = (tag, d) => {
  const dt = ((Date.now() - t0) / 1000).toFixed(2);
  console.log(`[${dt}s ${tag}] len=${d.length} ${d.toString('hex').slice(0, 400)}`);
  const txt = d.toString('latin1').replace(/[^\x20-\x7e]/g, '.');
  if (txt.trim()) console.log(`        ascii: ${txt.slice(0, 300)}`);
};
sock.on('connect', () => {
  console.log(`connected ${host}:${port}`);
  if (pre === 'preiac') {
    // WILL ECHO, WILL SGA, DO TM, DO ECHO
    sock.write(Buffer.from([255, 251, 1, 255, 251, 3, 255, 253, 240, 255, 253, 1]));
    console.log('sent pre-negotiation');
  }
  setTimeout(() => sock.write('whoami\r\n'), 1500);
  setTimeout(() => sock.write('\r\n'), 3000);
});
sock.on('data', (d) => log('RECV', d));
sock.on('end', () => console.log('END (server FIN)'));
sock.on('close', (h) => console.log('CLOSE hadError=' + h));
sock.on('error', (e) => console.log('ERROR ' + e.code + ' ' + e.message));
setTimeout(() => { console.log('timeout, destroying'); sock.destroy(); process.exit(0); }, (parseInt(secs, 10) || 10) * 1000);
