# 54 — native-wired-3.0：BAND1 TX 环 + wlanmgr profile/BSS 计划（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.0/e2631-openwrt-native-wired-3.0.bin` |
| sha256 | `8e9f8a026d783d7327dc2305c63f63ffc3643a2f0330b589024c6746bd19ad0a` |
| 版本 | `OpenWrt native-wired-3.0` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 测试 | wlanmgr 离线测试 13/13 ok |

未刷机。

## 2. 交付（doc 38 R4 收尾）

1. **BAND1 TX 数据环（qid19）**：分配（64 深 × 36B）并由 data-init
   编程（BASE/CNT/CPU_IDX=0）——与 BAND0 qid18 同形状、同 TXD 缺口；
   dataplane 映射更新（qid19 在位、RX data qid2 标注 P5）；
2. **wlanmgr profile/bssplan 子命令**：
   - `wlanmgr profile b0|b1 [key]` —— 读 vendor per-band profile
     （/etc/wireless/mt7916/mt7916.dbdc.<band>.dat），支持键查找与
     运营键（SSID/Channel/AuthMode/HT_BW/VHT...）摘要；
   - `wlanmgr bssplan` —— BSS bring-up 计划打印（band↔profile↔
     EEPROM 对照 + TMAC/wcid 证据引用 + 顺序：fw-state=7 → BSS 配置
     → carrier on）。

## 3. 状态

- 离线解码/骨架全闭环（doc 37-53 + 本篇）；
- L2-L6 真机闭环等用户解除禁刷放行清单 36（wlanmgr step1/2/3 +
  profile/bssplan 为验收操作界面）。
