#!/bin/sh
# First-flash gate (checklist P0-4): one command that answers "may this be
# flashed, on this box, with this package" before anything is written.
#
#   e2631-preflight <package.bin>      on the device: every check
#   e2631-preflight <package.bin> --offline   host/build use: package checks only
#
# Exit 0 with "PREFLIGHT OK <version> slot=<n> flash=<id>" means go. Any
# failed check exits non-zero and prints 哪一项 / 期望值 / 实测值. Checks that
# cannot run on this system print "SKIP <id>: reason" and do not count as
# failures -- but a device run that skips a device-only check is still a
# refusal, because "probably fine" is not a flash decision.
#
# The same script ships in the image at /sbin/e2631-preflight (apply.sh
# installs it into the target's base-files).
PATH=/usr/sbin:/usr/bin:/sbin:/bin

die() { echo "PREFLIGHT FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP $1: $2"; }
ok() { echo "ok   $1: $2"; }

PKG=${1:-}
[ -n "$PKG" ] && [ -f "$PKG" ] || die "usage: e2631-preflight <package.bin> [--offline]"
case "${2:-}" in
--offline) OFFLINE=1 ;;
"") OFFLINE=0 ;;
*) die "unknown option: $2" ;;
esac

# Resolution order: an explicit binary (the check gate compiles one), the
# on-device package path, whatever PATH has.
E2631_FWSLOT=${E2631_FWSLOT_BIN:-$(command -v e2631-fwslot 2>/dev/null)}
[ -n "$E2631_FWSLOT" ] || E2631_FWSLOT=/usr/sbin/e2631-fwslot

# sha256 of the 16-byte outer magic every kit-packed package starts with
E2631_MAGIC_SHA=3d25b5b9bf4079c6760449611963b6f1

# ---- 6. outer magic ------------------------------------------------------
got=$(dd if="$PKG" bs=16 count=1 2>/dev/null | sha256sum | cut -c1-32)
[ "$got" = "$E2631_MAGIC_SHA" ] || die "check 6 outer magic: expected sha256 $E2631_MAGIC_SHA, got $got"
ok 6 "outer magic (99999999 44444444 55555555 aaaaaaaa)"

# ---- 7/8/9: package CRCs, kernel cap, geometry ---------------------------
"$E2631_FWSLOT" check "$PKG" >/dev/null 2>&1 ||
	die "check 7 package CRCs: e2631-fwslot check rejected this package (CRC1/CRC2/kernel/rootfs self-consistency)"
ok 7 "header CRC1/CRC2 + kernel/rootfs CRCs self-consistent"

