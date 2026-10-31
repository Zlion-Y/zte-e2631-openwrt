#!/bin/bash
# The supported one-command path: base tree -> image -> flashable package.
#
#   bash scripts/build.sh                # or: make build
#   bash scripts/build.sh --no-pack      # stop at the image, pack by hand
#
# Requires scripts/bootstrap.sh to have run. Everything it writes goes under
# $E2631_OUT (default: <workspace>/out), and the log file is named with the start
# time so a failed run can be compared with the one before it.
#
# This is the same phase order the local WSL path uses (wsl/build-gen1.sh), with
# the pieces that only exist because of this machine's network left out. Set
# E2631_SEGMENTED=1 if downloads crawl there too - see BUILD.md, "the network".
set -eu
cd "$(dirname "$0")/.."
. scripts/env.sh

say() { printf 'build: %s\n' "$*"; }
die() { printf 'build: %s\n' "$*" >&2; exit 1; }

NOPACK=0
[ "${1:-}" = "--no-pack" ] && NOPACK=1

# ---- preconditions, stated before the forty-minute job starts --------------
[ -d "$E2631_TREE/target/linux/sanechips" ] ||
	die "no base tree at $E2631_TREE - run: bash scripts/bootstrap.sh"
[ -f "$E2631_TREE/.config" ] ||
	die "no .config in $E2631_TREE - run: bash scripts/bootstrap.sh"
grep -q 'Makefile.e2631' "$E2631_TREE/target/linux/sanechips/image/Makefile" ||
	die "the overlay is not applied to $E2631_TREE - run: bash scripts/bootstrap.sh"
grep -q '^CONFIG_TARGET_DEVICE_sanechips_generic_DEVICE_zte_zxhn-e2631=y' "$E2631_TREE/.config" ||
	die "the E2631 device is not selected in $E2631_TREE/.config - the seed did not land"
e2631_need_cmd make "run the build" || exit 1

STAMP=$(date +%Y%m%d-%H%M%S)
BUILDDIR="$E2631_OUT/build-$STAMP"
LOG="$BUILDDIR/world.log"
mkdir -p "$BUILDDIR"

export CURL_OPTIONS=${CURL_OPTIONS:---speed-limit 20000 --speed-time 20}
MAKEVAR=
if [ "${E2631_SEGMENTED:-0}" = 1 ]; then
	[ -x "$E2631_KIT/wsl/segdl.sh" ] || die "E2631_SEGMENTED=1 but $E2631_KIT/wsl/segdl.sh is not executable"
	# include/download.mk re-exports DOWNLOAD_TOOL_CUSTOM from .config, so the env
	# variable alone gets clobbered; a command-line make assignment survives.
	MAKEVAR="CONFIG_DOWNLOAD_TOOL_CUSTOM=$E2631_KIT/wsl/segdl.sh"
	say "segmented downloads on ($MAKEVAR)"
fi

say "make world  (log: $LOG)"
set +e
( cd "$E2631_TREE" && make -j"$(nproc)" world V=s $MAKEVAR ) >"$LOG" 2>&1
rc=$?
set -e
if [ "$rc" != 0 ]; then
	say "FAILED rc=$rc - the last errors in $LOG:"
	grep -E "^(make.*\*\*\*|.*Error [0-9])" "$LOG" | tail -8
	tail -20 "$LOG"
	die "see $LOG"
fi
say "make world done"

# ---- collect, then pack ----------------------------------------------------
bash "$E2631_KIT/wsl/collect.sh" "$BUILDDIR"
say "images collected into $BUILDDIR"

[ -f "$BUILDDIR/payload/sysupgrade-zte_zxhn-e2631/kernel" ] ||
	[ -n "$(find "$BUILDDIR/payload" -name kernel -print -quit)" ] ||
	die "the collected directory has no kernel payload - look at what $BUILDDIR/payload holds"

if [ "$NOPACK" = 1 ]; then
	say "--no-pack: packing left to you"
	say "  bash scripts/pack.sh --dir $BUILDDIR --out $E2631_OUT/e2631-openwrt-$STAMP.bin"
	exit 0
fi

OUT="$E2631_OUT/e2631-openwrt-$STAMP.bin"
# pack.sh owns template resolution, the node checks and the slot-header
# version stamping (single source: the overlay e2631-version file).
bash scripts/pack.sh --dir "$BUILDDIR" --out "$OUT"
say "package: $OUT"
