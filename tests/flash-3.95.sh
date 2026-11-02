#!/bin/bash
# Flash native-wired-3.95 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.95 is diagnostics only. 3.87 already brought band0 up: with rx_path sent as
# a chain COUNT (hweight8, as mt7915_mcu_set_chan_info() does for this command)
# the OMAC / DEV_INFO / CHANNEL_SWITCH / BSS_INFO sequence answers ret=0,
# bss-show reports "band 0: active ch=1", and FW_LOG still answers afterwards.
# Proving it was the problem: the log cut the block with grep -A9 (dropping the
# channel and bss lines) and the tuned firmware posts unsolicited eid 0x00
# events whose 68-byte hex dumps filled the 16 KiB status buffer (16383 bytes,
# measured), after which every append was dropped. evt_log now gates the hex.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.95/e2631-openwrt-native-wired-3.95.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.95.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.95.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.95.bin"

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
