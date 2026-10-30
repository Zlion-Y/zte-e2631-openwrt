# 53 — R4h：0x1B3448 已关联分支 + TMAC 家族补全（2026-10-31）

## 1. 0x1B3448 已关联分支解码（ctx+0 魔数相等路径）

```
1b3448: ldrh r2, [r1, #240]      ; ctx+240 (u16) = band 索引
1b3450: mla r8, 0x688, r2, r8    ; 第二级块 = arg5 + 0x688 × band_idx
1b3454: add r8, r8, #0x59000
1b3458: ldrb r2, [r8, #0xB85]    ; band 状态块 + 0xB85 字节
1b345c: orr r3, r3, r2, lsl #24  ; → TXD w0 bits24+（保护标志）
1b3460: b 0x1b3140               ; 回主路径
```

**语义修正**：已关联（ctx 魔数匹配）时，TXD w0 bits24+ 的保护标志来自
**band 状态块**（per-band 块 stride 0x688 与 AndesCtrlInit 的 band 块
布局一致），不是 per-STA 密钥块——保护是 band 级配置（AP 的
encryption mode）。

## 2. TMAC 家族补全

- `mtf_write_tmac_info_by_host`(0x1B3494)：host 托管变体——32B TMAC
  memzero、HcGetEdca 取 EDCA 参数、r5+16/+20/+54/+80 为 host 提供的
  TX 参数——固件 offload 帧的 TMAC 构造路径；
- `fill_key_install_cmd_v2`(0x1DC644)：密钥安装走 **MCU 命令路径**
  （os_alloc_mem + 命令构造）——密钥材质在固件侧，host ctx+8 块是
  镜像，由 host 密钥管理路径（SetKey 族）填充（R4i）。

## 3. per-STA ctx 布局现状（R4g+R4h 累计）

| ctx 偏移 | 内容 | 用途 |
|---|---|---|
| +0 (u32) | 状态魔数 0x00040001 | valid/associated 判定 |
| +8 (指针) | → host 密钥/保护镜像块（+41 字节 → TXD w0 b24+，未关联兜底） | 密钥状态镜像 |
| +240 (u16) | band 索引 | 已关联时定位 band 状态块 |

## 4. 下一步

- R4i：host 密钥管理路径（SetKey 族 → ctx+8 镜像填充）追溯；
- P5 落地时 wcid 分配器 + ctx 维护按此布局实现；
- L2-L6 真机闭环等用户解除禁刷（清单 36 已脚本化：wlanmgr step1/2/3）。

## 5. R4i 增补（同日）：ctx 写入者家族定位

**`ApUpdateCapabilityAndErpIe`**（对象前部 0x7xx-0xa6c 区）= per-STA ctx
的写入者家族：`HcGetMaxStaNum` 循环遍历 wcid（`mla 0x1C50×wcid + base`
同款地址算式）更新能力字段；ctx+0 魔数 0x00040001 = 能力字
（0x0004/0x0001 位组合，`orr r5,#4` 之类的位操作族在此维护）；区域还
含 slottime/ERP 更新与 per-band 交互（HW_SET_SLOTTIME、movt 189=0xBD
的 band 块引用）。

- 密钥材质路径已定性：fill_key_install_cmd_v2 → MCU 命令（固件侧持有
  真密钥）；host ctx 是能力/状态镜像；
- R4j（可选深化）：能力位 0x0004/0x0001 的逐位命名 + SetKey 族与 ctx
  镜像的同步点；
- P5 落地：wcid 分配器 + ctx 维护按此布局（状态魔数 + 能力字 + 密钥
  镜像指针）。

## 6. R4j 尝试与关闭（2026-10-31）：能力位逐位命名

尝试解析 write_tmac_info_offload_pkt 尾部 debug 块的逐位 printk 名称：
`.LC29-38` 符号在符号表**跨 section 重复**（.rodata.str1.4 0x2b8 组 =
mbss/BssCoex 字符串；另一组 0x1cc0 = TMAC 块）——objdump 重定位只给
符号名，归属歧义，离线无法可靠对应。

**决策关闭 R4j**：逐位命名是 debug 装饰，不影响 TXD 构造正确性；真机
首帧对照（清单 36 后）比静态猜名更可靠。SetKey 同步点已定性
（fill_key_install_cmd_v2 → MCU 命令；host ctx = 镜像）。

## 7. R4i 续（同日）：密钥安装命令结构（fill_key_install_cmd_v2 链）

`fill_key_install_cmd_v2`(0x1DC644) → `fill_wtbl_key_info_struc_v2`
(0x11EDCC)：

- **命令头**（区别于 Andes 24B 头）：byte0 = **0x11（WTBL key 命令）**、
  byte1 = 0、+2（u16）= 总长；长度按密码类型伸缩：
  - 基本 8B（无 .30/.480/.1536 位）→ 44B（WEP/TKIP 类）→ +36（PMF）
  → 再 +36（第二密钥位 0x600）；
- **密钥信息字段**：+4/+5/+8/+9/+10/+11 = 密码类型相关字段
  （cipher id / key 索引 / 长度），模板 +96/+163 = 密钥材质来源；
- os_alloc_mem(r7) → 填充 → 上层继续封装为 MCU 命令。

完整字段矩阵（cipher id 枚举、key material 偏移）= R4k；真机首帧对照
后按需补全（与清单 36 的关联步骤同步）。
