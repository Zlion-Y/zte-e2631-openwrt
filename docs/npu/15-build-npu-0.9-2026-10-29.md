# npu-0.9 构建记录：Andes MCU 命令编码器（2026-10-29）

> 本版把 `AndesInitCmdMsg` 的 24 字节命令头做成可执行的编码器：
> `/proc/zx-csp-wlan/ctl` 写 `mcu` 会按原厂顺序打印
> `PatchSemGet` / `AddressLenReq` / `PatchFinishReq` 三条命令的
> 完整字节。它不发送、不写硬件，只把“要发什么”固化下来，
> 供 G3.3 接 WFDMA 队列时逐字节比对。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/npu-0.9/e2631-openwrt-npu-0.9.bin` |
| 大小 | 20,972,052 B |
| sha256 | `4652951554fdbe13f9bbf7f2cc159c055628316fdaeefb8de26c1cd7109a9786` |
| 版本 | `OpenWrt npu-0.9` |
| verify_pkg / verify_img | ALL CHECKS PASSED（1 条已知良性 warning：uImage ih_hcrc 与标准算法不同，vendor 固件同样如此）/ BOOT-READY（`release.mjs` 内置跑） |

打包命令（构建树 `out/build-20261029-222651`，BUILD_RC=0）：

```sh
node host/release.mjs --dir D:/AI/Qoder/E2631-OpenWRT/out/build-20261029-222651 \
  --template "D:/AI/Qoder/E2631-OpenWRT/升级包/zxhne2631_hv100_fv1008b88000_firmware-20260916183918.bin" \
  --out D:/AI/Qoder/E2631-OpenWRT/out/npu-0.9/e2631-openwrt-npu-0.9.bin
node host/verify_pkg.mjs D:/AI/Qoder/E2631-OpenWRT/out/npu-0.9/e2631-openwrt-npu-0.9.bin \
  --dts overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts
```

## 2. 编码器

来源：`AndesInitCmdMsg`（0x1B8E5C）与三个 `MtCmd*` 调用点。

```
offset  field
0..1    len（payload 字节数）
2       type   0x10 sem / 0x01 或 0x05 addr / 0x07 finish
3       seq（本实现写 0）
4       cmd    0x17
6..7    参数高半字（本实现 0）
8..9    参数
12..15  参数
16..19  参数
20..21  GetRealPortQueueID(type)（本实现不建模，保持 0）
23      flags（bit2 同步等待）
24..    payload
```

`mcu` 输出示例（值来自本地固件头）：

```
mcu type=10 cmd=17 len=4: 04 00 10 00 17 00 ... 00 00 00 00
mcu type=01 cmd=17 len=12: 0c 00 01 00 17 ... 00 00 00 04 00 00 ... 
mcu type=07 cmd=17 len=0: 00 00 07 00 17 ...
```

`AddressLen` 的地址固定 `0x04000000`（MCU SRAM 基址），长度取
patch 文件大小；**分段方式仍未解**，输出里明确标注。

## 3. 见证

- `apply.sh` 断言：源码里同时存在 `type=%02x cmd=%02x` 与
  `SemGet(type 0x10)`，防止编码器被后续改动吞掉。
- 构建后 `zxnpu-wlan.o` 应含 `mcu: patch download sequence` 字符串。

## 4. 边界

- 没有 WFDMA MCU 环，所以没有 `chip_cmd_tx`；`mcu` 只编码。
- 队列号 `GetRealPortQueueID(type)` 尚未展开，先置 0。
- 未刷机。
