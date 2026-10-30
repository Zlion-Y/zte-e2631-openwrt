# 35 — native-wired-1.5 构建记录（2026-10-30）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.5/e2631-openwrt-native-wired-1.5.bin` |
| 大小 | 20,972,052 B |
| sha256 | `2935b27a254ca58c645aab2a6932fd4eea8626b772ab0972b858c8b08d1a22fd` |
| 版本 | `OpenWrt native-wired-1.5` |
| 构建目录 | `out/build-20261030-224217` |
| verify_pkg / verify_img | PASS（含 no-open-source-wireless）/ BOOT-READY |
| 基线 | 同 native-wired-1.3/1.4（021 移除，zxnative-eth 接管） |

未刷机。

## 2. 相对 1.4

- 双环：`zxnpu_mcu_submit_ring()`，scatter 走 FWDL qid16
  （0x18024400），AddressLen/FwStart/Sem/Finish 走 MCU_WM qid17
  （0x18024410）。依据 `GetRealPortQueueID`（0x1C112C）：type 0xEE →
  `chip_cap[0x162]`，其余 → 0x8000 控制队列；
- 新增第二套 DMA 环/缓冲（probe 分配），`fwdl_cidx` 独立计数；
- 提交日志打印 `qid16/qid17`，便于真机对照。

## 3. 见证（vmlinux，22:44）

```
grep -c "usage: fw-dl wm|wa"     -> 1
grep -c "remap %08x <- %04x"     -> 1
grep -c "qid%d slot"             -> 1
grep -c "MT_TOP_MISC state"      -> 1
```

## 4. 边界

- 未真机验证：PatchSemGet 的返回状态、命令 ack 匹配、事件环格式都还
  没有实测；两条环的 base/cnt 写法沿用 1.0 的推断；
- 下一步验机顺序：`fw-state`（应 1）→ `mcu-dl patch` → `fw-state`
  → `fw-dl wm` → `fw-dl wa` → `fw-state`（应 7）。
