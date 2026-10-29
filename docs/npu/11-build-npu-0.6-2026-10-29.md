# npu-0.6 构建记录：vendor MT7916 固件进镜像（2026-10-29）

> 本版把 W 侧 G3 需要的 vendor MT7916 固件/DBDC profile 装进 rootfs，
> 让 `zxnpu-wlan` 的固件审计能真正找到文件。它不是 AP 驱动，也没有
> MT7916 寄存器写；7906 仍由 mt76 驱动。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.6.bin` |
| 大小 | 20,972,052 B |
| sha256 | `4fd565c639634b51c26fc4804f1a4f317970598869ef886547b420073f93d430` |
| 版本 | `OpenWrt npu-0.6` |
| verify_pkg | ALL CHECKS PASSED |
| verify_img | BOOT-READY |
| rootfs | 12,057,225 B -> extent 0xb80000；overlay 仍余 4.50 MiB |

未刷机。

## 2. 新增

- `apply.sh` 在构建时从 `E2631_VENDOR_FW_DIR`（默认 `$E2631_WORK/out/stock-fw`）
  复制 7 个 vendor 文件到构建树的
  `target/linux/sanechips/base-files/lib/firmware/zte/mt7916/`：
  - `WIFI_RAM_CODE_MT7916.bin`（1,759,340 B）
  - `7916_WACPU_RAM_CODE_release.bin`（458,112 B）
  - `mt7916_patch_e1_hdr.bin`（8,672 B）
  - `MT7916_EEPROM.bin`（4,096 B）
  - `l1profile.dat.dbdc` / `mt7916.dbdc.b0.dat` / `b1.dat`（560/7,297/7,382 B）
- 文件不提交进仓库；没有 vendor 物料时构建继续，`zxnpu-wlan` 状态会显示 missing。
- `README.vendor-fw` 记录来源与用途。

## 3. 见证

- rootfs 解包：`lib/firmware/zte/mt7916/` 下 7 个文件 + README 全在。
- `verify_pkg`：rootfs extent 0xb80000，rootfs partition 0x1000000，overlay 4.50 MiB。
- 内核组件：`CONFIG_ZX_CSP_WLAN=y`、`zxnpu_wlan_probe/remove`；IDM/PPE 仍在。

## 4. 边界

- 仍没有 MT7916 MCU/WFDMA/TXD/AP；790a 默认不 probe。
- 固件只是“到位”，没有下载/校验流程。
- 设备未动。

## 5. 下一步

- G3.2：在 790a 上做只读 chip ID/固件版本探测，确认 BAR/寄存器可读；
  再决定是否把主功能切到自研驱动。
- 然后补 `WfMcuInit → WfMcuSysInit → chip_fw_init → NICLoadRomPatch →
  NICLoadFirmware` 的下载骨架。
- A 侧并行：CLA 45B key/rule 的最小 flow add/delete。
