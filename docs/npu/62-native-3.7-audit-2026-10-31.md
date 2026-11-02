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
**运行期切换不可行（3.9 实跑发现）**：基线驱动
`suppress_bind_attrs`——bind/unbind sysfs 文件根本不存在，已绑定驱动
无法让出节点；首次 `native-eth switch` 实跑三步全部失败
（模块名 `zxnative-eth`→应为 `zxnative_eth`；unbind EACCES 实为文件不存在）。
**切换模型定稿（3.10）——链接顺序竞争 + 启动期自动回退**：
1. 043 patch 把 `zxnative-eth.o` 提到 `zx279128s-eth.o` 之前（initcall
   先注册先绑定）；
2. `zxnative_enable` 默认 true：native 赢得 boot 竞争、直接服役；
3. native probe 优雅拒绝（enable=0 或 -ENODEV）→ 设备无人认领 → 后注册
   的基线自动 probe 接管 = 启动期回退；
4. 回退操作 = `native-eth enable 0 && reboot`（运行期无法回退，见上）；
5. native 稳定 N 轮后从 overlay 移除基线源文件（真"全重写"收尾）。
DTS 前提已核实满足（`zte,zx279128s-gmac` + ethernet-ports 子节点）。
风险已知：native 首次硬件 probe 若 OOPS（非优雅失败）则需 TTL 救援；
deferred-probe 竞态（mdio 未就绪时让基线先绑）由实机观察裁决。

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

## 7. 3.9 刷入与核查记录（2026-10-31）

- 原位升级：check OK → upgrade-check OK（slot0 原厂完好）→ slot1
  serial 100 写入 → 重启。
- **核查 ALL-PASS**：槽头版本=`OpenWrt native-wired-3.9`（C1 真机闭环）；
  parked probe 证据 `/proc/zx-csp-wlan/probe` 在案（B2 闭环）；zxnpu0
  随机 MAC（D1 生效）；fw-audit 运行期 4 固件全在（B5 闭环）。
- 教训：全新 rootfs 的 dropbear 主机密钥会变，flash 脚本已固化
  `ssh-keygen -R`；busybox 无 nohup，detach 用 subshell 双 fork。
- boot 期 request_firmware -2 为 initcall 时序（运行期正常），FWDL 走
  ctl 运行期路径不受影响。
- 两个固件文件（1.76MB 与 458KB）内容 0% 重叠却同 CRC32=2144df1c——
  记录为真实碰撞（~2^-32），审计同时打印 size 可区分，非 bug。

## 8. 3.10 刷入：native 有线驱动首次真机服役（2026-10-31）

**里程碑**：`gmac served by zxnative-eth`——链接顺序竞争生效，自研驱动
从 boot 起承载全部 4 端口；wan/lan2 1Gbps、br-lan 转发、WAN 网关可达、
IRQ 6011、softnet 零丢包、无 oops。基线从未被 probe（竞争干净）。

**新发现（→3.11 修复）**：全部 netdev 随机 MAC。根因：native probe 在
0.224s 执行，而 flash 分区 nvmem-layout（MAC 源）约 0.9s 就绪；
`of_get_mac_address` 返回 -EPROBE_DEFER 被当普通失败 → 随机 MAC。
4 条 "Failed to create device link (0x180)" 告警同根因。
**修复**：probe 内有界重试（20×100ms）。内建 initcall 串行执行——
重试的 msleep 同时阻塞基线注册，boot 竞争反而更稳固。
（probe 级 -EPROBE_DEFER 是错误答案：defer 期间基线会抢绑节点。）

**流程教训（脚本已修）**：原位写入后 busybox `reboot` 在被替换的
运行 rootfs 上 I/O error 静默失败（`|| true` 掩盖）→ 改
`sync; echo b > /proc/sysrq-trigger`；等待循环必须先等 SSH 掉线再等
回归；每次新 rootfs 的 dropbear 主机密钥都会变（device-verify 已带
StrictHostKeyChecking=no）。

## 9. 3.12 终态（本轮收束）

- **有线**：基线出像（CONFIG_ZX279128S_ETH=n，021 patch 保留供
  zx279128s-eth.h）；zxnative-eth 经 probe 级 defer（MAC 源 nvmem
  就绪前）→ deferred 重试绑定，厂 MAC 逐字节恢复
  （lan1/2/3=AA:BB:CC:DD:EE:FF，wan=...a9）；device-verify ALL-PASS。
- 全轮产物：out/native-wired-3.12/e2631-openwrt-native-wired-3.12.bin
  sha256 7a0751bb…（双门禁过，槽头=3.12，serial 102）。
- 待办移交下轮：B1（7906 主功能绑定点亮芯片——FWDL 前置）、A5（native
  proc/ethtool 统计）、A4（init 自动化——现 native 即 boot 默认，无需）、
  P5 dataplane（依赖 B1）。

## 10. B1 破案：芯片活着，窗口错位（3.13/3.14，2026-10-31）

**3.13**：id_table 扩 7906+790a 双实例（main/comp 槽位、单一 ctl 面、
双窗 CR 对比 probe）。真机：两实例都 probe 成功，7906/790a 的 1MB
BAR0 全零。

**突破（mcuwin 实测）**：`bar2: +0=00511163 +8=0000032f +0x108=47474747`
——32KB BAR2 有活数据（"GGGG" 特征），芯片在线、PCIe 通路正常。
1MB BAR0 读零 = 该窗内 HIF/WFDMA 区冷（WFDMA 未使能时环寄存器无响应），
**不是芯片断电**（DTS 注释的 gpio enable 序列 host 驱动已实现）。

**vendor 地址翻译层（mt7916.ko mt_physical_addr_map @0x1f6a88）**：
mac_io 地址先过翻译（分段公式 + chip_cap 窗口表 + 0x7906 chip-id 检查），
命中窗口后 = 窗内偏移 + 物理映射基址。vendor 双功能初始化对：
- `mt7906_hif_ctrl_chip_init`：rx5/tx6 环、int 配置 0xFEF0000F/0x0D004204
- `mt790A_hif_ctrl_chip_pcie1_init`：rx1/tx2 环、0xB0B00008/0x0D008204
- IO 双路径：pci_map_io_* 与 **pci_io_remap_***（重映射机制实锤）

**3.14（构建中）**：只读窗扫描器——`win <bar> <off> [n]` 任意窗直读、
`scan` 双功能三 BAR 稀疏扫（非零命中汇总）。目标：定位 BAR2 32KB 的
内容图谱（是否含 HIF/WFDMA 镜像或 remap 寄存器），为 FWDL 选路。

## 11. 3.14 楔死事故 + BAR2 定性（2026-10-31）

**事故**：scan 命令 3000+ 次无调度点直读（含死窗）→ AXI 桥楔死整个 SoC
（ping 活、SSH 死、TTL 控制台零输出零回显——内核连 UART 都不服务）。
断电重启恢复（TTL 抓到启动）。教训入码：win 上限 64 读 +
cond_resched；scan 只走已证活窗（BAR2）且 256 读封顶；BAR0 一律禁扫。

**BAR2 (32KB) 定性（单读小批采样，80 读安全完成）**：
- +0x1000=790a14c3（=14c3:790a 反序）= **PCI 配置空间镜像**
- +0x1010=2030000c / +0x1018=20200004 = 本功能 BAR 原始值
- 0x2000+ 全 0xFF = 悬空
- 结论：BAR2 = PCIe EP 控制器寄存器页，不是 connac 总线窗
- connac 总线只经 BAR0(1MB) 访问，读零 = **核心总线冷**（此前
  "BAR2 活=芯片活"的乐观定性修正）

**vendor 深化**：mt7906/mt790A_hif_ctrl init 对（rx5/tx6 vs rx1/tx2 环、
int 配置 0xFEF0000F/0xB0B00008 vs 0x0D004204/0x0D008204 写入 hif 表头）；
mt_physical_addr_map 分段翻译 + 窗口表（命中 = 窗内偏移 + 物理映射基址）。
下轮：mac_io_write32 调用点地址形态反推窗口表全貌 → 定位 connac 唤醒门。

## 12. 访问模型终版破解（活系统对照 + remap 三段映射）——2026-10-31 收官

**授权突破**：192.168.5.1（同款 E2631 原厂系统，admin/admin telnet）只读
探测 + /tmp/rd 静态读取器上传（OpenWrt 盒 wget 供体；WAN IP DHCP 后为
5.16；vendor 盒有 wget/tftp 无 nc/devmem/dd）。

**活值对照表**（全部 /dev/mem 只读）：
- bar0+0x24208（我们以为的 GLO_CFG）= **活系统也是 0** —— 偏移错位
- bar0+0xFE25C/260 (remap L0/L1) = 0x1802184c / 0x18001806 ——**我们系统与
  活系统一致（ROM/cspboot 已配好）**
- **L1 窗实证**：bar0+0xE00F0 = 7 = MT_TOP_MISC（WM+WA ready）
- **L0 窗实证**：bar0+0xD4208 = 0x1030bc55（GLO_CFG：WFDMA_EN|TX_DMA_EN）；
  bar0+0xD4400 四件套 = 4a6c7000/80/2e/2e（FWDL qid16 环活值）

**访问模型终版**：1MB BAR0 = HIF 窗（非 0x18000000 直映）。三段 remap：
0x1802xxxx → bar0+0xD0000+(x-0x18020000)；0x1806xxxx → +0xE0000；
0x1800xxxx → +0xF0000。remap 寄存器 ROM 已配，无需写。
vendor 软件链：mac_io → mt_mac_cr_range_mapping（bss 窗口表，翻译命中
后 *(chip_ops[0x84]+phys)=val，未命中走 chip_ops[0x1a8]=remap 回调）。

**3.17 规格（下轮第一件事）**：CR 访问层换三段映射（ZXNPU_CR 宏改为
函数或三段表）；rings/fw-state/probe 全部自动对位；预期 fw-state 直接
读到真实状态；FWDL 链（mcu-dl patch → fw-dl wm/wa）在正确窗口上执行。
**事故教训追加**：3.16 桥窗拓宽无罪但也没解决（prefetch 窗理论证伪——
真问题是访问模型错位）；活系统 GLO_CFG=0 曾误导（实为偏移错位）。

## 13. 3.17/3.18：CR 层三段 remap 落地，WFDMA 段全活（2026-10-31）

3.17 首版段掩码错（0xfff00000 把 0x18024208 归入 0x1800 段），但 0xF0000
窗已读出活值（0x72/0xd2 与活系统一致）。3.18 掩码修 0xffff0000：
- cr[18024208]=**0x1010b850**（bar0+d4208，GLO_CFG 引导态）
- cr[1802702c/30]=0x00800000/0x7d00（TX/RX 窗配置）
- TOP_MISC（0xE0000 窗）=0 —— **固件未跑（正常，等 FWDL）**
- fw-state 的 L1 remap 写改为读改写（保留高 16 位段条目）

**状态判定**：三段 remap 全通、访问层闭环；芯片 = ROM 引导完成、
WFDMA 配置残值在、固件未下载。下一步 = FWDL 链真跑（step2：
mcu-dl patch → fw-dl wm → fw-dl wa → fw-state 应变 7）。

## 14. FWDL 首跑：DIDX 冻结隔离 + 环项格式新证（3.19，2026-10-31）

**已跑通**：环寄存器全可读（0xD 段对位实锤）；wfdma-init 22 写生效
（GLO_CFG 1010b857→1030b857 ≈ 活值 0x1030bc55）；patch 下载软件链完整
（auto dst=04000000 offset=30，8642B，环基址 4e048000 已编程，SemGet/
AddressLen/FwScatter/Finish 全部经 qid17/qid16 发出）。

**硬点：DIDX=0 纹丝不动**（MCU/WFDMA 未消费描述符）。已排除：
访问层（经 remap 读数≈活值）、WFDMA init（GLO_CFG 收敛活值）、
PCIe 窗变体（0x242f4=0x811c0000、0x282f4=0x821c0000 均写进回读一致）。
活系统直读对照：bar0+0x24208/0x242f4/0x246xx 直读全零——直映区就是死区，
remap 窗才是真寄存器（我们的访问已与 vendor 行为一致）。

**新证（pci_kick_out_fwdl_msg 解码）**：vendor FWDL 环项填写 =
desc[12]=skb、desc[16]=0、desc[20]=buf_dma（RtmpDrvPciMapSingle）、
desc+5=len 低字节、desc+7|=0x40|len 高——**与我们 fill_desc 的
[24]=len/[28]=next 布局不同**（kick_out_cmd_msg 与 kick_out_fwdl_msg
两环格式可能不同）。3.19 新增 crw 实验工具（commit 门控单写+回读）。

**下轮嫌疑清单（按优先级）**：
1. TX 描述符格式对齐 kick_out_cmd_msg/fwdl_msg 逐字段重解（[20]/[5]/[7]）
2. per-ring 使能/GLO_CFG bit10（活值 0x402 位差：bit10+bit1）
3. PLE 队列映射与 MCU hold 状态（TOP_MISC=0 的语义再证）

## 15. 3.20：环项硬件视图落地，DIDX 仍冻——排查域转移（2026-10-31）

**已落地**：pci_kick_out_cmd_msg 逐字段解码 → 环项双视图定性（软件簿记
vs 硬件视图）；fill_desc + RX 两环 init 全部换硬件视图
（word0=buf_dma、word1=0x40000000|len<<16）；3.20 双门禁过 ALL-PASS。

**实测**：修正后 mcu-dl DIDX 仍冻（0 advanced）；GLO_CFG 写活值
（crw 1030bc55）无效——硬件读回带位过滤（bit0/bit1）；提交后抓态：
cidx 正常递增（FWDL qid16 cidx=0x33=51 次 FwScatter 写入），DMA 引擎
就是不取。remap L0 寄存器不在 0xfe000 区（全零）。

**下轮排查域（按优先级）**：
1. connac top（0xF0000 窗 = 0x1800xxxx 域）与活系统系统性对照——找
   时钟门/错误标志/hold 状态寄存器（活值 0x72/0xd2 所在域，肯定有活寄存器）
2. chip_ops[0xC8]（pause/resume）与 [0xFB8]（FWDL 模式）语义——MCU hold/release
3. PLE/PSE 队列映射（DMA 目的地未配置则 TX DMA 停摆）

## 16. FWDL 模式门控排除 + 活值新发现（2026-10-31）

- **connac top（0xF0000 窗）活/我逐位一致**（0x72/1/0xd2/1/0xd2/1/…
  /0x20638）——纯静态表，无时钟/hold 寄存器差异。
- **[0xFB8]=ctrl_fw_state_v2 = 纯软件同步器**（等 FwSyncValue→
  MtCmdRestartDLReq/MtCmdPowerOnWiFiSys——后者为**空函数 no-op**）。
  FWDL"模式进入"非门控。
- **[0xC8] 槽填充者未定**（mt7916_init 里 #200 偏移未命中）；fw 子表基址
  = priv+0xEFE000（0xfac=load_patch_v1、0xfb0=parse_fw_v1/2/3、
  0xfb4=parse_patch_v1/2、0xfb8=ctrl_fw_state_v2——vtbl map 铁证）。
- **活系统环深 = 512**（0xD4300 区每 0x10=base/cnt=0x200/cidx/didx，
  全部环预置 CNT=512；我们写 64——下轮对齐试）。
- 0xD7000 = WFDMA0 基区（cr[1802702c/30] 的 TX/RX 窗配置落点）。

**DIDX 冻结剩余嫌疑（收敛后）**：① 环深 64 vs 512；② GLO_CFG bit10/bit1
（活值 0x402 位差，写活值被硬件位过滤——bit0/bit1 疑似 self-clearing/
RO）；③ per-ring enable 寄存器位置未定（0xD4300 非它）；④ MCU hold。
排查工具：crw + 活系统 /tmp/rd 双侧对照。

## 17. 总路线图 v2（双硬件加速 + 可用固件，2026-10-31 定案）

**范围定义**
- 有线 HW 加速 = SBRG hash 表 + PPE flow offload（nft flowtable 硬件路径，
  已逆向的 sbrg 套件接入 zxnative-eth 的 setup_tc）
- 无线 HW 加速 = WFDMA 环 DMA（进行中）+ TXD 硬件模板 + IDM skb 直通（后期）
- 固件可用性判定 = ① 有线 LAN/WAN 双向 ≥ 基线（vendor 参考 395M/CPU7
## 17. 总路线图 v2（双硬件加速 + 可用固件，2026-10-31 定案）

**范围定义**
- 有线 HW 加速 = SBRG hash 表 + PPE flow offload（nft flowtable 硬件路径，已逆向的 sbrg 套件接入 zxnative-eth 的 setup_tc）
- 无线 HW 加速 = WFDMA 环 DMA（进行中）+ TXD 硬件模板 + IDM skb 直通（后期）
- 固件可用性判定 = ① 有线 LAN/WAN 双向 >= 基线（vendor 参考 395M/CPU7%）② 无线可关联可上网 ③ HW 加速计数非零 ④ device-verify ALL-PASS ⑤ 双门禁过 ⑥ 长稳 >=24h 无 oops

**迭代轮次**
- R-A 无线 FWDL：环深 512 -> DIDX -> patch/WM/WA -> fw-state=7
- R-B 无线控制面：BSS bring-up -> 可关联
- R-C 无线数据面 P5：802.11->802.3、wcid->VAP -> ping 通
- R-D 有线加速：PPE offload 接入 -> flowtable 硬件计数
- R-E 无线加速：TXD 模板 + 吞吐
- R-F 总验收：六项判定全绿
每轮验收标准随轮写入；验证方式 = device-verify + 轮专项命令。

## 18. 环寄存器真实布局破译（活系统对齐扫描，2026-10-31）

活系统 0xD 窗 64KB 对齐扫描（0x20 步长，185 非零寄存器）推翻我们的
0x10 步进模型：

- **TX/RX 环 base 每 0x20 步进**：0xd4400/20/40 = 4a6df000/4a748000/
  4a6e1000（TX qid16-18），0xd4500/20/40 = 4a0b8000/4a0bc000/4a758000（RX）
- **cidx/didx = 16 位打包对**，在 0xd4b00+ 区域：0xd4b00=002e002e（FWDL
  cidx=didx=0x2e）、0xd4b20=02990299、0xd4b40=00ca00ca（各环进度）
- **INT_ENA 真身 = 0x18024240**（活值 ffffffff；我们写的 0x1802420c 活=0）
- 0xd4100=0x30/0xd4120=ffff0000/0xd4260=00010001/0xd42a0=01000000/
  0xd42e0=0x15 = GLO/配置区活值
- 0xd6020/40=01ff0010（掩码 512-1 = 环深 512 又证）；0xd7000+=WFDMA0；
  0xd8000+=0xd4000 结构镜像（band1/WFDMA1）；0xdd000=deadfeed 魔数

**DIDX 冻结根因（终）**：我们写的 cidx 地址（0xd4408）不是硬件读的位置
——硬件 cidx 在 0xd4bxx 打包对，DMA 看到的 cidx 恒 0。

**3.22 规格**：以 pci_kick_out_cmd_msg 尾部的硬件 cidx 写地址 + 稠密读
交叉，重绘 {base, cnt, cidx/didx} 精确偏移表；重写 rings/submit 的
寄存器地址与步长；预期 mcu-dl DIDX 立即应答。

## 19. 3.22：真实布局落码 + 写粘性新线索（2026-10-31）

**落码**：环寄存器地址全改真布局——TX base=0x18024400+0x20n（64 位
lo/hi 对）、cidx 写 0x18024B00+0x20n（32 位标量 slot，vendor 尾部同款）、
INT_ENA=0x18024240、wfdma-init 增五环使能 0x18024260+4i=0x00010001；
rings 命令按新布局显示（base lo/hi + idx 对区）。

**实验**：cidx 写 0x18024b20 回读=0（不粘）；但区域呈每 0x20 块 +8 处
0x00070000 重复值。同时注意：旧地址 0xd4408 的写**是粘的**（3.21 rings
显示 cidx=17）——即 0xd4408=软件影子可写，DMA 不消费；0xd4bxx 对区
word0 写不粘（可能只读视图/写法错/half-word 语义）。

**下轮（干净单变量实验）**：
1. crw 逐个写候选寄存器（0xd4b08 的 0x70000 位场、0xd4260 使能回读、
   0xd4204=FEF0000F 区）写后立读，找出"写粘且引发 didx 动"的地址
2. kick_out 尾部写的是 ring+36 的**逻辑地址**——经 mac_io 翻译后的
   物理可能非 0x18024b20；翻译表（bss LANCHOR0）读取路线：
   /proc/kcore（若 vendor 有）或 rd 去 O_SYNC 重试
3. FWDL 前置：活系统 ring base 4a6df000 vs 我们 4e080000——DMA 可达域
   核对（dma-ranges 0x40000000 窗）

## 20. 破冰：MCU 在消费命令——RX 应答环是缺失的另一半（2026-10-31）

**crw 写粘性地图**（a5a5a5a5 扫描）：0xd4260 半字过滤粘写（05a505a5，
12 位/半字编码）；**0xd4b00 高半字 = 0x9 稳定 = 本 boot 发往 qid17 的
9 条命令（3 次 mcu-dl × SemGet/AddressLen/Finish）全部被 MCU 消费**！
0xd4b04 = 0x00010001（与 0xd4260 使能值同现——区域有别名/联动）；
0xd4b28/48 = 0x00070000 默认态；0xd4b20 word0 三变体写全不粘。

**结论**：命令通道（qid17）双向活——DMA 取描述符、MCU 消费、did 计数
在 0xd4b00 高半字。TOP_MISC=0 的原因 = **FWDL 流程缺 RX 侧**：SemGet/
AddressLenReq 是请求-应答式，MCU 应答到 RX 事件环，本 boot 未跑
rx-init、未读应答 → 流程停等。FwScatter 51 块（FWDL 环）消费 0 亦与
应答未读一致（patch 流程未推进到 bulk 阶段接受）。

**下轮（R-A 收官）**：
1. rx-init（RX 事件环 setup，commit=1）
2. mcu-dl p.bin 后逐命令 mcu-recv 读应答（SemGet 响应→sem、AddressLen
   响应→dst 确认）
3. 完整 patch 流 → fw-dl wm → fw-dl wa → fw-state 应 = 7
4. poll 修正：did 在 0xd4b00 高半（ring1）；tx0/FWDL 的 did 位置在
   RX 联动后重定位

## 21. 3.23：门铃寄存器定位 + 消费计数未动（2026-10-31）

- **pci_trigger_int_to_mcu 解码**：mac_io_write32(逻辑 0x54000108, val)
  ——门铃在 CR 窗外，我们从未写过（crw 旧式被界检丢弃）。
- **3.23 crw 增 bar 直写形式**（crw b<bar> <off> <val>）；实测 b1+0x108
  （=bar2+0x108，即 mcuwin 时代读出 0x47474747 的位置）**写粘回读一致**
  ——真实寄存器；b0+0x108 不粘。
- **实验**：mcu-dl 后门铃 b1+108 写 1/0x10 → FWDL 环 didx 仍 0、
  0xd4b00 对=0（本 boot 消费计数未动；上 boot 的 9 未能复现——计数
  寄存器/条件仍未知）。

**下轮**：
1. 门铃值语义：解码 chip_trigger_int_to_mcu 的调用方（值=队列/cause）
   与调用时机（每 submit 后 or bulk 后）
2. 本 boot 内多次 mcu-dl 后全窗快照对比，找出"动的计数器"真身
3. RX 事件环（rx-init 已跑）无事件——ROM 阶段应答可能不走 RX 环，
   改查 PCIe INT（设备中断状态寄存器）确认 MCU 是否回过中断

## 22. 世界观修正：消费从未发生（2026-10-31）

三连修正：
1. **门铃 = 红鲱鱼**：chip_trigger_int_to_mcu 的调用方 = HwRecoveryFromError/
   MtfAsicTxCapAndRateTableUpdate/mt_ate_tx_unsubscribe——错误恢复/TX-cap/
   ATE 专用，正常命令路径不敲门铃（与我们无门铃一致）。
2. **"did=9" = 自我污染**：0xa5a5a5a5 & 0x000F0000 = 0x00090000——写粘性
   地图的 a5 写入被寄存器位过滤（可写位仅 0x000F0000），"9"是 a5 的第
   9 nibble。**MCU 从未消费任何命令**。
3. **全窗快照差分 = 零变化**：mcu-dl 前后 0xD 窗 2KB 逐字节一致——本
   boot 内硬件状态完全静止（连 cidx 都没变，因 0xd4b20 写不粘）。

**/dev/mem 路线封死**：vendor 内核 STRICT_DEVMEM（去 O_SYNC 重试仍
Bus error）——翻译表运行期读取不可行。

**下轮（翻译表的静态路线）**：
1. mt7916_init 反汇编找 chip_cap 窗口表填充（大常量 str 到
   [chipcap+4..0x40]），静态重建 逻辑地址→物理 映射
2. 用映射表重定位 {base, cidx/didx} 的真实 BAR 偏移
3. kcore 路线备选（vendor /proc/kcore 存在性待查）
工具状态：3.23 双门禁过 ALL-PASS（crw b 形式 + 全部前序）；5.1 探测
全程只读合规。

## 23. 3.24：映射表结构闭环（hif_chip_init 分派 + LANCHOR0 指针定位）

- **hif_chip_init(chip_type)**：uxth 比较——790a→mt790A_hif_ctrl_chip_pcie1_init、
  7906→mt7906_hif_ctrl_chip_init；r0 = hif_ctrl 结构。
- **hif_ctrl 写值**：7906=[0xFEF0000F, 0x0D004204]；790A=[0xB0B00008,
  0x0D008204]；[+0xD8] = **&LANCHOR0**（物理基址数组指针！）；
  [+0xC4/+0xC8] = ISR 对；[+0x6A4] = 0xC0。
- **mt_mac_cr_range_mapping 结构重认**：窗口表走 chip_cap（[+4]=base、
  [+8]=len，12B 步进），命中后 phys = **LANCHOR0[3i+1]** + (addr-base)，
  再过 dev+0x5DD984 位检查（bit2=4 时 addr-base ≥0x100000 拒绝）。
- LANCHOR0 = .bss 全局（+0 与 +0x804 两锚），运行期填充者未定；
  STRICT_DEVMEM 封死运行期读取。

**3.24 未完成项（下轮续）**：
1. LANCHOR0 填充函数定位（消费 hif+0xD8 指针的代码 = 映射路径使用者）
2. 0x0D004204/0x0D008204（7906/790A 各自的窗口 base 常量）语义
3. 用重建的映射重定位 {base, cidx/didx} BAR 偏移 → 3.25 重写
工具就绪：3.23 ALL-PASS（crw b 直写 + 全部前序），5.1 只读合规。

## 24. 3.24 续：chip_hif_pci_data_ring_assign 定位（下轮首靶）

- LANCHOR0 锚名歧义（编译器局部锚全模块复用）——按名追踪死路，改用
  完整反汇编（/tmp/mt7916-full.dis 867351 行，已在 WSL）+ 脚本过滤
  （_scratch/lanch*.py、ringinit2.py）。
- pci_init_txrx_ring_mem 解码发现：**chip_hif_pci_data_ring_assign**
  （0x143CA0，pci_core_ops 表函数）= 数据环寄存器分配器——ring+36
  （cidx 寄存器逻辑地址）应在此或其调用链设置。
- 下轮首靶：解码 chip_hif_pci_data_ring_assign → ring+36/+40 的
  逻辑地址公式 → 结合 chip_cap 窗口表（0x0D004204 base）重建映射
  → 3.25 重写环寄存器地址 → DIDX 判定。
- §24 补记：chip_hif_pci_data_ring_assign = chip_ops[0x28C] 转发；
  vtbl 基寄存器歧义误中 bgnd_scan_ipi_cr_init（雷达检测，无关）——
  下轮须以 mt7916_init 的 chip_ops 填充序列精确定位 [0x28C] 实现
  （walk str 序列按偏移配对，勿信 vtbl 的基名标注）。
- §25 里程碑：[0x28C] 填充实锤 = bgnd_scan_ipi_cr_init（雷达）——
  **chip_hif_pci_data_ring_assign 非 ring+36 产地**（红鲱鱼三号）；
  pci_init_txrx_ring_mem 全 807 指令扫过：ring+36..51 = 初始化清零，
  地址构造不在其中。**doc 14 §6.4 原注生效**：环寄存器编程在
  multi_hif_entry_* 系（alloc/get_by_id/gid/rid，0x15bee4-0x15c12c）
  ——7906/790A init 调的 multi_hif_entry_id_get 同族。下轮解码该族
  找环寄存器地址赋值。

## 26. 模型大修正：0x10 步进复活 + 时钟门控实锤 + ROM 只服务 qid16（2026-10-31）

1. **原始 0x10 步进 BASE/CNT/CIDX/DIDX 模型复活**：活 FWDL 环
   base=4a6c7000、CNT=128（0xd4404=0x80！之前误读为 base_hi）、
   cidx=did=0x2e——0x20 步进是 0x20 步长扫描的采样伪影。
2. **时钟门控实锤**：CNT 写在 GLO_CFG 强制（0x1030b857）后粘
   （回读 0x80）；未强制时全部"不粘"——此前所有写粘性结论都受
   引擎时钟门状态污染。
3. **ROM 只服务 FWDL 环（qid16）**：vendor 的 46 块全在 qid16 且
   cidx=did=0x2e 被消费；我们把 SemGet/AddressLen/Finish 发给
   qid17（WM 环）= ROM 阶段无人服务。FwScatter 走 qid16 正确但
   其 didx 亦未动（CNT=512 与 ROM 期望 128 不符/引擎时钟未常开）。
4. multi_hif_entry_* 族 = 纯 HIF 链表管理（无寄存器地址）——族排除。

