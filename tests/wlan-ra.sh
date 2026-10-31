#!/bin/bash
# wlan-ra.sh — on-device acceptance driver for the R-A wireless round.
#
#   bash tests/wlan-ra.sh [user@host] [--commit]
#
# R-A's question is narrow: after the main function (7906) runs the wfsys
# reset and the mt76 WFDMA bring-up, does the firmware-download ring move?
# Everything here is read-only until --commit is passed, and the dry passes
# print the exact register values the commit would touch.
#
# Acceptance (must all hold for --commit):
#   A1 MT_TOP_MISC.FW_STATE differs before/after wfsys-reset (MCU re-ran ROM)
#   A2 dma-init reports hif misc idle and GLO_CFG TX/RX enable set
#   A3 driver-own completes without timeout and FW_OWN_STAT clears
#   A4 an MCU submit moves DMA_IDX (the engine fetched our descriptor)
set -u
DEV=${1:-root@192.168.1.1}
case "${2:-}" in --commit) COMMIT=1 ;; *) COMMIT=0 ;; esac
SSH="ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $DEV"

say() { printf '\n== %s\n' "$*"; }

# The ring block: +0 desc_base, +4 ring_size, +8 cpu_idx, +12 dma_idx,
# at the directly decoded bar0 offsets 0xd4400 (qid16 FWDL) and 0xd4410
# (qid17 MCU_WM). Full offsets, not concatenations - a d4400+0 style join
# produced d44000 and the driver (rightly) refused the out-of-range read.
DUMP='for a in d4400 d4404 d4408 d440c d4410 d4414 d4418 d441c; do \
printf "%s=" $a; echo "win 0 $a 1" > /proc/zx-csp-wlan/ctl; \
sed -n "s/.*= //p" /proc/zx-csp-wlan/status | tr -d "\n"; printf " "; done; echo'

run() {	# run <ctl command>
	$SSH "echo '$1' > /proc/zx-csp-wlan/ctl; cat /proc/zx-csp-wlan/status"
}

$SSH "cat /etc/e2631-version; uptime" || { echo "device unreachable"; exit 1; }

say "set commit=0 for the dry pass"
$SSH "echo 0 > /sys/module/zxnpu_wlan/parameters/commit; cat /sys/module/zxnpu_wlan/parameters/commit"

say "probe evidence (7906 main section)"
$SSH "awk '/7906 main/{p=1} p' /proc/zx-csp-wlan/probe | head -20"

say "ring registers before anything"
$SSH "$DUMP"

say "dry: driver-own"
run driver-own

say "dry: wfsys-reset"
run wfsys-reset

say "dry: dma-init"
run dma-init

if [ "$COMMIT" != 1 ]; then
	say "dry pass done; re-run with --commit to touch the hardware"
	exit 0
fi

$SSH "echo 1 > /sys/module/zxnpu_wlan/parameters/commit"

# mt76 order (pci probe -> init_hardware -> mcu_init): wfsys reset first,
# then the WFDMA bring-up, and only then driver own - taking ownership
# before the reset would be undone by it.
say "COMMIT wfsys reset (A1: FW_STATE must change)"
run wfsys-reset

say "COMMIT dma-init (A2: hif misc idle + TX/RX enable)"
run dma-init

say "COMMIT driver own (A3)"
run driver-own

say "ring registers after init"
$SSH "$DUMP"

say "A4: patch semaphore get on MCU_WM (qid17)"
run mcu-send

say "ring registers after mcu-send"
$SSH "$DUMP"

say "fw-state now"
run fw-state

say "kernel log (driver lines)"
$SSH "logread | grep -i 'zxnpu\|R-A ready' | tail -15"
