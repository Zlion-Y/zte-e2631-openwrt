#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Kill any build left over from the script I launched by mistake, then prove at
# the binary level that the kernel we are about to ship really carries the
# Vendor:Device gate inside pcie_failed_link_retrain().
set -u
LOG=/tmp/gen1-verify.out
exec >"$LOG" 2>&1

for p in $(pgrep -f 'make .*package/kernel' ; pgrep -f 'build-efusefix' ; pgrep -f 'make -j'); do
  kill "$p" 2>/dev/null && echo "killed $p"
done
sleep 3
echo "== make procs left"
pgrep -a -f 'make ' || echo none

TREE=$E2631_TREE
K=""
for d in "$TREE"/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/vmlinux" ] && { K=$d; break; }
done
echo "K=$K"
OD=""
for c in "$TREE"/staging_dir/toolchain-*/bin/*objdump; do
  [ -x "$c" ] && { OD=$c; break; }
done
echo "OD=$OD"

echo "== disassembly of pcie_failed_link_retrain in the BUILT vmlinux"
"$OD" -d --disassemble=pcie_failed_link_retrain "$K/vmlinux"

echo "== does it branch to pci_match_id? (the gate)"
"$OD" -d --disassemble=pcie_failed_link_retrain "$K/vmlinux" > /tmp/gen1.fn.asm
n=0
while IFS= read -r l; do
  case "$l" in
    *'pci_match_id'*) n=$((n + 1)); echo "  HIT $l" ;;
  esac
done < /tmp/gen1.fn.asm
echo "WITNESS call-pci_match_id=$n (want >=1)"
m=0
while IFS= read -r l; do
  case "$l" in
    *'removing 2.5GT/s'*) m=$((m + 1)) ;;
  esac
done < /tmp/gen1.fn.asm
echo "note: the info string lives in .rodata, count here=$m"

echo "== the two images we ship"
ls -l --time-style=+%H:%M:%S "$TREE/bin/targets/sanechips/generic/" | while IFS= read -r l; do echo "  $l"; done
echo GEN1-VERIFY-DONE
