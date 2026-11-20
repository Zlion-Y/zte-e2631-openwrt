# OpenWrt for Sanechips ZX279128 SoC Family

**ZTE ZXHN E2631 · ZTE ZXHN E2615 · CMCC RAX1800Z · ZTE ZXHN H3600**

[English](README.md) · [简体中文](README.zh-CN.md)

This repository provides an OpenWrt port, modular overlay, hardware toolset, and firmware packaging engine for wireless routers powered by the **Sanechips (ZTE Microelectronics) ZX279128 SoC family** (ARM Cortex-A9 dual-core @ 1.0 GHz).

Built on upstream Linux 6.18 LTS and OpenWrt master, this project delivers production-ready firmware with **non-destructive A/B dual-slot recovery**, hardware flow offloading (PPE/FFE), dynamic RF caldata injection, multi-gigabit routing, and global locale defaults.

Firmware releases and pre-built binaries: [Releases](https://github.com/Zlion-Y/zte-e2631-openwrt/releases).  
Build guide and packaging workflow: [BUILD.md](BUILD.md).  
Hardware issues, errata, and verification notes: [docs/known-issues.md](docs/known-issues.md).

---

## 1. Supported Hardware Matrix

| Device Model | SoC | Wi-Fi Subsystem | Wired Ethernet | Flash / RAM | Port Status |
|---|---|---|---|---|---|
| **ZTE ZXHN E2631** (AX3000) | ZX279128S (Cortex-A9 ×2 @ 1 GHz) | MT7916AN + MT7976DN (DBDC 2×2, 160MHz) | 4× GbE (Internal ZX5201 PHY) | 128 MiB SPI-NAND / 256 MiB DDR2 | ✅ Verified on Hardware |
| **ZTE ZXHN E2615** (AX1800) | ZX279128S (Cortex-A9 ×2 @ 1 GHz) | MT7915D (DBDC 2×2, 80MHz) | 4× GbE (Internal ZX5201 PHY) | 128 MiB SPI-NAND / 256 MiB DDR2 | ✅ Verified on Hardware |
| **CMCC RAX1800Z** (AX1800) | ZX279128R (Cortex-A9 ×2 @ 1 GHz) | MT7975DN + MT7905DEN (DBDC 2×2, 80MHz) | 4× GbE (Internal ZX5201 PHY) | 128 MiB SPI-NAND / 256 MiB DDR2 | ⚠️ Ported (`Makefile.e2631`) |
| **ZTE ZXHN H3600** (AX1800) | ZX279128S (Cortex-A9 ×2 @ 1 GHz) | MT7915 (DBDC 2×2, 80MHz) | 4× GbE + 1× 2.5GbE | 128 MiB SPI-NAND / 512 MiB DDR3 | 🔷 Upstream Baseline Ref |

---

## 2. Platform Architecture & Subsystems

```
+--------------------------------------------------------------------------+
|                     Sanechips ZX279128 SoC Platform                      |
|       Dual ARM Cortex-A9 @ 1.0 GHz · Soft-Float (VFP Disabled)           |
+--------------------+---------------------+-------------------------------+
|   Switch Subsystem |  Wireless (PCIe)    |   Storage / Partitioning      |
|  - 4x Internal GbE |  - MT7916 / MT7915  |  - 128 MiB SPI-NAND           |
|    PHY (ZX5201)    |  - mt76 driver      |  - A/B Dual Slot (28 MiB ea)  |
|  - Independent     |  - Factory caldata  |  - Dynamic slotpart parser    |
|    netdev per port |    hotplug parse    |  - 47 MiB userdata extroot    |
|  - Manual WAN swap |  - Dual-band AP     |  - In-band JFFS2 markers      |
+--------------------+---------------------+-------------------------------+
|                       Hardware Packet Processing                         |
|  - PPE Classifier & Flow Modifier (0x92000000)                           |
|  - IPv4 Hardware Flow Offloading (>=395 Mbps @ 7% CPU)                   |
|  - IPv6 Software Fast-Path with Hardware Trap Protection                 |
+--------------------------------------------------------------------------+
```

### Feature Implementation Status

* ✅ **Non-Destructive Dual-Slot Booting**: Flash partitions are dynamically divided into Slot 0 (`0x00700000`) and Slot 1 (`0x02300000`). Upgrades overwrite only the inactive slot; bootloader (`cspboot`) automatically boots the slot with valid CRC and higher sequence serial. Reverting to stock takes 5 seconds via `e2631-fwslot switch`.
* ✅ **Global Network & Locale Configuration**:
  - Web Interface: English (`en`) LuCI standard interface.
  - Timezone: UTC with global NTP pool (`*.openwrt.pool.ntp.org`).
  - Network: Standard `192.168.1.1/24` LAN DHCP, WAN auto-configured.
* ✅ **Gigabit Wired Switching (4 Ports)**:
  - Internal ZX5201 PHYs exposed as independent netdevs (`eth0`, `eth1`, `eth2`, `eth3`).
  - Arbitrary WAN port remapping supported on-the-fly (`e2631-wanport apply <port>`).
* ✅ **Hardware Flow Offloading (IPv4)**:
  - Hardware PPE engine handles wire-speed NAT/routing offload for IPv4.
  - Up to 384 concurrent offloaded flows; seamless fallback to CPU path on saturation.
  - IPv6 flows are trapped to kernel software path to prevent payload corruption.
* ✅ **Extroot Expansion (47 MiB Userdata)**:
  - Flash partition `0x03F00000 - 0x06E00000` automatically initialized as UBIFS on first boot, providing ample persistent storage for custom packages and services.
* ✅ **NAND Bitflip & ECC Resilience**:
  - Kernel parser `zx279128s-slotpart.c` tolerates correctable SPI-NAND bitflips.
  - JFFS2 clean markers written in-band (data area) to prevent multiple-write corruption on on-die ECC SPI-NAND chips.
* ❌ **USB Controller**:
  - Permanently disabled in device trees (`status = "disabled"`). Unpopulated hardware causes SoC watchdog hang when probed.

---

## 3. Quick Start: Building Firmware

All build operations run in standard Linux (Ubuntu 22.04 LTS / 24.04 LTS, Debian 12, or WSL2).

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y build-essential clang flex bison g++ gawk gcc-multilib \
    gettext git libncurses-dev libssl-dev python3 python3-setuptools rsync \
    swig unzip zlib1g-dev file wget curl busybox libelf-dev xsltproc nodejs
```
*Note: Node.js 20.15+ (or 22 LTS) is required for firmware image packaging (`tools/mkfactory.mjs`).*

### Building the Firmware

1. **Bootstrap the workspace**:
   ```bash
   bash scripts/bootstrap.sh    # or: make bootstrap
   ```
   This checks out the pinned OpenWrt Sanechips tree (`db8b8d4`), installs required feeds (`packages`, `luci`), applies target patches, and injects default device configuration.

2. **Select Target Device (Optional)**:
   By default, `seed/e2631.config` targets the **ZTE ZXHN E2631**.
   To target the **ZTE ZXHN E2615** or **CMCC RAX1800Z**, customize `.config`:
   ```bash
   cd ~/openwrt
   # For E2615:
   # CONFIG_TARGET_DEVICE_sanechips_generic_DEVICE_zte_zxhn-e2615=y
   # For RAX1800Z:
   # CONFIG_TARGET_DEVICE_sanechips_generic_DEVICE_cmcc_rax1800z=y
   make defconfig
   cd -
   ```

3. **Verify Pre-flight Gates**:
   ```bash
   make check                   # or: bash scripts/check.sh
   ```
   Validates shell scripts (`bash -n`), JavaScript tools (`node --check`), DTS cross-references, and ensures no private credentials exist.

4. **Compile and Package**:
   ```bash
   bash scripts/build.sh        # or: make build
   ```
   Compiles OpenWrt toolchain, kernel, rootfs, and packages the binary using the embedded vendor version-header template (`seed/vendor-verhdr-8b8.bin`). Output packages are placed in `out/`.

---

## 4. Installation & Upgrades

### Method 1: Vendor Web GUI First-Flash (Zero Risk)
1. Boot router into stock factory firmware.
2. Log into the web admin console (`192.168.5.3` or `192.168.2.1` depending on model).
3. Navigate to the hidden upgrade page (`/supgrade.html` on port 8085 or `/firmware_upgrade`).
4. Select the generated `.bin` package and upload.
5. Router will verify CRC, write to the secondary inactive slot, increment boot serial, and restart into OpenWrt. Stock firmware remains untouched in the original slot.

### Method 2: In-System Sysupgrade (OpenWrt to OpenWrt)
```bash
sysupgrade -v /tmp/openwrt-sanechips-upgrade.bin
```
The upgrade script validates the inactive slot and writes the payload without breaking dual-boot recovery.

### Reverting to Factory Firmware
To immediately boot back to stock firmware from OpenWrt:
```bash
e2631-fwslot switch
reboot
```
This increments the factory slot sequence counter, triggering `cspboot` to switch back on next power cycle.

---

## 5. Port Management & CLI Tools

| Command | Function |
|---|---|
| `e2631-fwslot info` | Display current and alternate slot state, kernel addresses, version headers, and boot priority |
| `e2631-fwslot switch` | Swap active boot slot (OpenWrt <-> Stock Factory) |
| `e2631-wanport show` | Display current WAN/LAN physical port assignments and link state |
| `e2631-wanport apply <port>` | Reassign WAN uplink to `wan`, `lan1`, `lan2`, or `lan3` |
| `e2631-upgrade check` | Check GitHub Releases for newer verified firmware |
| `e2631-leds` | Status LED daemon controlling RGB tri-color boot and connectivity indicator |

---

## 6. Acknowledgements & References

* **[@ghahramani](https://github.com/ghahramani)**: Upstream pioneer of the ZTE ZXHN H3600 OpenWrt port ([openwrt/openwrt#25469](https://github.com/openwrt/openwrt/pull/25469)), mainline Linux RFC, and technical architecture analysis.
* **[@zwzzrl](https://github.com/zwzzrl/zte-e2631-openwrt)**: Manual WAN port remapping architecture, PPE flow offloading analysis, IPv6 TCP trap discovery, and hardware wireless reboot recovery.
* **[@zwthys-cyber](https://github.com/zwthys-cyber/zte-e2631-openwrt)**: Upgrade hardening routines, mandatory SHA256 checksum enforcement, and CI infrastructure.
* **OpenWrt & Linux Kernel Projects**: Upstream maintainers of `mt76`, network subsystems, and the ARM Cortex-A9 architecture.
