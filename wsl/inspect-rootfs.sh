#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# What is actually inside the rootfs we are about to flash - format checks pass on
# images that will not boot, so look at the payload itself: the A/B switcher, whether the
# squashfs extracted completely, and what the shipped wireless config defaults to.
#
#   bash wsl/inspect-rootfs.sh <dir holding payload/sysupgrade-*>   (default: the newest)
set -u
LOG=$E2631_WORK/out/inspect-rootfs.log
exec >"$LOG" 2>&1
W=/tmp/finalroot
P=${1:-$E2631_WORK/out/gen1/payload/sysupgrade-zte_zxhn-e2631}
U=$(ls -d $E2631_TREE/staging_dir/host/bin/unsquashfs* | head -1)

# Extract FIRST. This script used to read /tmp/finalroot without ever populating it, so every
# report it gave was whatever an earlier run of the same script had left there -- a stale rootfs
# was being presented as the one we are about to flash.
rm -rf "$W"
"$U" -no-xattrs -quiet -d "$W" "$P/root" >>"$LOG" 2>&1
echo "=== extraction rc=$? : $(find "$W" | wc -l) paths unpacked from $P/root ==="
echo

echo "=== every e2631* file in the shipped rootfs ==="
find "$W" -name 'e2631*' | sed "s|$W/||" | sort

echo
echo "=== extraction completeness: listing vs extracted ==="
"$U" -ll "$P/root" 2>/dev/null | wc -l
echo "  (unsquashfs -ll lines above; files on disk: $(find "$W" | wc -l) incl dirs)"
"$U" -stat "$P/root" 2>&1 | tail -6
echo "  squashfs bytes=$(stat -c %s "$P/root")  md5 of image inside build dir:"
md5sum "$P/root"

echo
echo "=== shipped /etc/config/wireless (does boot bring radios up by itself?) ==="
cat "$W/etc/config/wireless" 2>/dev/null | head -40

echo
echo "=== trial/commit gate: what decides 'accepted' ==="
sed -n '1,40p' "$W/etc/init.d/e2631-trial" 2>/dev/null
echo "--- sbin/e2631-trial ---"
sed -n '1,60p' "$W/sbin/e2631-trial" 2>/dev/null

echo
echo "=== does anything still auto-load mt7915e? ==="
grep -rn "mt7915e" "$W/etc/" 2>/dev/null | head
echo "--- hotplug.d/net setup hooks ---"
ls "$W/etc/hotplug.d/" 2>/dev/null

echo
echo "=== LED / DTB sanity: led aliases and names baked into the shipped dtb ==="
D=$(find "$W" -name '*e2631*.dtb' | head -1); echo "dtb=$D"
[ -n "$D" ] && $E2631_TREE/staging_dir/host/bin/dtc -I dtb -O dts "$D" 2>/dev/null | sed -n '1,60p'
