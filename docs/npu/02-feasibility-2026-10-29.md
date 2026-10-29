# npu 可行性快照：A + W1 能做到什么程度（2026-10-29）

> 范围：保留 OpenWrt 通用用户态（A），第一版做“最小 AP + 有线/无线加速”（W1）；
> 设备驱动、FFE 控制面、WiFi 私有栈全部自研。本文只做**静态可行性判断**，
> 不代表已实现；所有结论标 `[实测]`/`[实测-静态]`/`[推断]`/`[未知]`。

## 0. 先给结论

| 目标 | 判断 | 大概量级 | 依据 |
|---|---|---|---|
| 有线原厂栈（1G 线速、IPv4/IPv6/PPPoE/硬件 NAT） | **可做，高置信** | 8-12 周 | 参考驱动 21,261 行 + `tm/plat` 反汇编 + 运行态基线 |
| FFE 控制面（替代 `nf_flow_table`） | **可做，中高置信** | 4-6 周 | 261 个 FFE 符号、13 个关键函数反汇编、`ffe-reconstructed.h` |
| MT7916 最小 AP + IDM 加速 | **有机会，中低置信，必须先 spike** | 4-6 个月 | 7,760 函数/2.59 MiB `.text`，但 ZTE 侧只有 9 个 import；固件/配置齐全 |
| 原厂全功能无线（8 VAP/MESH/APCLI/WPA3/160M/漫游） | **不建议首版做，低置信** | 6-12+ 个月 | WSC/WPA/MLME/AP profile 体量巨大 |
| 用户态 B（去掉 OpenWrt） | **不建议首版做** | +2-4 个月 | netifd/fw4/LuCI/DHCP/NAT 全重写 |

**一句话**：**“能上网、有加速、单/双 SSID 的路由器”是现实目标；
“原厂全功能无线”不是第一版目标。** 有线和 FFE 先落地，无线用 G3/G4 两个门禁决定是否继续。

## 1. 已有资产的“硬指标”

### 1.1 有线 / FFE

| 资产 | 规模 | 用途 |
|---|---:|---|
| 参考内核驱动 `drivers/net/ethernet/zte/` | 21,261 行 | NPPT/IDM/TM/PP/SE/CLA/PM/DSA 的工程骨架 |
| `tm.dis.txt` | 108,726 行 | tm.ko 全量反汇编（CLA/SPA/SBRG/ADM/PM/TM） |
| `plat.dis.txt` | 15,031 行 | plat.ko 全量反汇编（IDM/TM/BMU/NAPI） |
| `fpga_calls.txt` | 4,486 条 | tm.ko 的寄存器访问轨迹 |
| `build/re/ffe/symbols.txt` | 261 个符号 | FFE 流学习/协议层/NPU 插件/flush |
| `build/re/ffe/disasm/` | 13 个关键函数 | `ffe_learn_pkt`、`ffe_calc_key_len`、`ffe_general_insert_flow`、`npu_cap_setup` 等 |
| `ffe-full` 实机 | 6,018 帧零失步 | IDM 环机制已被真机验证（但接的是 mac80211，架构错） |

运行态基线（2026-10-29，只读）进一步证实：
FFE v3.6.6 在跑，WiFi 流 `eth0↔nbif0↔wlan0`、`npu:1`；`npu_cap` 覆盖
IPv4/IPv6/DS-Lite/raw/multicast；IDM1=AP、IDM0=APCLI。

### 1.2 MT7916 私有栈（最大不确定项）

| 指标 | 数值 |
|---|---:|
| 文件大小 | 5,586,568 B |
| 符号总数 | 25,134 |
| 函数数 | **7,760** |
| `.text` 总量 | **2.59 MiB** |
| 对 ZTE 侧的 import | **9 个**（`idm_skb_stack_pop/push/_push`、`wlan_get_multiap_btm_kick`、`__napi_schedule`、`__tasklet_hi_schedule`、`schedule`、两个 `__aeabi_unwind_*`） |
| `cfg80211`/`ieee80211` 引用 | **0**（确认私有栈） |
| 标准内核 API 重定位 | `pci_` 251、`dma_` 98、`skb_` 74、`netif_` 24、`napi_` 10、`tasklet_` 68、`request_firmware` 2 |
| 固件/配置字符串 | `/etc/wireless/mt7916/{WIFI_RAM_CODE_MT7916.bin,7916_WACPU_RAM_CODE_release.bin,mt7916_patch_e1_hdr.bin,MT7916_EEPROM.bin,l1profile.dat.dbdc,mt7916.dbdc.b0/b1.dat}` |
| 关键入口 | `mt_wifi_init`(0x149f34, 0x48c)、`rtmp_ap_init`(0x7d50, 0x1d8)、`mt_wifi_open`(0x26b2c8, 0x180)、`mt_rx_pkt_process`(0xcc77c, 0x36c)、`mt_engine_start_tx`(0x280314, 0x66c)、`mt_load_patch`(0x1d7180, 0x1d8)、`AndesSendCmdMsg`(0x1b9b18, 0xadc)、`MCUSysInit`(0x1b8b00, 0x158)、`hif_init_WFDMA`(0x195b6c, 0x2dc)、`pci_init_txrx_ring_mem`(0x1e4a64, 0xb2c) |

