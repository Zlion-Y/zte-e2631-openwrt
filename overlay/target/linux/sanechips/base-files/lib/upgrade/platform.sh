#!/bin/sh
# sysupgrade platform glue for the E2631 slot image.
#
# Accepts the FACTORY-format package (the same .bin the vendor suprade.html
# page takes); the sysupgrade .tar this tree also builds is rejected, because
# a slot write must go through e2631-fwslot's bad-block-checked writer - the
# layout cspboot selects by CRC depends on fixed offsets inside the slot.
#
# Config preservation is DELIBERATELY download-based (2026-10-24 decision):
# nothing outside slot 1 is ever written by an update - no parking on the
# userdata partition, no restore. Users who want their settings back after an
# upgrade download the archive first (System -> Backup / Flash Firmware) and
# re-upload it on the new image. When userdata is initialized as extroot,
# everything on it (packages, configs) survives a slot rewrite untouched.

. /lib/functions.sh

# fwslot lives in /usr/sbin (package default), not /sbin: a wrong absolute
# path here fails INSIDE the sysupgrade ramfs, after services are already
# down - the 2026-10-24 first run flashed nothing and just rebooted.
E2631_FWSLOT="$(command -v e2631-fwslot 2>/dev/null)"
[ -n "$E2631_FWSLOT" ] || E2631_FWSLOT=/usr/sbin/e2631-fwslot

RAMFS_COPY_BIN="$E2631_FWSLOT"

# sha256 of the 16-byte outer magic every kit-packed package starts with
# (99999999 44444444 55555555 aaaaaaaa); hexdump/od do not exist in the
# on-device busybox, md5sum/sha256sum do
E2631_MAGIC_SHA=3d25b5b9bf4079c6760449611963b6f1

platform_check_image() {
	case "$1" in
	*.bin)
		if [ "$(dd if="$1" bs=16 count=1 2>/dev/null | sha256sum | cut -c1-32)" != "$E2631_MAGIC_SHA" ]; then
			echo "Invalid image: no E2631 factory magic"
			return 1
		fi
		if ! "$E2631_FWSLOT" check "$1" >/dev/null 2>&1; then
			echo "e2631-fwslot check rejected this image"
			return 1
		fi
		return 0
		;;
	*)
		echo "This board upgrades with the factory-format .bin (or: e2631-upgrade run)"
		return 1
		;;
	esac
}

platform_do_upgrade() {
	# write OUR slot (1) through the guarded writer; slot 0 - the vendor
	# system - is never a target. A failed write leaves the running image
	# intact and cspboot CRC-rejects a half-written slot on the next boot.
	if ! "$E2631_FWSLOT" flash "$1" --slot 1; then
		v "flash failed - the running image is unchanged"
		exit 1
	fi
}

platform_copy_config() {
	true
}
