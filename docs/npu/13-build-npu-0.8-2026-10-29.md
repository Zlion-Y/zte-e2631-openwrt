# npu-0.8 构建记录：W 侧 G3.2（芯片探针 / WFDMA 计划 / 固件头）（2026-10-29）

> 本版把 MT7916 的“怎么访问寄存器、初始化要写哪 22 个值、固件
> 下载命令长什么样”从反汇编落到代码：只读探针、WFDMA 计划、
> 固件头解析全部可用；真正写寄存器要同时满足 `enable=1` 和
> `commit=1`，默认不会碰硬件。7906 仍由 mt76 驱动。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/npu-0.8/e2631-openwrt-npu-0.8.bin` |
| 大小 | 20,972,052 B |
| sha256 | `e25434208dae6d3eb3558ba440788268a19b78432e49d6a1d5d41736c8e1438c` |
| 版本 | `OpenWrt npu-0.8` |
| verify_pkg / verify_img | ALL CHECKS PASSED / BOOT-READY |

## 2. CR 空间：`0x18000000` 直接是 BAR0 偏移

`int_pci_io_write32`（0x1E0D24）只做 `*(u32 *)(hif_ctrl->base + addr) = val`，
`mt_mac_cr_range_mapping`（0x1F6978）负责把 `0x1802xxxx` 之类的
MAC CR 地址映射成 BAR 窗口偏移。对 `0x18000000` 段，映射就是
`addr - 0x18000000`，即：

```
BAR0 偏移 0x24208 = MAC CR 0x18024208
```

`zxnpu-wlan` 因此用 `ZXNPU_CR(addr)` 直接读写 BAR0；越界检查用
`pci_resource_len(bar0)`，任何超界访问都静默跳过。

## 3. `hif_init_WFDMA`（0x195B6C）的 22 次写

| 序号 | 寄存器 | 值 | 说明 |
|---|---|---|---|
| 1 | `0x18024208` | RMW：清 bit27，置 bit28+bit21 | WFDMA 全局配置 |
| 2-7 | `0x18024680..0x18024694` | `(1..6)<<22 \| 4` | 环 1..6 信用 |
| 8-12 | `0x18024640..0x18024650` | `(6..10)<<22 \| 4` | 环 6..10 信用（6 重复出现，原厂如此） |
| 13 | `0x1802420c` | `0xffffffff` | 中断使能 |
| 14 | `0x180242f4`（或 `0x180282f4`） | `0x811c0000` | PCIe 窗口；第二 MAC 代用 0x82xx |
| 15 | `0x1802702c` | RMW：清 `0x1fc0000\|0x20000`，置 `0x2800000` | TX 基址/窗口 |

路径分歧来自 `hc_get_asic_cap() & 0x80000`：命中走 `0x1802_82xx`，
否则走 `0x1802_42xx`。790a 属于第二函数，代码默认选 `0x82f4`，
并在输出里标为推断。

## 4. 固件下载命令（`MtCmd*`，全部 cmd=0x17）

原厂 `load_patch_v1`（0x1D5C0C）依次发三条 MCU 命令，Payload 都是
小端 32 位：

| 函数 | 类型字节 | cmd | Payload |
|---|---|---|---|
| `MtCmdPatchSemGet`（0x1CBFEC） | `0x10` | `0x17` | `sem`（4B） |
| `MtCmdAddressLenReq`（0x1CBDA8） | `0x05`（地址等于 chip_cap+0x12C 时）/ `0x01` | `0x17` | `addr, len, arg`（12B） |
| `MtCmdPatchFinishReq`（0x1CC0CC） | `0x07` | `0x17` | 无 |

命令封装走 `AndesAllocCmdMsg` / `AndesInitCmdMsg` / `AndesAppendCmdMsg`
/ `chip_cmd_tx`（0x142C70），最终落到 WFDMA 的 MCU 队列。类型字节
位于 Andes 命令头第 2 字节、cmd 位于第 4 字节——这是从三条命令
共同的 `strb` 偏移（sp+18 / sp+20）读出来的 [`静态`]。

## 5. 固件文件格式

`mt7916_patch_e1_hdr.bin`（8,672 B）是 ROM patch，前 48 字节：

```
0x00  date[16]       "20250103112625a\n"
0x10  platform[4]    "ALPS"
0x14  u32le          0x108a108a
0x18  u32le          0xffffffff
0x1c  u32le          0x00000000
0x20  u32le          0x11223344   <- patch magic
0x24  u32le          0x04000000   <- MCU SRAM 基址
0x28  u32le          0x00000000
0x2c  u32le          0x00000001
0x30  u32le          0xffff0000
```

`WIFI_RAM_CODE_MT7916.bin`（1,759,340 B）与
`7916_WACPU_RAM_CODE_release.bin`（458,112 B）没有头，首字节即
Thumb 代码（`0f3e c04e …` / `6f3a bcb4 …`），由 `mt_load_fw`
按 chip_ops 给出的地址分段投递。

## 6. 接口

`/proc/zx-csp-wlan/status`（只读）在 probe 时给出：PCI 配置、
BAR 起止、固件 CRC、patch 头字段、CR 探针值。

`/proc/zx-csp-wlan/ctl`（只写）：

| 命令 | 作用 |
|---|---|
| `probe` | 重新读 10 个 CR 并打印 |
| `fw` | 重新解析 3 个固件文件头 |
| `plan` | 打印 22 条 WFDMA 写计划（含当前值），不写 |
| `commit` | 真正执行；`zxnpu_wlan.commit=0` 时拒绝并说明原因 |

模块参数：`enable=1` 才 probe 790a；`commit=1` 才允许 `commit`。

## 7. 边界

- 仍不是 AP 驱动：没有固件下载、没有 MCU 事件环、没有 TX/RX ring。
- `commit` 会改 790a 的 WFDMA 配置；mt76 未绑定该功能，但若同机
  另有驱动占用，先确认再开。
- 未刷机。
