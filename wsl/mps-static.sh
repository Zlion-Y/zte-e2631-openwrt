#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Static evidence for the MPS/MRRS hypothesis. No device needed.
set -u
LOG=$E2631_WORK/out/mps-static.log
exec >"$LOG" 2>&1

echo "=== vendor mt7916.ko: which PCI core helpers does it call? ==="
KO=$E2631_WORK/device/backup/mt7916.ko
readelf -sW "$KO" 2>/dev/null | grep -iE 'pci|pcie' | head -60
echo "--- relocations mentioning mps/readrq/payload ---"
readelf -rW "$KO" 2>/dev/null | grep -iE 'mps|readrq|payload|pcie_|link_' | sort -u | head -40
echo "--- strings about mps/max-payload in vendor module ---"
strings -a "$KO" | grep -iE 'mps|payload|maxread|readrq' | head -20

echo
echo "=== our kernel source: locate probe.c ==="
K=$(ls -d $E2631_TREE/build_dir/target-*/linux-*/linux-*/ 2>/dev/null | head -1)
echo "K=$K"
if [ -n "$K" ]; then
  echo "--- pcie_bus_config default + parse ---"
  grep -n 'pcie_bus_config\|PCIE_BUS_DEFAULT\|PCI_BUS_TUNE' "$K/drivers/pci/probe.c" | head -40
  echo "--- pci_configure_mps() body ---"
  awk '/static void pci_configure_mps/,/^}/' "$K/drivers/pci/probe.c"
  echo "--- pcie_bus_configure_settings() body ---"
  awk '/^void pcie_bus_configure_settings/,/^}/' "$K/drivers/pci/probe.c"
  echo "--- pcie_get_... / set_... in include ---"
  grep -n 'PCIE_CAP_MAX_PAYLOAD\|pci_get_pcs... ' "$K/include/uapi/linux/pci_regs.h" | head -10
  grep -n 'PCI_EXP_DEVCTL_PAYLOAD\|PCI_EXP_DEVCTL_READRQ\|PCI_EXP_DEVCAP_PAYLOAD' "$K/include/uapi/linux/pci_regs.h"
  echo "--- does the DWC controller write DevCtl anywhere? ---"
  grep -rn 'PCI_EXP_DEVCTL\|pcie_set_mps\|pcie_get_mps' "$K/drivers/pci/controller/dwc/" | head -20
  echo "--- sanechips patches touching pci ---"
  ls $E2631_TREE/target/linux/sanechips/patches-6.18/ | grep -iE 'pci|dwc'
  echo "--- bootargs / cmdline in tree for our board ---"
  grep -rn 'bootargs\|append-rootarg\|CONFIG_CMDLINE' $E2631_TREE/target/linux/sanechips/ 2>/dev/null | head -20
  grep -n 'CONFIG_CMDLINE' "$K/.config" 2>/dev/null | head
fi

echo
echo "=== seed config: pcie-related symbols ==="
grep -iE 'PCIE|DWC' $E2631_KIT/seed/e2631.config 2>/dev/null | head -20
ls $E2631_KIT/seed/ 2>/dev/null
