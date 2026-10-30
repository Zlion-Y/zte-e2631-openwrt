# native-0.7：MCU 提交器与原始命令控制台（2026-10-30）

> 把 0.5 的一次性发送重构成可复用的提交器：rolling cidx、每个描述符
> 指向同一个命令缓冲、提交后轮询 DIDX；新增 `mcu-cmd <type> <cmd>
> <hex>`，可以手工发任意 MCU 命令做硬件诊断。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.7/e2631-openwrt-native-0.7.bin` |
| 大小 | 20,972,052 B |
| sha256 | `6f02cb687b868a71bcc4fc66afc4a342778b36f568e3a466999774236f08a04f` |
| 版本 | `OpenWrt native-0.7` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 提交器

`zxnpu_mcu_submit(msg, len, commit, verbose)`：

- slot = `mcu_cidx % 64`，描述符 next 指向下一槽；
- 命令缓冲 4 KiB，`memset` 后整段复制；
- 提交前打印 BASE/CNT/CIDX/DIDX，commit 时置 GLO_CFG bits0/2；
- 写 BASE/CNT，`mcu_cidx++` 后写 CIDX；
- `readl_poll_timeout` 等 DIDX 到同一槽位（50 ms）。

## 3. mcu-cmd

```
echo "mcu-cmd 16 23 00000000" > /proc/zx-csp-wlan/ctl
```

参数是十进制 type/cmd + 十六进制 payload。用于在没有状态机时手工
复现原厂命令（例如 PatchSemGet type=0x10 cmd=0x17）。

## 4. 边界

- ring 4 是 MCU 环仍是推断；DIDX 是否前进只能上机看。
- 未刷机。
