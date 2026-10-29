# 原厂运行态只读基线（E2631，2026-10-29）

> 设备：192.168.5.3，vendor Linux 4.1.25 #2 SMP（2025-09-25 21:30:57 CST），
> 首次探测时 uptime 22250 s。全程只读：只执行 `cat/ls/ps/grep/sed/iwconfig`，
> 未写任何 proc/sysfs、未加载/卸载模块、未重启、未运行原厂二进制。
>
> 证据文件（工作区外，不入库）：
> - `_scratch/npu-probe/vendor-ro-1.out.txt`（416 行，模块/FFE/netdev/PCIe）
> - `_scratch/npu-probe/tools.out.txt`（293 行，FFE cmd 帮助/流表/iwconfig/ps）
> - `_scratch/npu-probe/vendor-ro-2.out.txt`（418 行，iomem/IDM/FDB/模块参数）
> - `_scratch/npu-probe/vendor-ro-3.out.txt`（542 行，VLAN/wb_config/kallsyms）
> - 计划与只读脚本：`_scratch/npu-probe/*.plan`、`telnet_ro.mjs`
>
> 口令只从临时环境文件注入，未写入任何输出文件；转录已扫描确认不含口令。

## 1. 模块栈与依赖（`/proc/modules`）

| 模块 | 大小 | 引用计数 | 被谁引用 | 作用 |
|---|---:|---:|---|---|
| `xdpi.ko` | 882,299 | 0 | — | DPI 业务识别 |
| `mt7916.ko` | 4,506,386 | 4 | — | MTK 私有 AP SDK（CSP_WLAN） |
| `idmfdb.ko` | 64,461 | 0 | — | IDM 慢路径 FDB/转发 |
| `switch.ko` | 172,680 | 1 | idmfdb | SBRG 交换 + L3 会话入口 |
| `tm.ko` | 1,179,936 | 2 | mt7916, switch | 引擎寄存器 SDK |
| `plat_zxylzb_9128S.ko` | 167,427 | 4 | mt7916, idmfdb, switch, tm | IDM/TM/BMU/NAPI/OS 胶水 |
| `zx_ponreg.ko` | 2,256 | 2 | switch, tm | PON 寄存器窗口 |
| `patch.ko` | 2,057 | 0 | — | 运行时补丁 |
| `shellproc.ko` | 47,932 | 1 | patch | 内核调试器（不碰） |

依赖方向与 `docs/ffe-full-完整逆向与移植报告-2026-10-29.md:32-46` 一致，运行时再次证实。
`/kmodule/` 里还有 `bspdriver.ko`、`wlan_debug_module.ko`、`xdpi.ko` 等未加载模块。

## 2. FFE 运行态（`/proc/zte/sys/ffe/ffeinfo`）

```
version:3.6.6   enable:1
max flow:28136  curr:6  high_wm:26377  overflow_cnt:0
timeout:45 sec  layer_enable:3  no_delete:0  icmp_acc:0  acc_thresh:0
proto flow cnt:
  ipv4_udp:3 npu:3   ipv4_tcp:3 npu:3
  ipv6_udp:0 npu:0   ipv6_tcp:0 npu:0
  ipv4_mc:0  ipv4_raw:0  ipv6_mc:0  ipv6_raw:0
statics:
  rx pkts:934694  rx bytes:884424717
  tx pkts:934694  tx bytes:884460791
  slowpathcnt:108163  npuflowcnt:6  createfail:1  deletefail:0
  backlog qlen:0  dropped:0
RPS: cpu_cnt[2] cpu_map[3] tx_queue_max[1024]
  CPU0 flows:1 processed:0    CPU1 flows:5 processed:1253 rcv_rps:985
```

`/proc/zte/sys/ffe/cmd` 的只读帮助（`cat` 即可，未写入）确认八协议
`0/1/2`（全关 / 仅 FFE / FFE+NPU）以及 `npu`、`npudd`、`npuretry`、`pps`、
`layer_enable`、`acc_thresh`、`icmp_acc`、`no_delete`、`timer`、`flowcnt`、
`ffe`、`del`、`show`、`rps_tx_queue_max`、`rps_cpu_map`。

### 2.1 WiFi 流确实在 FFE/NPU 里

`/proc/zte/sys/ffe/ipv4_tcp`（slot_size 4096，2 条流）：

