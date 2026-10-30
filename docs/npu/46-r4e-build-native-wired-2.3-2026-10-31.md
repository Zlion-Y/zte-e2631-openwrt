# 46 — R4e 构建记录：native-wired-2.3（TXD 字落点 + 速率参数结构）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.3/e2631-openwrt-native-wired-2.3.bin` |
| sha256 | `53eb4f26d3d348dc…`（见打包日志全值） |
| 版本 | `OpenWrt native-wired-2.3` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. R4e 解码增量（doc 45 §5）

- **TXD 字落点**：r4+12（bit31 置位 + 模板 +14 u12 → bits16-27）、
  r4+20（模板 +46→bit9、+45→bit10、+12 原值 OR、+47→bit14）、
  r4+24（速率参数字：r6[0] bit6→#2048、r6[1] bit1→lsl14、
  r6[1] bits5-3=phy mode、r6[0] bits0-5=MCS/rate）；
- r6 = 定速率参数结构（byte0 bits0-5=MCS、byte1 bits5-3=phy mode、
  sxtb 判负走特殊分支）；
- 剩余 R4f：r4+0/+4/+8 三字来源 + skb 头衔接。

## 3. 备注

- 本轮打包曾出现瞬时 spawn 失败（凌晨连续构建 9 版后的系统资源压力），
  重跑成功——非树问题（kernel/rootfs 体积与 magic 均正常）；
- data-tx 输出已带 R4e 位图。
