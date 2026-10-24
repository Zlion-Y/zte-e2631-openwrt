#!/bin/sh
# Overlay the E2631 files onto a checked-out OpenWrt tree and wire them in.
# Idempotent: re-running only refreshes the copies.
#
#   git clone --branch <base-branch> https://github.com/Zlion-Y/openwrt-sanechips-mirror openwrt
#   git -C openwrt checkout $BASE_SHA
#   ./apply.sh openwrt
#
# scripts/bootstrap.sh does all of that, and scripts/env.sh resolves the paths.
set -eu
. "$(dirname "$0")/scripts/env.sh"

BASE_SHA=${E2631_BASE_SHA:-db8b8d4130457dbe46558a7cea872dd2dae539e6}
TREE=${1:-openwrt}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -d "$TREE/target/linux/sanechips" ] || {
	echo "apply.sh: no sanechips target in $TREE." >&2
	echo "        That is the base tree this overlay is written against; get it with" >&2
	echo "        'bash scripts/bootstrap.sh', or point E2631_TREE at a checkout of" >&2
	echo "        $E2631_BASE_REPO#$E2631_BASE_SHA." >&2
	exit 1; }

# Say which commit the tree is actually at. Everything below edits files by
# pattern, so a tree at another commit can apply cleanly and still be wrong.
if command -v git >/dev/null 2>&1 && [ -d "$TREE/.git" ]; then
	at=$(git -C "$TREE" rev-parse HEAD 2>/dev/null || echo unknown)
	[ "$at" = "$BASE_SHA" ] || echo "apply.sh: NOTE $TREE is at $at, not the pinned $BASE_SHA"
fi

cp -r "$HERE/overlay/." "$TREE/"
mkdir -p "$TREE/tools"
cp "$HERE/tools/mkfactory.mjs" "$TREE/tools/e2631-mkfactory.mjs"
# The overlay is authored on a filesystem without exec bits; these have to be
# runnable on the device, and rc.common refuses to enable a service script that
# is not executable.
for f in sbin/e2631-probe sbin/e2631-trial sbin/e2631-commit sbin/e2631-leds sbin/e2631-hb \
	 sbin/e2631-linkspeed sbin/e2631-wifi-on sbin/e2631-wifi-ap sbin/e2631-fwstate \
	 sbin/e2631-wifi-boot sbin/e2631-upgrade sbin/e2631-netune-apply \
	 sbin/e2631-userdata-status sbin/e2631-userdata-firstboot sbin/e2631-userdata-init \
	 etc/init.d/e2631-trial etc/init.d/e2631-wifi-eeprom etc/init.d/e2631-wifi-load \
	 etc/init.d/e2631-leds etc/init.d/e2631-hb etc/init.d/e2631-userdata \
	 etc/init.d/e2631-netune \
	 lib/upgrade/platform.sh \
	 etc/hotplug.d/firmware/11-mt76-caldata; do
	chmod +x "$TREE/target/linux/sanechips/base-files/$f"
done

# rc.d enablement deliberately ships NO "S95..." files: a Windows checkout of
# this repository cannot hold symlinks (the file becomes regular text and rcS
# would execute it), and rootfs.mk already auto-enables every init script that
# carries a START= line - the mechanism the v1.2 services rely on.

# Only two feeds matter for this board: packages (kmod-mt7915e, ttyd) and luci.
# Cloning routing/telephony/video costs several minutes of every local build and
# contributes nothing but unresolved-dependency warnings (bmx7) to the log.
# feeds.conf is regenerated from the .default, so drop any stale copy.
if [ -f "$TREE/feeds.conf.default" ]; then
	sed -i -E 's@^src-git (routing|telephony|video) @#src-git \1 @' "$TREE/feeds.conf.default"
	rm -f "$TREE/feeds.conf"
fi

# v1.3: Argon is vendored at overlay/package/luci/luci-theme-argon - the same
# default theme as v1.0..v1.2, now shipped in-repo instead of cloned from a
# third-party GitHub repository at apply time (that clone was this script's
# only network fetch). Bootstrap stays selected as the fallback theme.