```
flow[0] npu:1 1  attr:0003  layer:2
  src_key:/pon/[82:b7:14:3a:0a:20]:[f4:fc:49:60:6f:9c]/[112.53.48.125]:[192.168.5.10]:T 4:P 6/[443:41510]
  dst_key:/wlan0/[82:b7:14:3a:0a:20]:[f4:fc:49:60:6f:9c]/[112.53.48.125]:[192.168.5.10]:T 4:P 6/[443:41510]
  cmd:[tcp_skip]->[ipv4_skip]->[eth_skip]
  passed dev:[eth0]->[nbif0]->[wlan0]  mtu[1500] ll_rs[16] npu activepps[0] retry_left[0]

flow[1] npu:1 1  attr:0003  layer:2
  src_key:/wlan0/.../[192.168.5.10]:[112.53.48.125]:T 0:P 6/[41510:443]
  dst_key:/eth0/.../[192.168.5.10]:[112.53.48.125]:T 0:P 6/[41510:443]
  passed dev:[wlan0]->[nbif0]->[eth0]
```

`ipv4_udp` 有 3 条流，全部是 `src_key:/pon/` → `dst_key:/wlan0/`，`passed dev:[eth0]->[nbif0]->[wlan0]`，`npu:1 1`。
**结论**：原厂 WiFi 上下行都进同一张 FFE/NPU 流表，链路是
`eth0 ↔ nbif0 ↔ wlan0`，不是 `mac80211`/`netif_rx` 路径。

### 2.2 NPU 能力矩阵（`/proc/zte/sys/ffe/npu/npu_cap`）

只读 `cat` 得到 5 组 flow feature：

| index | rxdev | txdev | 能力摘要 |
|---:|---|---|---|
| 0 | `pon0 pon gpon0 epon0 ptm0` | `eth*[0] wlan*[0]` | layer2: v4_tcp/v4_udp/v6_tcp/v6_udp，pps 0、frag 0；layer3: v4_tcp pps20、v4_udp 0、dslite_tcp/udp pps256、v6_tcp pps20、v6_udp 0，frag 1 |
| 1 | `eth* wlan*` | `eth*[0] pon[0] ptm0[0]` | 同上；dslite pps128 |
| 2 | `eth*` | `eth0[0] ptm0[0]` | layer2/3 `ipv4_raw` |
| 3 | `eth0 pon ptm0` | `eth*[0]` | layer2/3 `ipv4_raw` |
| 4 | `pon0 pon gpon0 epon0 ptm0` | `eth*[0]` | layer2/3 `ipv4_mc`、`ipv6_mc` |

每组还带 `layer_cap[1..10]` 位图（如 layer3 v4_tcp 的
`1:0000001f 2:00001c1f 3:0000001e 5:00001c1c 9:0000001c`）。
**结论**：NPU 原生支持 IPv6 TCP/UDP、DS-Lite、raw、multicast；当前
`ipv6_*` 计数为 0 只是没有 v6 流量，不是硬件能力缺失。main 上“IPv6 只能走 CPU”
是软件选择，不是硬件限制。

`/proc/zte/sys/ffe/scripts/*` 是一组 shell 包装器，内容形如
`echo npu $1 > /proc/zte/sys/ffe/cmd`；本轮只读，未执行。
`hash/*`、`sharedata/ffe_cmd`、`sharedata/ffe_path`、`npu/npufilter` 读为空；
这些 dump 需要写 `cmd` 触发，未做。

## 3. IDM / IDMFDB 运行态

### 3.1 `/proc/idm/stat`（三组通道 0/1/2）

```
idm_push_cnt[0]:419364  [1]:0  [2]:866826
idm_pop_cnt[0]:418733   [1]:0  [2]:866705
idm_push_failed[2]:566  idm_push_failed_res[5]:566
idm_skb_stack_empty[0]:2167  [2]:3224
idm_pop_size_err[2]:2
idm_poll_cnt:2231830  idm_tx_done_total:280945
idm_txtq_err:0  idm_tx_full:0  idm_sw_tx_full:0
idm_rx_arp_count:0
```

含义：通道 0 与通道 2 在用，通道 1 空闲；通道 2 有 566 次 push 失败、3224 次
栈空、2 次 pop 尺寸错误。**IDM skb 栈不是无限可靠，重写时必须做池/水位/重填**。

### 3.2 `idm_map`（`/proc/idm_fdb/idm_map`）

