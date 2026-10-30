#!/bin/bash
# Offline verification of wlanmgr's subcommand -> ctl-command mapping.
# Runs the script against a stub control file (WLANMGR_CTL override) and
# compares what it writes. Verifies doc 36 sequences without a device.
# Note: run() overwrites the stub with each write (matches `echo > ctl`),
# so multi-command sequences are checked by their LAST write plus a static
# script-content assertion.
set -u
cd "$(dirname "$0")/.."
WL=overlay/target/linux/sanechips/base-files/sbin/wlanmgr
TMP=$(mktemp)
FAIL=0

check() { # check <subcommand> <expected ctl command>
	local sub=$1 want=$2 got
	rm -f "$TMP"
	WLANMGR_CTL="$TMP" sh "$WL" "$sub" >/dev/null 2>&1
	got=$(head -1 "$TMP" 2>/dev/null)
	if [ "$got" = "$want" ]; then
		echo "  ok: $sub -> '$want'"
	else
		echo "  FAIL: $sub -> got '$got', want '$want'"
		FAIL=1
	fi
}

echo "wlanmgr mapping test:"
check status  status
check expect  ack-expect
check l1p     l1profile
check rings   rings
check plane   dataplane
check recv    mcu-recv
check drecv   data-recv
check fwstate fw-state
check probe   probe

echo "wlanmgr commit forwarding test:"
rm -f "$TMP"
WLANMGR_CTL="$TMP" sh "$WL" commit data-init >/dev/null 2>&1
got=$(head -1 "$TMP" 2>/dev/null)
if [ "$got" = "data-init" ]; then
	echo "  ok: commit data-init -> 'data-init'"
else
	echo "  FAIL: commit data-init -> got '$got'"
	FAIL=1
fi

echo "wlanmgr step2 sequence test (needs commit=1 stub):"
mkdir -p /tmp/wl-stub/sys/module/zxnpu_wlan/parameters
echo 1 > /tmp/wl-stub/sys/module/zxnpu_wlan/parameters/commit
rm -f "$TMP"
WLANMGR_CTL="$TMP" WLANMGR_SYS=/tmp/wl-stub/sys sh "$WL" step2 >/dev/null 2>&1
last=$(tail -1 "$TMP" 2>/dev/null)
seqok=$(sed -n '/^\tstep2)/,/^\t;;/p' "$WL" | grep -cE 'mcu-dl|fw-dl wm|fw-dl wa|fw-state')
if [ "$last" = "fw-state" ] && [ "$seqok" -ge 4 ]; then
	echo "  ok: step2 ends at fw-state, 4-step chain present in script"
else
	echo "  FAIL: step2 last='$last' seqok=$seqok"
	FAIL=1
fi

echo "wlanmgr step3 sequence test:"
rm -f "$TMP"
WLANMGR_CTL="$TMP" WLANMGR_SYS=/tmp/wl-stub/sys sh "$WL" step3 >/dev/null 2>&1
last=$(tail -1 "$TMP" 2>/dev/null)
if [ "$last" = "dataplane" ]; then
	echo "  ok: step3 ends at dataplane"
else
	echo "  FAIL: step3 last='$last'"
	FAIL=1
fi
rm -rf /tmp/wl-stub "$TMP"
exit $FAIL
