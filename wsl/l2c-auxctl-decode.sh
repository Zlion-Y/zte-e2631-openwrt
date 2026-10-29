#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Decode the stock PL310 AUX_CTRL value (0x76430001, quoted by the PR's own DTS
# comment) against the bit definitions in OUR kernel tree, and show which DT
# properties our tree turns on. Run: bash build/wsl/l2c-auxctl-decode.sh
set -u
K=""
for d in $E2631_KIT/openwrt/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -d "$d" ] && { K=$d; break; }
done
[ -n "$K" ] || { echo "no kernel build dir"; exit 1; }
echo "K=$K"
echo "== l2x0.h: AUX_CTRL bit defines"
grep -rnE "L2X0_AUX_CTRL|AUX_CTRL_" "$K/include/soc/pxa/l2x0.h" "$K/arch/arm/include" 2>/dev/null
echo "== cache-l2x0.c: DT property -> aux bit mapping"
grep -nE "prefetch|double-linefill|shared-override|write-validate|no-outer-write-allocate|l2x0_of_parse|aux" "$K/arch/arm/mm/cache-l2x0.c"
echo L2C-DECODE-DONE
