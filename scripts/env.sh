# shellcheck shell=sh
# Every machine-specific path in this kit, resolved in one place.
#
#   from the repo root:   . scripts/env.sh
#   from wsl/ or tools/:  . "$(dirname "$0")/../scripts/env.sh"
#
# Precedence is: an exported variable, then $E2631_KIT/.env (untracked; copy
# .env.example), then the default. The defaults are chosen so a plain clone works
# on an ordinary Linux box: the base tree lives in ~/openwrt and everything else
# is derived from where this repo actually lives rather than from a hardcoded
# path. Nothing here builds anything - it only answers "where", so a failure can
# name the path that was wrong instead of leaving a stranger to guess.

# Find the repo root: a directory holding both apply.sh and overlay/.
e2631_find_kit() {
	_e_d=$(cd "$(dirname "$1")" 2>/dev/null && pwd) || return 1
	while [ -n "$_e_d" ] && [ "$_e_d" != "/" ]; do
		if [ -f "$_e_d/apply.sh" ] && [ -d "$_e_d/overlay" ]; then echo "$_e_d"; return 0; fi
		_e_d=$(dirname "$_e_d")
	done
	return 1
}

if [ -z "${E2631_KIT:-}" ]; then
	for _c in "$(e2631_find_kit "${BASH_SOURCE:-$0}" 2>/dev/null)" "$(e2631_find_kit "$PWD" 2>/dev/null)"; do
		[ -n "$_c" ] || continue
		E2631_KIT=$_c
		break
	done
fi
if [ -z "${E2631_KIT:-}" ]; then
	echo "env.sh: cannot locate this repository (a directory holding apply.sh and overlay/)." >&2
	echo "        Set E2631_KIT to the directory you cloned the kit into, then re-run." >&2
	exit 1
fi

# $KIT/.env is the local, untracked config. It is read before the defaults so it
# can supply them. Only the keys listed in the case below are honoured, assigned
# literally - no eval, so a value containing $( ) or a semicolon stays a value.
if [ -f "$E2631_KIT/.env" ]; then
	while IFS= read -r _l || [ -n "$_l" ]; do
		_k=$(printf '%s' "$_l" | sed -n 's/^[[:blank:]]*\(E2631_[A-Z0-9_]*\)[[:blank:]]*=.*$/\1/p')
		[ -n "$_k" ] || continue
		_v=$(printf '%s' "$_l" | sed -n 's/^[^=]*=[[:blank:]]*\(.*\)$/\1/p' | sed 's/[[:blank:]]*$//; s/^["'"'"']\(.*\)["'"'"']$/\1/')
		case "$_k" in
		E2631_WORK) [ -n "${E2631_WORK:-}" ] || E2631_WORK=$_v ;;
		E2631_OUT) [ -n "${E2631_OUT:-}" ] || E2631_OUT=$_v ;;
		E2631_TREE) [ -n "${E2631_TREE:-}" ] || E2631_TREE=$_v ;;
		E2631_BASE_REPO) [ -n "${E2631_BASE_REPO:-}" ] || E2631_BASE_REPO=$_v ;;
		E2631_BASE_BRANCH) [ -n "${E2631_BASE_BRANCH:-}" ] || E2631_BASE_BRANCH=$_v ;;
		E2631_BASE_SHA) [ -n "${E2631_BASE_SHA:-}" ] || E2631_BASE_SHA=$_v ;;
		E2631_TEMPLATE) [ -n "${E2631_TEMPLATE:-}" ] || E2631_TEMPLATE=$_v ;;
		E2631_PROXY) [ -n "${E2631_PROXY:-}" ] || E2631_PROXY=$_v ;;
		esac
	done < "$E2631_KIT/.env"
fi

# The directory that holds out/ and the vendor template package. Default: the
# parent of the kit, which is where a sibling checkout naturally lives.
E2631_WORK=${E2631_WORK:-$(dirname "$E2631_KIT")}
E2631_OUT=${E2631_OUT:-$E2631_WORK/out}
# The OpenWrt checkout the overlay gets applied to.
E2631_TREE=${E2631_TREE:-$HOME/openwrt}
# The base tree this port is known to build against (BUILD.md: "the base").
E2631_BASE_REPO=${E2631_BASE_REPO:-https://github.com/Zlion-Y/openwrt-sanechips-mirror}
E2631_BASE_BRANCH=${E2631_BASE_BRANCH:-pr-zte-zxhn-h3600}
E2631_BASE_SHA=${E2631_BASE_SHA:-db8b8d4130457dbe46558a7cea872dd2dae539e6}
# The version-header template. Default is the 532-byte header slice shipped here
# (seed/vendor-verhdr-8b8.bin): packing only reads the 512-byte version header, so a
# full vendor package is optional. Point this at an official .bin you own to run
# mkfactory's selftest/info, or to re-check the slice against the real thing.
E2631_TEMPLATE=${E2631_TEMPLATE:-$E2631_KIT/seed/vendor-verhdr-8b8.bin}
# Scripts that drop a log under out/ should not have to create it, and a fresh clone has
# no out/ yet. Failing to create it is not fatal: the read-only tools need no scratch dir.
[ -d "$E2631_OUT" ] || mkdir -p "$E2631_OUT" 2>/dev/null || true
export E2631_KIT E2631_WORK E2631_OUT E2631_TREE E2631_BASE_REPO E2631_BASE_BRANCH E2631_BASE_SHA E2631_TEMPLATE

e2631_need_cmd() {	# e2631_need_cmd <command> <what it is needed for>
	command -v "$1" >/dev/null 2>&1 && return 0
	echo "env.sh: no $1 on PATH - $2" >&2
	echo "        Debian/Ubuntu: sudo apt-get install $( [ "$1" = node ] && echo nodejs || echo "$1" )" >&2
	return 1
}

e2631_need_file() {	# e2631_need_file <path> <what it is> <how to get it>
	[ -f "$1" ] && return 0
	echo "missing $1 ($2)" >&2
	echo "        $3" >&2
	return 1
}
