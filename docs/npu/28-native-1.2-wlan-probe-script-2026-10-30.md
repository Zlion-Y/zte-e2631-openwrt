# native-1.2：W 侧上机探针脚本（2026-10-30）

> 把这一轮做的 MCU/环/固件工具串成设备上的一键探针：
> `/usr/sbin/e2631-wlan-probe [--commit] [patch-file]`。
> 默认只读；`--commit` 才走 PatchSemGet → rx-init → mcu-recv →
> （可选）mcu-dl → fw-start。驱动自身的 enable/commit 双门禁仍然生效。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-1.2/e2631-openwrt-native-1.2.bin` |
| 大小 | 20,972,052 B |
| sha256 | `8ac85ff0689763d7ecf1c77018904f0ddbc0031fc620bd482d5009cecbf60e0b` |
| 版本 | `OpenWrt native-1.2` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 用法（上机时）

```sh
insmod zxnpu-wlan enable=1          # 只读探针
e2631-wlan-probe

insmod ... enable=1 commit=1        # 允许写 MCU
e2631-wlan-probe --commit zte/mt7916/mt7916_patch_e1_hdr.bin
```

输出包含：模块参数、CR/BAR 探针、WFDMA 11 个环块、固件头/尾、
MCU 窗口，以及 commit 模式下的提交/收包/下载/启动日志。

rootfs 见证： 为 ，1545 B。

## 3. 边界

- 没有段表，RAM code 仍只支持手工单段 mcu-dl；
- 未刷机。
