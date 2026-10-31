# 60 — native-wired-3.5：BSS 配置文件读写骨架（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.5/e2631-openwrt-native-wired-3.5.bin` |
| sha256 | `f33d5a9d447cafb4e14a4674393f117e51ab1dd64f1b4427d9a8dc1075aa0eda` |
| 版本 | `OpenWrt native-wired-3.5` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 交付（P5 wlanmgr BSS 配置文件骨架，doc 38 R6）

- **`wlanmgr conf-write <band> <key> <value>`**：INI 风格 BSS 配置
  文件（/etc/wireless/zte-native.conf）的创建/更新——key 按_vendor
  dat 风格（SSID/Channel/AuthMode/HT_BW/...）；
- **`wlanmgr conf-read <band>`**：按 band section 读取；
- **离线验证**：b0.SSID=ZTE-Native + b0.Channel=6 写入/读回 ✓、
  b1.SSID=ZTE-5G 写入/读回 ✓（section 分离）；
- WLANMGR_CONF 环境变量覆盖（离线测试用），真机默认
  /etc/wireless/zte-native.conf。

## 3. P5 wlanmgr 骨架清单（全就绪）

status/expect/l1p/rings/plane/recv/drecv/fwstate/probe/profile/bssplan/
conf-read/conf-write/step1/2/3/commit/selftest——**18 个子命令**，
覆盖只读面/FWDL 链/数据面/BSS 配置/关联管理全操作界面。
