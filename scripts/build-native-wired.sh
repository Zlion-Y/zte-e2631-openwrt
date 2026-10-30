#!/bin/bash
# native-wired-1.3: P2 switchover build. 021 is removed; the new
# zxnative-eth driver owns the switch/GMAC/DMA node. MDIO/PHY (018/019)
# and the SoC infrastructure stay. Nothing is flashed.
set -uo pipefail
BASE=${E2631_WORK:-$(cd "$(dirname "$0")/.." && pwd)}
TREE=${E2631_TREE:-/home/builder/openwrt}
LOG=$BASE/out/build-native-wired-1.0.log
: > "$LOG"
cd "$BASE/native" || exit 1
echo "PHASE apply" | tee -a "$LOG"
bash ./apply.sh "$TREE" >>"$LOG" 2>&1 || { echo "APPLY_FAILED" | tee -a "$LOG"; exit 1; }

P=$TREE/target/linux/sanechips/patches-6.18
echo "PHASE switch to native-only wiring" | tee -a "$LOG"
rm -f "$P"/021-net-ethernet-zte-Add-ZX279128S-Ethernet-switch-drive.patch
rm -f "$P"/042-zte-add-the-ZX279128S-NPU-wiring.patch
rm -f "$P"/043-zte-add-the-ZX279128S-native-wired-driver.patch
cp "$BASE/overlay/target/linux/sanechips/patches-6.18/044-zte-add-the-ZX279128S-native-only-wiring.patch" "$P"/ >>"$LOG" 2>&1
echo "OpenWrt native-wired-1.0" > "$TREE/target/linux/sanechips/base-files/etc/e2631-version"

CFG=$TREE/target/linux/sanechips/config-6.18
sed -i '/^CONFIG_ZX279128S_ETH=/d; /^# E2631: wired hardware layer/d; /^# E2631: native wired data plane/d' "$CFG" >>"$LOG" 2>&1
grep -q '^CONFIG_ZXNATIVE_ETH=y$' "$CFG" || echo 'CONFIG_ZXNATIVE_ETH=y' >> "$CFG"
sed -i 's|^CONFIG_CMDLINE="\(.*\)"$|CONFIG_CMDLINE="\1 zxnative_eth.enable=1"|' "$CFG" >>"$LOG" 2>&1
{
  echo "witness: ZXNATIVE lines"
  grep -n 'ZXNATIVE\|ZX279128S_ETH' "$CFG"
  echo "witness: patches 044 present, 021/042/043 absent"
  ls "$P"/044-* 2>&1
  ls "$P"/021-* "$P"/042-* "$P"/043-* 2>&1
  echo "witness: cmdline"
  grep -n 'CONFIG_CMDLINE=' "$CFG"
} >>"$LOG" 2>&1

echo "PHASE clean" | tee -a "$LOG"
cd "$TREE" || exit 1
make target/linux/clean >>"$LOG" 2>&1
echo "PHASE prepare" | tee -a "$LOG"
make target/linux/prepare V=sc >>"$LOG" 2>&1 || { echo "PREPARE_FAILED" | tee -a "$LOG"; exit 1; }
K=$TREE/build_dir/target-arm_cortex-a9_musl_eabi/linux-sanechips_generic/linux-6.18.55
{
  echo "witness: prepared zte Kconfig/Makefile"
  grep -n 'ZXNATIVE_ETH\|ZX279128S_ETH' "$K/drivers/net/ethernet/zte/Kconfig" "$K/drivers/net/ethernet/zte/Makefile"
  ls -l "$K/drivers/net/ethernet/zte/zxnative-eth.c" "$K/drivers/net/ethernet/zte/zx279128s-main.c" 2>&1
} >>"$LOG" 2>&1

echo "PHASE build" | tee -a "$LOG"
cd "$BASE" || exit 1
bash scripts/build.sh --no-pack >>"$LOG" 2>&1
echo "BUILD_RC=$?" | tee -a "$LOG"
