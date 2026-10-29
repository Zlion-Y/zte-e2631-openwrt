#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Collect a finished local build into a directory that
#   node build/host/release.mjs --dir <this dir>   (run from the workspace root)
# accepts. The file names mirror the CI workflow's "collect the artifacts"
# step on purpose: both verifiers key off kernel / root / e2631.dtb /
# config-<version>, so a locally built image gets screened exactly like an
# artifact from Actions.
set -e
SRC="$E2631_TREE"
OUTD="${1:?usage: collect.sh <out-dir>}"
B="$SRC/bin/targets/sanechips/generic"

ls "$B"/*.bin >/dev/null 2>&1 || { echo "no images in $B yet" >&2; exit 1; }
mkdir -p "$OUTD/payload"
rm -f "$OUTD"/*.bin "$OUTD"/*.manifest "$OUTD"/config-* 2>/dev/null || true
cp -t "$OUTD" "$B"/*.bin "$B"/*.manifest 2>/dev/null || true
cp "$B"/config-* "$OUTD"/ 2>/dev/null || true

# The board .dtb on its own, so boot readiness is checked against the structure
# the kernel actually sees rather than the .dts source.
DTB=$(find "$SRC/build_dir" -maxdepth 6 -name 'image-zx279128s-zte-zxhn-e2631.dtb' | head -1)
test -n "$DTB" || { echo "no built board dtb found" >&2; exit 1; }
cp "$DTB" "$OUTD/e2631.dtb"

# The kernel's merged .config. bin/targets carries no config-* in this tree and
# the top-level .config hides the symbols only the target config file sets -
# which is precisely what the boot-readiness screen needs to see.
KV=$(sed -n 's/^KERNEL_PATCHVER:=\([0-9][0-9.]*\).*/\1/p' "$SRC/target/linux/sanechips/Makefile")
KCFG=$(find "$SRC/build_dir" -maxdepth 5 -name .config 2>/dev/null |
	grep -E '/linux-[^/]+/linux-[^/]+/\.config$' | head -1)
if [ -n "$KCFG" ] && [ -n "$KV" ]; then
	cp "$KCFG" "$OUTD/config-$KV"
	echo "kernel config: -> config-$KV ($(wc -l < "$OUTD/config-$KV") lines)"
else
	echo "WARN no kernel .config: release.mjs will refuse to pack" >&2
fi

# The sysupgrade file is a tar with the two payloads under a per-device
# directory; unpacking here gives the stable names kernel/root.
SX=$(ls "$B"/openwrt-*-sysupgrade*.bin | head -1)
test -n "$SX" || { echo "no sysupgrade image to unpack" >&2; exit 1; }
tar -xf "$SX" -C "$OUTD/payload"
find "$OUTD/payload" -maxdepth 3 -type f -printf '  %s\t%p\n'

# The slot tool also has to run on the *vendor* system, so ship the bare
# cross-compiled ELF, statically linked and soft-float.
ELF=$(find "$SRC/build_dir" -type f -name e2631-fwslot -not -path '*ipk*' | head -1)
if [ -n "$ELF" ]; then
	cp "$ELF" "$OUTD/payload/e2631-fwslot"
	file "$OUTD/payload/e2631-fwslot"
	# readelf prints "Flags:  0x5000200, Version5 EABI, soft-float ABI"; take the
	# number alone, since bash arithmetic chokes on the trailing comma.
	F=$(readelf -h "$ELF" | sed -n 's/^ *Flags: *\(0x[0-9a-fA-F]*\).*/\1/p')
	[ $(( F & 0x200 )) -ne 0 ] && [ $(( F & 0x400 )) -eq 0 ] ||
		echo "WARN e2631-fwslot flags=$F is not soft-float EABI5" >&2
else
	echo "WARN no e2631-fwslot ELF in build_dir" >&2
fi
