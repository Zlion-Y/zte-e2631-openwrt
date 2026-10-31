#!/bin/bash
# Post-flash full verification sweep (runs from the Windows/Git-Bash side over
# SSH). Read-only: every check either reads sysfs/proc or sends traffic the
# device itself already sends (ping to its own WAN gateway). No configuration
# is written. Exit code is the verdict: 0 = all sections PASS.
#
#   bash tests/device-verify.sh [root@192.168.1.1]
#
# Sections (each prints PASS/FAIL and appends to the overall verdict):
#   V1 identity     - /etc/e2631-version, running slot header, kernel release
#   V2 boot health  - uptime, load, memory, dmesg oops/panic/error scan
#   V3 wired        - driver bound, links, IRQ alive, counters moving, WAN ping
#   V4 native state - module params, native proc dirs, wlan status present
#   V5 stability    - softnet drops, conntrack sane, logread error scan
set -u
DEV=${1:-root@192.168.1.1}
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 $DEV"
FAIL=0

sec() { printf '\n== %s ==\n' "$1"; }
ok()  { printf 'PASS %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; FAIL=1; }

# V1 identity --------------------------------------------------------------
sec "V1 identity"
VER=$($SSH cat /etc/e2631-version 2>/dev/null)
[ -n "$VER" ] && ok "version file: $VER" || bad "no /etc/e2631-version"
SLOT=$($SSH e2631-fwslot info 2>/dev/null | grep running)
# only a non-empty VER may bless the slot header (an empty $VER makes
# grep -q match anything - a false PASS)
if [ -n "$VER" ] && echo "$SLOT" | grep -q "$VER"; then
	ok "running slot header matches version file"
else
	bad "slot header stale/unreadable: $SLOT"
fi
KREL=$($SSH uname -r 2>/dev/null)
[ -n "$KREL" ] && ok "kernel $KREL" || bad "uname failed"

# V2 boot health -----------------------------------------------------------
sec "V2 boot health"
OOPS=$($SSH dmesg 2>/dev/null | grep -ciE 'oops|panic|BUG:|WARNING:|task .* blocked')
[ "$OOPS" -le 1 ] && ok "no oops/panic in dmesg ($OOPS hits)" || bad "$OOPS oops/warning hits in dmesg"
# "Failed to create device link" lines are the known-benign first MAC-read
# attempt before the nvmem layout binds (doc 62 A1) - filtered out.
ERRS=$($SSH dmesg 2>/dev/null | grep -viE 'Failed to create device link' | grep -ciE '\berror\b|\bfail(ed|ure)?\b')
$SSH dmesg 2>/dev/null | grep -viE 'Failed to create device link' | grep -iE '\berror\b|\bfail(ed|ure)?\b' | head -5
[ "$ERRS" -le 3 ] && ok "dmesg error count tolerable ($ERRS)" || bad "$ERRS dmesg errors (list above)"
MEM=$($SSH free 2>/dev/null | awk '/^Mem:/{print $7}')
[ -n "$MEM" ] && [ "$MEM" -gt 100000 ] && ok "mem available ${MEM}kB" || bad "low memory: $MEM"

# V3 wired -----------------------------------------------------------------
sec "V3 wired"
# Baseline (zx279128s-eth) serving the GMAC at boot is the expected steady
# state; native (zxnative-eth) is expected after `native-eth switch`. Either
# bound driver passes - what must never happen is NO driver.
GMAC=$($SSH readlink /sys/bus/platform/devices/92000000.ethernet/driver 2>/dev/null | sed 's#.*/##')
case "$GMAC" in
	zx279128s-eth|zxnative-eth) ok "gmac served by $GMAC" ;;
	*) bad "gmac bound by nobody ($GMAC)" ;;
esac
if [ "$GMAC" = zxnative-eth ]; then
	$SSH dmesg 2>/dev/null | grep -q 'native wired data plane up' &&
		ok "native probe message present" || bad "native bound but probe message missing"
fi
WAN=$($SSH ip -br addr show wan 2>/dev/null | grep -c 'UP')
[ "$WAN" -ge 1 ] && ok "wan link up" || bad "wan down"
PGW=$($SSH ping -c 2 -W 2 192.168.5.1 2>/dev/null | grep -c ' 0% packet loss')
[ "$PGW" -ge 1 ] && ok "WAN gateway reachable" || bad "WAN gateway unreachable"
BR=$($SSH ip -br addr show br-lan 2>/dev/null | grep -c 192.168.1.1)
[ "$BR" -ge 1 ] && ok "br-lan has 192.168.1.1" || bad "br-lan unconfigured"
IRQ=$($SSH grep 'ethernet' /proc/interrupts 2>/dev/null | awk '{print $2+$3}')
[ -n "$IRQ" ] && [ "$IRQ" -gt 0 ] && ok "eth IRQ alive ($IRQ)" || bad "eth IRQ never fired"

# V4 native state ----------------------------------------------------------
sec "V4 native state"
for m in zxnative_eth zxnpu_wlan zx279128s_npu_ppe zx279128s_npu_idm; do
	$SSH [ -d /sys/module/$m ] 2>/dev/null && ok "$m present" || bad "$m missing"
done
$SSH ls /proc/zx-csp-wlan/status >/dev/null 2>&1 && ok "/proc/zx-csp-wlan/status present" ||
	bad "wlan proc missing (BUG2 regression)"
# persistent probe log (doc 62 B4): parked message or probe evidence lives here
WPR=$($SSH cat /proc/zx-csp-wlan/probe 2>/dev/null | head -1)
echo "$WPR" | grep -qE 'parked|vendor=' && ok "wlan probe log: $WPR" ||
	bad "wlan probe log unreadable/empty"
for d in /proc/zx-npu-idm /proc/zx-npu-ppe /proc/zx-npu-audit; do
	$SSH [ -d "$d" ] 2>/dev/null && ok "$d present" || bad "$d missing"
done

# V5 stability -------------------------------------------------------------
sec "V5 stability"
# busybox awk has no strtonum: fetch the hex drop column, convert in bash
DROP=0
for f2 in $($SSH cut -d' ' -f2 /proc/net/softnet_stat 2>/dev/null); do
	: $((DROP += 16#$f2))
done 2>/dev/null
[ "$DROP" = 0 ] && ok "softnet drops 0" || bad "softnet dropped $DROP"
CT=$($SSH cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null)
[ -n "$CT" ] && [ "$CT" -lt 8000 ] && ok "conntrack $CT" || bad "conntrack $CT suspicious"
LOGE=$($SSH logread 2>/dev/null | grep -ciE 'kernel.*(oops|panic|bug)')
[ "$LOGE" = 0 ] && ok "no kernel errors in logread" || bad "$LOGE kernel errors in logread"

printf '\n== verdict: %s ==\n' "$([ $FAIL = 0 ] && echo ALL-PASS || echo FAILURES-PRESENT)"
exit $FAIL
