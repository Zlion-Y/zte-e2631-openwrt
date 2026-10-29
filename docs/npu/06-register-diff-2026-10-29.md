# P1 register-diff：E2631 vs ZX279133 参考（第一版，2026-10-29）

> 目标：在写任何 vendor-native 驱动寄存器之前，先把 E2631 与 cnjn ZX279133
> 参考的地址/偏移/语义差异钉死。本文只做**差异表 + 未知项 + 只读审计计划**，
> 不写寄存器、不上机。
>
> 证据等级：`[实测]` 运行态；`[实测-静态]` 反汇编/符号/寄存器表；
> `[推断]` 由证据推断；`[未知]` 没有足够证据。

## 0. 结论

1. **同一引擎家族，不是同一套地址**：E2631 的 NPP/IDM/TM/PP 与 ZX279133 参考
   在块基址、块内偏移、IDM 队列模型上都不同。参考驱动的**算法/结构**可复用，
   **常量/偏移不能直接复制**。
2. **IDM 基址语义已确认（与参考一致）**：`+0x08`=RX ring 基址（2048×8B），
   `+0x04`=TX ring 基址（1024×8B，位于 base+0x4000）。ffe-full 的真机
   6018 帧 RX 就是用 `+0x08`=RX 跑通的，ZX279133 参考也是 `+0x04` TX /
   `+0x08` RX。**上一版“`+0x04`=RX B”是误读，已纠正**：`pon_npp_idm_init`
   写的是 base 和 base+0x4000，不是两个 RX 环。
   E2631 与参考真正的 IDM 差异在 doorbell/完成侧：E2631 `+0x40` TX kick/ack、
   `+0x44` TX done、`+0x48` RX 投递 `0x10000`、`+0x4C` RX done；
   参考用 `+0x80/+0x84` TX doorbell、`+0x88` RX_RELEASE、`+0x100` BP_REFILL，
   两套完成模型不能互换。ffe-full 的 TX `inject_ok=0`，所以 TX 侧仍待实测。
3. **E2631 没有 accelerator reserved-memory**：运行态 `/proc/iomem` 只有
   PCIe + System RAM；参考树用 DT 预留 BMU/SE/IDM。E2631 的 BMU/SE/IDM
   地址/大小必须从 vendor 驱动反推，不能照抄 DT。
4. **IRQ 基本对得上**：E2631 `idm` DTS IRQ 38 → GIC 70 → Linux virq 28，
   与 `/proc/interrupts` 的 `28: GIC 70 idm` 一致；`npp` 35/38、`tm` 36、
   `pp` 37。参考有 5 条中断（nppt/rx/idm/buffer-release/local-test），
   E2631 只看到 4 条活跃，local-test/buffer-release 需再确认。
5. **PPU microcode 在本地没有找到**：参考用 `zte/zx279133/mcode_intel.bin`
   （103456 B，CRC32 `0xef1d7647`）；E2631 vendor 包/模块里没有同名文件，
   需要从原厂 rootfs 只读拉取或确认它是否内建。

## 1. 顶层地址表

