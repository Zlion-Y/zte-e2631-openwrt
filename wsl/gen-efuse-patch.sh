#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Generate the mt76 feed patch that makes mt7915 load its calibration file
# BEFORE touching the (blank) on-chip efuse.
#
# Done as a generated diff rather than a hand-written patch because the context
# lines have to match the source we actually build (mt76-2026.10.08~467ae2e3,
# which is what produces kmod-mt7915e -- not the in-tree kernel copy).
set -u
LOG=$E2631_WORK/out/gen-efuse-patch.log
exec >"$LOG" 2>&1
set -x

SRC=$(ls -d $E2631_TREE/build_dir/target-*/linux-sanechips_generic/mt76-*/ 2>/dev/null | head -1)
echo "SRC=$SRC"
[ -f "$SRC/mt7915/eeprom.c" ] || { echo "no feed source"; exit 1; }

W=/tmp/efusepatch
rm -rf "$W"; mkdir -p "$W/a/drivers" "$W/b/drivers"
# keep the a/b layout patch(1) expects for -p1 under the package source root
mkdir -p "$W/a/mt7915" "$W/b/mt7915"
cp "$SRC/mt7915/eeprom.c" "$W/a/mt7915/eeprom.c"
cp "$SRC/mt7915/eeprom.c" "$W/b/mt7915/eeprom.c"

awk '
{ print }
/u32 eeprom_blk_size = MT7915_EEPROM_BLOCK_SIZE;/ {
	print ""
	print "\t\t/* E2631: the per-unit calibration of this board lives as a file"
	print "\t\t * inside the jffs2 \"wifi\" partition, so neither of_node nor"
	print "\t\t * nvmem can hand it to mt76_eeprom_init() and we land here with"
	print "\t\t * ret == 0. The efuse on the chip is blank, so everything read"
	print "\t\t * below is thrown away and the default bin is used anyway -- but"
	print "\t\t * the dozen MCU round trips to discover that are exactly where"
	print "\t\t * probe intermittently stalls on this SoC. Try the file first."
	print "\t\t */"
	print "\t\tif (!mt7915_eeprom_load_default(dev) && !mt7915_check_eeprom(dev))"
	print "\t\t\treturn 0;"
}
' "$W/b/mt7915/eeprom.c" > "$W/b/mt7915/eeprom.new"
mv "$W/b/mt7915/eeprom.new" "$W/b/mt7915/eeprom.c"

cd "$W" || exit 1
diff -u a/mt7915/eeprom.c b/mt7915/eeprom.c > /tmp/efuse.patch
echo "diff rc=$?"
sed -n '1,40p' /tmp/efuse.patch

# prove it applies against the pristine source
cp "$W/a/mt7915/eeprom.c" /tmp/check.c
patch --dry-run -p1 -i /tmp/efuse.patch < /dev/null 2>&1 | head -3
cd /tmp && mkdir -p pt && cd pt && rm -rf mt7915 && cp /tmp/check.c /dev/null 2>/dev/null
mkdir -p mt7915 && cp "$W/a/mt7915/eeprom.c" mt7915/eeprom.c
patch -p1 -i /tmp/efuse.patch >/dev/null 2>&1; echo "apply rc=$?"
diff -q mt7915/eeprom.c "$W/b/mt7915/eeprom.c" && echo "APPLIED PATCH == GENERATED FILE"

OUT=$E2631_WORK/build/overlay/package/kernel/mt76/patches
mkdir -p "$OUT"
cp /tmp/efuse.patch "$OUT/001-mt7915-use-caldata-file-before-efuse.patch"
chmod +x "$OUT/001-mt7915-use-caldata-file-before-efuse.patch" 2>/dev/null
echo "written: $OUT/001-mt7915-use-caldata-file-before-efuse.patch"
ls -l "$OUT"
