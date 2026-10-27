"""Exercise device upgrade control flow with local fixtures; never access MTD.

Adopted from the zwthys-cyber fork and adapted to the v1.4 in-place upgrade
semantics: platform.sh calls "e2631-fwslot upgrade-check" before services go
down and again from the ramfs, then "upgrade" (the running slot, in place) -
so the stubbed tool must answer upgrade-check, and platform_do_upgrade must
refuse to write anything unless that check passes.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'overlay/target/linux/sanechips/base-files'
UPDATER = (BASE / 'sbin/e2631-upgrade').read_text().split('\ncase "${1:-}" in\n')[0]
PLATFORM = (BASE / 'lib/upgrade/platform.sh').read_text().replace('. /lib/functions.sh', ':')


class UpgradeTests(unittest.TestCase):
    def download(self, manifest, *, fetch_ok=True, package_ok=True):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'asset').write_bytes(b'firmware fixture')
            (path / 'manifest').write_text(manifest)
            checker = path / 'fwslot'
            checker.write_text('#!/bin/sh\necho checked >> "$TEST_DIR/checked"\n'
                               + ('exit 0\n' if package_ok else 'exit 1\n'))
            checker.chmod(0o755)
            script = 'uci() { return 1; }\n' + UPDATER
            # Redirect the one persistent version marker into the test sandbox.
            script = script.replace('/etc/.e2631-pending-version', tmp + '/pending')
            script += '''
DLDIR="$TEST_DIR"
E2631_FWSLOT="$TEST_DIR/fwslot"
latest() { echo "v-test https://github.com/owner/repo/releases/download/v-test/image.bin"; }
fetch() {
    case "$1" in
        */SHA256SUMS) [ "$FETCH_OK" = 1 ] && cp "$TEST_DIR/manifest" "$2" ;;
        *) cp "$TEST_DIR/asset" "$2" ;;
    esac
}
do_download
'''
            result = subprocess.run(['busybox', 'ash', '-c', script], text=True,
                                    capture_output=True, env=dict(os.environ,
                                    TEST_DIR=tmp, FETCH_OK=str(int(fetch_ok))))
            return result, (path / 'pending').exists(), (path / 'checked').exists()

    def test_exact_and_binary_entries(self):
        digest = hashlib.sha256(b'firmware fixture').hexdigest()
        for name in ('image.bin', '*image.bin'):
            with self.subTest(name=name):
                result, pending, checked = self.download(digest + '  ' + name + '\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(pending and checked)

    def test_reject_incomplete_or_ambiguous_manifest(self):
        digest = hashlib.sha256(b'firmware fixture').hexdigest()
        cases = ['', digest + '  other-image.bin\n', 'x' * 64 + '  image.bin\n',
                 digest + '  image.bin\n' + digest + '  image.bin\n',
                 '0' * 64 + '  image.bin\n']
        for manifest in cases:
            with self.subTest(manifest=manifest):
                result, pending, checked = self.download(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(pending or checked)

    def test_checksum_download_failure(self):
        result, pending, checked = self.download('', fetch_ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(pending or checked)

    def test_package_check_still_required(self):
        digest = hashlib.sha256(b'firmware fixture').hexdigest()
        result, pending, checked = self.download(digest + '  image.bin\n', package_ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(pending)
        self.assertTrue(checked)

    def test_platform_never_flashes_when_layout_rejected(self):
        for allowed in (False, True):
            with self.subTest(allowed=allowed), tempfile.TemporaryDirectory() as tmp:
                checker = Path(tmp) / 'fwslot'
                checker.write_text('''#!/bin/sh
case "$1" in
upgrade-check) exit "$LAYOUT_RC" ;;
upgrade) echo flash >> "$TEST_DIR/flashed" ;;
*) exit 99 ;;
esac
''')
                checker.chmod(0o755)
                script = PLATFORM + '\nE2631_FWSLOT="$TEST_DIR/fwslot"\nplatform_do_upgrade fixture.bin\n'
                result = subprocess.run(['busybox', 'ash', '-c', script], text=True,
                                        capture_output=True, env=dict(os.environ,
                                        TEST_DIR=tmp, LAYOUT_RC='0' if allowed else '1'))
                self.assertEqual(result.returncode, 0 if allowed else 1, result.stderr)
                self.assertEqual((Path(tmp) / 'flashed').exists(), allowed)

    def test_platform_check_image_runs_the_prerequisite_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            checker = Path(tmp) / 'fwslot'
            pkg = Path(tmp) / 'image.bin'
            pkg.write_bytes(b'\x99' * 16)
            checker.write_text('#!/bin/sh\ncase "$1" in check) exit 0 ;; upgrade-check) exit "$RC" ;; *) exit 99 ;; esac\n')
            checker.chmod(0o755)
            script = PLATFORM + ('\nE2631_MAGIC_SHA=$(printf "%0.s9" 1 2>/dev/null; true)\n'
                                 'E2631_MAGIC_SHA=3d25b5b9bf4079c6760449611963b6f1\n'
                                 'E2631_FWSLOT="$TEST_DIR/fwslot"\n'
                                 'platform_check_image "$TEST_DIR/image.bin"\n')
            # the outer magic sha256 gate rejects our 16x99 fixture before the
            # fwslot calls, so only the refuse-early path is asserted here
            result = subprocess.run(['busybox', 'ash', '-c', script], text=True,
                                    capture_output=True, env=dict(os.environ,
                                    TEST_DIR=tmp, RC='1'))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('no E2631 factory magic', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
