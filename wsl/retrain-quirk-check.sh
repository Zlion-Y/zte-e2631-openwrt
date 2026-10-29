#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Our booted kernel prints "removing 2.5GT/s downstream link speed restriction"
# for the ZTE root port (1cf2:6830), yet upstream 6.18 gates that on a
# Vendor:Device list containing only ASMedia. Find out which code actually runs
# in OUR build, and whether a target patch touches it.
set -u
TREE=$E2631_TREE
LOG=/tmp/retrain.out
exec >"$LOG" 2>&1

K=""
for d in "$TREE"/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/drivers/pci/quirks.c" ] && { K=$d; break; }
done
[ -n "$K" ] || { echo "no kernel build dir"; exit 1; }
echo "K=$K"

echo '== the ids[] list in OUR quirks.c'
n=0
while IFS= read -r l; do
  n=$((n + 1))
  case "$l" in
    *'pcie_failed_link_retrain'*) echo "  line $n: $l"; s=$n ;;
  esac
done < "$K/drivers/pci/quirks.c"
i=0
while IFS= read -r l; do
  i=$((i + 1))
  if [ $i -gt $s ] && [ $i -le $((s + 45)) ]; then printf '%4d %s\n' "$i" "$l"; fi
done < "$K/drivers/pci/quirks.c"

echo '== does any target/local patch touch retrain or LNKCTL2?'
for p in "$TREE"/target/linux/sanechips/patches-*/ "$TREE"/target/linux/generic/backport-*/ "$TREE"/target/linux/generic/pending-*/; do
  [ -d "$p" ] || continue
  for f in "$p"*.patch; do
    [ -f "$f" ] || continue
    case "$f" in
      *pcie*|*pci*|*link*|*retrain*) printf '  maybe: %s\n' "$f" ;;
    esac
  done
done

echo '== grep-ish scan of every patch for the restriction string'
for f in "$TREE"/target/linux/*/patches-*/*.patch "$TREE"/target/linux/generic/*/*.patch; do
  [ -f "$f" ] || continue
  while IFS= read -r l; do
    case "$l" in
      *'2.5GT/s downstream'*|*'pcie_failed_link_retrain'*|*'SANECHIPS'*) printf '  %s :: %s\n' "${f##*/}" "$l" ;;
    esac
  done < "$f"
done
echo RETRAIN-DONE
