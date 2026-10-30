# native-1.0：固件头/尾解析与一键补丁下载（2026-10-30）

> `mcu-dl` 现在可以只给文件名：如果文件头 `+0x20` 是 `0x11223344`，
> 自动取 `+0x24` 为加载地址、`+0x40` 为负载起点；新增 `fw-info`
> 打印补丁头或 MTK 固件 trailer（chip_id/n_region/format/date/crc）。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-1.0/e2631-openwrt-native-1.0.bin` |
| 大小 | 20,972,052 B |
| sha256 | `3d05497d7a29caf726b3df3e81de47198381432c2487ddf693b4443bedca7d39` |
| 版本 | `OpenWrt native-1.0` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 自动参数

`mt7916_patch_e1_hdr.bin`：`+0x20=0x11223344`、`+0x24=0x04000000`、
`+0x28=0`，负载从 `+0x40` 起。因此：

```
echo "mcu-dl zte/mt7916/mt7916_patch_e1_hdr.bin" > /proc/zx-csp-wlan/ctl
```

即可按 SemGet → AddressLen(0x04000000, 8632, 0x80000000) → 17×512B
FwScatter → Finish 执行（仍需 enable=1 + commit=1）。

## 3. fw-info

```
echo "fw-info" > /proc/zx-csp-wlan/ctl
```

对三个文件逐个打印：补丁头（日期/平台/加载地址）或 trailer
（chip=0x13 n_region=9 fmt=2 date=20250103112717 crc=f9b7d222）。

## 4. 边界

- RAM code 仍不能自动分段：format-2 region 表布局未解（doc 14 §6.14）；
- 未刷机。
