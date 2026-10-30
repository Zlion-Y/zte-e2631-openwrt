# 42 — R4 构建记录：native-wired-1.9（WFDMA 数据面骨架）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.9/e2631-openwrt-native-wired-1.9.bin` |
| sha256 | `eae4d0fd47d5a0cc8ab806a14784d7a6a7d272c846feaf6583b837d7b9f9febd` |
| 版本 | `OpenWrt native-wired-1.9` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | 源码 data-init/dataplane=4；vmlinux `dataplane map|data-init` = 18 |

未刷机。

## 2. R4 交付（doc 38 R4：TX/RX 数据环 + 队列→VAP 映射）

新命令（全部 commit 门禁内，`dataplane` 纯只读）：

- **`data-init`**：分配并编程两个数据环——
  RX qid1（`0x18024510`，64 深 × 36B 自描述描述符，1700B 缓冲 +
  16B RXD 预填[DMA/len/0x40]，vendor `pci_init_txrx_ring_mem` 同约定）、
  TX qid18（BAND0，环寄存器已编程但**保持空**——connac2 TXD 格式
  尚未从 vendor 二进制解出，如实标注）；GLO_CFG 使能位、
  BASE/CNT/CPU_IDX=0/DMA_IDX=0 按 doc 14 §6.5.3 已核布局；
- **`data-recv`**：数据环收帧器（cidx/didx 步进 + 首帧 32B dump +
  commit 后回写 CPU_IDX）——真机第一帧 WiFi 数据帧的对照入口；
- **`dataplane`**：队列→VAP 映射表（BAND0 qid18→wlan0、BAND1
  qid19→wlan5g0、事件 qid0；l1profile main_ifname 命名）+ NAPI 在
  P5（VAP netdev 出现后）附加的说明。

## 3. 验收对照（doc 38 R4 标准）

- 离线：双门禁 ✅、寄存器布局 doc 14 §6.5.3 一致 ✅、TXD 缺口如实标注 ✅；
- 真机（R5）：`data-init` 后 `data-recv` 应见关联客户端的首帧——
  L4/L6 判据，等刷机窗口。

## 4. 边界

- connac2 TXD 格式未解码 → TX 环空转（骨架），下一轮解
  `pci_kick_out_data_tx`（0x1E1AD4 族）补齐；
- 未刷机。
