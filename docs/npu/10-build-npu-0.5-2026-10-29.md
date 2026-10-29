# npu-0.5 构建记录：MT7916 G3 骨架（2026-10-29）

> 这是 W 侧的第一个内核组件：只绑定第二个 PCIe 功能 `14c3:790a`，
> 默认不 probe；开启后只做 PCIe 映射、配置/BAR 读取和 vendor 固件文件校验，
> **不写任何 MT7916 寄存器**。它还不是 AP 驱动，也不影响 mt76 的 7906。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.5.bin` |
| 大小 | 20,972,052 B |
| sha256 | `a3955389a886ebc4775274ad912ff92b079b7334a80c9a3eeb2d4fb8dac88d7c` |
| 版本 | `OpenWrt npu-0.5` |
| verify_pkg | ALL CHECKS PASSED |
| verify_img | BOOT-READY |

未刷机。

## 2. 新增

`zxnpu-wlan.c`（`CONFIG_ZX_CSP_WLAN=y`）：

- PCI driver `zxnpu-wlan`，id table 只匹配 `14c3:790a`（7906 留给 mt76，
  避免把当前 WiFi 弄死）。
- `zxnpu_wlan.enable=0` 时 probe 直接 `-ENODEV`，设备保持未绑定。
- `enable=1` 时：
  - `pcim_enable_device` + `pcim_iomap_regions`（BAR0/1/2）；
  - 读 vendor/device/revision/command/status/BAR 资源；
  - `request_firmware_direct` 检查 `zte/mt7916/{WIFI_RAM_CODE, WACPU,
    patch, EEPROM}`，算大小/CRC32；
  - 创建 `/proc/zx-csp-wlan/status`。
- **没有任何 MT7916 寄存器写**；这是 G3.1 的只读阶段。

## 3. 见证

```
CONFIG_ZX_NPU_IDM=y
CONFIG_ZX_NPU_PPE=y
CONFIG_ZX_CSP_WLAN=y
```

System.map：`zxnpu_wlan_probe/remove`；`zxnpu_idm_*`、`zxnpu_ppe_*` 仍在。

## 4. 边界

- 790a 只是 companion function；主固件/AP 仍在 7906，由 mt76 驱动。
- vendor 固件文件目前不在镜像里（`request_firmware_direct` 会显示 missing）；
  下一步要加一个只含 vendor 固件路径的包，或在镜像里放 `/lib/firmware/zte/mt7916/`。
- 没有 MCU 命令、WFDMA、TXD、AP 管理。

## 5. 下一步

- 打包 vendor MT7916 固件/配置到镜像（本地构建，不入仓库大文件）。
- G3.2：在 790a 上只读 chip ID/固件版本；再决定是否主功能切换。
- A 侧并行：CLA 45B key/rule 的最小 flow add/delete。
