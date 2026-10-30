# native-0.4：MCU 门铃窗口探针（2026-10-30）

> 在 0.3 的有线基线之上，`zxnpu-wlan` 增加 `mcuwin`：只读扫描每个
> 内存 BAR 的 +0/+8/+0x108，用来确定原厂 MCU 门铃 `0x54000108`
> 到底落在哪个 BAR 窗口。没有新增任何写操作。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.4/e2631-openwrt-native-0.4.bin` |
| 大小 | 20,972,052 B |
| sha256 | `a100bde9d1f150b31466500918af8f938f376a81b628194028ec142548db0b0b` |
| 版本 | `OpenWrt native-0.4` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021 硬件层）+ npu 驱动族；无线仍无 |

未刷机。

## 2. 门铃与窗口

- `pci_trigger_int_to_mcu`（0x191DA0）= `mac_io_write32(priv,
  0x54000108, val)`，即门铃在 `0x54000000` 窗口；
- `mt7916_init` 把它写进 ops 槽 `0x1D4`（见 doc 14 §6.9），
  同表还有 `MtCmdSendMsg@0xFC`、`fwdl_datapath_setup@0xD4`、
  `fw_prepare@0x100`、三个 PCIe 中断处理；
- `chip_show_dma_info` 在结尾还读了 `0x54002500/0x54000500/0x54000504`，
  说明 0x54 窗口里还有一组 WFDMA1 寄存器。

`mcuwin` 对每个 `IORESOURCE_MEM` BAR 且长度 ≥ 0x10c 的窗口打印
`+0/+8/+0x108`；上机后把 0x54000108 的归属 BAR 与 `rings` 的环值
对照，就能确定 H2D 通道的访问方式。

## 3. 接口

`/proc/zx-csp-wlan/ctl`（`zxnpu_wlan.enable=1`）：

```
probe | fw | plan | mcu | rings | mcuwin | commit
```

`mcuwin` 只读；`commit` 仍需 `zxnpu_wlan.commit=1`。

## 4. 边界

- 仍未发送任何 MCU 命令；固件下载（PatchSemGet/AddressLen/Finish）
  只有编码器与计划。
- 未刷机。
