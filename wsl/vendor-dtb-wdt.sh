#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Carve the vendor DTB out of the stock kernel image and look for the hardware
# watchdog node: we need its compatible + register window to write a driver (or
# at least to poke it from userspace) so a hung boot resets itself instead of
# costing a power cycle.
set -u
LOG=$E2631_WORK/out/vendor-dtb-wdt.log
exec >"$LOG" 2>&1
IMG=$E2631_WORK/device/backup/kernel1.bin
DTC=$(ls $E2631_TREE/staging_dir/host/bin/dtc 2>/dev/null | head -1)
echo "dtc=$DTC"
W=/tmp/vdtb; rm -rf $W; mkdir -p $W
for off in 0xa00 0x331080; do
	o=$((off))
	dd bs=1 skip=$o count=2000000 if=$IMG of=$W/cand-$(printf %x $o).dtb 2>/dev/null
	f=$W/cand-$(printf %x $o).dtb
	echo "--- candidate @ $off size=$(stat -c %s $f)"
	# sanity: totalsize field is at +4 big endian
	ts=$(od -An -j4 -N4 -tu4 --endian=big $f 2>/dev/null | tr -d ' ')
	echo "    DTB totalsize field = $ts (file has $(stat -c %s $f) bytes available)"
	if [ -n "$ts" ] && [ "$ts" -gt 100 ] && [ "$ts" -lt 5000000 ]; then
		dd bs=1 count=$ts if=$f of=$W/exact-$(printf %x $o).dtb 2>/dev/null
		echo "    carved exact $W/exact-$(printf %x $o).dtb ($ts bytes)"
		"$DTC" -I dtb -O dts $W/exact-$(printf %x $o).dtb -o $W/dts-$(printf %x $o).dts 2>&1 | head -3
		echo "    nodes mentioning wdt/watchdog/monitor:"
		grep -n -iE "watchdog|wdt|monitor|reset" $W/dts-$(printf %x $o).dts 2>/dev/null | head -25
		echo "    model/compatible:"
		grep -n -E "model =|compatible = \"zte" $W/dts-$(printf %x $o).dts 2>/dev/null | head -6
	fi
done
echo
echo "=== vendor kernel: the driver name behind monitor watchdog, from its symbol table ==="
strings -a $IMG | grep -iE "monitor_wdt|monitor_wdog|zxWdt|zx_wdt|watchdog" | head -20
