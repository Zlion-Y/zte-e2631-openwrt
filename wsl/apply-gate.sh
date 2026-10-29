#!/bin/bash
# P1-1 gate: prove apply.sh fails fast and self-checks. Four parts, all cheap:
#   1. it is idempotent on the real tree (report + dirty-path count stable);
#   2. a wrong pinned base SHA refuses before touching anything;
#   3. the structural assertions pass on a correct minimal skeleton tree;
#   4. four constructed defects each make it refuse.
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO/scripts/env.sh"
REAL=${E2631_TREE:?E2631_TREE must point at the base tree}
OUT=${E2631_OUT:?E2631_OUT must be set}
# A Windows-side make check has no base tree (it lives in WSL): the other
# gates SKIP with a stated reason, so does this one.
if [ ! -d "$REAL/target/linux/sanechips" ]; then
	echo "SKIP apply.sh gate: no base tree at $REAL (run it in WSL, where E2631_TREE points at the checkout)"
	exit 77
fi

bash "$REPO/apply.sh" "$REAL" >/dev/null 2>&1 || { echo "FAIL: apply.sh refuses the real tree" >&2; exit 1; }
r1=$(cat "$OUT/apply-report.txt")
d1=$(git -C "$REAL" status --short 2>/dev/null | wc -l)
bash "$REPO/apply.sh" "$REAL" >/dev/null 2>&1 || { echo "FAIL: apply.sh re-run failed" >&2; exit 1; }
r2=$(cat "$OUT/apply-report.txt")
d2=$(git -C "$REAL" status --short 2>/dev/null | wc -l)
[ "$r1" = "$r2" ] || { echo "FAIL: apply-report.txt differs between identical runs" >&2; exit 1; }
[ "$d1" = "$d2" ] || { echo "FAIL: tree dirty-path count changed between identical runs ($d1 -> $d2)" >&2; exit 1; }
echo "PASS: apply.sh is idempotent on the real tree ($d1 dirty paths, report stable)"

if E2631_BASE_SHA=0000000000000000000000000000000000000000 \
	bash "$REPO/apply.sh" "$REAL" >/tmp/e2631-apply-refuse.log 2>&1; then
	echo "FAIL: apply.sh accepted a wrong pinned base" >&2; exit 1
fi
grep -q "REFUSING" /tmp/e2631-apply-refuse.log || { echo "FAIL: the wrong-base refusal says nothing useful" >&2; exit 1; }
echo "PASS: a wrong pinned base refuses before touching the tree (--allow-other-base overrides)"

mk_fake() {	# a minimal tree with every piece apply.sh's assertions read
	local d=$1
	rm -rf "$d"
	mkdir -p "$d/target/linux/sanechips/image" "$d/target/linux/sanechips/dts" \
		"$d/target/linux/sanechips/patches-6.18" "$d/target/linux/generic" \
		"$d/package/kernel/mt76" "$d/scripts"
	printf '%s\n' '$(eval $(call BuildImage))' >"$d/target/linux/sanechips/image/Makefile"
	cp "$REPO/overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts" \
		"$d/target/linux/sanechips/dts/"
	: >"$d/target/linux/sanechips/patches-6.18/026-ARM-dts-zte-Add-ZX279128S-SoC-description.patch"
	printf 'CONFIG_VFP=y\nCONFIG_VFPv3=y\nCONFIG_NEON=y\nCONFIG_KERNEL_MODE_NEON=y\nCONFIG_ARM_THUMBEE=y\n' \
		>"$d/target/linux/sanechips/config-6.18"
	printf '# CONFIG_SOFTLOCKUP_DETECTOR is not set\n# CONFIG_DETECT_HUNG_TASK is not set\n# CONFIG_DEFAULT_HUNG_TASK_TIMEOUT is not set\n' \
		>"$d/target/linux/generic/config-6.18"
	printf 'AUTOLOAD:=$(call AutoProbe,mt7915e)\n' >"$d/package/kernel/mt76/Makefile"
	printf 'src-git packages https://git.openwrt.org/feed/packages.git\nsrc-git luci https://git.openwrt.org/project/luci.git\nsrc-git routing https://git.openwrt.org/feed/routing.git\n' \
		>"$d/feeds.conf.default"
}
run_fake() { bash "$REPO/apply.sh" "$1" >/dev/null 2>&1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mk_fake "$tmp/t0"
run_fake "$tmp/t0" || { echo "FAIL: apply.sh refused a correct skeleton tree" >&2; exit 1; }
echo "PASS: structural assertions pass on a correct skeleton"

mk_fake "$tmp/t1"; rm "$tmp/t1"/target/linux/sanechips/patches-6.18/026-*
run_fake "$tmp/t1" && { echo "FAIL: missing dtsi-creating patch was accepted" >&2; exit 1; }
echo "PASS: tamper 1/4 - missing base dtsi patch refused"

mk_fake "$tmp/t2"; sed -i '/BuildImage/d' "$tmp/t2/target/linux/sanechips/image/Makefile"
run_fake "$tmp/t2" && { echo "FAIL: an image Makefile without the BuildImage hook was accepted" >&2; exit 1; }
echo "PASS: tamper 2/4 - unhookable image Makefile refused"

mk_fake "$tmp/t3"; rm "$tmp/t3/package/kernel/mt76/Makefile"; mkdir "$tmp/t3/package/kernel/mt76/Makefile"
run_fake "$tmp/t3" && { echo "FAIL: unreadable mt76 Makefile was accepted" >&2; exit 1; }
echo "PASS: tamper 3/4 - unverifiable mt76 AutoProbe state refused"

mk_fake "$tmp/t4"; rm "$tmp/t4/target/linux/generic/config-6.18"
run_fake "$tmp/t4" && { echo "FAIL: missing generic config was accepted" >&2; exit 1; }
echo "PASS: tamper 4/4 - missing generic config refused"
