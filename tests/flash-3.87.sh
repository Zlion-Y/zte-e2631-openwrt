#!/bin/bash
# Flash native-wired-3.87 to the device and run the full post-flash sweep.
# Route: e2631-fwslot upgrade (in-place rewrite of the RUNNING slot = the
# OpenWrt slot). NEVER install/flash - the other slot holds the vendor
# system (forbidden to touch). Read-only until the explicit upgrade line.
#
# 3.87 fixes one field. 3.86 got RX_HDR_TRANS 0x47 and MAC_INIT_CTRL 0x46
# answered (ret=0) and then lost the firmware inside CHANNEL_SWITCH 0x08: no
# more events on either MCU ring, qid20 stopped being fetched, and FW_LOG -
# which had answered a minute earlier - timed out as well. The cause is a line
# in mt7915_mcu_set_chan_info():
#
#	if (cmd == MCU_EXT_CMD(CHANNEL_SWITCH))
#		req.rx_path = hweight8(req.rx_path);
#
# rx_path is a chain COUNT for this command, not the antenna mask. 3.86 sent
# 0x3 where the firmware expects 2, i.e. it asked a 2-chain band for three RX
# paths and the WA core wedged mid-tune. 3.87 sends the count and gives the
# first tune 5 s instead of 2 s (it runs the calibration, not just a retune).
#
# The rest of the sequence is unchanged: RX_HDR_TRANS 0x47, MAC_INIT_CTRL 0x46,
# DEV_INFO_UPDATE 0x2a, CHANNEL_SWITCH 0x08, BSS_INFO_UPDATE 0x26, exposed as
# radio-mac / radio-chan / radio-bss / radio-up plus mac-set.
set -eu
DEV=${1:-root@192.168.1.1}
PKG=${2:-../out/native-wired-3.87/e2631-openwrt-native-wired-3.87.bin}
SSH="ssh -b 192.168.1.177 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $DEV"
ssh-keygen -R 192.168.1.1 >/dev/null 2>&1 || true
SCP="scp -O -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

[ -f "$PKG" ] || { echo "package missing: $PKG"; exit 1; }
echo "== local sha256"
sha256sum "$PKG"

echo "== transfer"
$SCP "$PKG" "$DEV:/tmp/e2631-3.87.bin"

echo "== on-device gate: e2631-fwslot check"
$SSH "e2631-fwslot check /tmp/e2631-3.87.bin"

echo "== on-device gate: upgrade-check"
$SSH "e2631-fwslot upgrade-check"

echo "== in-place upgrade of the running slot"
$SSH "e2631-fwslot upgrade /tmp/e2631-3.87.bin"

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
