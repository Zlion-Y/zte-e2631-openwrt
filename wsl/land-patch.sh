#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Land the efuse patch for real, and prove it before spending 10 minutes on
# `make world`.  Two things bit us: sync.sh has no --delete (so a stale 001-
# twin of the patch survives renames on the Windows side), and apply.sh needs the
# tree path as $1.  Witnesses are printed after every step.
set -u
TREE=$E2631_TREE
echo "=== 1. sync with --delete, then apply.sh <tree> ==="
rsync -a --delete --exclude .git $E2631_WORK/build/ $E2631_KIT/
find $E2631_KIT -name '*.sh' -exec chmod +x {} +
ls -l $E2631_KIT/overlay/package/kernel/mt76/patches/
( cd $E2631_KIT && bash apply.sh "$TREE" > /tmp/apply.out 2>&1 ); echo "apply rc=$?"
tail -4 /tmp/apply.out
echo "--- tree patches now:"
ls -l "$TREE/package/kernel/mt76/patches/"

echo
echo "=== 2. clean + prepare the package, then look at the extracted source ==="
cd "$TREE" || exit 1
make package/kernel/mt76/clean >/tmp/mt76clean.out 2>&1; echo "clean rc=$?"
make package/kernel/mt76/{download,prepare} >/tmp/mt76prep.out 2>&1; echo "prepare rc=$?"
tail -6 /tmp/mt76prep.out
M=$(ls -d build_dir/target-*/linux-sanechips_generic/mt76-*/ 2>/dev/null | head -1)
echo "M=$M"
if [ -n "$M" ]; then
	echo "WITNESS patch-in-source=$(grep -c 'Try the file first' "$M/mt7915/eeprom.c" 2>/dev/null)"
	sed -n '/u32 eeprom_blk_size = MT7915_EEPROM_BLOCK_SIZE;/,+13p' "$M/mt7915/eeprom.c" 2>/dev/null
fi
echo
echo "=== 3. did anything delete the tree patches dir? ==="
ls -l "$TREE/package/kernel/mt76/patches/"
git -C "$TREE" status --porcelain package/kernel/mt76 | head
