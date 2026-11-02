#!/bin/bash
# Flash native-wired-3.82 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.82 = efuse reads over the MCU. 3.78-3.80 closed the command surface: the
# post-RDY ring is qid20 (MCU_WA, 0x18024440), not qid17, and the RX ring's
# CPU_IDX has to be announced as (didx - 1) % depth (mt76's q->head) rather than
# didx + depth. With both fixed, mt7915's own post-download init calls are
# answered. 3.82 goes one step further and asks for data instead of an ack:
# MCU_EXT_QUERY(EFUSE_ACCESS) {addr,valid} -> {addr,valid,data[16]}, i.e.
# mt7915_mcu_get_eeprom(). The map holds the chip id, the MAC address and the
# band capabilities, which is the gate in front of band0 rings and wiphy
# registration. The raw TLV payload is dumped too, so the response layout is
# learned rather than assumed.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.82/e2631-openwrt-native-wired-3.82.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.82.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.82.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.82.bin"

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
