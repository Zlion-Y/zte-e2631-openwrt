# 38 — 双驱动全功能迭代规划（2026-10-31）

> 用户目标：基于原厂源码与行为，写出有线 + 无线两套驱动，**均实现硬件加速**，
> 构建功能完整、可正常使用的固件；允许实机探测**原厂系统**行为作参考（只读）；
> 自行规划迭代、逐步离线验证。本文是迭代总纲：每轮任务拆解、验收标准、验证
> 方式，以及硬件加速的实现范围与"可用"的判定依据。
>
> 前提约束不变：我们的固件**不刷实机**（实机探测仅限原厂系统只读）；
> 真机验收（清单 36）等用户放行刷机后执行。

## 1. 起点（已就绪）

- 有线：`zxnative-eth.c`（1055 行，doc 30）——复位/时钟/SBRG/PP/TM/BMU/
  DMA/MAC/phylink/数据面全骨架，enable=0，**未真机验证**；021 已移除。
- 无线：`zxnpu-wlan.c`（1365 行）——G3 全链（probe/CR/FWDL 双环 qid16/17/
  mcu-dl/fw-dl/fw-state），**未真机验证**；固件分段表完整解码（doc 33）。
- 原厂行为证据：ffeinfo/IDM/kallsyms/npu_cap 活体采集（doc 17）、
  事件分发链骨架（doc 37 §2.1）、zwzzrl 双源核验（doc 16 §7）。

## 2. 硬件加速的实现范围（明确口径）

### 有线硬件加速 = PPE 流卸载（IPv4）

- **范围**：`zxnative_eth` 在数据面稳定后接 PPE/FFE：45B key 哈希放置
  （doc 12 规格）、PM 重写表、7-bit 改写索引、127 流容量 + -ENOSPC 回软
  （doc 16 §2.5）；v6 明确**不做硬件卸载**（原厂自身 v6 L3 也是纯 CPU
  143 Mbit/s 上限，doc 16 §7——这是双方独立实证的结论）。
- **实现位置**：`zxnative-eth.c` 的 ndo_setup_tc 路径 + FFE 控制面
  （原厂 /proc/zte/sys/ffe 语义的等价物）。
- **验收**：真机 LAN→WAN 吞吐测试（对照原厂 ≥900 Mbit/s、FFE 软路径
  143 Mbit/s 的分界）+ ppe_add_fail/流计数对齐 doc 17 §2 口径。

### 无线硬件加速 = IDM/WFDMA 环数据面

- **范围**：下行 = PPE/PP 重写 → IDM RX 环 → 交付 VAP（原厂 idm1 模型，
  doc 17 §1.6）；上行 = VAP → IDM TX 环注入；**不做** mac80211 注入
  （ffe-full 0.11 已定性不可承载生产流量）。
- **依赖顺序**：FWDL 链真机通过（清单 36 §2）→ 事件环/ack（doc 37）→
  WFDMA 数据环（base/cnt/cidx/didx，doc 14 §6.5.3 布局已解）→ BSS/关联
  （清单 36"尚未覆盖"项）。
- **验收**：真机 dma_state 式计数（对齐 ffe-full-0.4 的下行实证口径）+
  客户端关联后双向吞吐 + 空口失败率（对照 0.11 的 33% 失败教训）。

### 明确不在范围（记录取舍）

- v6 硬件卸载（原厂自己不做）；xdpi/DPI 会话加速（原厂 xdpi.ko，超出
  首版）；mesh/WPA3/160M 全功能（清单 36 口径：先最小 AP）。

## 3. 迭代轮次（每轮：任务拆解 → 离线验证 → 验收标准）

### R1（本轮）：MCU 事件环分发链 + ack 格式

- 拆解：①分发链骨架（type 0..7 → 事件类）✅ 已入 doc 37 §2.1；
  ②arch_ops[448] PCI 实现定位 → 事件头字段偏移（事件 ID/seq/len）；
  ③ack 匹配点（等待队列 completion）。
- 验证方式：全部结论带 mt7916.ko 反汇编地址引用（双源：zwzzrl 侧无此
  解码，纯 vendor 依据）。
- 验收标准：事件头字段表落 doc 37；`zxnpu_wlan_ack_expect()` 骨架
  （只记录期望不阻塞）进 1.6；双门禁过。

### R2：EEPROM/l1profile 初始化序列

- 拆解：vendor dat 文件（l1profile.dat.dbdc、mt7916.dbdc.b0/b1）逐字段
  语义 + 初始化命令序列（对照清单 36"尚未覆盖"第二项）。
- 验证：与原厂 /etc/wireless/mt7916 清单（board/etc-wireless.out.txt）
  逐字段对照；编码器输出与文件字节一致。

### R3：zxnative-eth 统计/ethtool + PPE offload 骨架

- 拆解：ethtool stats（原厂 gmac 计数寄存器表）、PPE offload hook 骨架
  （**默认关闭**，enable+commit 双门——不违反 Qoder 的顺序线，因为
  默认路径不变）。
- 验证：check.sh + 双门禁 + 寄存器写入计划对照 doc 12/16 表。

### R4：WFDMA 数据面骨架（无线）

- 拆解：TX/RX 数据环分配与寄存器编程（doc 14 §6.5.3 布局）、NAPI、
  队列→VAP 映射。
- 验证：离线单测（描述符构造 golden vector）+ 双门禁；真机验证在 R5。

### R5（需真机窗口）：清单 36 验收 + offload 接回 + 联调

- 刷机授权后：清单 36 §1-2 → 数据面 → offload → 吞吐验收（§2 口径）。

## 4. 固件"可用"的判定依据（分层口径）

| 层级 | 判定 | 当前状态 |
|---|---|---|
| L1 可刷写 | 双门禁过 | ✅ native-wired-1.5 |
| L2 可启动 | 真机串口起 shell、无重启循环 | ⏳ 需刷机授权 |
| L3 有线可用 | LAN/WAN DHCP、ping、iperf 双向 | ⏳ 同上（zxnative-eth 未验证） |
| L4 无线可用 | 客户端关联 + 透传 | ⏳ 依赖 L3 + FWDL 链验证 |
| L5 有线加速 | IPv4 流走 PPE，吞吐 ≈900M、CPU 降 | ⏳ R3-R5 |
| L6 无线加速 | 双向走 IDM/WFDMA，dma_state 计数增长 | ⏳ R4-R5 |

"功能完整、可正常使用" = L4 达成（无加速的完整可用）+ L5/L6 达成（加速
版）；判定全部以真机实测为准，离线门禁只是 L1 级证据。

## 5. 风险与顺序约束（继承）

- Qoder 顺序线保留：清单 36 第 2 步（FWDL 真机验证）通过前不改
  `zxnative_eth` 的 offload 默认行为（R3 只做门禁内骨架）。
- 一次真机窗口只动一个变量；原厂槽保留；失败即停（清单 36 §3 分诊表）。
