# native-1.3：多段下载（manifest 驱动）（2026-10-30）

> `mcu-dl-regions <file> <region,...>` 接受手工/探针得到的段表：
> 每段 `addr len offset [flags]`（十六进制、逗号分隔），逐段发送
> AddressLen(addr,len,0x80000000|flags) + 512B FwScatter。这样在
> format-2 段表解出之前，也能把 RAM code 按段喂给 MCU；FwStart 由
> 独立的 `fw-start` 发出。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-1.3/e2631-openwrt-native-1.3.bin` |
| 大小 | 20,972,052 B |
| sha256 | `0c017378fb921196120261b99c312106d189c7ea8379cc7e0b1fcfcd1bf92f11` |
| 版本 | `OpenWrt native-1.3` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021）+ npu；无线仍无 |

未刷机。

## 2. 用法

```
insmod zxnpu-wlan enable=1 commit=1
echo "mcu-dl-regions zte/mt7916/WIFI_RAM_CODE_MT7916.bin \
      00100000 3C0700 0 20, 00000000 3800 3C0700" > /proc/zx-csp-wlan/ctl
echo "fw-start 1 0" > /proc/zx-csp-wlan/ctl
```

段表来源：`parse_fw_v2` 的 36B 项规则（+0 addr、+5 flags、+32 len、
src 按 len+4 递增）；一旦 format-2 元数据解出，可由驱动自动生成。

## 3. 边界

- 段表目前要人工给；没有自动解析；
- 仍未在硬件上验证；未刷机。
