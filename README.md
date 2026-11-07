# ZTE ZXHN E2631 · OpenWrt

中兴巡天 AX3000（ZX279128S）的 OpenWrt 移植：overlay + 板级工具 + 打包器。
固件与各版本变更日志见 [Releases](https://github.com/Zlion-Y/zte-e2631-openwrt/releases)；
逐项实测与取舍记在 [docs/known-issues.md](docs/known-issues.md)。
本仓库不提供、也不链接原厂固件；随仓库分发的第三方物料见 [NOTICE](NOTICE)。

## 芯片与硬件状态

| 芯片 / 部件 | 规格 | 驱动 | 状态 |
|---|---|---|---|
| SoC | ZTE ZX279128S，双核 Cortex-A9 @1 GHz，256 MiB DDR2 | — | ✅ |
| 千兆交换 | 4×内部 GbE PHY（ZX5201，eth0-3 独立 netdev） | `zx279128s-eth`（内建） | ✅ |
| 硬件转发 | PPE 分类器/修改器（0x92000000）+ NPP/IDM/TM 引擎 | `zx279128s-ppe` + nft flowtable | ✅ IPv4 |
| 无线 | MT7916AN + MT7976DN（Filogic 630，DBDC 2×2） | mt7915e（mt76）+ 原厂 caldata | ✅ |
| Flash | SPI-NAND 128 MiB（HYF1GQ4UTACAE，2 KiB 页 / 128 KiB 块） | mtk-spi + spi-nand | ✅ |
| PCIe | Gen1 x1（挂 MT7916） | `zx279128s-pcie` | ✅ |
| UART | PL011 ×2 @115200n8 | pl011 | ✅ |
| LED | 三色：`red/green/blue:status`（active-low） | gpio-leds + `e2631-leds` | ✅ |
| USB | 未贴片 / 探测会挂死 | — | ❌ 保持禁用 |
| 原厂私有加速 | IDM / NPP / TM（cspkernel 4.1 私有栈） | 未移植 | ❌ |

## 适配状态

✅ = 已适配并在实机验证；⚠️ = 部分可用 / 带限制；❌ = 未适配

| 功能 | 状态 | 说明 |
|---|---|---|
| 原厂网页刷入（supgrade） | ✅ | 写入目标槽，serial 自动递增 |
| 系统内升级（sysupgrade / LuCI） | ✅ | 原地升级运行槽；写前校验另一槽原厂镜像完好 |
| 双槽 A/B + 切回原厂 | ✅ | `e2631-fwslot switch` / LuCI 板卡页；槽角色按内容判定，不写死槽号 |
| 千兆有线 4 口（1 WAN + 3 LAN） | ✅ | 1000M full；WAN/LAN 可手动互换（`e2631-wanport` / LuCI 页面） |
| 硬件流卸载（IPv4） | ✅ | PPE 直转，实测 ≥395 Mbit/s、CPU 7%；384 条流表，溢出自动回落 |
| 硬件流卸载（IPv6） | ❌ | 走 CPU 软件路径（PPE 激活时 IPv6 trap 恒开保证正确性） |
| Wi-Fi 双频 AP | ✅ | 2.4G = HE40（ch1）/ 5G = HE80（ch36）；RF 校准为原厂 caldata 全量透传 |
| Wi-Fi 硬件加速 | ❌ | 原厂 IDM/NPP 私有栈未移植（~200 Mbit/s CPU 路径）；FFE 实验分支 opt-in |
| 47 MiB userdata 扩展存储 | ✅ | 首启自动采纳（格式化+迁移+启用），重启后接管 ~35 MiB UBIFS；配置零丢失 |
| LuCI 中文后台（Argon） | ✅ | 板卡页：槽位表 / 在线升级 / WAN 口选择 / 备份 |
| WAN 侧调试访问 | ✅ | WAN 区放行私网段 22/80/443（**首启无密码，先设密码**） |
| 在线更新 | ✅ | GitHub Release 检查 + 下载，SHA256 强制校验，按数字比较版本 |
| LED 状态灯 | ✅ | 启动红慢闪 → 联网绿常亮 / 离线红常亮 |
| NTP / 时区 / 中文 | ✅ | Asia/Shanghai，国内 NTP 源 |
| 内置插件 | ✅ | SmartDNS / nlbwmon / UPnP / WOL / adblock-fast 随镜像内置（中文界面） |
| 代理栈 homeproxy | ⚠️ | sing-box 1.14.0 + LuCI，经 apk 装入 userdata 卷（Release 附件），订阅导入/中文 |
| 槽位布局 | ✅ | rootfs 窗口 17M（为内置插件扩容，上限按原厂头位置反推），升级包 21.2 MiB |
| USB | ❌ | 保持禁用（未贴片，探测挂死） |
| PPPoE 硬件转发 | ❌ | 未验证（同族 ZX279133 实测不走硬件路径） |

## 原厂驱动重写线（`native` 分支，实验）

主固件用 mt76 驱动 MT7916，能用。`native` 分支做的是另一件事：**不用 mt76**，把原厂
4.1.25 的 `mt7916.ko`（未 strip，25 345 个符号）当作唯一行为规范，在 6.18 上把
WFDMA / MCU 双核 / 私有 TXD 这一层显式重写出来，目标是让无线侧也能接上 SoC 的硬件加速，
而不是停在 mt76 的 CPU 路径上。

三条做法：

1. **读二进制，不猜**。宿主 binutils 不认 ARM，所以自带 ELF32 解析 + capstone 的反汇编工具
   （`native/tools/re/`），按符号取函数、按重定位还原调用目标；
2. **先算再刷**。字段表读全、手算期望值，刷机只验证固件行为，不验证"我算错没有"；
3. **一次一个假设**。一个 boot 只有一轮"复位→下载→RDY"，命令写错会把 WA 核打死只能重刷。

**结论（版本 3.97，2026-11-02）**：前四段对齐成立——固件下载（`fw-state=7`）、校准上传
（4 页 `ret=0`）、band0 的 MAC/DEV/信道/BSS/STA_REC/DRR 全部 `ret=0`、数据帧描述符字节正确
且被 DMA 引擎取走。**但"引擎读走描述符"与"帧真的出现在空中"之间还差两层主机侧初始化**
（MAC 层的 band 寄存器、AP 启动的信标/TBTT/中断收尾），这两层目前是空白，也是下一步唯一要做的事。

完整过程、已确证的寄存器事实、已排除的假设与后续路线见
[docs/原生无线驱动重写-2026-11-02.md](docs/原生无线驱动重写-2026-11-02.md)。

## 构建

WSL（或任意 Linux）内：

```sh
git clone --branch pr-zte-zxhn-h3600 https://github.com/Zlion-Y/openwrt-sanechips-mirror openwrt
git -C openwrt checkout db8b8d4130457dbe46558a7cea872dd2dae539e6
./apply.sh openwrt          # 或 make bootstrap：铺 overlay + 补丁 + 种子配置
make check                  # 本地门禁（语法 / ash / 槽位工具 / dts 交叉引用 / 凭据扫描）
make build                  # make world + 打包成原厂 .bin
```

- 打包默认用随仓库的版本头切片 `seed/vendor-verhdr-8b8.bin`（与官方全包逐字节同果），
  也可用 `--template` 指向你手里的官方包；
- 打包器独立复核几何与 CRC，`tools/verify_img.mjs` 静态筛查"起不来"类问题；
- 详细步骤、网络补丁与注意事项见 [BUILD.md](BUILD.md)。

## 致谢

- **[@ghahramani](https://github.com/ghahramani)** —— H3600 整条线：
  [openwrt#25469](https://github.com/openwrt/openwrt/pull/25469)、
  [构建分支](https://github.com/ghahramani/openwrt/tree/zte-zxhn-h3600)、
  [主线 RFC](https://github.com/ghahramani/linux/tree/h3600-linux-rfc)、
  [工程长文](https://ghahremani.medium.com/resurrecting-isp-e-waste-how-an-ai-breakthrough-unlocked-the-hyperoptic-zte-h3600-ported-modern-60e74c87c11e)。
  本仓库 overlay 基线树即该分支镜像（钉在 `db8b8d4`）；其中若干公开说法在这块板子上不成立，
  逐条更正见 [docs/01](docs/01-情报汇总与事实核验-2026-10-13.md)。
- **[@zwzzrl](https://github.com/zwzzrl/zte-e2631-openwrt)** —— 手动 WAN 口选择、IPv6 硬件卸载打断 TCP
  的发现与因果链、`e2631-wifi-boot` 重配网修复，以及 PPE 逆向的多项输入。
- **[@zwthys-cyber](https://github.com/zwthys-cyber/zte-e2631-openwrt)** —— 升级加固
  （`SHA256SUMS`、仅 https 镜像加速、失败路径回归）与 CI 基线。
- **[cubingmucus998/zte-zx279128s-recon](https://github.com/cubingmucus998/zte-zx279128s-recon)** ——
  这颗 SoC 最早的公开 DTB、分区表与 flash 事实。
- **[@cnjn](https://github.com/cnjn)** —— [zxslc-sr1010](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010)
  的 UART 逆向与主线补丁；[zte-mesh](https://github.com/cnjn/zte-mesh) 自 v1.3 起整体引入。
- **[openwrt.style.dev/h3600](https://openwrt.style.dev/h3600/)** 与
  [恩山讨论帖](https://www.right.com.cn/forum/thread-8487689-1-3.html) —— 只当线索：前者抓取时 502
  且站内说法互斥，后者补了不少没被英文记录的原始行为。
- **OpenWrt / Linux 上游与 mt76 维护者（@nbd168 等）** —— 本仓库只带一个 mt76 补丁与三个 target 补丁。
