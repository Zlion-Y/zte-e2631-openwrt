#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Dry-run kernel patch 031 against the source the build currently uses, so a bad
# context line is caught before spending 40 minutes on `make world`.
set -u
TREE=$E2631_TREE
LOG=/tmp/patch31-check.out
exec >"$LOG" 2>&1
P=$E2631_WORK/build/overlay/target/linux/sanechips/patches-6.18/031-pci-gate-link-retrain-speed-lift-by-vendor-device.patch
K=""
for d in "$TREE"/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/drivers/pci/quirks.c" ] && { K=$d; break; }
done
[ -n "$K" ] || { echo "no kernel source in build_dir"; exit 1; }
echo "K=$K"
cd "$K" || exit 1
echo "== dry run"
patch -p1 --dry-run --fuzz=0 -i "$P"
echo "DRY-RC=$?"
echo "== also with default fuzz"
patch -p1 --dry-run -i "$P"
echo "FUZZ-RC=$?"
echo "== the function as it stands today"
n=0
s=0
while IFS= read -r l; do
  n=$((n + 1))
  case "$l" in *'pcie_failed_link_retrain(struct pci_dev *dev)'*) s=$n ;; esac
done < drivers/pci/quirks.c
echo "function starts at line $s"
i=0
while IFS= read -r l; do
  i=$((i + 1))
  if [ $i -ge $s ] && [ $i -le $((s + 34)) ]; then printf '%4d %s\n' "$i" "$l"; fi
done < drivers/pci/quirks.c
echo PATCH31-CHECK-DONE
