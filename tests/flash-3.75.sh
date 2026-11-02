#!/bin/bash
# Flash native-wired-3.75 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.75 = the ring-index wrap fix on top of 3.71. 3.71 did the FW_SCATTER
# framing correction of audit §76 (raw frames on the FWDL ring, the form the
# patch stage has always used successfully) and the r2 silence went away; the
# download then died on "cidx == didx" because didx is kept modulo the 128-deep
# ring while the software cidx free-runs. 3.75 takes every ring-index distance
# modulo depth.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.75/e2631-openwrt-native-wired-3.75.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.75.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.75.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.75.bin"

echo "== reboot via sysrq (busybox reboot hit I/O errors on the freshly"
echo "== rewritten running rootfs and silently failed once already)"
$SSH "sync; echo b > /proc/sysrq-trigger" || true

echo "== waiting for the box to go down (reboot actually happens)"
for i in $(seq 1 20); do
	sleep 3
	$SSH -o ConnectTimeout=3 echo up >/dev/null 2>&1 || break
done
echo "== waiting for it to come back (max 150s)"
for i in $(seq 1 50); do
	sleep 3
	$SSH -o ConnectTimeout=3 echo up >/dev/null 2>&1 && break
done
sleep 25	# let netifd/dropbear settle before the sweep

echo "== post-flash full verification"
bash tests/device-verify.sh "$DEV"
