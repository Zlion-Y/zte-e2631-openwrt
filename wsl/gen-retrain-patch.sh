#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Generate kernel patch 031 from the source the build actually uses, so the
# context lines and tabs are exact. Inserts the upstream Vendor:Device gate into
# pcie_failed_link_retrain() so the ZTE root ports keep the Gen1 speed their
# bootloader trained (the vendor product only ever runs Gen1 on that link).
set -u
TREE=$E2631_TREE
LOG=/tmp/gen-retrain.out
exec >"$LOG" 2>&1
OUT=$E2631_WORK/build/overlay/target/linux/sanechips/patches-6.18/031-pci-gate-link-retrain-speed-lift-by-vendor-device.patch

K=""
for d in "$TREE"/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/drivers/pci/quirks.c" ] && { K=$d; break; }
done
[ -n "$K" ] || { echo "no kernel source"; exit 1; }
echo "K=$K"

W=/tmp/retrainpatch
rm -rf "$W"; mkdir -p "$W/a/drivers/pci" "$W/b/drivers/pci"
cp "$K/drivers/pci/quirks.c" "$W/a/drivers/pci/quirks.c"
cp "$K/drivers/pci/quirks.c" "$W/b/drivers/pci/quirks.c"

TAB=$'\t'
waiting=0
ins=0
rep=0
: > "$W/b/drivers/pci/quirks.new"
while IFS= read -r line; do
  case "$line" in
    *'pcie_failed_link_retrain(struct pci_dev *dev)'*)
      printf '%s\n' "$line" >> "$W/b/drivers/pci/quirks.new"
      waiting=1
      continue ;;
  esac
  if [ $waiting = 1 ] && [ "$line" = "{" ]; then
    printf '%s\n' "$line" >> "$W/b/drivers/pci/quirks.new"
    printf '%sstatic const struct pci_device_id ids[] = {\n' "$TAB" >> "$W/b/drivers/pci/quirks.new"
    printf '%s%s{ PCI_VDEVICE(ASMEDIA, 0x2824) }, /* ASMedia ASM2824 */\n' "$TAB" "$TAB" >> "$W/b/drivers/pci/quirks.new"
    printf '%s%s{}\n' "$TAB" "$TAB" >> "$W/b/drivers/pci/quirks.new"
    printf '%s};\n' "$TAB" >> "$W/b/drivers/pci/quirks.new"
    waiting=0
    ins=$((ins + 1))
    continue
  fi
  case "$line" in
    *'(lnkcap & PCI_EXP_LNKCAP_SLS) != PCI_EXP_LNKCAP_SLS_2_5GB) {'*)
      printf '%s\n' "${line%) \{*} &&" >> "$W/b/drivers/pci/quirks.new"
      printf '\t    pci_match_id(ids, dev)) {\n' >> "$W/b/drivers/pci/quirks.new"
      rep=$((rep + 1))
      continue ;;
  esac
  printf '%s\n' "$line" >> "$W/b/drivers/pci/quirks.new"
done < "$W/b/drivers/pci/quirks.c"
mv "$W/b/drivers/pci/quirks.new" "$W/b/drivers/pci/quirks.c"
echo "inserted-blocks=$ins replaced-conditions=$rep"
[ "$ins" = 1 ] && [ "$rep" = 1 ] || { echo "unexpected match count"; exit 1; }

cd "$W" || exit 1
diff -u a/drivers/pci/quirks.c b/drivers/pci/quirks.c > /tmp/retrain.patch
echo "diff rc=$?"
while IFS= read -r l; do printf '%s\n' "$l"; done < /tmp/retrain.patch

# prove it applies to the pristine copy
rm -rf /tmp/pt31; mkdir -p /tmp/pt31/drivers/pci
cp "$W/a/drivers/pci/quirks.c" /tmp/pt31/drivers/pci/quirks.c
cd /tmp/pt31 || exit 1
patch -p1 --fuzz=0 -i /tmp/retrain.patch > /dev/null 2>&1
echo "APPLY-RC=$?"
diff -q drivers/pci/quirks.c "$W/b/drivers/pci/quirks.c" && echo "APPLIED == GENERATED"

cp /tmp/retrain.patch "$OUT"
echo "written: $OUT"
echo GEN-RETRAIN-DONE
