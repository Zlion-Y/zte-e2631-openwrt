#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Two facts that decide the recovery route:
#  1. can a physical button arm failsafe without serial TX?
#  2. does failsafe hand us a network shell (telnetd) we can drive ourselves?
set -u
LOG=$E2631_WORK/out/failsafe-route.log
exec >"$LOG" 2>&1
W=/tmp/fsroot
echo "W=$W exists=$([ -d $W ] && echo yes || echo no)"
echo "=== hotplug.d dirs ==="
ls $W/etc/hotplug.d/ 2>/dev/null | tr '\n' ' '; echo
ls -d $W/etc/hotplug.d/keys $W/etc/hotplug.d/input 2>/dev/null || echo "(no keys/input dir => no button-triggered failsafe)"
echo
echo "=== preinit failsafe listener ==="
grep -nE "keys|usb_kbd|console|read -t|telnetd|hotplug-call" $W/lib/preinit/30_failsafe_wait 2>/dev/null | head -20
echo
echo "=== who runs the failsafe hook, and what does it do ==="
for f in $W/lib/preinit/40_* $W/lib/preinit/50_*; do
	[ -e "$f" ] || continue
	echo "--- $f"; cat "$f" | head -22
done
echo
echo "=== telnetd present in busybox? ==="
grep -rn "telnetd" $W/lib/preinit/ $W/etc/preinit 2>/dev/null | head -8
echo
echo "=== does the flashed IMAGE (not overlay) contain my hook? ==="
grep -n "e2631-linkspeed" $W/etc/init.d/e2631-wifi-eeprom 2>/dev/null || echo "  clean: hook lived only in the overlay"
echo
echo "=== failsafe timeout / console read source ==="
grep -nE "fs_failsafe_wait_timeout|keypress|/dev/console" $W/lib/preinit/00_preinit.conf $W/lib/preinit/30_failsafe_wait 2>/dev/null | head -12