# A dead mirror is worse than a slow one: OpenWrt's curl is started with
# --connect-timeout but no transfer watchdog, so a host that accepts the
# connection and then sends nothing (ftpmirror.gnu.org did exactly that here,
# 0 bytes for 6 minutes, load 0.03) blocks the build forever instead of falling
# through to the next entry. Two knobs, both supported upstream:
#   CURL_OPTIONS            honoured verbatim by scripts/download.pl -> rotate
#   E2631_GNU_MIRROR        put a fast GNU mirror at the head of "@GNU"
# The mirror is opt-in because the CI runner is not on such a network.
if [ -n "${E2631_GNU_MIRROR:-}" ] && [ -f "$TREE/scripts/projectsmirrors.json" ]; then
	# It is already in the list, just last: download.pl only rotates when curl
	# fails, so a stalled head entry costs the whole timeout chain per package.
	# Move the chosen mirror to the front instead of appending a duplicate.
	# Plain sed with a | delimiter - this file is /bin/sh, so no ${var//x/y}.
	sed -i "\|$E2631_GNU_MIRROR|d" "$TREE/scripts/projectsmirrors.json"
	sed -i "0,/\"@GNU\": \[/s||\"@GNU\": [\n\t\t\"$E2631_GNU_MIRROR\",|" "$TREE/scripts/projectsmirrors.json"
	sed -n '/"@GNU": \[/,+2p' "$TREE/scripts/projectsmirrors.json"
fi

IMG="$TREE/target/linux/sanechips/image/Makefile"
if ! grep -q 'Makefile.e2631' "$IMG"; then
	# Insert before BuildImage so the Device definition is seen by image.mk.
	sed -i 's|^$(eval $(call BuildImage))$|include $(TOPDIR)/target/linux/sanechips/image/Makefile.e2631\n\n$(eval $(call BuildImage))|' "$IMG"
fi
grep -q 'Makefile.e2631' "$IMG" || { echo "failed to hook image/Makefile" >&2; exit 1; }

# Take the MT7916 off kmodloader's autoload list. The package's AutoProbe entry
# makes kmodloader load mt7915e during the boot phase, where the PCI core probes
# the chip's two functions (14c3:7906 and the 14c3:790a ext-HIF) in parallel -
# and the main function's hif2 handshake reads the other function's BAR, which
# has been seen to wedge this SoC completely. /etc/init.d/e2631-wifi-eeprom
# loads the module instead and binds the two functions one at a time, after the
# calibration file the driver needs has been staged.
MT76="$TREE/package/kernel/mt76/Makefile"
if [ -f "$MT76" ]; then
	sed -i '/AUTOLOAD:=$(call AutoProbe,mt7915e)/d' "$MT76"
	grep -q 'AutoProbe,mt7915e' "$MT76" && {
		echo "mt7915e AutoProbe still present in $MT76" >&2; exit 1; }
	echo "mt7915e autoload removed: e2631-wifi-eeprom owns the load order"
fi

# The target inherits cortex-a9 hard-float defaults; this SoC has no VFP at all.
# Turn the option off together with everything that depends on it: leaving
# CONFIG_VFPv3=y with VFP unset makes the kernel's syncconfig restart and then
# stop on an interactive prompt (that is what a "VFP ... [N/y/?] (NEW)" failure
# in a CI log means). The names are matched case-insensitively and rewritten
# with their own spelling, because the fragment writes CONFIG_VFPv3, not
# CONFIG_VFPV3.
CFG="$TREE/target/linux/sanechips/config-6.18"
for opt in VFP VFPv3 NEON KERNEL_MODE_NEON ARM_THUMBEE; do
	if sed -E -n "s|^CONFIG_(${opt})=y\$|# CONFIG_\\1 is not set|Ip" "$CFG" \
	   | grep -q .; then
		sed -E -i "s|^CONFIG_(${opt})=y\$|# CONFIG_\\1 is not set|I" "$CFG"
		echo "kernel config: disabled ${opt}"
	fi
