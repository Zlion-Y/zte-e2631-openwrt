# 49 — R4e 收尾 + wlanmgr CLI：native-wired-2.6（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.6/e2631-openwrt-native-wired-2.6.bin` |
| sha256 | `c3329aac100143d5c7a151c291857496d828f957fb61fbe042e388a0eb80a746` |
| 版本 | `OpenWrt native-wired-2.6` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| rootfs 见证 | `/sbin/wlanmgr` 0755 在位、版本 `native-wired-2.6` |

未刷机。

## 2. 交付

1. **TXD 全字存储图**（doc 45 §7）：7 个字（+0/+4/+8/+12/+20/+24/+28）
   全部有字段来源——固定率主路径闭环；
2. **wlanmgr CLI**（P5 骨架）：`/usr/sbin/wlanmgr` 薄封装
   /proc/zx-csp-wlan/ctl——只读子命令（status/expect/l1p/rings/plane/
   recv/drecv/fwstate）+ 显式 `wlanmgr commit <cmd>` 转发写命令；
   apply.sh chmod 列表接线；
3. 版本 2.6（首版 2.6 构建因 shell 链 grep 退出码断链用了 2.5 版本号，
   已修正重构建）。

## 3. 状态

TMAC→TXD 离线解码全部闭环（doc 44/45）。真机验收（清单 36，L2-L6）
等用户解除禁刷。
