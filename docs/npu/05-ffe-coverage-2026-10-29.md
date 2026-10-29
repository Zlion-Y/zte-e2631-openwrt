# FFE 覆盖表（W1 第一版，2026-10-29）

> 证据：`build/re/ffe/symbols.txt`（261 个精选符号）、`ksym-all.txt`（21,946 个）、
> `build/re/ffe/disasm/`（14 个反汇编文件）、`ffe-reconstructed.h`、
> 运行态 `/proc/zte/sys/ffe/*`。分类是工程划分，不是原厂源码结构。

## 0. 结论

- 261 个精选符号：`ffe_*` 196、`npu_*` 30、其它 35（CSPKernel QC/QoS、Kernel_ASEND、
  `idm_cfg_*` 等）。
- 已有反汇编 14 个文件，其中 10 个与 `symbols.txt` 名字精确对应；其余是
  `init_ipv6_packet_layer`、`ffe_ipv6_hash`、`npu_cap_nat6_front_create`、
  `CSPKernel_QC_MatchIPv6`（名字不同/在 `ksym-all` 里）。
- W1 必须实现的核心约 **40-50 个函数**；QoS/QC、URL filter、sharedata 展示、
  部分 netlink/邻居事件可以延后。
- 最大未知仍是 **128B 流槽的 proto_state 布局、NPU cap 的寄存器序列、
  FDB/fkb 的交付契约**；这些不是靠符号名能补的。

## 1. 符号分布

| 组 | 数量 | 说明 |
|---|---:|---|
| `ffe_*` | 196 | 流生命周期、协议注册、flush、proc、RPS/backlog、stats |
| `npu_*` | 30 | NPU 插件框架、cap（nat6/pps/mc/retry）、filter |
| `CSPKernel_QC_*` / `CSPKernel_QoS_*` | 28 | QoS/QC 分类器，W1 可延后 |
| 其它 | 7 | `Kernel_ASEND`、`idm_cfg_read/write`、`CSPKernel_Skb_*` |

## 2. W1 必须实现（按数据流顺序）

### 2.1 流键 / 学习 / 插入 / 删除

| 函数 | 状态 | 说明 |
|---|---|---|
| `ffe_calc_key_len` | 已反汇编 | 基键 16B + 协议层追加；64B 描述符表、最多 16 层 |
| `ffe_init_flow_key` | 已反汇编 | 初始化 16B 基键 |
| `ffe_copy_flow_key` | 已反汇编 | 协议层复制键 |
| `ffe_learn_pkt` | 已反汇编 | 首包学习主流程 |
| `ffe_general_alloc_flow` | 已反汇编 | 128B 流槽分配 |
| `ffe_general_insert_flow` | 已反汇编 | 流槽插入/身份匹配 |
| `ffe_general_delete_flow` | 待反汇编 | 删除路径 |
| `ffe_free_flow` / `ffe_general_free_flow` | 待反汇编 | 释放/RCU |
| `ffe_build_packet_cmd` | 已反汇编 | 构造协议层 cmd 链 |
| `ffe_register_packet_layer` / `ffe_register_proto` | 待反汇编 | 协议层注册 |
| `ffe_receive_pkt_slow_path` | 待反汇编 | 未命中回栈/backlog |
| `ffe_receive_skb` / `ffe_learn_skb` | 待反汇编 | gmac/nbif 入口 |
| `ffe_general_xmit_fbd` / `ffe_default_xmit` | 待反汇编 | FBD 出口 |

### 2.2 conntrack / 组播 / 设备事件联动

需要：
`ffe_flush_flow_by_conntrack`、`ffe_flush_flow_by_conntrack2`、
`ffe_flush_flow_by_maddr4/6`、`ffe_flush_flow_by_dev`、`ffe_flush_flow_by_type`、
`ffe_refresh_conntrack`、`ffe_check_ct_status`、`ffe_netdev_event`、
`ffe_neigh_event`、`ffe_ppp_demand_event`、`ffe_netlink_route_notify`。

W1 先做 conntrack + 设备 down/flush；邻居/PPP/netlink 可后置。

### 2.3 NPU 插件

| 函数 | 状态 | 说明 |
|---|---|---|
| `npu_register_driver` | 已反汇编 | 7 槽驱动链 |
| `npu_cap_setup` | 已反汇编 | cap front/backend 挂链 |
| `npu_cap_nat6_front_create` | 已反汇编 | IPv6 NAT 键提取（32B） |
| `ffe_npu_filter_flow` | 待反汇编 | 协议过滤 |
| `ffe_npu_create/delete/refresh/get_stats` | 待反汇编 | 流生命周期 |
| `ffe_npu_enable/disable` | 待反汇编 | 总开关 |
| `npu_cap_*`（pps/mc/retry） | 部分待反汇编 | 可按 W1 需要裁剪 |

