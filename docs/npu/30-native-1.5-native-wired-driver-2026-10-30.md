# native-1.5：P2 原生有线驱动（内建）（2026-10-30）

> 新增独立驱动 `zxnative-eth.c`（约 970 行）：按原厂寄存器语义重写
> 交换机/GMAC/DMA 数据面，不依赖 021 的代码。编成模块
> `zxnative-eth.ko`，绑定同一 DT 节点但 `zxnative_eth.enable=0` 默认
> 拒绝 probe；与内建的 021 基线共存（基线先绑定，模块不会抢设备）。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-1.5/e2631-openwrt-native-1.5.bin` |
| 大小 | 20,972,052 B |
| sha256 | `acb36ac3f093f93e473fd800fd3847f20ef79782542d91c3f12eb28fa8b919ef` |
| 版本 | `OpenWrt native-1.5` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 数据面 | 有线（021 基线）+ npu；zxnative-eth 内建但 enable=0 |

未刷机。

## 2. 驱动内容

- 复位/时钟：GLB reset、门控、5 个 MAC 端口时钟；
- SBRG/PP：端口过滤、转发、trap 以太网类型、PP 复位；
- TM：16 个 CPU 队列（读 table1 残值防 TFTP 残留）、384 RED/调度表；
- BMU：分配/释放、池阈值（4096×2048B）；
- DMA：16 MiB arena（tab0/tab1/RX/TX/area50 布局同原厂）、
  1024×16B TX desc、8×1024 RX desc、TX done 计数；
- MAC：5 端口初始化、phylink（MAC_10/100/1000FD、internal/GMII）；
- 数据面：TX 直接入 TM 端口队列（`40+port`），RX 逐队列 copy-out、
  按描述符端口号分发到 netdev、own-address/空槽/重同步处理；
- 统计/PPE/switchdev/ethtool 暂未移植（后续 P2 步骤）。

kernel 见证：`CC drivers/net/ethernet/zte/zxnative-eth.o`（内建），System.map 含 `zxna_probe`。

## 3. 接线

- 补丁 043 向 021 创建的 zte/Kconfig/Makefile 追加 `ZXNATIVE_ETH`
  （tristate）与 `obj-` 行；
- apply.sh 钉死 `CONFIG_ZXNATIVE_ETH=m`；
- 043 的符号是 bool；apply.sh 钉死 `CONFIG_ZXNATIVE_ETH=y`（旧的 =m 残留会被 awk 清掉）。

## 4. 边界

- 未在硬件上启用/验证；enable=0 时 probe 立即 -ENODEV，不碰寄存器；
- 仍是“第二驱动”，P2 完成（校验数据面后）才会替换 021；
- 未刷机。
