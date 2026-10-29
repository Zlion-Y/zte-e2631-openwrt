# ffe-full 分支：原厂硬件加速完整逆向与移植

> 本分支**不合并到 main**。它是"中兴原厂有线/无线硬件加速（FFE + IDM/NPU）
> 在 ZX279128S 上的完整逆向与移植"的独立研究成果，含大量实机验证记录与
> 一套完整的诊断设施。main 保持已验证的软件路径（v1.8.0 系列）。

## 一、这个分支做了什么

在 main 既有的 FFE 控制面（`zx279128s-ffe.c`）基础上，把原厂无线加速链路
**逐层逆向并移植到 6.18**，直到实机跑通，然后如实记录它在什么边界上不可用。

### 逆向（全部有反汇编/实机证据，产物在 `re/ffe/`）

| 结论 | 证据 |
|---|---|
| 原厂有线+无线 = 同一套 cspkernel4.1 内建 FFE + NPU（tm.ko 是引擎寄存器 SDK） | 泄露路径 `/zte/csp/opensource/cspkernel4.1/protocol/ffe/`、11 个符号族 |
| IDM 环：RX 2048×8B + TX 1024×8B；`+0x04`=TX 基址、`+0x08`=RX 基址 | plat `pon_npp_idm_init` 注解反汇编 |
| **RX 缓冲必须逐个经 `IDM+0x48` 提交，写常量 `0x10000`（不是缓冲地址）** | plat `0x67b0: mov ip,#65536; str ip,[idm+0x48]` |
| 引擎参数 `+0x10/0x18/0x1c/0x24`、深度 `+0x0c`、使能 `+0x00`、NPP 全局掩码 | plat `tm_pon_npp_init`/`pon_npp_idm_init` 逐条转写 |
| WiFi 伪端口 0x18-0x1F 无上电寄存器——是 CLA `w0[23:12]` 的硬布线译码 | tm.ko `tm_acl_get_fastHashRule` 0x49138 跳表 |
| 原厂 IPv6 用 SBRG 间接表（非 CLA 哈希），384 位表项，60/60 复验 | 上游 zwzzrl fork `cc661ebc`/`70858ac1`（已记录在 `docs/`） |
| 无线数据面 = `idm_skb_stack_push/pop` + `Kernel_ASEND`，驱动直通 | E2631/F670L `mt7916.ko` readelf 导入表 |

### 移植（本分支代码）

- `zx279128s-idm.c`：IDM 环形引擎（按 vendor 序列重写两轮）
- `zx279128s-ppe.c`：无线 VAP ingress 判定、12 位 egress、无线 egress 解析、
  NAT 安全门、15 类流拒绝原因计数
- `zx279128s-ffe.c`：修复接缝指针永不赋值的 bug（FFE 控制面此前从未生效）
- 内核补丁 `035`（IDM 编入 eth 驱动）、`036`（`nf_flow_table` 无线入口钩子）
- mac80211 补丁 `991`（`TC_SETUP_FT` 转投给 PPE）
- `docs/ffe-full-完整逆向与移植报告-2026-10-29.md`
- `docs/ffe-full-实机迭代状态-2026-10-29.md`（逐轮实机记录）

## 二、实机验证结果（这是本分支最重要的内容）

在真机（E2631，`OpenWrt ffe-full-0.11`）上完整跑通并测量：

```
idm rx_frames:  0 → 6018
rx_submit:      每一轮都精确等于 2048 + rx_frames    ← 环零失步、零泄漏
rx_unicast:     3683/6018 = 61% 真实单播
ppe_flows:      20-27 条流同时在硬件表
HW_OFFLOAD:     20-26
CPU 负载:       0.19-0.24（几乎空闲）
ppe_add_fail:   ≤7
```

**结论：机制正确，架构不足以承载生产流量。**

三个无法回避的架构性原因（详见
`docs/ffe-full-实机迭代状态-2026-10-29.md` 第八节）：

1. **39% 的交付帧是非单播**（广播/组播/ARP）。交给 mac80211 的 AP 发送路径后，
   其多播队列/速率/聚合策略与原厂"驱动直通自己构造 802.11 帧"不同构，
   ARP/NDP 一丢新连接就建不起来——这表现为"有的网站能开、有的不行"。
