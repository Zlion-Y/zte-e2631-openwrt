#!/bin/bash
# Get a clean Linux box to the point where `make world` can succeed: base tree at
# the pinned commit, the E2631 overlay applied, the two feeds that matter
# installed, and the seed .config in place.
#
#   bash scripts/bootstrap.sh          # or: make bootstrap
#
# Idempotent: every step checks before it acts, so re-running after a failed
# build costs seconds. It never deletes a tree - if something looks wrong it
# stops and says what.
set -eu
cd "$(dirname "$0")/.."
. scripts/env.sh

say() { printf 'bootstrap: %s\n' "$*"; }
die() { printf 'bootstrap: %s\n' "$*" >&2; exit 1; }

e2631_need_cmd git "clone the base tree and the feeds" || exit 1
e2631_need_cmd make "drive the OpenWrt build" || exit 1
e2631_need_cmd gcc "build the toolchain (build-essential)" || exit 1
e2631_need_cmd g++ "build some host tools (build-essential)" || exit 1
e2631_need_cmd file "identify the built payloads" || exit 1

# ---- 1. the base tree --------------------------------------------------------
# A full clone, not a shallow one: the port is pinned to a commit that may not be
# the branch head, and a shallow clone cannot check out an arbitrary SHA.
if [ ! -d "$E2631_TREE" ]; then
	say "cloning $E2631_BASE_REPO#$E2631_BASE_SHA into $E2631_TREE"
	git clone --branch "$E2631_BASE_BRANCH" "$E2631_BASE_REPO" "$E2631_TREE" ||
		die "clone failed - network, or the branch moved? $E2631_BASE_REPO can be overridden in .env"
fi
[ -d "$E2631_TREE/target/linux/sanechips" ] ||
	die "no sanechips target in $E2631_TREE - that is not the base tree this kit overlays (set E2631_TREE, or remove the directory and re-run)"

if ! git -C "$E2631_TREE" rev-parse --verify "$E2631_BASE_SHA^{commit}" >/dev/null 2>&1; then
	say "fetching $E2631_BASE_BRANCH (the pinned commit is not in this clone yet)"
	git -C "$E2631_TREE" fetch origin "$E2631_BASE_BRANCH" ||
		die "cannot fetch $E2631_BASE_SHA - it must be reachable from $E2631_BASE_BRANCH"
fi
if [ "$(git -C "$E2631_TREE" rev-parse HEAD)" != "$E2631_BASE_SHA" ]; then
	if [ -n "$(git -C "$E2631_TREE" status --porcelain)" ]; then
		die "$E2631_TREE is at $(git -C "$E2631_TREE" rev-parse --short HEAD), not the pinned $E2631_BASE_SHA, and has uncommitted changes.
       Look at 'git -C $E2631_TREE status' first: if those edits are yours, point E2631_TREE at a
       fresh clone instead of overwriting them. If they are only a stale copy of this overlay,
       reset the tree to the pinned commit and re-run."
	fi
	say "checking out the pinned commit $E2631_BASE_SHA"
	git -C "$E2631_TREE" checkout -q "$E2631_BASE_SHA"
fi

# ---- 2. the overlay ----------------------------------------------------------
say "applying the E2631 overlay to $E2631_TREE"
bash "$E2631_KIT/apply.sh" "$E2631_TREE"

# The two edits apply.sh must leave behind, checked here rather than trusted:
# both are what make the image boot on this board instead of the H3600.
grep -qx 'CONFIG_CMDLINE_FORCE=y' "$E2631_TREE/target/linux/sanechips/config-6.18" ||
	die "apply.sh left no forced kernel command line in $E2631_TREE - the vendor bootargs would win"
grep -q 'Makefile.e2631' "$E2631_TREE/target/linux/sanechips/image/Makefile" ||
	die "apply.sh did not hook image/Makefile - the E2631 device definition would be invisible"

# ---- 3. feeds ---------------------------------------------------------------
# Only packages and luci. Two traps, both hit in practice: a hand-cloned feed has
# no feeds/<name>.index and `feeds install` then says "Ignoring feed 'luci'" -
# the build still succeeds, just without LuCI, silently; and a killed half-clone
# leaves a shallow repo whose next pull re-fetches the full history.
for f in packages luci; do
	[ -d "$E2631_TREE/feeds/$f/.git" ] || {
		say "cloning feed $f"
		git clone -q --depth 1 --single-branch "https://github.com/openwrt/$f" "$E2631_TREE/feeds/$f"
	}
	[ -f "$E2631_TREE/feeds/$f.index" ] || {
		say "indexing feed $f"
		( cd "$E2631_TREE" && ./scripts/feeds update "$f" 2>&1 | tail -3 )
	}
done
say "installing feed packages"
( cd "$E2631_TREE" && ./scripts/feeds install -a 2>&1 | grep -E "Ignoring|Installing" | tail -8 )
[ -d "$E2631_TREE/package/feeds/packages" ] || die "feed install produced no package/feeds/packages"

# ---- 4. the seed config ----------------------------------------------------
say "seeding .config from seed/e2631.config"
cp "$E2631_KIT/seed/e2631.config" "$E2631_TREE/.config"
( cd "$E2631_TREE" && make defconfig >/dev/null 2>&1 )
for want in CONFIG_TARGET_DEVICE_sanechips_generic_DEVICE_zte_zxhn-e2631= \
	CONFIG_SOFT_FLOAT= CONFIG_PACKAGE_luci= CONFIG_PACKAGE_e2631-fwslot=; do
	grep -q "^$want" "$E2631_TREE/.config" || die "seed check: $want missing after defconfig"
done
say "seed ok: E2631 device selected, soft-float on, LuCI and the slot tool in the image"

say "done. next: bash scripts/build.sh   (about 40 minutes on 2 cores, then it packs)"
