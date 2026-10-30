# 41 — R3 构建记录：native-wired-1.8（ethtool 统计 + offload 骨架）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.8/e2631-openwrt-native-wired-1.8.bin` |
| sha256 | `a26e5c5094ed25285eecf6096f37e932fe1c6d1d37c8c9af96d083db4bfa7b62` |
| 版本 | `OpenWrt native-wired-1.8` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | 源码 zxna_setup_tc=2、zxna_get_ethtool_stats=1；vmlinux `ppe offload plan|zxnative-eth` = 6 |

未刷机。

## 2. R3 交付内容（doc 38 R3）

1. **ethtool 统计**：`zxna_ethtool_ops`（get_drvinfo/get_strings/
   get_sset_count/get_ethtool_stats），暴露两个真实驱动计数器
   （tx_pending、tx_done_excess）；`netdev->ethtool_ops` 已接线。
2. **PPE offload 骨架**：`zxna_setup_tc` 处理 TC_SETUP_FT——
   - `zxnative_eth.offload=0`（默认，0444）→ -EOPNOTSUPP，零日志零触碰；
   - `offload=1` → 打印编程计划（IPv4-only、45B key、双 CRC32 bank、
     127 流上限）后仍 -EOPNOTSUPP——**分类器零触碰**。
3. 顺序线遵守：清单 36 第 2 步（FWDL 真机验证）通过前，offload 实编程
   不动（Qoder 划线保留在 doc 37 §3.4）。

## 3. 验收对照（doc 38 R3 标准）

- check.sh：待复跑（本轮变更涉及 zxnative-eth.c 与版本文件）；
- 双门禁：✅；
- 寄存器口径：offload 计划与 doc 12/16 表一致（无硬件写入）✅。
