#!/bin/bash
# build-latest.sh — the per-round loop: build, pack, flash, verify.
#
#   bash tests/build-latest.sh              # full loop
#   bash tests/build-latest.sh --no-flash   # stop after the package
#
# Why this exists. The round commands used to pack with
#   pack.sh --dir $(ls -dt ../out/build-20261031-* | head -1)
# and after the date rolled over that glob kept matching the previous day's
# tree. A "3.29" package then shipped a 3.28 rootfs while the slot header (read
# from the overlay version file) said 3.29 - sixteen flashes looked like a
# write problem, but the package had simply never been rebuilt. This script
# takes the newest build-* by mtime and refuses to pack when the rootfs version
# inside it disagrees with the overlay.
set -eu
cd "$(dirname "$0")/.."
. scripts/env.sh

say() { printf 'build-latest: %s\n' "$*"; }
die() { printf 'build-latest: %s\n' "$*" >&2; exit 1; }

DO_FLASH=1
[ "${1:-}" = "--no-flash" ] && DO_FLASH=0

VER=$(cat overlay/target/linux/sanechips/base-files/etc/e2631-version)
SHORT=${VER#OpenWrt native-wired-}
say "overlay version: $VER"

BUILD_SH=${E2631_BUILD_SH:-$E2631_WORK/_scratch/npu-probe/build-native-wired-3.26.sh}
[ -f "$BUILD_SH" ] || die "no build script at $BUILD_SH (set E2631_BUILD_SH)"
say "building via $(basename "$BUILD_SH")"
bash "$BUILD_SH" || die "build failed"

BDIR=$(ls -dt "$E2631_OUT"/build-* 2>/dev/null | head -1)
[ -n "$BDIR" ] || die "no build-* directory under $E2631_OUT"
[ -d "$BDIR/payload" ] || die "$BDIR has no payload (collect.sh step missing)"
say "newest build dir: $BDIR"

ROOT=$(find "$BDIR/payload" -maxdepth 2 -name root -type f | head -1)
[ -n "$ROOT" ] || die "no payload root under $BDIR"
GOT=$(unsquashfs -cat "$ROOT" etc/e2631-version 2>/dev/null | tr -d '\n' || true)
[ "$GOT" = "$VER" ] || die "stale build directory: its rootfs says '$GOT', the overlay says '$VER'.
       The tree was not regenerated - read the build log before packing."
say "rootfs version matches the overlay: $GOT"

OUT="$E2631_OUT/native-wired-$SHORT/e2631-openwrt-native-wired-$SHORT.bin"
mkdir -p "$(dirname "$OUT")"
bash scripts/pack.sh --dir "$BDIR" --out "$OUT"

[ "$DO_FLASH" = 1 ] || { say "--no-flash: package at $OUT"; exit 0; }

FLASH=$(ls -t tests/flash-*.sh 2>/dev/null | head -1)
[ -n "$FLASH" ] || die "no tests/flash-*.sh to drive the device"
say "flashing via $(basename "$FLASH")"
bash "$FLASH"
