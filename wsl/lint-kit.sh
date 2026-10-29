#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Normalize line endings for every script we push to the box and prove each one
# parses under busybox ash.  Without this, a Windows-authored CRLF shows up on
# the device as a baffling "unexpected newline (expecting ")")".
set -u
cd "$E2631_KIT" || exit 1   # the repo root; out/ lives one level above
FILES="
board/fw-ab.sh
board/hb.sh
board/mpscfg.sh
board/stages.sh
board/linkspeed.sh
overlay/target/linux/sanechips/base-files/sbin/e2631-hb
overlay/target/linux/sanechips/base-files/sbin/e2631-wifi-on
overlay/target/linux/sanechips/base-files/sbin/e2631-wifi-ap
overlay/target/linux/sanechips/base-files/sbin/e2631-trial
overlay/target/linux/sanechips/base-files/etc/init.d/e2631-trial
overlay/target/linux/sanechips/base-files/sbin/e2631-linkspeed
overlay/target/linux/sanechips/base-files/sbin/e2631-fwstate
overlay/target/linux/sanechips/base-files/etc/init.d/e2631-wifi-eeprom
overlay/target/linux/sanechips/base-files/etc/init.d/e2631-wifi-load
overlay/target/linux/sanechips/base-files/etc/init.d/e2631-hb
"
rc=0
for f in $FILES; do
	[ -f "$f" ] || { echo "MISSING $f"; rc=1; continue; }
	sed -i 's/\r$//' "$f"
	cp "$f" /tmp/lint.sh
	if /bin/busybox ash -n /tmp/lint.sh 2>/tmp/lint.err; then
		echo "OK    $(basename "$f")"
	else
		echo "FAIL  $f: $(head -c 120 /tmp/lint.err)"
		rc=1
	fi
done
# apply.sh and the wsl/ helpers run under WSL bash, not ash: same CRLF problem,
# different parser.  Check them separately so one gate does not hide the other.
for f in apply.sh wsl/*.sh; do
	sed -i 's/\r$//' "$f"
	if bash -n "$f" 2>/tmp/lintb.err; then
		echo "bash-OK  $f"
	else
		echo "bash-FAIL $f: $(head -c 160 /tmp/lintb.err)"
		rc=1
	fi
done
echo "rc=$rc"