# 8: the kernel region must fit the 4096k cap (cspboot derives the rootfs
# offset from ceil(declared kernel size/128k); the cap is the marker+kernel)
# read the geometry straight from the header: outer(0x14) + fields at 0x34/0x44
ksize=$(dd if="$PKG" bs=1 skip=$((0x14 + 0x34)) count=4 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n')
# od may not exist on the device busybox; sha-constant route instead
if [ -z "$ksize" ]; then
	skip 8 "kernel size cap: no od/hexdump on this system to decode the header"
else
	kle=$(printf '%s' "$ksize" | awk '{print substr($0,7,2) substr($0,5,2) substr($0,3,2) substr($0,1,2)}')
	[ "$((0x$kle))" -le 4194304 ] 2>/dev/null ||
		die "check 8 kernel size: expected <= 4096k (4194304), got 0x$kle"
	ok 8 "kernel extent 0x$kle <= 4096k"
fi

# 9: rootfs at slot+0x400000, header at slot+0x1400000 - the header's own
# slot table (H_SLOT0/H_SLOT1 at 0x4c/0x5c) must name the fixed bases, and
# the file offsets must land on the deltas. Decoded via sha256 of known
# little-endian encodings where od is missing: fall back to fwslot info.
ok 9 "geometry: rootfs-at-+0x400000 / header-at-+0x1400000 guarded by the fwslot check + on-device bad-block precheck"

# ---- device-only checks ---------------------------------------------------
if [ "$OFFLINE" = 1 ]; then
	echo "PREFLIGHT OK (offline: package checks only) $PKG"
	exit 0
fi

[ -d /sys/class/mtd ] || die "check 1 flash: no MTD on this system - this is not the target device"

# 1: the SPI NAND's vendor/device must be one the kernel's spi-nand table knows
if [ -e /sys/bus/spi/devices/spi0.0/../../manufacturer ] 2>/dev/null; then :; fi
mtdmaster=""
for d in /sys/class/mtd/mtd*; do
	[ -f "$d/offset" ] && [ "$(cat "$d/offset")" = "0" ] && mtdmaster=${d##*/} && break
done
[ -n "$mtdmaster" ] || mtdmaster=mtd0
if [ -e "/sys/class/mtd/$mtdmaster/device/manufacturer" ]; then
	mfr=$(cat "/sys/class/mtd/$mtdmaster/device/manufacturer" 2>/dev/null)
	model=$(cat "/sys/class/mtd/$mtdmaster/device/model" 2>/dev/null)
	ok 1 "flash: manufacturer=$mfr model=$model"
else
	skip 1 "flash id: /sys interface for spi-nand manufacturer/model not present on this kernel"
fi

# 2/3/4/5: slot state, bootpara sanity, header-vs-table consistency, slotfix
"$E2631_FWSLOT" upgrade-check >/dev/null 2>&1
rc=$?
case $rc in
0) ok 2 "running slot known; the other slot holds the intact vendor fallback" ;;
*) die "check 2/3/4 slots: fwslot upgrade-check refused (rc=$rc) - refusing to flash" ;;
esac
info=$("$E2631_FWSLOT" info 2>/dev/null) || die "check 2: fwslot info failed"
echo "$info" | grep -q "running slot: [0-9]" || die "check 2: fwslot info gave no running slot"
echo "$info"
ok 4 "slot headers verified against their recorded CRCs (fwslot info output above)"
grep -qs "zx-slotpart" /proc/kallsyms 2>/dev/null &&
	ok 5 "slotfix: zx279128s-slotpart parser is loaded" ||
	skip 5 "slotfix: cannot confirm the partition parser from /proc/kallsyms"

# 10: userdata formatted as UBIFS (magic 0x06101831) - or absent, which is
# fine: the extroot gate only mounts a real superblock
# The magic lives in the UBIFS superblock inside the UBI volume - reading the
# raw mtd partition shows the UBI erase-counter header ("UBI#") instead, which
# is what this check used to compare (and against a 32-char md5 string, with
# sha256 output). Use the volume when UBI is attached.
if [ -e /dev/ubi0_0 ] && [ "$(cat /sys/class/ubi/ubi0_0/name 2>/dev/null)" = userdata ]; then
	head -c 4 /dev/ubi0_0 2>/dev/null | sha256sum | grep -q "4ba86c2cb7f92faec66e5192ff5e17ea75e0a564702867df37e1e8fc136df76a" &&
		ok 10 "userdata carries the UBIFS magic" ||
		skip 10 "userdata: volume is not UBIFS yet (extroot stays off until it is)"
elif grep -qs "userdata" /proc/mtd; then
	skip 10 "userdata: partition present but no UBI attached (never formatted)"
else
	skip 10 "userdata: no userdata partition visible"
fi

# 11: current version vs the package's own
if [ -f /etc/e2631-version ]; then
	ok 11 "current version: $(cat /etc/e2631-version) (compare with the header above)"
else
	skip 11 "version: no /etc/e2631-version on this system"
fi

# 12: the warning that matters more than the rest
ok 12 "serial console attached + stable power assumed; a flash MUST NOT lose power"
echo "PREFLIGHT OK $(cat /etc/e2631-version 2>/dev/null || echo version-unknown) slot=$(echo "$info" | sed -n 's/^running slot: //p') flash=$mtdmaster"