函数族分布（按名字前缀）：

```
mt_ 438 / 109,504 B      rtmp_ 137 / 34,004 B    Rtmp 147 / 31,696 B
pci_ 78 / 26,500 B       ap_ 97 / 46,588 B       bss_ 20 / 5,788 B
Andes 26 / 17,344 B      Mlme 47 / 26,736 B      Wpa 18 / 10,464 B
Wsc 118 / 85,448 B       ba_ 24 / 12,040 B       hif_ 48 / 5,256 B
```

**关键判断**：
- 对 ZTE fabric 的耦合只有 9 个 import，`idm_skb_stack` 可以用自研池替代，
  不需要把 tm.ko 的整套 API 搬过来。这是无线可行性的最大正面信号。
- 但 `mt_wifi_init()` 直接调 `qm_init/tm_init/WfInit/MlmeInit/NICInitializeAsic/
  RTMPEnableRxTx/rtmp_ap_init` 等一大串初始化；`MCUSysInit()` 调
  `chip_fw_init/NICLoadFirmware`；`pci_kick_out_fwdl_msg()` 走 Andes 队列。
  最小 host driver 不是“几百行”，而是一个**受控子集**。
- `Wsc`（WPS）118 个函数/85 KB、`ap_` 97 个/46 KB、`Mlme` 47 个/27 KB：
  如果首版不做 WPS/MESH/漫游，可以砍掉很大一块。

## 2. 组件级可行性打分

图例：🟢 高 / 🟡 中 / 🔴 低

| 组件 | 判断 | 说明 |
|---|---|---|
| 有线 netdev/SBRG/PPE/CLA/PM | 🟢 | 参考驱动 + vendor `tm.dis` + 运行态基线；主要风险是 E2631 偏移/队列差异 |
| TM/BMU/QMG/SCH/RED | 🟢 | `plat.dis`/`tm.dis` 都有初始化序列；BMU 基址需实测 |
| IDM 环 | 🟡 | `ffe-full` 已实证 6,018 帧零失步；但 E2631 的 `+0x04/+0x08/+0x44/+0x48/+0x4C` 语义仍有冲突，需只读钉死 |
| FFE 控制面 | 🟡 | 261 符号、13 函数反汇编；协议层/NPU 插件框架已知，完整状态机还需补 |
| MT7916 固件下载/MCU/WFDMA/TXD | 🟡🔴 | 入口/字符串/大小都已知，但无源码；G3 是 go/no-go |
| MT7916 最小 AP（BSS/SSID/WPA2/关联） | 🟡 | 可砍 WSC/MESH/漫游；但 WPA/MLME/AP 状态机仍需逐函数转写 |
| MT7916 全功能（8 VAP/MESH/APCLI/WPA3/160M） | 🔴 | 体量大、边角多，不适合首版 |
| OpenWrt A（netifd/fw4/LuCI 胶水） | 🟢 | 保留现成用户态；只需让 FFE/WiFi 控制面接进去 |
| 用户态 B（全自研） | 🔴 | DHCP/NAT/DNS/UI 全重写，不建议首版 |
| 授权/provenance | 🟡🔴 | 固件 blob、mcode、寄存器镜像的再分发边界未定 |

## 3. 三档“能做到什么”

### Tier 1：有线原厂栈（建议先做，8-12 周）

交付：
- 自研有线驱动族（NPPT/gmac、SBRG/switch、PPE/CLA/PM、TM/BMU、IDM）。
- 自研 FFE 控制面，替代 `nf_flow_table` 硬件卸载。
- IPv4 → IPv6 → PPPoE → 硬件 NAT/SNAT → 组播 flush，按顺序。
- 1G 线速、CPU 接近空闲、24h 稳定。
- OpenWrt A：netifd/fw4/LuCI 继续用，只做胶水。

风险：E2631 与 ZX279133 的寄存器/队列差异；IDM 语义；32 位 DMA。
判断：**高置信可做**。

### Tier 2：最小无线 AP + 加速（G3 通过后再投入，4-6 个月）

