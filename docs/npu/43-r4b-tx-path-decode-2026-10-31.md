# 43 — R4b：TX 路径架构链解码 + native-wired-2.0（2026-10-31）

## 1. TX 架构链（已核实，地址全部来自 mt7916.ko 反汇编）

```
skb（802.11 帧或 offload 帧）
→ write_tmac_info_offload_pkt(0x1B05E8)：栈上逐字节构建 ~54B TMAC
  （~50 处 strb，值来自 ~15 个来源；逐字节语义 = R4c 任务）
→ TMAC push 进 skb → 入数据环（qid18 BAND0）
→ pci_kick_out_data_tx(0x1E117C)：**纯门铃**——
  一次 hif_io_write32(hif_ctrl, ring+36 值, ring+40 寄存器)，
  即把 CPU_IDX 写进环块 +8 寄存器（doc 14 §6.5.3 布局互证 ✓）
```

TX 环结构与 RX 完全同构（BASE/CNT/CPU_IDX/DMA_IDX 四连寄存器）。

## 2. R4b 结论与边界

- **已解**：kick 机制（CPU_IDX→+8）、TX 环与 RX 环寄存器布局同构、
  TMAC 构建位置与体量（~54B 栈模板）；
- **未解（R4c）**：TMAC ~54B 的逐字节语义——需要追溯 ~50 处 strb 的
  值来源（字段：len/格式标志/目标队列/wcid/加密标志等）；
- **固件侧**：`data-tx` 命令落地为**计划打印机**（不发送）——TX 环
  在 TMAC 追溯完成前保持空（与 doc 42 的声明一致）。

## 3. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.0/e2631-openwrt-native-wired-2.0.bin` |
| sha256 | `2665e19471bc2b8c45177cd6fb4a754ade192ec2c6e17c15a5322d61f4a69b27` |
| 版本 | `OpenWrt native-wired-2.0` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | 源码 `data-tx: plan printer` = 1 |

未刷机。

## 4. 下轮（R4c/R5）

1. R4c：write_tmac_info_offload_pkt 逐字节追溯（预计 2-3 小时）——
   解完后 `data-tx` 从计划打印机升级为真实 TXD 构造器（仍 commit 门禁）；
2. R5：清单 36 真机验收（L2→L4）——需用户解除禁刷。