```
1:  idm:0 ssid:1 dev:apcli0
2:  idm:0 ssid:2 dev:apcli5g0
8:  idm:1 ssid:0 dev:wlan0
9:  idm:1 ssid:1 dev:wlan1
10: idm:1 ssid:2 dev:wlan2
11: idm:1 ssid:3 dev:wlan3
12: idm:1 ssid:4 dev:wlan5g0
13: idm:1 ssid:5 dev:wlan5g1
14: idm:1 ssid:6 dev:wlan5g2
15: idm:1 ssid:7 dev:wlan5g3
```

AP VAP 全部走 `idm1`，APCLI 走 `idm0`；与内核 `wlan_to_idm_map` 的
`AP=(1,off)`、`APCLI=(0,off)` 一致。

### 3.3 每 VAP 计数（`/proc/idm_fdb/<dev>/stat`）

| VAP | en/idm/ssid | Tx 单播 | Rx | 多播/广播 |
|---|---|---|---|---|
| wlan0 | 1 / 1 / 0 | 37,625 pkt / 47,645,766 B | 24,433 pkt / 3,219,532 B | 0 |
| wlan5g0 | 1 / 1 / 4 | 381,227 pkt / 430,540,730 B | 256,512 pkt / 113,888,990 B | 0 |

`forbidIdm=0`；`vlan/*`（vid/pri/up_add_tag/down_strip 等）全 0；
`flood_status` 全 0；`multicast` 空；`debug=0`。
`/proc/idm_fdb` 下还有 wlan1/2/3、wlan5g1/2/3、apcli0/apcli5g0 的目录。
`/proc/idm_fdb/<dev>/vlan/*` 的 `vid/pri/mvid/mpri/up_add_tag/down_*_strip`
全 0，`vlan/stat` 的四类计数也全 0。

`/proc/zte/sys/ffe/npu/ipv4_udp_npu` 与 `ipv4_udp` 内容一致：3 条流全部
`npu:1 1`，`passed dev:[eth0]->[nbif0]->[wlan0]`。

## 4. WiFi 私有栈运行态

### 4.1 `iwconfig`（无线扩展，不是 cfg80211）

- `wlan0`：IEEE 802.11ax，Mode:Master，ESSID `Shiyan-Y`，Channel 8，Bit Rate 573 Mb/s。
- `wlan5g0`：IEEE 802.11ax，Mode:Master，ESSID `Shiyan-Y`，Channel 44，Bit Rate 2.401 Gb/s。
- `wlan3` / `wlan5g3`：Mode:Master，ESSID `MESH-606F9C`，Channel 8 / 44，Bit Rate 573 Mb/s / 2.401 Gb/s。
- `wlan1`、`wlan2`、`wlan5g1`、`wlan5g2`：空 ESSID 的 Master VAP。
- `apcli0` / `apcli5g0`：Mode:Managed，Not-Associated，Channel 8 / 44。
- `Tx-Power` 显示 0 dBm，不能据此推断实际发射功率（私有驱动未填该字段）。
- `/proc/net/wireless` 同时列出 8 个 wlan + 2 个 apcli，status 0000、quality 10。

### 4.2 进程与线程

`ps` 里没有 `hostapd`/`wpa_supplicant`。与无线相关的：

```
cspd             (PID 597, 174 MB VSZ)
idm_service      (PID 993)
multiapd         (PID 1286)
mqtt/httpd/telnetd/upnpd/cmd/laninfo/plugin daemons
RtmpCmdQTask / RtmpWscTask / HwCtrlTask / ser_task / RtmpMlmeTask
ffeflush         (内核线程)
```

`iwconfig`/`iwpriv` 存在；`hostapd`/`wpa_supplicant` 不存在。
`mt7916` 只有 `DebugCategory`/`DebugLevel` 两个只写参数（root 读也被拒绝）；
`plat`/`tm`/`switch` 没有 parameters 目录；`idmfdb` 只有两个 LED 名参数。

`iwpriv` 的私有 ioctl 列表（所有 wlan/apcli/eth 都列出同一组）：

```
set (8BE2)            show (8BF1)           phystate (8C03)
get_site_survey (8BED) set_wsc_oob (8BF9)   get_mac_table (8BEF)
e2p (8BE7)             bbp (8BE3)           mac (8BE5)
rf (8BF3)              get_wsc_profile (8BF2) get_wps_pin (8BFD)
get_ba_table (8BF6)    stat (8BE9)          rd (8BF7)
rx (8C01)
```

`set` 是写口，本轮没有执行；其余 get 也未逐个调用。该表是原厂
`cspd`/`iwpriv` 的控制面，重写时要么实现等价 ioctl，要么定义自己的
netlink/proc 接口。

