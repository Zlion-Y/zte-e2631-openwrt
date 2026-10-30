# native-0.9：MCU 命令环校正到 0x24410 + 固件 trailer（2026-10-30）

> 用 mt76 的 MT7916 队列表核对出上一版的关键错误：`0x24440` 不是
> MCU WM 命令环，而是 MCU WA（qid 20）。主机→固件命令环是
> **`0x18024410`（qid 17）**。本版把 `mcu-send`/`mcu-cmd`/`mcu-dl`
> 全部改到该环；RX 事件环 `0x18024500`（qid 0）原本正确。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.9/e2631-openwrt-native-0.9.bin` |
| 大小 | 20,972,052 B |
| sha256 | `db7d36fd3b978eefe739ca64ab51f60d21410ca836cb544e64b9f815748b3e67` |
| 版本 | `OpenWrt native-0.9` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 队列映射（硬件常量核对）

```
TX qid 16 FWDL   0x18024400
   qid 17 MCU_WM 0x18024410   ← 命令环
   qid 18 BAND0  0x18024420
   qid 19 BAND1  0x18024430
   qid 20 MCU_WA 0x18024440
RX qid 0 MCU_WM  0x18024500   ← 事件环
   qid 1 MCU_WA  0x18024510 … qid 5 BAND1 0x18024550
```

`ring_base = WFDMA0(0x24000) + 0x300 + qid*0x10`，与
`MT_RING_SIZE=0x10` 和原厂 `chip_show_dma_info` 的转储清单完全一致。

## 3. 固件 trailer

`WIFI_RAM_CODE_MT7916.bin` 尾部 36B 是 MTK fw trailer：chip_id 0x13、
n_region 9、format_ver 2、date "20250103112717"、crc 0xf9b7d222。
region 表是原厂 format 2 布局，不是上游 44B 布局；解出
`parse_fw_v1` 的字段偏移后才能自动下载 RAM code。

## 4. 边界

- 仍未在硬件上验证 DIDX/事件；未刷机。