| 块 | E2631 | ZX279133 参考 | 判定 | 证据 |
|---|---|---|---|---|
| NPP/GREG | `0x921C0000` | NPPT `0x19000000` | 同功能、不同基址 | E2631 硬件视图；参考 DTS `zx279133.dtsi:538` |
| IDM | `0x921C8000`（NPP+0x8000） | `0x19280000`（NPPT+0x280000） | **不同偏移** | 硬件视图/`re-notes-tm.md:103`；参考 `zx279133.h:367` |
| SIPC | `0x921CC000`（NPP+0xC000） | `NPPT+0x4000`（`SIPC_CFG 0x4000`） | **不同偏移** | `re-notes-tm-regtables.txt:259`；参考 `zx279133.h:73` |
| SPA | `0x921D4000`（NPP+0x14000） | `NPPT+0x8054`（`SPA_IND_CMD`） | **不同偏移** | `re-notes-tm.md` §3.1；参考 `zx279133.h:338` |
| TM | `0x92340000` | 参考常量相对 NPPT（如 BMU `0x3c000`） | **不同布局** | 硬件视图；参考 `zx279133.h:92` |
| BMU | `0x92348000`（TM+0x8000） | `NPPT+0x3c000` | **不同偏移** | `re-notes-tm.md` §6.1；参考 `zx279133.h:92` |
| QMG | `0x9234C000`（TM+0xC000） | `NPPT+0x24000` | **不同偏移** | `re-notes-tm.md` §3.3；参考 `zx279133.h:111` |
| SCH | `0x92354000`（TM+0x14000） | `NPPT+0x2c000`（SSCH） | **不同偏移** | `re-notes-tm.md` §3.3；参考 `zx279133.h:180` |
| RED | `0x92344000`（TM+0x4000） | `NPPT+0x20004` | **不同偏移** | `re-notes-tm.md` §3.2；参考 `zx279133.h:133` |
| PP | `0x92380000` | PPS `0x18000000` | 同功能、不同基址 | 硬件视图；参考 DTS `zx279133.dtsi:541` |
| CLA | `0x9238C014/18/1C`（间接 cmd/done/data） | `PPS+0x500c8/0x50208/...` | **不同偏移** | `re-notes-tm-regtables.txt:1`；参考 `zx279133.h:310` |
| SBRG | `0x92388014`（间接 cmd） | PP+0x8000 家族（DSA `rtl8372n`） | **可能同块内偏移，待核** | `re-notes-tm-regtables.txt:64`；参考 DSA 驱动 |
| PM | `0x9239C014/18/1C` | 参考 `zx279133.h` 未列同一组常量 | **需重推** | `re-notes-tm-regtables.txt:251` |
| ADM/DPA | `0x92394000` / `0x92398000` | 参考 TM/PP 内部常量 | **需重推** | E2631 寄存器表；参考 `zx279133.h` |
| SMAC | `0x92200000 + n*0x10000` | XMAC0 `0x140000` / XMAC1 `0x180000`（相对 NPPT） | **不同布局** | `re-notes-tm-regtables.txt:263`；参考 `zx279133.h:452` |
| PON | `0x92000000` | 参考 NPPT 内部/SerDes 不同 | **不同布局** | 硬件视图 |

## 2. IDM 差异（最高优先级）

### 2.1 寄存器语义

| 项 | E2631 vendor（已反汇编确认） | ZX279133 参考 | 判定 |
|---|---|---|---|
| RX/TX ring 基址 | `+0x08` = RX（2048×8B）；`+0x04` = TX（1024×8B，base+0x4000） | `+0x08` = RX base；`+0x04` = TX base | **基址语义同源**；E2631 TX doorbell/done 不同 |
| TX ring | 1024×8B 软件环；`+0x40` 写 `count<<16`（kick）/done ack；`+0x44` 读低 16 位完成 | 4 个 TX 队列×1024；doorbell `+0x80/+0x84`，队列 n>0 用 `4*(n+39)/4*(n+42)` | **不同模型** |
| RX 投递 | `+0x48` 逐 desc 写 `0x10000`（不是缓冲地址） | `+0x88` RX_RELEASE（`rx_work | queue<<12`） | **不同；E2631 已定** |
| BP 重填 | `+0x48` 投递 + 内存自由环；初始 512 个 skb | `+0x100` BP_REFILL | **不同** |
| RX 完成 | `+0x4C` 低 16 位 | 参考用 `+0x88` release/完成路径 | **不同；E2631 已定** |
| 开关/中断 | `+0x24` 初始 31；open/stop 对 bit2 做 RMW | INT_CPU `+0x44`、mask `+0x40` | **不同** |
| 队列模型 | 2 个 netdev `idm0/idm1`；运行态通道 0/2 在用 | 24 RX / 16 CPU RX / 4 TX；RX 2048、BP 4096 | **不同** |
| 缓冲 | vendor `idm_skb_stack` + skb 表；运行态有 push/pop 计数 | page_pool + 2048×0x940 stride + BP ring | **不同实现** |

**结论**：参考 `zx279133-idm.c` 的环形引擎**不能直接移植**；只能复用
page_pool/NAPI/描述符消费的工程结构，寄存器序列必须按 E2631 `plat` 重新转写。

### 2.2 消解证据（2026-10-29）

用 `annot.py plat pon_npp_idm_init idm_net_tx idm_net_poll idm_check_tx_done_nolock`
得到带 reloc 注释的反汇编（`_scratch/npu-probe/plat-idm-annot.txt`、
`plat-idm-extra-annot.txt`）：

