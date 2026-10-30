# native-wired-1.0：P2 切换构建（021 移除，zxnative-eth 接管）（2026-10-30）

> 这是第一条**没有 021**的有线镜像：补丁 044 从零创建
> zte/Kconfig 与 Makefile，只编 ZX_NPU* / ZX_CSP_WLAN / ZXNATIVE_ETH；
> 021、042、043 全部移除，MDIO（018）与 Sanechips PHY（019）保留。
> 内核命令行加 `zxnative_eth.enable=1`，由 P2 驱动绑定
> `zte,zx279128s-gmac`。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.0/e2631-openwrt-native-wired-1.0.bin` |
| 大小 | 20,972,052 B |
| sha256 | `bb54a902fe44d2a712580532544adc6e01fb41908e8e9e34199e64a4a4749c3b` |
| 版本 | `OpenWrt native-wired-1.0` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 构建方式

```
bash scripts/build-native-wired.sh
```

脚本做四件事（全部有日志见证）：

1. 正常 apply（overlay → 树）；
2. 删除 021/042/043，放入 044；把 rootfs 版本改成 native-wired-1.0；
3. target config：去掉 `CONFIG_ZX279128S_ETH`，钉 `CONFIG_ZXNATIVE_ETH=y`，
   `CONFIG_CMDLINE` 追加 `zxnative_eth.enable=1`；
4. clean/prepare/build。

## 3. 见证

prepare 后内核源码树：

```
drivers/net/ethernet/zte/Kconfig:64:config ZXNATIVE_ETH
drivers/net/ethernet/zte/Makefile:7:obj-$(CONFIG_ZXNATIVE_ETH) += zxnative-eth.o
（无 ZX279128S_ETH）
```

System.map：

```
zxna_probe            1   ← P2 驱动
zx_eth_probe          0   ← 021 不存在
zx279128s_mdio_probe  1   ← 018 MDIO
sanechips PHY         2   ← 019
```

编译：`CC drivers/net/mdio/mdio-zx279128s.o`、`CC zxnative-eth.o`。

## 4. 与基线镜像的差别（如实声明）

- 没有 PPE/tc 硬件 offload、没有 switchdev 硬件桥：转发走 Linux 桥；
- 统计/ethtool/MIB、trap-DMAC 路由表尚未移植；
- **完全未在真机验证**：这是 P2 的线路切换件，不是“已验证可用”的结论；
- 回退：仍可刷回 native-1.5（021 基线）或原厂槽。
