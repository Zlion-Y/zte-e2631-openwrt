#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Can we enter failsafe WITHOUT serial TX, and does failsafe give us a network
# shell?  Check the image we actually flashed, not the upstream docs.
set -u
LOG=$E2631_WORK/out/failsafe-check.log
exec >"$LOG" 2>&1
P=$E2631_WORK/out/efusefix/payload/sysupgrade-zte_zxhn-e2631
U=$(ls -d $E2631_TREE/staging_dir/host/bin/unsquashfs* | head -1)
W=/tmp/fsroot; rm -rf $W; mkdir -p $W
"$U" -no-xattrs -d $W "$P/root" >/dev/null 2>&1

echo "=== board DTS buttons (which physical key can trigger failsafe) ==="
grep -n -B2 -A8 "keys\|button\|gpio-keys" $E2631_TREE/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts 2>/dev/null | head -40
echo
echo "=== /etc/preinit failsafe logic in the shipped image ==="
grep -n -A6 "failsafe\|fail_wait\|boot_failcount" "$W/etc/preinit" 2>/dev/null | head -40
echo
echo "=== button hotplug handlers that arm failsafe ==="
ls -l "$W/etc/hotplug.d/" 2>/dev/null | head
for f in "$W"/etc/hotplug.d/button/* "$W"/lib/preinit/*; do
	[ -e "$f" ] || continue
	echo "--- $f"; cat "$f" 2>/dev/null | head -14
done
echo
echo "=== is telnetd (busybox) present, and does preinit start it in failsafe? ==="
ls -l "$W/bin/busybox" >/dev/null 2>&1 && busybox_sh=$(basename "$W/bin/busybox"); grep -c telnet "$W/etc/preinit" 2>/dev/null
strings "$W/bin/busybox" 2>/dev/null | grep -x -E "telnetd|telnet" | head -3
grep -rn "telnetd" "$W/etc/preinit" "$W/lib/preinit/" 2>/dev/null | head -5
echo
echo "=== does the image still carry my bad hook, or only the overlay does? ==="
grep -n "e2631-linkspeed" "$W/etc/init.d/e2631-wifi-eeprom" 2>/dev/null && echo "  ^ IN THE IMAGE" || echo "  image copy is clean (hook only lived in the overlay)"
echo
echo "=== network config in preinit: which iface, which IP ==="
grep -rn -A6 "lan=\|ifname.*lan\|192.168.1.1" "$W/etc/network.preinit_config" "$W/lib/preinit/"* 2>/dev/null | head -20
ls "$W/etc/" | grep -i "network" 
