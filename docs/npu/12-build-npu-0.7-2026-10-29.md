# npu-0.7 构建记录：CLA 哈希流表规格与硬件自检（2026-10-29）

> 本版不再猜 A 侧流表格式。它把原厂 `tm.ko` 的寄存器表、哈希多项式
> 选择器、45 字节 key、以及 520 个哈希槽的放置顺序全部钉到指令级，
> 并把同一套算法做成 `/proc/zx-npu-ppe/cla` 的只读自检（`poly` /
> `slots` / `entry`），写入仍默认关闭。结论与基树里已经在跑的
> `zx279128s-ppe.c` 完全吻合——A 侧硬件 offload 不是待做项，而是
> 需要可观测性。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/npu-0.7/e2631-openwrt-npu-0.7.bin` |
| 大小 | 20,972,052 B |
| sha256 | `bcadc2012cd360fa6db3858480c108b6f55d0c27633600cc3b6e063a6750fcbf` |
| 版本 | `OpenWrt npu-0.7` |
| verify_pkg / verify_img | ALL CHECKS PASSED / BOOT-READY |
| 新增内核物证 | `zx279128s-npu-ppe.o` 里有 `cla_poly(%08x):`、`slot: bank0=%u bank1=%u`、完整命令列表 |

未刷机。

## 2. `claRegTable`：原厂寄存器描述符表

`tm.ko` 的 `claRegTable`（GLOBAL，.data+0x70，83 项 × 28 字节）是
`tmOnuRegRead/Write` 的唯一地址来源。每项：

```
u16 rid; u16 pad; u32 kind; u32 base; u32 mask;
u16 shift; u16 max; u32 stride; u32 counter;
```

`base` 不是绝对地址，而是 **4 字节单位** 的块内偏移；物理地址 =
块基址 + base×4。CLA 的关键项（`_scratch3/.../modules/mod_tm.ko`，
偏移 0xA8840 起）：

| rid | base×4 | 物理地址 | mask | 用途 |
|---|---|---|---|---|
| 0 | 0x38C014 | `0x9238C014` | 0x0FFFFFFF | 间接命令 |
| 1 | 0x38C018 | `0x9238C018` | 0x1 | 状态（bit0 = idle） |
| 2 | 0x38C01C | `0x9238C01C` | 0xFFFFFFFF | 数据口，stride 1，max 17 |
| 3 | 0x38C080 | `0x9238C080` | 0x000FFFFF | `cla_set_config` |
| 12 | 0x38C090 | `0x9238C090` | **0x00FF00FF** | 哈希多项式选择 |
| 13 | 0x38C094 | `0x9238C094` | 0x0000000F | outer hash 空间配置 |

这与 `06-register-diff` 里由调用点反推的 `+0x14/18/1C/80` 一致，
但 rid 12/13 是这一版新钉死的。

## 3. 哈希多项式选择器（CLA+0x90）

`cla_get_hash_poly_config`（0x11AC）读 rid 12，把寄存器拆成 8 个
2 位字段：

```
hash0..3        = bit[1:0]  [3:2]  [5:4]  [7:6]
outer_hash0..3  = bit[17:16][19:18][21:20][23:22]
```

硬件按 hash 类型选其中一个发生器。软件侧对应的 4 张 256 项表在
`tm.ko` 的 .data 中（.data+0xAEC / 0xEEC / 0x12EC / 0x16EC），首项
`table[1]` 就是多项式本身：

| 选择 | 多项式 | 名称 |
|---|---|---|
| 0 | `0x04C11DB7` | CRC-32 |
| 1 | `0x1EDC6F41` | CRC-32C |
| 2 | `0xF4ACFB13` | CRC-32/AUTOSAR |
| 3 | `0x32583499` | 第四张表 |

表是标准 MSB-first 形式（`table[0]=0`，`table[1]=poly`，
`table[2]=0x09823B6E`…）；`t[i]` 等于“单字节 i 的 CRC”，因此可在
驱动 probe 时用逐位算法生成，不必内嵌 4 KiB vendor 数据。

## 4. 45 字节 key 的位布局

`cla_acl_hash_addr_gen`（0x6CDC）先把 45 字节 key **倒序**拷进局部
缓冲区（`key[44]` 落在最低地址），再做

```
crc = table[(crc >> 24) ^ byte] ^ (crc << 8)   // 45 次
hash = crc & 0xFFFF
```

等价于“按 key 字节从 44 到 0 走表”。字段本身由
`tm_acl_fast_add_v4v6`（0x4C04C）从规则缓冲的 20 个 16 位字段拼出；
基树 `zx279128s-ppe.c` 的 `zx_ppe_slots()` 已经给出可直接落盘的子集：

```
bit 23..30   extract rule id（IPv4 TCP/UDP = 0x98）
bit 32       direction（0 = 上游）
bit 33..     7 × 16 位：proto, sip>>16, sip&0xffff, dip>>16,
             dip&0xffff, sport, dport
