#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Is there a driver for "arm,cortex-a9-twd-wdt", and is the node present in the
# DTS we actually ship?  A hardware watchdog that fires while the CPU spins with
# interrupts disabled is the only instrument that both proves the hang and
# recovers from it without a power cycle.
set -u
LOG=$E2631_WORK/out/twd-wdt.log
exec >"$LOG" 2>&1
K=$(ls -d $E2631_TREE/build_dir/target-*/linux-*/linux-*/ | head -1)
echo "K=$K"
echo "=== any in-tree driver/binding matching cortex-a9-twd-wdt ==="
grep -rIn "cortex-a9-twd-wdt" "$K/Documentation/" "$K/drivers/" 2>/dev/null | head -10
echo
echo "=== ARM CPU watchdog candidates in the tree ==="
grep -rIn "ARM_CPU_WATCHDOG\|SBSA_WATCHDOG\|CADENCE_WATCHDOG\|ARM_SP805" "$K/drivers/watchdog/Kconfig" 2>/dev/null | head -12
grep -n "^config" "$K/drivers/watchdog/Kconfig" | grep -iE "arm|versatile|sp805|twd" | head -10
echo
echo "=== our shipped board DTS: watchdog / pl310 / twd nodes ==="
D=$E2631_TREE/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts
grep -n -iE "watchdog|twd|pl310|cache|sys-ctrl|crm" "$D" 2>/dev/null | head -14
echo
echo "=== the SoC dtsi the vendor used (from the vendor DTB we carved) ==="
/usr/bin/dtc -I dtb -O dts $E2631_WORK/out/e2638-vendor.dtb -o /tmp/vend.dts 2>/dev/null
awk '/watchdog@/,/};/' /tmp/vend.dts
echo "--- pl310 node (the L2 that the deadlock is about):"
awk '/pl310|cache@/,/};/' /tmp/vend.dts | head -20
echo
echo "=== vendor kernel: which watchdog driver did they build? ==="
strings -a $E2631_WORK/device/backup/kernel1.bin | grep -iE "twd|watchdog|monitor_wdt" | head -12
echo
echo "=== our kernel .config: watchdog framework state ==="
grep -nE "WATCHDOG|SBSA|SP805" "$K/.config" 2>/dev/null | head -12
echo
echo "=== is the pl310 L2 also what our DTS enables? ==="
grep -nE "CACHE_L2X0|OUTER_CACHE|PL310|ARM_GLOBAL_TIMER|HAVE_ARM_ARCH_TIMER" "$K/.config" | head -10
