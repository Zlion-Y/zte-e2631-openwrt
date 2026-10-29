#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Does the base tree's IO-lock patch cover the register reads mt7915 itself
# does (mt7915_rr / __mt7915_rr), or only mt76 core's?  If mt7915's own reads
# still go straight to readl(), then every MCU doorbell/poll on this board is
# an unlocked PCIe device read -- exactly the documented L2C-310 deadlock.
set -u
LOG=$E2631_WORK/out/iolock-coverage.log
exec >"$LOG" 2>&1

P=$E2631_TREE/package/kernel/mt76/patches/001-wifi-mt76-serialize-register-reads-with-outer-cache.patch
echo "=== patch exists: $(ls -l "$P" 2>/dev/null | wc -l) ==="
echo "=== files it touches ==="
grep -E '^(---|\+\+\+) ' "$P"
echo
echo "=== the helper it adds (wherever it is defined) ==="
awk '/^\+\+\+ /{f=$2} /mt76_readl|outer_io_lock/{print f": "$0}' "$P" | head -25
echo
echo "=== mt7915's own read helpers in the pristine source (from git, build_dir may be mid-refresh) ==="
for SRC in $(ls -d $E2631_TREE/build_dir/target-*/linux-sanechips_generic/mt76-*/ 2>/dev/null) $E2631_TREE/package/kernel/mt76/src; do
	echo "--- SRC=$SRC"
	[ -d "$SRC" ] || continue
	grep -n "mt76_readl\|readl(" "$SRC/mt7915/mmio.c" 2>/dev/null | head -12
done
echo
echo "=== is there a tarball/source we can inspect instead? ==="
ls -l $E2631_TREE/dl/mt76* 2>/dev/null | head -3
ls -d $E2631_TREE/build_dir/target-*/linux-sanechips_generic/mt76-*/ 2>/dev/null | head -2
echo
echo "=== build progress ==="
grep -h '^PHASE' $HOME/build.log 2>/dev/null | tail -6
pgrep -a -f '[m]ake' | head -3
