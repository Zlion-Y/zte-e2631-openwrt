#!/bin/bash
# Native wiring patches 037-041: staged b/ trees, emitted with `diff -ruN`
# over a diffroot/{a,b} pair (headers come out as a/... b/... natively).
set -uo pipefail
W=/d/AI/Qoder/E2631-OpenWRT/native/_wire
PARENT=/d/AI/Qoder/E2631-OpenWRT/_wire-parent
OUT=/d/AI/Qoder/E2631-OpenWRT/native/overlay/target/linux/sanechips/patches-6.18

rm -rf "$W"
mkdir -p "$W/pristine/drivers/net/ethernet/zte"
cp "$PARENT/Kconfig.orig"  "$W/pristine/drivers/net/ethernet/Kconfig"
cp "$PARENT/Makefile.orig" "$W/pristine/drivers/net/ethernet/Makefile"

emit() { # emit <patchfile> <a-stage> <b-stage>
	local f=$1 a=$2 b=$3 d=$W/diffroot
	rm -rf "$d"; mkdir -p "$d"
	cp -r "$W/$a" "$d/a"
	cp -r "$W/$b" "$d/b"
	(cd "$d" && diff -ruN a b || true) > "$OUT/$f"
	echo "$f: $(grep -c '^[+-]' "$OUT/$f") +/- lines"
}

# ---------- stage 1: scaffold (new zte dir + parent hooks) ----------
B1="$W/b1"; mkdir -p "$B1/drivers/net/ethernet/zte"
cp "$PARENT/Kconfig.orig"  "$B1/drivers/net/ethernet/Kconfig"
cp "$PARENT/Makefile.orig" "$B1/drivers/net/ethernet/Makefile"

cat > "$B1/drivers/net/ethernet/zte/Kconfig" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-only
config NET_VENDOR_ZTE
	bool "ZTE devices"
	depends on ARCH_ZTE || COMPILE_TEST
	default y
	help
	  If you have a network (Ethernet) device belonging to this class, say Y.

	  Note that the answer to this question doesn't directly affect the
	  kernel: saying N will just cause the configurator to skip all
	  the questions about ZTE devices.

if NET_VENDOR_ZTE

config ZX_NPU
	bool "ZTE ZX279128S npu scaffold"
	depends on OF
	help
	  Builds the npu scaffold. It exposes /proc/zx-npu/status and performs
	  no MMIO access; the vendor-native wired and MT7916 data paths are
	  added incrementally on the native line.

	  If unsure, say Y on this board.

endif # NET_VENDOR_ZTE
EOF
cat > "$B1/drivers/net/ethernet/zte/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ZX_NPU) += zx279128s-npu.o
EOF
sed -i 's|^source "drivers/net/ethernet/xircom/Kconfig"$|source "drivers/net/ethernet/xircom/Kconfig"\nsource "drivers/net/ethernet/zte/Kconfig"|' "$B1/drivers/net/ethernet/Kconfig"
printf 'obj-$(CONFIG_NET_VENDOR_ZTE) += zte/\n' >> "$B1/drivers/net/ethernet/Makefile"

emit 037-zte-add-the-ZX279128S-NPU-scaffold.patch pristine b1

add_entry() { # add_entry <dir> <config-block-file> <objline>
	local d=$1 cfg=$2 obj=$3
	sed -i '/^endif # NET_VENDOR_ZTE$/d' "$d/drivers/net/ethernet/zte/Kconfig"
	cat "$cfg" >> "$d/drivers/net/ethernet/zte/Kconfig"
	printf 'endif # NET_VENDOR_ZTE\n' >> "$d/drivers/net/ethernet/zte/Kconfig"
	printf '%s\n' "$obj" >> "$d/drivers/net/ethernet/zte/Makefile"
}

# ---------- stage 2: audit ----------
B2="$W/b2"; cp -r "$B1" "$B2"
cat > "$W/cfg-audit" <<'EOF'
config ZX_NPU_AUDIT
	bool "ZTE ZX279128S npu read-only register audit"
	depends on OF
	help
	  Maps the NPP/IDM/TM/PP accelerator windows and exposes a proc file
	  that reads a small documented set of registers only when userspace
	  writes "run". It never writes a register.

	  If unsure, say Y on this board.

EOF
add_entry "$B2" "$W/cfg-audit" 'obj-$(CONFIG_ZX_NPU_AUDIT) += zx279128s-npu-audit.o'
emit 038-zte-add-the-ZX279128S-NPU-read-only-audit.patch b1 b2

# ---------- stage 3: IDM ----------
B3="$W/b3"; cp -r "$B2" "$B3"
cat > "$W/cfg-idm" <<'EOF'
config ZX_NPU_IDM
	bool "ZTE ZX279128S vendor-native IDM fabric engine"
	depends on OF
	help
	  Builds the zxnpu0 netdev and the IDM RX/TX ring engine. The
	  hardware is untouched until zxnpu_idm.enable=1 and the netdev is
	  opened; the default firmware therefore keeps every write out.

	  If unsure, say Y on this board.

EOF
add_entry "$B3" "$W/cfg-idm" 'obj-$(CONFIG_ZX_NPU_IDM) += zx279128s-npu-idm.o'
emit 039-zte-add-the-ZX279128S-NPU-IDM-engine.patch b2 b3

# ---------- stage 4: PPE ----------
B4="$W/b4"; cp -r "$B3" "$B4"
cat > "$W/cfg-ppe" <<'EOF'
config ZX_NPU_PPE
	bool "ZTE ZX279128S vendor-native PPE/CLA access"
	depends on OF
	help
	  Low-level CLA/SBRG indirect table access for the vendor-native
	  wired data plane. The proc interface is inert until
	  zxnpu_ppe.enable=1.

	  If unsure, say Y on this board.

EOF
add_entry "$B4" "$W/cfg-ppe" 'obj-$(CONFIG_ZX_NPU_PPE) += zx279128s-npu-ppe.o'
emit 040-zte-add-the-ZX279128S-NPU-PPE-CLA-access.patch b3 b4

# ---------- stage 5: CSP WLAN ----------
B5="$W/b5"; cp -r "$B4" "$B5"
cat > "$W/cfg-wlan" <<'EOF'
config ZX_CSP_WLAN
	bool "ZTE ZX279128S vendor-native MT7916 G3 skeleton"
	depends on PCI && OF
	help
	  Vendor-native MT7916 host-driver skeleton. Binds only the 14c3:790a
	  ext-HIF function, reads BAR0 and the vendor firmware headers, and
	  performs no write until zxnpu_wlan.enable=1 and commit=1.

	  If unsure, say Y on this board.

EOF
add_entry "$B5" "$W/cfg-wlan" 'obj-$(CONFIG_ZX_CSP_WLAN) += zxnpu-wlan.o'
emit 041-zte-add-the-ZX279128S-CSP-WLAN-G3-skeleton.patch b4 b5

echo ALL-DONE
