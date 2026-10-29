# MT7916 私有栈模型（第一版，2026-10-29）

> 证据：`_scratch/npu-probe/mt7916-key.dis.txt`（12 个入口，3,808 行）、
> `_scratch3/zte-zx279128s-recon-master/docs/tm-objdump/mt7916.{symtab,relocs}.txt`、
> `mt7916_tm_imports.txt`、`mt7916.ko` 字符串、运行态基线。
> 标注：`[实测-静态]` = 反汇编/符号；`[实测]` = 运行态；`[推断]` = 由调用图推断；
> `[未知]` = 没有足够证据。

## 0. 结论

1. `mt7916.ko` 是 **MTK 私有 AP SDK**：`cfg80211`/`ieee80211` 引用为 0，
   AP 管理、TX/RX、QM、WFDMA 都在驱动内部。
2. 对 ZTE fabric 的耦合只有 **9 个 import**：`idm_skb_stack_pop/push/_push`、
   `wlan_get_multiap_btm_kick`、`__napi_schedule`、`__tasklet_hi_schedule`、
   `schedule`、两个 `__aeabi_unwind_*`。`idm_skb_stack_*` 可以用自研池替代。
3. 最小 host driver 不是“几百行”，但可以砍掉 WPS/MESH/漫游，只保留
   **PCIe → 固件下载 → MCU → WFDMA → TX/RX → 一个 BSS** 的子集。
4. 最大未知是 **WFDMA/MCU/TXD 的寄存器级序列**，不是 AP 业务逻辑；
   G3（固件启动 + 一帧收发）必须先做。

## 1. 入口规模（`mt7916.symtab.txt`）

| 函数 | 地址 | 大小 | 直接调用数/唯一目标 | 作用 |
|---|---:|---:|---:|---|
| `mt_wifi_open` | 0x26b2c8 | 0x180 | 15 / 12 | 打开 netdev，读 profile，进 `mt_wifi_init` |
| `mt_wifi_init` | 0x149f34 | 0x48c | 57 / 45 | 驱动总初始化：HwCtrl/QM/TM/MLME/ASIC/AP |
| `MCUSysInit` | 0x1b8b00 | 0x158 | 11 / 6 | MCU 固件初始化 |
| `mt_load_patch` | 0x1d7180 | 0x1d8 | 7 / 5 | ROM patch 加载 |
| `pci_kick_out_fwdl_msg` | 0x1e1720 | 0x2a0 | 14 / 10 | 通过 Andes 队列投递固件下载消息 |
| `hif_init_WFDMA` | 0x195b6c | 0x2dc | 28 / 3 | WFDMA 初始化（22 次 `mac_io_write32`） |
| `pci_init_txrx_ring_mem` | 0x1e4a64 | 0xb2c | 29 / 16 | TX/RX ring 分配与初始化 |
| `mt_rx_pkt_process` | 0xcc77c | 0x36c | 29 / 18 | RX 包处理，直接调 `idm_skb_stack_pop` |
| `mt_engine_start_tx` | 0x280314 | 0x66c | 17 / 8 | QM 入队/发送 |
| `rtmp_ap_init` | 0x7d50 | 0x1d8 | 13 / 10 | AP 初始化 |
| `ap_set_wireless_bss_configs` | 0xcefd8 | 0xac | 7 / 1 | BSS 配置 → `set_ap_wireless` |
| `RTMP_AP_IoctlHandle` | 0x37abc | 0x8c4 | 55 / 43 | 私有 ioctl 分发 |
| `AndesSendCmdMsg` | 0x1b9b18 | 0xadc | 79 / 29 | Andes MCU 命令发送 |

`mt7916.ko` 总计 **7,760 个函数 / 2.59 MiB `.text`**；`Wsc` 118 个函数/85 KB、
`ap_` 97/46 KB、`Mlme` 47/27 KB。首版不做 WPS/MESH/漫游可以砍掉很大一块。

## 2. 初始化链（`[实测-静态]` 调用图）

