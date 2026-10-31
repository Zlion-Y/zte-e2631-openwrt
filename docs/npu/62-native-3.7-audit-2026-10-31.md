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
  （lan1/2/3=f8:73:1a:d3:ef:a8，wan=...a9）；device-verify ALL-PASS。
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