| 指令位置 | 证据 | 结论 |
|---|---|---|
| `0x66d8/0x66f0` | `npp_base+0x8000+0x08 <- base`；`+0x04 <- base+0x4000` | `+0x08`=RX 基址；`+0x04`=TX 基址（TX 环在 +0x4000） |
| `0x67b0` | `npp_base+0x8000+0x48 <- 0x10000` | RX 投递写常量 `0x10000` |
| `0x63b8-0x63d4` | `dsb sy`；`count<<16` 写 `+0x40` | TX kick = `+0x40` |
| `0x545c-0x5460` | 读 `+0x44` 低 16 位作为完成数 | TX done = `+0x44` |
| `0x5cf4` | 读 `+0x4C` 低 16 位 | RX done = `+0x4C` |
| `0x5504-0x5510` | done 处理后把完成数写回 `+0x40` | `+0x40` 兼作 ack |

**注意**：`re-notes-tm.md` 原写“`+0x04/+0x08` RX A/B”和
“`+0x44` TX kick/done”，两处都要修正：`+0x08` RX、`+0x04` TX；
`+0x40` 写 kick/ack、`+0x44` 读 done。ffe-full 的代码/真机 RX 帧与
ZX279133 参考在基址语义上一致；E2631 与参考的差异在 doorbell/完成模型，
不在 ring 基址。

参考 `zx279133-idm.c:1368-1443` 的 `idm_init()` 仍是 ZX279133 的
队列模型（24 RX/16 CPU RX/4 TX、+0x80/+0x84 doorbell、+0x88/+0x100
release/refill），不能直接套到 E2631 的 `+0x40/+0x44/+0x48/+0x4C`。

## 3. TM / BMU / QMG / SCH / RED

| 项 | E2631 | ZX279133 参考 | 判定 |
|---|---|---|---|
| BMU 基址 | `TM+0x8000 = 0x92348000` | `NPPT+0x3c000` | 不同 |
| BMU 内存 | 运行态无 iomem 保留；vendor 自由栈基址 `0x0104C040`；`u32_BP_SIZE` 等运行期全局未取 | DT 预留 BMU `0xecc000`；normal 0x1800×0x840、jumbo 0x20×0x2600、desc 0x200000 | 不同 |
| BMU 分配 | `+0x14` 触发，轮询 bit0-3，读 `+0x0C` | 参考 `BMU_ENABLE 0x3c000` 等 | 不同偏移 |
| QMG | `TM+0xC000` | `NPPT+0x24000` | 不同 |
| SCH | `TM+0x14000` | `NPPT+0x2c000` | 不同 |
| RED | `TM+0x4000` | `NPPT+0x20004` | 不同 |

E2631 的 `re-notes-tm.md` §6.1 已给出 vendor `pon_tm_bmu_init` 的
基址寄存器（TM+0xE8/0xEC/0xF4/0xF8/0xFC）和 `0x0104C040` 自由栈；这些是
E2631 的真实值，不能换成参考的 DT 预留几何。

**`pon_tm_bmu_init` 反汇编补强**（`annot.py plat pon_tm_bmu_init`，
`_scratch/npu-probe/plat-idm-annot.txt`）：

| 偏移（TM+0x8000 之后） | 写入 | 证据 |
|---|---|---|
| `+0x04` / `+0x08` | `0x0104C040`（normal/jumbo 自由栈基址） | `movw 49216; movt 260` -> `0x0104C040`；`str` 到 +0x04/+0x08 |
| `+0x58` / `+0x5C` | `u32_BPPE_POOL_SIZE` / `u32_JUMBO_BPPE_POOL_SIZE` | `ldr` 全局后 `add #88/#92` |
| `+0xE8` | `pdt_mem_size - 0x40000000` | `add #-1073741824` |
| `+0xEC` | `pdt_mem_size - 0x40000000 + 0x10000` | `+65536` |
| `+0xF4` | `pdt_mem_size - 0x40000000 + 0x520000` | `+5373952` |
| `+0xF8` | `u32_BP_SIZE` | `str` 到 +248 |
| `+0xFC` | `BP_SIZE | JUMBO_BP_SIZE << 16` | `orr r2,r1,lsl#16` |
| `+0x48` / `+0x4C` | `u32_BPPE_POOL_SIZE` / `u32_JUMBO_BPPE_POOL_SIZE`（第二组写口） | `add #72/#76` |

