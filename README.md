# E2631 native · 原厂行为对齐线（native-0.3）

独立本地仓库。使命：**彻底放弃开源有线/无线数据面驱动，按原厂 4.1.25 二进制
为唯一行为规范，在 6.18 上重写 ZX279128S 有线栈 + MT7916 私有栈（A+W）**。
上游研究谱系与逆向证据在 `Zlion-Y/zte-e2631-openwrt` 的 `npu` 分支
（docs/npu 00-17）；本仓库只走 native 线。

## 本线与 npu 分支的差别

| | npu 分支 | 本仓库（native） |
|---|---|---|
| 开源数据面 | 保留为默认启动路径，native 模块 enable 门禁共存 | 无线开源栈移除；有线保留 vendor-RE 硬件层作基线 |
| 启动路径 | v1.8.0 已验证栈 | 0.2 起 SoC 基础设施 + WFDMA 只读探针；0.3 起有线基线回归 |
| 用途 | 行为对照与共存迭代 | 全原厂重写的主线 |

## 构建期强制项（0.3）

- **021**（ZX279128S Ethernet switch/gmac 硬件层）—— 0.3 起**保留**：
  没有它镜像没有任何网口，不能算“可用”。它是本项目从原厂固件逆向出的
  硬件层，P2 的原生重写完成后才会替换；
- **037-041** —— 由 **042-zte-add-the-ZX279128S-NPU-wiring.patch** 取代：
  042 直接追加到 021 创建的 zte/Kconfig 与 zte/Makefile，两套驱动共存；
- **034**（FFE 控制面接线）—— overlay 补丁删除，P3 按原厂语义重建；
- **kmod-mt7915e / kmod-mt7916-firmware / kmod-mac80211 / kmod-cfg80211 /
  wpad* / wireless-regdb / kmod-nft-offload** —— seed 与 DEVICE_PACKAGES
  双层拒绝（defconfig DEFAULT_ 机制防复活）；
- 保留：SPI/NAND/PCIe/slotpart/DTS/USB(关) 等原厂 SoC 基础设施补丁、
  zte-mesh（原厂二进制用户态）、e2631 板级工具、zxnpu 驱动族（042）。

## 版本进展与已知代价（如实声明）

| 版本 | 数据面 | 说明 |
|---|---|---|
| native-0.1 | 无 | 开源有线/无线数据面全部摘除 |
| native-0.2 | 无 | 增加 WFDMA 环只读探针（ctl 写 rings），sha256 5b0a508e… |
| native-0.3 | 有线（021 硬件层） | 恢复网口可用；无线仍无；npu 族由 042 接线 |
| native-0.4 | 有线 + npu 探针 | 增加 mcuwin（BAR +0/+8/0x108 只读），sha256 a100bde9… |
| native-0.5 | 有线 + MCU 发送实验 | 64×36B 描述符环 + PatchSemGet 编码，双门禁，sha256 a9fd1e02… |
| native-0.6 | 有线 + MCU 收发实验 | RX/事件 ring0（32×1700B、16B RXD）+ mcu-recv，sha256 d7a5d384… |
| native-0.7 | 有线 + MCU 提交器 | rolling cidx 提交器 + mcu-cmd 原始命令控制台，sha256 6f02cb68… |
| native-0.8 | 有线 + 补丁下载 | mcu-dl：SemGet→AddressLen→FwScatter→Finish 全序列，双门禁，sha256 dda87dc2… |
| native-0.9 | 有线 + 队列校正 | MCU 命令环改到 0x18024410（qid 17），固件 trailer 已解，sha256 db7d36fd… |
| native-1.0 | 有线 + 一键补丁下载 | mcu-dl 自动读 patch 头（dst/offset）+ fw-info，sha256 3d05497d… |
| native-1.1 | 有线 + 固件启动命令 | fw-start（type 0x02）；format-2 段表排查结论，sha256 27ead6d9… |

无线仍无：MT7916 host driver 的 native 重写尚未到位（P4）。有线加速仍由
021+PPE 提供；P2 的原生重写完成后替换 021。

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
