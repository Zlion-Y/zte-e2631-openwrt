#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Build the FW_STATE probe module against the exact kernel we ship, so it loads
# into the running system with no reflash.
set -u
TREE=$E2631_TREE
SRC=$E2631_WORK/out/regprobe
LOG=/tmp/regprobe-build.out
exec >"$LOG" 2>&1

K=""
for d in "$TREE"/build_dir/target-*/linux-sanechips*/linux-*/; do
  [ -f "$d/Makefile" ] && [ -f "$d/Module.symvers" ] && { K=$d; break; }
done
[ -n "$K" ] || { echo "no prepared kernel with Module.symvers"; exit 1; }
echo "K=$K"

CC=""
# The kernel records its own compiler prefix (arm-openwrt-linux-muslgnueabi-gcc);
# matching it is what keeps modpost from complaining about a different compiler.
for c in "$TREE"/staging_dir/toolchain-*/bin/*muslgnueabi-gcc "$TREE"/staging_dir/toolchain-*/bin/*-gcc; do
  [ -x "$c" ] && { CC=$c; break; }
done
[ -n "$CC" ] || { echo "no cross gcc"; exit 1; }
CROSS=${CC%gcc}
echo "CC=$CC"
echo "CROSS=$CROSS"

W=/tmp/regprobe-build
rm -rf "$W"; mkdir -p "$W"
cp "$SRC/regprobe.c" "$SRC/Makefile" "$W/"
make -C "$K" M="$W" ARCH=arm CROSS_COMPILE="$CROSS" modules
echo "MODULE-RC=$?"
ls -l "$W"/*.ko
[ -f "$W/regprobe.ko" ] || { echo "no module produced"; exit 1; }

mkdir -p "$SRC"
cp -f "$W/regprobe.ko" "$SRC/regprobe.ko"
printf 'size='
wc -c < "$SRC/regprobe.ko"
printf 'vermagic='
strings -a "$SRC/regprobe.ko" | while IFS= read -r l; do
  case "$l" in vermagic*) echo "$l" ;; esac
done
sha256sum "$SRC/regprobe.ko"
echo "== undefined symbols the loader must satisfy"
nm -u "$SRC/regprobe.ko"
echo REGBUILD-DONE
