# native-0.6：RX/事件环与 WFDMA 使能（实验，双门禁）（2026-10-30）

> 在 0.5 的 MCU 发送路径旁补齐接收侧：32×36B 描述符 + 32×1700B
> 缓冲（每块头部 16B RXD），环寄存器在 RX ring0 `0x18024500`，
> `rx-init` 提交时才写并使能 WFDMA（GLO_CFG bits 0/1/2）。
> `mcu-recv` 只读 DIDX、打印落在这个环上的原始字节并回收 CIDX。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.6/e2631-openwrt-native-0.6.bin` |
| 大小 | 20,972,052 B |
| sha256 | `d7a5d384bd0f4795f885295c16e0544e880430600dbd81c8c7ed3c32a726f59c` |
| 版本 | `OpenWrt native-0.6` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 依据

- RX 缓冲/RXD 预填：doc 6.4（`pci_init_txrx_ring_mem` 0x1E4A64）：
  1700B/块，头部 16B RXD（+0 DMA、+6 长度低字节、+7 0x40|长度高字节）；
- 描述符：36B，`[0]=16 [4]=buf [8]=buf+size [12]=buf [24]=size [28]=next`；
- 环寄存器：`mt_asic_init_txrx_ring` 的 BASE/CNT/CPU_IDX/DMA_IDX 四连；
- RX ring0 承载 MCU 事件是 MTK 惯例推断，未真机验证。

## 3. 接口

```
rx-init     # dry-run 打印；commit=1 时写环并使能 WFDMA
mcu-recv    # 读 didx，打印 pending 数与每槽前 32 字节，回收 cidx
```

## 4. 边界

- 事件格式未解；本版只保证能“看到”落到 ring0 的字节。
- 没有固件下载状态机；未刷机。
