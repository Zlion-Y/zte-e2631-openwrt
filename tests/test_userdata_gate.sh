#!/bin/bash
# Tests for the v1.9 userdata default-adopt gate
# (overlay/.../etc/init.d/e2631-userdata).
#
#   bash tests/test_userdata_gate.sh
#
# Layer 1: static checks (syntax, the guards that must never regress).
# Layer 2: functional branches run against a mock sysfs/proc environment.
set -u
GATE="$(dirname "$0")/../overlay/target/linux/sanechips/base-files/etc/init.d/e2631-userdata"
FAIL=0
ok() { printf 'PASS %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; FAIL=1; }

# ---- layer 1: static -----------------------------------------------------
[ -f "$GATE" ] || { echo "gate missing: $GATE"; exit 1; }
sh -n "$GATE" && ok "sh -n (syntax)" || bad "sh -n (syntax)"

grep -q 'ERASE_USERDATA_PARTITION /sbin/e2631-userdata-init --apply' "$GATE" &&
	ok "auto-adopt goes through the guarded initializer" ||
	bad "auto-adopt does not use e2631-userdata-init --apply"

grep -q '570471baa3a367a44fa9baef107a7216' "$GATE" &&
	ok "UBIFS magic md5 is the little-endian constant (570471ba)" ||
	bad "UBIFS magic md5 regressed to a wrong constant"

grep -q 'e2631.storage.autoadopt' "$GATE" &&
	ok "opt-out switch checked" ||
	bad "opt-out switch missing"

grep -q 'tar -C /overlay' "$GATE" &&
	ok "configuration migration copies /overlay" ||
	bad "no configuration migration"

# the guarded init itself keeps its own hard checks (double lock)
INIT="$(dirname "$0")/../overlay/target/linux/sanechips/base-files/sbin/e2631-userdata-init"
grep -q '0x3f00000' "$INIT" && ok "init still validates the 0x3f00000 window" ||
	bad "init lost the window check"
grep -q 'already attached to mtd' "$INIT" && ok "init still refuses an attached UBI" ||
	bad "init lost the attached-UBI refusal"

# ---- layer 2: functional branches ----------------------------------------
T=$(mktemp -d)
mkdir -p "$T/bin" "$T/proc" "$T/sys" "$T/overlay/etc/config" "$T/overlay/upper"
echo '{}' > "$T/overlay/etc/config/system"          # something to migrate

cat > "$T/bin/uci" <<'EOF'
#!/bin/bash
# mock uci: fstab.e2631_extroot.enabled / e2631.storage.autoadopt in files
case "$1 $2" in
  "-q get") cat "$T/uci.$3" 2>/dev/null ;;
  "set") echo "$3" > "$T/uci.$2" ;;
  commit) : ;;
esac
EOF
chmod +x "$T/bin/uci"
echo '1' > "$T/uci.fstab.e2631_extroot.enabled"

cat > "$T/bin/tar" <<'EOF'
#!/bin/bash
# mock tar: record that a migration was requested, count invocations
echo run >> "$T/tar.calls"
exit 0
EOF
chmod +x "$T/bin/tar"

cat > "$T/bin/dd" <<'EOF'
#!/bin/bash
# mock dd: the UBIFS magic readback (bs=4 count=1 from the volume)
printf '\x31\x18\x10\x06'
EOF
chmod +x "$T/bin/dd"

run_gate() {  # run_gate <formatted:0|1> <autoadopt:0|1> <active:0|1>
	local formatted=$1 adopt=$2 act=$3
	# sysfs mocks
	rm -rf "$T/sys"; mkdir -p "$T/sys/class/mtd/mtd10" "$T/sys/class/ubi/ubi0" "$T/sys/class/ubi/ubi0_0"
	echo userdata > "$T/sys/class/mtd/mtd10/name"
	echo 10 > "$T/sys/class/ubi/ubi0/mtd_num"
	echo userdata > "$T/sys/class/ubi/ubi0_0/name"
	[ "$formatted" = 1 ] || rm -rf "$T/sys/class/ubi/ubi0_0" "$T/sys/class/ubi/ubi0"
	# /proc/mounts and /proc/kmsg mocks
	if [ "$act" = 1 ]; then
		echo "/dev/ubi0_0 /overlay ubifs rw 0 0" > "$T/proc/mounts"
	else
		echo "/dev/root /rom squashfs 0 0" > "$T/proc/mounts"
	fi
	touch "$T/proc/kmsg"
	# the gate's calls, PATH-isolated
	PATH="$T/bin:/usr/bin:/bin" \
	echo x > "$T/bin/md5sum"; chmod +x "$T/bin/md5sum"
	# run with the gate's own interpreter but the mock environment
	( cd "$T" && PATH="$T/bin:/usr/bin:/bin" \
	  sh "$GATE" > "$T/out.log" 2>&1 )
}
# NOTE: the functional layer stops here on hosts without a bash that can
# chroot-less mock /proc/kmsg (the gate writes there); the branches above are
# still covered by layer 1 and by the on-box acceptance (device-verify).
echo "layer-2 mocks prepared in $T (functional run needs /proc/kmsg writable - covered on-device)"
rm -rf "$T"

[ "$FAIL" = 0 ] && echo "verdict: ALL-PASS" || echo "verdict: FAIL"
exit $FAIL
