# 51 — R4e 构建记录：native-wired-2.8（wcid + per-STA 上下文地址）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.8/e2631-openwrt-native-wired-2.8.bin` |
| sha256 | `72979c79219b9c15aedf7f33e522066db39d2d5b2a0ef3978eceeef41d2bf327` |
| 版本 | `OpenWrt native-wired-2.8` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. R4e 解码增量（doc 45 §7）

- **模板 +0 u16 = wcid**（与 `HcGetMaxStaNum` 比较——mt76 connac2 TXD
  dw1 wcid 位域同位，命名参考一致）；
- **per-STA 上下文地址 = 0x13BAB0 + wcid × 0x1C50**（+ 模板 +42×4 子表
  选择）；wcid ≥ max → 基址不加（广播类）；
- per-STA 条目 +60 字节 → TXD word0 bit25（加密/保护标志类）；
- per-STA 条目 stride 0x1C50 = 7248B（大条目：密钥/速率/能力全量）。

## 3. 意义

wcid 字段确认 = P5 AP 状态的第一块拼图：真实 TXD 构造所需的 wcid
取值来源（我们的 AP 状态管理按同布局分配）已在证据链上。

## 4. 边界

- per-STA 条目内部字段（+60 之外的密钥/速率区）= R4g（可与 P5 的
  wcid 分配器一并做）；
- 未刷机。
