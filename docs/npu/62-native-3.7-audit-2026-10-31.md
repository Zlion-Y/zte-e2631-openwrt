# 62 — native-wired-3.7 真机全面审计（2026-10-31）

背景：3.7 已在机稳定运行（13:34 启动，21 min uptime 后审计）。本文按
**类别 × 优先级** 汇总全部已知/潜在问题与修复方案；修复**批量并入下一轮
构建**（3.9），一次刷入，刷后跑 `tests/device-verify.sh` 全量核查。

## 0. 设备现状快照（审计取证）

| 项 | 值 |
|---|---|
| 版本 | /etc/e2631-version=native-wired-3.7；**槽头="OpenWrt ffe-full-0.6"（陈旧，C1）** |
| 内核 | 6.18.55，四个 native 驱动全部内建（非模块） |
| 有线 | **基线驱动 zx279128s-eth 绑定在跑**（A1！），native-eth 已注册未绑定；lan2/wan 1Gbps、br-lan 转发、WAN 网关 1.2ms |
| 无线 | 790a full-probe 过（上会话手动 enable+rescan）；CR 探针**全零**（B1：芯片冷态） |
| 内存/负载 | available 163MB，load 0.00，softnet 零丢包，IRQ 正常 |
| dmesg | 唯一 error：`rdinit=/init failed: -2`（OpenWrt 无 initramfs 常规消息，**非问题**） |

## 1. 有线（zxnative-eth / 基线共存）

### A1 [P0·发现] 有线数据面实际由基线驱动承载，native 从未真机服役
证据：`/sys/class/net/lan2/device/driver → zx279128s-eth`（基线）；
`/sys/bus/platform/drivers/zxnative-eth` 已注册但无绑定；dmesg 无
"native wired data plane up"。**两个驱动同时编入**（基线
main/switch/ffe.c 仍在 overlay files/，先注册先绑定）。
**方案**（分两轮）：
1. 本轮（3.9）：随包交付 `native-eth switch` 切换 CLI（unbind 基线 →
   bind native，10s 内无 "native wired data plane up" 或无端口 up 则
   自动回退基线）。首刷后**手动**执行观察第一次硬件 probe，不自动。
2. native 稳定 N 轮后：从 overlay 移除基线源文件，实现真"全重写"。
DTS 前提已核实满足（`zte,zx279128s-gmac` + ethernet-ports 子节点）。

### A2 [P1·BUG] zxna_remove 只注销第一个端口（unbind 路径 UAF）
zxnative-eth.c `zxna_remove()`：`if (a->ports[i]) { unregister_netdev; break; }`
——其余 3 端口仍注册但 devm 将释放全部 netdev/adapter 内存 → 悬挂注册表项。
且缺 `netif_napi_del`、`hrtimer_cancel`、phylink disconnect/destroy、
INT_MASK 屏蔽顺序不对（先注销后屏蔽）。
**方案**：全部端口注销；先 `writel(~0, INT_MASK)` + `hrtimer_cancel` +
`netif_napi_del`，再逐端口 unregister；phylink 循环 destroy。本批修。

### A3 [P1·BUG] probe 失败路径（unreg）同样缺 netif_napi_del
napi 在 `devm_request_irq` 之前注册；irq 失败 → goto unreg → napi 未删
而 netdev 将被 devm 释放。**方案**：unreg 尾部补 `netif_napi_del(&a->napi)`。

### A4 [P1·缺口] 无开机自动化：重启后管理面靠手工救
enable 默认 N、probe -ENODEV；上会话 enable+bind 均为手工。任何无人值守
重启 = eth 不起（SSH/LuCI 失联）。
**方案**：A1 的 switch CLI 兼作显式启动器；**下一轮**（native 服役后）
改为 init.d 自动启动。本轮基线仍在，基线开机自起保住管理面。

