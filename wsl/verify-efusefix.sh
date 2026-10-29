#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# mt7915_eeprom_load() is inlined, so key the witness off the function that
# actually ships: within mt7915_eeprom_init's body, the call that loads the
# calibration FILE must be relocated BEFORE the call that reads the efuse.
set -u
LOG=$E2631_WORK/out/efusefix-verify.log
exec >"$LOG" 2>&1
SRC=$E2631_TREE
M=$(ls -d $SRC/build_dir/target-*/linux-sanechips_generic/mt76-*/ | head -1)
OD=$(ls $SRC/staging_dir/toolchain-*/bin/*-objdump | head -1)
for F in "$M/mt7915/eeprom.o" "$SRC/build_dir/target-*/root-sanechips/lib/modules/6.18.55/mt7915e.ko"; do
	F=$(ls $F 2>/dev/null | head -1)
	[ -n "$F" ] || { echo "missing $F"; continue; }
	echo "=== $F ==="
	nm "$F" | grep -iE ' t | T ' | grep -i "eeprom_init\|eeprom_load" | head -5
	body=$(awk '/<mt7915_eeprom_init>:/,/^$/' <<<"$("$OD" -dr "$F" 2>/dev/null)")
	d=$(echo "$body" | grep -n "mt7915_eeprom_load_default" | head -1 | cut -d: -f1)
	fb=$(echo "$body" | grep -n "mt7915_mcu_get_eeprom_free_block" | head -1 | cut -d: -f1)
	echo "  first load_default at body-line ${d:-none}, first free_block at ${fb:-none}"
	if [ -n "$d" ] && [ -n "$fb" ]; then
		[ "$d" -lt "$fb" ] && echo "  PASS: file-load precedes the efuse probe" || echo "  FAIL: efuse still first"
	else
		echo "  INCONCLUSIVE"
	fi
	echo "$body" | sed -n 's/.*R_ARM_[A-Z0-9_]* *[0-9a-f]* *\(mt7915_[a-z_]*\|mt76_[a-z_]*\).*/  call \1/p' | head -12
done
