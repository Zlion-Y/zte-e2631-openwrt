# native-1.1：MtCmdFwStartReq + format-2 段表结论（2026-10-30）

> 补齐固件启动命令 `fw-start <option> [override]`（type 0x02、
> cmd 0x17、payload {option, override}），并把 format-2 region 表的
> 排查结论写死：它不在文件尾的明文区，继续在尾部猜布局是死路。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-1.1/e2631-openwrt-native-1.1.bin` |
| 大小 | 20,972,052 B |
| sha256 | `27ead6d973eca77c1a8dec1d6978b6592e3e866b2ae1d068169384b3041096f2` |
| 版本 | `OpenWrt native-1.1` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. fw-start

`load_fw_v1` 在最后调用 `MtCmdFwStartReq(priv, option, override)`：
option bit0 = 有 override 段（feature_set 0x20），bit2 = band1。
`fw-start 0 0` 等同默认启动；`fw-start 1 0x04000000` 用于覆盖地址。

## 3. format-2 段表排查

`parse_fw_v2` 逐指令确认：每个 band 的段表在 `base+size-36*count`
（count 由 band 决定：band0=2、band1=1），每项 36B，字段
`+0 BE u32 addr`、`+5 flags`、`+32 BE u32 len`，src 从文件头
按 `len+4` 递增。但实际文件该位置是 0/构建信息，说明 band 的
`[base,size]` 指向的不是整个文件，而是 driver 组装后的镜像区。
下一步应解 `load_fw_v2_compressimg` 的 `[fp+4]/[fp+8]` 来源
（即 priv+0xEFC8 附近 per-band fw_info 的填充者）。

## 4. 边界

- 没有段表就无法自动分段；`mcu-dl` 仍按单段手工/补丁头模式工作。
- 未刷机。