### A5 [P2·观测] native-eth 无 /proc 接口、ethtool 无统计
唯一观测 = dmesg + /proc/net/dev。**方案**（下轮）：/proc/zx-csp-eth/status
（端口/link/ring/计数）+ ethtool get_sset_count/strings/stats。

### A6 [P3·性能] offload 全关：tx-csum/sg/tso off（[fixed] rx-csum off）
基线亦如此。软件校验和是当前 1Gbps 可跑的原因之一（保守正确）。**方案**：
P3 阶段拿到 PPE/硬件 csum 证据后再开，不提前。

## 2. 无线（zxnpu-wlan）

### B1 [P1·架构] 只绑 790a 无法点亮芯片：CR 探针全零
证据：`echo probe` 后 cr[18000000..180282f4] 全 0x00000000 —— 无任何驱动
触碰 7906 主功能（mt76 已剥离），芯片未上电/未时钟，790a 的 BAR 读零。
**方案**（下一轮设计，不进本批）：id_table 扩到 7906+790a 双实例；
7906 实例负责电源/时钟/补丁下载（FWDL 走主 HIF），790a 实例挂 BAND0/1
数据面。这是 P5 dataplane 的前置。

### B2 [P0·已修→3.8] parked probe：proc/VAP 控制面常驻
probe 始终成功，enable=0 只跳过 PCI/BAR 段（零硬件访问）。
### B3 [P0·已修→3.8] vap 参数 0444→0644
内建驱动改参免重启；配合 remove+rescan 重探生效。
### B6 [P0·已修→3.8] submit ring parked 防护（!w->bar[0] 拒绝，防 NULL 解引用）

### B4 [P2·观测] status 是单发缓冲：probe/fw 审计日志被后续 ctl 命令清掉
ctl write 处理器每次 `memset(w->status)`。**方案**（本批）：
probe/audit 结果改写到独立持久的 `probe_log`，status 保留两段式
（首段 probe 记录 + 当前命令输出）。

### B5 [P2·观测] fw 审计（CRC）boot 后无法重触发
**方案**（本批）：新增 ctl 命令 `fw-audit`（复用 zxnpu_wlan_audit_firmware）。

### B7 [P2·计划] P5 dataplane 未完成（802.11→802.3 转换、wcid→VAP 映射、
beacon 编程）——路线图项，依赖 B1。

## 3. 打包 / 版本 / 槽位

### C1 [P0·已修→3.8] 槽头版本戳陈旧（ffe-full-0.6）
根因链：build.sh 不传 --version + 本地 .env 残留 E2631_VERSION=ffe-full-0.6
+ env.sh 不转发该变量 → release.mjs 拷贝 512B 模板头原样。
**修复（已验证）**：新增 `scripts/pack.sh` 单一打包实现——版本号强制取自
overlay `etc/e2631-version`（单一权威源）传 --version；build.sh 收敛调用；
.env/.env.example 清残留。演练包 `mkfactory info` 实测
`version="OpenWrt native-wired-3.8"` 端到端通过。

### C2 [P2·流程] e2631-fwslot upgrade 语义固化
原位重写运行槽（serial 自动 max+1，cspboot 选高 serial——设备侧已自动），
**禁用 install/flash**（写另一槽=原厂系统）。写进 switch CLI 注释与
BUILD 文档。

## 4. 系统 / 其他

### D1 [P3] zxnpu0 全零 MAC（IDM netdev 未设 eth_hw_addr_random）+
probe 失败路径 singleton 不清。**方案**：两行小修，本批顺手。
### D2 [P3·噪音] dnsmasq "DNS-rebind dns.msftncsi" —— 联网检测域名被
rebind 防护误报，可忽略或加 rebind_domain 白名单，不修。
### D3 [非问题] odhcpd ra_lifetime=0 —— 上游未下发 PD，LAN 无公网 v6 前缀，
行为正确。
### D4 [非问题] `rdinit=/init failed: -2` —— 无 initramfs 启动的常规回退。

