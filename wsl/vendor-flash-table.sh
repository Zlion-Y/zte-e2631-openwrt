#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# What flash does the VENDOR kernel support?  The stock upgrade package carries a plaintext
# legacy uImage at 0x234 (verhdr kSize at 0x48 tells how long), which decompresses to a
# vmlinux whose string table still holds ZTE's own SPI-NAND table ("SPI NAND <part> <size> 3,3V").
#
#   bash wsl/vendor-flash-table.sh [path/to/vendor-firmware.bin]
#
# Why it matters: the vendor table is the BOM list -- every flash a shipping E2631 may carry.
# Our OpenWrt image only boots a chip whose ID is in the Linux side tables, so this is the list
# to compare against when someone asks "my E2631 has a different flash, will it take your image?".
export LC_ALL=C
# No filename baked in: this reads whichever official package you point it at -
# the same input as E2631_TEMPLATE.
PKG=${1:-${E2631_TEMPLATE:?usage: vendor-flash-table.sh <path/to/vendor-firmware.bin>  (or set E2631_TEMPLATE)}}
W=/tmp/vendor-kernel; rm -rf "$W"; mkdir -p "$W"
[ -f "$PKG" ] || { echo "no such package: $PKG" >&2; exit 1; }

KS=$(od -An -tu4 -j $((0x14 + 0x34)) -N4 "$PKG" | tr -d ' ')      # declared kernel size
echo "package $(stat -c %s "$PKG") B, verhdr kSize=$KS (0x$(printf %x "$KS"))"
# skip_bytes/count_bytes, never bs=1: a package on /mnt/d is a drvfs file, and bs=1 turns a
# 3.5 MiB extract into ~3.5 million single-byte reads that do not finish in minutes.
dd if="$PKG" of="$W/k.uImage" bs=1M iflag=skip_bytes,count_bytes skip=$((0x234)) count="$KS" 2>/dev/null
PAY=$(od -An -tu4 -j 12 -N4 --endian=big "$W/k.uImage" | tr -d ' ') # uImage payload size
dd if="$W/k.uImage" of="$W/k.zImage" bs=1M iflag=skip_bytes,count_bytes skip=64 count="$PAY" 2>/dev/null
OFF=$(grep -abo $'\x1f\x8b\x08' "$W/k.zImage" | head -1 | cut -d: -f1)
[ -n "${OFF:-}" ] || { echo "no gzip stream found in the zImage" >&2; exit 1; }
dd if="$W/k.zImage" of="$W/k.gz" bs=1M iflag=skip_bytes,count_bytes skip="$OFF" 2>/dev/null
gzip -dc "$W/k.gz" > "$W/vmlinux" 2>/dev/null
echo "vendor vmlinux: $(stat -c %s "$W/vmlinux") B (from zImage offset $OFF)"
echo

echo "== ZTE's SPI-NAND table entries (part / density) =="
strings -a "$W/vmlinux" | grep -aE '^SPI NAND ' | sort -u
echo
echo "== how it reports what it found, and its NAND driver =="
strings -a "$W/vmlinux" | grep -aE "device found, Manufacturer ID|No NAND device found|SPI-NAND:" | head -4
strings -a "$W/vmlinux" | grep -aE "zte,ZX279127-spifc|denali,denali-nand-dt|zx_denali_nand" | head -4
echo
echo "== compare with the Linux side of our own image =="
echo "  upstream 6.18 tables: winbond gigadevice micron macronix xtx foresee toshiba skyhigh ato alliancememory fmsh paragon esmt"
echo "  base tree backports add: ESMT F50L1G41LC, FudanMicro FM25S01BI3, HeYangTek HYF1GQ4UDACAE, FMSH FM25G01B/02B, Etron EM73D044VCx"
echo "  our board booted with:  $(grep -ah 'SPI NAND was found' $E2631_WORK/out/*.log 2>/dev/null | tail -1 | sed 's/^.*\] //')"
echo
echo "reminder: matching is by two-byte (manufacturer, device) ID, not by part number --"
echo "          a one-letter suffix difference is usually a different ID and therefore a new table entry."
