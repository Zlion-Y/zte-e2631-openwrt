#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# After the build: (a) did the efuse patch actually land, (b) which direct
# device reads in mt7915 are still NOT routed through the IO-lock helper.
set -u
LOG=$E2631_WORK/out/mt7915-readl-gap.log
exec >"$LOG" 2>&1
M=$(ls -d $E2631_TREE/build_dir/target-*/linux-sanechips_generic/mt76-*/ | head -1)
echo "M=$M"
echo "=== WITNESS lines from the build log ==="
grep -h "WITNESS" $HOME/build.log 2>/dev/null | tail -8
echo
echo "=== efuse patch present in the prepared source? ==="
grep -n -A2 "Try the file first" "$M/mt7915/eeprom.c" 2>/dev/null | head -6
echo
echo "=== mt76_readl helper as built ==="
awk '/static inline u32 mt76_readl/,/^}/' "$M/mt76.h"
echo
echo "=== mt7915 direct readl() users NOT routed through mt76_readl ==="
grep -rn "readl(" "$M/mt7915/" 2>/dev/null | grep -v mt76_readl | head -20
echo
echo "=== __mt7915_rr body ==="
awk '/static u32 __mt7915_rr/,/^}/' "$M/mt7915/mmio.c"
echo "=== mt7915_rr / mt7915_rd wrappers ==="
awk '/u32 mt7915_rr\(/,/^}/' "$M/mt7915/mmio.c"
awk '/static void mt7915_mmio_mcu_reset|mt7915_register_device/,/^}/' "$M/mt7915/mmio.c" | head -5
echo
echo "=== count of raw readl sites per file (mt7915 + core) ==="
for f in "$M"/mt7915/*.c "$M"/mmio.c "$M"/pci_util.c; do
	[ -f "$f" ] || continue
	n=$(grep -cE '[^_]readl\(' "$f" 2>/dev/null)
	r=$(grep -c 'mt76_readl(' "$f" 2>/dev/null)
	echo "$(basename "$f"): raw-readl=$n  mt76_readl=$r"
done
echo
echo "=== does the loaded module still export/call outer_io_lock? ==="
for ko in "$M"/mt7915/mt7915e.ko "$M"/mt76-core.ko; do
	[ -e "$ko" ] && echo "$(basename $ko): undefined outer_io=$(nm -u "$ko" 2>/dev/null | grep -c outer_io)"
done
