# native-0.3：有线基线回归 + npu 族共存（2026-10-30）

> 0.1/0.2 的镜像没有任何网口，不能算“可用”。0.3 把 021 的
> ZX279128S 交换机/GMAC 硬件层恢复为基线数据面（它是本项目从原厂
> 固件逆向出的硬件层，不是上游开源驱动），并把 npu 驱动族改成
> **042 追加接线**，与 021 共存。无线仍未恢复。

## 1. 接线方式

- 021 只创建 `drivers/net/ethernet/zte/{Kconfig,Makefile}` 并接入
  `drivers/net/ethernet/{Kconfig,Makefile}`；
- 037-041 原本是“从零创建同名文件”的补丁，与 021 冲突（本次构建
  先复现了冲突：`zte/Kconfig already exists`、4 个 hunk FAILED）；
- 042 改为**只在 021 的文件末尾追加**：
  - Kconfig：在 `endif # NET_VENDOR_ZTE` 前插入 ZX_NPU / AUDIT /
    IDM / PPE / CSP_WLAN 五个 config；
  - Makefile：追加五行 `obj-$(CONFIG_...)`；
- apply.sh 在 prepare 前删掉 037-041，并确保 021 存在（被旧版
  native apply 删过就从 git 恢复）。

## 2. 见证

prepare 后直接读内核源码树：

```
config ZX279128S_ETH + config ZX_NPU + AUDIT + IDM + PPE + CSP_WLAN
obj-$(CONFIG_ZX279128S_ETH) += zx279128s-eth.o
obj-$(CONFIG_ZX_NPU) += zx279128s-npu.o
obj-$(CONFIG_ZX_CSP_WLAN) += zxnpu-wlan.o
```

目标配置里同时钉死 `CONFIG_ZX279128S_ETH=y` 与五个 NPU 符号。

## 3. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-0.3/e2631-openwrt-native-0.3.bin` |
| 大小 | 20,972,052 B |
| sha256 | `04461f092c0b04313bcb5fba0537fa67234365b0cc0f9688b151402e17f837c0` |
| 版本 | `OpenWrt native-0.3` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

## 3.1 符号见证

System.map 同时含 `zx_eth_probe`、`zxnpu_idm_probe`、`zxnpu_ppe_probe`、`zxnpu_wlan_probe`（各 1 处），说明两套驱动都在内核里。

## 4. 边界

- 有线可用性依赖 021 硬件层在 6.18 上的既有验证（v1.8.0 真机）；
  native 侧只是恢复接线，未改它的寄存器序列。
- 无线仍无（mt76/mac80211 仍被拒绝）；MT7916 的 native 路径仍在
  G3.3（门铃 0x54000108 已定位，cmd buffer 未定）。
- 未刷机。
