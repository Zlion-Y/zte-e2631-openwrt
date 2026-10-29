#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Disassemble the vendor MTK driver's firmware bring-up functions and save the
# listing into the workspace, so the register protocol can be read offline.
#
# Targets from build/wsl/mtk-init-sequence.sh: chip_fw_init, StartFirmware,
# FwStart, LoadFirmware, hif_mcu_fw_init, WfHifHwInit,
# mt7906_hif_ctrl_chip_init, chip_wait_hif_dma_idle, chip_reset_hif_dma.
set -u
KO=$E2631_WORK/device/backup/mt7916.ko
OUTDIR=$E2631_WORK/out/mtk
mkdir -p "$OUTDIR"
FULL=/tmp/mtk.asm

OD=""
for c in $E2631_TREE/staging_dir/toolchain-*/bin/*-objdump; do
  [ -x "$c" ] && { OD=$c; break; }
done
[ -n "$OD" ] || { echo "no toolchain objdump"; exit 1; }
echo "OD=$OD"
"$OD" --version
"$OD" -d "$KO" > "$FULL" 2>&1
printf 'asm-lines='
wc -l < "$FULL"

pick() { # <symbol>
  local s=$1 f="$OUTDIR/func-$s.txt" n=0
  : > "$f"
  local on=0
  while IFS= read -r l; do
    case "$l" in
      *"<$s>:"*) on=1; printf '%s\n' "$l" >> "$f"; n=$((n + 1)); continue ;;
    esac
    if [ $on = 1 ]; then
      case "$l" in
        *"<"*">:"*) on=0 ;;
        *) printf '%s\n' "$l" >> "$f"; n=$((n + 1)) ;;
      esac
    fi
  done < "$FULL"
  printf '  %-32s lines=%s -> %s\n' "$s" "$n" "$f"
}

for s in chip_fw_init StartFirmware FwStart LoadFirmware hif_mcu_fw_init \
         WfHifHwInit mt7906_hif_ctrl_chip_init chip_wait_hif_dma_idle \
         chip_reset_hif_dma hif_dma_reset hif_mcu_init hif_kick_out_fwdl_msg; do
  pick "$s"
done
echo MTK-DISASM-DONE
