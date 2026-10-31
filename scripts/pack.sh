#!/bin/bash
# The one pack step, shared by scripts/build.sh and manual Windows-side runs
# (node lives off the WSL box, so the 3.x flow packs after build.sh --no-pack).
#
#   bash scripts/pack.sh --dir out/build-YYYYMMDD-HHMMSS [--out out/pkg.bin]
#
# The slot-header version always comes from the overlay version file: a stale
# E2631_VERSION in .env or the environment once shipped a native build whose
# slot header read "ffe-full-0.6" (release.mjs copies the 512-byte template
# header verbatim unless --version is passed). Do not stamp by hand.
set -eu
cd "$(dirname "$0")/.."
. scripts/env.sh

say() { printf 'pack: %s\n' "$*"; }
die() { printf 'pack: %s\n' "$*" >&2; exit 1; }

DIR=
OUT=
while [ $# -gt 0 ]; do
	case "$1" in
	--dir) DIR=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	*) die "unknown argument: $1 (usage: pack.sh --dir <build dir> [--out f])" ;;
	esac
done
[ -n "$DIR" ] || {
	# Newest build-* by mtime. A hardcoded date in the caller's glob once
	# kept matching the previous day's tree across midnight, so a "3.29"
	# package shipped a 3.28 rootfs (sixteen flashes chased a write bug
	# that was never there). Defaulting to the newest tree removes the
	# chance to make that mistake by hand.
	DIR=$(ls -dt "$E2631_OUT"/build-* 2>/dev/null | head -1)
	[ -n "$DIR" ] || die "no --dir given and no build-* under $E2631_OUT"
	say "no --dir given; packing the newest tree: $(basename "$DIR")"
}
case "$DIR" in /*) ;; *) DIR="$PWD/$DIR" ;; esac
[ -d "$DIR/payload" ] || die "$DIR holds no payload - pass the build dir collect.sh wrote"

[ -n "$E2631_TEMPLATE" ] || die "no version-header template and no default either.
       env.sh supplies seed/vendor-verhdr-8b8.bin, which ships in this repository -
       unset E2631_TEMPLATE to get it back, or point it at an official package
       (BUILD.md, 'the template')."
case "$E2631_TEMPLATE" in ""|/*) ;; *) E2631_TEMPLATE="$E2631_WORK/$E2631_TEMPLATE" ;; esac
[ -f "$E2631_TEMPLATE" ] || die "E2631_TEMPLATE=$E2631_TEMPLATE does not exist.
       If you did not move it, the shipped header slice is missing from your clone:
       'git checkout -- seed/vendor-verhdr-8b8.bin'. BUILD.md, 'the template', covers
       cutting your own from an official package."

e2631_need_cmd node "pack the image into the vendor upgrade format" || exit 1
if ! node -e 'const z=require("node:zlib");if(typeof z.crc32!=="function")process.exit(1)' 2>/dev/null; then
	die "node $(node -v) has no zlib.crc32 - the packer needs Node 20.15+ (22 LTS recommended)"
fi

# The single version source: the overlay file the image itself carries.
E2631_VERFILE="$E2631_KIT/overlay/target/linux/sanechips/base-files/etc/e2631-version"
[ -s "$E2631_VERFILE" ] || die "no version file at $E2631_VERFILE - the overlay is incomplete"
PKG_VERSION=$(cat "$E2631_VERFILE")
case "$PKG_VERSION" in
	OpenWrt\ *) ;;
	"") die "version file is empty: $E2631_VERFILE" ;;
	*) PKG_VERSION="OpenWrt $PKG_VERSION" ;;
esac
say "stamping slot header: $PKG_VERSION"

OUT=${OUT:-$E2631_OUT/e2631-openwrt-$(basename "$DIR").bin}
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
say "packing with template $(basename "$E2631_TEMPLATE")"
node host/release.mjs --dir "$DIR" --template "$E2631_TEMPLATE" \
	--version "$PKG_VERSION" --out "$OUT"
say "package: $OUT"
