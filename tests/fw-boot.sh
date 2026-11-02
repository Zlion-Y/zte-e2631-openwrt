#!/bin/bash
# fw-boot.sh — the full wireless firmware bring-up, end to end.
#
#   bash tests/fw-boot.sh [user@host]
#
# Sequence (mt76 order, everything commit-gated):
#   enable -> reprobe -> dma-init -> driver-own -> wfsys-reset ->
#   driver-own -> dma-init -> patch download (fw_ring=0) ->
#   WM regions+start -> WA regions+start -> poll MT_TOP_MISC for 7
#
# Acceptance for the round: fw-state leaves 1 and reaches 7 (RDY).
set -u
DEV=${1:-root@192.168.1.1}
SSH="ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $DEV"
P=/proc/zx-csp-wlan/ctl
S=/proc/zx-csp-wlan/status

say() { printf '\n== %s\n' "$*"; }

$SSH "cat /etc/e2631-version" || { echo "device unreachable"; exit 1; }

say "enable + reprobe + symlinks + fw_ring=0"
$SSH "echo 1 > /sys/module/zxnpu_wlan/parameters/enable
echo '0000:01:00.0' > /sys/bus/pci/drivers/zxnpu-wlan/unbind 2>/dev/null
echo '0001:01:00.0' > /sys/bus/pci/drivers/zxnpu-wlan/unbind 2>/dev/null
echo '0000:01:00.0' > /sys/bus/pci/drivers/zxnpu-wlan/bind
echo '0001:01:00.0' > /sys/bus/pci/drivers/zxnpu-wlan/bind
ln -sf /lib/firmware/zte/mt7916/mt7916_patch_e1_hdr.bin /lib/firmware/p.bin
ln -sf /lib/firmware/zte/mt7916/WIFI_RAM_CODE_MT7916.bin /lib/firmware/wm.bin
ln -sf /lib/firmware/zte/mt7916/7916_WACPU_RAM_CODE_release.bin /lib/firmware/wa.bin
sleep 2
echo 0 > /sys/module/zxnpu_wlan/parameters/fw_ring
echo 1 > /sys/module/zxnpu_wlan/parameters/commit"

say "dma-init"
$SSH "echo dma-init > $P; tail -2 $S"

say "driver-own"
$SSH "echo driver-own > $P; tail -1 $S"

say "wfsys-reset (ROM re-runs)"
$SSH "echo wfsys-reset > $P; grep fw-state $S"

say "driver-own + dma-init + rx-init (fresh window)"
$SSH "echo driver-own > $P; tail -1 $S
echo dma-init > $P; tail -1 $S
echo rx-init > $P; grep 'reg ' $S | head -1"

say "patch download (fw_ring=0, scatter via qid16)"
$SSH "echo 'mcu-dl p.bin' > $P
cat $S
echo mcu-recv > $P; cat $S"

say "WM firmware regions + start"
$SSH "echo 'fw-dl wm' > $P
cat $S
echo mcu-recv > $P; cat $S"

say "WA firmware regions + start"
$SSH "echo 'fw-dl wa' > $P
cat $S
echo mcu-recv > $P; cat $S"

say "sync (fw-state readback)"
$SSH "echo sync > $P; tail -2 $S"

say "fw-state poll (target: 7 = RDY)"
$SSH "for i in 1 2 3 4 5 6 7 8; do echo fw-state > $P; grep state $S | tail -1; sleep 2; done"

say "ring final state"
$SSH "for a in d4410 d4414 d4418 d441c d4500 d4508 d450c; do printf '%s=' \$a; echo 'win 0 '\$a' 1' > $P; sed -n 's/.*= //p' $S | tr -d '\n'; printf ' '; done; echo"

$SSH "echo 0 > /sys/module/zxnpu_wlan/parameters/commit"