done
for opt in VFP VFPv3 NEON; do
	grep -qiE "^CONFIG_${opt}=y$" "$CFG" && {
		echo "kernel config: $opt still enabled" >&2; exit 1; }
done

# The vendor U-Boot hands the kernel its own bootargs - on this board they are
# "root=/dev/mtdblock8 rootfstype=jffs2 init=/bin/sh -c ..." (read from
# /proc/cmdline of the running vendor system), which describes the vendor
# partition table and would leave PID 1 as a shell. Compile in the command line
# of our own table and ignore the bootloader's, the way the SR1010 port does.
#
# The mtdblock number is the row the "rootfs" label occupies in the board DTS,
# counted from mtd0 because CONFIG_MTD_PARTITIONED_MASTER is off. Counted, not
# remembered: a table edit that shifts the rows would otherwise bake a wrong
# root= into the kernel and only show up on the device, after the flash.
DTS="$TREE/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts"
ROOTDEV=$(awk '
	/[[:space:]]partitions[[:space:]]*\{/ { p = 1 }
	p {
		d += gsub(/\{/, "{") - gsub(/\}/, "}")
		if (match($0, /label[ \t]*=[ \t]*"[^"]*"/)) {
			s = substr($0, RSTART, RLENGTH); sub(/^[^"]*"/, "", s); sub(/".*$/, "", s)
			if (s == "rootfs") { print n; exit }
			n++
		}
		if (d == 0) p = 0
	}' "$DTS")
[ -n "$ROOTDEV" ] || { echo "no partition labelled rootfs in $DTS" >&2; exit 1; }

# Written unconditionally, and our previous block removed first: apply.sh runs
# against a working tree that may already have been patched, and a stale
# CONFIG_CMDLINE in target/linux/sanechips/config-6.18 would otherwise survive
# every later edit here (that is how a changed root= or a dropped earlycon would
# silently keep the old value).
CMD_MARK='# E2631: the vendor bootargs name the vendor layout, not ours (see apply.sh)'
sed -i -e "\|^$CMD_MARK\$|d" -e '/^CONFIG_CMDLINE_BOOL=y$/d' -e '/^CONFIG_CMDLINE_FORCE=y$/d' \
	-e '/^CONFIG_CMDLINE=/d' "$CFG"
printf '%s\n' "$CMD_MARK" \
	'CONFIG_CMDLINE_BOOL=y' \
	"CONFIG_CMDLINE=\"console=ttyAMA0,115200n8 earlycon root=/dev/mtdblock$ROOTDEV rootfstype=squashfs ro rootwait\"" \
	'CONFIG_CMDLINE_FORCE=y' >> "$CFG"
echo "kernel config: forced the OpenWrt command line, root on mtdblock$ROOTDEV, earlycon on"

# The MT7916 takes the whole board down without ever printing a line: no console
# output, no ARP, and only a power cycle brings it back. That is indistinguishable
# from a dead CPU, so let the kernel speak for itself next time:
#   SOFTLOCKUP_DETECTOR  a CPU spinning with preemption off gets reported
#   DETECT_HUNG_TASK     a task blocked in one place for 25 s dumps its stack
#   MAGIC_SYSRQ_SERIAL   SysRq over a plain serial line, no break needed, so 'w'
#                        and 'l' can be typed at the console while it is stuck
DBG_MARK='# E2631: lockup instrumentation for the MT7916 bus hang (see apply.sh)'
sed -i -e "\|^$DBG_MARK\$|d" -e '/^CONFIG_SOFTLOCKUP_DETECTOR=y$/d' \
	-e '/^CONFIG_DETECT_HUNG_TASK=y$/d' -e '/^CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=/d' \
	-e '/^CONFIG_MAGIC_SYSRQ_SERIAL=y$/d' -e '/^CONFIG_PCIEPORTBUS=y$/d' \
	-e '/^CONFIG_PCIEAER=y$/d' -e '/^CONFIG_PCIEAER_INJECT=y$/d' "$CFG"
printf '%s\n' "$DBG_MARK" \
	'CONFIG_SOFTLOCKUP_DETECTOR=y' \
	'CONFIG_DETECT_HUNG_TASK=y' \
	'CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=25' \
	'# CONFIG_BOOTPARAM_HUNG_TASK_PANIC is not set' \
	'CONFIG_MAGIC_SYSRQ_SERIAL=y' \
	'CONFIG_PCIEPORTBUS=y' \
	'CONFIG_PCIEAER=y' >> "$CFG"
echo "kernel config: softlockup + hung task + serial SysRq + PCIe port services/AER on"
# The two detectors above do NOT come from our fragment: target/linux/generic
# names them explicitly as "is not set", and the generic fragment wins over the
# target one, so asking in sanechips/config-6.18 changes nothing. Flip them where
# they are actually decided.
GEN="$TREE/target/linux/generic/config-6.18"
if [ -f "$GEN" ]; then
	sed -i -e 's|^# CONFIG_SOFTLOCKUP_DETECTOR is not set$|CONFIG_SOFTLOCKUP_DETECTOR=y|' \
		-e 's|^# CONFIG_DETECT_HUNG_TASK is not set$|CONFIG_DETECT_HUNG_TASK=y|' \
		-e 's|^# CONFIG_DEFAULT_HUNG_TASK_TIMEOUT.*$|CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=25|' \
		-e 's|^CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=.*$|CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=25|' "$GEN"
	grep -qE '^CONFIG_SOFTLOCKUP_DETECTOR=y$' "$GEN" || {
		echo "generic config: could not turn the soft lockup detector on" >&2; exit 1; }
	echo "generic config: soft lockup + hung task detectors on"
fi
# The vendor kernel runs PCIe port services with AER enabled; without them a
# poisoned TLP or an unread completion produces a silent bus stall instead of a
# report, which is exactly the failure being chased here.
#
# /dev/mem is NOT reachable by editing fragments. The built kernel config
# (out/gen1/config-6.18:2314) comes out as "# CONFIG_DEVMEM is not set" even though
# generic/config-6.18:1494 and sanechips/config-6.18:147 both read CONFIG_DEVMEM=y and
# nothing anywhere asks for n -- drivers/char/Kconfig:311 is a plain "bool / default y".
# Why the request is dropped is still unexplained, so do not re-add a fragment edit on
# the assumption that it will work. Read the MT7916's MT_TOP_MISC without a driver with
# the probe module instead: build/wsl/build-regprobe.sh.

# apply.sh copies; it does not sync. A patch or base-file edit that leaves
# overlay/ therefore stays in the tree for ever and keeps being applied - that is
# exactly how two abandoned Dosilicon SPI-NAND patches went on building into the
# kernel after they were deleted from this repository. Anything in the tree's patch
# series that is neither in the base commit nor in overlay/ is listed, not removed:
# it may be somebody's own work, and deleting it is their call.
PD="$TREE/target/linux/sanechips/patches-6.18"
if [ -d "$PD" ] && [ -d "$TREE/.git" ]; then
	orphan=
	for f in "$PD"/*.patch; do
		[ -e "$f" ] || continue
		b=$(basename "$f")
		git -C "$TREE" ls-files --error-unmatch "target/linux/sanechips/patches-6.18/$b" \
			>/dev/null 2>&1 && continue
		[ -e "$HERE/overlay/target/linux/sanechips/patches-6.18/$b" ] && continue
		orphan="$orphan $b"
	done
	if [ -n "$orphan" ]; then
		echo "apply.sh: WARNING these patches are in the tree but in neither the base commit nor overlay/:"
		for b in $orphan; do echo "            $b"; done
		echo "        If they are stale, remove them and re-run; if they are yours, ignore this."
	fi
fi

echo "overlay applied to $TREE (base $BASE_SHA)"
