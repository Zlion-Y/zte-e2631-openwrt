# E2631 native · 原厂行为对齐线（native-0.1）

独立本地仓库。使命：**彻底放弃开源有线/无线数据面驱动，按原厂 4.1.25 二进制
为唯一行为规范，在 6.18 上重写 ZX279128S 有线栈 + MT7916 私有栈（A+W）**。
上游研究谱系与逆向证据在 `Zlion-Y/zte-e2631-openwrt` 的 `npu` 分支
（docs/npu 00-17）；本仓库只走 native 线。

## 本线与 npu 分支的差别

| | npu 分支 | 本仓库（native） |
|---|---|---|
| 开源数据面 | 保留为默认启动路径，native 模块 enable 门禁共存 | **移除**（构建期摘除） |
| 启动路径 | v1.8.0 已验证栈 | OpenWrt 基座 + SoC 基础设施，无数据面驱动 |
| 用途 | 行为对照与共存迭代 | 全原厂重写的主线 |

## native-0.1 移除清单（构建期强制）

- **021**（ghahramani 系 ZX279128S Ethernet switch/gmac 驱动，eth0..3）
  —— apply.sh 从基础树摘除补丁，镜像内无此驱动；
- **034**（FFE 控制面接线）—— overlay 补丁删除，P3 按原厂语义重建；
- **kmod-mt7915e / kmod-mt7916-firmware / kmod-mac80211 / kmod-cfg80211 /
  wpad* / wireless-regdb / kmod-nft-offload** —— seed 与 DEVICE_PACKAGES
  双层拒绝（defconfig DEFAULT_ 机制防复活）；
- 保留：SPI/NAND/PCIe/slotpart/DTS/USB(关) 等原厂 SoC 基础设施补丁、
  zte-mesh（原厂二进制用户态）、e2631 板级工具、zxnpu 驱动族（037-041）。

## native-0.1 已知代价（如实声明）

镜像**没有有线网口也没有无线**：gmac/交换机数据面与 MT7916 host driver
的 native 重写尚未到位（P2/P4）。刷入后 = 可启动、UART/可管理、无网络数据面。
这是切换期的定义性行为，不是缺陷；网络能力随 P2（有线驱动族）与 P4（MT7916
固件启动门禁）逐模块回归。

rootfs 残留审计（0.1，逐文件核验）：**开源驱动包零残留**（manifest 与 apk
数据库双向核验，kmod-mt76*/mac80211/cfg80211/mt7915e/mt7915-firmware/
mt7916-firmware/wpad*/hostapd-common 全部不选中）。仍存在的 5 个 wifi 名义
文件均无可执行行为：4 个是 OpenWrt **base-files 自带**的 netifd 辅助脚本
（无驱动注册时 netifd 不会调用），1 个是 apply.sh 只增不删遗留的本仓热插拔
脚本 11-mt76-caldata（固件事件永不触发；已在 apply.sh 加树内删除，0.2 起
消失）。

## 构建

```sh
bash apply.sh /home/builder/openwrt     # WSL 内，/home/builder/openwrt 为钉死基线树
cd /home/builder/openwrt && make target/linux/clean && make target/linux/prepare V=sc
bash scripts/build.sh --no-pack
node host/release.mjs --dir <build 目录> --template <官方 8B8 包> \
  --out out/e2631-openwrt-native-0.1.bin
node host/verify_pkg.mjs out/e2631-openwrt-native-0.1.bin \
  --dts overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts
```

vendor 二进制物料（.ko/固件/内核 dump）不入库，仍在 `../build/board/`；
MT7916 固件构建时从 `$E2631_VENDOR_FW_DIR`（默认 `../out/stock-fw`）拷入。

## 稳定纪律

- 每个写寄存器必须指回原厂反汇编或活体证据；无证据不写。
- 新驱动默认 enable=0；写操作 enable+commit 双门。
- 每次构建过 verify_pkg + verify_img 双门禁；重启/变砖风险清单逐项过
  （ffe 六项根因模式）。
- 禁刷实机；刷写须用户另行放行。
