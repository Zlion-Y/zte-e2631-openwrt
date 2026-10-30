# 40 — R2：l1profile/EEPROM 初始化解析 + native-wired-1.7（2026-10-31）

## 1. 解码结论（R2 任务：对照 /etc/wireless/mt7916 清单逐字段）

vendor 驱动**硬编码** `/etc/wireless/mt7916/l1profile.dat.dbdc`
（mt7916.ko strings + "load l1profile succeed/failed" 日志），L1 profile
解析出 INDEX0 的三组键并据此继续：

| 键组 | 字段 | 语义 |
|---|---|---|
| profile | `INDEX0_profile_path=/var/tmp/mt7916.dbdc.b0.dat;/var/tmp/mt7916.dbdc.b1.dat` | 每 band 驱动 profile（b0=2.4G、b1=5G） |
| EEPROM | `INDEX0_EEPROM_name=caldata`、`offset=0x0`、`size=0x30000` | 校准数据来源：caldata 命名源、192 KiB、偏移 0 |
| 接口 | `main=wlan0;wlan5g0`、`ext=wlan;wlan5g`、`wds/apcli/mesh` | VAP 命名模板（与我们 netifd 命名一致的来源） |
| SKU | `single_sku_path`、`bf_sku_path` | 逐速率功率表 |
| init | `INDEX0_init_script=/lib/wifi/mtwifi.lua` ×2 | vendor 用户态初始化（native 线由 wlanmgr 替代，P5） |

per-band profile（mt7916.dbdc.b0/b1.dat，7.3 KB each）= MTK ProfileDat
键值集（AccessControlList0..15 起头的操作默认值全表）。

## 2. 固件落地

`/proc/zx-csp-wlan/ctl` 新增 **`l1profile`** 只读命令：按 vendor 方式解析
镜像内 `zte/mt7916/l1profile.dat.dbdc`（九个白名单键逐行输出），跟读
b0/b1 profile 报告行数/键数——真机验收第 3 步（EEPROM/caldata 初始化）
的对照基准。纯文件解析，无寄存器访问。

## 3. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.7/e2631-openwrt-native-wired-1.7.bin` |
| sha256 | `7b7ac4a8e704005e7163e194b31000670af8d5cfe94ec10b160d9ca32939aab9` |
| 版本 | `OpenWrt native-wired-1.7` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | 驱动源码 `l1profile: %zu bytes` = 1 |

未刷机。

## 4. R3（同日接续）：zxnative-eth ethtool 统计 + PPE offload 骨架

- `zxna_ethtool_ops`：get_drvinfo + 两个真实计数器（tx_pending、
  tx_done_excess）的 strings/sset_count/ethtool_stats；
- `zxna_setup_tc`：TC_SETUP_FT 入口骨架——`zxnative_eth.offload=0`
  （默认，0444 只读参数）时直接 -EOPNOTSUPP；offload=1 也只打印编程
  计划（45B key/双 CRC32 bank/127 流上限/仅 IPv4——doc 16 §4.2 纪律：
  永不清 IPv6 trap 半边）并拒绝——**分类器零触碰**，遵守 Qoder 顺序线
  （清单 36 第 2 步通过前不动 offload 实编程）；
- `.ndo_setup_tc` + `netdev->ethtool_ops` 接线完成 → 1.8 构建。
