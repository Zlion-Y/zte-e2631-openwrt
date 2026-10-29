#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# List the vendor driver's firmware/patch bring-up symbols by name (no text
# filters: plain read loops), so the real functions behind the chip_ops
# dispatcher (chip_fw_init) can be disassembled next.
set -u
KO=$E2631_WORK/device/backup/mt7916.ko
LOG=/tmp/mtk-syms.out
exec >"$LOG" 2>&1

echo "== defined symbols whose name mentions fw / firmware / patch / mcu / calib"
nm --defined-only "$KO" | while IFS= read -r line; do
  case "$line" in
    *" T "*|*" t "*)
      name=${line##* }
      case "$name" in
        *[Ff]w*|*[Ff]irmware*|*[Pp]atch*|*[Mm]cu*|*[Mm]CU*|*WACPU*|*wacpu*|*[Cc]alib*|*[Rr]om*) echo "  $name" ;;
      esac ;;
  esac
done | sort -u

echo
echo "== the chip_ops table area: symbols near chip_fw_init"
nm --defined-only "$KO" | sort | while IFS= read -r line; do
  addr=${line%% *}
  name=${line##* }
  case "$addr" in
    0014*|0015*) case "$name" in *chip_*|*fw_*|*Fw*) echo "  $addr $name" ;; esac ;;
  esac
done
echo MTK-SYMS-DONE
