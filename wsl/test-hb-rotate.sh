#!/bin/bash
# Offline test for the heartbeat log bound (the shipped script had no cap at all: two
# boots of uptime left 1610 lines / 69 KB on an 8 MiB jffs2 overlay). Runs the real
# sbin/e2631-hb for one beat against a synthetic log and checks that it keeps the
# newest lines - including the tail of the previous boot, which is the whole point.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
HB="$REPO/overlay/target/linux/sanechips/base-files/sbin/e2631-hb"
T=/tmp/hbrotate
rm -rf "$T"; mkdir -p "$T"
cp "$HB" "$T/hb"; sed -i 's/\r$//' "$T/hb"

LOG="$T/hb.log"
# 700 lines, the last 3 of them marked as the dying boot's tail
i=1
while [ "$i" -le 700 ]; do echo "HB up=$i irq=1 sta=0 aer=0 ld=0.1" >> "$LOG"; i=$((i + 1)); done
for k in DEADTAIL-1 DEADTAIL-2 DEADTAIL-3; do echo "KM $k" >> "$LOG"; done
BEFORE=$(wc -l < "$LOG")

LIMIT=600 LOGF="$LOG" timeout 2 sh "$T/hb" 1 >/dev/null 2>&1
AFTER=$(wc -l < "$LOG")

echo "lines before=$BEFORE after=$AFTER (limit 600 + marker + beats)"
if [ "$AFTER" -gt "$BEFORE" ]; then
	echo "FAIL: the log grew past its limit"; exit 1
fi
if grep -c DEADTAIL "$LOG" | grep -qv '^3$'; then
	echo "FAIL: the tail of the previous boot was lost"; exit 1
fi
echo "PASS: bounded to $AFTER lines and the previous boot's tail survived"
grep -n 'hb opened' "$LOG" | sed 's/^/  marker at line /'
echo "HB-ROTATE-DONE"