2. **帧级不可回退**。流进了 PPE 表后内核流表永久移交，硬件交付的任何缺失
   都是直接丢包。原厂用 `conntrack 联动 flush + retry` 补，我们未重建该状态机。
3. **空口有损**（2.4G `tx failed` 33%）。软件路径靠重传兜住，硬件路径兜不住
   就被放大成断网。

**根本差异**：原厂 `mt7916.ko` 是 MTK 私有 AP SDK（5.6 MB / 7760 函数，
不用 cfg80211），自己从 IDM 环取帧、自己写 TXD。我们用开源 mt76 + mac80211，
中间那层抽象把帧级控制权（重试/聚合/队列）交给了内核。**逆向下得了寄存器，
还原不了这层架构。**

## 三、默认行为

本分支固件**默认关闭硬件流卸载**（`uci-defaults/66_e2631_hw_offload_default_off`）：

```
firewall.@defaults[0].flow_offloading_hw = 0
firewall.@defaults[0].flow_offloading    = 1   # 软件流表，稳定
```

想复现实验：

```sh
uci set firewall.@defaults[0].flow_offloading_hw='1'
uci commit firewall
echo 1 > /sys/module/zx279128s_eth/parameters/wifi_offload
fw4 reload
cat /sys/kernel/debug/92000000.ethernet/dma_state    # 观察 idm/ppe 计数器
```

关闭：把上面两个值改回 0。

## 四、本分支同时修好的、与硬件加速无关的实机问题

这些值得保留，与 main 的差异点在这里：

- **WiFi 160 MHz**：5G 从写死的 HE80 改为 **HE160**（设备支持，实测
  `channel 36 width: 160 MHz`，客户端协商 1441 Mbps HE-MCS 7 2SS）。
  2.4G 保持 HE40+noscan。
- **5G 独立 SSID**：`ChinaNet-ygckk9_5G`，避免客户端自动连到拥挤的 2.4G。
- **IPv6 中继固件化**：上游不分配 PD 的部署形态内置
  （`uci-defaults/65_e2631_ipv6_relay`：lan ra/dhcpv6/ndp=relay +
  wan6 master + `sourcefilter=0`），刷机即生效，不再手工重配。
- **PPE 野指针卡死修复**：`zx_eth_setup_tc()` 对 mac80211 转投的 wlan netdev
  取 `netdev_priv()->adapter` 是野指针 → 流表绑定卡死 → 看门狗复位循环。
  改为 `zx_idm_adapter()` 单例解析。
- **build.sh 版本号自动化**：构建前把 `E2631_VERSION` stamp 进 rootfs 并在
  收集后强校验（此前每版都要手工补，多花 3 轮）。

## 五、诊断设施（长期资产）

`/sys/kernel/debug/92000000.ethernet/dma_state` 现在输出：

- IDM 计数：`rx_frames/rx_dropped/rx_novap/rx_submit/rx_unicast`、分通道
  `rx_ch[8]`、`inject_ok/inject_full/tx_kicks/tx_queued/hook_fired`、`egress_low`
- PPE 流拒绝原因 15 类：`ppe_skip[meta nock v6 nobasic nonip nol4 nokeys
  ip4off l4off htype action noodev wifiegr badmac hashfull]`
- 无线出口失败 4 类：`wifi_egress_skip[badadapter notbridge wrongbridge nofdb]`

定位这类问题不必再靠猜。

## 六、构建与产物

```sh
bash scripts/bootstrap.sh
bash scripts/build.sh          # 产物 out/e2631-openwrt-ffe-full-<ver>.bin
```

- 本分支最后一个实机验证产物：`e2631-openwrt-ffe-full-0.11.bin`
  （`verify_pkg` ALL PASS + `verify_img` BOOT-READY）
- 刷机：`/usr/sbin/e2631-fwslot upgrade <bin>` 原地重写运行槽（原厂槽不动），
  然后 `ubus call system reboot`

**注意**：本分支的升级流程**不保留 uci 设置**（rc.local、v6 中继、防火墙
开关都会被重置）。v6 中继与硬件卸载默认值已固件化，其余需自行重配。

## 七、致谢

- **@zwzzrl**：IPv6 硬件表（SBRG 间接表 + 384 位表项布局）的突破、
  PPE IPv6 trap 发现、wifi-boot 重协商修复
- **@zwthys-cyber**：升级加固与 CI 基线
