# 57 — native-wired-3.3：per-band BSS 状态骨架（bss-show/bss-set）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.3/e2631-openwrt-native-wired-3.3.bin` |
| sha256 | `ce87c6429ab6cea6697b55c9d027117c8741e6449cc57942f0d580e33ad944ee` |
| 版本 | `OpenWrt native-wired-3.3` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 交付（P5 wlanmgr 骨架深化，doc 38 §3）

- **per-band BSS 状态**（`zxna_bss[2]`）：band/ssid(33B)/channel/
  protect（band 块 +0xB85 语义，doc 53 §1）/beacon 计数；
- **`bss-show`**：两 band 的 BSS 状态打印；
- **`bss-set <band> <ch> <ssid>`**：BSS 配置暂存（dry run 语义——
  beacon/CSA 编程在 P5、commit 后）；信道 1..196 校验。

## 3. 边界

- bss-set 只暂存镜像，不编程 beacon（P5，需固件起来 + 完整 AP 状态）；
- 未刷机。
