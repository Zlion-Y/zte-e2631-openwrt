# 58 — native-wired-3.4：关联状态机骨架 + cipher 命名（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.4/e2631-openwrt-native-wired-3.4.bin` |
| sha256 | `728724ebf4b4167cb70b3c9e045fe3826ae2dce4857446a345cc2d2b6b3c9dbc` |
| 版本 | `OpenWrt native-wired-3.4` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 交付（P5 关联管理骨架，doc 38 §3 R4j 收口）

1. **sta-add → BSS 联动**：分配 wcid 时将 STA 关联到 band 的 BSS
   （zxna_bss[band].active = true）——关联状态机第一步
   （free → associated → BSS 链接）；
2. **`key-set <wcid>`**：密钥镜像标记（fill_wtbl_key_info_struc_v2
   命令路径的 host 侧对位；真实密钥材质 P5）——ctx 镜像 state
   校验（未关联拒绝）；
3. **cipher 命名表**：data-tx 打印补 R4k 映射
   （sec_cfg+96 的 5/13/16 → keyinfo+8 的 1/2/3，WEP/TKIP/AES 类，
   [推断] mt76 命名）。

## 3. 关联状态机现状

free --sta-add--> associated(BSS linked) --key-set--> key-set
--sta-del--> free。802.11 帧处理（auth/assoc 报文解析）在固件起来后
接入（P5）。未刷机。