模块 holders 与 `/proc/modules` 一致：

```
plat holders: idmfdb, mt7916, switch, tm
tm holders:   mt7916, switch
switch holders: idmfdb
mt7916 holders: (空)
```

`grep 192.168.5.10 /proc/net/nf_conntrack` 为空；不据此判断 conntrack 不工作
（可能文件为空或格式不同），留待后续。

### 4.3 wb_config（`/proc/net/wb_config`）

```
WB_RX_HANDLER  MCAST prio16 c03f365c
NBIF_TX_HANDLER QOS prio16 c040fb7c
NBIF_TX_HANDLER FFE prio16 c04122bc
WAN_BRIDGE[1]: BASE_DEV[eth0]
  IF[1]: REAL_DEV[nbif0] ROUTE_MODE[0] VLAN_ATTRIBUTE[8] VLAN[0000] 802_1P[0] ServiceType[0]
```

这就是 `nbif0` 挂在 `eth0` 上的运行时证据；`FFE` 和 `QOS` 是 nbif TX 链上的两个
handler。

### 4.4 kallsyms（本次 build 的精确地址）

`/proc/kallsyms` 有完整符号。与重写直接相关的：

```
wlan_tx_handle        c02afcb0
wlan_rx_handle        c02b0140
wlan_tx_ffe_learn     c02affd0
wlan_register_dev     c02aff88
wlan_rx_debug         c02b01d0
nbif_xmit             c03f2ad4
wb_recv_skb           c03f292c
wb_config_proc_read   c03f1ed4
nbif_create           c03f34ec
nbif_delete           c03f3600
ffe_learn_pkt         c0410958
ffe_receive_pkt_slow_path c04105a4
ffe_nbif_tx_handler   c04122bc
ffe_register_proto    c0411be8
ffe_register_packet_layer c0411af4
ffe_register_ingress_notifier c0410544
ffe_register_egress_notifier  c041055c
ffe_calc_key_len      c04119f4
ffe_general_insert_flow c0410d74
ffe_flush_flow_by_conntrack（在后续 grep 范围内）
```

模块侧：

```
tm_acl_get_fastHashRule   bf095c68 [tm]
tm_acl_fast_add_v4v6      bf09a054 [tm]
tm_acl_setMtchInfo.constprop.53 bf0a4404 [tm]
cla_* / spa_* / sbrg_* / adm_* / pm_*  [tm]
rtmp_ap_init              bf200d50 [mt7916]
mt_wifi_init              bf342f34 [mt7916]
mt_wifi_open              bf4642c8 [mt7916]
mt_rx_pkt_process         bf2c577c [mt7916]
mt_load_patch             bf3d0180 [mt7916]
mt_engine_start_tx        bf479314 [mt7916]
mt_engine_stop_tx         bf479980 [mt7916]
RtmpIoctl_rt_ioctl_*      [mt7916]（siwscan/giwscan/siwencodeext/…）
```

地址只对本次 vendor build 有效；引用时必须同时记录模块 SHA256/版本。

## 5. PCIe / 中断

- `0000:00:00.0`：`14c3:7906`，class `0x028000`，IRQ 37，CPU0 1,352,502 次。
- `0000:01:00.0`：`14c3:790a`，class `0x028000`，IRQ 38，CPU0 615,994 次。
- `idm` IRQ 28：CPU1 215,031 次；`pon_tm` IRQ 29：CPU0 89,764 次；
  `pon_npp` 27、`pon_pp` 30 均为 0。
- `/proc/iomem` 只列 PCIe 窗口和 System RAM：
  `40000000-4fffffff : System RAM`；**没有 NPP/IDM/TM/PP 的 reserved 区域**。
  结论：vendor 驱动用 ioremap 但不 `request_mem_region`，或根本不走 iomem 树；
  重写时不能假设 DT reserved-memory，需按 vendor 寄存器/驱动自行分配。

## 6. 内存 / CPU / MTD

- `MemTotal 225324 kB`，`MemFree 101212 kB`，`VmallocUsed 50432 kB`。
- CPU：2× ARMv7 Cortex-A9（part `0xc09`），BogoMIPS ~1990，
  `Hardware: ZTE ZX279128S (Device Tree)`。
- `/proc/mtd`：Whole flash 128 MiB；Bootloader/tag/wifi 各 1 MiB；
  usercfg/defcfg 各 2 MiB；kernel1 3.25 MiB；kernel2 4 MiB；
  rootfs 18.125 MiB；其余插件/状态分区。

