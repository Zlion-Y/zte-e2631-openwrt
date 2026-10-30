# native-0.2：WFDMA 环只读探针（2026-10-30）

> 接手 zcoZCode 的 native 线后的第一个检查点。镜像仍然没有数据面
> （native 0.1 的既定状态），新增的是 G3.3 下一步需要的环级观测：
> `/proc/zx-csp-wlan/ctl` 写 `rings` 会读 11 个 WFDMA 环块 + band 镜像
> 的 BASE/CNT/CPU_IDX/DMA_IDX，不写任何寄存器。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.2/e2631-openwrt-native-0.2.bin` |
| 大小 | 20,972,052 B |
| sha256 | `5b0a508e505bd4b1dd990b852b3e4b2186095866df3425faad4f1ac25d389472` |
| 版本 | `OpenWrt native-0.2` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 无（与 0.1 相同；021/034 已摘除） |

未刷机。

## 2. 环寄存器的来源

`chip_show_dma_info`（mt7916.ko 0x1A9188）是原厂的环转储器：

```
TX 环 0..4：0x18024400 + 0x10*n
RX 环 0..5：0x18024500 + 0x10*n
每块 4 个寄存器：BASE / CNT / CPU_IDX / DMA_IDX
第二代 MAC 镜像：同表 +0x4000（0x1802_84xx/85xx）
```

`dump_dma_tx_ring_info`（0x2FE8）与 `dump_dma_rx_ring_info`（0x3094）
逐项读 +0/+4/+8/+0xC，并给出 TX 空闲 = cidx−didx、RX 待收 =
didx−cidx−1 的算法。`mt_asic_init_txrx_ring`（0x1E0270）的写序是：

```
TX: write(base+0, desc_dma)；write(base+8, CNT-1)；write(base+4, CNT)
RX: write(base+8, desc_dma)；write(base+8, 0)；write(base+4, CNT)
```

## 3. 接口

`/proc/zx-csp-wlan/ctl`（只写，需要 `zxnpu_wlan.enable=1` 才 probe 设备）：

| 命令 | 作用 |
|---|---|
| `rings` | 读 5 个 TX + 6 个 RX 环块（band0），再读 +0x4000 镜像 |
| `probe` | 既有 10 个 CR 探针 |
| `fw` / `mcu` / `plan` / `commit` | 0.1 已有的固件头 / Andes 命令编码 / WFDMA 计划 |

`rings` 是纯读取；即使 enable=1 也不会改设备状态。

## 4. 边界

- 读到的是 mt76 或其他驱动当前留在环里的值；空白环全 0 也是有效信息。
- 仍没有 MCU 发送、固件下载、TX/RX 数据环。
- 未刷机；上机核对需要用户放行。
