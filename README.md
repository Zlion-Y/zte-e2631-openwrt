# 中兴 ZXHN E2631（Sanechips ZX279128S）上的 OpenWrt

**中文** · [English](README.en.md)

**要把它构建出来：看 [BUILD.md](BUILD.md)（这份暂时只有英文）。** 那是从一次 clone 到一个能刷进去的包的**唯一受支持路径**，
里面写明了本仓库无法替你准备的两样输入（钉住版本的基线树、原厂版本头模板包）以及所有失败模式。
`make check` 会跑完全部门禁。[docs/architecture.md](docs/architecture.md) 画出各块的对应关系；
[docs/known-issues.md](docs/known-issues.md) 是机器可读的诚实清单，包括真机上**仍然坏着**的部分。

本仓库装着这个端口的全部内容：把基线树变成 E2631 构建的 overlay（`overlay/`）、本地构建路径
（`wsl/`，按 `wsl/INDEX.md` 分组；可移植的入口在 `scripts/`）、板侧脚本与原厂侧刷写计划（`board/`）、
Windows 侧工具（`host/`），以及带证据注记的中文工作文档（`docs/`）。构建在本机的 WSL 里跑，
`make check` 是本地门禁入口；`.github/workflows/check.yml` 在每次 push/PR 上跑同一套检查，
构建由 `.github/workflows/firmware.yml` 按需触发。

    git clone --branch pr-zte-zxhn-h3600 https://github.com/Zlion-Y/openwrt-sanechips-mirror openwrt
    git -C openwrt checkout db8b8d4130457dbe46558a7cea872dd2dae539e6
    ./apply.sh openwrt

`scripts/bootstrap.sh` 把这三行连同 feeds 与 seed 配置一起做掉，并逐步留下见证；
`scripts/build.sh` 接着往下走到一个可刷的包。

打包成原厂升级格式是在任意目录跑 `node host/release.mjs --dir <out/build-dir>`：
`out/`、基线树和模板包都通过 `E2631_WORK`/`E2631_TREE`/`E2631_TEMPLATE` 解析
（环境变量或 `.env`，见 `.env.example`），所以结果不再取决于你当时站在哪个目录里。
它会先打包，再**独立重推一遍几何**，然后按"哪些东西会让机器起不来"筛一遍产物。
版本头模板**随仓库发布**：`seed/vendor-verhdr-8b8.bin` —— 532 字节，就是外层头加那 512 字节的版本头，
打包时不传 `--template` 也能跑。实测同一份构建目录用这份切片、和用 21 MB 原厂全包，打出来的包**逐字节相同**
（sha256 `02b35106…`，也就是已发布的 v1.2）；来龙去脉与重新切一份的注意事项写在 [seed/README.md](seed/README.md)。

在"让四十分钟的构建当测试"之前，有三道筛查必须先过：