## 7. 这些事实对 npu 重写的含义

1. **WiFi 加速路径已被运行态证实**：`FFE/NPU + IDM + nbif + wlan private stack`，
   不是 mac80211；`ffe-full` 的寄存器逆向方向是对的，错在把帧交给 mac80211。
   `npu_cap` 进一步证明 NPU 支持 IPv6/DS-Lite/raw/multicast——**IPv6 不是硬件限制**，
   main 的 v6 CPU-only 是软件选择。
2. **IDM1 = AP，IDM0 = APCLI**；`idm_map` 的 8-15/1-2 是运行时映射，
   重写时可以直接用作 golden reference。
3. **`nbif0` 绑定 `eth0`**；FFE/QOS 是 nbif TX handler；这给
   `wb_config`/`nbif_xmit`/`wb_recv_skb` 的重写提供了精确锚点。
4. **没有 reserved-memory**：BMU/SE/IDM 的地址与大小必须从原厂驱动寄存器序列
   反推，不能用 `/proc/iomem` 或 DT 预留节点当证据。
5. **IDM 栈有真实压力**：`push_failed[2]=566`、`stack_empty[2]=3224`；
   重写必须实现水位、重填、丢包计数，不能假设“永远有空闲 skb”。
6. **多 VAP/MESH/APCLI 是运行态**：wlan0/5g0 AP、wlan3/5g3 MESH、
   4 个空 VAP、2 个 APCLI；最小重写如果只做单 AP，必须显式禁用其余 VAP。
7. **控制面是 `cspd + idm_service + multiapd + private ioctl`**，没有 hostapd；
   `iwpriv` 的 16 个私有 ioctl 就是原厂控制面，我们的 `wlanmgr/ffectl`
   要覆盖这部分，而不是“接 hostapd”。
8. **`tm/switch/plat` 没有 module parameter**：原厂行为硬编码/IOCTL；
   重写不能指望通过参数对齐，必须反汇编 + 表编程。
9. **`dmesg/head/tail` 不存在**：后续只读取证用 `cat/sed/grep`；
   不要写依赖这些 applet 的脚本。

## 8. 尚未取得的（不要当结论）

- FFE `hash/sharedata/npu/npufilter` 读为空；`scripts/*` 是写 `cmd` 的包装器，
  本轮只读约束下不执行。`npu_cap` 已取得完整能力矩阵。
- BMU/SE/IDM 物理基址/大小：`/proc/iomem` 不显示；需反汇编 `plat`/`tm` 并只读
  读寄存器确认。
- `mt7916` 固件下载/MCU 命令/WFDMA/TXD 细节：只拿到符号，尚未反汇编到函数级。
- `createfail:1`、`push_failed[2]=566` 的具体原因：需要后续受控负载实验，
  本轮不做。
- `iwpriv` 私有 ioctl 全集：未跑；下一轮只读列举。
- 原厂 rootfs/cspd 二进制：本地没有；若要复刻控制面，P0 需只读拉取或自写。

## 9. 下一轮只读取证清单

1. 逐个执行只读的 `iwpriv wlan0 get_mac_table/stat/phystate/get_site_survey`
   （不执行 `set`），确认返回格式。
2. `/proc/zte/sys/ffe/npu/npu_cap` 已取；补 `npu`/`hash`/`sharedata` 的可写
   dump 触发条件（只记录，不在本轮执行）。
3. `/proc/net/wb_config` 已取；补 `/proc/net/speedtest` 的只读状态（若安全）。
4. `/proc/net/nf_conntrack` 中与 `192.168.5.10`/`wlan0` 相关的条目（只读过滤）。
5. `/proc/net/bridge/*`、`brctl show`（若存在）的桥/FDB 状态。
6. `cat /sys/module/mt7916/holders/*`、`/sys/module/tm/holders/*`（依赖关系）。
7. 原厂 `/etc/wireless/mt7916/*.dat` 的逐字段解析（已有本地副本）。

## 10. 安全与证据管理

- 口令只经临时环境文件注入，未进入 argv、输出、计划文件或仓库；
  临时文件已删除。
- 本轮所有命令均为只读；未触碰 `cmd`、`sysfs` 写口、`/proc/tm/shell`、
  `fw_flashing`、`e2631-fwslot`、模块加载/卸载、重启。
- 转录文件留在 `_scratch/npu-probe/`，不入库；正式文档只保留脱敏后的计数与符号。
- 设备当前仍在原厂系统运行；本轮没有改变其状态。