`pon_tm_queue_init` 用 `pdt_mem_size - 0x40000000 + 0x520000` 起、
`+0x5E0000` / `+0x5F0000` 两段描述区；与笔记一致。
`u32_BP_SIZE`/`u32_BPPE_POOL_SIZE`/`pdt_mem_size` 的具体值仍是运行期全局，
需要只读探针读 `BMU+0x58/0x5C`/`+0xFC` 或从 `pdt_mem_size` 导出确认。



## 4. PP / CLA / SE / PM / SBRG

| 项 | E2631 | ZX279133 参考 | 判定 |
|---|---|---|---|
| CLA 间接口 | `0x9238C014` cmd、`+0x18` done、`+0x1C` data | `PPS+0x500c8`/`+0x50208`/... | 不同偏移 |
| CLA hash/规则 | `tm_acl_get_fastHashRule` 0x48c68；45B key CRC16 double bank | `zx279133-offload.c` ZCAM/DDR/IKEY | 算法同源、布局需重推 |
| SBRG 间接口 | `0x92388014` cmd、`+0x18` done、`+0x1C` data | DSA `rtl8372n` 使用 PP+0x8000 家族 | 可能同块内偏移，待核 |
| PM 间接口 | `0x9239C014` cmd、`+0x18` done、`+0x1C` data | 参考 PM 常量未对齐 | 需重推 |
| WANID | vendor `(16386+wanid)<<7`，32 项；上游 0-15/下行 16-31 | `WANID_COUNT 32`、同方向语义 | **语义可能同源，偏移需核** |
| PPU microcode | 本地未找到 E2631 版本 | `mcode_intel.bin` 103456B，CRC32 `0xef1d7647` | **未知，需拉取/确认** |

参考的 `zx279133-offload.c` 表构建逻辑（16B key、ZCAM 4×5×256、DDR
bucket、IKEY 4096、age 2^18、stat 1024）可作为**算法参考**；但 E2631 的
CLA/SE 基址和表项宽度要按 vendor `tm.ko`/`plat` 重新确认。

### 4.1 E2631 关键间接口的物理地址（已确认）

来源：`tm_regtables.txt` + `tm.dis.txt` 的 `cla_set_config`、
`cla_set_indirect_rw_cmd/data`、`spa_set_hashram/matchram`、
`pm_set_indirect_rw_cmd`、`sbrg_set_indreg_cmd`（注释在
`_scratch/npu-probe/tm-pp-annot.txt`）。

| 功能 | E2631 物理地址 | 参考对应 | 判定 |
|---|---|---|---|
| CLA 间接 cmd / done / data | `0x9238C014` / `0x9238C018` / `0x9238C01C` | `PPS+0x500c8` / `+0x50208` / ... | 不同 |
| CLA config（rid3） | `0x9238C080` | `zx279133.h` 无同址 | 不同 |
| SBRG 间接 cmd / done / data | `0x92388014` / `0x92388018` / `0x9238801C` | DSA 的 PP+0x8000 家族 | **布局可能同源，待核** |
| PM 间接 cmd / done / data | `0x9239C014` / `0x9239C018` / `0x9239C01C` | 参考 PM 常量未对齐 | 不同 |
| SPA 间接 cmd / status / data | `0x921D4014` / `0x921D4018` / `0x921D401C` | `NPPT+0x8054/0x8058/0x805C` | 不同 |
| SPA hash RAM / match RAM | 经 `spa_set_indirect_rw_cmd/data` 写 `SPA+0x14/0x18/0x1C` | 参考 SE/CLA 侧 | 不同接口、算法需重推 |

`cla_set_config` 不是直接写寄存器：它把 14 个输入位拼成 `r1`，再调
`tmOnuRegWrite(rid=3, ...)`，最终落到 `0x9238C080`；具体位序见
`tm.dis.txt:0x838-0x888`。`spa_set_hashram`/`matchram` 调
`spa_set_indirect_rw_cmd/data`，命令字/数据字仍需逐位转写。
`pm_set_indirect_rw_cmd` 和 `sbrg_set_indreg_cmd` 都经 `tmOnuRegWrite`，
表项分别落在 `0x9239C014` 和 `0x92388014`。

