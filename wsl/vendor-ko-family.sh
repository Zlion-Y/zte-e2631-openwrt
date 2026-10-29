#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Which driver family is the stock mt7916.ko? mt76 (upstream) or MTK's own
# proprietary HAP/HIF driver? It decides whose firmware-download flow we should
# be comparing against. Run: bash build/wsl/vendor-ko-family.sh
set -u
KO=$E2631_WORK/device/backup/mt7916.ko
ls -l "$KO"
echo "== vermagic / name"
strings -a "$KO" | grep -m3 -E '^vermagic=|^mt7916' || true
echo "== symbol side: which family of names does it define?"
nm -o --defined-only "$KO" 2>/dev/null | grep -c ' T ' || true
for pat in mt7915_eeprom_load mt76_register_device mt7915_mcu_send_message \
           wlan_start rtmp_pci_probe MT7915LoadFirmware wifi_sys_init \
           AsicFwInitCmd txbf_init mt7915_mac_init; do
  n=$(nm --defined-only "$KO" 2>/dev/null | grep -c "$pat" || true)
  echo "  defined $pat = $n"
done
echo "== firmware file requests baked in"
strings -a "$KO" | grep -E '\.bin$|/etc/wireless|l1profile|\.dat$' | sort -u | head -20
echo "== mt76-vs-mtk telltales in strings"
strings -a "$KO" | grep -icE 'mt76' 
strings -a "$KO" | grep -icE 'connac|HIF_USB|rt2860|APCLI|wlanop'
echo VENDOR-KO-FAMILY-DONE