### 2.4 协议层

需要：`init_eth/vlan/pppoe/ipv4/udp/tcp/ipv6/multicast_packet_layer`，
以及各自的 parse/output 回调。当前只对 `init_ipv6_packet_layer`、
`ffe_ipv6_hash`、`CSPKernel_QC_MatchIPv6` 有反汇编；其余在 `ksym-all.txt`。

W1 顺序：eth → vlan → ipv4 → tcp/udp → pppoe → ipv6 → multicast。

### 2.5 proc / 控制面

需要：`ffe_proc_cmd_write`、`ffe_proc_cmd_show_help`、`ffe_cmd_proto_on_off`、
`ffe_cmd_enable_ffe`、`ffe_cmd_del_flows`、`ffe_cmd_show`、`ffe_proc_ffeinfo_show`、
`ffe_proto_proc_open`、`ffe_sharedata_seq_show`（可后置）、RPS 四个 cmd。

W1 至少保留 `/proc/zx-ffe/...` 的 on/off/flush/统计；命名不必与 vendor 相同。

### 2.6 RPS / backlog

`ffe_backlog_rx`、`ffe_backlog_poll`、`ffe_rps_enqueue_tx_backlog`、
`ffe_rps_init`、`ffe_rps_process_tx_backlog`、`ffe_rps_trigger_softirq`。
W1 可以先用单队列/直接 xmit，RPS 作为 P3 后段优化。

## 3. 可以延后

| 组 | 函数 | 理由 |
|---|---|---|
| QoS/QC | `CSPKernel_QC_*`、`CSPKernel_QoS_*`、`CSPKernel_skb_SelectQueue`、802.1P/DSCP remark | 首版不做 QoS；tcont/队列先按默认 |
| URL filter | `ffe_set_url_filter_active` | 业务功能，不影响转发 |
| sharedata 展示 | `ffe_sharedata_*` | 调试展示 |
| bridge stats | `ffe_bridge_update_stats`、`ffe_bridge_fdb_update` | 统计，不影响数据面 |
| netlink/邻居/PPP demand | `ffe_netlink_route_notify`、`ffe_neigh_event`、`ffe_ppp_demand_event` | W2 |
| IPv6 nat6/mc | `npu_cap_nat6_*`、`npu_mc_*` | W1 先 IPv6 基本 TCP/UDP；NAT6/MC 后置 |

## 4. 未知项（必须靠反汇编/实测补）

1. **128B 流槽布局**：身份字段 +0x34、proto_state、conntrack 指针、age/stat 位置。
2. **协议层 64B 描述符表**：键长在 -0x18，最多 16 层；每层 parse/output 的 ABI。
3. **NPU cap 寄存器序列**：`npu_cap_setup` 之后 front/backend 的硬件写表；
   `npu_cap_nat6` 的 32B 键只是其中一块。
4. **FDB/fkb 交付契约**：`ffe_receive_fkb`、`fbd_xmit_fkb` 的 buffer 所有权/长度/
   headroom；与 IDM 的 `idm_skb_stack` 如何衔接。
5. **IPv6 原生表**：`ffe_ipv6_hash` + SBRG 间接表的槽号来源（运行态 `npu_cap`
   已证能力，但槽分配未解）。
6. **conntrack 事件**：`ffe_flush_flow_by_conntrack` 的调用点/锁/RCU 语义。

## 5. 实施顺序

1. 反汇编 `ffe_general_delete_flow`、`ffe_receive_pkt_slow_path`、
   `ffe_register_packet_layer/proto`、`ffe_npu_filter_flow`。
2. 从 `ksym-all.txt` 找协议层 init/parse/output，补 `init_eth/vlan/...` 的
   函数地址和调用图。
3. 先写 `zxnpu-ffe.c` 的流键/学习/插入/删除 + proc on/off，不接 NPU；
   用 golden vector 验证键/槽布局。
4. 再接 PPE/CLA/PM 表（P2 的 `zxnpu-ppe`），最后接 NPU cap。
5. 每步都保留“关 FFE 回软件路径”的开关。

## 6. 证据路径

- 符号：`build/re/ffe/symbols.txt`、`build/re/ffe/ksym-all.txt`
- 反汇编：`build/re/ffe/disasm/`
- 结构：`build/re/ffe/ffe-reconstructed.h`
- 运行态：`docs/npu/01-vendor-runtime-baseline-2026-10-29.md`
- 可行性：`docs/npu/02-feasibility-2026-10-29.md`
- 构建：`docs/npu/04-build-npu-0.1-2026-10-29.md`
