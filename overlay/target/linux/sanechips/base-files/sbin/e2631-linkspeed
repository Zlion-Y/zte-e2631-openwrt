#!/bin/sh
# Report what each PCIe link actually negotiated (speed, width, and the root
# port's Target Link Speed), reading config space the way lspci does.
#
# Registers (include/uapi/linux/pci_regs.h): the PCIe capability sits at 0x70 on
# the ZTE root port (1cf2:6830) and at 0x80 on the MT7916 functions, so the
# offset has to be walked, not assumed. LnkSta is cap+0x12 (speed bits[3:0],
# width bits[9:4], DLLLA bit13), LnkCtl2 is cap+0x30 (TLS bits[3:0]).
#
# This tool used to have a "gen1" mode that wrote TLS and set Retrain Link. It
# was falsified twice over and it is gone on purpose:
#  - the vendor runs its main function at Gen1 x1 (LnkSta 0x1011) and the
#    firmware download still hangs, so speed was not the cause; the difference
#    came from our un-gated pcie_failed_link_retrain(), and kernel patch 031
#    stops that from firing, which is the clean way to sit at Gen1;
#  - doing it by hand at runtime dropped DLLLA from 1 to 0 and wedged the box,
#    and because the call had been put in a START=05 hook, every later boot hung
#    before ssh came up.
# Reading LnkSta is safe; changing link state on this board is not.
#
# Usage:  e2631-linkspeed                                  # both root ports
#         DEVS="/sys/bus/pci/devices/0000:01:00.0" e2631-linkspeed
set -u

DEVSYS=${DEVSYS:-/sys/bus/pci/devices}
DEVS=${DEVS:-"$DEVSYS/0000:00:00.0 $DEVSYS/0001:00:00.0"}

rdhex() { dd bs=1 skip="$2" count="$3" if="$1" 2>/dev/null | hexdump -v -e '1/1 "%02x"'; echo; }
# Little-endian numeric read straight from hexdump: "2/2 %d" consumes two bytes
# and decodes them in host byte order (little endian on this SoC), which avoids
# any ${var:0:2} substring syntax -- busybox ash does not have it.
rd16() { dd bs=1 skip="$2" count=2 if="$1" 2>/dev/null | hexdump -v -e '2/2 "%d"'; echo; }
capoff() { # <config> <capid> -> offset of the first capability with that id
	local p out=""
	p=$((0x$(rdhex "$1" 52 1) & 0xfc))
	while [ "$p" -ne 0 ] && [ "$p" -lt 248 ]; do
		[ "$(rdhex "$1" "$p" 1)" = "$(printf %02x $2)" ] && { echo $p; return 0; }
		p=$((0x$(rdhex "$1" $((p + 1)) 1)))
	done
	echo ""
}

for d in $DEVS; do
	cfg=$d/config
	[ -r "$cfg" ] || continue
	dev=${d##*/}
	c=$(capoff "$cfg" 16)
	[ -n "$c" ] || { echo "$dev: no PCIe cap"; continue; }
	lsta=$(rd16 "$cfg" $((c + 0x12)))
	lctl2=$(rd16 "$cfg" $((c + 0x30)))
	echo "$dev cap=0x$(printf %x $c) LnkSta=0x$(printf %04x $lsta) speed=$((lsta & 0xf)) width=$(((lsta >> 4) & 0x3f)) dllla=$(( (lsta >> 13) & 1 )) LnkCtl2=0x$(printf %04x $lctl2) TLS=$((lctl2 & 0xf))"
done
