# 36 — MT7916 native 驱动真机验收清单（2026-10-30）

固件：`native-wired-1.5`（sha256 `2935b27a…`），未刷机。
刷入非活动槽后按下面顺序执行；每一步记录 `/proc/zx-csp-wlan/status`。

## 0. 前置

```sh
insmod zxnpu-wlan enable=1 commit=1      # 或内核内建时用 cmdline
cat /sys/module/zxnpu_wlan/parameters/enable
cat /sys/module/zxnpu_wlan/parameters/commit
```

预期：probe 打印 vendor=14c3 device=790a、三个 BAR、FW 审计（四份
`zte/mt7916/*` 文件存在且 CRC 可算）。

## 1. 只读面

```sh
echo probe   > /proc/zx-csp-wlan/ctl
echo rings   > /proc/zx-csp-wlan/ctl
echo mcuwin  > /proc/zx-csp-wlan/ctl
echo fw-state > /proc/zx-csp-wlan/ctl     # 需要 commit=1
```

预期：

| 命令 | 预期 |
|---|---|
| rings | TX qid16..20、RX qid0..5 的 base/cnt/cidx/didx 可读，不崩 |
| mcuwin | 门铃窗口可读 |
| fw-state | `MT_TOP_MISC state=1 (firmware download)` |

若 fw-state 读不到（全 0/全 f），先查 L1 remap（0xFE260 / 0xE0000）。

## 2. 下载链

```sh
echo "mcu-dl zte/mt7916/mt7916_patch_e1_hdr.bin" > /proc/zx-csp-wlan/ctl
echo fw-state > /proc/zx-csp-wlan/ctl
echo "fw-dl wm" > /proc/zx-csp-wlan/ctl
echo "fw-dl wa" > /proc/zx-csp-wlan/ctl
echo fw-state > /proc/zx-csp-wlan/ctl
```

预期：

1. patch：SemGet → AddressLen(0x04000000, 8642) → 17 个 scatter
   → Finish；每条日志 `didx advanced`；
2. WM：9 段，`FwStart option=00000001 override=0220a800`；
3. WA：3 段，`FwStart option=00000004 override=00000000`；
4. 最终 `state=7 (WM+WA ready)`。

## 3. 失败分诊

| 现象 | 先查 |
|---|---|
| `didx still` | 环 base/cnt 写法、GLO_CFG 使能位、是否用错环 |
| fw-state 停在 1 | patch 未被接受：检查 SemGet 响应与 Finish 负载 |
| FwStart 后仍 1 | WM 段地址/长度错，或 scatter 走了错误的环 |
| state=3 而非 7 | WA 未加载/未启动 |
| probe 无输出 | PCIe 未枚举或 enable=0 |

## 4. 尚未覆盖

- MCU 事件环格式与 ack 匹配（当前只打印 RX 环形内容）；
- EEPROM/l1profile 初始化；
- WFDMA 数据面（TX/RX 队列、NAPI、AP 模式）；
- 有线 P2 的 PPE offload 尚未接回。

以上都必须在第 2 步通过后再做；未通过前不要动 `zxnative_eth` 的
offload 路径。
