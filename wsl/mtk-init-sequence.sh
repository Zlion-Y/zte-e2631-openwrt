#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Mine the stock MTK driver (board/backup/mt7916.ko) for its firmware bring-up
# so we can compare it with the mt76 path that keeps hanging this board.
#
# What we want: what it does around "download ROM patch / WM / WA, then wait for
# the MCU to report ready" -- the symbols it defines, the strings it prints, and
# whether the ready signal is a register poll or an IPC/semaphore handshake.
set -u
KO=$E2631_WORK/device/backup/mt7916.ko
LOG=/tmp/mtk-init.out
exec >"$LOG" 2>&1

echo "== module identity"
modinfo "$KO"

echo "== bring-up related symbols it defines"
for pat in wifi_sys_init AsicFw InitFirmware LoadFirmware FwStart StartFirmware \
           waitWfdmaInit reset_ip_state MCU_INIT patch_sem PatchSem fwReady \
           WaStart WMStart PowerOnTranHW DomainReset hal_init asic_init; do
  n=$(nm --defined-only "$KO" | grep -ic "$pat")
  printf '  define %-22s %s\n' "$pat" "$n"
done

echo "== undefined symbols mentioning fw/patch/mcu/wfdma/calib"
nm --undefined-only "$KO" | grep -iE 'fw|patch|mcu|wfdma|eeprom|calib' | sort

echo "== image/handshake strings"
strings -a "$KO" | grep -iE 'rom_patch|RAM_CODE|WACPU|l1profile|DBDC_card|\.bin$' | sort -u

echo "== timeout / ready wording"
strings -a "$KO" | grep -iE 'timeout|wait.*fw|fw.*ready|init fail|download|WMMCU|WMCPU|TBL_init' | sort -u

echo "== probe/init entry points it defines"
nm --defined-only "$KO" | grep -E ' T ' | grep -iE 'pci_probe|register_device|hif|init_hw|mcu_start|fw_' | sort

echo MTK-INIT-DONE
