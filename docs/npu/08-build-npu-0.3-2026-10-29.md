# npu-0.3 构建记录：vendor-native IDM 环形引擎（2026-10-29）

> 这是 npu 分支第一个**数据面代码**检查点：`zxnpu0` netdev + 自研 IDM
> RX/TX 环。它仍然不是完整 A+W 数据面：有线仍走 v1.8.0 PPE，无线仍走
> mt76/mac80211；IDM 默认不碰硬件，必须显式 enable + ifup。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.3.bin` |
| 大小 | 20,972,052 B |
| sha256 | `7f5507139bbe7c3ceb307ff5e07f2c0985b9ee2b041f6b3dbe54b9ce9be37ed8` |
| 版本 | `OpenWrt npu-0.3` |
| verify_pkg | ALL CHECKS PASSED（1 个既有 uImage `ih_hcrc` warning） |
| verify_img | BOOT-READY: no static blocker found |

未刷机。

## 2. 新增数据面组件

`zx279128s-npu-idm.c`（`CONFIG_ZX_NPU_IDM=y`）：

- 平台驱动 `zte,zx279128s-npu-idm`，注册 netdev **`zxnpu0`**。
- 固定映射 NPP `0x921C0000`（0x40000）与 IDM `0x921C8000`（0x4000）。
- 一条 RX 环 2048×8B（`+0x08` 基址）和一条 TX 环 1024×8B（`+0x04` 基址，
  放在 ring+0x4000）。
- 门铃语义按 P1 反汇编：TX kick/ack `+0x40`、TX done `+0x44`、
  RX submit `+0x48`（写 `0x10000`）、RX done `+0x4c`。
- NPP 侧写 masks、GREG authen/closed、wifi_queue1=0、SIPC rx_en。
- NAPI RX 把帧交给 `netif_receive_skb(zxnpu0)`；TX 从 `zxnpu0` 注入 fabric。
- 默认 `zxnpu_idm.enable=0`，`zxnpu0` 打开时直接 `-EOPNOTSUPP`；只有
  `enable=1` + `ip link set zxnpu0 up` 才执行第一笔寄存器写。

## 3. 构建见证

```
CONFIG_ZX_NPU=y
CONFIG_ZX_NPU_AUDIT=y
CONFIG_ZX_NPU_IDM=y
# CONFIG_ZX_FFE is not set
```

System.map：

```
c060675c t zxnpu_idm_poll
c06069c4 t zxnpu_idm_probe
c0606ba0 t zxnpu_idm_xmit
c0606e1c t zxnpu_idm_remove
```

DTB：`npu-scaffold`、`npu-audit`、`npu-idm` 三个节点都在；无
`zx_idm_*`/`zx_wifi_flow_offload`（ffe-full 残留仍为 0）。

## 4. 上机实验（尚未执行，需要显式放行）

```sh
echo 1 > /sys/module/zx279128s_npu_idm/parameters/enable
ip link set zxnpu0 up
cat /proc/zx-npu-idm/status
cat /proc/zx-npu-audit/audit   # 先跑只读审计对照
```

风险：这是第一次让自研代码写 IDM 寄存器；失败可能静默挂死，只能断电。
必须保留原厂槽/failsafe，且一次开机只动这一个变量。

## 5. 边界

- 没有 TM/BMU 的 vendor-native 初始化；当前依赖主驱动的 TM/BMU 已经起来。
- 没有 PPE/CLA 流表，也没有 MT7916 无线递送；`zxnpu0` 是 fabric 测试口。
- 没有真机验证；`verify_pkg`/`verify_img` 只证明可刷和结构正确。

## 6. 下一步

- P1 剩余：CLA/SPA/PM table 字段语义、WANID/SE 槽位、PPU mcode。
- 数据面：PPE/FFE vendor-native 控制面，或先做 MT7916 G3 的 PCIe/固件下载
  骨架。两条线最终要在 `zxnpu0`/IDM 上合流。
