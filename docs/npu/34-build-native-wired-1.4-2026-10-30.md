# 34 — native-wired-1.4 构建记录（2026-10-30）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.4/e2631-openwrt-native-wired-1.4.bin` |
| 大小 | 20,972,052 B |
| sha256 | `8b0e11efdf4ffa42870f160ad1dd6047cc3e1fba7882a85afddadf3d90c9f5d2` |
| 版本 | `OpenWrt native-wired-1.4` |
| 构建目录 | `out/build-20261030-223740` |
| verify_pkg | PASS（含 no-open-source-wireless 检查） |
| verify_img | BOOT-READY：no static blocker found |
| 基线 | 与 native-wired-1.3 相同（021 移除，zxnative-eth 接管） |

未刷机。

## 2. 内容

- `zxnpu-wlan.c`：format-2 region 表解析（`parse_fw_v3` 语义）、
  `fw-plan wm|wa`、`fw-dl wm|wa`、patch 30 字节头、
  `fw-state`（L1 窗口读 `MT_TOP_MISC`）；
- 修掉 `fw` 前缀吞掉 `fw-info`/`fw-start` 的分发顺序问题；
- `e2631-wlan-probe --commit`：patch → WM → WA，各步之间 drain 一次
  事件环。

## 3. 边界

- 本次内核在 prepare 时取的树还没有 FWDL 环拆分（见 35 篇，1.5）；
- 未真机验证；MCU sem 响应、命令 ack、事件环格式仍未确认。
