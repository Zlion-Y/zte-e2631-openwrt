#!/bin/bash
. "$(dirname "$0")/../scripts/env.sh"
# Generate the mt76 patch that widens mt7915_firmware_state()'s wait for
# MT_TOP_MISC FW_STATE == FW_STATE_RDY from 1000 ms to a much longer value.
#
# Why: on this board the probe now consistently prints
#   WA Firmware Version ... then "Timeout for initializing firmware" (-EIO)
# and mcu.c:2106 is a bare `mt76_poll_msec(dev, MT_TOP_MISC, ..., 1000)`.
# A 256 MHz-ish Cortex-A9 behind a Gen2 x1 link is not obviously able to have
# the WACPU ready inside one second, and MTK's own driver waits far longer.
# If the chip only needs time, this turns a hard -EIO (and, on a cold chip, the
# stall we have been seeing) into a slow but successful init.
set -u
LOG=$E2631_WORK/out/gen-fwtimeout.log
: > "$LOG"
exec >>"$LOG" 2>&1

NEW=${NEW:-8000}
OLD=${OLD:-1000}
SRC=""
for d in $E2631_TREE/build_dir/target-*/linux-sanechips*/mt76-*/; do
  [ -f "$d/mt7915/mcu.c" ] && { SRC=$d; break; }
done
echo "SRC=$SRC  NEW=$NEW"
[ -n "$SRC" ] || { echo "no feed source"; exit 1; }

W=/tmp/fwtimeout
rm -rf "$W"; mkdir -p "$W/a/mt7915" "$W/b/mt7915"
cp "$SRC/mt7915/mcu.c" "$W/a/mt7915/mcu.c"
cp "$SRC/mt7915/mcu.c" "$W/b/mt7915/mcu.c"

# Rewrite only inside mt7915_firmware_state(): the "state, 1000))" argument.
hit=0
: > "$W/b/mt7915/mcu.new"
while IFS= read -r line; do
  case "$line" in
    *"$OLD))"*)
      printf '%s\n' "${line/$OLD/$NEW}" >> "$W/b/mt7915/mcu.new"
      hit=$((hit + 1))
      ;;
    *) printf '%s\n' "$line" >> "$W/b/mt7915/mcu.new" ;;
  esac
done < "$W/b/mt7915/mcu.c"
mv "$W/b/mt7915/mcu.new" "$W/b/mt7915/mcu.c"
echo "lines-changed=$hit"
[ "$hit" = 1 ] || { echo "expected exactly one match, got $hit"; exit 1; }

cd "$W" || exit 1
diff -u a/mt7915/mcu.c b/mt7915/mcu.c > /tmp/fwtimeout.patch
echo "diff rc=$?"
while IFS= read -r l; do printf '%s\n' "$l"; done < /tmp/fwtimeout.patch

# prove it applies against the pristine copy
mkdir -p /tmp/ptw/mt7915
cp "$W/a/mt7915/mcu.c" /tmp/ptw/mt7915/mcu.c
cd /tmp/ptw || exit 1
patch -p1 -i /tmp/fwtimeout.patch > /dev/null 2>&1
echo "apply rc=$?"
diff -q mt7915/mcu.c "$W/b/mt7915/mcu.c" && echo "APPLIED == GENERATED"

OUT=$E2631_WORK/build/overlay/package/kernel/mt76/patches
mkdir -p "$OUT"
cp /tmp/fwtimeout.patch "$OUT/003-mt7915-wait-longer-for-fw-state.patch"
echo "written: $OUT/003-mt7915-wait-longer-for-fw-state.patch"
ls -l "$OUT"
echo GEN-FWTIMEOUT-DONE
