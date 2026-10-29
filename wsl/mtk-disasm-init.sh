#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Disassemble the vendor MTK driver's firmware bring-up so we can read the actual
# register protocol it uses to get the WACPU to "ready", and compare it with the
# mt76 path that keeps hanging this board.
#
# Targets found by build/wsl/mtk-init-sequence.sh:
#   chip_fw_init, StartFirmware/FwStart, LoadFirmware, hif_mcu_fw_init,
#   WfHifHwInit, mt7906_hif_ctrl_chip_init, chip_reset_hif_dma, chip_wait_hif_dma_idle
set -u
KO=$E2631_WORK/device/backup/mt7916.ko
LOG=/tmp/mtk-disasm.out
exec >"$LOG" 2>&1

# any cross-objdump will decode ARM; we only need immediates and bl targets
OD=""
for c in arm-none-eabi-objdump arm-linux-gnueabi-objdump objdump /usr/bin/objdump; do
  command -v "$c" >/dev/null 2>&1 && { OD=$c; break; }
done
[ -n "$OD" ] || { echo "no objdump available"; exit 1; }
echo "OD=$OD"

"$OD" -d "$KO" > /tmp/mtk.asm 2>&1 || { echo "disassembly failed"; exit 1; }
wc -l < /tmp/mtk.asm

for sym in chip_fw_init StartFirmware FwStart LoadFirmware hif_mcu_fw_init \
           WfHifHwInit mt7906_hif_ctrl_chip_init chip_reset_hif_dma \
           chip_wait_hif_dma_idle WfHifSysInit hif_dma_reset hif_mcu_init; do
  echo
  echo "######## $sym"
  awk -v s="$sym" '
    index($0, "<"s">:") { p=1; print; next }
    p==1 {
      if ($0 ~ /^[0-9a-f]+ </) { p=0 }
      else print
    }
  ' /tmp/mtk.asm
done

echo
echo "######## immediates that look like MT7916 register offsets in those windows"
awk '
  /mov.*#[0-9]/ || /orr.*0x18/ { print }
' /tmp/mtk.asm | grep -iE '0x18|0x80|1114|0x460' | sort -u

echo MTK-DISASM-DONE
