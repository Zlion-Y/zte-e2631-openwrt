#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Does our host controller driver actually consume max-link-speed, and where is
# link speed really programmed?  Explains why a config-space TLS write did not
# downtrain the link, and decides whether the DT property is a viable lever.
set -u
LOG=$E2631_WORK/out/dwc-linkspeed.log
exec >"$LOG" 2>&1
K=$(ls -d $E2631_TREE/build_dir/target-*/linux-*/linux-*/ | head -1)
D="$K/drivers/pci/controller/dwc"
echo "=== who reads max-link-speed / link_gen ==="
grep -rIn "max-link-speed\|link_gen\|num_lanes" "$D/"*.c | head -20
echo
echo "=== the ZTE host driver file and what it programs at init ==="
ls "$D" | grep -i zte
for f in "$D"/pcie-zte*.c; do
	[ -e "$f" ] || continue
	echo "--- $f"
	grep -nE "GEN2_CTRL|LINK_WIDTH|PORT_LINK_CTRL|link_gen|max-link|speed|LTSSM|RETRAIN|start_link" "$f" | head -30
done
echo
echo "=== designware core: where is the appinit/link up sequence ==="
grep -nE "GEN2_CTRL|PORT_LINK_CTRL|LINK_WIDTH_SPEED|start_link_up|dw_pcie_link_up" "$D/pcie-designware.c" "$D/pcie-designware.h" | head -24
echo
echo "=== the PR patch that added the ZTE host: does it mention speed? ==="
grep -nE "GEN2_CTRL|link_gen|max-link-speed|PORT_LINK_CTRL|speed" $E2631_TREE/target/linux/sanechips/patches-6.18/016-PCI-dwc-Add-ZTE-ZX279128S-host-controller-driver.patch | head -20
echo
echo "=== our board DTS pcie nodes as shipped ==="
grep -n -B3 -A12 "pcie@f000000\|pcie@f1000000\|&pcie0\|&pcie1" $E2631_TREE/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts | head -40
