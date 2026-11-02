#!/bin/bash
# Flash native-wired-3.83 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.83 = eeprom from the shipped map instead of over the MCU. 3.81/3.82 asked
# firmware for efuse blocks (MCU_EXT_QUERY(EFUSE_ACCESS), 8-byte and full
# 24-byte requests, both s2d variants): every one is acknowledged and none
# carries a payload. /lib/firmware/zte/mt7916/MT7916_EEPROM.bin is 4096 bytes =
# MT_EE_MAX_V2 and parses as a plain mt7915 v2 map (chip id 0x7916 at 0x000,
# MAC at 0x004, WIFI_CONF at 0x190, TX0_POWER_*_V2 at 0x441/0x445), which is
# exactly the fallback mt7915_eeprom_load_default() takes - and it is what sets
# flash_mode. 3.83 loads it with request_firmware_direct, validates the chip id
# and parses per-band select / tx path into the chainmask.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.83/e2631-openwrt-native-wired-3.83.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.83.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.83.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.83.bin"

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
