#!/bin/bash
# One command for every gate this kit has. There is deliberately no CI: the build
# runs locally, so the checks that used to live in a workflow have to be runnable
# by hand, and this is the entry point that runs all of them.
#
#   bash scripts/check.sh            # or: make check
#
# A gate that cannot run on this machine reports SKIP with the reason instead of
# failing the run - the reason always says what to install, because a silently
# missing gate is worse than a red one.
set -u
cd "$(dirname "$0")/.."
. scripts/env.sh

rc=0
run() {	# run <label> <gate function>
	local label=$1 fn=$2 out code
	printf '%-32s ' "$label"
	out=$("$fn" 2>&1); code=$?
	if [ $code -eq 0 ]; then
		echo PASS
	elif [ $code -eq 77 ]; then
		echo "SKIP  $(printf '%s' "$out" | head -1)"
	else
		echo FAIL
		printf '%s\n' "$out" | sed 's/^/    /'
		rc=1
	fi
}

g_shell() {	# every helper in the kit has to parse
	local bad=0 f
	for f in "$E2631_KIT"/*.sh "$E2631_KIT"/wsl/*.sh "$E2631_KIT"/scripts/*.sh "$E2631_KIT"/board/*.sh; do
		[ -f "$f" ] || continue
		bash -n "$f" 2>/dev/null || { echo "bash -n: $f"; bad=1; }
	done
	return $bad
}

g_js() {
	e2631_need_cmd node "check the JavaScript tools" || return 77
	local bad=0 f
	for f in "$E2631_KIT"/tools/*.mjs "$E2631_KIT"/host/*.mjs; do
		node --check "$f" 2>/dev/null || { echo "node --check: $f"; bad=1; }
	done
	return $bad
}

g_ash() {	# the scripts that get put on the device, under the device's shell
	command -v busybox >/dev/null 2>&1 || {
		echo "no busybox: the board scripts are not ash-checked here. apt-get install busybox,"
		echo "because a CRLF or a bashism in them only shows up on the device."; return 77; }
	bash "$E2631_KIT/wsl/lint-kit.sh"
}

g_slot_tool() {	# compiles with -Werror, then parses a good and a bad package
	command -v gcc >/dev/null 2>&1 || { echo "no gcc: e2631-fwslot was not compiled here"; return 77; }
	if [ ! -f "$E2631_OUT/fixture/fixture.bin" ]; then
		if command -v node >/dev/null 2>&1; then
			node "$E2631_KIT/tools/make_fixture.mjs" "$E2631_OUT/fixture" >/dev/null || {
				echo "make_fixture.mjs failed"; return 1; }
		else
			# The split this machine has: gcc in the Linux box, node only on the
			# Windows side. Saying PASS here would hide the two parse checks.
			echo "no $E2631_OUT/fixture/fixture.bin and no node here: run"
			echo "     node tools/make_fixture.mjs $E2631_OUT/fixture"
			echo "on the side that has node, then re-run this gate."
			return 77
		fi
	fi
	bash "$E2631_KIT/wsl/check-kit.sh"
}

g_secret() {
	command -v git >/dev/null 2>&1 || { echo "no git: the tracked-file scan cannot run"; return 77; }
	bash "$E2631_KIT/scripts/secret-scan.sh"
}

g_dts() {
	e2631_need_cmd node "resolve the board dts includes" || return 77
	# The board file includes zte/zx279128s.dtsi, which is not in this repo: patch
	# 026 of the base tree creates it. Without the tree the gate can only see the
	# board file itself, and every &label in it looks undefined.
	if [ ! -d "$E2631_TREE/target/linux/sanechips/patches-6.18" ]; then
		echo "no base tree at $E2631_TREE - run scripts/bootstrap.sh, then re-run this gate"
		return 77
	fi
	node "$E2631_KIT/tools/dts_lint.mjs" \
		"$E2631_KIT/overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts" \
		"$E2631_TREE/target/linux/sanechips/patches-6.18" \
		"$E2631_TREE/target/linux/sanechips/dts" \
		"$E2631_KIT/overlay/target/linux/sanechips/patches-6.18"
}

g_upgrade() {
	command -v python3 >/dev/null 2>&1 && command -v busybox >/dev/null 2>&1 || {
		echo "install python3 and busybox for upgrade regression tests"; return 77; }
	python3 "$E2631_KIT/tests/test_upgrade.py" >/dev/null || {
		echo "test_upgrade.py failed"; return 1; }
}

g_apply() {
	bash "$E2631_KIT/wsl/apply-gate.sh"
}

run "upgrade failure paths (python)" g_upgrade
run "apply.sh fail-fast + structural assertions" g_apply
run "shell syntax (bash -n)" g_shell
run "javascript (node --check)" g_js
run "device scripts (busybox ash)" g_ash
run "slot tool (-Werror, parse, reject)" g_slot_tool
g_preflight() {
	local F="$E2631_OUT/fixture/fixture.bin"
	command -v busybox >/dev/null 2>&1 || { echo "no busybox: the preflight script is not ash-checked"; return 77; }
	bash -n "$E2631_KIT/scripts/preflight.sh" || return 1
	command -v gcc >/dev/null 2>&1 || { echo "no gcc: the package checks need e2631-fwslot compiled"; return 77; }
	[ -x /tmp/e2631-fwslot-native ] || gcc -std=gnu11 -w -o /tmp/e2631-fwslot-native 		"$E2631_KIT/overlay/package/firmware/e2631-fwslot/src/e2631-fwslot.c"
	E2631_FWSLOT_BIN=/tmp/e2631-fwslot-native
	[ -f "$F" ] || { echo "no $F - run node tools/make_fixture.mjs $E2631_OUT/fixture"; return 77; }
	if ! sh "$E2631_KIT/scripts/preflight.sh" "$F" --offline >/dev/null 2>&1; then
		echo "preflight refused a valid fixture"; return 1
	fi
	if sh "$E2631_KIT/scripts/preflight.sh" "$E2631_OUT/fixture/fixture-broken.bin" --offline >/dev/null 2>&1; then
		echo "preflight accepted a corrupted fixture"; return 1
	fi
	echo "valid fixture passes, corrupted fixture refuses"
}

run "board dts cross-reference" g_dts
run "first-flash preflight (fixture)" g_preflight
run "no credential shapes" g_secret

[ $rc -eq 0 ] || { echo; echo "check: FAILED"; exit 1; }
echo
echo "check: everything passed or skipped with a stated reason."
echo "       The two gates that read an artifact - verify_pkg.mjs and verify_img.mjs -"
echo "       run inside scripts/build.sh / node host/release.mjs, so they need a build."
