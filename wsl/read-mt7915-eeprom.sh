#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Read the mt7915 EEPROM load path from the tree we actually build, so the
# "avoid the efuse round-trip at boot" patch can be written against real code.
set -u
LOG=$E2631_WORK/out/mt7915-eeprom-src.log
exec >"$LOG" 2>&1
K=$(ls -d $E2631_TREE/build_dir/target-*/linux-*/linux-*/ 2>/dev/null | head -1)
echo "K=$K"
D="$K/drivers/net/wireless/mediatek/mt76/mt7915"
echo "=== files ==="
ls -l "$D"
echo
echo "=== eeprom.c: mt7915_eeprom_load / _init ==="
sed -n '1,120p' "$D/eeprom.c"
echo
echo "=== who calls eeprom_load, and the request_firmware names ==="
grep -rn "mt7915_eeprom_load\|MT7916_EEPROM_DEFAULT\|MT7915_EEPROM_DEFAULT\|request_firmware\|mt7915_mcu_get_eeprom" "$D"/*.c "$D"/*.h
echo
echo "=== mcu_get_eeprom / free_block: the MMIO/MCU round trip we want to skip ==="
awk '/int mt7915_mcu_get_eeprom\(/,/^}/' "$D/mcu.c"
awk '/int mt7915_mcu_get_eeprom_free_block\(/,/^}/' "$D/mcu.c"
echo
echo "=== is there already a firmware-first path (nvmem) in mt76 core? ==="
grep -rn "mt76_eeprom_override\|nvmem" "$K/drivers/net/wireless/mediatek/mt76/eeprom.c" | head -20
echo
echo "=== does the package Makefile apply patches from the target dir? ==="
ls $E2631_TREE/target/linux/sanechips/patches-6.18/ | tail -6
ls $E2631_KIT/overlay/../patches 2>/dev/null | head
grep -rn "mt76" $E2631_KIT/apply.sh | head -20