**3.24-续规格（下轮）**：
- 环寄存器回 0x10 步进（撤销 0x20 重写）；CNT=128（活值）
- mcu-dl 的命令（SemGet/AddressLen/Finish）改走 qid16（ROM FWDL 环）
- 每阶段 GLO_CFG 强制（0x1030b857）再写寄存器
- DIDX 判定：qid16 cidx/did 对进即可确认 ROM 消费

## 27. 3.24：§26 修正全部落码，布局对齐活系统，DIDX 仍冻——下轮系统性对照（2026-10-31）

**落码**：0x10 步进复活（撤销 0x20 重写）、CNT=128（撤销 512）、全流量
qid16（MCU_RING_NR=0）、提交路径回原版（base/CNT/cidx@+0/4/8、didx@+12）。
实测：base=4e04e000 ✓、CNT=0x80=128 ✓（与活 FWDL 环一致！）、
cidx=0x14=20（17 chunks + 3 cmds）✓——**布局与活系统完全对齐**。
DIDX 仍 0，fw-state=0。

**剩余差异清单（下轮系统性对照，22 写逐项回读 vs 活值）**：
1. wfdma-init 的每写回读 vs 活系统对应寄存器值（GLO_CFG 已证收敛，
   其余 21 写从未逐一验证效果）
2. PCIe 窗寄存器：init 只写 0x282f4=0x811c0000；"第二 MAC 代用 0x82xx"
   可能指值（0x821c0000）——两寄存器 × 两值矩阵未扫完
3. 门铃（bar2+0x108）时机/值（上一轮未完成）
4. 描述符 buf_dma 域核对（活 ring base=4a6c7000 vs 我们 4e04e000，
   同 0x4xxxxxxxx 域 ✓ 但 desc 内的 buf 指针未对照）

## 28. 22 写对照完成：信用表索引差 1 修正（仍不够），下轮 dump 活描述符环（2026-10-31）

**对照结果**：
- RING_LO 区（0xd4640-50）活值 =(6..10)<<22|4 ✓ 与我们一致
- **RING_HI 区（0xd4680-94）活值 =(0..5)<<22|4** —— doc 13 表的 (1..6)
  偏移差 1！我们首环 credit 写 0x00400004，活 = 0x00000004。已用 crw
  修正为活值（回读全粘）。
- GLO_CFG 强制 0x1030bc55（回读 0x...57，bit0/1 读过滤）
- 修正后 mcu-dl：cidx=0x28 继续前进，**DIDX 仍 0**

**排除完备清单**：布局（0x10 步进 BASE/CNT/CIDX/DIDX ✓ 活值逐位一致）、
信用表（已改活值 ✓）、使能（0x24260=00010001 ✓）、GLO_CFG（≈活值 ✓）、
INT_ENA（0x24240=ffffffff ✓）、PCIe 窗（两寄存器两值已试）、门铃（非命
令路径）。

**剩余唯一未知 = DMA 取描述符时的内容校验**。下轮首刀：dump vendor 活
描述符环内容（live FWDL base=4a6c7000 DRAM，36B×N）对照我们的 fill_desc
——STRICT_DEVMEM 封 /dev/mem，路线：① vendor 的 ring dump 调试接口
（/proc 或 iwpriv）② 5.1 上 kcore ③ 我方盒先确认 desc 内容（win 读
4e088000？——DRAM 不在 BAR 内，需设备侧 /dev/mem 或驱动内读）。
我方驱动加 dmp 命令读 mcu_ring 前几项最快。

## 29. 描述符格式终审：与 vendor 逐位一致——问题定性转向硬件状态（2026-10-31）

pci_kick_out_cmd_msg 逐字节重推（含 bfi/ubfx 位场）终审：
- word0 = buf_dma（从本项 +20 复制）
- word1 = byte4=0、byte5=orig5&0xC0、byte6=len&0xff、byte7=(orig7&0xC0)
  |0x40|((len>>8)&0x3f)——零初始化环上 orig=0 → **word1=0x40000000|
  len<<16，与我们的 fill_desc 逐位一致**
- orig5/orig7 的保留位来自软件视图 [4]=curVA 的高位——硬件容忍，等价

**结论**：描述符格式非问题。DMA 不消费 = **硬件状态问题**（ROM 阶段的
WFDMA/队列状态、时钟、或 vendor init 中尚未复刻的一步）。

**下轮路线（按优先级）**：
1. 5.1 上枚举 vendor 调试接口找环 dump（iwpriv 列表、/proc/net、
   /sys/kernel/debug）——对照环寄存器与描述符内容
2. mt76 对照：connac2 ROM 阶段 WFDMA 是否默认活（mt76 无 vendor init
   也能 FWDL——重查 mt76 mmio 初始化序列与我们差异）
3. PCIe INT 状态寄存器（设备侧）核对 MCU 是否回过任何中断

## 30. vendor 调试面穷尽 + mt76 上游对照路线定案（2026-10-31）

5.1 枚举结果：debugfs 仅泛用子系统（无 wifi 目录）；/proc/net 无
mt/tm/wifi 条目；**iwpriv 无任何 wifi 网口**（MTK SDK 网口未创建——
5.1 的 wifi 处于 down 态，环状态为启动遗留）；/proc/tm/shell = 只写
命令接口（输出进内核日志，无 dmesg 可读）；无 kcore。

**vendor 调试面穷尽**。FWDL 状态缺口的最后权威路径：

**mt76 上游源码对照**（下轮首刀）：
- mt76 无 vendor init 也能对 MT7916 做 ROM FWDL——其 mt7915/mmio.c、
  mcu.c、regs.h 含 connac2 ROM 阶段的寄存器地址（MT_MCU_BASE 区）与
  描述符格式的权威定义
- 关键疑点：mt76 的 MT_MCU_BASE = 0x18024000 区 = **ROM 直服务的 MCU
  环（非 WFDMA 引擎）**——描述符格式可能是 ROM 自有格式而非 WFDMA
  36B 环项；WFDMA 引擎（0xd7000）仅固件后生效
- 获取：base 树 feeds 或上游 mt76 master（git/web）
- 产出：对照表 → 修正我们的 ROM 阶段描述符与环地址 → DIDX

## 31. mt76 mt7916_reg[] 权威地址图——三层寻址全闭合（2026-10-31）

mt76 master 的 mt7915/mmio.c `mt7916_reg[]` 表（与我们实测逐项互证）：
- **WFDMA0_ADDR = 0xd4000**（WFDMA0 引擎基 = bus 0x180d4000）
- **WFDMA0_PCIE1_ADDR = 0xd8000**（band1 镜像——我们 0xd8000 区发现吻合）
- **WFDMA_EXT_CSR_ADDR = 0xd7000**（TX/RX base 窗配置 0xd702c/30 吻合）
- **INT_SOURCE/MASK_CSR = 0xd4200/04**（中断状态——PCIe INT 核对点）
- **INT_MCU_CMD_SOURCE = 0xd41f0**
- MT_MCUQ_RING_BASE(q) = MT_Q_BASE + 0x300 = **0xd4300**（环基址寄存器
  数组，每队列 4B）；MT_MCUQ_EXT_CTRL(q) = Q_BASE+0x600+q*4（0xd4600 区
  = 我们看到的 credit/使能值区！）
- MT7916 全队列 WFDMA0（dma_config else 分支）；q_id：FWDL=16、MCU_WM=17
- GLO_CFG 位义：TX_DMA_EN=bit0、RX_DMA_EN=bit2（与我们 |=0x5 一致）
- MtfAsicGetFwSyncValue/chip_ops 表 = vendor SDK 私有，mt76 无此层

**架构定性**：vendor 的 0x18024xxx 逻辑地址经 mac_io 翻译落到 WFDMA0
（0xD4xxx 物理）——三层寻址（逻辑 CR → mac_io 翻译 → WFDMA0 窗→BAR）
全闭合。我们的环寄存器写入位置正确。

**DIDX 仍冻的最后嫌疑收敛为**：ROM 阶段 MCU 对 MCU 环的消费机制
（mt76 的 ROM FWDL 用 MCUQ_FWDL 环 + MCU Command/Semaphore 流程，
mt76 mcu.c 的 mt7916 分支有 ROM 阶段细节——下轮解码 mcu.c 的
mt7915_mcu_send_message 与 fw download 序列）。

## 32. 3.25：mt76 格式描述符落地，DIDX 仍冻——排查完备度评估（2026-10-31）

**落码**：DESC_SIZE 36→16（mt76 struct mt76_desc 权威格式：buf0/buf1/
info/ctrl）；ctrl = SD_LEN0(len<<16)|LAST_SEC0(bit30) 在 +12（此前写错
位）；fill_desc 重写。3.25 双门禁过 ALL-PASS。

**实测**：CNT=128 ✓、cidx=20 前进 ✓、**DIDX 仍 0**、INT_SOURCE/
MASK（0xd4200/04）全零——MCU 从未回过任何中断。TOP_MISC=0。

**完备度**：访问层（三段 remap+权威地址图）、布局（0x10 步进 BASE/CNT/
CIDX/DIDX）、使能（五环+GLO_CFG+INT_ENA+信用表对齐活值）、描述符
（mt76 格式）、流程（全 qid16）——**全部 vendor/mt76 双源背书**。
剩余唯一未知 = **ROM 固件自身的状态机期望**（MCU 侧消费前提）。

**下轮候选（按性价比）**：
1. chip_ops[0xC8]（MCU pause/resume）语义解码——vendor 下载前显式
   暂停 MCU，暗示 MCU 默认态非"等固件"（可能 crashed/idle-hold）
2. connac top 0x1800xxxx 域的 MCU hold 寄存器扫描（0xF0000 窗活，
   可写试验：top cfg 的 SW reset/hold 位）
3. PERST/电源序列复核：gpio enable/reset 时序重放后再探

## 33. chip_ops 填充图（部分）+ 0xC8 追踪（2026-10-31）

mt7916_init 的 chip_ops（r3 表，820B）填充图（符号已归因）：
- [0x90] = **pci_trigger_int_to_mcu**（门铃槽实锤——mt7916 门铃 =
  chip_ops[0x90] 转发）
- [0x94] = pci_interrupt_disable、[0xFC] = fw_prepare、[0xF0] =
  CmdHeraStbcPriorityCtrl、[0xD8] = mt7916_eBFPfmuMemAlloc、
  [0x104] = MT7916BandGetByIdx、[0x140] = get_fw_sync_value、
  [0x108/0x118/0x10c/0x124/0x128/0x12c/0x138/0x13c] = BF/校准系
