#!/bin/bash
# Flash native-wired-3.80 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.80 = the MCU_WA command ring. 3.74 closed the download (fw-state 7,
# stable), but every post-RDY command went unanswered on a perfectly drained
# ring, and 3.77's four wire layouts all failed identically, so it was never a
# framing problem: mt7915_mcu_send_message() moves commands from MT_MCUQ_WM
# (qid17) to MT_MCUQ_WA (qid20) the moment MT76_STATE_MCU_RUNNING is set, and
# qid20 lives at 0x18024440 - a ring this driver had never built. 3.80 adds
# qid20 plus a runtime selector (mcu-ring 0|1|4) so one boot can A/B all three
# banks.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.80/e2631-openwrt-native-wired-3.80.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.80.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.80.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.80.bin"

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