### 4.2 命令字格式（E2631，`[实测-静态]`）

`cla_set_indirect_rw_cmd`（0x44c）、`spa_set_indirect_rw_cmd`（0x27b44）、
`pm_set_indirect_rw_cmd`（0x230c0）都按同一模式组装：

```
cmd = index | (table << 22) | (rw << 27)
```

| 块 | 范围检查 | 写入寄存器 | 数据口 |
|---|---|---|---|
| CLA | table 0..9；index ≤ 0x400000 | CLA+0x14 (`0x9238C014`, rid0) | CLA+0x1C (`0x9238C01C`, rid2) |
| SPA | table 0..5；index ≤ 0x400000 | SPA+0x14 (`0x921D4014`, rid4) | SPA+0x1C (`0x921D401C`, rid6) |
| PM | table 0..4；index ≤ 0x400000 | PM+0x14 (`0x9239C014`, rid0) | PM+0x1C (`0x9239C01C`) |
| SBRG | table 0..4；index ≤ 4096；另带 bit31 选择位 | SBRG+0x14 (`0x92388014`, rid19) | SBRG+0x1C/0x20/0x24 (rid76/77/78) |

`cla_set_config` 的 14 个输入位到 `0x9238C080` 的位序（`[实测-静态]`，
`tm.dis.txt:0x838-0x888`）：

```
in[0] -> bit19   in[1] -> bit17   in[2] -> bit16   in[3] -> bit15
in[4] -> bit14   in[5] -> bit13   in[6] -> bit12   in[7] -> bit11
in[8] -> bit10   in[9] -> bit9    in[10] -> bit3   in[11] -> bit2
in[12] -> bit1   in[13] -> bit0
```

`bit18`/`bit4..8` 未被这组输入覆盖，可能是保留/由其他路径写。
这些位语义尚未命名，P2 前要用 vendor 调用点（`cla_set_config` 的调用者）
和运行态基线对照。

参考 ZX279133 的 SE/CLA/PM 命令寄存器（`PPS+0x500c8`、
`SMMU0 0x48004`、`PPU cluster 0x90d00`）与 E2631 不同；算法可参考，
命令字/表项不能照抄。

## 5. IRQ / 时钟 / 内存

### 5.1 IRQ

E2631 运行态（`/proc/interrupts`）：

```
27: GIC 67  pon_npp
28: GIC 70  idm
29: GIC 68  pon_tm
30: GIC 69  pon_pp
```

DTS：`npp@921c0000` IRQ 35,38；`tm@92340000` 36；`pp@92380000` 37；
`idm@921c8000` 38（disabled 节点）。IRQ 38 → GIC 70 → Linux virq 28，
与运行态一致。参考 DTS 有 5 条：nppt/rx/idm/buffer-release/local-test；
E2631 只看到 4 条活跃，**local-test/buffer-release 需再确认**。

### 5.2 时钟/复位

E2631 vendor 用 `zx_ponreg.ko` + `plat` 的 `apb_read/write`；参考用
`topcrm`/`lsp` 时钟框架。E2631 的 `zx_ponreg` 不是通用时钟驱动，重写时
需要把 vendor 的 gate/reset 序列单独列出来；目前 `[未知]`。

### 5.3 内存/DMA

- E2631：`MemTotal 225324 kB`；`/proc/iomem` 只有 `40000000-4fffffff System RAM`，
  **没有 NPP/IDM/TM/PP 保留区**；vendor 用 `pdt_mem_size`/`0x0104C040` 自由栈。
- 参考：DT 预留 BMU/SE-hash/IDM 三块，**同样使用 32 位 DMA**
  （`zx279133-main.c:395` 的 `DMA_BIT_MASK(32)`）。
- E2631 也是 32 位 DMA，但**没有 reserved-memory**，且多一层
  `phys+0x40000000` 总线别名；所有描述符/缓冲必须 <4GiB。
  这是移植参考 IDM 代码时必须改的第一件事。

## 6. P1 门禁（写寄存器前必须解决）

