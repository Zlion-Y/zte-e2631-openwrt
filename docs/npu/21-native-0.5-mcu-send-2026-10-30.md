# native-0.5：最小 MCU 发送路径（实验，双门禁）（2026-10-30）

> 把 §6.10 的 type-1 描述符环模型落成代码：分配 64×36B 描述符环 +
> 1 KiB 命令缓冲，按 `pci_kick_out_cmd_msg` 的字段填描述符，向
> ring 4 (`0x18024440`) 写 BASE/CNT/CIDX 并回读 DIDX。
> 默认只做 dry-run；真正写需要 `zxnpu_wlan.enable=1` +
> `zxnpu_wlan.commit=1` 两个门。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.5/e2631-openwrt-native-0.5.bin` |
| 大小 | 20,972,052 B |
| sha256 | `a9fd1e0242df242790198ac418d62c32b8976d6ad18afcd61e8372be2e89a264` |
| 版本 | `OpenWrt native-0.5` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 命令与描述符

`mcu-send` 构造 `PatchSemGet`：

```
24B Andes 头：len=4, type=0x10, cmd=0x17
payload：sem=0（4B）
```

36B 描述符（与 `pci_kick_out_cmd_msg` 同型）：

```
[0]=16  [4]=buf_dma  [8]=buf_dma+len  [12]=buf_dma
[24]=len  [28]=next_desc_dma
```

提交（仅 commit=1）：`ring4 base=dma; cnt=64; cidx=1`，20 ms 后回读
`cidx/didx`。cidx 前进且 didx 变化 = WFDMA 至少消费了描述符；命令是否被
MCU 接受要看后续响应（本版还没有事件环）。

## 3. 依据与不确定性

- 环块布局：`mt_asic_init_txrx_ring`（0x1E0270）四寄存器写序；
- 描述符字段：`pci_kick_out_cmd_msg`（0x1E19C0）；
- ring 4 作为 MCU 环是 **MTK 惯例推断**，原厂 dump 只覆盖 TX 0..4；
- 因此这是实验路径：写之前先 dry-run 看地址/长度，commit 只动 ring 4。

## 4. 边界

- 没有 RX 事件环，收不到 `CmdPatchSemRsp` 等响应；
- 未加载/未下载固件；无线仍不可用；
- 未刷机。