```
module_init
  └─ mt_wifi_open
       ├─ load_dev_l1profile / RTMPPreReadProfile
       ├─ mt_wifi_init
       │    ├─ DriverOwn / wifi_sys_reset / HwCtrlInit / rtmp_cfg_init
       │    ├─ qm_init / tm_init / WfInit / MlmeInit
       │    ├─ NICInitializeAsic
       │    ├─ RTMPEnableRxTx
       │    ├─ rtmp_ap_init
       │    │    ├─ APOneShotSettingInitialize
       │    │    ├─ APInitForMain
       │    │    ├─ RtmpOSNetDevAddrSet
       │    │    └─ muru_cfg_dlul_limits / SetMura*
       │    └─ InitTxSCommonCallBack
       ├─ mt_service_init / mt_service_open
       ├─ RTMPDrvOpen
       └─ RT28xx_MBSS_Init
```

`MCUSysInit` 的直接调用：

```
hc_get_chip_ops
chip_fw_init
NICLoadFirmware
hw_io_read32 / hw_io_write32
printk
```

`mt_load_patch` 的直接调用：

```
hc_get_chip_cap / hc_get_chip_ops
show_trinfo_proc
os_free_mem
printk
```

`pci_kick_out_fwdl_msg` 的直接调用：

```
hc_get_hif_ctrl
RTMP_QueryPacketInfo → RtmpDrvPciMapSingle
AndesQueueTailCmdMsg
hif_io_write32
OS_SPIN_LOCK_IRQSAVE / OS_SPIN_UNLOCK_IRQRESTORE
```

`[推断]` 固件下载的调用顺序是：
`MCUSysInit → chip_fw_init → NICLoadFirmware → mt_load_patch /
pci_kick_out_fwdl_msg → Andes 队列 → WFDMA → MCU 事件`。
具体命令 ID/阶段需要继续反汇编 `chip_fw_init`、`NICLoadFirmware`、
`pci_kick_out_fwdl_msg` 的调用者。

### 2.1 固件下载链（G3 反汇编补强，2026-10-29）

新增反汇编：`_scratch/npu-probe/mt7916-g3.dis.txt`、`mt7916-g3b.dis.txt`，
调用图 `mt7916-g3-calls.txt`。

```
WfMcuInit (0xad7b8)
  ├─ WfMcuSysInit (0x146abc)
  │    └─ chip_fw_init (0x142d3c) → hc_get_chip_ops / hc_get_hif_type
  ├─ NICLoadRomPatch (0x1de8cc) ──b──> mt_load_patch (0x1d7180)
  ├─ NICLoadFirmware (0x1de8d0) ──b──> mt_load_fw (0x1d7358)
  ├─ WfMcuSysExit
  ├─ HWCtrlOpsReg / MtCmdCr4Set / asic_wa_update
  └─ hw_io_read32 / hw_io_write32
```

- `NICLoadRomPatch`/`NICLoadFirmware`/`NICRestartFirmware` 都是 4 字节
  thunk：`b mt_load_patch`、`b mt_load_fw`、`b mt_restart_fw`。
- `mt_load_patch`：`hc_get_chip_cap/ops`、`show_trinfo_proc`、`os_free_mem`。
- `mt_load_fw`：`hc_get_chip_cap/ops`、`show_trinfo_proc`、`os_free_mem`；
  真正的下载通过 chip ops 函数指针（间接 `blx`，不是直接 `bl`）。
- `parse_patch_v1`：`os_alloc_mem` + `ctrl_fw_state_v2` 等；`load_patch_v1`：
  `MtCmdPatchSemGet` → `MtCmdAddressLenReq` → `MtCmdPatchFinishReq`，
  每条命令都经 `AndesAllocCmdMsg/InitCmdMsg/AppendCmdMsg` + `chip_cmd_tx`。
- `net_ad_enq_pkt`：`skb_clone`、`send_mlme_pkt`；`net_ad_post_tx`：
  `net_ad_thread_proceed_tx`、`net_ad_enq_pkt`、`complete`。
- `set_ap_wireless` 只做 `strcmp`；`APOneShotSettingInitialize` 做定时器/
  表初始化；`APInitForMain` 做 ASIC/MLME/WCID/中断等。

**G3 仍缺**：`chip_fw_init` 的 chip ops 函数指针目标（间接调用）、
`mt_load_fw` 的下载分段/地址、`AndesSendCmdMsg` 的命令 ID 与 ack 匹配。
这三项是 G3.2 之前必须补的静态项。

## 3. 固件与配置（`[实测-静态]` 字符串）

`mt7916.ko` 里出现的路径：