```

其余字段（outport/inport/tag_level/l2_type/pppoe/ex_rule_id/
extra_data0..19）由 `cla_acl_hash_addr_gen` 的调试打印命名：
`hash_mode, outport, inport, tag_level, l2_type, pppoe_flag,
ex_rule_id, ex_rule_mode, direct, extra_dataN`。W1 只需要 IPv4
5 元组这一条，已经足够。

## 5. 520 个哈希槽与写入协议

`cla_set_hash_table`（0x5A14）把 0..519 映射到 5 张表：

| 槽 | 表 | 表内下标 | 容量 |
|---|---|---|---|
| 0..255 | 2 | 0..255 | 256 |
| 256..383 | 3 | 0..127 | 128 |
| 384..447 | 4 | 0..63 | 64 |
| 448..511 | 5 | 0..63 | 64 |
| 512..519 | 6 | 0..7 | 8 |

每槽 60 字节 = 15 个 32 位字，硬件按 **字 14 → 字 0** 接收。
写入序列与基树 `zx_pp_write()` 相同：等 idle（CLA+0x18 bit0=1）→
写命令 `index | table<<22`（读加 bit27）→ 写数据口 → 再等 idle。
`aclGetAvailableHashAddr`（0x4B988）按 type 0/1/2 扫描：
type 0 依次尝试 poly0..3，槽 = 哈希值 & (256/128/64/64) 再加基址；
找到空位后把 0x80010000 或进槽号（所以返回值为负才是成功）。
type 2 是 512..519 的 8 槽扩展表。

`tm_acl_get_fastHashRule`（0x46FE8）产出那 60 字节；其中
`rule[19..57]` 是 key 的 20 个 16 位字段来源，`rule[115]/[116]`
存放 ext idx 的两个半字节（`cla_set_external_hash_table` 的入参）。

## 6. 与基树有线 offload 的对照

基树 `zx279128s-ppe.c`：

- `zx_ppe_slots()` 用同样的 45 字节 key、同样的“倒序 CRC”；
- 槽 0 = `crc32 & 0xff`，槽 1 = `256 + (crc32c & 0x7f)`，
  正好是原厂 type 0 的前两次尝试；
- `zx_ppe_hash_entry()` 的 w0..w8 就是原厂 60 字节规则的前 36 字节；
- 表 2/3、17 字数据口、`0x44` 常量都与 `tm.ko` 对上。

结论：**A 侧硬件流表已经实现且在发布版里工作**。npu-0.7 的价值是
把“为什么是这些值”固定在原厂指令上，并提供在线核对手段。

## 7. 接口

`/proc/zx-npu-ppe/cla`（写入不超过 512 字节）：

| 命令 | 作用 |
|---|---|
| `dump` | CLA+0x80/88/90/94/C8/CC/D0 + 状态 |
| `poly` | 解码 8 个多项式选择器与 outer 空间配置 |
| `slots <proto> <sip> <dip> <sport> <dport>` | 打印 45B key、4 个哈希、bank0/bank1 槽号，并读回两个槽的 15 个字 |
| `entry <slot>` | 读回任意槽（0..519） |
| `read/write <table> <index> [value]` | 原始间接访问；写需 `enable=1` |
| `entryw <slot> <60 hex bytes>` | 整槽写入；需 `enable=1` |

只读命令不需要 enable；`slots` 读回的是**当前硬件里真实存在的
规则**，可以和 `tc` 装进去的流逐字节对比。

## 8. 边界

- 仍未做：把任意 5 元组**装进**硬件（60 字节规则的动作字段尚未
  全解）、外部 ext 表（表 6 之外）、老化/统计。
- 模块默认 `enable=0`；`slots` 只读，不会动原厂/基树已装的表。
- 未刷机；上机核对顺序见 `11`/本文第 7 节。
