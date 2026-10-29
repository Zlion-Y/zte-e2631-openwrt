// Publish a finished, already-verified package to this repository's GitHub Release.
// The kit has no CI any more, so this is the last step of the local build path:
//   node build/host/publish.mjs --tag v1.0 --bin out/e2631-openwrt-v1.0.bin --notes out/relnotes.md
// Token only ever comes from `git credential fill` on stdin, via spawnSync (execFileSync puts the
// whole input into the exception object when it fails, which prints the token).
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const TAG = arg('--tag');
const BIN = arg('--bin');
const NOTES = arg('--notes');
const NAME = arg('--name', `E2631 OpenWrt ${TAG}`);
if (!TAG || !BIN || !NOTES) {
  console.error('usage: publish.mjs --tag <t> --bin <file> --notes <md> [--name <title>]');
  process.exit(2);
}
if (!fs.existsSync(BIN)) { console.error('no such package: ' + BIN); process.exit(2); }
if (!fs.existsSync(NOTES)) { console.error('no such notes file: ' + NOTES); process.exit(2); }

const HERE = path.dirname(new URL(import.meta.url).pathname).replace(/^\/([A-Za-z]:)/, '$1');
const REPO = path.join(HERE, '..');
const SLUG = (() => {
  const u = spawnSync('git', ['-C', REPO, 'remote', 'get-url', 'origin'], { encoding: 'utf8' }).stdout.trim();
  const m = /github\.com[:/]([^/]+\/[^/.]+)(\.git)?$/.exec(u);
  if (!m) { console.error('cannot parse origin remote: ' + u); process.exit(1); }
  return m[1];
})();

const cred = spawnSync('git', ['credential', 'fill'], { input: 'protocol=https\nhost=github.com\n\n', encoding: 'utf8' });
const token = /^password=(.*)$/m.exec(cred.stdout || '')?.[1]?.trim();
if (!token) { console.error('no credential from git credential fill'); process.exit(1); }
const hdr = { authorization: `token ${token}`, accept: 'application/vnd.github+json', 'user-agent': 'qoder-kit', 'content-type': 'application/json' };

const data = fs.readFileSync(BIN);
const sha256 = crypto.createHash('sha256').update(data).digest('hex');
const head = data.subarray(0, 16).toString('hex');
console.log(`slug=${SLUG} tag=${TAG} file=${path.basename(BIN)} size=${data.length} sha256=${sha256}`);
console.log(`first 16 bytes = ${head}`);
if (head.slice(0, 32) !== '999999994444444455555555aaaaaaaa') {
  console.error('REFUSING: not a vendor package (magic block missing)');
  process.exit(3);
}

// do not create a second release for the same tag
const existing = await (await fetch(`https://api.github.com/repos/${SLUG}/releases/tags/${TAG}`, { headers: hdr })).json();
let rel = existing && existing.id ? existing : null;
if (rel) console.log(`release for ${TAG} already exists: ${rel.html_url} (assets ${(rel.assets || []).length})`);
else {
  const r = await fetch(`https://api.github.com/repos/${SLUG}/releases`, {
    method: 'POST', headers: hdr,
    body: JSON.stringify({
      tag_name: TAG, name: NAME, body: fs.readFileSync(NOTES, 'utf8'),
      draft: false, prerelease: false, target_commitish: 'main',
    }),
  });
  if (!r.ok) { console.error('create release HTTP', r.status, (await r.text()).slice(0, 300)); process.exit(4); }
  rel = await r.json();
  console.log(`created release ${rel.tag_name}: ${rel.html_url}`);
}

const assets = rel.assets || [];
const dupe = assets.find((a) => a.name === path.basename(BIN));
if (dupe) console.log(`asset already uploaded: ${dupe.browser_download_url} (${dupe.size} B, digest ${dupe.digest || 'n/a'})`);
else {
  const url = `https://uploads.github.com/repos/${SLUG}/releases/${rel.id}/assets?name=${encodeURIComponent(path.basename(BIN))}`;
  const up = await fetch(url, {
    method: 'POST',
    headers: { authorization: `token ${token}`, 'content-type': 'application/octet-stream', 'user-agent': 'qoder-kit', 'content-length': String(data.length) },
    body: data,
  });
  if (!up.ok) { console.error('upload HTTP', up.status, (await up.text()).slice(0, 300)); process.exit(5); }
  const a = await up.json();
  console.log(`uploaded ${a.name} ${a.size} B -> ${a.browser_download_url}`);
  console.log(`github digest: ${a.digest || '(not reported)'}   local sha256: sha256:` + sha256);
}

// sidecar so the download can be checked without trusting anyone's word
const sum = `${sha256}  ${path.basename(BIN)}\n`;
const sumName = 'SHA256SUMS';
const haveSum = (rel.assets || []).find((a) => a.name === sumName);
if (haveSum) console.log(`SHA256SUMS already present (${haveSum.digest || 'n/a'})`);
else {
  const up2 = await fetch(`https://uploads.github.com/repos/${SLUG}/releases/${rel.id}/assets?name=${sumName}`, {
    method: 'POST',
    headers: { authorization: `token ${token}`, 'content-type': 'text/plain', 'user-agent': 'qoder-kit', 'content-length': String(sum.length) },
    body: sum,
  });
  console.log(up2.ok ? `uploaded SHA256SUMS` : `SHA256SUMS upload HTTP ${up2.status}`);
}
console.log('release page: ' + (await (await fetch(`https://api.github.com/repos/${SLUG}/releases/${rel.id}`, { headers: hdr })).json()).html_url);
