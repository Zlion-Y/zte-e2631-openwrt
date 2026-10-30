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
