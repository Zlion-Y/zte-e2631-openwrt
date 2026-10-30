# native-0.8：补丁下载状态机（SemGet → AddressLen → Scatter → Finish）（2026-10-30）

> 依据 doc 14 §6.11 闭合的下载协议，`mcu-dl <file> <dst-hex> [offset]`
> 按原厂顺序发四条命令：PatchSemGet、AddressLen、逐块 FwScatter、
> PatchFinish。命令缓冲扩到 4 KiB，块大小 512 B（栈帧限制）。默认 dry-run，
> enable+commit 双门禁。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.8/e2631-openwrt-native-0.8.bin` |
| 大小 | 20,972,052 B |
| sha256 | `dda87dc28cc640356dee1ef7bee298ed2fa5f0cd2ff7acda40d6d9f023894fe8` |
| 版本 | `OpenWrt native-0.8` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 命令序列

```
SemGet     type 0x10 cmd 0x17 {sem=1}
AddressLen type 0x05 (dst==0x04000000) / 0x01, cmd 0x17 {dst, total, 0x80000000}
FwScatter  type 0xEE cmd 0x10 {512 B 文件块}  x ceil(total/512)
Finish     type 0x07 cmd 0x17 {}
```

`AddressLen` 的 type 选择来自 `MtCmdAddressLenReq`：地址等于
`chip_cap+0x12C`（MCU SRAM 基址 0x04000000）时用 0x05，否则 0x01。

## 3. 用法（硬件实验）

```
modprobe zxnpu-wlan enable=1 commit=1
echo "mcu-dl zte/mt7916/mt7916_patch_e1_hdr.bin 04000000 64" \
    > /proc/zx-csp-wlan/ctl
cat /proc/zx-csp-wlan/status
```

offset 参数留给“文件头 vs 负载”尚未钉死的场景：原厂
`parse_patch_v1` 从补丁文件里构造 `{src,dst,len}` 段表，段表布局还没
完全解出，所以先让调用者显式给 dst/offset，避免猜错写坏 MCU SRAM。

## 3.1 校正

0.9 起命令环改为 （qid 17 MCU_WM）；0.8 及之前的  发到了 （MCU_WA），帧格式对但队列不对，属实验记录。

## 3.1 校正

0.9 起命令环改为 0x18024410（qid 17 MCU_WM）；0.8 及之前的 mcu-dl 发到了 0x24440（MCU_WA），帧格式对但队列不对，属实验记录。

## 4. 边界

- 没有等 MCU 响应（mcu-recv 只能看原始字节），成功与否需人工判读；
- `parse_patch_v1` 的段表还没解，默认不自动推导 dst/offset；
- 未刷机。
