# 61 — native-wired-3.6：bss-up 闭环（conf → bss-set → driver）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.6/e2631-openwrt-native-wired-3.6.bin` |
| sha256 | `b8f78aa5d3248c209a13db831e2a4bbcbf132a65a36af5b0fa6ad14a92b8661a` |
| 版本 | `OpenWrt native-wired-3.6` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 交付（P5 wlanmgr BSS 配置完整化，doc 38 R6）

**`wlanmgr bss-up <band>`**：从配置文件读出 SSID/Channel，自动执行
data-init（使能数据 RX DMA）+ bss-set（暂存 BSS）——**conf → bss-set →
driver 的三步闭环**。band 编号自动转换（b0→0）。需 commit=1。

真机用法：`wlanmgr conf-write b0 SSID mynet` → `conf-write b0 Channel 6`
→ `wlanmgr bss-up b0` → `wlanmgr sta-add <mac>` → `wlanmgr sta-list`。

## 3. P5 wlanmgr 子命令全景（20 个）

status/expect/l1p/rings/plane/recv/drecv/fwstate/probe/step1/step2/step3/
data-init/data-recv/data-tx/dataplane/mcu-send/mcu-cmd/mcu-dl/... +
conf-read/conf-write/bss-up/bss-set/bss-show/profile/bssplan/commit/
sta-add/sta-del/sta-list/key-set。
