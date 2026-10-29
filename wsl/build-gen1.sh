#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Rebuild the image with kernel patch 031 (gate the 2.5GT/s speed lift), which
# should leave the ZTE root ports at the Gen1 speed U-Boot trained -- the same
# speed the vendor stack runs at. Witnesses are printed after each phase so a
# green rc cannot hide an unapplied patch.
set -u
TREE=$E2631_TREE
LOG=$HOME/build-gen1.log
say() { echo "PHASE $(date +%T) $1 $2" | tee -a "$LOG"; }

count_match() { # <file> <substring>
  local n=0
  if [ -f "$1" ]; then
    while IFS= read -r l; do
      case "$l" in *"$2"*) n=$((n + 1)) ;; esac
    done < "$1"
  fi
  printf '%s' "$n"
}

say START ""
bash $E2631_KIT/wsl/sync.sh >>"$LOG" 2>&1
say SYNC "rc=$?"
( cd $E2631_KIT && bash apply.sh "$TREE" ) >>"$LOG" 2>&1
say APPLY "rc=$?"

ls -l "$TREE/target/linux/sanechips/patches-6.18/" >>"$LOG" 2>&1

# apply.sh only copies; it never deletes. Drop the 8 s timeout patch from the
# tree so this build differs from the one on the box by exactly ONE change
# (031), and refresh the mt76 package so the removal actually lands in build_dir.
rm -f "$TREE/package/kernel/mt76/patches/003-mt7915-wait-longer-for-fw-state.patch"
cd "$TREE" || exit 1
# "refresh" does NOT un-apply a patch that has been removed from patches/ -- the
# quilt state in build_dir keeps it applied. Clean first, then re-extract.
make package/kernel/mt76/clean >>"$LOG" 2>&1
say MT76_CLEAN "rc=$?"
make package/kernel/mt76/{download,prepare} >>"$LOG" 2>&1
say MT76_PREPARE "rc=$?"
S=""
for d in build_dir/target-*/linux-sanechips*/mt76-*/; do
  [ -f "$d/mt7915/mcu.c" ] && { S=$d; break; }
done
n=0
while IFS= read -r l; do
  case "$l" in *'state, 8000'*) n=$((n + 1)) ;; esac
done < "$S/mt7915/mcu.c"
say WITNESS "mcu.c-has-8000=$n (want 0)"
o=0
while IFS= read -r l; do
  case "$l" in *'state, 1000'*) o=$((o + 1)) ;; esac
done < "$S/mt7915/mcu.c"
say WITNESS "mcu.c-has-1000=$o (want 1)"
m=0
while IFS= read -r l; do
  case "$l" in *'E2631: the per-unit calibration'*) m=$((m + 1)) ;; esac
done < "$S/mt7915/eeprom.c"
say WITNESS "efuse-patch-present=$m (want 1)"

# /dev/mem has to survive into the built kernel config, not just the fragment.
say WITNESS "generic-devmem=$(count_match "$TREE/target/linux/generic/config-6.18" 'CONFIG_DEVMEM=y')"
say WITNESS "generic-io-strict-off=$(count_match "$TREE/target/linux/generic/config-6.18" '# CONFIG_IO_STRICT_DEVMEM is not set')"

cd "$TREE" || exit 1
make target/linux/clean >>"$LOG" 2>&1
say KERNEL_CLEAN "rc=$?"
make target/linux/prepare V=sc >>"$LOG" 2>&1
say KERNEL_PREPARE "rc=$?"

K=""
for d in build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/drivers/pci/quirks.c" ] && { K=$d; break; }
done
say KERNEL_SRC "$K"
if [ -n "$K" ]; then
  n=0
  while IFS= read -r l; do
    case "$l" in *'pci_match_id(ids, dev)'*) n=$((n + 1)) ;; esac
  done < "$K/drivers/pci/quirks.c"
  say WITNESS "match-id-occurrences=$n (want >=1)"
  m=0
  while IFS= read -r l; do
    case "$l" in *'ASMEDIA, 0x2824'*) m=$((m + 1)) ;; esac
  done < "$K/drivers/pci/quirks.c"
  say WITNESS "asmedia-list=$m (want 1)"
  r=$(find "$K/drivers/pci" -name '*.rej' 2>/dev/null | wc -l)
  say WITNESS "reject-files=$r (want 0)"
  say WITNESS "kernel-dotconfig-devmem=$(count_match "$K/.config" 'CONFIG_DEVMEM=y') (informational: the shipped image came out 0 even with both fragments asking for it)"
  say WITNESS "kernel-dotconfig-io-strict=$(count_match "$K/.config" 'CONFIG_IO_STRICT_DEVMEM=y') (want 0)"
fi

make -j"$(nproc)" world >>"$LOG" 2>&1
say WORLD "rc=$?"
# collect.sh takes the output directory as $1; called bare it dies with a usage
# error and leaves the Windows side without artifacts.
bash $E2631_KIT/wsl/collect.sh /tmp/gen1 >>"$LOG" 2>&1
say COLLECT "rc=$?"
rm -rf $E2631_WORK/out/gen1
mkdir -p $E2631_WORK/out/gen1
cp -r /tmp/gen1/. $E2631_WORK/out/gen1/ >>"$LOG" 2>&1
say PULL "rc=$?"
ls -l $E2631_WORK/out/gen1 >>"$LOG" 2>&1
say DONE ""
