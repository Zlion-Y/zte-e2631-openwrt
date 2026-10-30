# 44 — R4c：TMAC 52 字节布局追溯 + native-wired-2.1（2026-10-31）

## 1. TMAC 布局（write_tmac_info_offload_pkt 0x1B05E8，逐 strb 追溯）

TMAC = **52 字节**（`__memzero(sp+4, 52)` 后逐字节填充）。字段位置 →
值来源（全部带指令地址证据，源在 per-STA 结构 r4 上）：

| 偏移 | 来源 | 备注 |
|---|---|---|
| +4/+5 | u16 @ sta+3552 | `ldrh [r2]` |
| +7 | 常数 1 | |
| +24 | arg5 (u32) | 栈参 |
| +28 | 常数 0x18 | 长度/标记类 |
| +31 | sta+14（s8 → +40 sxtb） | |
| +34 | 条件字节（bit 判定） | |
| +35/+36 | 常数 1 | |
| +37/+38 | 参数 r7/r6 | |
| +45 | sta+41 | |
| +48 | sta+3528（类型，cmp #2 分支） | |
| +52 | sta+3420 | |
| +53 | 常数 1 | |
| +6/+9/+11/+12/+15/+17/+20/+21 | 零 | |

## 2. 函数家族定性（关键发现）

`write_tmac_info_offload_pkt` 是 **offload/管理帧路径**：
- `HcGetBcnQueueIdx`(0x1b06a4) / `HcGetMgmtQueueIdx`(0x1b072c) —— 队列选择
- `HcGetBandByWdev`(0x1b06e8) —— band 推导
- `asic_write_tmac_info_fixed_rate`(0x142404) —— 定速率帧的 TMAC 变体
- 尾部 debug 块：sta+8 能力字 bits 16..31 逐 bit printk —— **与
  npu_cap/zwzzrl layer_cap 位掩码语义同族**（doc 16 §7 交叉印证）

**数据帧 TMAC 构造器 = `asic_write_tmac_info_fixed_rate` 家族（0x142404）
——R4d 目标。**

## 3. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.1/e2631-openwrt-native-wired-2.1.bin` |
| sha256 | `473aa475216c7e17027e1c2127e345f772a01534157700cd4f0aa69ed68ed0d1` |
| 版本 | `OpenWrt native-wired-2.1` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | 源码 `TMAC template printer` = 1 |

## 4. 升级与边界

`data-tx` 从计划打印机升级为 **TMAC 模板打印机**（52B 布局 + 字段来源
全部可离线核对）——但**真实 TXD 构造仍不可行**：字段语义（哪字节 =
wcid/队列/加密）需要 per-STA 结构上下文 + 我们自己的 AP 状态（wcid 分配
在 P5）。R4d = `asic_write_tmac_info_fixed_rate` 家族逐字节追溯 + sta
结构字段命名。未刷机。