交付：
- 自研 MT7916 host driver：PCIe、固件下载（ROM patch/WM/WA）、MCU、
  WFDMA TX/RX、TXD、一个 2.4G + 一个 5G BSS、WPA2、关联/去关联。
- 自研 `wlanmgr`：替代 hostapd/cspd 的最小配置面。
- IDM/FFE 加速：单播下行/上行，逐流可回退。
- 不做 MESH、APCLI、多 VAP、WPS、WPA3、160M、漫游。

风险：WFDMA/MCU/TXD/BA 的正确性；固件命令时序；空口稳定性。
判断：**有机会，但必须先过 G3（固件启动 + 一帧收发）**。

### Tier 3：原厂全功能无线（不建议首版，6-12+ 个月）

- 8 VAP、MESH、APCLI、WPA3、160M、band steering、802.11k/v/r、WPS、
  多 SSID VLAN、DBDC profile。
- 需要复刻 `Wsc`/`Wpa`/`Mlme`/`ap_` 的大量状态机；首版做这个会把有线/加速拖死。
- 判断：**低置信，除非拿到 MTK/ZTE 源码或明确授权**。

## 4. Go/No-Go 门禁

| 门 | 判据 | 失败怎么办 |
|---|---|---|
| **G1 有线驱动** | 自研驱动能 probe、ping、1G 双向；无 Oops/watchdog | 回退参考/主线有线路径，先保可用 |
| **G2 FFE v4** | IPv4 TCP/UDP 学习+卸载+删除+统计；conntrack 联动；1G | 先保留软件路径，继续修 FFE |
| **G3 无线固件** | 自研 driver 能加载 vendor 固件、MCU 有应答、TX/RX 各一帧 | **停止全栈无线**；只做有线加速，或重新评估 |
| **G4 最小 AP** | 手机关联、WPA2、DHCP、ping；1h 不死 | 停在 G3，不投入多 VAP/MESH |
| **G5 无线加速** | IDM 下行/上行；CPU 下降；逐流可回退；24h 稳定 | 默认关闭无线加速，保留软件路径 |

**G3 是关键**：它证明“自研 host driver + vendor 固件”这条路成立；过不了，
Tier 2/3 都不成立。建议 G3 与 G1/G2 **并行**做静态准备，但只在有线可用的前提下上机。

## 5. 未来两周的具体动作（不碰设备写口）

1. **mt7916 反汇编**：把 5 组入口（固件下载/MCU/WFDMA/TX/RX/AP init）扩到
   调用图前两层；产出 `docs/npu/03-wireless-model.md`。
2. **固件/配置映射**：把 `WIFI_RAM_CODE`、`WACPU`、patch、EEPROM、dbdc/l1profile
   的加载顺序、校验、路径写成表；全部来自字符串/反汇编。
3. **有线 register-diff 第一版**：NPP/IDM/TM/PP/SBRG/CLA/PM/BMU；标出
   E2631 vs ZX279133 的差异和未知项。
4. **FFE 覆盖表**：261 符号 → 已反汇编/未反汇编/必须实现/可延后。
5. **G3 试验设计**：最小 host driver 的 probe/固件下载/MCU/一帧收发步骤、
   失败判据、回退方式；不写代码，只出设计。

两周后给一份“G1/G3 是否开工”的评审；开工前不改设备、不刷机。

## 6. 需要你确认/提供的

- **A + W1 是否确认**（本文按此展开）。
- **G3 上机窗口**：自研 host driver 第一次加载 vendor 固件时需要设备时间；
  失败可能挂死，必须有串口/断电恢复方案。
- **授权边界**：vendor 固件 blob、mcode 的再分发；如果只本地构建、不随仓库分发，
  风险不同。
- **是否接受“首版不做 MESH/APCLI/多 VAP”**；如果必须首版全功能，工期和风险按 Tier 3 重估。

## 7. 证据路径

- 运行态基线：`docs/npu/01-vendor-runtime-baseline-2026-10-29.md`
- mt7916 关键函数反汇编：`_scratch/npu-probe/mt7916-key.dis.txt`（12 个入口，3,808 行）
- mt7916 符号/重定位：`_scratch3/zte-zx279128s-recon-master/docs/tm-objdump/mt7916.{symtab,relocs}.txt`
- mt7916 对 ZTE 的 import：`.../mt7916_tm_imports.txt`（9 个）
- tm/plat 反汇编：`.../tm.dis.txt`、`.../plat.dis.txt`、`fpga_calls.txt`
- FFE：`build/re/ffe/symbols.txt`、`disasm/`、`ffe-reconstructed.h`
- 参考驱动：`_scratch/ref-cnjn/kernel/drivers/net/ethernet/zte/`
