#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Is the documented deadlock guard actually live in our build?
# The base tree's own mt76 patch routes reads through mt76_readl(), which only
# takes outer_io_lock() when CONFIG_CACHE_L2X0_IO_LOCK is enabled. If that
# symbol is off, the guard is inert and we are exactly in the documented case.
set -u
LOG=$E2631_WORK/out/l2x0-iolock.log
exec >"$LOG" 2>&1
K=$(ls -d $E2631_TREE/build_dir/target-*/linux-*/linux-*/ | head -1)
echo "K=$K"
echo "=== does the symbol exist in this tree at all? ==="
grep -rn "L2X0_IO_LOCK" "$K/arch/arm/" 2>/dev/null | head -20
echo
echo "=== outer_io_lock definition / users ==="
grep -rn "outer_io_lock\|outer_sync" "$K/include/linux/" "$K/arch/arm/mm/" "$K/mm/" 2>/dev/null | head -20
echo
echo "=== our kernel .config ==="
grep -nE "CACHE_L2X0|IO_LOCK|PL310|L2C0" "$K/.config" 2>/dev/null
echo
echo "=== same question against the fragments (merge order) ==="
for f in $E2631_TREE/target/linux/generic/config-6.18 $E2631_TREE/target/linux/sanechips/config-6.18; do
	echo "--- $f"; grep -nE "L2X0|IO_LOCK|PL310" "$f" 2>/dev/null || echo "  (not mentioned)"
done
echo
echo "=== mt76_readl as built: locked or plain? ==="
M=$(ls -d $E2631_TREE/build_dir/target-*/linux-sanechips_generic/mt76-*/ | head -1)
grep -rn -A 12 "mt76_readl" "$M"/*.h "$M"/*.c 2>/dev/null | head -30
echo
echo "=== vendor kernel: did they enable the equivalent? ==="
grep -nE "L2X0|IO_LOCK|CACHE" device/backup/vendor-config-pcie.txt 2>/dev/null | head
grep -nE "L2X0_IO_LOCK|CACHE_L2X0" $E2631_KIT/../device/backup/vendor-config-full.txt 2>/dev/null | head
echo "vendor config file: $(ls -l $E2631_WORK/device/backup/vendor-config-full.txt 2>/dev/null)"
grep -nE "L2X0_IO_LOCK|CACHE_L2X0|NEED_DMA_MAP_STATE|DMA_API_OPS" $E2631_WORK/device/backup/vendor-config-full.txt 2>/dev/null | head