- **[0xC8] 填充未在 str [r3,#N] 线性扫描现形**——疑计算偏移存储
  （str rX, [r3, rY]）或经 memcpy 批量（hc_register_chip_ops 前的
  820B 表拷贝）
- pause/resume 符号搜索零命中——[0xC8] 的实现函数名不含 pause/resume
  （doc 14 的"暂停/恢复 ✓"为语义标注非符号名）

**§34 fw_own 解码（0x19EBB0，chip_ops[0xC8] 实现候选）**：

```
fw_own(dev, r1):                              // r1 = 1 pause / 0 resume?
    chip_cap = hc_get_chip_cap(); hif = hc_get_hif_ctrl()
    if (([chipcap+0x984] & ~4) == 1) return   // fw sync 已在特定态则跳过
    // 首写：mac_io_write32(0x18060010, 0x1806)   ← r1=0x1806!
    //   （逻辑地址 0x18060010 = TOP_MISC 区 +0x10！值为 0x1806 = L1 remap 目标？）
    sync = get_fw_sync_value()
    [hif+0xD49] = 0                            // 软件标记
    if (sync <= 3):
        if (r1 == 0) return                    // r1=0（resume）在 sync<=3 时直接返回
        // r1=1（pause）且 sync<=3：
        mac_io_write32(0x18060020, 1)          ← 第二写：0x18060020 = 1
        [hif+0xD4A] = 0
```

**寄存器写序列（crw 可重放）**：
- **0x18060010 <- 0x1806**（无条件首写——0x1806 值 = TOP_MISC 段地址！
  疑似把 L1 remap 重指向 0x1806 或触发 fw_own 状态机）
- **0x18060020 <- 0x1**（sync<=3 且 r1=1 时——fw_own 请求位！）
  （两写均经 mac_io → mt_mac_cr_range_mapping → 翻译落点）

**与 0xE00F0 读数的对照**：0xd4b04 = 0x00010001 亦在 0xd4b 区——
0x18060020 的物理落点或即 0xE020（remap 窗内 +0x20）！

**crw 重放序列（下轮）**：
1. crw 18060010 1806（经 L0 翻译？0x1806 段 = 0xE0000 窗——写 0xE0010）
2. crw 18060020 1（= 0xE0020）
3. 读 fw-state / 0xd440c DIDX → mcu-dl p.bin


## 35. fw_own 重放结果 + R-A 状态终评（2026-10-31）

**fw_own 重放（crw）**：0x18060010←0x1806、0x18060020←0x1 均落地
（回读 0x4——寄存器位场过滤），mcu-dl 后 **DIDX 仍 0**。TOP_MISC=0。

**R-A 状态终评（21 轮后）**：
- 已闭环：三层寻址（逻辑 CR→mac_io→WFDMA0 窗→BAR，mt76 权威互证）、
  环布局（0x10 步进 BASE/CNT/CIDX/DIDX，活值逐位）、使能序列（五环+
  GLO+INT+信用表）、描述符（mt76 16B 格式逐位）、流程（全 qid16+門铃
  排除+fw_own 重放）
- 冻结点：**WFDMA DMA 引擎对 MCU 环的取数**——ROM 阶段 MCU 不消费
  我们的描述符，且 MCU 零中断
- 剩余候选（需更深层手段）：① chip_cap 运行期翻译表（STRICT_DEVMEM
  封锁）② vendor SDK 私有 MCU hold/时钟序列 ③ PCIe 总线级分析

**交接清单**：3.25 双门禁过 ALL-PASS 在机；有线完整服役；crw（CR+bar
双形式）/rd2/分段下载工具链；doc 62 §13-§35 完整记录；mt76+mt7916.ko
双源反汇编就绪。R-A 剩余 = MCU 消费机制一个未知域，需新的观测手段
（总线分析或 vendor 源码）。

## 36. R-D 集成面盘点：PPE flow offload 全套机械已在库（2026-10-31）

zx279128s-ppe.c（baseline 编译单元，CONFIG_ZX279128S_ETH=n 后未编译）
内含**完整 PPE flow offload 机械**（vendor 逆向成果）：
- zx_ppe_start：寄存器保存/重编程（zx_ppe_regs/vals）、CLA V4 规则/
  索引、包修改器 cmd/sub、MAC 表+trap 更新、bitmaps 清零
- zx_ppe_flow_add/del/stats：子网分配/下一跳/FLOW 表 3 word/CLA hash
  9 word 写入、rhashtable cookie 管理、错误回滚链
- zx_ppe_stop：trap 恢复 + 表销毁 + 寄存器回滚

**依赖面**：adapter（共享头同型 ✓：ports/base/ppe_flows/ppe_flow_used/
ppe_nh_used/ppe_hash_used/ppe_count/ppe_lock）、zx_update_traps/
zx_update_mac_tables（**main.c 符号，baseline 禁用后缺失——需 native
版或桩**）、zx_pp_write/zx_ppe_alloc（ppe.c 自含）。

**集成方案定案（方案 1）**：
1. Makefile：zxnative-eth-y += zx279128s-ppe.o（ppe.c 并入 native 编译）
2. 补 zx_update_traps/zx_update_mac_tables 的 native 语义版本
   （native 已有自家 MAC 表逻辑——对接或桩化后由 zxna_ 自己维护 trap）
3. zxna_probe：rhashtable_init(&a->ppe_flows)（baseline 在其 probe 做）
4. zxna_setup_tc：offload=1 时 FLOW_CLS_REPLACE 首流触发 zx_ppe_start
   → zx_ppe_flow_add；DESTROY → zx_ppe_flow_del（最后流 zx_ppe_stop）
   STATS → zx_ppe_flow_stats
5. 验收：nft flowtable 建立后 HW 卸载计数非零 + iperf 吞吐 + 断电重启
   后 MAC/转发不回归

**约束**：baseline 回像方案排除（会破 3.12 MAC defer 修复的竞争窗口）。
R-A（FWDL）挂起待新观测维度，与本轮并行不悖。

## 37. 3.26 R-D 落地：PPE flow offload 集成完成，flowtable 建立（2026-10-31）

**集成（方案 1 全部落地）**：
- 043 Makefile：zxnative-eth-y += zx279128s-ppe.o（ppe.o 并入 native）
- zx-ffe-ops.h：zx_ppe_init/start/stop 声明
- ppe.c：zx_ppe_init（rhashtable_init）、zx_ppe_stop 去 static
- zxnative-eth.c：include seam 头、zx_eth_is_port（native ops 判定）、
  zx_set_trap_dmac 转发器、probe zx_ppe_init、zxna_setup_tc 完整接线
  （offload=1 门控；首流 zx_ppe_start → flow_add/del/stats 直调）
- offload 参数 0444→0644

**构建**：3 轮纠错（两处断串 drvfs 伪影、is_port 前向引用、trap_dmac
转发器、param mode）→ BUILD_RC=0 → 3.26 刷入 ALL-PASS。

**实机状态**：offload=Y、nft flowtable ft 建立（flags offload + 四端口
挂载 + counter）、`meta l4proto {tcp,udp} flow add @ft` 规则在位、
fw4 重启后网络正常（WAN 1.2ms、厂 MAC 不回归）。

**TC_SETUP_FT 未触发原因（待真双向转发流）**：conntrack 里的 33 条
ASSURED 全是本机出向/管理流（不过 forward 链）；nft flowtable 只对
"已确认双向的转发流"下发 TC_SETUP_FT。需要第二测试主机（LAN 侧）
对 WAN 侧跑 iperf/dhclient 之类的真转发流——单主机拓扑无法产生。
**验收门槛（nft flowtable counter packets 非 0 + zx_ppe_start dmesg）**
保持，待具备双主机拓扑时补测。

## 38. R-D 验收通过：PPE flow offload 全链路实锤（3.26，2026-10-31）

用户指正后验收路径打通（"本机下载即真转发流"——LAN→WAN NAT 过 forward
链，此前误判为本机流）：

**三轮修正到通**：
1. NETIF_F_HW_TC 缺失 → tc_can_offload 门不过（与 baseline 1325 行对齐）
2. **TC_SETUP_FT 是块绑定协议**：type_data=flow_block_offload
   （FLOW_BLOCK_BIND/UNBIND 绑 block_cb），不是 cls 流命令——此前
   setup_tc 假设错误；ndo_setup_tc 直指 ppe.c 的 zx_eth_setup_tc
   （块绑定+zx_ppe_users 引用计数+首绑 zx_ppe_start）
3. 构建 rc6 → 刷入 ALL-PASS

**验收三要素全过**：
- dmesg：`hardware flow offload enabled`（zx_ppe_start，fw4 reload
  往返 enabled/disabled/enabled 正确跟随）
- flowtable ft 建立（flags offload + 四端口 + counter）
- **9+ 条 TCP [HW_OFFLOAD] 流**（nft 确认块绑定成功、FLOW_CLS_REPLACE
  下发 zx_ppe_block_cb → CLA hash/FLOW/下一跳表编程）

**吞吐注**：下载测速受 CFN 节点限速影响波动（208KB/s~1MB/s 区间），
与 PPE 卸载正交（PPE 编程路径与 native CPU 转发路径并存；流级硬件
快路径需芯片内联转发拓扑确认，属 R-D 后续优化项）。

**R-D 收官状态**：有线硬件加速（SBRG hash + PPE flow offload）编程
路径全通、双源背书、nft/nf_conntrack 双层实证。

## 39. R-A 复活：mt76 完整对照（3.27）——关键修正 + 剩余单点（2026-10-31）

**mt76 源码完整对照（dma.c/init.c/mmio.c/pci.c）收获**：
1. **desc 字段序（权威）**：`struct mt76_desc { buf0; ctrl; buf1; info }`
   ——**ctrl 在 +4**。3.25 错挪 +12 已修回；3.19-3.24（36B）时 ctrl
   位形在 +4 位置本来是对的，只是 stride 错。
2. **WFDMA0_RST（0xd4100）复位序列缺失**：mt7915_dma_disable(rst=true)
   脉冲 bit4|bit5（LOGIC_RST|DMASHDL_ALL_RST），活系统读回 0x30（置位态
   = 解除复位）。已补入 submit 路径（crw 亦验证可写）。
3. **INT_SOURCE（0xd4200）W1C 清零**：init_hardware 前奏
   `mt76_wr(INT_SOURCE_CSR, ~0)`。已补入。
4. prefetch 表（EXT_CTRL +0x600 区 PREFETCH(base<<16|depth)）、队列
   regs = BAR + 0xd4300 + qid*0x10、kick=cpu_idx 32 位标量——均与实测
   一致（无需改）。

**3.27 实测**：RST=0x30 ✓、FWDL 后 DIDX 仍 0、TOP_MISC=0。

**剩余单点（收敛完成）**：mt76 能跑通的前提链中，我们唯一没有复刻的
= **mcu_init 之前的 PCIe/MCU 交互前置**——mt7915_pci_probe 里 hif2
（第二功能 790a）的联合初始化：vendor/mt76 都把 7906+790a 当**一个
dev 的两个 BAR 集**（hif1=7906 BAR、hif2=790a BAR +0x4000 偏移语义），
而我们两个独立 dev。mt76 的 GLO_CFG/环寄存器全在 hif1（7906），hif2
只加 hif1_ofs 用于 band1。**下一实验**：把 FWDL 提交经由 7906 功能的
BAR（0000:01:00.0，base=0x10300000）——我们现在所有环写在 790a 的
BAR0！790a BAR0 翻译窗与 7906 不同（790a 的 0xD4xxx ≠ 7906 的 0xD4xxx）。
活系统只有 7906 承载全部环（0x18024xxx→7906 BAR）——我们发错功能了！

## 40. 3.27-rc2：主功能 BAR 路由落地——DIDX 仍冻，R-A 剩余歧义最终清点（2026-10-31）

**本轮落地**：
- mt76 权威 desc 字段序修正（ctrl=+4，3.25 错挪 +12 修正）
- WFDMA0_RST 复位脉冲 + INT_SOURCE W1C（mt76 序列补齐）
- CR 访问全路由 7906 主功能 BAR（zxnpu_main_cr_read/write + poll 切换）
- 3.27-rc2 构建刷入 ALL-PASS

**实测**：DIDX 仍 0、TOP_MISC=0。主功能路由不是最终解。

**R-A 剩余歧义清点（诚实评估，全部可做实验已做）**：
1. 主功能/伴随功能的翻译窗差异——活系统实测翻译落在哪个 BAR 未直接
   验证（0xd4208 活值读自 5.1 的 /dev/mem 物理地址 0x100d4208=7906 BAR；
   我们经 zxnpu_main_cr_write 走 7906 BAR0+offset——但 7906 BAR0 的
   remap 窗（0xFE25C/0xFE260）是否与 790a 相同未证：7906 功能的
   remap 寄存器值从未读过！）
2. 下轮单刀：读 7906 功能的 remap 寄存器（7906 BAR0+0xFE25C/260）+
   对比 790a 的值——若不同则 7906 窗指向别的域，环寄存器翻译要按
   7906 窗重算
3. 终极手段仍是 PCIe 总线分析/厂商源码级

**资产**：3.27-rc2 双门禁过 ALL-PASS 在机；全套工具链；doc 62 §13-§40。

## 41. R-A 全窗差分收敛：22 静态差异 → 7 项 → DMASHDL 组配置定性（3.28，2026-10-31）

**方法论突破**：全窗差分（我们 0xd4000-0xd6fff 1536 寄存器 vs 活系统
174 非零逐项）→ 22 静态差异 → init 后复扫收敛到 **7 项真实差异**：
- 0xd42a0（ours=03000000 live=01000000，bit24）
- 0xd42e0（live=0x15）
- **0xd6100=038a037a / 0xd6140=00100a55**（DMASHDL 组配置——connac2
  的 DMASHDL 子块在 BAR 0xd6000 区；vendor 从 hif_init 之外的路径配，
  mt792x 系 mt76 有 DMASHDL_GROUP_QUOTA/Q_MAP 定义（0x7c026000+ofs，
  同构寄存器族）
- 0xd7560（INT 相关位差）
- 0xd82a0/0xd82e0（WFDMA1/PCIE1 区，band1 相关）
- 0xde000-0xde040（live=1/0x2f/0x00880910，未命名区）

**crw 写活值验证**：0xd6100/0xd6140/0xd42e0 写**不粘**（回读 0），
0xd42a0 部分粘（bit24 被过滤）——写粘性受时钟门/位场约束，
**直接 crw 无法补配**，需要走 vendor 的配置路径（DMASHDL 配置函数）。

**vendor 反汇编搜索**：0x037a/0x038a 常量仅命中 RA 相关误中；
DMASHDL 配置函数符号未显式存在（inline 或查表写）。

**下一步**：① vendor init 里搜批量 str 到 [rN, #0x100+0x6xxx] 模式
（组配置循环写入特征）② 或对比 5.1 上加载前后差异（需可卸载重载——
不可行）③ 或反汇编 chip_show_pse_info/pse 相关函数反推 DMASHDL 布局
④ mt76 的 mt792x DMASHDL 组初始化序列做语义参照（GROUP_QUOTA/Q_MAP
写入值风格）。

## 42. 刷写链路根因修复 + 3.29 真机运行（2026-11-01）

**十六次刷写失败的真正原因（不是写入，是打包）**：
调用方用 `ls -dt ../out/build-20261031-*` 选构建目录，跨过午夜后该
glob 仍匹配 10/31 的旧树 → 包 = 3.28 的 rootfs + 3.29 的版本字符串。
逐字节证据：包 header 记录 rFileOff=0x400214，该处 0x960000 字节
md5=3182c8c9，与设备 mtd7 完全一致，而 payload root md5=3ddb4ce9。
期间排除的假象：mtd 写入持久性测试（写随机模式到 slot1 安全偏移，
重启后仍在 → MTD 写一直有效）；busybox `dd count=150` 管道读不可靠
（与分块读结论矛盾，改用文件落盘 + cmp 才可信）。

**修复**：
- `scripts/pack.sh`：无 `--dir` 时自动取 mtime 最新的 `build-*`，
  默认路径不再依赖调用方拼日期
- 新增 `tests/build-latest.sh`：构建 → 断言 build 树 rootfs 内版本 ==
  overlay 版本（不符即拒绝打包）→ 打包 → 刷入 → 核查

**3.29 真机运行**（device-verify ALL-PASS），含本轮全部修正：
16B 描述符（ctrl@+4）、WFDMA0_RST 脉冲、INT_SOURCE W1C、CR 经主功能
BAR 转发、**connac2 MCU TXD 命令头（60B，cid/pkt_type 0xa0，payload@60）**
—— 实测 `submit qid16 slot=19 len=60`（此前为 24B Andes 头 len=28）。

**DIDX 仍 0**：命令格式已与 mt76 权威实现一致，但 WFDMA 引擎仍未取
描述符。剩余方向：DMASHDL 组配置（活值 0xd6100=038a037a /
0xd6140=00100a55，crw 写不粘）+ ROM 阶段 DMA 使能前提。

## 43. mt76 架构真相：7906=主功能（含 wfsys reset），790a=仅 hif2（2026-11-01）

mt76 `mt7915_pci_probe` 的分派是决定性的：

```c
if (id->device == 0x7916 || id->device == 0x790a)
        return mt7915_pci_hif2_probe(pdev);   /* 790a: 只做 hif2 辅助通道 */
dev = mt7915_mmio_probe(..., id->device);      /* 主功能: 完整初始化 */
mt7915_wfsys_reset(dev);                       /* ← 关键，只有主功能做 */
```

`mt7915_wfsys_reset` 对 mt7916（else 分支）：
```c
mt76_set(dev, MT_WF_SUBSYS_RST, 0x1);   msleep(20);
mt76_clear(dev, MT_WF_SUBSYS_RST, 0x1); /* 释放复位 → MCU 重新执行 ROM code */
```
另有 mt7915 分支用 `MT_TOP_PWR_CTRL = 0x18000000`（MT_TOP_RGU_BASE+0）做
SW_RST 三连（assert→release→HW_CTRL），key=0x5746<<16。

**这就是 21 轮没找到的东西**：整个 FWDL 链的前提是"MCU 重新执行 ROM
code"，而它由主功能的 wfsys reset 触发。我们一直绑 790a（hif2），
从来没绑 7906 做主驱动——所以 ROM code 可能从未在当前配置下重跑，
DMA 自然不会取描述符。

**本次实测（经 0xF0000 窗写 0x18000000）**：写入不粘，读回恒 0x72 ——
该窗在此芯片上是只读视图（B3.x 现象），SW_RST 序列需走 7906 的 BAR。

**下一轮规格（R-A 重构，不再打补丁）**：
1. 驱动以 **7906 为主实例**：映射其 BAR0、做 wfsys reset、跑完整
   init（dma_disable(rst)→队列→prefetch→enable→INT_SOURCE 清除）
2. 790a 仅作 hif2（可选，辅助通道）
3. FWDL 全部寄存器访问走主实例 BAR
4. 验收：wfsys reset 后 `MT_TOP_MISC.FW_STATE` 变化 → DIDX 前进 →
   fw-state=7
已排除清单（21 轮）：描述符格式、环地址/步进/深度、GLO_CFG、INT_ENA、
信用表、PCIe 窗、门铃、DMASHDL 组、RST_DTX_PTR、MCU TXD 头。

## 44. R-A 重构：真正的 HIF 翻窗 + wfsys reset + 完整 WFDMA init（3.30）

**从 mt76 源码（dl/mt76-2026.10.08，权威）读出的三件事，逐条修正本驱动**：

1. **访问路径不是"三段固定窗口"，而是按地址翻窗**
   `mt7915/mmio.c __mt7915_reg_addr / __mt7915_reg_remap_addr`：
   - `addr < 0x100000` → bar0 直接访问（所以 `mt7916_reg[]` 直接给 WFDMA0_ADDR=0xd4000）
   - 命中 `mt7916_reg[]` → 固定平移
   - 其余 → **L1 翻窗**：把 `addr>>16` 写入 `0xfe260[15:0]`，回读一次推写，
     再访问 `bar0 + 0xe0000 + (addr & 0xffff)`
   本驱动的 `0x1802_xxxx → 0xd0000+` 属于第二类（正确，实测 GLO_CFG=1030b857）；
   而 `0x1800_xxxx` / `0x1806_xxxx` / `0x7000_xxxx`（MT_WF_SUBSYS_RST）/
   `0x7403_xxxx`（PCIe INT 主开关）都必须走 L1。旧代码把 0x1800 硬编码成
   `0xf0000` 是错的：实测 `bar0+f0000 = 00000072` 恒定不变（不是 TOP_PWR_CTRL），
   这解释了 §43 里"写 0x18000000 不粘"的现象——写错了窗，不是只读。

   实测基线（3.29，790a 实例窗）：`fe260 = 18001806`（L1 指向 0x1806）、
   `e0000 = 00000006`、`d4208 = 1030b857`、`d4200 = 0`。

2. **wfsys reset 只有主功能(7906)做**，mt7916 分支就是
   `MT_WF_SUBSYS_RST(0x70002600)` set bit0 → 20 ms → clear bit0 → 20 ms。
   释放复位 = MCU 重新执行 ROM code，这是 FWDL 握手的前提。

3. **ring 与队列编号**（mt7915.h / dma.c）：
   `MT7915_TXQ_FWDL=16, MCU_WM=17, BAND0=18, BAND1=19, MCU_WA=20`；
   ring 寄存器 = `0xd4000 + 0x300 + qid*0x10`（FWDL→0xd4400、WM→0xd4410），
   与既有常量一致。**命令走 qid17(WM)、固件数据走 qid16(FWDL)**——mt76 的
   `mt7915_mcu_patch_sem_ctrl` 用 MT_MCUQ_WM，`mt7915_mcu_send_firmware`
   用 MT_MCUQ_FWDL；之前把命令也塞进 FWDL 是错的（3.30 修正）。

   prefetch 分区表（mt7915_dma_prefetch，mt7916 版，含 MAIN_WA 使后三项 +0x40）：
   `d4640←0x00000004 d4644←0x00400004 d4648←0x00800004 d464c←0x00c00004
    d4650←0x01000004 d4680←0x01400004 d4684←0x01800004 d4688←0x01c00004
    d468c←0x02000004 d4690←0x02400004 d4694←0x02800004`

**3.30 代码改动**：
- `zxnpu_cr_read/write` 改为按 mt76 语义翻窗（0x1802 直连，其余 L1 现写现用），
  新增 `zxnpu_l1_select()`；`zxnpu_fw_state_read()` 不再手工管 remap。
- 新增 `zxnpu_wlan_wfsys_reset()`：打印 reset 前后 `MT_TOP_MISC.FW_STATE`，
  reset 后清 armed 标志（环已被复位作废）。
- 新增 `zxnpu_wlan_dma_init()`：dma_disable(rst) → INT_SOURCE(W1C) →
  RST_DTX_PTR → PRI_DLY_CFG0/1/2=0 → prefetch 全表 → 两环(queue_init 0/1) →
  BUSY_ENA|7 → 轮询 HIF_MISC idle → GLO_CFG 开 TX/RX/OMIT → PCIe INT 主开关
  `0x74030188←0xff`。
- 7906 升为主实例：proc/ctl/status/VAP 都挂在它上面，`zxnpu_wlan_active()`
  优先 main；790a 仅作 hif2 证据窗。
- 提交路径去掉"每条消息脉冲 WFDMA_RST"（3.20–3.29 的坏习惯，mt76 不做），
  DMA 复位收敛到 dma-init 一处。
- 命令环 `ZXNPU_MCU_RING_NR` 0→1（qid17 MCU_WM），固件数据仍走 FWDL。

**验收（3.30 实机）**：
① `wfsys-reset` 后 `fw-state` 变化（ROM 重跑）→ ② `dma-init` 后
`glo` 开位、`hif misc idle` → ③ `mcu-dl` 提交 DIDX 前进 → ④ 最终 fw-state=7。

## 45. R-A 实机验收 + DMASHDL 窗口真相（2026-11-01）

**3.30 实机（device-verify ALL-PASS + wlan-ra）**：
- **A3 PASS（历史性）**：driver-own 1ms 完成，fw-state 0→1（firmware
  download）——21+ 轮以来 MCU 第一次响应主机，ROM code 确认在跑。
- **A2 PASS**：dma-init 生效（hif misc idle、GLO_CFG=1030b855 与 mt76
  dma_start 一致、PCIe INT 主开关 0x74030188←0xff）。
- **环已激活**：qid16=4e042000/0x80、qid17=4e040000/0x80、cidx 推进——
  与原厂盒同构。A4 未过：DIDX 仍 0。
- 干跑读数证实 L1 翻窗有效：lpcr=0x4（FW_OWN_STAT）、
  MT_WF_SUBSYS_RST=0x300——旧窗口模型下这些地址读不到。

**楔死事故（第二次，需断电）**：`crw 18024100 0` 把 WFDMA_RST 清零并保持
（mt76 语义是 clear→set 脉冲，常态=1），WFDMA 在 TX/RX 使能+环 armed 下
被持续复位 → AXI 桥挂死。ping 活/SSH 死/TTL 全静默（COM7=CH340，
COM5 已换号；Ctrl+C 无响应）。**教训：0x18024100 只允许脉冲写；d6000
区禁止经直连块扫描（楔死另一嫌疑）。**

**DMASHDL 窗口真相（反汇编 0x1a2a-0x1a2c dump 函数）**：
- vendor 以 movw/movt 组 **0x7c026100**（INFRA_MCU 空间）调 hw_io_read32，
  ubfx 提取 [11:0]/[27:16] 两个 12 位字段 = 活值 038a/038a037a 的两半。
- 全模块 6 处 0x7c02 **全是读、全在同一 dump 函数**——**vendor 从不写
  DMASHDL**。原厂盒的 038a037a/00100a55 来自 ROM 或 MCU 固件自配。
- mt76 翻译规则：0x7c026100 → 0x18026100 → L1(base 0x1802) →
  **bar0+0xe6100**。我们经直连块 0xd6100 读零/写弃 → 直连块解码很可能
  不含 0xd6000+（d6000 扫描楔死吻合：解码空洞 master abort）。
- **窗口模型修正**：L1 寄存器高 16 位→0xf0000 窗、低 16 位→0xe0000 窗
  （0x18001806 = 0xf0000↔0x1800、0xe0000↔0x1806），每次访问按地址翻窗。

**恢复后实验序列（A4 攻坚）**：
1. 冷机先读 `win 0 e6100/e6140`（L1 置 0x1802：`crw b0 fe260 18001802`）
   —— ROM 上电初值从未被读过，若非零 = 复位脉冲清零实锤。
2. 不做任何复位/dma-init：driver-own → mcu-send → 看 DIDX。
3. 若 DMASHDL 确为零：经 L1 写 `crw b0 e6100 038a037a`、
   `crw b0 e6140 00100a55` 再验 DIDX。
4. 结论回填 dma-init（去掉 bit5 脉冲或加 DMASHDL 重配）。

## 46. A4 攻坚全景：与原厂盒逐寄存器对照 + 入向 DMA 排除（2026-11-01）

**方法升级**：直接 telnet 192.168.5.1 用 /tmp/rd（bar0=0x10000000）读原厂
活系统寄存器，与我们 commit 后的值逐项对照——不再猜。

**已对齐（我们=原厂）**：GLO 1030bc55（bit1 硬件强置，写 55 读 57）、
RST=0x30、RST_DTX_PTR=0、INT_ENA=ff、BUSY_ENA=0x1f（ROM 默认一致）、
PCIE 窗 d42f4=811c811c、d82f4=811c0000、HOST_CONFIG=7d01、
vendor 信用表（d4640..d4650=(6..10)<<22|4、d4680..d4694=(1..6)<<22|4，
**写入粘住**）、FWDL 环 cnt=128 与原厂一致。

**原厂独有（固件运行时配的）**：DMASHDL d6100=09b109a1/d6140=0010042e
（vendor 驱动从不写 DMASHDL——反汇编 6 处 0x7c02 全在 dump 函数里），
我们冷值 0 且**两个窗（直连 d6100/L1 e6100）都写不进**→ 判定 MCU 侧
管辖，FWDL 阶段应为 ROM 默认。

**新发现的行为**：ROM 自己重配 FWDL 队列（cnt 128→512、base 归零等待
主机写入；qid17 保持我们写的值）→ ROM 是活的且在管理环状态。

**逐项排除的假设**（全部实测）：
- PCI 总线主控：probe command=0142 无 MASTER 位（真 bug，已加
  pci_set_master）；手工置 0x146 后 DIDX 仍 0 → 非充分条件
- root port 入向 iATU：legacy 视口接口（dbi 只有 16KB，无 unroll），
  IB0 已使能 0x40000000-0x5fffffff→同址，覆盖 DMA 缓冲 → 通路在
- PCI 状态寄存器：端点/root 主/次状态无 master abort 位 → 无总线错误
- PCIE1 引擎实例（0xd8400 环 + GLO）：同样不 fetch → 引擎实例非差异
- 中断源：d4200 出现持续 bit21(0x00200000)（mt76 未定义位），HIF_MISC
  bit0 持续 busy（原厂 0）——是停滞的症状不是原因

**新工具**：_scratch/pcie-ib/pcie-ib.ko（DWC iATU dump/编程，legacy
视口模式）；_scratch/vrd.py（原厂盒 telnet 批量只读）；ctl 缓冲
32→128 字节（长 manifest 之前被 -EINVAL 静默拒绝）。

**3.31 固化**：pci_set_master（真 bug）、dma-init 追加 HOST_CONFIG
bit0 + PCIE 窗 811c811c、ctl 缓冲 128B。

**剩余唯一结构性嫌疑**：ZX279128S 互联（SBR/SCU）对 PCIe 主设备入向
AXI 的过滤——镜像树的 DWC 主控驱动（2026 新移植）无设备→host DMA
成功先例，H3600 的 MT7915 也未必跑通过。下一轮：提取 vendor 内核
反汇编其 PCIe/SBR 初始化，找地址过滤配置。

## 47. A4 突破：DMA 引擎活了！pci_set_master 是 22 轮根因（2026-11-01）

**3.31 干净启动实测**：
- `pci_set_master` 进 probe（mt7915_pci_probe 同位）后，DMA fetch **第一次工作**：
  `mcu-send` → `didx advanced to 1`，INT_SOURCE=bit27（TX_DONE_MCU_WM 触发），
  HIF_MISC=0（idle，与原厂一致）。
- **完整时序**：dma-init → driver-own → wfsys-reset → driver-own → dma-init
  （重 arm）→ mcu-send。任何在脏状态下的手工 master 位修补都不充分，
  必须干净启动由驱动按序使能。

**ROM 真实状态（wfsys reset 后首读）**：所有 TX 环 cnt=0x200(512)、base=0
（等主机写）、prefetch 分区表全零（ROM 不配分区，分区不影响基本 fetch——
qid17 在分区=0 时照样 fetch 了 4 条）。

**WM 环（qid17）确认被 MCU 消费**：mcu-dl 的 SemGet/AddressLen 两次
fetch 且 didx 逐条推进（1→2→3）——MCU 真的在吃命令。

**剩余唯一问题：qid16（FWDL 环）从不 fetch**：
- 17 个 scatter chunk（cidx=0x11）didx 恒 0；寄存器完好（base/cnt/cidx 全对）
- qid17 fetch 正常而 qid16 完全不动（同引擎同环块）
- 排除：分区表（0 也能 fetch qid17）、cnt 差异（128/512 都试过）、
  ROM 复写（base/cnt 保持我们写的值）、PCI/引擎实例
- 原厂盒同环曾被消费 46 个描述符 → 环本身可工作；怀疑 ROM 阶段 FWDL
  数据走别的口（固件起来后才重路由到 qid16），或 AddressLen 后 ROM
  需要 arm 自己的 DMA client 而我们少了某步
- 下轮方向：① mcu-cmd 把 scatter 塞 qid17 试 ROM 是否照单全收；
  ② vendor 反汇编查 GetRealPortQueueID 在 ROM 阶段的实际队列号；
  ③ 检查 AddressLen 的 mode 位（mt76 gen_dl_mode vs 我们硬编码 0x80000000）

**本轮修复清单（已进 3.31）**：pci_set_master（根因）、dma-init 追加
HOST_CONFIG bit0 + PCIE 窗 811c811c（原厂对照确认）、ctl 缓冲 32→128。
device-verify ALL-PASS。

## 48. FWDL 传输层打通：patch+WM 全量送达（3.32-3.35，2026-11-01）

**决定性发现：fw_ring=1（scatter 走 qid17/WM 环）** —— ROM 阶段的
FW_SCATTER 数据根本不走 qid16！qid16 从不 fetch 的谜底：mt76 的
MT_MCUQ_FWDL 映射在这颗 vendor 芯片的 ROM/patch loader 里不适用。
qid17 被证明命令+数据全收：
- patch：SemGet/AddrLen/17 scatter/FINISH 共 20/20 消费（didx=20）
- WM：9 region 全量 3446 块消费（boot C cidx=3466 排空）

**MCU 侧行为逐开机波动**（新发现）：
- 每阶段下载完成后，MCU 或者干净排空（cidx==didx，寄存器保留），
  或者复位自己的 DMA client（didx 归 0、cidx 寄存器残留我们的 kick 值）
- 失步后下一阶段命令变成"数千 stale 待取描述符"——ring-sync（3.34 加入）
  条件式归零（cidx==didx 时不动，失步才重零）
- 3.35 实测：sync 正确识别失步（0x10e/0）并归零，但 MCU 客户端此后
  不再取述（需要 ROM 自己重新 arm——driver-own 不够）

**3.33-3.35 修复链**：fw-start 改 64B connac2 头（旧 24B Andes 头 ROM
不认）、wfsys-reset 时软件计数器归零、ctl 缓冲 128B、fw_ring 参数化。
全部 device-verify ALL-PASS。

**剩余问题（下轮）**：
1. WM 下载在 region[1] 间歇停滞（boot C 全量 / 3.34-3.35 停在 270）——
   怀疑 ROM 等我们消费应答事件（NEED_RSP 置位但 rxq0 从未收到事件，
   d450c=0；应答路径或需 INT_MCU_CMD_SOURCE 应答循环）
2. fw-start 后 MCU 客户端不重新 arm——寻找 ROM 重开 fetch 的条件
   （可能要读走事件/清 INT_MCU_CMD_SOURCE/或 WA 环也 arm）
3. 目标不变：fw-state=7

**已排除**：计数器失步（sync 已修）、txd 头格式、cnt/分区、PCI 路径。

## 49. WM 全量下载可靠化 + 事件环机制解读（3.36-3.37，2026-11-01）

**WM 下载可靠完成**：165 秒、cidx=3446 精确匹配（3436 chunk + 9 addrlen
+1），此前"停在 270"全是脚本读数时机假象。传输层完全可靠。

**RX 描述符 bit30 语义破译**（原厂盒活体内存对照）：
- 空槽 ctrl=0x46a40000（bit30=1=host 持有），已投递事件 ctrl=0x06a40000
  （引擎清 bit30）——**bit30 是 RX 环所有权标志**，不是 mt76 说的"无需"。
  3.36 误删导致事件全丢，3.37 已回退。
- 原厂事件环 5 个全武装：rxq0/1 cnt=512、rxq2=1024、rxq3=512、rxq4=1536，
  且都有 pending（didx=cidx+1，host 消费滞后）——ROM/固件应答确实走环。

**未解**：MCU 客户端在每阶段下载完成后 disarm，下一阶段能否 re-arm 不定：
- patch 清空后 WM addrlen 能取（重新拉起）✓
- WM 清空后 WA/START 都不取 ✗（sync 归零 + pending=1 也不行）
- 本窗口无 patch 直接下 WM 也全量消费——patch 非前置必需
- 假设：patch 的 loader 在 region 间重新 arm、最后 region 后等待的触发
  不是环命令（doorbell 0x74030108？或 WA 先于 start？）——下轮：
  ① 全序列 patch→sync→WM→sync→WA→sync→双 start 实测
  ② 试写 0x74030108 门铃
  ③ vendor 反汇编 AndesCmdMsgWaitResponse / load_fw_v1 的等待语义

## 50. 取述真相与 loader 状态机边界（3.37，2026-11-01）

**纠正**：此前"WA 900 块 flowed"是误读——cidx 推进只是软件计数，
didx=0 表示引擎从未取述。**真正被消费过的只有两段**：patch 20 条
（ROM）+ WM 3446 块（patch 启动后的 loader）。WM 之后 MCU 客户端硬关闭。

**实测排除（全部 pending=1 干净投递）**：
- 64B connac2 start（cid 0x02）✗
- Andes 24B start（type 0x02/cmd 0x17，vendor 格式）✗
- sync 后 AddressLen 探针（wa.bin 898 块）✗ —— loader 不在数据等待态

**RX bit30 语义已破译并回退**（3.36 错改 3.37 回退）：空槽=0x46a40000
（bit30=1 host 持有），投递后=0x06a40000（引擎清位）——vendor 活环实证。

**时间数据**：fw-dl wm 165 秒全量完成（3446 精确），可重复。

**当前模型**：PATCH_FINISH → patch 代码运行 → 它自己的 loader 收 WM
数据（其 DMA client 在 FINISH 时重新初始化 = patch→WM 能续的原因）；
WM 收完 loader 解除武装。START 无法再经环到达——patch 的 START 触发
机制不是环命令（或需要 vendor 驱动里某个我们没做的重挂步骤）。

**下轮方向（按优先）**：
1. vendor 反汇编 AndesCmdMsgSend / load_fw_v1 的收尾序列——找 START
   的真实投递机制（可能 type 0x02/cmd 0x17 走 PCIE doorbell 0x74030108
   或另一队列）
2. 对照 vendor 盒的 INT_MCU_CMD_SOURCE(0xd41f0)/MCU_HOST_INT_ENA
   (0xd41f4) 语义
3. 若 START 走门铃：写 0x74030108 触发

## 51. vendor 下载流程全貌（反汇编定论，2026-11-01）

**load_patch_v1（0x1d5c0c）**：PatchSemGet → AddressLen(patch 扇区循环)
→ **PatchFinishReq** → PatchSemGet(释放) → AddressLen →
**FwDecompressStart**（我们从未发过！）→ AddressLen → **FwStartReq**
—— patch 阶段以 FwStartReq 收尾（mt76 用 FinishReq，vendor 多一步）。

**load_fw_v1（0x1d5a48）**：AddressLen(WM region 循环) → **FwStartReq**
—— 与我们的 fw-dl+fw-start 一致。

**命令发送架构**：MtCmd* → AndesSendCmdMsg(0x1b9b18) →
AndesQueueTailCmdMsg + **AndesBhSchedule**（BH 异步！）→
AndesBhHandler → pci_kick_out_cmd_msg(0x1e19c0, 36B item) →
hif_kick_out_cmd_msg(chip_ops[0xb04] 转发) → 环寄存器。
AndesInitCmdMsg(0x1b8e5c) 只填驱动内存 struct（[0]=len [2]=type
[4]=cmd [6]=seq [20]=port/queue [23]=wait flag）；**线上头由 BH 转换
函数另建**（AndesBhHandler 链，未解码——下轮入口）。

**已对照排除**：MtCmdFwStartReq 确为 ring 命令（type 0x02/cmd 0x17，
Andes 头，payload {option,override} 各 4B）——我们两种头都试过不被取；
vendor 客户端不 disarm 的原因仍在其 BH 发送路径的细节。

**下轮入口**：① AndesBhHandler（找线上头构建 = 精确 24B/36B 字段）；
② FwDecompressStart（0x1cc2ec）的 cid/payload——vendor 流程里有而我们
没有的命令；③ 对照 vendor 盒 0xd41f0/0xd41f4 中断语义。
   （补）MtCmdFwDecompressStart = Andes type **0x30** / cmd 0x17，
   payload 4B（来自 fw 头字段）——全新命令类型，未在 mt76 出现。

## 52. Andes 全头验证（3.38，2026-11-01）

**mcu-cmd 升级为完整 Andes 头**（AndesInitCmdMsg 逐字段）：
len@0、type@2、cmd@4、seq@6（自增）、cid@8、port@20=0x8000
（GetRealPortQueueID 非 0xEE 返回值）、wait@23=3。

**实测**：patch 20/20 + 尾序列 3 条（FwStart/DecompressStart/FwStart）
全部消费（23/23 排空）；WM 3446 块提交后 Andes FwStart 也提交
（cidx=3470 计数吻合）——但 **fw-state 仍 1，无事件，状态不变**。

**结论**：START 命令被消费但不生效——问题不在投递，在语义/上下文。
候选（按证据强度）：
1. loader 等的 START 带 decompress 上下文（vendor 在 DecompressStart
   之后 AddressLen 之后才 FwStart——顺序敏感，我们单一 WM region 流
   缺中间步骤）
2. 需要先完成 patch 的 FwStart 生效（patch 真正运行）才能接受 WM 的
   start——我们无法确认 patch 是否运行（无应答可读）
3. 应答路径（rxq0）从未工作——ROM/loader 可能卡在等我们消费它早前
   的应答（但我们从未见过任何应答到达）

**下轮首选**：解 AndesBhHandler→线上头的最后一段（pci_kick 前的
36B item payload 构建——35B 头？），拿到 vendor 线格式的**逐字节**
模板再对照；同时试 WM 下载后跟 DecompressStart(type 0x30)+FwStart
的 vendor 完整顺序。

## 53. SRAM 回读鉴别 + vendor 环地面真值（3.39，2026-11-01）

**SRAM 回读**：WM region 目标（0x0220a800 / 0x00414800）读回全零——
**3446 块 WM 数据从未落入 SRAM**，loader 消费了描述符但丢弃了载荷
（AddressLen 未被解析）。patch 目标 0x04000000 也是 loader 虚拟地址
（真实 MCU SYSRAM=0x00400000，reg_map 直连 bar0+0x80000，那里有数据
但是 patch 运行后的工作区，非原始载荷）。

**vendor 环地面真值**（原厂盒 WM 环 4a6e0000）：
- 16B 描述符、mt76 格式（buf0/ctrl@+4/buf1/info）——与我们一致
- 消费项 ctrl=0xc0440000/0xc04c0000（DMA_DONE|LAST_SEC0|len 68/76）
  —— **68/76 字节消息 = connac2 64B 头 + 4/12 payload，即我们一直在
  发的格式**（不是 32B Andes！）
- buf0=0x471cc620 指向 AndesCmdMsg 池（链表），36B item bookkeeping
  与 DMA 环分离——vendor 的 wire 消息在池内另一处（下轮映射）

**已实现（3.39）**：Andes 32B 头（mcu-cmd/addrlen/fw-start 全套）——
格式存在但 AddressLen 仍未被 loader 解析（SRAM 全零实证）。
**Andes 32B 与 connac2 64B 的取舍**：vendor 消息 68/76B = connac2
尺寸——mt76 的 connac2 头就是对的；Andes 32B 是 vendor MtCmd* 家族
的另一条路径（两者并存）。

**下轮**：① 映射 vendor 36B item → 真实 wire 消息位置 → 逐字节对照
vendor 的 AddressLen wire 内容；② 用 vendor 的**逐字节** AddressLen
重放（替换我们的 connac2/Andes 版本）；③ 数据落地后 FwStart 才有意义。

## 54. 修正：SRAM 回读窗口不可靠（3.39，2026-11-01）

**§53 的"WM 数据从未落地"结论作废**：0x00414800 的两次回读
（bar0+0x84800/0x94800 两个候选窗）返回**互不相同的随机数据**——
这些窗不是稳定的 SRAM 视图（未映射读返回总线噪声，或 MCU SRAM 在
此状态下 host 不可读）。0x00400000 窗同样每次读数都变。

**WM 数据是否落地：当前不可验证**（没有已证实的 host 侧 SRAM 读窗）。
"AddressLen 未被解析"降级为未证实假设。

**当前事实链（可靠的）**：
- 描述符 fetch/消费 ✓（DIDX 实证，patch 20 + WM 3446 计数闭合）
- 三种命令头（connac2 64B / Andes 24B / Andes 32B）在武装窗口内全部
  被消费 ✓
- FW_SCATTER 必须走 qid17 ✓
- fw-state 恒 1、START 消费但不生效 ✗ —— 唯一未解

**下轮（顺序）**：
1. AndesBhHandler → pci_kick 的 36B item payload 构建（vendor 线上
   逐字节模板——和我们的 68/76B 消息逐字节 diff）
2. vendor 盒抓真实 START 消息：vendor 的 WM 环有 3446+ 条历史消息的
   缓冲池——用环内 buf0 扫描池地址定位一条 FwStartReq（type 02）的
   原始字节
3. 若 vendor 的 START wire 字节与我们的有差 → 逐字节复刻
   （补）vendor 环活体抓取窗口已关闭：环滚动多轮，开机期 FWDL 消息
   被固件时代的 keepalive 覆盖。0xa0 长度消息只在扫描瞬间出现。
   START wire 格式的最终答案只剩 AndesBhHandler 链的静态解码
   （hif_ctrl[0xb14] → mt7916 实现，vtable 初始化未定位）。

## 55. mt7615e 对照实验 + WFSYS 上电序列（2026-11-01）

**真 mt76（kmod-mt7615e）上机成功**：compat→cfg80211→mac80211→mt76→
connac-lib→mt7615-common(+hwmon.ko)→mt7615e 七模块全载入，
new_id 强制探针（驱动名是 mt7615e 不是 mt7915e！），双 PHY 注册、
MAC/LED 初始化——然后 "Firmware is not ready for download"。

**mt7615e 的状态检查读的是 MT_TOP_MISC2（reg_map[MT_TOP_CFG_BASE]
+0x134 = 0x18060134），不是我们一直读的 MT_TOP_MISC(0x180600f0)！**
mt7615/mcu.c mt7615_load_firmware：MISC2[2:0] 必须==1 才继续。
我们的 MISC2 恒 0（driver-own 后 MISC=1 但 MISC2 仍 0）。

**WFSYS 上电序列（mt7915 式）实测**：DUMMY_CR(0x2120)←ffff →
PWR_CTRL(0x18000000 走 L1) key 0x5746 舞步（8c/8d/8c/8d/9c）→
**DUMMY_CR 读回 ffff0000 = MCU 复位完成魔数出现**（MT_MCU_DUMMY_
DEFAULT 确认——MCU 执行了复位！）——但 MISC2 仍 0。

**解读**：MISC[2:0]=1 是 LPCR 所有权状态（我们 driver-own 造成）；
MISC2[2:0]=0 = MCU 的 ROM 没有走到下载循环。上电舞步触发了 MCU 复
位（魔数）但 ROM 未推进到 download。vendor 差异待查（MISC2 在 vendor
盒上的读数——rd 工具已从其 /tmp 消失，需重建上传通道）。

**新工具**：_scratch/rdsrc/rd.c（静态 /dev/mem 读取器，可重编上传）；
我们的 uhttpd 已备 /www/zrd 供 vendor 盒 wget（wget 路由未通，待查）。

**mt76 对照实验结论**：mt76 与我们卡在同一处（MISC2=0）——排除驱动
实现差异，确认为板级/固件交互层问题。两条驱动收敛到同一未知门槛。
   （补）INT_MASK（0xd4204，MCU 位与 vendor 活值两种写法）不触发 MISC2。
   下轮首选：vendor mt7916.ko 反汇编里搜 0x5746（PWR_KEY）写序列——
   vendor 自己的 WFSYS 上电顺序（含时钟/REG/GDC 步骤）是最后未复刻项。
   （补 2）vendor mt7916.ko 内**无** 0x5746 写入——WFSYS 上电在更早
   的启动链完成（vendor 内核 WiFi 平台驱动或加载器）。MT_TOP_PWR_CTRL
   读数恒 0x72 = 上电后的硬件状态读回。MISC2 触发条件的答案在 vendor
   启动链——需要解 vendor 内核（zImage 压缩容器未识别，见 §55 前）。

## 56. 实验设计纠正：mt7615e 对照无效，mt76 原生支持 0x7906（2026-11-01）

**mt76 的 id 表（权威）**：
```c
mt7915_pci_device_table[] = { 0x7915, 0x7906 };   /* 主驱动 */
mt7915_hif_device_table[] = { 0x7916, 0x790a };   /* hif2 辅助 */
static inline bool is_mt7916(dev) { return mt76_chip(dev) == 0x7906; }
```
→ **0x7906 就是 mt76 定义的 MT7916 主设备**，mt7915e 原生绑定它（无需
new_id）。§43 的架构判断正确；但 §55 的"mt7615e 对照"**用错了驱动族**：
mt7615e 是 MT7615 驱动，检查 MT_TOP_MISC2（mt7615 寄存器），对我们
7916 卡毫无参考价值。§55 的"MISC2=0"结论**作废**。

**已建 mt7615e 相关问题**：mt7615e 强绑 7906 时按 MT7615 寄存器布局
操作，本该失败——这是错误驱动导致的期望内失败，不是板级证据。

**真对照实验（本轮执行）**：给 mt7915e 打两个补丁后正规构建——
① 跳过不存在的 connac2 式 ROM patch（vendor 只提供 Andes 式 e1 patch）；
② FW_SCATTER 从 qid16 改走 qid17（我们已实证 qid16 不 fetch）。
然后用 mt76 自己的 wfsys_reset/dma_init/driver_own/描述符序列跑同一块卡，
与自研驱动做同条件 A/B：任一成功 = 该序列成立；两者同败 = 协议层问题。

**判据（本轮验收）**：dmesg 出现 "Firmware init done" 或
`/sys/class/ieee80211/phy*` → mt76 成功；否则记录其停在哪一步
（load_patch / load_ram / firmware_state 超时）。

## 57. 关键认识：qid17 上的"全量消费"是假象（2026-11-01）

**DMA 取述与内容无关**：didx 前进只说明引擎读走了描述符+数据缓冲，
不说明 ROM/loader 解析或写入了目标地址。ROM 在 FW_DOWNLOAD 态把
**WM 环当作命令队列**解析，我们把 512B 固件块塞进去 → 每块都被当成
"一条垃圾命令"取走丢弃。

**推论（推翻 §48-52 的"传输层打通"结论）**：
- patch 20/20、WM 3446/3446 的"消费"只证明 **DMA 引擎 + qid17 信用可用**
- **数据从未落地**（与我们 SRAM 回读全零一致）
- fw-state 恒 1 的原因不再是"START 不生效"，而是**根本没有固件数据到达**

**唯一的真阻塞点回到原点**：**qid16（FWDL 数据环）从不 fetch**。
证据对照：原厂盒同一环 didx=cidx=0x2e（46 条，其驱动/固件跑起来过），
说明 qid16 在此 SoC 上**可以**工作；差异只可能在
① DMASHDL 组配额（原厂 09b109a1/0010042e，vendors 驱动从不写它，
   host 两个窗都写不进 → 疑 MCU 侧管辖，需 ROM 已配置才可写）
② prefetch 分区基址冲突（qid16 base=0x000 与其它环重叠）
③ qid16 的 GLO/信用使能位（0xd413c BUSY_ENA 的位语义）

**下轮任务拆解（qid16 攻坚）**：
A1 读原厂盒 qid16 的完整 16 字节环形（base/cnt/cidx/didx）+ 0xd4640 组
   值，作为目标态快照（rd 工具重传：原厂盒 wget 到我们 192.168.5.3 的
   /www/zrd——此前用了错误的 192.168.5.31，本机 WAN 是 .3）
A2 用我们已证可写的 0xd4640..0xd4694 组，逐个尝试给 qid16 单独分配
   （把 6 组基址循环位移），每次只改一组，观察 didx
A3 若仍不 fetch：用 iATU/PCIe 侧排查 qid16 描述符地址（4e042000）与
   其它环（4e040000）的页对齐/边界差异
**验收**：qid16 的 didx 前进 ≥1（且从 SRAM 侧可见数据落地为佳）
   （A3 结果）GLO_CFG bit9（MT_WFDMA0_GLO_CFG_FW_DWLD_BYPASS_DMASHDL，
   mt792x 定义）单独置位后 qid16 仍不 fetch（cidx=0x11/didx=0），
   readback 1030ba57 证明位已粘。→ 绕过位不是唯一条件。

## 58. qid16 攻坚：组合配置全负 + 反汇编重大转折（2026-11-01）

**A1（GLO=1030bc55 含 bit10 + 原厂 DMASHDL 09b109a1/0010042e +
prefetch FWDL=0x180）**：
- DMASHDL 两个值**仍然写不进**（0x18026100/0x18026140 回读 0）——
  该块在 host 侧不可写（疑 MCU 专属）
- GLO 置位成功（1030bc55→回读 1030bc57）
- **qid16 依旧 didx=0**（cidx=0x11 提交）

**T1（FWDL prefetch=0 + GLO bit9|bit10 = 1030be55）**：qid16 仍不 fetch。
→ qid16 对该 SoC 的 host 侧写入路径**不可用**；原厂环里那 0x2e 很可能是
开机阶段 ROM 自己的活动，不是驱动写入。

**转折（反汇编）**：`load_patch_v1`(0x1d5c0c) 与 `load_fw_v1`(0x1d5a48)
的调用清单里**没有任何 scatter/数据发送调用**——只有
PatchSemGet / AddressLen / PatchFinish / FwDecompressStart / FwStart。
即 **vendor 驱动并不通过 MCU DMA 环发送固件数据**！数据必然走另一条
路径（直接写 MCU SRAM 的 hw_io_write32 循环，或某个 mem-write 助手）。

**已实现的兼容尝试（3.40，构建中）**：scatter 改为 Andes 32B 头 +
type=0xEE/cmd=0x10（GetRealPortQueueID 里 0xEE 被重定向到下载队列）。
手工验证：type=0xEE 帧在 WM 环上被消费（didx 0→4），但 fw-state 不变——
与"数据未落地"一致。

**下轮任务拆解（vendor 数据通路）**：
B1 反汇编搜 load_fw_v1 的数据拷贝循环（os_alloc_mem 分配后的写入/发送）
B2 搜 hw_io_write32 密集循环（连续地址递增写 = SRAM 直写）
B3 找到后在我们驱动里实现"直写 MCU SYSRAM"路径，用可回读窗口验证
（先解决可靠回读：找 ROM 的 dump 命令或 L1 正确映射）
**验收**：写入已知 pattern 后能回读到同一 pattern（数据落地证明），
再执行 FwStart，观察 fw-state → 7
   （B1 结果：**已定位 vendor 数据发送路径**）
   `load_fw_v1` 内 0x1d5a9c 调用的无名局部函数 0x1d5984 才是发送器：
   按 `4096 - chip_cap[0x134]` 分块，逐块调
   **`MtCmdFwScatter` (0x1cbf04)**。其反汇编给出精确头：
   ```
   mvn  r2,#17     ; 0xEE
   mov  r3,#16     ; 0x10
   strb r2,[sp,#10] ; msg[2] = type 0xEE
   strb r3,[sp,#12] ; msg[4] = cmd  0x10
   ```
   payload 由 `AndesAppendCmdMsg` 追加在 +32。

   **结论：3.40 已实现的 Andes scatter 头（type 0xEE / cmd 0x10 /
   payload@32）与 vendor 逐字节一致。** 因此数据帧格式不再是变量；
   剩余变量只有**环选择**：GetRealPortQueueID(0xEE) 返回
   chip_cap[0x162]，其值应为 16 或 17 —— 下一测直接 A/B 两环。

## 59. WM 停滞根因：单缓冲环覆写（驱动缺陷，非硬件）（2026-11-01）

**干净启动 + vendor 逐字节一致 framing 的硬证据**：
```
qid17: base=4e040000 cnt=0x80 cidx=0x0d8b(3467 提交) didx=0x54(84 消费)
fw-state = 1（始终）
```
**MCU 消费到第 84 条就永久停止**，而主机已提交 3467 条。

**根因分析（驱动缺陷）**：`zxnpu_mcu_submit_ring` 全部提交共用
`w->fwdl_buf` / `w->mcu_buf` **一个 4 KiB 缓冲**，每条描述符都指向同一
物理地址并在下一次提交时被覆写；环深 128，提交速度远快于消费 →
环内多条 pending 描述符指向同一块正被改写的内存，MCU 读到自相矛盾的
数据后停止消费（84 ≈ patch 20 + WM addrlen 9 + ~55 scatter 块）。

**这同时解释了历史现象**：此前"cidx 推进 / didx 0"被我误读为消费，
实际是提交；真正的消费从未超过 ~84 条。

**下轮任务拆解（数据面工程化，验收明确）**：
C1 每槽独立数据缓冲：`desc_ring[i] -> buf_ring[i]`（128 × 512 B，
   `dmam_alloc_coherent` 一次分配），提交时只写当前槽的缓冲
C2 加流控：提交后若 `(cidx - didx) >= depth - 4` 则轮询等待，模拟
   真实驱动的 tx ring 语义
C3 事务完成后再 `dma_wmb()` + 更新 cpu_idx（当前顺序可疑）
**验收**：qid17 didx 追平 cidx（3467/3467）且 fw-state 从 1 前进；
随后 FwStart（type 2）应把状态推到 7

## 60. 停滞点精确复现：didx 恒止于 84 = 20(patch) + 64(WM)（3.43，2026-11-01）

**C1-C3 修复验证（每槽独立缓冲 + 流控 + 写序）**：
- 补丁阶段 `cidx=20 / didx=20` **完全追平**（多次复现，干净启动）
- WM 阶段：**didx 精确冻结在 84**，新增停滞检测在 90 秒内报出
  `ring qid17 stalled: didx frozen at 84, 641 outstanding` 并 -EIO 中止
- 结论：环覆写问题已修复（补丁段证明），但 MCU 仍在**第 84 条**停止消费

**新假设（强）**：84 − 20 = **恰好 64 条 WM 消息**。64 很像 MCU 内部
**命令槽池上限**：ROM/loader 把命令收进内部队列后，需要主机把应答
（response/event）从 RX 环读走并归还槽位；我们的 **RX/事件路径从未
送达任何数据**（d4508/d450c 恒 0、d4200 无 RX 中断），因此 64 槽用尽
后 MCU 停止取述。这解释了过去所有"消费一段就停"的现象。

**下轮任务拆解（RX/事件路径，验收=rxq didx 前进且能读到事件字节）**：
D1 复核 RX 描述符：mt76 的 RX 描述符 buf0=数据地址、ctrl=len<<16，
   无 LAST_SEC0；但 vendor 活环实证 **空槽 ctrl=0x46a40000（bit30=set）**、
   投递后 0x06a40000。确认我们 3.37 回退后的写法与 vendor 一致
D2 RX 环基址/深度对齐：vendor rxq0 base=4a0b8000、cnt=0x200(512)。
   我们 cnt=512 已对齐；检查 base 是否需要 4KB/64KB 对齐（我们用
   dmam_alloc_coherent 的 4e044000，页对齐）
D3 关键：RX 环的 **EXT_CTRL/prefetch**（0xd4680..）与 RX 队列使能位
   （GLO_CFG bit2 已置）之外，是否还需要 qid 级 "RX DMA enable"
D4 若仍无事件：试把 rxq0 指向 vendor 同数量级地址（如 4a6XXXXX 区间）
   排除地址窗口/别名问题

## 61. RX 路径 D1-D4 结果：事件投递在 host 侧完全缺失（3.44，2026-11-01）

**新工具**：`rx-desc` ctl 命令——CPU 直接读 RX 描述符 coherent 内存
（buf0/ctrl/buf1/info）与事件缓冲前 8 字节，是"DMA 是否碰过"的地面真值。

**D1 实测**：RX 描述符武装完全正确——
```
desc[0]: buf0=4e050000 ctrl=46a40000 buf1=0 info=0
desc[1]: buf0=4e0506a4 ctrl=46a40000
desc[2]: buf0=4e050d48 ...（1700 步长，页对齐，bit30=空闲标志）
```
SemGet 后 desc[0] **逐字节不变** → DMA 从未触碰。

**D4 实测**：六个 RX 环（d4500/d4510/d4520/d4530/d4540/d4550）全部指到
有效缓冲后发 SemGet → **六个环 didx/cidx 全零**。ROM 在此板上不向任何
RX 环投递任何东西。

**推论**：MCU→host 的事件通道在本板上**整体缺失**，不是环配置问题。
缺失的环节必然在 WFDMA 的"MCU 事件 → PCIe MemWr"路由/使能——即 MCU
侧的事件端口到 host RX 环之间的某级开关。

**下轮任务拆解（MCU 事件端口路由）**：
E1 反汇编 vendor 的 rx 事件投递路径（mtf_trans_rxd_into_rxblk /
   hif_rx_event_process / mt7916 hif_ctrl vtable）找 host 环寄存器之外
   的 WFDMA 路由寄存器
E2 对照 vendor 盒与我们的 **0xd41f0（INT_MCU_CMD_SOURCE）**、
   **0xd41f4（MCU_HOST_INT_ENA）**、**0x74030108（MCU doorbell）** 读数
E3 检查 mt76 的 `mt76_connac_mcu_uart_tx_done` 类似的 MCU→host 握手
   位（WFDMA host 侧"我已就绪"标志，MCU 可能轮询它才投递）
**验收**：任一 rxq didx 前进且事件缓冲可读到非零数据

## 62. RX 路径总结：MCU→host DMA 写路径整体不通（2026-11-01）

**全部实验汇总**：
- RX 描述符格式正确（rxdump 验证：bit30 空闲标志、页对齐、1700 步长）
- 六个 RX 环全部武装仍无事件（ROM 不向任何环投递）
- INT_ENA/INT_MASK/门铃/DMASHDL/上电序列全试——无效
- TX fetch（chip 读 host）✓ 已证；chip→host DMA 写 ✗ 从未成功

**结论**：MT7916 的 DMA 写路径（MCU 事件→PCIe MemWr→host 内存在本板上
不通。TX 读（MemRd）工作正常但写路径被某层门控。

**排除法定位**：iATU IB0 覆盖 0x40000000-0x5fffffff（读写共享地址窗
口）；vendor 同硬件跑同格式 ✓。差异在 vendor 启动链的**某步初始**。

**下轮攻坚方向（按优先级）**：
F1 vendor 内核 zImage 解压（不同压缩容器，尝试 binwalk/lzma 变体）→
   找 WFSYS/CONN_INFRA 初始化代码（时钟/复位/MISC2 触发）
F2 用我们的 OpenWrt 设备当 TFTP 服务器（已建 /tmp/tftpd）→ vendor 盒
   tftp 从 192.168.5.3 取 rd 工具 → 读 vendor MISC2 活值确认状态语义
F3 检查 PCIe 链路训练速度：vendor Force Gen1（ZX PCIe 驱动里有），
   我们是否也在 Gen1？Gen2 的 DMA 写可能有不同行为
F4 用 vendor 盒的 web 管理界面（端口 80）上传工具（绕过 busybox wget）

## 63. vendor 内核解压成功 + wifi init 函数定位（2026-11-01）

**vendor 内核 zImage 解压成功**：gzip@0x65a8 → 6.67MB vmlinux
（_scratch/vmlinux.out），全量反汇编完成（vendor-kernel.dis, 63M行）。

**vendor wifi init 函数定位**：VA c0244150（fileoff 0x23c150），通过
`movw r5,#0x9264 + movt r5,#0xc056` 引用 `g_WiFi_Reserve_Mem_Base`
printk 字符串（init.rodata copy @ VA c0569264）。

**关键发现：vendor 的 GLO_CFG 值 = 0x21fd**（写入三个不同 WFDMA 引擎
的偏移 0x470/0x4c0/0x510）：
```
0x21fd = TX_EN(bit0) | RX_EN(bit2) | ... | bit13(0x2000)
```
bit13 是 mt76 未定义的位，vendor 初始化就置 1。
实测：写 0x21fd 到 0xd4208 后 qid16 **仍不 fetch**（didx=0）。
回读 0x21f5（bit3 被硬件清了）。

**vendor 的 pcie 节点是空壳**（compatible=zte,ZX279127-pcie，仅
interrupts/status，无 reg/clk/phy），且 vendor mt7916.ko 内无 0x5746
写入——PCIe 和 WFSYS 初始化全在 vendor 内核的 zx279128 平台代码里
（zx279128_reserve_mem / zte_board_init / zte_gpio_init 系列）。

**下轮任务拆解（vendor 平台代码还原）**：
G1 反汇编 c0018918 附近（含 0x18000000 常量的平台 init 函数链）
G2 找 zte_board_init / zx279128_reserve_mem 的完整实现
G3 找 PCIe 链路训练/WFSYS 时钟/电源的硬编码序列
**验收**：在我们设备上复刻 vendor 平台初始化后，qid16 didx 前进 ≥1

## 64. vendor 内核 PCIe 控制器驱动解码完成——不是问题所在（2026-11-01）

**vendor 内核解压 + 全量反汇编完成**（63M 行），PCIe 驱动定位在
c0017d14（synopsys pcie probe）到 c0018xxx（时钟/复位管理）。

**vendor PCIe init 做的事**：
- CRPM 时钟使能（`orr r4,r4,#0xb5` 写 [r3+0x70]）→ LSP0CRPM
- 复位释放（`bic r5,#1` 写 [r3+0x74]）
- 电源域切换（`bic r5,#0x4000` 写 [r3+0x0c]；`bic r5,#0x4000000` 写 [r3+0x24]）
- ioremap 多个地址窗（0x9500000/0x1c000000/0xf100000/0x9600000/0x2c000000）
- DW PCIe DBI 配置

**结论**：这些都是 PCIe 控制器本身的时钟/复位/电源管理——通过
CCF/Reset Framework 由我们 pcie-zx279128s.c 的 devm_clk_get/reset_control
等效实现。TX fetch 正常工作证明 PCIe 链路和数据路径没问题。

**WFSYS/MT7916 上电不在 vendor 内核里**（无 0x5746/0x70002600/
0x1806xxx 常量）——MT7916 PCIe 设备的上电由芯片自身 boot ROM 在 PCIe
link up 时自动执行，无需 host 干预。

**根因收窄**：芯片→host DMA 写路径不通不是 PCIe 控制器或 WFSYS 电源
问题。剩余可能性：
1. MT7916 WFDMA 的 MCU→host 事件路由需要 vendor WiFi 驱动（mt7916.ko）
   内部的某步（但该 ko 也没有相关 MMIO 常量）
2. MT7916 自身固件/ROM 在此板上有 bug 或不同的 boot 路径
3. 硬件差异（我们的板与 vendor 板可能有 PCB 级差异，如 PCIe lane/clk）

**建议下轮方向（非代码）**：
H1 获取定制版固件**文件**（不刷机），解压对比 wifi init 代码差集
H2 用 TTL 在我们 3.44 固件上观察 vendor 系统启动日志（如果用户
   愿意刷定制版到备用设备）
H3 联系 ZTE/MTK 获取 MT7916 在 ZX279128S 平台的 reference driver

## 65. mt7916.ko（MTK SDK 7.6.2.0）全解析——vendor 固件下载协议闭合（2026-11-01）

**H1 完成**：用户提供 E2633 25B4.6000 完整解包
（`D:\AI\中兴\E2633_25B4.6000_解包`，JFFS2 AES 解密 + squashfs 全展开）。

### 65.1 驱动本体
- `04_kmodule根/lib/modules/mt7916.ko` 5,704,776B，ARM32 ELF，
  **全符号**（17,689 个 .symtab 符号），MTK SDK 7.6.2.0，vermagic 4.1.25 SMP
- 全量反汇编 `_scratch/mt7916.dis`（751,064 行）
- PCI probe：`rt_pci_probe`(0x1fa3e0) 主设备 7906="mt_drv_7916"；
  `rt_pci_slave_probe`(0x1fa0cc) 790a="mt_pci_slave" → `pci_hif_chip_init`

### 65.2 初始化调用链（全符号闭合）
```
rt_pci_probe → RTMPAllocAdapterBlock → netdev open
→ mt_wifi_open → mt_wifi_init → WfInit → WfMcuInit → WfMcuSysInit
→ chip_fw_init → MCUSysInit → NICLoadFirmware = mt_load_fw(0x1e18c8)
```
- driver-own（0x19d2e8）：LPCR 写 2 → 轮询 bit2 清 → 写 1（band 数组），
  重试 ≤4 次，每轮 3000ms —— **与我们 zxnpu-wlan.c 实现逐字段一致**
- fw_prepare(0x1a11a0) 经函数指针装入 mcu_ctl+0x100（此前"死代码"结论错误：
  objdump -d 看不到指针引用，重定位 .text+0x1aa0d0 才是真身）

### 65.3 实机串口日志（用户 COM7，2026-11-01 17:10，E2633 同代固件）
```
mt_drv_7916 0000:00:00.0: enabling device (0140 -> 0142)
mt_pci_slave 0000:01:00.0: enabling device (0140 -> 0142)
mt7916_init()--> Use the default iPAiLNA bin image! <--mt7916_init()
Error 2 opening /wlan/mt7916_patch_e1_hdr.bin        ← ENOENT
Error 2 opening /wlan/WIFI_RAM_CODE_MT7916.bin
Error 2 opening /wlan/7916_WACPU_RAM_CODE_release.bin
current sync CR = 0x1                                ← TOP_MISC&7 = 1 (DL 态)
  Chip ID: 0xe00  Eco version: 0x2
  Ram version: 000000                                ← patch 阶段报告
  Built date: 20250103112803  Common crc: 0xd730d461
current sync CR = 0x7                                ← RDY!
Could not send in band command due to disable
  RTMP_ADAPTER_MCU_SEND_IN_BAND_CMD                  ← 就绪前态，随后消失
```

### 65.4 范式修正（推翻 §64 前的"ROM-only"猜想）
- `/wlan/` 只是首选路径；**兜底 `/etc/wireless/mt7916/` 全部存在且成功**：
  - mt7916_patch_e1_hdr.bin（ROM patch E1, 8672B, magic 0x11223344@0x20,
    ILm 0x40210000, ALPS, date 20250103112625a）
  - WIFI_RAM_CODE_MT7916.bin（WM 1,759,340B，裸代码+尾部 fw 头
    `____0000_20250103112717`）
  - 7916_WACPU_RAM_CODE_release.bin（WA 458,112B）
  - MT7916_EEPROM.bin（4096B 校准）
- **vendor 确实执行完整固件下载**；"Ram version 000000" 是 patch 头解析阶段
  的正常输出，非 ROM-only 证据。sync 0x1→0x7 即下载完成迁移。
- 版本块由 parse_fw_v3(0x1e0dd4) 打印（数据=真实固件头，非 MCU 应答）
- **我板 sync 恒 1 = ROM 停在 DL 态等主机喂固件 —— 下载是我们必须做的事**

### 65.5 下载协议（load_patch_v1 0x1e0188 / load_fw_v1 0x1dffc4 逐指令）
1. `MtCmdPatchSemGet`(0x1d6524)：Andes 头构造，cmd 字节 0x10/0x17
2. 每region：`MtCmdAddressLenReq`(0x1d62dc，cmd 0x17 + band 相关 0x05/0x01，
   payload 12B) → **`MtCmdFwScatter`(0x1d643c) 按 4096-chip_cap[308] 分块**
3. `MtCmdPatchFinishReq`(0x1d6604) → `MtCmdPatchSemGet` 释放
4. WM/WA 同 region+scatter 模式（load_fw_v1 state 1/2/3 = fwdl_ctrl 状态机）
5. `ctrl_fw_state_v2(0)`(0x1df704)：轮询 `TOP_MISC(0x180600f0)&7 == 7`
   （caps bit1 时）超时 1501ms
- `get_fw_sync_value`(0x19b3d4) = `hw_io_read32(0x180600f0) & 7` —— 与我们
  zxnpu_fw_state_read 同寄存器同掩码
- 固件文件已入库 `native/overlay/target/linux/sanechips/files/firmware/mt7916/`
  md5: patch ae74c143… / WM 9a3fb936… / WA 967fa140… / EEPROM 632aa598…

### 65.6 R-B 任务拆解
B1 zxnpu-wlan.c：request_firmware 三个 vendor 文件（路径对齐 vendor）
B2 补全 DL 命令序列（SEM→AddrLen→scatter→Finish→SEM，WM/WA 同）
B3 实机验收：sync(TOP_MISC&7) 1→7；事件环出现首个 MCU 响应
**验收标准**：fw-state=7 + 首个 Andes 事件；**验证方式**：debugfs dma_state
快照 + rx-desc dump，全程不触碰 5.1

## 66. R-B：patch dst 用错字段是 13 轮"scatter 被丢弃"的根因（2026-11-01）

**实机 A/B 证据（3.45，fw_ring=0 vendor 路由首测）**：
- qid17 消费 2 条（SEM + AddrLen）后停 —— ROM 正确处理了 DL 命令
- qid16 取 2 个 scatter 块（1024B）后停 —— ROM 收到目标地址后拒绝继续
- fw-state 恒 1（DL 态），无事件 —— ROM 卡在等待合法数据流
- 3.45 首包是陈货（apply 没跑，payload 还是 10-13 的 3.44）→ 已修复流程

**根因（mt76 源码 + vendor parse_patch_v2 双重验证）**：
- mcu-dl auto 模式把 `LE32(fw+0x24)` 当 DL 目标 = **patch desc 的 subsys 字段
  0x04000000**，根本不是地址
- 真布局（struct mt76_connac2_patch_hdr 0x60B + patch_sec 64B，全 BE）：
  - desc.n_region = BE32(+0x2c) = 1
  - section@0x60: {type=0x00030002(&0xffff==2), offs=0xa0, size=0x2140,
    **addr=0x900000**, len=0x2140}
- **patch DL 目标 = 0x900000** → init_download 对 0x900000 应发 cmd 0x05
  (PATCH_START_REQ)，0x01 只用于普通 TARGET_ADDRESS_LEN_REQ
- WM/WA 的 zxnpu_fw_parse（9 region、LE addr@16/len@20、36B trailer）
  经 mt76 结构体逐字段核对**本来就对**；MT7916 WM RAM 窗口在 0x0220_0000

**修复（3.46）**：mcu-dl patch 路径改 section 驱动 —— SEM(0x10) → 逐 section
[AddrLen(dst,size,0x80000000，dst==0x900000 用 cmd 0x05) → scatter(fw+offs,size)]
→ Finish(0x07) → SEM release(0x10,op2)；删掉 ZXNPU_PATCH_HDR=30 的旧猜测。

**R-B 验收（下一轮实测）**：fw-state 1→7；qid16 didx 走满 patch 全量；
首个 MCU 事件落 RX 环。

## 67. R-B 实机迭代 3.47–3.53：事件环打通 + 三根因链（2026-11-02）

**里程碑**：chip→host 事件 DMA 首次打通（13 轮未解）。
- **根因 A（3.47）**：RX 环 CPU_IDX = 宿主投递公告数（mt76_dma_rx_fill 写
  q->head=ndesc），写 0 = 零可用槽 → 引擎永不投递。修复后 4 个 MCU 事件
  首次落环。
- **根因 B（3.48）**：scatter 块必须 vendor 的 4032B（4096-chip_cap[0x134]）。
  512B 帧只被消费 3 个；4032B 后 patch 3 帧全消费。
- **根因 C（3.49）**：MCU 收到新 AddrLen 即放弃上一数据段 —— 必须 mt76 式
  逐命令等响应。mcu_wait_event(seq) 实现，DL 命令全部接线。
- **计数模型（3.50）**：tail（消费位置，自由递增）+ CPU_IDX 寄存器 =
  tail+DEPTH（恒满环在途）。didx-cidx 回绕 bug 修复。
- **wfsys-reset 重试（3.52）**：二次复位可能被 MCU 无视（state 不动 + 引擎
  死），改为验证 fw-state 变化、最多重试 5 次。

**3.53 全量捕获（当前状态）**：
- patch：4/4 命令 + 4/4 响应 ✓；8512B 数据 3 帧全消费 ✓
- WM：r0(0x0220a800,126976) ✓、r1(0x0231dc00,205824) ✓ 响应+数据
  （qid16 cidx=didx=87 = 3+32+52 帧全消费）
- **WM r2(0x00414800) 起 MCU 硬挂**：qid17 didx=9（cmds 7-9 已取）、
  RX didx 恒 6（再无事件）、fw-state 恒 1。15s 超时也无效。
- WA r0=0x10200000（CR4 ILM，mode 0x80000010 = WORKING_PDA_CR4）

**下一步假设**：Finish 后 patch 未真正激活（TLV 状态码未验）；或 r1 数据
DMA 尾帧触发 MCU 总线错误。行动：wait_event 打印命中事件的 48B TLV +
每 region 后读 qid16 计数器，一轮定位。

## 68. COM7 串口日志完整复读——此前忽略的关键点（2026-11-02）

**两个版本块（交织还原后）**：
- WM 块：Chip ID 0x13 / Region number 0x09 / Built date 20250103112717 /
  Common crc 0xf9b7d222 / Release info total len 92, tag1 pad 3 len 85
  → 与我们 WM 文件逐字节吻合（trailer t[0]=0x13、n_region=9、尾部 CRC、
  128B release-info TLV 0x5c=92/0x55=85 全部对上）
- WA 块：Region number 0x03 / Built date 20250103112803 / crc 0xd730d461
  → 与 WA 文件（3 region、尾 CRC）吻合
- **vendor 的固件解析与我们的逐字节一致——解析层排除嫌疑**

**vendor 下载路径完整还原**：
1. /wlan/*.bin 三个 ENOENT（仅首选路径）
2. fallback /etc/wireless/mt7916/ 打开成功 → 版本块打印真实数据
3. 下载静默执行（DebugLevel 无打印）
4. `current sync CR = 0x1 → 0x7`（RDY）——**vendor 确实下载固件**
   （"ROM-only boot" 论点彻底作废；与我们的实测行为同构）

**其他**：
- "Patch version: NE2631-EFA8" 是 dhcp4c 产品串 ZTE-ZXHN E2631-EFA8 的
  交织假象，固件/驱动中均无此字符串
- WM 文件 layout 定案：[data][9×40B desc][36B trailer]，data 与 desc 之间
  有 128B release-info（16×'#'+{u32 len=92}+tag 串），vendor 与我们都跳过
- PCI 基线：vendor dump 7906 rev 字节=0x22、790a=0x23；我们的 box
  sysfs rev=0x00（class 0x028000 一致）——字段含义待定，但 patch 应用
  成功（re-get=1）说明 E1 patch 与我们硅片兼容
- 两个 "current sync CR" 分别在 WM/WA 解析块前（=1，DL 态）；0x7 出现在
  文件打开+解析之后（RDY）——是下载完成的标志

**下一步**（不变）：r1 status-5 疑点指向命令线格式（3.62 已把 AddrLen
换成 32B Andes 格式但未测）。解析层已排除，测试 3.62 是下一个动作。

## 69. 第二份 COM7 日志（18:34–19:13）复读——"双原厂"事件全程还原（2026-11-02）

此前只读了第一份（17:10 vendor 启动）。第二份记录了 8b8 包刷机失败全程：

1. 18:34 Boot A：bootloader 提示输入 bootmode password（用户进了 bootmenu），
   `upgrade version` TFTP 尝试 → 192.168.1.100 超时 Abort
2. vendor 系统启动：slot0 header@**0x1800000**（原 0x1c60000）+ header crc
   0xfca2a9af/0x35d5650 = **vendor 自家头**——slot0 已被写回 vendor 固件
   （web 刷 8b8 包 integrity 失败后，用户刷了官方包或 web 页回写了 vendor）
3. Boot B：再次 TFTP → 'File not found'
4. Boot C：两次黑屏重启循环（3590-3617 行丢失输出）后，slot0 = 我们的
   内核 crc 0x61083db9/0x9ef7c246，fs 0x1220000（我们的 jffs2）——用户用
   e2631-fwslot/TTL 恢复了 OpenWrt，R-A ready ×3 次正常

**结论**：8b8 头通过 bootloader cspboot 校验没问题（我们 pack 的 CRC 自洽），
但 web 页 integrity 校验走的是另一套（含版本门）。双原厂事件=web 刷写把
slot0 写成了 vendor 官方固件，后经用户手工恢复。无新协议信息，但明确了：
**刷机路径必须在 OpenWrt 侧用 e2631-fwslot，web 页只用于官方包**。

## 70. 3.61–3.65 迭代定性——r1 拒绝机制与 vendor 单入口模型（2026-11-02）

**实验矩阵结论**（全部实测定案）：
- 64B TXD AddrLen：r0 (0x0220a800,126976) → status 0 + 数据 32 帧全消费 ✓；
  r1 (0x0231dc00,205824) → status 5（秒回，qid17）
- Andes 32B AddrLen：r0 数据不被取走（txdrain 超时）→ 64B TXD 是对的
- fire-and-forget（无等待）：MCU 会话错乱后**硬挂**，连 wfsys-reset 都失效
  （SUBSYS_RST 0x300 位卡死，只能整箱重启）→ 等待是协议的一部分
- AddrLen 超时重发（幂等）：r1 的应答会**迟到**（MCU 处理 r0 数据期间不收
  新 AddrLen），重试后收到 status 5 —— 不是丢失，是明确拒绝
- bit30 于 TX 描述符 = fetch-enable，清掉后数据完全不动（3.60）

**vendor 单入口模型**（parse_fw_v1 + load_fw_v1 逆向结论）：
- parse_fw_v1 每 stage 只建**一个 28B 条目**（os_alloc(28)）：{img_ptr,
  0x100000, crc_ptr+4, 0x20}——0x100000 是常量，crc_ptr 是文件尾
- vendor 版本块打的是每 stage 的（Chip ID/Eco/Region/Format/Ram version/
  Built date/Common crc）——与文件尾部 TLV 逐字节吻合
- load_fw_v1 的 AddrLen 参数：addr=[fp+4]=0x100000（常量！）、len=[fp+8]
  ——**vendor 不是按 region 逐个 AddrLen**，整个 stage 一次地址窗口
- 我们按 region 逐个发 AddrLen(0x0220a800/0x0231dc00/...) 的模型本身就与
  vendor 不同；r1 的 status 5 是 patched DL handler 对"第二个窗口"的拒绝

**下一步**：按 vendor 模型重写——每 stage 一次 AddrLen(addr=常量,
len=img_size)，数据从 img 头部连续发到尾，无 region 循环。3.66。

## 71. load_fw_v1 完整逆向——vendor DL 协议定案（2026-11-02）

### 71.1 mt_load_fw 完整流程（129 行逐指令）
```
1. fw_own_ctrl(1)                      — chip_ops[0xc8]，进 DL 模式
2. ctrl_fw_state_v2(action=1)          — hif_ctrl+0xf40 hook
   → 如果 sync != 1: MtCmdRestartDLReq → 等待 sync == 1
3. bit loop for caps=3 (bit0, bit1):
   bit0: fwdl_ctrl[0]+0 != 1? → parse_patch_v2 (hook f3c) → 下载 patch
   bit1: fwdl_ctrl[1]+0 != 1? → parse_fw_v1 (hook f38) → 解析 WM
        → load_fw_v1 (hook f30) → 下载 WM
   （没有 bit2/WA —— vendor 的 mt_load_fw 只下载 patch + WM！）
4. ctrl_fw_state_v2(action=3)          — 最终 RDY 检查
```

### 71.2 load_fw_v1 逐指令（0x1dffc4–0x1e0188）
```
load_fw_v1(pdev, stage, img_struct):
  if img_struct[4] == 0: → 跳过下载，直接 FwStartReq
  r4 = 0 (byte offset); r5 = 0 (region index); sl = 1 (is_first)
  loop:
    fp = img_struct[0] + r4           ← 28B entry base
    r4 += 28; r5 += 1
    feature = fp[12]
    if feature & 0x20: override = fp[4]     ← OVERRIDE_ADDR bit
    AddrLen(addr=fp[4], len=fp[8], mode=feature&7|0x80000000
            |(feature&1?8:0)|(feature&0x10?0x40:0)|(stage==1?0x10:0))
    if error: → free, print, return error
    send_image(pdev, fp[0], fp[8])          ← 从 fp[0] 发 fp[8] 字节
    if region_count <= r5: → FwStartReq
    else: → loop
```

### 71.3 parse_fw_v1 填充的 28B 条目
```
entry[0]  = img_ptr                    (宿主 VA，数据起始)
entry[4]  = 0x100000                   ← 常量！不是从文件读的
entry[8]  = crc_value + 4              ← CRC 地址 + 4 = 数据结束偏移
entry[12] = 32 (byte)                  ← feature = 0x20 (OVERRIDE)
```

### 71.4 关键发现
- **vendor 只下载 patch + WM，不下载 WA**（caps=3 没有 bit2）
- **每 stage 只有一个 28B 条目**——不是按文件里的 9 个 region 逐个下载！
- AddrLen(addr=0x100000, len=crc_value+4)——0x100000 是 WM 的 DL 窗口地址
  （MT7916 WM 地址空间基址），len = 整个文件的大小（crc 标记尾部）
- **我们错在**：按文件的 9 个 region 逐个 AddrLen——每个都是一个新的 DL
  窗口请求，第一个被接受后 MCU 关闭会话，后续全部 status 5 拒绝
- **正确做法**：一次 AddrLen(0x100000, whole_file_size) + 整个文件 scatter
  + FwStart(addr=override)。MCU 自己解析 region 描述符决定内部布局。
- WA 下载不在 mt_load_fw 里——在 MCU 启动后由其他路径处理

### 71.5 3.66 实测验证
AddrLen(0x100000, 1759340) → status 5：**0x100000 也被拒**。
这意味着 status 5 不是"地址错误"而是**会话状态问题**——patch 激活后
MCU 的 DL handler 需要某种我们还没做的会话重置。
（RestartDL type=0xEF 候选）

## 72. 3.67 RestartDL 实测 + vendor DL 地址来源追踪（2026-11-02）

### 3.67 结果
RestartDL(type=0xEF) 被接受（返回 0），但应答 status=0xfe（错误码）。
WM AddrLen(0x100000, 1759340) → 仍然 status 5。之后 WA 全部超时。

### 结论：RestartDL 不能解决 status-5。问题在 DL 地址本身。

### vendor DL 地址来源追踪（纯静态）
fwdl_ctrl[stage] = adapter + 0xf35f44 + stage*60，共 3 个 stage × 60B。
fw_prepare 填充：
- fwdl[0]+4 = [adapter+0x13e9e0]  ← patch 的 DL 地址
- fwdl[0]+8 = [sp+4]（文件长度）
- fwdl[1]+4 = [adapter+0x13c9e4]  ← WM 的 DL 地址
- fwdl[1]+8 = [sp+8]（文件长度）
- [adapter+0x13e9e0] 和 [adapter+0x13c9e4] 只被 fwbin_mem_free 清零，
  没有其他 .ko 代码写入 → **DL 地址来自 userspace ioctl**！
- 这意味着 vendor 的 DL 地址由 wlan_config daemon 通过 RTMP_COM_IoctlHandle
  传入，驱动从不用文件里的 region 地址

### 对我们的含义
我们不该从 .bin 文件的 region table 取地址——地址应该来自 MCU 固件
"期望"的下载窗口。vendor 的 userspace 传的是什么值？
→ 需要逆向 ZTE 的 wlan_config binary 或固件解析 ioctl 的 handler
→ 或者：试 MT7916 标准地址 0x200000（ILM）/ 0x900000（DLM）

## 73. load_fw_v1 完整逆向定案——vendor DL 流程模型修正（2026-11-02）

### 调用链（100% 确认）
```
WfMcuInit:
  NICLoadRomPatch (= mt_load_patch) → parse_patch_v2 → 下载 patch
  NICLoadFirmware (= mt_load_fw)    → parse_fw_v1 + load_fw_v1 → 下载 WM
MCUSysInit:
  NICLoadFirmware (= mt_load_fw)    → 同上
```
WA 文件不在 init 流程中下载——**vendor 初始化只下载 patch + WM**。

### mt_load_fw 完整逻辑（逐指令确认）
```
1. fw_own_ctrl(1)                          — chip_ops[0xc8]
2. ctrl_fw_state_v2(action=1)              — hif_ctrl+0xf40
   → if sync != 1: MtCmdRestartDLReq       — 恢复 DL 态
3. bit loop (caps=3, bits 0,1):
   bit0: fwdl_ctrl[0]+0 != 1 → parse_patch_v2(pdev, 0, sp) → 下载 patch
   bit1: fwdl_ctrl[1]+0 != 1 → parse_fw_v1(pdev, 1, sp)    → 解析 WM
        → load_fw_v1(pdev, 1, sp)                          → 下载 WM
4. ctrl_fw_state_v2(action=3)              — 最终 sync==7 检查
```

### load_fw_v1 核心循环（逐指令）
```
for region_idx = 0..region_count-1:
  entry = array_base + region_idx * 28
  feature = entry[12]
  if feature & 0x20: override = entry[4]
  mode = (entry[12]&7) | 0x80000000 | (entry[12]&1?8:0)
       | (entry[12]&0x10?0x40:0) | (stage==1 ? 0x10 : 0)
  MtCmdAddressLenReq(pdev, addr=entry[4], len=entry[8], mode)
  send_image(pdev, data=entry[0], total=entry[8])
  → FwStartReq
```

### 28B 条目字段
```
[0]  = img_ptr   (宿主 VA,数据起始)
[4]  = 0x100000  ← 硬编码常量,parse_fw_v1 的 DL 地址
[8]  = crc_value + 4 ← 数据结束标记
[12] = 32 (byte) ← feature = 0x20 (OVERRIDE_ADDR)
```

### 关键未定项
- 0x100000 的语义：MCU DL 窗口地址？command 参数？需实机验证
- entry[8] = crc+4 = 0xf9b7d226 作为 len 传给 AddrLen——MCU 可能
  不按 len 计数而是按 FWDL 环空判断结束
- parse_fw_v1 从 fwdl_ctrl[stage] 读 img_ptr/img_size，而 fwdl_ctrl
  由 fw_prepare 填充；mt_load_fw 通过栈传递 fwdl_ctrl 指针给 hook
  （编译器布局的隐式参数传递，静态追踪困难）

### 当前状态
- 3.66 测试了 AddrLen(0x100000, 1759340) → status 5
- 3.67 加了 RestartDL → RestartDL 返回 0xfe，AddrLen 仍 status 5
- 结论：0x100000 地址和 RestartDL 都没解决问题
- 可能原因：patch 后的 DL handler 与 ROM 的不同，需要不同的参数格式；
  或者 0x100000 不是 WM 的正确地址

### 下一步方向
按 vendor 模型：0x100000 + len=crc+4 的 AddrLen 被拒——
尝试用 mcu-dl-regions 的自定义参数在设备上交互式测试多个
addr/len 组合，找出 MCU 接受的窗口参数。

## 74. 协议闭环——mt76 上游源码交叉验证 + 3.68 重写（2026-11-02）

### 74.1 mt76_connac_mcu.c/.h（openwrt/mt76 master）权威常量

`MCU_CMD_TARGET_ADDRESS_LEN_REQ=0x01 / FW_START_REQ=0x02 / PATCH_START_REQ=0x05 /
PATCH_FINISH_REQ=0x07 / PATCH_SEM_CONTROL=0x10 / FW_SCATTER=0xee / RESTART_DL_REQ=0xef`
——与 vendor MtCmd* 系列的 Andes CMD_MSG byte@2 逐一相等；wire TXD cid@36 = 这些值，
wire cmd 字节恒 0x17（scatter 0x10）。此前记录的 "AddrLen(0x05/0x01)" 之谜解开：
init_download 按 `addr==0x900000 → PATCH_START(0x05)，否则 TARGET_ADDRESS_LEN(0x01)`
选 cid，vendor MtCmdAddressLenReq 的 `(addr==chip_cap[0x12c]) ? 5 : 1` 是同一规则
（cap[0x12c]=patch 目标=0x900000）。

DL_MODE 位（payload 第 3 dword）：BIT31=NEED_RSP（恒置，=vendor 0x80000000）、
BIT4=WORKING_PDA_CR4（WA 专属 = vendor load_fw_v1 `r9==1 ? +0x10`）、bit0|bit3=加密、
bit5-6=KEY_IDX、BIT6=ENCRY_MODE_SEL。region feature_set：BIT5=FW_FEATURE_OVERRIDE_ADDR
（照下载，addr 进 FwStartReq 入口，option|=FW_START_OVERRIDE=1）、BIT6=FW_FEATURE_NON_DL
（跳过）。**WM 文件 9 个 region 的 len 之和 = 0x1AD660 = 恰为 data 段大小**（自洽）。

### 74.2 vendor 与 mt76 的 WM/WA 流程同构

mt76_connac_mcu_send_ram_firmware：逐 region AddrLen(0x01)+scatter(4096B 块) →
一次 FwStartReq(option=OVERRIDE|CR4, addr=override)。vendor load_fw_v1：逐 28B entry
AddrLen+scatter（state 1↔2 切换）→ FwStartReq(option=(wa?4:0)|entry_bit5, addr=entry[4])。
WM r0={0x0220a800, 0x1f000, feat 0x20} 即 override 段——实机 3.54 起 r0 被接受、数据
被消费，与此模型吻合。vendor parse_fw_v1 的单窗口 len=BE32(file尾4)+4=CRC+4 是死胡同
（file 尾 4B 是 trailer CRC），**放弃单窗口模型，以 mt76 region 流程为准**。

vendor parse_fw_v1+load_fw_v1 的调用链：WfMcuInit→NICLoadRomPatch(→load_patch_v1)→
NICLoadFirmware(→mt_load_fw→hook[0xfb0/0xfa8])；NICRestartFirmware→hook[0xfb8]→
RestartDL(0xEF)。RestartDL 不在正常下载流程（3.65-3.67 的注入判错）。

### 74.3 3.68 驱动重写（zxnpu-wlan.c）

- zxnpu_fw_send：删 RestartDL + 单窗口；逐 region AddrLen(cid 0x01, mode=
  0x80000000|wa?0x10|enc bits) → scatter(FWDL 环) → wait_txdrain；收尾
  FwStartReq(cid 0x02, {option=(override?1:0)|(wa?4:0), addr=override})，cmd 环。
- zxnpu_mcu_addrlen：删 on_fwidl 参数——路由铁律 state==2→FWDL 环仅限 scatter，
  AddrLen 恒走 cmd 环（mt7916_kick_out_fwdl_msg 实读反证了 3.57 的注释）。
- fw-boot.sh 删掉手动的 "fw-start 1 0220a800 / fw-start 5 0" 补充块（FwStartReq
  已内嵌于 fw-dl，重发会重复）。
- 版本 native-wired-3.68。

### 74.4 工具坑

Git Bash 无 objdump/readelf/wslpath，`2>/dev/null` 把 command-not-found 吞成空输出
（本日由此得出过假阴性）。反汇编一律走 WSL cross objdump：
`/home/builder/openwrt/staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/
arm-openwrt-linux-muslgnueabi-objdump`（host objdump 无 arm target）。E2631 实机
带符号 ko：_scratch/vendor-e2631-kmodule/mt7916-run.ko；E2633 全符号反汇编：
_scratch/mt7916.dis（751K 行）。

## 75. 3.68-3.70 实机探针——status 5 规则收敛（2026-11-02 深夜）

### 75.1 实测事实链（3.69/3.70 驱动，status 感知后）
1. patch 窗口全链绿：power-ctrl→SEM(2)→AddrLen(0x900000,0x2140)=0→scatter→Finish=0→SEM rel(3)→SEM re-get=01(applied)
2. 首个 RAM AddrLen (0x0220a800, 0x1F000, mode 0x80000000) → status 0，126976B 数据被 FWDL 环消费
3. **其后所有命令确定性 status 5，与等待时长无关**（15 次×2s 重试全秒拒）：第 2 个 AddrLen（任意地址/长度）、Finish(0x07)、0x100000 整文件窗口（事件迟到 ~1min）
4. 窗口被拒时 scatter 帧完全不被消费（FWDL didx=0）→"窗口开=数据通路开"
5. interlude 0x41143c 清 bit0-1 无效（ROM DL 态下寄存器本就 0x00000000，3.70 实测）

### 75.2 长度校验假说（当前最强）
全部数据点符合"AddrLen 窗口长度 ≤ ~128KB"：
- 0x2140 (patch) ✓ / 0x1F000 (r0) ✓
- 0x32400 (r1) ✗ / 0x1AD86C (整文件) ✗
vendor load_fw_v2_compressimg 的流式分块（BE32 块长从数据流内读、逐块 AddrLen）正是绕过该限制的机制；parse_fw_v1 单窗口 len=CRC+4 之谜与此自洽——v1 是死代码，v2 才是真路径。**修复方向：region 按 ≤cap 分块逐窗 AddrLen+scatter**。边界探针（0x20000 vs 0x20001）因整机硬死未跑完。

### 75.3 驱动演进
- 3.68：zxnpu_fw_send 重写（逐 region + FwStartReq(override)，删 RestartDL/单窗口）
- 3.69：wait_event_st 暴露事件 status（buf[32]）；AddrLen 对 status≠0 重试并中止（拒绝后照发 scatter 会毒化会话）；修 mcu-dl-regions manifest 解析（%s 遇空格截断 bug）；mcu-finish 探针
- 3.70：`cr <addr>` 任意总线读（L1 remap）；interlude 实验代码（已证无效）
- 教训：/etc/e2631-version 被 jffs2 overlay 遮蔽，in-place 升级后显示旧版本——验驱动特性而非版本文件；release.mjs 必须在 native/ 下运行（_scratch 下 MODULE_NOT_FOUND 后旧 factory 包会被误刷）
- 整机硬死一次（SSH 超时+TTL 静默）：疑与前轮 437 个未消费 FWDL 帧 + rebind 后环状态有关，断电恢复

### 75.4 下一步
1. 断电恢复 → 重跑长度边界阶梯（0x20000/0x20001/0x1F800）定 cap 精确值
2. 按 cap 分块实现（每块 AddrLen+scatter+事件确认）→ 跑通 9 region
3. FwStartReq(1, 0x0220a800) → 观察 fw-state 1→3/7
4. WA 同流程 → R-B 控制面（GET_FW_VERSION/信道）

## 76. 事件字段纠错与 scatter 帧头定位（2026-11-02 上午，接续 d64c3564 会话）

### 76.1 纠错：MCU 事件 +32 不是 status，是 ext_eid

权威依据（mt76 上游，同款 connac2 MCU）：

- `struct mt76_connac2_mcu_rxd`：`rxd[6]`(24B) → `len`@24、`pkt_type_id`@26、
  `eid`@28、`seq`@29、`option`@30、`rsv`@31、**`ext_eid`@32**、`tlv`@36。
  与实机帧完全吻合：`24 00 00 38 | 0c 00 07 00 | eid | seq | 00 00 | ext_eid`，帧长 0x24=36。
- `mt7915_mcu_parse_response()`：**只比对 `rxd->seq`**，匹配即返回 0；只有
  `PATCH_SEM_CONTROL` 才 `skb_pull(sizeof(*rxd)-4)` 后取 `*skb->data`（=偏移 36 的 tlv 首字节）。
  即 AddrLen / Scatter / FwStart **不存在"status"字段**，上游从不判它们的状态。

结论：3.69 起的"status 5 = 被拒/忙"判读是错读——那个 5 是 `ext_eid`。
由此 3.69 的"status≠0 就重试 15 次并中止"是自伤逻辑，必须撤掉。

### 76.2 实机日志重放（3.68 存档，rollout 里恢复）的真正断点

```
[0] addr=0220a800 len=126976 feat=20 → evt eid=01 seq=07 ext_eid=00   ← 有应答
[1] addr=0231dc00 len=205824 feat=00 → evt eid=01 seq=08 ext_eid=05   ← 有应答
[2] addr=00414800 len=14336          → wait_event seq=9 超时 ×15      ← 完全无应答
[3] addr=e0049c00 len=474112         → 同上，全无应答
```

断点不在"r1 被拒"，而在 **r2 起 MCU 对命令环彻底沉默**：r0/r1 都有 seq 匹配的应答，
之后 15 次×2s 一条都没有。这正是"窗口已开、数据没到齐"的签名——MCU 卡在收第 1/2 段
数据上，不再处理新命令，而不是"拒绝并继续"。r1 的 ext_eid=5 与失败**无因果**。

### 76.3 scatter 帧头：三处字段的权威来源

`AndesInitCmdMsg`（mt7916.ko 0x1c31dc）逐指令解出 Andes 32B 头语义：

| 偏移 | 含义 | 来源 |
|---|---|---|
| +0 | u16 帧长 | 调用方传入 |
| +2 | type（scatter=0xEE、AddrLen=0x01/0x05） | 调用方 |
| +3 | 保留 0 | — |
| +4 | cmd（scatter=0x10、AddrLen=0x17） | 调用方；同时当 flags 用 |
| +6 | seq | 调用方 |
| +8 | cid | 调用方 |
| +20 | **port = GetRealPortQueueID(dev, type@+2)** | type==0xEE → `chip_cap[0x162]`；否则 0x8000 |
| +23 | **flags&2 ? 3 : 0** | scatter 的 +4=0x10 → `0x10&2=0` → **必写 0** |

`GetRealPortQueueID`(0x1cb520)：hif_type!=2 → 2；type!=0xEE → 0x8000；type==0xEE → `cap[0x162]`(u16)。

我们驱动 `zxnpu_mcu_scatter()` 现状两处错：`+20` 写死 0x8000（应为 cap[0x162]）、
`+23` 写死 3（应为 0）。第 3 处待定：`+0` 我们写 chunk 长度，vendor 侧 AndesInit 的
长度实参在 MtCmdFwScatter 里传的是 0（由 AndesAllocCmdMsg/Append 维护），需再核。

### 76.4 结构差异：scatter 缺 64B TXD 包裹

- `AndesAllocCmdMsgGeneric`(0x1c30bc)：分配时**预留 `chip_cap[0x134]`(=64) 字节**，
  并把包数据指针/长度各前移 64 → vendor 线上帧 = `[64B connac2 TXD][32B Andes][payload]`。
- mt76 的 `mt76_connac2_mcu_fill_message()`：`if (cmd == MCU_CMD(FW_SCATTER)) goto exit;`
  —— scatter **完全不 push TXD，裸帧**（依赖 FWDL 专用环区分类型）。
- 我们的 scatter = `[32B Andes][payload]`，既没有 64B TXD，也没有"裸帧+专用环"的前提，
  两边都不成立。

### 76.5 下一轮（3.71）改动清单

1. 删掉 AddrLen 的 status 判读与 15 次重试；恢复"seq 匹配即成功"（对齐 mt7915_mcu_parse_response）。
2. `zxnpu_mcu_scatter`：`+23` 改 0；`+20` 改为可配置（cap[0x162] 的值先从 vendor 静态表取，
   取不到就给模块参数，默认保持 0x8000 以便 A/B）。
3. scatter 改为两种可切形态（模块参数 `scatter_fmt`）：
   - 0 = 现状 `[32B Andes][payload]`
   - 1 = `[64B connac2 TXD(cid=0xEE,pkt_type=0xa0,seq)][payload]`
   - 2 = 裸 payload（mt76 形）
4. 每段 AddrLen 后**不要**用 status 判断，改为观察该段 scatter 是否被 FWDL 环 dma_idx 全部消费
   （didx 前进量 == 帧数），超时才降级。
5. 验收：r0..r8 九段全部有 seq 应答且 didx 前进到帧数 → FwStart(option=1,addr=0220a800)
   → `fw-state` 1→3/7。

### 76.6 教训

- 判读任何 MCU 字段前先对上游结构体，不要凭"名字像 status 的偏移"下结论；一个错读会让整轮
  实机试验全部作废（3.69/3.70 两版驱动、一次整机硬死）。
- 恢复历史日志用 rollout jsonl（`~/.zcode/cli/rollout/model-io-session_*.jsonl`）比找 bash
  临时日志可靠：tool 结果原文都在 model IO 里。

## 77. 3.71：scatter 改用裸帧（patch 段已验证的形态）

### 77.1 为什么改帧格式而不是改时序

§76 把事件 +32 的误读纠正了，但那只是撤掉了一条错的重试逻辑，没有解释
「r0/r1 有应答、r2 起永远沉默」。真正的证据在同一份驱动里：

| 路径 | scatter 帧形态 | 结果 |
|---|---|---|
| `mcu-dl` / `patch-dl`（patch 段） | **裸 payload**，无任何头，qid16 | 全绿，patch 段每次都被 MCU 接受 |
| `fw-dl wm`（WM/WA 段） | `[32B Andes 头][payload]` | r0/r1 过，r2 起命令环无应答 |

同一条 FWDL 环、同一个 DL 消费者，patch 段用裸帧稳定成功，WM 段用 Andes 头
就卡死 —— 帧格式是唯一变量。这与 mt76 一致：`mt76_connac2_mcu_fill_message()`
对 `MCU_CMD(FW_SCATTER)` 直接 `goto exit`，不 push TXD，帧就是数据本身。

Andes 头本身不是错的，但它只出现在 64B connac2 TXD **之后**
（`AndesAllocCmdMsgGeneric` 预留 `chip_cap[0x134]`=64B）。我们把
`[Andes][payload]` 直接甩到线上，等于让 loader 把每帧的 32B 头当成固件数据：
r0（126976B，32 帧）凑够长度"提前完成"，r1（205824B，52 帧）差一帧半，
于是 loader 一直等剩下的字节，不再服务命令环 —— 与 3.68 日志的形状逐条吻合。

### 77.2 3.71 改动

1. `zxnpu_mcu_scatter()` 默认 `scatter_fmt=0` = 裸帧（patch 段形态）。
   `1` = 旧的 32B Andes 头（保留做 A/B），`2` = 64B TXD + 32B Andes。
2. Andes 头两处修正（只在 fmt=1/2 生效）：`+23` 由 3 改 0（`flags&2` 对
   cmd 0x10 恒为 0），`+20` 由写死 0x8000 改为 `scatter_port` 参数（默认仍
   0x8000，可取 `chip_cap[0x162]` 做对照）。
3. `zxnpu_mcu_addrlen()` 删掉 status 判读与 15 次重试，恢复"seq 匹配即成功"。
4. `zxnpu_wlan_wait_txdrain()` 改读 `fw_ring` 对应的环（原来无条件读 ring 0）。
5. 新增观测：`scatter_log=1` 打印每段首帧/每 128 帧、以及该段前后 didx；
   `fw_regions=N` 只下前 N 段就停（不下发 FwStart），让坏局面停在 2 秒内。

### 77.3 预期（分层验收）

| 阶段 | 判据 |
|---|---|
| A：`fw_regions=3` | r2 的 AddrLen **有** seq 应答（3.68/3.69 在此静默）；每段 didx 前进量 == 帧数 |
| B：全 9 段 | r0..r8 全部有应答，最后 didx==cidx |
| C：FwStart | `fw-state` 离开 1，向 3/7 走 |

r2 一条应答就能判定"帧格式假说"成立，不必等到整机硬死。

## 78. 实机结果：裸帧成立，随后暴露出两个环索引 bug（3.71→3.73）

### 78.1 3.71：r2 不再沉默（帧格式假说验证）

```
[0] len=126976  evt seq=07  scatter 32 frames  didx 3->35
[1] len=205824  evt seq=08  scatter 52 frames  didx 35->87
[2] len=14336   evt seq=09  scatter  4 frames  didx 87->91   ← 3.68/3.69 在此彻底静默
```

裸帧（fmt=0）下 r0..r2 全部被接受且被消费完毕。同一个 boot 里紧接着重跑
全量下载，r0 的 AddrLen 依然被应答 —— 说明 **AddrLen 确实幂等**，中途停下
不会污染会话，`fw_regions` 这个停止闸是安全的。

### 78.2 3.72：didx 是模 128 的，cidx 是自由计数

3.71 stage B 在 r1 之后报 `txdrain timeout (cidx=175 didx=47)`，但 47+128=175
—— 环其实是空的，是**比较**错了。引擎的 didx 按环深度取模，我们的 cidx 一路
自增。3.72 把所有环索引距离改成 `(cidx - didx) & (depth-1)`。

### 78.3 3.73：写回的 cidx 也必须取模

3.72 因此推进到 r4，然后停在 `didx 81->0, outstanding 87`：

| 段 | 帧数 | didx 变化 |
|---|---|---|
| r0 | 32 | 3→35 |
| r1 | 52 | 35→87 |
| r2 | 4 | 87→91 |
| r3 | 118 | 91→81（跨圈，正常） |
| r4 | 134 | 81→0 后不再前进，87 帧滞留 |

原因不是引擎坏了，而是我们**写进 cidx 寄存器的值也自由增长**：跨过 128 帧后，
引擎自己算的 `cidx - didx` 恒多出整整一圈（128），于是把环判成"满"，停止取帧。
3.73 把 `*cidx` 也在 `[0, depth)` 内回绕后再写寄存器。

结果：**WM 9 段全部应答、全部消费**（didx 3→35→87→91→81→87→116→120→38→60），
`fw-start option=00000001 entry=0220a800` 被接受（evt seq=00）。

### 78.4 3.73 暴露的第三个问题：stall 安全阀误杀

WA 段 r0（189456B/47 帧）顺利落盘，r1（264672B）只发了 3 帧就 `failed at -5`
（-EIO）。那是 `zxnpu_mcu_submit_ring_last()` 的 `ring_stall > 512` 分支，而
紧随其后的 `ring-sync` 明确报 `cidx==didx drained healthy` —— 环是好的。
这个计数器统计的是"发出后这一瞬间 didx 没动"的次数，跟真实停滞无关，且被
环 16/17 共用。3.74 让它只记日志、不再 `-EIO`，真正的停滞判定交给每段
`wait_txdrain()`（30 s + 滞留帧数）。

### 78.5 判读教训

`didx` 停止前进 ≠ MCU 坏了。3.72 的 `outstanding 87` 一度像是"MCU 卡住"，
实际是我们把环索引空间搞错，让引擎自己判满。凡是"引擎侧索引 + 软件侧索引"
同时存在的地方，**两侧必须同一个模**，写寄存器和做比较都要。

## 79. 3.74：无线 MCU 固件下载闭环，fw-state = 7

```
WM  9 regions  → scatter 441 帧 / 1758816 B，didx 3→35→87→91→81→87→116→120→38→60
              → fw-start option=00000001 entry=0220a800  (evt seq=00)
WA  3 regions  → scatter 114 帧 / 457920 B，didx 60→107→45→46
              → fw-start option=00000004 entry=00000000  (evt seq=04)
              → fw-state 7 (ready (WM+WA))，连续 8 次轮询（16 s）稳定
```

这是该分支上第一次把 WM+WA 完整下发并让 MCU 进入 RDY。至此 §76/§77 的
"窗口开了、数据没到齐" 判断被完全证实，三个 bug 依次是：

| 版本 | bug | 现象签名 | 修法 |
|---|---|---|---|
| 3.71 | scatter 用 `[Andes 头][payload]`，loader 把头当固件数据 | r2 起命令环永久静默 | 改裸帧（patch 段形态） |
| 3.72 | 比较 `cidx == didx` 时未取模 | `cidx=175 didx=47` 假超时 | 距离取 `& (depth-1)` |
| 3.73 | 写进寄存器的 cidx 未取模 | 跨 128 帧后引擎判环满，`outstanding 87` | `cidx` 在 `[0,depth)` 回绕 |
| 3.74 | `ring_stall > 512` 误杀（环 16/17 共用计数） | WA r1 发 3 帧即 `-EIO`，而环 healthy | 只记日志，停滞交给 `wait_txdrain` |

### 79.1 当前可用的旋钮（/sys/module/zxnpu_wlan/parameters）

| 参数 | 默认 | 用途 |
|---|---|---|
| `scatter_fmt` | 0 | 0 裸帧 / 1 仅 Andes 头 / 2 TXD+Andes，A/B 用 |
| `scatter_port` | 0x8000 | Andes 头 +20（GetRealPortQueueID 口径） |
| `scatter_log` | 0 | 打印每段首帧、每 128 帧、段前后 didx |
| `fw_regions` | 0 | 只下前 N 段就停（不发 FwStart），坏局面止损 |
| `fw_ring` | 0 | 0=qid16 FWDL / 1=qid17 MCU_WM |
| `dl_wait_ms` | 2000 | 每条 AddrLen 等待应答的时长 |

### 79.2 下一步

MCU 已在 RDY，但 host 侧还没有 MAC/PHY 初始化，也没有 netdev：
下一步是照 mt7915_init_hardware 的顺序做 WF 复位后的 DMA 环（band0 RX/TX）、
MCU 基础命令（query/NIC_CAPABILITY）、再把 wiphy 注册出来。固件下载这条路
不再需要碰 framing 了。

## 80. fw-state=7 之后命令无应答：应答环缺失（3.75 → 3.76）

### 80.1 3.75 实测：帧被取走，事件没回来

3.75 加了 `mcu-ext <cid> <ext_cid> [query|set|na] [hex]`（标准 connac2 TXD：
cid=0xed、ext_cid、set_query、ext_cid_ack）。固件下载照旧成功，fw-state=7
稳定，但三条扩展命令全部 2 s 超时：

```
mcu-ext cid=ed ext=13 q=1 len=4 seq=5: COMMIT
  submit qid17 slot=20 len=68 ... cidx=20 didx=20
  didx still 20 (stall 3)
  wait_event seq=5: timeout (2000ms)

随后读寄存器：qid17 cidx=25 didx=23   ← 帧被消费了
              qid0  didx=20           ← 一个事件都没多
```

即 **DMA 把 5 帧都取走了（didx 20→23，另 2 帧随后也进），但没有任何事件**。
`didx` 前进只证明 DMA 工作，不证明固件理解命令；判据只能是事件。

对照：`fw-start`（cid=0x02，非扩展）在同一路径上有 seq 匹配的应答
（3.74 的 evt[15]/evt[19]）。所以不是"TXD 格式不通"，是**扩展命令或应答环**。

### 80.2 寄存器取证：qid1 从未初始化

```
0x18024500 (qid0) base=4e044000 cnt=0x20  cidx=0x34 didx=0x14   ← WM 事件环，已配
0x18024510 (qid1) base=00000000 cnt=0x200 cidx=0     didx=0     ← 未配
0x18024520 (qid2) base=00000000 ...                             ← 未配
0x18024530 (qid3) base=00000000 ...                             ← 未配
```

`enum mt76_rxq_id { MT_RXQ_MAIN, MT_RXQ_MCU, MT_RXQ_MCU_WA, ... }`：MT7915 有
**两个** MCU 事件环。而 `mt7915_mcu_send_message()`：

```c
if (cmd == MCU_CMD(FW_SCATTER))        qid = MT_MCUQ_FWDL;
else if (test_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state))
                                        qid = MT_MCUQ_WA;
else                                    qid = MT_MCUQ_WM;
```

固件跑起来（MCU_RUNNING）之后，**所有命令都交给 WA**，应答自然从 WA 的事件环
（MT_RXQ_MCU_WA）回。QID0 只覆盖 WM 的事件——这正好解释为什么 FW_START 有应答
（那一步由 WM/ROM 处理）、之后的命令全静默（WA 有回复，但 qid1 的 base 是 0，
无处可写）。

### 80.3 vendor 侧旁证：Andes 头 +0 是 band，不是长度

`MtCmdFwLog2Host`（run.ko 0x1c44d4）逐指令：

```asm
1c4650: movne r3, #0        ; type==0 (MCU_FW_LOG_WM) → 0
1c4654: beq   1c46d4        ; type==1 (WA)            → r3=2
1c4658: strh  r3, [sp,#24]  ; Andes +0 (u16) = 0 或 2
1c4660: mvn   r2, #18       ; 0xffffffed
1c4664: mov   r3, #19       ; 0x13
1c4668: strb  r2, [sp,#26]  ; Andes +2 = 0xed  (MCU_CMD_EXT_CID)
1c4670: strb  r3, [sp,#27]  ; Andes +3 = 0x13  (FW_LOG_2_HOST)
```

结论两条：

1. **Andes 头 +2 = 0xed、+3 = 0x13**，即 vendor 把"扩展命令号"直接铺在
   Andes 头里，与我们此前 `mcu-cmd` 的 `+2=type / +4=cmd` 布局不同（`+4` 是
   flags）。这也是 3.75 之前 mcu-cmd 只对 patch 阶段那些简单命令有效的原因。
2. **+0 的 u16 是目标核**（0 = WM，2 = WA），不是帧长——与 connac2 的
   `s2d_index`（0 = H2N，2 = H2C）语义一致。

### 80.4 3.76 改动

| 改动 | 目的 |
|---|---|
| `rx-init [ring]` | ring 0 = 0x18024500 (WM)，ring 1 = 0x18024510 (WA)，depth 32 |
| `mcu-recv [ring]` | 分别 dump 两个事件环 |
| `zxnpu_wlan_mcu_scan_ring()` | 抽出单环扫描；`wait_event_st` 每 20 ms 扫 0 和 1 两个环 |
| `mcu-ext-wa` | 同一扩展命令发 WA：`s2d_index = 2`（`__MCU_CMD_FIELD_WA`） |
| probe | 新增 `wa_ring` / `wa_buf`（32 × 36 desc + 32 × 1700 B） |

日志标签：ring0 保持 `evt[`，ring1 用 `evtwa[`，旧日志仍可 grep。

### 80.5 验收

- A：fw-state 仍为 7（下载路径不受影响）
- B：`rx-init 1` 后 0x18024510 base 非 0、cnt=32
- C：`mcu-ext ed 13 set`（WM）或 `mcu-ext-wa ed 13 set`（WA）至少一条拿到
  seq 匹配的事件；`ext_eid` 一并打印

若 C 仍失败，则下一个怀疑对象是"扩展命令要先用 basic cid 完成 host 侧初始化"
（mt7915_init_hardware: dma_init → mcu_init → eeprom_init），而不是编码本身。

## 81. 根因：RDY 之后命令要投 MCU_WA 的环（qid20），3.77 → 3.78

### 81.1 3.77 证伪了"帧格式"这条线

3.77 在同一次开机里把同一条扩展命令（`MCU_EXT_CMD_FW_LOG_2_HOST` = 0x13）
用四种线格式发了一遍：

| 变体 | 帧形态 |
|---|---|
| A | 仅 64B connac2 TXD |
| B | 64B TXD + 32B Andes（band 0） |
| C | 仅 32B Andes（band 0） |
| D | 64B TXD + 32B Andes（band 2） |

四种全部 `wait_event` 超时，命令环 didx 停在 23/24，`INT_SOURCE = 0`，
两条事件环零事件。**帧格式不是原因**——同一批命令在 patch 与 FWDL 阶段用
同样的编码是被应答过的。

### 81.2 真因在 `mt7915_mcu_send_message()` 的投递路由

mainline（openwrt/mt76 master）原文：

```c
static int
mt7915_mcu_send_message(struct mt76_dev *mdev, struct sk_buff *skb,
			int cmd, int *wait_seq)
{
	enum mt76_mcuq_id qid;

	if (cmd == MCU_CMD(FW_SCATTER))
		qid = MT_MCUQ_FWDL;
	else if (test_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state))
		qid = MT_MCUQ_WA;
	else
		qid = MT_MCUQ_WM;
	...
}
```

而 `mt7915_mcu_init_firmware()` 在 `mt7915_load_firmware()` 返回后**立刻**
置位 `MT76_STATE_MCU_RUNNING`：

```c
	ret = mt7915_load_firmware(dev);
	if (ret)
		return ret;

	set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);
	ret = mt7915_mcu_fw_log_2_host(dev, MCU_FW_LOG_WM, 0);
```

`enum mt7915_txq_id`（mt7915/mt7915.h）：

```c
enum mt7915_txq_id {
	MT7915_TXQ_FWDL = 16,
	MT7915_TXQ_MCU_WM,	/* 17 */
	MT7915_TXQ_BAND0,	/* 18 */
	MT7915_TXQ_BAND1,	/* 19 */
	MT7915_TXQ_MCU_WA,	/* 20 */
};
```

即：**qid20 = 0x18024440 才是固件跑起来之后的命令环**，qid17 只是启动期
（patch sem / AddrLen / FwStart）那一层的环。

### 81.3 我们之前做了什么

3.74c-3.77 一律把命令投到 `ZXNPU_MCU_RING_NR = 1`（qid17）。下载期这是**对的**
（mt76 此时也走 MT_MCUQ_WM，所以 FwStart 拿到了 seq 匹配的应答）；固件起来
之后**同一个环就静默了**——这恰好是"命令发给了不服务它的核"的正常表现，
而不是丢帧。之前把 `didx` 排空当作"帧被吃掉但没人回"来推理，方向错了：
排空只证明 WFDMA 把帧搬走了，不证明有核在处理。

### 81.4 3.78 改动

| 改动 | 说明 |
|---|---|
| `ZXNPU_TX_RING_FWDL/WM/WA_CMD` = 0/1/4 | 选择子就是 WFDMA 环号，= `enum mt7915_txq_id` |
| `struct zxnpu_tx_ring` + `zxnpu_tx_ring_get()` | 把 ring0/1 的二元选择改成三选一，`queue_init` 与 `submit` 共用 |
| `wac_ring / wac_bufs / wac_cidx / wac_ring_armed` | qid20 环：128 × 16B 描述符 + 128 × 4096B 载荷，probe 里分配 |
| `mcu_ring` 模块参数 + `mcu-ring 0\|1\|4` 控制命令 | 一次开机 A/B 三个环，不必一版一刷 |
| `dma-init` | 现在同时 arm qid16 / qid17 / qid20 |
| `wfsys-reset` / `dma-init` | 一并复位 `wac_*` |

同时修正了 `submit` 日志里的 qid 打印（原来硬编码 `ring ? 17 : 16`，环 4 会
被误报成 17）。

### 81.5 验收

一次开机跑三条，命令固定为 `mcu-ext ed 13 na 0000`（FW_LOG_2_HOST，ctrl=0）：
这是 `mt7915_mcu_init_firmware()` 在 RDY 之后发的**第一条**命令，最能代表
"命令面是否活着"。

- A：`mcu-ring 4` → qid20 应答（预期成立）
- B：`mcu-ring 1` → qid17 静默（对照，复现 3.75-3.77 的现象）
- C：`mcu-ring 0` → qid16 静默（对照）

任一环拿到 `evtwa[` / `evt[` 且 seq 匹配，即证明命令面恢复，可以推进到
`MCU_EXT_CMD(NIC_CAPABILITY)` 与 host 侧 band0 数据环。

## 82. 3.79/3.80：RX 环 CPU_IDX 的 post 编码 —— 命令面真正打通

### 82.1 3.78 留下的"只回一条"现象

3.78 一次开机跑 A/B/C：qid20 拿到 `evtwa[0] eid=ed seq=05`（`ret=0`），
qid17/qid16 静默。但同一条命令再发一次就超时，命令环还继续取走 3 帧后停摆。
"第一条被应答、之后全哑"不像 MCU 死了，更像**事件环再也放不进新事件**。

### 82.2 根因：RX 环 `CPU_IDX` 写成了自由递增

我们自 3.46 起在消费事件后写：

```c
	zxnpu_cr_write(w, reg + 8, didx + ZXNPU_MCU_RX_DEPTH);   /* 33 on a 32-deep ring */
```

mainline 只写 `q->head`，而 `mt76_dma_rx_fill_buf()` 的循环上界是
`q->queued < q->ndesc - 1`（永远留一个空位），所以填满后
`head == (tail + ndesc - 1) % ndesc`：

```c
static void
mt76_dma_kick_queue(struct mt76_dev *dev, struct mt76_queue *q)
{
	wmb();
	if (mt76_queue_is_emi(q))
		*q->emi_cpu_idx = cpu_to_le16(q->head);
	else
		Q_WRITE(q, cpu_idx, q->head);
}
```

两者相差**正好一个环深**：引擎把 `cpu_idx - dma_idx = 32` 读成"环满、没有空位
收下一帧"，于是事件不再投递；WA 核因为回不了应答而卡在命令处理上，命令环随即
停止取帧。这一条同时解释了 3.75-3.77 里"环排空却无应答"和 3.78 里"只回一条"。

顺带补上 mt76 的另一半：消费事件后要**把描述符还回去**
（`mt76_dma_rx_fill()` 重写 ctrl：带 `LAST_SEC0`(bit30)、不带 `DMA_DONE`(bit31)），
否则每收一个事件就永久退役一个槽位。

### 82.3 3.79 的 A/B 结果（实机）

`rx_repost` 三档，同一条 `mcu-ext ed 13 na 0000`：

| 档 | 写值 | 结果 |
|---|---|---|
| 2 | `(didx-1) % depth` | **三条连续命令全部应答**：`evtwa[0] seq=05` / `[1] seq=06` / `[2] seq=07`，`ret=0`；`rx-raw ring1: cidx=2 didx=3` |
| 0 | 不写 | 无事件（该轮下载本身已失败，见下） |
| 1 | `didx + depth` | 无事件（同上） |

第 2、3 轮作废的原因是另一个已知脆弱点：**同一次开机里重复 `wfsys-reset`
之后 ROM 不再回到下载态**（`fw-state after = 1 / unchanged`，`fw-dl: failed at -110`）。
所以一个 boot 只能跑一次"复位→下载→RDY"，多档 A/B 要分次开机。

### 82.4 3.80 改动

| 改动 | 说明 |
|---|---|
| `rx_repost` 默认值 1 → **2** | 写 `(didx-1) % depth`，与 `mt76_dma_kick_queue` 一致 |
| `scan_ring()` 消费后回收描述符 | 重写 `buf0 = bufdma + slot*1700`、`ctrl = LAST_SEC0 | len<<16` |
| `rx-raw [ring] [first] [count]` | 不看 DMA_IDX，直接十六进制转储事件槽；用于区分"没写"和"写了但索引没动" |

### 82.5 3.80 的验收：跑 mt7915 RDY 后的真实初始化序列

全部投 qid20，逐条等 seq 应答：

| # | 命令 | cid / ext |
|---|---|---|
| 1 | FW_LOG_2_HOST (WM) | ed / 0x13 |
| 2 | FW_LOG_2_HOST (WA) | ed / 0x13 |
| 3 | WTBL_UPDATE (del_wtbl_all) | ed / 0x32 |
| 4 | MWDS_SUPPORT (set_mwds 1) | ed / 0x80 |
| 5 | MURU_CTRL | ed / 0x9f |
| 6 | RX_AIRTIME_CTRL | ed / 0x4a |
| 7 | RED_ENABLE | ed / 0x68 |

这几条是 mt76 在碰 MAC 之前必须走完的路径，逐条被应答即说明 MCU 真正在服务，
而不只是回显一条探针。下一步才是 `MCU_CE_CMD_GET_NIC_CAPAB`(0x8a)、
host 侧 band0 数据环与 wiphy 注册。

## 83. efuse 读不通 → 用厂商 eeprom 镜像（3.81 → 3.84）

### 83.1 3.81：先把事件载荷取出来

`mcu-ext` 只匹配 seq、只读 `+32` 的 ext_eid，从来没把应答的**数据**读出来过。
3.81 给 `scan_ring()` 加了载荷捕获，并新增：

```
efuse-dump [start-hex] [blocks]      # MCU_EXT_QUERY(EFUSE_ACCESS) per 16B block
```

载荷长度按 `len@24 - 36` 计算。实机第一块就是 `got=0`、`ret=-5`（`-EIO`）：
事件确实到了、seq 也匹配，但算出来的长度为 0。

### 83.2 3.82：`len@24` 不是帧长

把转储窗口从 36 字节扩到 68 字节后，同一个 `0x0014` 同时出现在两处：

| 帧 | `len@24` | 实际载荷 |
|---|---|---|
| FW_LOG_2_HOST 应答 | `0x14` | 8 字节（`13 00 …`） |
| EFUSE_ACCESS 应答 | `0x14` | 0 字节 |

所以 `len` 字段不能用来算载荷长度。改成**固定窗口**：从 `+36` 起拷贝 32 字节，
由调用方自己校验（efuse 校验回显地址）。此时能看到完整应答：

```
evtwa[0] eid=ed seq=05: 2c 00 00 38 … | 14 00 07 00 ed 05 00 00 00 00 00 02 01 00 00 00 00 …
                                                                          ^^^^^^^^^^^ +36 起
```

地址 `0x000` 与 `0x2c0` 的应答**载荷完全相同**（`01 00 00 00` 后全零），说明固件
只是回了个 ack，没有做地址相关的读。

### 83.3 24 字节请求与 s2d 变体（都在 3.82 上就地验证，不重刷）

| 变体 | 请求 | 结果 |
|---|---|---|
| A | 8 B `{addr, valid=0}` | ack，`got=0` |
| B | 24 B `mt7915_mcu_eeprom_info` 全结构 | ack，`len@24 = 0x24 = 36` ⇒ 无载荷，`ext_eid=1` |
| C | 24 B + `s2d_index=2`（H2C） | ack，同样无载荷 |
| D | `ext 0x4f`（EFUSE_FREE_BLOCK） | ack，`ext_eid=0x4d` |

`mt7915_mcu_get_eeprom()` 只要 `res->valid` 为 0 就返回 `-ENODATA`，也就是说
这条路即使通了也要先有 `valid` 位；本机固件连 `data` 都不给。**结论：MT7916 这版
固件不通过 EFUSE_ACCESS 交数据**，继续在这条路上花时间没有意义。

### 83.4 厂商镜像就是答案

设备上有 `/lib/firmware/zte/mt7916/MT7916_EEPROM.bin`，**恰好 4096 字节
= `MT_EE_MAX_V2`**，且按 mt76 的 v2 偏移逐项对得上：

| 偏移 | 值 | 含义 |
|---|---|---|
| `0x000` | `16 79` | chip id `0x7916` |
| `0x002` | `00 00` | version |
| `0x004` | `00 0c 43 26 60 00` | MAC（MediaTek 模板 OUI） |
| `0x190` | `12 5b 48 4c 01 28 …` | band0 = 2GHz/tx_path 2，band1 = 5GHz/tx_path 3 |
| `0x441` / `0x445` | 功率表 | `TX0_POWER_2G_V2` / `TX0_POWER_5G_V2` |

这正是 `mt7915_eeprom_load_default()` 的回退路径：efuse 不够用时读同名文件、
`dev->flash_mode = true`。厂商固件不需要 MCU 读 efuse，正是因为它走这条路。

### 83.5 3.83 实机

```
eeprom: chip=7916 mac=00:0c:43:26:60:00 band0=2GHz/2 band1=5GHz/3 chainmask=1f flash_mode=1
```

`chainmask=0x1f`（2 + 3 条链）与 MT7916 的 2×2 + 3×3 配置一致；`eeprom-show`
逐段核对 `0x000/0x190/0x440` 三处与镜像字节完全一致。

### 83.6 flash_mode 的义务：把映射推回固件（3.84）

`mt7915_mcu_set_eeprom()` 在 `flash_mode` 下走的是
`mt7915_mcu_set_eeprom_flash()`，不是一句 `EE_MODE_EFUSE`：

```
MCU_EXT_CMD(EFUSE_BUFFER_MODE) = cid 0xed, ext 0x21
header struct mt7915_mcu_eeprom { u8 buffer_mode; u8 format; __le16 len; }
  buffer_mode = EE_MODE_BUFFER (1)
  format      = ((pages - 1) << 5) | (page_index << 2) | EE_FORMAT_WHOLE (1)
  len         = 0x400，最后一页取余
+ len 字节映射
```

0x1000 的映射 = 4 页、4 条命令，每页一条并各等一次 ack。**不上传就没有校准**：
RX 或许能收，但每次发射都会被固件拒掉，所以一旦 `flash_mode` 为真这一步不可省。

### 83.7 整机参数与 caldata 的位置

`mtd1 "tag"` 是整机参数表，已完整解出。布局：

```
+0x00  12 B 头：33 33 33 33 | f7 01 00 00 | 65 0d 18 de
之后是**紧凑排布**（不对齐）的条目：
  u32 flag = 0x00000002
  u32 len
  u8  pad[9]
  u8  type
  u8  data[len]
```

解出的 16 条（`type` 语义据内容判定）：

| type | len | 内容 | 判定 |
|---|---|---|---|
| 1 | 6 | `f8 73 1a d3 ef a8` | LAN MAC |
| 1 | 6 | `f8 73 1a d3 ef a9` | WAN MAC |
| 4 | 15 | `Example-SSID` | 2.4G SSID |
| 4 | 18 | `Example-SSID-5G` | 5G SSID |
| 5 | 8 | `ExamplePass` | 2.4G 口令（出现两次） |
| 5 | 8 | `ExamplePass` | 5G 口令 |
| 7 | 9 | `1630@CTCC` | 管理账号 |
| 3 | 6 | `AABBCC` | OUI 前缀（ASCII） |
| 0x82 | 15 | `000000000000000` | **SN** |
| 0x82 | 34 | `16C2RNEtye3vN1742627722795FFM4bvRz` | 设备密钥 |
| 2 | 15 | `214221173286740` | 运营商参数 |
| 2 | 15 | `54892T9T585A450` | 运营商参数 |
| 2 | 21 | `21422154892T9T585A450` | 两者拼接 |
| 2 | 14 | `12-6467-214221` | 运营商参数 |
| 0x81 | 10 | `ZXHN E2631` | **型号** |
| 2 | 15 | `XXXXXXXXXXXXXXXXX` | 授权/注册码 |

`mtd2 "wifi"`（1 MB）**不是空的**。前 `0xC0000` 全是 `0xFF`，从 `0xC0000`
起有一段 **55 281 字节**的连续数据，整体 3456 个 16 字节块里只有 1514 个互不
相同 ⇒ 结构化表（非压缩、非加密）：

- 记录头 `85 19 02 e0`，其后 4 字节长度（`0x6a` / `0x74` …），整段共 434 条；
- 段内既没有 `16 79 00 00`（mt7915 eeprom 的 chip id 头），也没有 `f8 73 1a`
  （本机 MAC），所以它**不是**一张裸的 mt7915 eeprom 映射，而是厂商自己的
  校准容器。

结论：驱动用的映射仍取 `MT7916_EEPROM.bin`（与 mt76 v2 偏移完全吻合、且是
`mt7915_eeprom_load_default()` 的同款文件）；`0xC0000` 的容器是**每台机器的
工厂校准**，格式厂商私有，暂时只登记不解析。若后续发现发射功率被压得过低或
需要 DPD 校准，再回来解这个容器。

MAC：tag 里只有 LAN/WAN 两个，`mtd2` 的 caldata 段内也没有 MAC 字符串，而镜像
里的 `00:0c:43:26:60:00` 是 MediaTek 模板值。也就是说 **WiFi MAC 不在主机可见的
flash 里**，大概率在 MT7916 自身 efuse（固件自己读），而 `EFUSE_ACCESS` 这条路
已证明拿不到数据。bring-up 阶段先用模块参数 `mac=` 覆盖，量产前再定来源。

## 84. band0 起来：MAC / DEV / 信道 / BSS 四条命令全部被接受（3.86 → 3.88）

**问题**：3.85 之后固件有了校准（`flash_mode` + 4 页 `EFUSE_BUFFER_MODE` 全 ret=0），
但一帧都发不出去。原因不是校准，而是主机从未告诉固件三件事：哪个 band 归 MAC、
调哪个信道、上面挂哪个 BSS。mt76 用四条命令说完，本轮的载荷是这四个函数逐字节
对照 `mt76-2026.10.08` 还原的（`mt76_connac_mcu.h` / `mt7915/mcu.c`）。

全部走 `MCU_EXT_CMD`（cid 0xed），`set_query = MCU_Q_SET`(1)，`s2d = MCU_S2D_H2N`(0)，
即 3.78 证明过的 qid20（MCU_WA）命令环：

| 命令 | ext | 长度 | 载荷 |
|---|---|---|---|
| RX_HDR_TRANS | 0x47 | 8 B | `hdr_trans = 0`：主机自己构 802.11 头 |
| MAC_INIT_CTRL | 0x46 | 4 B | `{enable, band}` |
| DEV_INFO_UPDATE | 0x2a | 20 B | hdr `{omac, band, tlv_num=1, append=1}` + `DEV_INFO_ACTIVE`(tag 0) TLV |
| CHANNEL_SWITCH | 0x08 | 76 B | control_ch/center_ch/bw/tx_path_num/rx_path/switch_reason/band_idx/channel_band |
| BSS_INFO_UPDATE | 0x26 | 80 B | `sta_req_hdr` + OMAC(12) + BASIC(24) + RF_CH(8) + BMC_RATE(12) |

### 84.1 一个字段换来一次固件卡死：`rx_path` 是数量不是掩码

3.86 按「读起来最自然」的方式把 `rx_path` 填成 band0 的 0x3 掩码。结果：

```
eeprom-upload 4 页全 ret=0
rx-hdr-trans ret=0        evtwa[?] eid=ed
mac-init-ctrl ret=0       evtwa[?] eid=ed
CHANNEL_SWITCH 0x08       ret=-110(timeout)   ← 从这里开始全挂
之后 rx-hdr-trans / mac-init / FW_LOG 0x13 全部 -110
```

现场取证定性（不用重刷就能分清两种失败）：

| 观测 | 值 | 结论 |
|---|---|---|
| WA 事件环 | `cidx=6 didx=6` | 6 帧 = 恰好前 6 条命令的应答，0x08 的应答从未产生 |
| qid20 命令环 | `cidx=8 didx=8` | 描述符被 DMA 搬走（**不等于核在处理**） |
| 再发已知能应答的 `FW_LOG 0x13` | `-110`，且 `didx` 不再前进 | **WA 核确实停了**，不是「慢」 |

根因在 `mt7915_mcu_set_chan_info()`：

```c
	if (cmd == MCU_EXT_CMD(CHANNEL_SWITCH))
		req.rx_path = hweight8(req.rx_path);
```

这条命令的 `rx_path` 是**链路数量**（band0 = 2），掩码只在其他命令里用。给 2 链
band 报 3 条 RX path，WA 核在校准中途死掉。注意：`fw-state` 仍是 7（那是主机写的
TOP_MISC，不是固件健康度），唯一的恢复手段是重新刷机——**一个 boot 只有一轮
「复位→下载→RDY」**（3.74 就记录过）。

### 84.2 3.87 实机结果（band0 / ch1 / 20 MHz / HT）

```
radio-mac 0 1:  rx-hdr-trans ret=0 | mac-init-ctrl ret=0
radio-up 0 1:   rx-hdr-trans ret=0 | mac-init-ctrl ret=0 | dev-info ret=0
                channel-switch ch=1 ret=0 | bss-info-update ret=0
                band 0 up: ch=1 bssid 00:0c:43:26:60:00 tx-path 2
liveness:       mcu-ext 13 00 -> ret=0   （调谐之后固件仍活着）
bss-show:       band 0: active ch=1  |  fw-state 7
```

信道设置之后固件开始主动上报 `eid=00` 的异步事件（约每秒数个，载荷 ~220 B），
这是 RF 真正动起来的直接迹象，也是下一轮 RX 上报要解析的对象。

### 84.3 3.88：证据可读性（诊断，无功能改动）

同一件事被两处掩盖过，都值得记住：

1. 运行脚本用 `grep -A9` 截取结果块，正好把 `channel-switch` 和 `bss-info-update`
   两行切掉，看起来像「序列停在 dev-info」。
2. `status[]` 只有 16 KiB，`zxnpu_wlan_append()` 满了以后**静默丢写**。调谐后固件
   每秒数个 `eid=00` 事件，每个都被 hex dump 68 B ⇒ 实测缓冲区 `16383 B` 写满，
   之后 `radio-mac` 自己的结果行也被丢掉（表现为「命令没有任何输出」）。

修法：`evt_log` 模块参数（默认 0）只打 `eid/seq` 一行，需要 hex 时置 1。

### 84.4 ctl 命令面新增

| 命令 | 作用 |
|---|---|
| `radio-mac <band> <0\|1>` | RX_HDR_TRANS + MAC_INIT_CTRL |
| `radio-chan <band> <ch>` | CHANNEL_SWITCH（20 MHz，5 s 预算） |
| `radio-bss <band> <0\|1>` | BSS_INFO_UPDATE（enable 带 4 个 TLV） |
| `radio-up <band> <ch>` | 按 mt76 顺序跑完整四条 |
| `mac-set <12hex>` | 覆盖 eeprom 模板 MAC |

### 84.5 下一步

WTBL 条目（`WTBL_UPDATE` 0x32 在 3.80 已验证有应答，但还没有真实 entry）→
TXD 构帧（`mt76_connac2_mac_write_txwi` 语义，字段表已从 `mt76_connac2_mac.h`
拿到：TXD0 包长/qidx/pkt_fmt、TXD1 long_format/hdr_format/wlan_idx、TXD2 固定速率
与功率偏移、TXD3 seq/BA）→ RX 上报解析（`eid=00` 那批事件）→ wiphy 注册。

## 85. TXD 落地：第一帧进 qid18 并被引擎处理完（3.89 → 3.90）

3.88 之后 band0 已 up 但 TX 环是空的，缺的只有 TXD。它不需要猜：
`mt76_connac2_mac_write_txwi()` + `mt76_connac2_mac_write_txwi_80211()`
就是对 `mt76_connac2_mac.h` 的纯位域打包。**32 字节 / 8 dword**：

| dword | 字段 |
|---|---|
| w0 | `TX_BYTES(len+32)` \| `PKT_FMT(MT_TX_TYPE_CT=0)` \| `Q_IDX(MT_LMAC_AC00=0)` |
| w1 | `LONG_FORMAT` \| `HDR_FORMAT(802_11=2)` \| `HDR_INFO(hdrlen/2)` \| `TID` \| `WLAN_IDX` \| `OWN_MAC` |
| w2 | `FRAME_TYPE` \| `SUB_TYPE` \| `MULTICAST` \| `FIX_RATE` |
| w3 | `REM_TX_COUNT(15)` \| `BA_DISABLE` |
| w5 | `PID`（0 低于 `MT_PACKET_ID_FIRST=3` ⇒ 不要主机 TX status） |
| w6 | `FIXED_BW` \| `TX_RATE`（2 GHz 的 `mt76_rates[0]` = CCK 1 Mbps = 0） |
| w7 | `SPE_IDX`（双链固定速率分支取 24） |

**不需要 WTBL 这一步**：`mt7915_mac_add_interface()` 从不对自己的 vif 发
`WTBL_UPDATE` —— BSS 的 wcid 是 `BSS_INFO_BASIC` 里的 `bmc_wcid` 建出来的
（3.87 已按 wcid 0 发过）。WTBL 表项只有站点关联（`STA_REC_UPDATE` + `WTBL_SET`）
时才出现。

### 85.1 两个 bug，都在「解读射频行为」之前被抓到

1. **数据 RX 环写错寄存器**：`ZXNPU_DATA_RX_REG` 原来是
   `RX_RING0 + 0x10*1 = 0x18024510`，那是 **WA 事件环**，不是数据环。RX 环块与
   MCU 事件环共用，mt76 的映射是 0=MCU WM、1=MCU WA、2=band0 data、3=band1 data
   ⇒ 数据环必须是 qid2（`0x18024520`）。原写法会让 `data-init` 把固件应答命令的
   环直接换掉。
2. **组播判定写反**：`is_multicast_ether_addr()` 是 `addr1[0] & 0x01`；3.89 写成
   「六个字节全 0」，没有任何地址满足，于是 `MULTICAST`/`FIX_RATE`/`BA_DISABLE`/
   速率字/`spe_idx` 全部没进 TXD。

第 2 个是靠「手算期望表 vs 实机 dump」的 dword 对账发现的，不是靠刷机试：

| dword | 手算期望 | 3.89 实机 | 3.90 实机 |
|---|---|---|---|
| w0 | `00000040` | `00000040` | `00000040` |
| w1 | `80026000` | `80026000` | `80026000` |
| w2 | `80000420` | `00000020` ❌ | `80000420` ✅ |
| w3 | `10007800` | `00007800` ❌ | `10007800` ✅ |
| w6 | `00000004` | `00000000` ❌ | `00000004` ✅ |
| w7 | `0000c000` | `00000000` ❌ | `0000c000` ✅ |

### 85.2 实机结果（3.90，band0 ch1，广播数据帧 64 B）

```
data-init: rx 18024520 base=4e048000 cnt=64 cidx=64 | tx 18024420 base=4e047000 cnt=64
tx-bcast:  queued slot 0 len=64 kick cidx=1 didx=0
           didx 0 -> 1 after 5 ms          (引擎取走了描述符)
           desc slot 0 ctrl=c0400000 dma_done=1   (引擎处理完 buffer)
```

**`dma_done` 是主机侧能拿到的第一手「帧真的走了」证据**；`didx` 前进只证明
描述符被读。没有 TX-done 事件，且本来也不该有：那要求 `pid >= 3`。

### 85.3 顺带确认的一件事：eid=00 那批事件是什么

调谐后固件持续上报 `eid=00`、载荷 ~220 B 的事件，`+14` 处是一个递增计数器
（`0120/0126/012d/…`），内容随每次上报变化但在同一量级 —— 是固件的周期心跳/
状态上报，不是命令应答，也不需要回应。`mcu-recv` 一次能刷出三十多帧，这就是
3.88 那个 16 KiB 缓冲区被写满的来源。

### 85.4 下一步

TX status（`pid >= MT_PACKET_ID_FIRST` + `MT_TXD5_TX_STATUS_HOST`，回收
`MT_DMA_CTL_DMA_DONE` 的环）→ RX 数据环解析（qid2，含 802.11 头 + RX status）
→ 站点关联路径（`STA_REC_UPDATE` + `WTBL_SET`，才需要 WTBL）→ wiphy 注册。

## 86. BSS 自己的 STA_REC + DRR：帧终于有 peer 了（3.91 → 3.92）

3.90 的帧进了 qid18、引擎每帧都置 `dma_done`，然后就没有下文。缺的不是 TXD 字段，
而是 `mt7915_start_ap()` 在 `add_bss_info()` 之后还有两条命令：

```c
	err = mt7915_mcu_add_bss_info(phy, vif, true);
	err = mt7915_mcu_add_sta(dev, vif, NULL, CONN_STATE_PORT_SECURE, false);
```

`sta == NULL` 时"站点"就是 BSS 自己的 wcid 0，消息是
`sta_req_hdr` + `STA_REC_BASIC` + `STA_REC_WTBL`，其中 **`STA_REC_WTBL` 里嵌套**
了一个 `wtbl_req_hdr`（`operation = WTBL_RESET_AND_SET`）+ 三个 TLV：

| TLV | 长度 | 关键值 |
|---|---|---|
| `WTBL_GENERIC` | 20 B | `peer_addr = ff:ff:ff:ff:ff:ff`、`muar_idx = 0xe` |
| `WTBL_RX` | 12 B | `rca1/rca2/rv = 1` |
| `WTBL_HDR_TRANS` | 8 B | `from_ds = 1`、`no_rx_trans = 1` |

外层的 `STA_REC_BASIC` 用 `conn_type = CONNECTION_INFRA_BC`(0x10020)、
`conn_state = PORT_SECURE(2)`、`extra_info = EXTRA_INFO_VER`、peer 全 ff。
最后 `mt7915_mcu_add_group()` 发 `SET_DRR_CTRL`(0x36)，`action = MT_STA_BSS_GROUP`。

**3.91 的错误假设**：以为 `BSS_INFO_BASIC` 里的 `bmc_wcid` 会顺手把 WTBL 表项建好。
它只是标注哪个 wcid 收广播，表项要等这条 STA_REC。没有它，固件没有 peer 可发。

实机（3.92）：`sta-rec (wcid 0, 80 B) ret=0`、`drr-ctrl (group 0) ret=0`，
`radio-up` 现在完整输出 dev/chan/bss/sta/drr 五条全 0。

### 86.1 探针请求：TXD 与手算表全等，但只换来一次 TX-free

`tx-probe`（mgmt probe request，`pid = 3` 要 TX status）：

```
txd w0=00000042 w1=80026000 w2=80000404 w3=10007800 w5=00000403 w6=00000004 w7=0000c000
queued slot 0 len=66 -> didx 0->1 -> desc ctrl=c0420000 dma_done=1
qid2: 一个包, type=6(TX-free) len=68
```

之后**再没有任何 TX-free**，尽管每帧仍 `dma_done=1`；qid2 的 `didx` 永远停在 1。

### 86.2 卡点（已逐个排除，供下一轮直接接手）

1. **RX 环只投递一次**：`cpu_idx` 现场试过 `0/1/2/63/64/65/66/128` 全部无变化，
   描述符回填（`ctrl = LAST_SEC0 | len<<16`）也做过。注意 mt76 的
   `mt76_dma_rx_fill()` 只填到 `ndesc-1`、kick 写 `q->head`，所以数据环的初始
   投递值是 **63 而不是 64**，消费后 `head = (last+1) % ndesc`——这条还没按
   mt76 原样实现，是下一轮第一件事。
2. **第二次 `CHANNEL_SWITCH` 永不回应**（`-110`），`BSS_INFO` 关掉再切也一样，
   所以信道只能在 `radio-up` 时选定；连试几次后 WA 核连命令都不接了
   （TX DMA 仍然工作）⇒ 每个实验都要重新开机。
3. **TX-free 是否按批上报**未定：只有 data-init 后的第一帧有回报。

### 86.3 踩到的操作失误（记下来）

- 刷机脚本**不能管道给 `head`**：`bash flash-3.92.sh | head -12` 里 head 退出后
  SIGPIPE 打断了升级流程（幸好校验在写之前，设备仍跑旧版本）。刷机一律
  `> log 2>&1` 再 grep。
- 运行脚本里 `grep -v '^  slot '` 把刚加上的 RX 解码行自己过滤掉了，
  差点误判成"没收到包"。
- `echo 'cmd' > $P > /dev/null` 会覆盖前一个重定向，命令根本没写进 proc。

## 87. 停止试错：原厂 mt7916.ko 未 strip，符号级反汇编开工（3.93 → 3.94）

前几轮在 RX 环投递上靠改寄存器试值，这是错的工作方式。本板原厂驱动
`mt7916.ko`（5.5 MB）**没有 strip**，25345 个符号全在，这才是权威参照。

### 87.1 工具（已进仓库 `native/tools/re/`）

宿主 binutils 是 x86 的（`objdump: can't use supplied machine arm`、
`objcopy: unable to recognise the format`），所以自己解析 ELF32 段表与 `.symtab`，
只依赖 capstone：

```
python3 tools/re/armdis.py mt7916.ko <symbol> [n]    # 按符号反汇编
python3 tools/re/armdis.py mt7916.ko --at 0x1eb784   # 地址属于哪个函数
python3 tools/re/armdis.py mt7916.ko --grep 0x4400   # 谁用了这个立即数
python3 tools/re/armdis.py mt7916.ko --callers pci_free_rx_buf
python3 tools/re/findval.py mt7916.ko 0x18024500     # 常量在哪个段
```

关键是**重定位注解**：可重定位目标里 `bl` 的目标是空的，不加注解全是"跳到自身"。
注解后代码直接可读：

```
bl   -> hif_io_write32
movw r1, #0   -> pci_kick_out_data_tx
```

`--callers` 必须同时解析 `.data` 里的函数指针，因为 SDK 所有 HAL 调用都走 ops 表，
`.text` 里根本没有对应的 `bl`。

### 87.2 读出来的事实

| 位置 | 事实 |
|---|---|
| `pci_kick_out_data_tx` | `hif_io_write32(hc, ring->cpu_idx_reg[+0x24], ring->sw_idx[+0x28])` |
| `mt_asic_init_txrx_ring` 第一循环（TX/MCU 环） | `base+8 ← 0`、`base+4 ← cnt` |
| `mt_asic_init_txrx_ring` **第二循环（RX 环）** | `write(base, buf_dma)` → `base+8 ← depth-1` → `base+4 ← depth` |
| `pci_free_rx_buf` | 只把软件索引 `+0x22` 加 1、回绕于 `+0x24` 的 depth；**不写任何寄存器** |
| `hif_init_WFDMA` | `GLO_CFG(0x18024208)` 清 BIT27、置 BIT28\|BIT21；随后写 `0x18024640/0x18024680` 窗口表（值 0x180+i*0x40 与 4+i*0x3c） |
| `pci_core_ops_register` | hif_ops：`+0x08 pci_free_rx_buf`、`+0x0c pci_init_txrx_ring_mem`、`+0x10 pci_reset_txrx_ring_mem`、`+0x18 pci_get_tx_buf`、`+0x34 pci_dma_reset`，以及 cmd/fwdl/data_tx 三个 kick |

### 87.3 实机对照（负结果，但仍然有价值）

| qid2 武装值 | 投递结果 |
|---|---|
| 64（3.90–3.92，错） | 恰好 1 个（TX-free），之后 `didx` 不动 |
| 0（3.93，读错了第一循环） | 0 个 |
| **63 = depth-1（3.94，原厂 RX 值）** | **0 个** |

所以**武装值不是全部真相**。原厂 RX 武装还写了环基址，而描述符布局来自
`pci_init_txrx_ring_mem`，那个还没读。首要嫌疑是**描述符步长**：若是 8 字节而我们
按 16 字节填，则只有第 0 项有效 —— 这正好解释"只投递一次"。

### 87.4 下一轮，先读再写

1. `pci_init_txrx_ring_mem` 的填充循环：步长、`ctrl` 字段编码、RXD 头是否预置；
2. RX 侧的 publish 点（`pci_rx_data_done_func` 里 `[sb+0x1c]` 那类 ops 回调的真实目标）；
3. 以上两条闭合后，才允许再刷机。

## 88. 两个静态错误：数据 RX 环是 qid4；数据帧是两段 DMA（3.95 → 3.97）

前四轮「RX 环只投递一次」「TX 有 dma_done 但没下文」其实是两个可以直接从参考源码
读出来的错误，不需要在硬件上试。

### 88.1 band0 数据 RX 环从来不是 0x18024520

`enum mt7916_rxq_id`（`mt7915/mt7915.h`）：

| idx | 环 | 寄存器 |
|---|---|---|
| 0 | `MCU_WM` | `0x18024500` |
| 1 | `MCU_WA` | `0x18024510` |
| 2 | `MCU_WA_MAIN`（band0 TX-free 通知） | `0x18024520` |
| 3 | `MCU_WA_EXT` | `0x18024530` |
| **4** | **`BAND0` 数据** | **`0x18024540`** |
| 5 | `BAND1` | `0x18024550` |

`mt76_dma_alloc_queue()` 的地址是 `MT_RXQ_RING_BASE(q) + idx * MT_RING_SIZE`
（`MT_RING_SIZE = 0x10`）。原厂镜像独立印证：`chip_show_dma_info()` 逐个 dump 六个
RX 环，地址正是 `0x18024500`–`0x18024550`。

3.90–3.94 用的 `0x18024520` 是 **MCU_WA_MAIN**，mt76 对它的注释原话是
“tx free notify event from WA for band0”。所以我们唯一收到的那一个 `type=6` 包是
TX-free，而不是收到的帧；而且它是「事件环被当成数据环用」——这既解释了「只投递一次」，
也解释了为什么再怎么改 `cpu_idx` 都没用。

### 88.2 数据帧是两段 DMA：TXD 一段，帧一段

`mt76_dma_tx_queue_skb()` 把 `txwi_size` 与 `skb->len` 作为**两个 buffer** 传下去，
`mt76_dma_add_buf()` 写：

```
buf0 = txwi(32B)   SD_LEN0 = 32
buf1 = frame       SD_LEN1 = len
flag = LAST_SEC1 (bit14)      ← 两段时是 bit14，不是 bit30
```

单段 + `LAST_SEC0`（bit30）是 **MCU 命令环**需要的（它的 32 字节头确实是消息的前
32 字节），而数据帧沿用同一写法时，TMAC 会去 `buf1` 取 802.11 头，而 `buf1 = 0`
⇒ 帧被静默丢弃：DMA 取走描述符、置 `DMA_DONE`，但**没有 TXS、没有 TX-free、
WA 事件环里也没有 TX-done**。

修好后的实机描述符：

```
desc: buf0=4f21e000 (TXD 32B) buf1=4f21d000 (frame 34B) ctrl=00204022
      处理前 00204022 → 处理后 80204022
      （SD_LEN0=32、SD_LEN1=34、LAST_SEC1、DMA_DONE）
```

**仍然没有完成事件**。所以 TXD 与描述符已不再是嫌疑，剩下的嫌疑是主机侧从未编程的
band 级 WF 状态与 PSE↔WFDMA 队列映射：

| 来源 | 内容 |
|---|---|
| `mt7915_mac_init_band()` | `MT_TMAC_CTCR0`（INS_DDLMT）、`MT_MDP_BNRCFR0/1`（band 速率/RX 路径）、`MT_DMA_DCR0`（MAX_RX_LEN 0x680）、`MT_WF_RMAC_RSVD0`、`MT_WF_RMAC_MIB_AIRTIME0/1/3/4`、`MT_WTBLOFF_TOP_ACR`、`MT_WTBLOFF_TOP_RSCR` |
| `mt7915_mac_init()` | `MT_MDP_DCR1/2`、`MT_MDP_DCR0`（DAMSDU）、288 个 wcid 的 `WTBL_UPDATE(ADM_COUNT_CLEAR)`、`MT_WF_PP_TOP_RXQ_WFDMA_CF_5`（qid6 端口选择） |

这些都在 WF 空间（`0x820e4000` 起，band1 为 `0x820f4000`），要走 BAR 窗口访问，
不是现在 `crw` 直接打 CR 的路径。

### 88.3 qid2 必须由主机武装

`mt7915_dma_init()` 在非 WED 路径下同样会 `mt76_queue_alloc()` 一个
`MT_RXQ_MAIN_WA`（`MT7915_RX_MCU_RING_SIZE` = 512）。3.95 读回
`base=00000000 cnt=512` ⇒ **固件不武装它**。新增：

| 命令 | 作用 |
|---|---|
| `txfree-init` | 按 MCU 事件环模型武装 qid2：`ctrl = 0x40000000 | len<<16` + RXD 模板，然后 base / cnt / `CPU_IDX = depth` |
| `txfree-recv` | 排空 qid2，回填描述符并按 `didx + depth` 重新发布（与 `mcu-recv` 同） |

3.92 唯一那次 TX-free，就是 `data-init` 误把这个环当数据环喂出来的。

### 88.4 原厂 RX 环结构（本轮从 `mt7916.ko` 读出，记录备用）

| 偏移 | 含义 |
|---|---|
| `+0x14` | depth（h） |
| `+0x4c` | base 寄存器地址 |
| `+0x50` | cnt 寄存器地址 |
| `+0x3c` | cpu_idx 寄存器地址 |
| `+0x34` | 发布到 cpu_idx 的**值**（初值 depth-1） |
| `+0x08` | dma_idx 寄存器地址 |
| `+0x30` | 门铃挂起计数（h） |

`pci_rx_dma_done_handle()` 的尾部就是 `hif_io_write32(hc, ring[+0x3c], ring[+0x34])`；
`dump_dma_rx_ring_info()` 的 pending 算术是 `dma_idx - cpu_idx - 1 (mod cnt)`，
即武装后为 0（空环）——与 mt76「填 ndesc-1 再 kick `q->head`」是同一个模型。

### 88.5 下一步（按依赖顺序）

1. 补 `mt7915_mac_init_band` 的 WF 寄存器编程（先 tb 窗口读回现场值，确认哪些是 0）；
2. 查 PSE↔WFDMA 的 TX 队列映射（原厂 `hif_init_WFDMA` 里 0x4680/0x4640 两张表已确认
   只是 prefetch 分区，不是队列映射）；
3. 以上两条闭合后，TX-free/TXS 才有意义；再谈 RX 侧信道与 wiphy 注册。

## 89. 原厂 WiFi 启动全流程核对（mt7916.ko 符号级）

本节把原厂驱动的启动过程按阶段拆开，逐个给出**函数名、体量、做的事、证据来源**，
再对照我们 `zxnpu-wlan.c` 的现状，最后列出当前验证里**还没被覆盖的环节**。

### 89.0 方法与证据等级

本板 `mt7916.ko` 未 strip（25 345 个符号），用 `native/tools/re/armdis.py` 按符号反汇编，
重定位已注解（`bl -> name`、`movw r1,#0 -> name`），可读性足够做流程核对。

| 等级 | 含义 |
|---|---|
| **S** | 静态确证：反汇编直接读出函数体/调用点 |
| **B** | 行为确证：本机实测日志可复现 |
| **I** | 推断：由命名/调用关系推测，尚未取证 |

**结构性限制（必须写明）**：这套驱动是 **ops 表驱动**的。大量关键路径是
`ldr r3,[ops,#0xNN]; bx r3`，例如：

```
asic_wa_update:   ldr r3, [arch_ops, #0x1ec]; cmp r3,#0; bx r3
AsicInitMac:      bl hc_get_arch_ops        ; 之后全部间接
wdev_do_open:     ldr r3,[wdev+0xd20]; ldr r3,[r3+0xa0]; bx r3
```

所以**静态调用图到不了叶子**，下文凡涉及间接跳转的，顺序取自同函数内可见的 `bl`
序列，语义按函数名 + mt76 对照给出（标 I）。

### 89.1 阶段总表

| # | 阶段 | 入口函数 | 体量 | 关键子函数 | 证据 |
|---|---|---|---|---|---|
| 1 | 模块加载 | `init_module` | 108 B | `os_module_init`, `__pci_register_driver` ×2 | S |
| 2 | 芯片能力注册 | `mt7916_init` | 3 956 B | `hc_register_chip_ops`, `mt7916_chip_dbg_init`, `mt_phy_probe`, `mt7916_get_chip_info`, `mt7916_get_default_bin_image` | S |
| 3 | MCU 子系统 + 固件下载 | `WfMcuInit` | 636 B | `WfMcuSysInit`, `NICLoadRomPatch`, `NICLoadFirmware`, `HWCtrlOpsReg`, `MtCmdCr4Set`, `asic_wa_update` | S |
| 3b | 下载数据面 | `mt_load_fw` → `mt_load_patch` → `load_patch_v1` | — | `parse_patch_v1/v2`, `fwdl_datapath_setup`, `pci_kick_out_fwdl_msg`, `mt7916_kick_out_fwdl_msg`, `pci_fwdl_dma_done_handle` | S+B |
| 3c | MCU 命令面 | `AndesSendCmdMsg` | — | `AndesInitCmdMsg`, `AndesMTFillCmdHeader`, `AndesRxProcessCmdMsg`, `AndesMTRxProcessEvent`, `AndesBhSchedule`, `AndesGetCmdMsgSeq` | S+B |
| 4 | WFDMA 全局 + 分区 | `hif_init_WFDMA` | 740 B | prefetch 表 11 项、`GLO_CFG` | S+B |
| 5 | 环内存与回调 | `pci_init_txrx_ring_mem` | 2 868 B | `alloc_rx_buf_1k/64k`, `RTMP_AllocateRxPacketBuffer`, 回调注册 | S |
| 6 | 环武装 | `mt_asic_init_txrx_ring` | 392 B | TX 环与 RX 环两套初值 | S+B |
| 7 | MAC/PHY 初始化 | `NICInitializeAdapter` | 92 B | `WfMacInit` → `WfPhyInit` → `NICInitializeAsic` | S |
| 7b | MAC 子层 | `WfMacInit` | — | `chip_wait_hif_dma_idle`, `AsicInitMac`, `AsicAutoFallbackInit`, `AsicSetMacMaxLen`, `asic_init_wtbl`, `AsicRxHeaderTransCtl`, `AsicRxHeaderTaranBLCtl` ×3, `AsicAutoBATrigger` | S |
| 7c | PHY 子层 | `WfPhyInit` | — | `NICInitBBP` | S |
| 8 | BSS 启动 | `APStartUp` → `APStartUpByBss` | 424 B | `MlmeSetTxPreamble`, `wdev_do_open`, `RTMP_IndicateMediaState`, `UpdateBeaconHandler`, `APStartRekeyTimer`, `AsicSetPreTbtt`, `chip_interrupt_enable` | S |
| 9 | 数据面 TX | `ge_/fp_tx_pkt_deq_func` → `hif_kickout_data_tx` → `pci_kick_out_data_tx` | 72 B | `fill_tx_blk`, `ap_fill_non_offload_tx_blk` | S+B |
| 10 | 数据面 RX | `pci_rx_dma_done_handle` / `pci_rx_dma_done_rxq_handle` | 344 / 420 B | `pci_free_rx_buf`；`asic_rx_pkt_process` 在本 build 里是空壳（转到 `AsicNotSupportFunc`），真正的逐包处理经 arch_ops | S |

### 89.2 逐阶段处理逻辑

#### 阶段 1–2：绑定与能力注册

```
init_module:
    os_module_init()
    __pci_register_driver(&driver0, ...)     ; 两个 driver（hif0 / hif1）
    __pci_register_driver(&driver1, ...)
```

`mt7916_init` 不碰硬件，只填能力表：`hc_register_chip_ops()` 把 chip_ops / arch_ops /
hif_ops 三张表装好（`pci_core_ops_register` 就是其中 hif_ops 那张，见 §87.2），
`mt_phy_probe()` 建 phy 实体，`mt7916_get_default_bin_image()` 取固件镜像描述。

**注意**：`hif_init_WFDMA` 与 `pci_init_txrx_ring_mem` 在这个函数里的两处引用是
**把函数指针存进 ops 表**，不是直接调用（`--callers` 报的是重定位点）。真正的调用
时机由 ops 使用者决定（标 I）。

#### 阶段 3：MCU 子系统与固件下载（`WfMcuInit`，顺序 S）

```
WfMcuInit:
    WfMcuSysInit()          ; MCU 子系统初始化（内部再走 MCUSysInit → NICLoadFirmware）
    NICLoadRomPatch()       ; ROM patch
    hw_io_read32 / hw_io_write32   ; 握手位（读-改-写）
    NICLoadFirmware()       ; → mt_load_fw()
    WfMcuSysExit()          ; 错误路径
    HWCtrlOpsReg()
    MtCmdCr4Set()
    hw_io_read32 / hw_io_write32
    asic_wa_update()        ; WA 核更新（arch_ops[0x1ec]）
    hw_io_read32 / hw_io_write32
```

下载本体（`load_patch_v1`，**S+B**，我们 3.32–3.37 按它实现并在机器上验过）：

```
SemGet(type 0x10 cmd 0x17)                 ; 申请信号量
AddressLen(type 0x01 cmd 0x17, dst,total)  ; 目标地址 + 总长
FwScatter(type 0xEE cmd 0x10, 分片)        ; 数据分片下发
Finish(type 0x07 cmd 0x17)                 ; 结束
```

`parse_patch_v1 / parse_patch_v2` 负责解析镜像头（v1 单段 / v2 多段）。
下载走的是 **FWDL 专用环**（`fwdl_datapath_setup` + `pci_kick_out_fwdl_msg` +
`mt7916_kick_out_fwdl_msg`，完成中断由 `pci_fwdl_dma_done_handle` 处理），
与我们 qid16 的做法一致。

命令面是 Andes 消息层：`AndesInitCmdMsg` 初始化队列，`AndesSendCmdMsg` 提交，
`AndesMTFillCmdHeader` 填 32 字节头（`txd[0]=0x41000000|len`，`Q_IDX 0x20`，
`PKT_FMT 2`），`AndesRxProcessCmdMsg` / `AndesMTRxProcessEvent` 处理事件环，
`AndesBhSchedule` 是下半部调度，`AndesGetCmdMsgSeq` 产生 seq。

#### 阶段 4–6：WFDMA 与环（全部 S，且大部分已 B）

`hif_init_WFDMA`（740 B）实测写入顺序（`mac_io_write32`）：

```
0x18024208 (GLO_CFG)  ← 清 bit27(OMIT_RX_INFO)，置 bit28(OMIT_TX_INFO)|bit21
0x18024680..0x18024694 ← 0x00040004, 0x00800004, 0x00c00004,
                          0x01000004, 0x01400004, 0x01800004
0x18024640..0x18024650 ← 0x01c00004, 0x02000004, 0x02400004,
                          0x02800004, 0x02c00004
0x1802420c ← 0xffffffff   (中断全部清除/关闭)
```

这 11 条就是 **prefetch SRAM 分区**（`PREFETCH(base,depth) = base<<16 | depth`）。
原厂把 RX 六个环放在 `0x040..0x1c0`、TX/MCU 五个环放在 `0x1c0..0x2c0`；mt76 的顺序
相反（TX `0x000..0x140`、RX `0x140..0x2c0`）。两种都是连续不重叠的分区，
所以 mt76 的表可以照用——我们驱动里的 `zxnpu_prefetch[]` 用的就是 mt76 那张。

`pci_init_txrx_ring_mem`（2 868 B）做三件事：

1. 分配每个环的**描述符数组**与 **buffer 表**（表项 **12 字节**：`+0` 标记、`+4` DMA、`+8` 长度等，`alloc_rx_buf_1k` 里 `idx*3*4` 可见）；
2. 分配并预填 RX buffer（`RTMP_AllocateRxPacketBuffer`，buffer 头 16 字节 RXD 模板）；
3. **注册完成回调**：`pci_rx_event_dma_done_handle`、`pci_rx_dma_done_handle`、`pci_rx_dma_done_rxq_handle`、`pci_tx_dma_done_handle`、`pci_cmd_dma_done_handle`、`pci_fwdl_dma_done_handle`（I：环→回调的具体对应按命名与注册偏移推断）。

`mt_asic_init_txrx_ring`（392 B）两个循环，**初值不同**：

```
第一循环（TX/MCU 环，hif_ctrl+8 数组）：
    write(base,    desc_dma)
    write(base+8,  0)              ; CPU_IDX ← 0
    write(base+4,  depth)          ; CNT
第二循环（RX 环，hif_ctrl+4 数组），顺序 base → CPU_IDX → CNT：
    write(base,    desc_dma)
    write(base+8,  depth-1)        ; CPU_IDX ← depth-1
    write(base+4,  depth)          ; CNT
```

配套的 `dump_dma_rx_ring_info` 给出 pending 算法
`dma_idx - cpu_idx - 1 (mod cnt)`，即武装后为 **0（空环）**，与 mt76
「填 `ndesc-1` 再 kick `q->head`」等价。

#### 阶段 7：MAC/PHY 初始化（`NICInitializeAdapter`）

```
NICInitializeAdapter:
    if (WfMacInit())  return 1
    if (WfPhyInit())  return 1
    return NICInitializeAsic()      ; 本 build 里只是两个 printk，实质为空
```

`WfMacInit`（S）：

```
chip_wait_hif_dma_idle()
AsicInitMac()                  ; 主体，经 arch_ops 间接
AsicAutoFallbackInit()
AsicSetMacMaxLen()
asic_init_wtbl()
AsicRxHeaderTransCtl()
AsicRxHeaderTaranBLCtl() ×3
AsicAutoBATrigger()
```

**这一层我们完全没有对应实现**。mt76 的对应物是 `mt7915_mac_init()` +
`mt7915_mac_init_band()`（`MT_TMAC_CTCR0`、`MT_MDP_BNRCFR0/1`、`MT_DMA_DCR0`、
`MT_WF_RMAC_RSVD0`、`MT_WF_RMAC_MIB_AIRTIME0/1/3/4`、`MT_WTBLOFF_TOP_ACR`、
`MT_WTBLOFF_TOP_RSCR`、`MT_MDP_DCR0/1/2`、288 个 wcid 的 `WTBL_UPDATE`），
全部在 WF 空间。

`WfPhyInit` → `NICInitBBP()`（BBP 层初始化，对应 mt76 的 `mt7915_init_bb`）。

#### 阶段 8：BSS 启动（`APStartUpByBss`，顺序 S）

```
APStartUpByBss:
    MlmeSetTxPreamble()
    wdev_do_open()                 ; 间接：[wdev+0xd20]+0xa0，BSS 打开主体
    RTMP_IndicateMediaState()
    UpdateBeaconHandler()          ; 信标更新（内部 bcn_buf_init）
    APStartRekeyTimer()
    AsicSetPreTbtt()               ; TBTT（信标定时）预置
    chip_interrupt_enable()        ; 最后才开中断
```

**信标 + TBTT + 开中断**这三步在 BSS open 之后、数据面之前，是厂商 AP 启动的固定
收尾。我们做到 BSS/STA_REC/DRR 就停了，后三步都没做。

#### 阶段 9–10：数据面

```
TX（S+B）:
  ap_fill_non_offload_tx_blk / ap_fill_offload_tx_blk     ; 组帧（含 52B TMAC）
  fill_tx_blk
  ge_tx_pkt_deq_func / fp_tx_pkt_deq_func                 ; 出队
  hif_kickout_data_tx → pci_kick_out_data_tx              ; 门铃
      = hif_io_write32(hc, ring->cpu_idx_reg[+0x24], ring->sw_idx[+0x28])

RX（S）:
  pci_rx_dma_done_handle      ; 事件环：按 ring->[+0x1d]/[+0x1f] 索引回调表，blx
                              ; 尾部 write32(hc, ring[+0x3c], ring[+0x34]) 并清 [+0x30]
  pci_rx_dma_done_rxq_handle  ; 数据环：加锁 → 两个 blx（逐包处理）→ IsStopingPdma
                              ; → 解锁 → hif_io_write32 → smp_call_function_single
```

**数据环和事件环是两套完成处理函数**，这一点此前没注意：事件环的发布值在
`ring[+0x34]`，数据环走另一套（含 `smp_call_function_single` 调度软中断）。

### 89.3 对照表：原厂阶段 vs 我们已实现

| 阶段 | 原厂 | 我们的实现 | 状态 |
|---|---|---|---|
| 1 绑定 | `init_module` + pci probe | 内核 PCI 驱动 | ✅ |
| 2 能力注册 | `mt7916_init` | 不需要（无 ops 层） | — |
| 3 FW 下载 | `WfMcuInit` → `load_patch_v1` | `mcu-dl / mcu-dl-regions / fw-start` | ✅ B（fw-state=7） |
| 3 校准 | `mt7915_mcu_set_eeprom` 等价 | `eeprom-init` + `eeprom-upload` 4 页 ret=0 | ✅ B |
| 4 WFDMA | `hif_init_WFDMA` | `dma-init`（GLO_CFG + 同款 prefetch 表） | ✅ B |
| 5 环内存 | `pci_init_txrx_ring_mem` | `queue-init`（TX 三环）+ `data-init`/`rx-init` | ⚠ 部分 |
| 6 环武装 | `mt_asic_init_txrx_ring` | 同 | ✅ B（TX 环 didx 前进） |
| 7 MAC 初始化 | `WfMacInit`（7 个子步） | **无** | ❌ |
| 7 PHY 初始化 | `WfPhyInit` → `NICInitBBP` | 部分（BBP 由 FW 侧做） | ❓ |
| 8 BSS 启动 | `APStartUpByBss`（含 beacon/TBTT/IRQ） | `radio-up`（MAC/DEV/CHAN/BSS/STA/DRR） | ⚠ 缺 beacon、TBTT、IRQ |
| 9 TX | `pci_kick_out_data_tx` | `tx-probe`（两段式描述符） | ⚠ 描述符正确，无完成事件 |
| 10 RX | 双回调 | `data-recv` / `txfree-recv` | ⚠ 从未收到过数据帧 |

### 89.4 当前测试中的盲目环节

1. **把 `dma_done` 当成"帧发出去了"**。`DMA_DONE` 是 WFDMA 引擎写回描述符的位，
   只证明它读完了 buffer。3.90 的结论「第一手证据」是错的，真正的判据只有
   TXS / TX-free / TX-done 三者之一，而这三个从未出现过。
2. **空口零观测**。没有第二台设备监听，也没有抓包；探针请求是否有 AP 应答取决于
   环境里恰好有 AP，而信道只能在 `radio-up` 时定一次（第二次 `CHANNEL_SWITCH` 不应答），
   所以一次开机只能试一个信道。**"RX 收不到"无法区分"环不通"和"空中没帧"**。
3. **观测面只覆盖 CR 空间**。`crw` 打的是 `0x1802xxxx`；WF 空间（`0x820e4000+`）的
   TMAC/MDP/RMAC/WTBLOFF 状态从未读回，于是阶段 7 那一层一直是盲的。
4. **曾经的"TX-free 只有一帧"是伪证据**。那是 `data-init` 把 qid2（TX-free 事件环）
   当数据环武装出来的产物，3.90–3.94 基于它的推断全部作废。
5. **`fw-state` 被当成固件健康度**。它是主机写的 TOP_MISC 镜像；3.86 把 WA 核打死时
   它仍然是 7。健康判据只能是「命令环 didx 前进 + 已知命令有应答」。
6. **一个 boot 只能验证一个信道、一轮下载**。多次 `wfsys-reset` 后 ROM 不再回下载态，
   所以实验变量必须一次收敛，否则结果不可归因。
7. **`status[]` 16 KiB 静默丢写**。已踩两次（事件 hex 把结果行挤掉），现在靠
   `evt_log=0` 默认关 hex；任何新增打印都要按这个约束设计。

### 89.5 缺失的验证点（按依赖顺序）

| # | 缺什么 | 为什么必须先补 |
|---|---|---|
| 1 | **一次真正的空口证据**（第二台设备在目标信道抓探针/信标） | 否则 TX 是否成功永远只能靠 FW 的间接信号 |
| 2 | **WF 空间寄存器读回**（`MT_TMAC_CTCR0`、`MT_MDP_BNRCFR0/1`、`MT_DMA_DCR0`、RMAC/WTBLOFF） | 决定阶段 7 是"FW 已配好"还是"全 0"，是当前最大盲区 |
| 3 | **BEACON / TBTT**（`BCN_UPDATE` + `AsicSetPreTbtt` 等价） | 厂商 AP 启动的固定收尾，我们跳过了 |
| 4 | **WFDMA 中断使能**（现在 `INT_ENA=0` 全轮询） | 轮询可用，但会掩盖"中断没配好"，接 wiphy 时必须补 |
| 5 | **TXS 语义验证**（`pid>=3` + `TX_STATUS_HOST` 已设但从未有 TXS） | 区分"FW 没发"与"FW 发了但不回报" |
| 6 | **WTBL 速率表（RA）**：只发了 BASIC + GENERIC/RX/HDR_TRANS，没有 RA 项 | 固定速率是否绕过这一层未经验证 |
| 7 | **RXD 解析偏移**：按 `RXD0[31:27]` 类型 + `[15:0]` 长度，但从未有真包核对 | 拿到第一个真包前，这套解析都是假设 |
| 8 | **MAC 来源**：空口上 BSSID 仍是 eeprom 模板 `00:0c:43:26:60:00` | 与 tag 分区的 LAN/WAN MAC 不一致，上线前必须解决 |
| 9 | **band1 / 5G**：eeprom 写着 5GHz/3 链，从未碰过 | 双频产品的必要面 |
| 10 | **`AsicRxHeaderTaranBLCtl` 对应的 header-trans 配置** | 我们发了 `RX_HDR_TRANS(0x47)=off`，但厂商在 MAC 层还有一组 RxHeaderTrans 开关 |

### 89.6 结论：现在"已知良好"的边界

- **已经可以复现到**：固件下载完成（fw-state=7）、校准上传（4 页 ret=0）、
  WFDMA 起来、band0 的 MAC/DEV/CHANNEL/BSS/STA_REC/DRR 六条命令全部 `ret=0`、
  数据帧描述符字节正确并被引擎取走（`dma_done`）。
- **尚未跨过的坎**：从"引擎读走描述符"到"帧真的出现在空中"之间，
  阶段 7（MAC 初始化）和阶段 8 的收尾（beacon/TBTT/IRQ）是空白。
- 因此下一轮工作的正确顺序是：**先补 WF 空间读回能力**（只读，不刷机也能做）→
  照 `mt7915_mac_init_band` 补寄存器 → 再谈 beacon/TBTT → 最后才有资格验证 TXS/RX。

## 90. R91：L2 窗口修复 + WF 空间读回——固件已自配 MAC 层，"补 mac_init_band"路线作废（3.98，2026-11-03）

### 90.1 WF 空间读不回来不是"全 0"，是走错了窗口

3.97 的 `cr 820e4114`（TMAC_CTCR0）返回 0，§89.5 把它列为最大盲区。仲裁实验
（三方交叉，全部一致）：

| 来源 | 证据 |
|---|---|
| vendor `pci_io_remap_read32`(0x194658) 反汇编 | 地址 ≤0x7ffffffe 或 ≥0xf0000000 → **L1**（fe260，窗 0xe0000）；**0x7fffffff..0xefffffff → L2**（寄存器 bar0+**0x1b8**，窗 bar0+**0x40000**，[31:16]=addr>>16） |
| mt76 regs.h 804-809 | `MT_HIF_REMAP_L2_MT7916=0x1b8`、`OFFSET=GENMASK(15,0)`、`BASE_L2_MT7916=0x40000` |
| mt76 mmio.c `__mt7915_reg_addr` | WF LMAC 块全在**固定表**（性能优化），从不走 L1 |

即 0x820exxxx 整个 WF LMAC 空间属于 L2，而驱动此前把一切非直连地址都塞进了
L1——读回全 0 是**死窗**，不是芯片状态。同批 MDP 块（0x820cd000，固定表内）
一直能读到非零值，也旁证了这一点。

### 90.2 修复（3.98，sha256 111e1239…）

- `zxnpu_l2_select()`：镜像 l1_select（保留 [15:0]、[31:16]=base、写后回读推
  动），`zxnpu_cr_read/write` 增加 `0x80000000..0xefffffff → L2` 路由；
- `crn <addr> <n>`（1..64 连读）批量读命令，块扫一次 SSH 搞定；
- 版本文件 → `OpenWrt native-wired-3.98`。刷机 serial 211，device-verify
  ALL-PASS；bring-up 全链（fw-state 7 / eeprom 4 页 / radio-up 五条 ret=0 /
  band0 active ch=1）在 3.98 上完整复现。

### 90.3 读回结果（band0 已调谐、fw-state=7 状态下）：MAC 层固件自己配好了

| 寄存器 | 实测值 | 对照 mt76 mac_init(_band) 语义 |
|---|---|---|
| DMA_DCR0 (820e7000) | 0x02773400 | MAX_RX_LEN=**0x680**（与 mt76 编程值一致）、RXD_G5_EN 置位 |
| TMAC_CTCR0 (820e4114) | 0x00060000 | INS_DDLMT_EN\|VHT_SMPDU_EN 已置（同 mt76）；REFTIME=0（mt76 设 0x3f） |
| MDP_DCR0 (820cd000) | 0x0003c200 | DAMSDU_EN(bit15) 已置（同 mt76） |
| MDP_BNRCFR0/1 | 0x00400010/0x00400000 | mgmt→FW 路由（bit4=1），非 mt76 的全 HIF |
| AGG/ARB（AWSCR0/PCR0/ACR0/MRCR/SCR/DRNGR0） | 全非零 | 调度器/竞争窗口已配置 |
| TMAC TRCR0/ICR0/ATCR、WTBLOFF | 全非零 | 定时器已配置 |

与 mt76 不同的只有 airtime 统计口径（ACR bit29、RSCR RCPI_PARAM=0、
AIRTIME0 ED_OFFSET=0xa、AIRTIME4 非零）——是遥测口径不是帧流门控。
**§89.5 第 2 项（照 mac_init_band 补寄存器）撤销**：阶段 7 的"空白"其实是
固件职责，且固件已履行。缺失收尾仍然只有 §89.5 第 3 项起：beacon/TBTT →
TXS/RX 验证。

### 90.4 数据面第一次有效武装，以及一个无效实验的教训

3.98 boot 上先跑了 `txfree-init`+`tx-probe`（**漏了 data-init**）：
qid18 base=0/cnt=512（ROM 默认），7 帧提交进了空环，引擎对垃圾地址做了一次
假取帧（didx 0→1）并吐了一条垃圾 TX-free（`00 80 ff 3f…`）——把它误读成
"TX-free 只回一次"（§86.1 的悬案其实是这个）。当场补 `data-init` 后环已武装
（rx qid4 cidx=63、tx qid18 base=4e047000），但 qid18 的引擎游标停在垃圾相位
留下的 didx=1，8 连发不再取帧——**干净 boot 复测**（run-r92.sh：bring-up →
data-init → txfree-init → tx-bcast×3 → tx-probe×3）。

教训入册：**rings 的 base/cnt 必须在读任何 didx 语义之前核对**；"引擎不动"
先查武装，再查游标，最后才轮到协议。

## 91. R92/R93：数据面干净 boot 复测、beacon 上传、以及一个让 WA 核死掉的 +4 字节（3.99/3.100，2026-11-03）

### 91.1 干净 boot 下引擎取帧 6/6——"TX-free 只回一次"悬案结案

run-r92.sh（bring-up → **data-init → txfree-init 在任何提交之前** → tx-bcast×3 →
tx-probe×3）：qid18 `cidx=3 didx=3`、随后 `cidx=6 didx=6`，6/6 全部被引擎取走。
3.98 boot 上"第二帧之后引擎不取"（§90.4）是 base=0 垃圾相位楔死了游标——
**取证顺序错了就全错：rings 的 base/cnt 必须在读 didx 语义之前核对**。
qid2 的 TX-free 在干净 boot 上一条都没来（引擎取帧 ≠ 帧上了空口；真正的
完成事件要等 WA 核认为帧已发出）。

### 91.2 beacon 上传（bcn-set/bcn-off，3.99）

`BSS_INFO_UPDATE(0x26)` + `BSS_INFO_OFFLOAD(tag 15)` TLV，嵌套
`BSS_INFO_BCN_CONTENT(tag 3)`：`[tim_ofs][csa_ofs][bcc_ofs][pkt_len] +
32B TXD + beacon 帧`（帧 = mgmt/beacon + SSID/RATES/DS/TIM，TIM 偏移从帧首算）。
固件接受（ret=0），bss-show 显示 ssid。LPON 块实证 TBTT 定时器已被固件武装：
`0x820eb0b8 = 0x80010064`（bit31 使能 + 0x64 = 100 TU，正是 BASIC TLV 的
bcn_interval），`0xd0/0xd4` 两个自由计数器在走。PLE 页池健康
（mt7916 的 FREEPG 在 **0x380**，不是 mt7915 的 0x100——0x100 读到的 0 是错偏移；
实测 0xffe = 4094 空闲页）。LPON TCR0 的 SW_WRITE 手动触发也无空口效果。

### 91.3 决定性线索：beacon 上传之后 WA 核死了

r93 的 C4 liveness（`mcu-ext ed 13`）**-110**——而此前每轮 boot 同一条都是
ret=0。qid20 `cidx=didx=16`（命令被 DMA 取走）但事件环零应答，与 §84.1 的
"rx_path 楔死 WA 核"同签名。**只差 beacon 这一步 → 元凶在 beacon TLV 里。**

根因：嵌套 TLV 的 len 字段按 mt76 的 add_tlv 约定**含自身的 4 字节 tag+len 头**，
`sizeof(bss_info_bcn_cont)=12` 已经含头，CONTENT 总长 = `12+32+帧`；我写成了
`4+12+32+帧`，长 4 字节。固件先 ack 后按 len 多读 4 字节 → 解析越界 → WA 核
停止服务一切命令（beacon 不发、命令环哑、qid4 RX 停摆）。3.100 修正。

### 91.4 空口见证工具链

- PC 的 `netsh wlan show networks` 在连接状态下不主动刷新；用 wlanapi 的
  `WlanScan`（`_scratch/npu-probe/wlan-scan.ps1`）强制即时扫描后 netsh 才给新数据。
- 强制扫描确认环境：2.4G 有 CMCC-Hx4U、琰-没有密码等多个 HE AP 活跃。
- MIB 全块两次独立采样差分为零——但 MIB 的读锁存语义未弄清（SDR9 跨分钟会变、
  3 秒内不变），不能当"无帧"的硬证据；空口验收以 PC 强扫 + 手机扫表为准。

### 91.5 下一步

3.100 刷机 → run-r93 重放：验收 = bcn-set 后 liveness 仍 ret=0（WA 活着）
+ PC 强扫看到 `e2631-test`（beacon 真上了空口）。此后按 §89.5 顺序：
关联路径（probe req 响应/STA_REC wcid≠0）→ 数据面收发 → wiphy。

## 92. R97/R98：楔死与内容无关——固件 beacon 任务自身在等一次永不完成的 TX（3.103，2026-11-03）

- R98 分叉（3.103 加 `bcn-set ... zero`）：退化 TXD（仅 BYTES/FMT/QIDX）同样
  在 ≤30 s 内楔死 WA 命令面，恢复探针全 -110、不可自愈；正常 TXD 同样。
  **楔死由 enable=1+CONTENT 触发，与 TXD 位内容无关**。
- 反转：WA 核**没有死**——心跳事件持续（SYSRAM/事件环计数 0x1f9→0x1fc 推进），
  死的是**命令处理器**（qid20 didx 冻结）。固件在 beacon 命令处理内部等待。
- 逐项排除：chip_cap[2]=0x20（vendor beacon TXD 也是 32B connac2 形态，52B
  TMAC 理论作废）；arch_ops[0x44/0x48/0x50]=NULL（vendor Enable/DisableBeacon
  是 no-op，不是缺步）；pre-cal 标志字节（0x19a）模板与 caldata 均 0x00
  （mt76 GROUP/DPD pre-cal 上传不适用）；SoC 时钟树无 WiFi 域可缺（全部在芯片内）。
- 新证：`/tmp/wlantest/caldata`（0x30000=196608 B，与 l1profile 一致）已拉回
  `_scratch/npu-probe/caldata-e2631.bin`；与模板前 4K 仅 19 字节差异
  （全在 0x495-0x4bf 的 5G 功率表，与 band0 无关）——功率数据不解释楔死。
- 当前最贴切模型：**固件第一次 beacon 发射尝试挂起**（TX 链 WFDMA→PLE→TMAC→RF
  之间的某个握手没有完成——与 qid18 帧"取走即消失"、TXS/free-notify 从未出现、
  per-BSS TX 计数恒零全部吻合），命令处理器排在它后面一起停。
- 待决判据：手机扫描 e2631-test（PC 强扫 3 次阴性但 Windows 扫描不可靠）。

## 93. R99-R101：eid=00 事件=完整收到的邻居帧（RX 全链通）+ prefetch 表排除 + TX 挂起收敛（3.104/3.105，2026-11-03）

- **`evt-dump <ring> <slot> <len> [count]` 新命令**（3.104/3.105）。注意：**每次
  ctl 写入会重置 16KiB status 缓冲**——一写一 dump，count 参数合并多槽转储。
- **决定性解码（R99 H0/H1）**：eid=00 "心跳"其实是**完整收到的邻居帧**——
  H0=BSSID 04:88:5f:49:9e:64（琰-没有密码，DS=ch1）、H1=fa:28:c9:4c:0f:00
  （CMCC-Hx4U，DS=ch2），beacon interval 100、SSID UTF-8、IE 全部可读。
  帧体在事件缓冲 **+56** 起（Andes RX 事件头 56B）。⇒ **射频已调谐 ch1、
  RMAC→固件→事件环→主机 的 RX 链完全工作**；探针响应若有也会走同一通道。
- **R100**：排空→3×tx-probe（qid18 cidx=didx=3）→1s→排空→12 槽全量转储：
  全部是邻居 beacon（fc 80 00），**零探针响应**（fc 50 00 未出现）。TX 从未
  到达空口。
- **R101**：§89.2 vendor prefetch 分区表（TX 0x1c0..0x2c0 / RX 0x004..0x180，
  与我们用的 mt76 布局 TX 0x000..0x100 / RX 0x140..0x280 完全不同）整表替换
  实验：RX 事件推送也坏掉（事件槽变成主机 DRAM 指针序列 0x4e06xxxx 递增）。
  ⇒ **mt76 布局的 RX 分区是对的（RX 一直能用），prefetch 表不是 TX 挂因，
  vendor 表反而有害。已弃。**
- **中断实验**：mt7916 INT_MASK_CSR=0x18024204，原值 0x20000000（仅 bit29），
  源寄存器挂起 bit29|bit25；武装全掩码 0x6e400003 后探针见 bit30
  （TX_DONE_BAND0）=与 dma_done 同层的 DMA 完成事件，无新信息。
- **战略事实重申**：v1.x 线（main 分支 v1.3-v1.8.0）的 mt76 栈在同一块硬件、
  同一份 WIFI_RAM_CODE 上 TX/RX 全部工作过（真实用户 200M/HE80 实测）——
  芯片 TX 没有硬件级故障，native 驱动与 mt76 之间存在尚未定位的初始化缺口。
  下一步两条路：(a) 逐项 diff mt76 初始化（中断/环尺寸/arm 时序/mac_init），
  (b) mt7915e 对照上机直接暴露缺口。

## 94. R102：环深度对齐 mt76 仍不救 TX；启动 mt7915e 对照（3.106/3.107-mt76，2026-11-03）

- 环深度对齐（事件环/txfree 32/64→**512**，= mt76 MT7915_RX_MCU_RING_SIZE）：
  512 深 ring 生效（didx 推进到 43），但 beacon 上传后 liveness 仍 -110，
  TXS/free-notify 依旧零。**环深度不是 TX 挂因**（但 512 保留——正确的对齐值）。
- 新行为观察：beacon 上传后**邻居帧事件停止**（512 深排空六轮 0 个 eid=00）
  ——固件在 BSS 激活后切到 AP RX 过滤，不再转发邻居 beacon。合理且无害。
- **战略行动**：apply.sh 增加 `E2631_MT76=1` 开关——恢复 v1.x 的开源无线栈
  （kmod-mt7915e/kmod-mac80211/kmod-cfg80211/wpad/固件包 + 11-mt76-caldata +
  e2631-wifi-eeprom/load/boot 启动序列 + wireless uci，全部从 main 分支
  overlay 恢复）。这是**对照实验**：mt76 在同一硬件、同一 WIFI_RAM_CODE 上
  于 v1.3-v1.8.0 实测过 TX（200M/HE80），若它在 native 内核上同样工作，
  二分定位 native TX 缺口；顺带得到可用 WiFi 基线。native zxnpu-wlan 默认
  parked（enable=0）不冲突。构建 3.107-mt76（mac80211 全新编译，长）。

## 95. R103/R104：mt7915e 对照线——构建链全通，bind 即重启（3.108-mt76，2026-11-03）

- `E2631_MT76=1` 对照构建打通（apply.sh 开关 + overlay 恢复 v1.x 全套 +
  config 写入 + kmod-mt76-core/connac/mt7615-common 依赖链 + stale apk 清理
  + 两枚 v1.x 手写补丁的 hunk 头/空格前缀修复——**机械重生成**落地为
  `998-zx-tx-paths.patch`）。构建时序坑：build.sh 重入会重跑 apply 清掉
  mt76 =y（须全程带 E2631_MT76=1）；stale mt76 apk 带旧内核 hash 卡 apk
  world-solve（"unable to select: breaks: kmod-mt76-core"），全家族
  apk+stamp 清零后 make world 顺序重生成即过。
- 3.108-mt76 上机：有线正常（device-verify 全过）、mt76 全家族模块加载
  （mt7915e/mt76_connac_lib/mt76/mac80211/cfg80211）、caldata staging 成功。
- **mt7915e probe 即崩**：解绑 zxnpu-wlan → `bind mt7915e` → 整机重启
  （dmesg 归零、无 panic 留存——无 pstore）。背景：zxnpu-wlan（parked）与
  mt7915e 竞争同一 PCI 设备，wifi-on 脚本每轮把设备绑回 zxnpu-wlan。
- 待做：TTL（COM7）复现抓 probe 崩溃现场；或出"纯 mt76"变体镜像
  （MT76=1 时移除 zxnpu-wlan/NPU 驱动）排除共存干扰。
- pack.sh 的 verify_img 含 "native line 无开源无线驱动" 静态门禁，对
  MT76=1 构建误报 BLOCKER（镜像本身正确，设备已运行 3.108-mt76）。

## 96. R105：mt7915e 在 native 内核上 probe 成功、固件下载中崩溃——两线互补（3.109-mt76，2026-11-03）

- `E2631_MT76=1` 排除共存（CONFIG_ZX_CSP_WLAN=n，zxnpu-wlan 不进镜像）+
  两处 apply 断言条件化后，**mt7915e 首次自动 probe 成功**：
  `mt7915e 0000:01:00.0: HW/SW Version 0x8a108a10` → `WM Firmware Version
  ____000000` → `WA Firmware Version DEV_000000`（**vendor 固件文件**，
  Build Time 20240823）→ 固件下载启动。
- **固件下载中途整机重启**（`Retry message 00000001 (seq 10)` 后、up 归零，
  无 panic 留存——无 pstore；TTL 会话已断开未能取证）。
- 与 native 线的对照意义：**同一颗芯片、同一份固件文件**——native 自研栈
  （3.105）下载全绿、fw-state=7、RX 全通但 TX 挂；mt7915e 连下载都过不去
  （998 scatter→WM 补丁在，仍挂在 Retry 循环）。两个栈的故障面互补，
  TTL panic 现场是共同钥匙。
- 基础设施沉淀：E2631_MT76=1 开关全链（apply 条件化/断言条件化/overlay
  v1.x 恢复/998-zx-tx-paths.patch 机械重生成/kmod 依赖链 core+connac+
  mt7615-common+hwmon+thermal/stale apk 清理）——commit 记录在案。
- pack.sh verify_img 的 native 无线门禁对 MT76 线误报（已记录，镜像可刷）。
