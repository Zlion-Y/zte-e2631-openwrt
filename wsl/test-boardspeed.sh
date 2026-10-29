#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Offline check of board/linkspeed.sh (was device/) against config space pulled from the live
# board (out/fake-sys/*.config, produced by node on the Windows side because NTFS
# forbids ':' in names and WSL has no node).  Known-good decode: both ZTE root
# ports have the PCIe cap at 0x70 and LnkSta=0x3012 => Gen2 x1, DLLLA=1.
set -u
W=$E2631_WORK
T=/tmp/lsys
rm -rf $T; mkdir -p $T
for f in $W/out/fake-sys/*.config; do
	base=$(basename "$f" .config)
	name=$(echo "$base" | tr '_' ':')
	mkdir -p "$T/$name"
	cp "$f" "$T/$name/config"
	echo "fixture $name ($(stat -c %s "$T/$name/config") bytes)"
done
sed 's/\r$//' $W/build/board/linkspeed.sh > $T/linkspeed.sh
/bin/busybox ash -n $T/linkspeed.sh && echo "ash syntax OK" || { echo "ASH FAIL"; exit 1; }

echo "--- report (expect speed=2 width=1 TLS as enumerated) ---"
DEVSYS=$T /bin/busybox ash $T/linkspeed.sh
echo "--- DEVS override: name one root port explicitly ---"
DEVS="$T/0000:00:00.0" /bin/busybox ash $T/linkspeed.sh
echo "--- the script must not write config space any more ---"
# It used to have a "gen1" mode that set TLS and Retrain Link; doing that by hand
# dropped DLLLA and wedged the box, and patch 031 makes the retrain never fire in
# the first place. Assert the mutation is gone rather than testing it.
md5_before=$(cd $T && find . -name config | sort | xargs md5sum | md5sum)
DEVSYS=$T /bin/busybox ash $T/linkspeed.sh >/dev/null
DEVSYS=$T /bin/busybox ash $T/linkspeed.sh gen1 >/dev/null
md5_after=$(cd $T && find . -name config | sort | xargs md5sum | md5sum)
if [ "$md5_before" = "$md5_after" ]; then
	echo "PASS: config space unchanged (still $md5_after)"
else
	echo "FAIL: the fixture was written to"
	exit 1
fi
grep -q 'conv=notrunc' $T/linkspeed.sh && { echo "FAIL: a write path is still in the script"; exit 1; }
echo "PASS: no write path left in the script"