```
/etc/wireless/mt7916/WIFI_RAM_CODE_MT7916.bin      WM 固件
/etc/wireless/mt7916/7916_WACPU_RAM_CODE_release.bin  WA 固件
/etc/wireless/mt7916/mt7916_patch_e1_hdr.bin       ROM patch
/etc/wireless/mt7916/MT7916_EEPROM.bin             EEPROM/校准
/etc/wireless/mt7916/l1profile.dat.dbdc            DBDC profile
/etc/wireless/mt7916/mt7916.dbdc.b0.dat / b1.dat   每 band profile
/lib/firmware/MT7916_EEPROM.bin                    备用 EEPROM 路径
```

相关字符串：

```
prepare_fwdl_img failed, Status[=0x%08x]
NICLoadRomPatch failed, Status[=0x%08x]
MCU Init Done!
EEPROM Init Done!
Load l1profile succeed/failed
WFDMA info
TXD null!!
DBDC MODE=%d, ConcurrentBand=%d
iwpriv ra0 set srcfgsrquerytxd=1
```

运行态确认这些文件在 `/etc/wireless/mt7916/` 下，MD5 已记录
（`build/board/etc-wireless.out.txt`）。

## 4. PCIe / WFDMA / TX/RX

### 4.1 PCIe

- 设备：`14c3:7906` @ `0000:00:00.0`，`14c3:790a` @ `0000:01:00.0`；
  class `0x028000`；IRQ 37/38（运行态）。
- `%pOF`/`/proc/iomem` 显示 PCIe 窗口；`mt7916` 的 PCIe 函数族有 78 个
  （`pci_*`，26.5 KB），包括 `pci_init_txrx_ring_mem`、`pci_kick_out_fwdl_msg`、
  `pci_rx_dma_done_handle`、`pci_tx_dma_done_handle`。

### 4.2 WFDMA

`hif_init_WFDMA` 只有 3 个唯一调用（`hc_get_asic_cap`、`mac_io_read32`、
`mac_io_write32`），但有 **22 次 `mac_io_write32`**：典型“读-改-写一串固定
寄存器”的初始化。`[未知]` 具体偏移/值需要逐条转写。

### 4.3 RX

`mt_rx_pkt_process` 直接调用：

```
idm_skb_stack_pop      ← 从 IDM 共享池取空闲 skb
__alloc_skbuff
skb_put
memcpy
asic_get_packet_type
asic_trans_rxd_into_rxblk
rx_packet_process
asic_rxv_handler
asic_rx_event_handler
chip_txs_handler
hif_free_rx_buf
RTMPFreeNdisPacket
```

`[推断]` RX 帧从 IDM/PP 可见内存取回后，先解析 RXD，再按类型交给
`rx_packet_process`（数据）或 `asic_rxv_handler`（管理/事件）。

### 4.4 TX

`mt_engine_start_tx` 直接调用：

```
net_ad_set_device_info
net_ad_cfg_queue
net_ad_get_wmm_idx
net_ad_set_wmm_param_by_qid
net_ad_enq_pkt
net_ad_post_tx
```

`[推断]` 这是 MTK 自己的 QM 入队/发送路径；`net_ad_post_tx` 最终触发 WFDMA/TXD。
`TXD` 的构造函数不在本次 12 个入口里，需要继续反汇编
`net_ad_enq_pkt`/`net_ad_post_tx` 及其调用者。

## 5. AP 与控制面

- `rtmp_ap_init`：`APOneShotSettingInitialize`、`APInitForMain`、
  `RtmpOSNetDevAddrSet`、`muru_cfg_dlul_limits`、`SetMura*`。
- `ap_set_wireless_bss_configs` 只调 `set_ap_wireless`；BSS 参数由上层 profile 填。
- `RTMP_AP_IoctlHandle` 处理 `iwpriv` 私有 ioctl，直接调用包括：
  `RTMPAPQueryInformation`、`RTMPIoctlGetMacTable`、`RTMPIoctlGetSiteSurvey`、
  `RTMPIoctlRXStatistics`、`RTMPIoctlStatistics`、`RTMPIoctlPhyState`、
  `RTMPIoctlGetBaTable`、`RTMPIoctlSetWSCOOB`、`MBSS_Init/Remove`、
  `ApCliIfUp`、`WscInit` 等。
- 运行态 `iwpriv` 列出 16 个私有 ioctl；`cspd`/`idm_service`/`multiapd` 通过它们
  管理 AP/MESH/APCLI。首版 `wlanmgr` 只需要其中一部分（BSS/SSID/安全/关联）。

