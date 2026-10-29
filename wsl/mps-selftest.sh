#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Offline self-test for board/mpscfg.sh: run the same ash logic against the
# vendor's captured config space, whose expected decode we know by hand:
#   DevCap=0x10008fe1 (256 B max) DevCtl=0x2910 (MPS 128, MRRS 512, RO/ExtTag/SERR)
#   DevSta=0x0000 LnkSta=0x1011 (Gen1 x1)
set -u
W=$E2631_WORK
T=/tmp/mpstest
rm -rf "$T"; mkdir -p "$T/0000:01:00.0" "$T/0001:01:00.0"
cp "$W/out/vendor-ep0-config.bin" "$T/0000:01:00.0/config"
cp "$W/out/vendor-ep1-config.bin" "$T/0001:01:00.0/config"
chmod u+w "$T"/*/config
cp "$W/build/board/mpscfg.sh" "$T/mpscfg.sh"
sed -i 's/\r$//' "$T/mpscfg.sh"

if command -v ash >/dev/null 2>&1; then SB="ash"
elif [ -x /bin/busybox ]; then SB="/bin/busybox ash"
elif command -v busybox >/dev/null 2>&1; then SB="busybox ash"
elif command -v dash >/dev/null 2>&1; then SB="dash"
else SB="bash --posix"; fi
echo "### shell: $SB   ($($SB -c 'echo $0' 2>/dev/null))"

run() { DEVSYS="$T" $SB "$T/mpscfg.sh" "$@" </dev/null; }

echo "### read pass (expect MPS=128 MRRS=512 RO=1 ExtTag=1 SERR=1, gen1 width1)"
run read 2>&1 | sed -n '1,60p'
echo
echo "### write 0x2910 (idempotent) then 0x0020 (MPS 256) then back"
run write 0x2910 2>&1 | grep -E 'DevCtl now'
run write 0x0020 2>&1 | grep -E 'DevCtl now'
echo "-- decode after writing 0x0020:"
run read 2>&1 | grep -E '^\s+DevCtl='
echo "-- bytes on disk: $(xxd -s 0x88 -l 2 -p "$T/0000:01:00.0/config")  (expect 2000)"