## 5. 本轮修复批次（→ native-wired-3.9，一次构建）

| # | 项 | 层 | 状态 |
|---|---|---|---|
| 1 | A2/A3 zxna_remove/unreg 清理（全端口注销+phylink+napi_del+顺序） | 内核 | 本批修 |
| 2 | B4/B5 wlan 持久 probe_log + /proc/zx-csp-wlan/probe + fw-audit | 内核 | 本批修 |
| 3 | D1 IDM 零 MAC（singleton 清理已有，实误报） | 内核 | 本批修 |
| 4 | A1 第 1 步 native-eth switch CLI（手动触发+回退） | overlay 脚本 | 本批交付 |
| 5 | C1 版本戳 + B2/B3/B6 parked/vap | 内核+脚本 | 已修（3.8 构建） |
| 6 | `tests/device-verify.sh` 刷后全核查（V1-V5 五段） | 测试 | 本批交付 |

不进本批：B1（7906 架构，下轮）、A4（init 自动化，native 服役后）、
A5/A6（观测/性能，下轮）、B7（P5 路线图）。

## 5b. 补充：机械扫描发现（全部并入本批）

独立代码扫描（3 文件 3700 行）在自查之外新增 9 项，全部修复：

| # | 发现 | 严重度 | 修复 |
|---|---|---|---|
| M1 | **`strncmp(cmd,"mcu",3)` 前缀遮蔽**：mcuwin/mcu-cmd/mcu-dl/mcu-dl-regions/mcu-recv/mcu-send 全部死代码——MCU/FWDL 命令面整体不可达 | 功能（重大） | 精确匹配（`cmd[3]` 须为 `\0`/`\n`） |
| M2 | wlan remove 从不注销 VAP/删 NAPI：remove/rescan 迭代下 netdev 悬挂（`->w` 指针 UAF，P5 接 poll 即炸） | 潜在 UAF | `zxnpu_wlan_vaps_exit()`（unregister+napi_del+free） |
| M3 | `mcu-dl-regions` `off+len` 无符号回绕 → 巨值参数把野指针交给 DMA | 内存破坏（潜伏） | 减法形式 `len > size ‖ off > size-len` |
| M4 | patch 头守卫 `>30` 但读 +0x20..0x27：31-35B 文件越界读 | 越界读 | `>= 0x28` |
| M5 | `first16=%*phN` 无 16B 下限守卫 | 越界读 | `>=16` 分支打印 |
| M6 | 区域表 `off+len` u32 回绕（镜像内伪造 len） | 内存破坏（潜伏） | 减法形式 |
| M7 | 十项 `dmam_alloc_coherent` 全不判空；`dband_ring` 失败时 DMA 基址仍写硬件 | 低概率高影响 | 统一判空 → -ENOMEM |
| M8 | BAR 表错位：申请 BIT(0)\|2\|4 却存 table[0..2]（bar[1] 恒 NULL，BAR4 无人领，mcuwin 标签错） | 诊断失真 | bar[i]=table[0/2/4] + resource 索引 0/2/4 |
| M9 | `zxna_mac_words` 有符号移位（gcc 实际正确） | 规范 | `(u32)` 提升 cast |

扫描同时证实安全：wcid 分配器边界、proc 解析宽度、mutex 平衡、
cidx/didx 模运算、PPE 槽映射/CLA 命令守卫。注意：M3/M4/M6 是因 M1
潜伏——单独修任何一个而不修 M1 会把它们变成活的内存破坏。本批一起修，
正是"批量修复"的意义。

## 6. 刷后核查流程（每轮固定）

`bash tests/device-verify.sh`（V1 版本/槽头、V2 健康无 oops、V3 有线
功能/IRQ、V4 native 状态/proc、V5 稳定性指标）+ 手工附加：native-eth
switch 观察（如本轮）+ wlanmgr step1。全过才进下一轮。