## 6. 与 ZTE fabric 的耦合

`mt7916_tm_imports.txt` 只有 9 个 import：

```
__aeabi_unwind_cpp_pr0 / pr1
__napi_schedule
__tasklet_hi_schedule
_idm_skb_stack_push
idm_skb_stack_pop
idm_skb_stack_push
schedule
wlan_get_multiap_btm_kick
```

`[推断]` 这意味着：
- 自研 host driver 不需要复刻 tm.ko 的 API；只需要一个
  `idm_skb_stack` 等价物（自研 page/skb 池）和 NAPI/tasklet。
- `wlan_get_multiap_btm_kick` 只与 MESH/BTM 有关；首版不做 MESH 可以留空。
- 其余全部是标准内核 API；`pci_`/`dma_`/`skb_`/`netif_`/`tasklet_` 的重定位
  数量见 `02-feasibility`。

## 7. G3 最小 driver 设计（固件启动 + 一帧）

### G3.1 probe / 只读识别

- 绑定 `14c3:7906`，映射 BAR0/BAR1，读 chip ID/version（只读）。
- 打印 PCIe 配置空间/资源；不写寄存器。
- 成功判据：读出的 chip ID 与 vendor 驱动/EEPROM 一致；系统不挂。

### G3.2 固件下载

- 加载 `mt7916_patch_e1_hdr.bin`、`WIFI_RAM_CODE_MT7916.bin`、
  `7916_WACPU_RAM_CODE_release.bin`、`MT7916_EEPROM.bin`。
- 按 `MCUSysInit → chip_fw_init → NICLoadFirmware → pci_kick_out_fwdl_msg`
  的顺序；先只做 ROM patch + WM，WA 后置。
- 成功判据：MCU 事件/ack 收到，`MCU Init Done` 状态可读；不要求 AP。
- 失败判据：MCU 无响应、WFDMA 不前进、PCIe 错误；立即停止，不重试刷写。

### G3.3 一条 RX/TX ring

- 只初始化一条 RX ring + 一条 TX ring；用最小 WFDMA 配置。
- RX ring 的缓冲先用自己的 page/skb 池（不接 IDM），确认 DMA 能写回。
- 成功判据：RX 描述符完成计数增长；能收到至少一个 beacon/管理帧。

### G3.4 一帧 TX + 一帧 RX

- 发送一个最小管理帧（自带 TXD），确认 TX done。
- 同时确认 RX 至少一帧；不要求关联、不要求加密。
- 成功判据：TX done 计数 +1，RX 完成计数 +1，无 PCIe 错误/挂死。
- 失败判据：TXD 被固件拒绝、WFDMA 停、MCU 超时；停在 G3，不进入 G4。

### G3 的未知项（必须先补）

1. `hif_init_WFDMA` 的 22 个寄存器偏移/值；
2. `pci_init_txrx_ring_mem` 的 ring 基址/深度/中断配置；
3. `chip_fw_init`/`NICLoadFirmware` 的固件头解析与下载分段；
4. `AndesSendCmdMsg` 的命令 ID/ack 匹配；
5. TXD 构造与 `net_ad_post_tx` 的队列映射。

## 8. 下一步

- 反汇编 `chip_fw_init`、`NICLoadFirmware`、`net_ad_enq_pkt`、
  `net_ad_post_tx`、`set_ap_wireless`、`APOneShotSettingInitialize` 的调用图前两层。
- 从 `mt7916.symtab.txt`/`relocs` 提取 `WFDMA`/`TXD`/`MCU` 相关函数，补
  `docs/npu/03-wireless-model.md` 的“未知项”。
- 先写 G3 的只读 probe/固件下载骨架，不接 AP、不接 IDM；G3 通过后再进入 G4。

## 9. 证据路径

- 反汇编：`_scratch/npu-probe/mt7916-key.dis.txt`
- 调用图：`_scratch/npu-probe/mt7916-calls.txt`
- 符号/重定位：`_scratch3/.../mt7916.{symtab,relocs}.txt`
- ZTE import：`_scratch3/.../mt7916_tm_imports.txt`
- 运行态：`docs/npu/01-vendor-runtime-baseline-2026-10-29.md`
- 可行性：`docs/npu/02-feasibility-2026-10-29.md`