| # | 门禁 | 判据 | 失败后果 |
|---:|---|---|---|
| 1 | IDM `+0x04/+0x08` 语义 | **[已解]** `+0x08` RX、`+0x04` TX（TX 环 +0x4000）；ffe-full RX 已验证，TX 待实测 | 进 P2 前用只读基线复核 |
| 2 | IDM `+0x40/+0x44/+0x48/+0x4C` | **[已解]** `+0x40` TX kick/ack、`+0x44` TX done、`+0x48` RX submit、`+0x4C` RX done | 已消解，进 P2 前仍用只读基线复核 |
| 3 | BMU/SE/IDM 内存几何 | vendor 自由栈 `0x0104C040` + `u32_BP_SIZE`/`pdt_mem_size` 来源 | 池初始化失败/踩内存 |
| 4 | IRQ 映射 | 4 条活跃 IRQ + local-test/buffer-release 是否存在 | 丢中断/NAPI 不唤醒 |
| 5 | CLA/SE/PM 基址/命令格式 | **[部分解]** 间接口地址 + `index|table<<22|rw<<27` 格式已确认；表项字段语义待解 | 表写到错误地址/字段错位 |
| 6 | PPU microcode | 找到 E2631 的 mcode 或确认无 | 分类器不工作 |
| 7 | WANID/SE 槽位 | 运行态 `idm_map`/`npu_cap` + vendor 表 | 无线伪端口找不到 |
| 8 | 32 位 DMA/别名 | 描述符/缓冲 `upper_32_bits` 检查 | DMA 写错内存 |

## 7. 只读审计计划（下一步）

1. **[已做 plat/tm] 反汇编注释**：`pon_npp_idm_init`（0x6508）、
   `idm_net_tx`（0x61d0）、`idm_net_poll`（0x5c70）、
   `idm_check_tx_done_nolock`（0x5448）、`pon_tm_bmu_init`（0x87e0）、
   `pon_tm_queue_init`（0xaa50）已注释；`tm.dis` 的 `cla_set_config`、
   `cla_set_indirect_rw_*`、`spa_set_hashram/matchram`、
   `pm_set_indirect_rw_cmd`、`sbrg_set_indreg_cmd` 也已注释。见
   `_scratch/npu-probe/plat-idm-annot.txt`、`plat-idm-extra-annot.txt`、
   `tm-pp-annot.txt`。
   命令字格式与 CLA config 位序已转写（§4.2）；下一步是给 table/字段命名，
   并补 `sbrg` 表选择、`spa_set_hashram/matchram` 的数据布局。
2. **只读白名单**：从 vendor 的 `dump_idm_desc_rx`、`dump_desc`、
   `dump_bmu_reg` 等 debug 函数提取“只读寄存器”清单；只读这些寄存器。
3. **审计模块**：在 npu 分支加 `zx279128s-npu-audit.c`，用 `devm_ioremap`
   （不 `request_mem_region`）映射 NPP/TM/PP/IDM，只读白名单，输出
   `/proc/zx-npu/audit`；**不写、不 reset**。
4. **对照**：把读回值与本文表对照；不一致的项先查清，再进 P2。
5. **上机窗口**：审计模块需要一次真机启动；失败只损失一次重启，
   原厂槽和 failsafe 始终保留。

## 8. 机器可读表

`re/npu/register-diff-e2631-vs-zx279133.csv` 收录本文的关键行，字段：

```
block,item,e2631,reference,status,action,evidence
```

`status` 取值：`confirmed` / `conflict` / `different` / `same-family` / `unknown`。

## 9. 证据路径

- E2631 地址/IRQ：`build/docs/refs-local/E2631原厂硬件视图-2026-10-12.txt`
- E2631 寄存器表：`_scratch/re-notes-tm-regtables.txt`
- E2631 IDM/TM 笔记：`_scratch/re-notes-tm.md`
- E2631 端口映射：`_scratch/re-notes-portmap.md`
- E2631 plat/tm 反汇编：`_scratch3/zte-zx279128s-recon-master/docs/tm-objdump/{plat,tm}.dis.txt`
- 参考 IDM：`_scratch/ref-cnjn/kernel/drivers/net/ethernet/zte/zx279133-idm.c`
- 参考常量：`.../zx279133.h`、`zx279133-offload.c`、`zx279133-tm.c`
- 运行时：`docs/npu/01-vendor-runtime-baseline-2026-10-29.md`
