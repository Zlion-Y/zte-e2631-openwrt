# ZTE ZXHN E2631 · OpenWrt

中兴巡天 AX3000（ZX279128S）的 OpenWrt 移植：overlay + 板级工具 + 打包器。
固件与各版本变更日志见 [Releases](https://github.com/Zlion-Y/zte-e2631-openwrt/releases)；
逐项实测与取舍记在 [docs/known-issues.md](docs/known-issues.md)。
本仓库不提供、也不链接原厂固件；随仓库分发的第三方物料见 [NOTICE](NOTICE)。

## 芯片与硬件状态

| 芯片 / 部件 | 规格 | 驱动 | 状态 |
|---|---|---|---|
| SoC | ZTE ZX279128S，双核 Cortex-A9 @1 GHz，256 MiB DDR2 | — | ✅ |
| 千兆交换 | SBRG 五口交换 + 4×内部 GbE PHY（ZX5201，eth0-3 独立 netdev） | `zx279128s-eth`（内建） | ✅ |
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
| 47 MiB userdata 扩展存储 | ⚠️ | UBI/UBIFS 格式化+挂载实测通过；默认关闭，需手动初始化后启用 |
| LuCI 中文后台（Argon） | ✅ | 板卡页：槽位表 / 在线升级 / WAN 口选择 / 备份 |
| WAN 侧调试访问 | ✅ | WAN 区放行私网段 22/80/443（**首启无密码，先设密码**） |
| 在线更新 | ✅ | GitHub Release 检查 + 下载，SHA256 强制校验，按数字比较版本 |
| LED 状态灯 | ✅ | 启动红慢闪 → 联网绿常亮 / 离线红常亮 |
| NTP / 时区 / 中文 | ✅ | Asia/Shanghai，国内 NTP 源 |
| USB | ❌ | 保持禁用（未贴片，探测挂死） |
| PPPoE 硬件转发 | ❌ | 未验证（同族 ZX279133 实测不走硬件路径） |
| Wi-Fi 关联死机 / 冷启挂死 | ❌ | 未根治；有 pending 标记自愈兜底（损失一次断电） |

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

- **@ghahramani** —— H3600 这一整条线：[openwrt/openwrt#25469](https://github.com/openwrt/openwrt/pull/25469)、
  构建分支 [ghahramani/openwrt 的 `zte-zxhn-h3600`](https://github.com/ghahramani/openwrt/tree/zte-zxhn-h3600)、
  24 补丁的主线 RFC [ghahramani/linux 的 `h3600-linux-rfc` 分支](https://github.com/ghahramani/linux/tree/h3600-linux-rfc)，
  以及那篇[工程长文](https://ghahremani.medium.com/resurrecting-isp-e-waste-how-an-ai-breakthrough-unlocked-the-hyperoptic-zte-h3600-ported-modern-60e74c87c11e)。
  本仓库 overlay 的那棵基线树就是该分支的镜像
  （[`Zlion-Y/openwrt-sanechips-mirror`](https://github.com/Zlion-Y/openwrt-sanechips-mirror)，钉在 `db8b8d4`）——
  没有它，这个端口得从一个空 `target/` 开始啃。同时要说清楚：其中若干公开说法在这块板子上没活下来
  （`kernel1`/`kernel2` 这两个分区名、"`mtd erase kernel1` 就能回原厂"、`TESTED.md` 里那个 sha256 和它的"USB 3.0"），
  每条都连同"是哪个实测把它关掉"的记在
  [docs/01-情报汇总与事实核验-2026-10-13.md](docs/01-情报汇总与事实核验-2026-10-13.md) 里。
  感谢和更正都是认真的。
- **[@zwzzrl](https://github.com/zwzzrl/zte-e2631-openwrt)** —— 同型端口的真机迭代者，本仓库
  v1.4–v1.8 的很大一部分来自他的实测与代码：
  - **手动 WAN 口选择**整个功能（`sbin/e2631-wanport`、LuCI「网络 → WAN 口」页、`e2631_ports`、
    14 场景离线自检）连同他的真机整链验收（wan → lan3 → 回退）一起采纳；他同时用两起真实事故
    证明了「自动盲插」不可取（PPPoE/静态上行探不到；自动切换会把管理网口切成 WAN，WAN 区
    input=REJECT → SSH/HTTP 全断，只能到现场换线）。
  - **硬件卸载打断 IPv6 TCP** 是他先在真机上测出来的（握手 1ms、载荷 0 字节），并定位到
    `zx_update_traps()` 清 IPv6 ethertype trap + MAC 移出 trap-DMAC 表的联动。本仓库据此
    选择「改驱动而非关功能」（PPE 激活时 IPv6 trap 恒开），但问题的发现与因果链是他的。
  - `e2631-wifi-boot` 的**重配网修复**（标记存在但 uci 里没有 wifi-device 时重新 provision，
    否则整个开机周期 LuCI 都没有无线页）、出厂 `wireless` 段、apply.sh 的
    **overlay-removed 删除同步**机制、rpcd 插件执行位、`/etc/config` 权限归一，均来自他的真机踩坑。
  - 他对 **PPE/IPv6 卸载的外部哈希表**、原厂 CLA 初始化寄存器、520 槽/5 张 hash 表、以及
    「PPE 模块 AUTOLOAD 会把机器搞成开机挂死」的警告，构成了 `docs/原厂加速方案逆向-2026-10-28.md`
    的重要输入（其中「开机挂死只能串口救」那条，直接决定了我们不在未验证的情况下发货 FFE 控制面）。
- **[@zwthys-cyber](https://github.com/zwthys-cyber/zte-e2631-openwrt)** —— 升级加固与 CI：
  强制且严格解析的 `SHA256SUMS`、镜像加速仅 https、限定发布仓库、升级失败路径回归测试
  （`tests/test_upgrade.py`），以及本仓库 `.github/workflows/` 的两个 workflow 的基线。
- [cubingmucus998/zte-zx279128s-recon](https://github.com/cubingmucus998/zte-zx279128s-recon) —— 这颗 SoC 的公开侦察材料。
  最早那批 DTB、分区表和 flash 事实，很大一块出自这里。
- **@cnjn** —— [linux-mainline-zte-zxslc-sr1010](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010)：
  ZX279133 的 UART 逆向与主线补丁系列，是这条线上的前序工作。E2631 的若干结论是拿它当对照才确认（或被推翻）的。
  同作者的 [zte-mesh](https://github.com/cnjn/zte-mesh)（原厂 multiapd 沙箱化当 mesh 主控，[恩山帖](https://www.right.com.cn/forum/thread-8486503-1-1.html)）
  自 v1.3 起以 `overlay/package/network/zte-mesh` 整体引入：PAUSE 帧压上传、PPPoE 不走硬转、JFFS2 清除标记放数据区
  这几条同族坑也是他的实测先指出来的。
- [openwrt.style.dev/h3600](https://openwrt.style.dev/h3600/) —— 海外的 H3600 适配记录。
  2026-10-15 抓它时返回 502，而且站内并存三套互不兼容的回退说法 ⇒ 把它当线索，不要当权威。
- [恩山无线论坛上关于这台机器的讨论](https://www.right.com.cn/forum/thread-8487689-1-3.html) —— 中文社区那一侧的记录，
  包括一些没人用英文写下来的原厂行为。
- **OpenWrt 与 Linux kernel 两个上游项目**，以及 mt76 的维护者们（**@nbd168** 等）。我们只带了**一个** mt76 补丁
  （`overlay/package/kernel/mt76/patches/002-mt7915-use-caldata-file-before-efuse.patch`）和**三个** target 补丁
  （`029`/`030` 是 SPI-NAND 上的 JFFS2，`031` 是 PCIe 链路重训练那道闸）；其余全是上游原样。

上面这些若有写错或漏掉的，正确的地方是把它连同证据一起补进 `docs/01-...md`，而不是在这里加个脚注。
