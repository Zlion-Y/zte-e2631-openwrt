# 52 — R4g：per-STA 上下文结构确认 + R4e 更正（native-wired-2.9）（2026-10-31）

## 1. 更正（自我核验发现，doc 45 §9）

R4e 的"+60 → TXD w0 bit25"解读**作废**：0x1B30C0 的
`ldrb r0, [r9, #60]` 基址 = `max_sta_num + template42*4 + mode`
（mode = 模板+28，0..3）——一个 per-mode 配置表，不是 per-STA 地址。
核验纪律适用于自己的解码（重读 R4e 区域时发现基址对不上）。

## 2. 正确的 per-STA 上下文结构（0x1B307C-0x1B3130）

- 地址 = `arg5 + 0x13BAB0 + wcid × 0x1C50`（arg5 = 固定率 TMAC 第五参，
  per-STA 表基址由调用方传入）；
- wcid ≥ max_sta_num → 地址置 0，整块跳过（广播类，字段从模板 +41 兜底）；
- **ctx+0（u32）= 状态字**：与魔数 **0x00040001** 比较（valid/associated
  类），相等走 0x1B3448 特殊路径（已关联分支）；
- 不等：**ctx+8（指针）→ 指向块 +41 的字节 → TXD w0 bits24+**
  （加密/保护标志来源）。

## 3. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.9/e2631-openwrt-native-wired-2.9.bin` |
| sha256 | `bcbc2719b17af9da177d5421512c38f69ee73578feb7fc845e169dde13201fc4` |
| 版本 | `OpenWrt native-wired-2.9` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | data-tx 打印含 R4g 更正（`ctx+0 state magic 0x00040001`） |

未刷机。

## 4. 意义与下一步

per-STA 条目（stride 0x1C50）：+0 状态魔数、+8 指针（→密钥/保护块）。
P5 AP 状态管理的锚点确认：我们的 wcid 分配器按同布局维护 ctx（状态字 +
密钥块指针）。R4h = 0x1B3448 已关联分支 + 密钥块写入方追溯。