各版本的固件、真机状态与变更日志写在 [Releases](https://github.com/Zlion-Y/zte-e2631-openwrt/releases)
里对应版本下；逐条的实测与取舍记在 [docs/known-issues.md](docs/known-issues.md) 与
`docs/v1.*-progress.md`。

`tools/dts_lint.mjs` 对着内核补丁和抓下来的头文件，解析板级文件里的 include、`&labels` 和 dt-binding 宏；
槽位工具用 `-Werror` 原生编译，必须既接受一个合成包、又拒绝一个被翻掉一字节的副本；
`tools/verify_img.mjs` 检查成品镜像里那些会让机器起不来的东西 —— `root=` 对得上 dtb 真正创建的分区顺序、
flash/squashfs/jffs2 驱动是内建的、强制命令行生效、USB 与看门狗保持关闭、soft-float，
以及根文件系统的包清单。

## 这块板的硬件事实（构建者必须知道）

* 一个 WAN + 三个 LAN，四个口全在集成 GbE PHY 上 —— 没有外置 PHY
* 256 MiB 内存，128 MiB SPI-NAND（HYF1GQ4UTACAE，2 KiB 页 / 128 KiB 块）
* **整镜像 A/B 槽**（`0x700000` / `0x2300000`，各 28 MiB），槽角色由版本头
  serial 决定、不固定（cspboot 规则：CRC1 有效 + serial 大者，平手选槽 0）
* 没有 USB：原厂内核里根本没有 USB；在没焊控制器的板子上探测它会重启整颗 SoC
* 用户态 soft-float：这颗 SoC 不实现 VFP，所以 `CONFIG_VFP` 必须保持关闭
* JFFS2 clean marker 放在**数据区**而不是 OOB（补丁 029、030）：带 on-die ECC 的 SPI-NAND 每次编程都会
  重算页 ECC，于是同一页写两次读回来就是不可纠。缺了这条，overlay 重挂失败，机器卡在只读根上
* 内核命令行是编译进去的，并且开了 `CONFIG_CMDLINE_FORCE`：bootloader 传的
  `root=/dev/mtdblock8 rootfstype=jffs2 init=/bin/sh` 说的是原厂布局，且会把 PID 1 换成一个 shell

## 在 WSL 里构建

如果是一台不掺和 Windows 的 Linux 机器，用 `scripts/bootstrap.sh` 和 `scripts/build.sh` —— 那是可移植路径，
读的是同一份 `.env`。下面这节是本机 WSL 的那一半：同样的步骤，再加上为这里的网络打的补丁。

Windows 侧没有交叉工具链，WSL 里有，`wsl/build.sh` 对着 `$E2631_TREE` 跑 CI 当年跑的那些步骤，
本仓库挂在 `$E2631_KIT`。不一样的是网络：这里没有代理，而肯应答的镜像都是**按连接限速**
（kernel.org 73 KB/s、sources.cdn.openwrt.org 25 KB/s、sources.openwrt.org 14 KB/s、
musl.libc.org 在 12 秒探测窗口内一个字节都不给）。三件事让这一个问题不至于拖死构建：

* `wsl/segdl.sh` 把每个 distfile 拆成八段并行字节范围下载，通过 `DOWNLOAD_TOOL_CUSTOM` 挂进去 ——
  那是 `scripts/download.pl` 唯一提供的钩子，因为它的 `select_tool()` 优先选 curl，永远轮不到 aria2c。
  限速是按连接而不是按主机算的，所以 linux-6.18.55.tar.xz（147 MiB）大约三分钟到位，聚合 ~288 KB/s，
  而且它的 sha256 与 download.pl 命令行里那个对得上 ⇒ 结果是**字节精确**，不只是快。
  对范围请求回 200 + 整个文件的服务器（mirrors.huaweicloud.com 就是这样）会退回单连接，
  否则每个 worker 都会收到同一份字节。
* `CURL_OPTIONS="--speed-limit 20000 --speed-time 20"` 给 curl 装了个传输看门狗 —— OpenWrt 默认不设，
  于是一个"接了连接但什么都不发"的镜像会永久阻塞而不是切换。有一次已经装好工具链的构建，
  在 1 MiB 的 musl 包上干等了五分钟。
* `wsl/parget.sh <url> <dl/name> [conns] [sha256]` 是同一招的手动版，给那些必须在重启构建之前就落地的文件。

**故意不提前跑 `make download`**：它会遍历整棵树的每一个包，于是当时正在抓 broadcom-wl 和 libwebsockets
—— 这两个都不在本次构建的选择里 —— 而被选中的包反而在后面排队。

`wsl/collect.sh <out-dir>` 接着把构建产物摆成 CI artifact 的那个样子
（`kernel` / `root` / `e2631.dtb` / `config-6.18` / `e2631-fwslot`），
也就是 `node build/host/release.mjs --dir out/<dir>` 要打包和筛查的形态。发行版里挂着 Windows 的盘，
所以什么都不必靠 stdio 管道搬。`wsl/INDEX.md` 把里面其余脚本分了组（构建路径 / 补丁生成器 / 见证 / 探针），
免得文件数量盖住"该跑哪几个"。

发布仍是手动一步（Release 由本地构建产物创建）：
`node build/host/publish.mjs --tag <t> --bin out/<package>.bin --notes out/<notes>.md`
建 Release、上传包和一份 `SHA256SUMS`，并把 GitHub 自己算出来的 asset digest 打出来，好跟本地 sha256 并排比。
头 16 字节不是那四个原厂魔数的文件它直接拒绝，所以一个只打了一半的包不可能被误发出去。

有一个门禁坑值得单独记：`wsl/inspect-rootfs.sh` 过去只读 `/tmp/finalroot` 而从不解包进去，
于是它报的是上一轮留在那儿的东西 —— 现在它先解包。任何"根文件系统里到底有什么"的回答，
只要来自一个你本次没有刚解出来的目录，那就是个过期答案。

## WAN 口选择（任意有线口当 WAN，手动指定）

原厂叫「WAN/LAN 自适应」/ 盲插，它是在 cspd 的 NetAdapt 里探测上行后用 ioctl 改交换芯片端口角色。
这里完全不碰芯片：四个口本来就是四个独立 netdev，"谁当 WAN"只等于 **wan 接口绑在哪个 netdev、
那个 netdev 不在 br-lan 里**。逆向记录见
[docs/wanlan-adaptive-逆向记录-2026-10-27.md](docs/wanlan-adaptive-逆向记录-2026-10-27.md)。

- **默认就是原厂布局**：`wan` 当 WAN，`lan1/lan2/lan3` 在 `br-lan`。开机不做任何动作，
  没有开机服务、没有热插拔钩子。
- **手动指定**：`e2631-wanport apply <lan1|lan2|lan3|wan>`，或 LuCI 的 `网络 → WAN 口`
  点一行的「设为 WAN 口」。写进 uci，重启后保持；目标口当前有链路时会先拒绝（多半是管理网线），
  确认要换这个口时加 `--force`。
- **WAN MAC 固定**在 `e2631_ports.global.wan_mac`：换物理口时上游看到的 MAC 不变，租约、
  DUID、上游的 MAC 绑定都还能续。
- **逻辑层自检**在 PC 上跑：`sh scripts/selftest-wanport.sh`。

**为什么不做自动识别**：PPPoE / 静态地址 / 对端不开 DHCP 的上行根本探不到；而自动切换能把
管理侧关在门外（管理网线所在口被切成 WAN，WAN 区 `input=REJECT`，SSH/HTTP 全断）。所以
`apply` 是唯一入口。


## 这里刻意不放的东西

`out/` 留在仓库之外，在 `$E2631_WORK`（本机是 `D:\AI\Qoder\E2631-OpenWRT\out`）：每一个构建目录、
打好的镜像、日志和一次性实验脚本。那里是 1.1 GB 要么可再生、要么是 `docs/` 里点了名的证据的东西，
`docs/` 也就地说明了这点 —— 写成 `out/xxx.log` 的路径指的是那个目录，不是本仓库。
`.env` 同样不在这里（在的是 `.env.example`）：它是唯一可能写着某人磁盘路径的文件。
`.gitignore` 另外排除：`board/harvest-H8.txt`（356 MB 的原厂系统采集）、
`board/backup/` 与 `board/web/`（mtd 与驱动 dump、爬下来的原厂 UI）、
`board/config18.xml` 与 `board/webcreds.out.txt`（一份原厂配置导出，和一次**故意**去 grep 密码字段的动作 ——
两个都不该出现在 GitHub 上），以及 `host/askpass.bat`（一个 git-credential helper，唯一用途是把令牌递给 git）。

关于 `host/` 补一句：里面有几个脚本是一次性分析，读的是证据文件 —— 转录、mtd dump、原厂配置导出 ——
而那些文件是**故意**不进 git 的。它们在这里，是因为 `docs/` 引用了它们打印出来的数字，
不是因为它们能在你的机器上跑起来。属于受支持路径的那几个是
`release.mjs`（打包 + 两道筛查）、`verify_pkg.mjs`、`publish.mjs`、`stage.mjs`、`test-token-expansion.mjs`；
构建侧的工具在 `tools/` 和 `wsl/`。

我们还有两个相关的兄弟仓库。`Zlion-Y/e2631-mainline-zx279128s`（主线 Linux 端口）仍是独立一条线 ——
"这颗 SoC 真的能起"的一手证据仍然出自它；而且自 2026-10-14 起它只剩内核：
它以前带的 TFTP / U-Boot bring-up 通路已经退役到该仓库的 `v0.1.1` tag 上。
`Zlion-Y/e2631-openwrt`（stage-1 的 initramfs 脚手架）已经并进来：历史在 `stage1-scaffold` 分支
（`git log --oneline stage1-scaffold`），而 `tools/PROBE.md` —— 第一版板级文件当初是照着哪些未知写的 ——
现在在 `main` 上，并附了一张状态表，说明那些未知里哪些已经被真机闭环、哪些还没有。
那个分支上其余内容都是本仓库内容的旧副本：它的板 DTS、`apply.sh` 和 seed 配置都早于槽几何、
nvmem MAC 接线和 JFFS2 那组补丁；它的 `tools/*.py` 筛查被重写成了 `.mjs`，因为这机器上没有能用的 python。

原厂 telnet 口令同样不在这里 —— 而且 `scripts/secret-scan.sh` 现在就是守住这条的门禁，
它扫的是**形状**而不是某一个字面值。所有曾经带着它的 plan 和 expect 脚本现在都写 `${ZTE_TELNET_PW}`，
`host/telnet_sh.mjs` 与 `host/stage.mjs` 从环境里展开、没设变量就拒绝运行 ——
由 `host/test-token-expansion.mjs` 端到端验证：它在 127.0.0.1 上起一个假 telnet 端点，
检查真值确实到了线上、而字面 token 从来没到过。

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
