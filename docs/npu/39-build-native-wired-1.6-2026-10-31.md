# 39 — native-wired-1.6 构建记录（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.6/e2631-openwrt-native-wired-1.6.bin` |
| 大小 | 20,972,052 B |
| sha256 | `2ebaa009563ba59b564f3fb30e9af9f0ad54f22d7fd1e7a4d933828342994eb1` |
| 版本 | `OpenWrt native-wired-1.6` |
| 构建目录 | `out/build-20261031-030343` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机（禁刷约束不变）。

## 2. 相对 1.5（doc 37 §2.1/§2.2 解码落地）

- 新增 `ack-expect` 只读命令：按 doc 37 §2.2 的事件头解码
  （+2=eid、+3=seq/flag、+4=status），打印 FWDL 全程期望表——
  SemGet/AddressLen/Finish 的响应判据、WM 9 段+FwStart
  option=00000001 override=0220a800、WA 3 段 option=00000004、
  fw-state 1→7 迁移——真机 `mcu-recv` 拿到帧后逐字段对照；
- 纯文档式 proc 输出，无等待无行为变更（清单 §1 只读纪律）。

## 3. 见证

- 构建日志 witness：`ack expectations (doc 37` 在驱动源码 = 1；
- vmlinux 字符串 `ack expectations (doc 37` = 1；
- 版本 `OpenWrt native-wired-1.6`。

## 4. 边界

- 事件头字段（+2/+3/+4）语义来自反汇编解码，最终以真机 `mcu-recv`
  首帧对照为准（清单 36 §2 通过后）；
- 未刷机。
