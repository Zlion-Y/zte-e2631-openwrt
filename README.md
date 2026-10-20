# 中兴 ZXHN E2631（Sanechips ZX279128S）上的 OpenWrt

**中文** · [English](README.en.md)

## 交接索引：2026-10-20（已上传全部本地工作记录）

**重要：本分支是完整的技术归档，并不是已验证可向普通用户发布的固件。** 在原作者 [Zlion-Y](https://github.com/Zlion-Y/zte-e2631-openwrt) 完成基础适配、A/B 及封包工作的基础上，我继续完成了 Wi-Fi 启动路径改进、PL310 相关保护、47 MiB UBIFS Extroot 实测，以及尚未完结的首次初始化安全原型。

- **[完整项目交接：已验证结果、源码、所有主要风险](docs/HANDOFF-2026-10-20-zh.md)**；适合直接发给上游原作者。
- **已验证于一台测试机**：Linux 6.18.55 启动；PL310 日志确认保护启用；MT7916 延迟加载后双频 AP 自动广播、修改 SSID/WPA2 后正常重启保留；已准备好的 userdata UBI/UBIFS 卷能够作为 `/overlay` 使用，显示约 37.5 MiB 总容量、35.5 MiB 可用。
- **开机无线问题的真实边界**：之前在早期 `rc.d` 加载无线导致 SSH 未启动即整机卡死；新的 `START=95`、procd、延迟 25 秒与 PCIe 主功能手动绑定使已测试的正常重启能够自动进入网络管理并启动 AP。**未证明所有冷启动 DMA 硬卡死和客户端关联硬死机已根治**。
- **安全 WIP（未完成、不能直接用于全新设备）**：`e2631-userdata-status`、`e2631-userdata-probe`、`e2631-userdata-firstboot` 是检测/规划；`e2631-userdata-init` 包含需显式确认的 NAND 写入，**尚未完成首次 UBIFS 创建/迁移/激活**；`e2631-userdata-migrate` 是文件暂存复制原型，尚不能确保 whiteout、xattrs 及断电安全。已有 9 项隔离模拟测试通过，**不是实机首次初始化测试**。
- **心跳默认停用**：`e2631-hb` 现需 `/etc/e2631-hb-enable` 才启动，防止持续诊断写入；后期本地更改不包含在此前核验的镜像里。
- **分支用途**：本分支 `handoff/consolidated-2026-10-20` = 已公开的本地实验代码 + 技术文档；`handoff/local-2026-10-20` = 原样保存的本地提交；`feature/wifi-autoboot` = Wi-Fi 功能开发；`main` = 默认首页分支，**不包含整套无线 autoboot 或最新初始化实验代码**。无需、也不应为传递代码强行合并这些分支。
- **安全提示**：开发中使用的开放 SSID `OpenWrt` 和可能无管理员密码的配置极不安全，不适合直接面向公众分发。原厂 A 槽不能自动覆写；禁止自动格式化 userdata。

下文 2026-10-19 状态及 2026-10-14 原作者历史测试继续保留作对照；如两处相矛盾，优先看上方最新状态与完整交接中的验证边界。

---

## 当前开发状态（2026-10-19）

本仓库由 [tigaotg](https://github.com/tigaotg) 维护，
基于 [Zlion-Y/zte-e2631-openwrt](https://github.com/Zlion-Y/zte-e2631-openwrt)
继续开发，并保留上游贡献记录。

| 功能 | 验证状态 |
|---|---|
| Linux 6.18.55 | 真机启动成功 |
| ZX279128S 双核 | 正常运行，无需禁用 CPU 核心 |
| PL310 L2 Cache 修复 | 已验证 |
| MT7916 2.4 GHz / 5 GHz | AP 启动与无线连接已验证 |
| Wi-Fi 开机自动加载 | 重启测试成功 |
| 默认 Wi-Fi | OpenWrt，无线密码为空 |
| 用户 SSID、密码持久化 | 重启测试成功 |
| 原厂 A / OpenWrt B 双槽 | 已验证 |
| LuCI | 官方 Bootstrap，简体中文 |
| NAND userdata | DTS 中包含 47 MiB 分区 |
| UBIFS Extroot | 当前设备验证成功，可写容量约 37.5 MiB |

### 尚待解决

- MT7916 多次彻底断电后的冷启动可靠性。
- 自动加载中断后的无线恢复机制。
- 新刷机设备的 UBIFS Extroot 首次启动验证。
- 首次安装和用户配置的完整回归测试。
- 默认开放 Wi-Fi 与无管理员密码组合的安全风险。

### 最新实验固件：6ab4488（2026-10-19）

已制作实验固件，并在一台 ZTE E2631 VID31 上完成真机刷入与启动验证。

- Linux 6.18.55 成功启动，双核 Cortex-A9 正常运行。
- 通过原厂升级页面将 OpenWrt 写入 B 槽，保留 A 槽原厂系统。
- MT7916 双频 Wi-Fi 自动初始化成功。
- 47 MiB userdata 分区的 UBI/UBIFS Extroot 自动挂载成功。
- LuCI 管理界面可用。

**当前限制：**

- Extroot 配置仍包含测试设备专属 UUID。
- 全新设备尚不能保证自动初始化 userdata。
- Wi-Fi 长期稳定性及反复冷启动仍需验证。
- 尚未确认从仓库源码完整复现本次固件的所有构建输入。

目前 GitHub Release `e2631-experimental-6ab4488` 仅保存为草稿，
尚未公开发布或上传固件附件。

**本版本不应作为适用于所有 E2631 的正式安装固件。**

下方保留原始构建说明与历史测试记录。

---


**要把它构建出来：看 [BUILD.md](BUILD.md)（这份暂时只有英文）。** 那是从一次 clone 到一个能刷进去的包的**唯一受支持路径**，
里面写明了本仓库无法替你准备的两样输入（钉住版本的基线树、原厂版本头模板包）以及所有失败模式。
`make check` 会跑完全部门禁。[docs/architecture.md](docs/architecture.md) 画出各块的对应关系；
[docs/known-issues.md](docs/known-issues.md) 是机器可读的诚实清单，包括真机上**仍然坏着**的部分。

本仓库装着这个端口的全部内容：把基线树变成 E2631 构建的 overlay（`overlay/`）、本地构建路径
（`wsl/`，按 `wsl/INDEX.md` 分组；可移植的入口在 `scripts/`）、板侧脚本与原厂侧刷写计划（`board/`）、
Windows 侧工具（`host/`），以及带证据注记的两份中文工作文档（`docs/`）。**这里已经没有 CI** ——
构建发生在本机的 WSL 里，`make check`（跑 `wsl/check-kit.sh` 与 ash/dts 门禁）加上
`node build/host/release.mjs` 就是原来放在 workflow 里的那些检查。

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
`tools/dts_lint.mjs` 对着内核补丁和抓下来的头文件，解析板级文件里的 include、`&labels` 和 dt-binding 宏；
槽位工具用 `-Werror` 原生编译，必须既接受一个合成包、又拒绝一个被翻掉一字节的副本；
`tools/verify_img.mjs` 检查成品镜像里那些会让机器起不来的东西 —— `root=` 对得上 dtb 真正创建的分区顺序、
flash/squashfs/jffs2 驱动是内建的、强制命令行生效、USB 与看门狗保持关闭、soft-float，
以及根文件系统的包清单。

与 H3600 那份板级文件**有意不同**的地方：

* 一个 WAN + 三个 LAN，四个口全在集成 GbE PHY 上 —— 没有外置 PHY
* 256 MiB 内存，不是 512
* **整镜像 A/B 槽**（`0x700000` / `0x2300000`，各 28 MiB），而不是 4 MiB kernel 分区 + UBI
* 没有 USB：原厂内核里根本没有 USB；在没焊控制器的板子上探测它会重启整颗 SoC
* 用户态 soft-float：这颗 SoC 不实现 VFP，所以 `CONFIG_VFP` 必须保持关闭
* JFFS2 clean marker 放在**数据区**而不是 OOB（补丁 029、030）：带 on-die ECC 的 SPI-NAND 每次编程都会
  重算页 ECC，于是同一页写两次读回来就是不可纠。缺了这条，overlay 重挂失败，机器卡在只读根上
* 内核命令行是编译进去的，并且开了 `CONFIG_CMDLINE_FORCE`：原厂 U-Boot 传的是
  `root=/dev/mtdblock8 rootfstype=jffs2 init=/bin/sh`，那说的是原厂布局，而且会把 PID 1 换成一个 shell

上面两处配置改动是 per-target 的，所以一棵在这里 overlay 过的树构建出来的是 E2631，不是 H3600。

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

发布是最后一个本地步骤，因为已经没有 CI 了：
`node build/host/publish.mjs --tag <t> --bin out/<package>.bin --notes out/<notes>.md`
建 Release、上传包和一份 `SHA256SUMS`，并把 GitHub 自己算出来的 asset digest 打出来，好跟本地 sha256 并排比。
头 16 字节不是那四个原厂魔数的文件它直接拒绝，所以一个只打了一半的包不可能被误发出去。

有一个门禁坑值得单独记：`wsl/inspect-rootfs.sh` 过去只读 `/tmp/finalroot` 而从不解包进去，
于是它报的是上一轮留在那儿的东西 —— 现在它先解包。任何"根文件系统里到底有什么"的回答，
只要来自一个你本次没有刚解出来的目录，那就是个过期答案。

## 历史测试记录（2026-10-14，v1.2 / gen1）

> 以下记录仅代表当时版本，并非当前开发状态。

当前包：Release **`v1.2`** —— `e2631-openwrt-v1.2.bin`，20,972,052 B，sha256
`02b35106a1259ed1aed04c35cb6b037c635b6fb68f3e2906da2931cb96d12ddc`。它的内核段与下表里那个 `gen1` 镜像
（就是刷进板子并跑着的那一个）**逐字节相同** ⇒ 启动路径没有新变量；所有差异都在 rootfs 里：

| release | rootfs 侧 |
|---|---|
| `v1.0` | 第一个能刷能启的包；没有 `/sbin/e2631-wifi-ap`；600 秒 trial 保险仍会自动退回原厂槽 |
| `v1.1` | 加上 `/sbin/e2631-wifi-ap`；首启自动回退改成需要手动武装（`touch /etc/e2631-trial-arm`） |
| `v1.2` | 修好那个 helper：命名段解析（生成器写的是 `radioN`/`default_radioN`，不是 `@wifi-iface[n]`）、一个器件上第二条 phy 的 `+N` path 后缀、以及用 `wifi down/up` 而不是光 `wifi reload`。真机确认：`phy1-ap0 type AP ssid E2631 channel 36 (5180 MHz)`、hostapd 在跑、**手机能看到 5G** |

手动开射频，两条命令，开机仍然静默（没有 `/etc/e2631-wifi-autoload`，驱动没加载）：
`/sbin/e2631-wifi-on 0` 然后 `/sbin/e2631-wifi-ap`。真机可用；老的 `v1.0` 注记只因为里面那些实测数字
没变才留着。

| | |
|---|---|
| 通过原厂 `supgrade.html` 页面刷入 | **可用** —— 页面原样收下这个包，写入槽 1，serial 自己从 33 走到 34，然后重启进我们的 OpenWrt |
| 从 OpenWrt 切回原厂系统 | **可用** —— `e2631-fwslot switch` + `reboot`，failsafe 里也够得着 |
| LuCI（Argon 主题）、`reboot`、三色灯 | **可用** |
| 有线 | 链路训练到 1 Gbps，但吞吐在 ~305 Mbit/s 进 / ~190 Mbit/s 出封顶 —— 驱动没有 checksum/TSO/SG/RSS offload、只有一个 RX 队列，而且这些活全落在 CPU0 上，尽管两个核都在线 |
| Wi-Fi（MT7916） | **能用、不稳定、默认关闭** —— 见下 |

除非存在 `/etc/e2631-wifi-autoload`，或者有人手动跑 `/sbin/e2631-wifi-on 0`，射频不会自己起来。
这是**故意的选择，不是绕路**：只要驱动是从 `rc.d` 加载的，一次失败的实验就足以让我再也回不进这台机器。
"AP 在广播、monitor 接口、空口 RX"之前的每一步都已在真机上确认可用。两件事还没确认：
**冷启动后的第一次固件下载**大约三次里成一次（同一份镜像上三次冷启：挂、成、挂）；
以及**客户端关联**仍然会把机器硬死机 —— 静默、没有 AER 记录、没有 oops。
两条都带着样本数据写在 `docs/02-...md` 的 P 节。

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
- [cubingmucus998/zte-zx279128s-recon](https://github.com/cubingmucus998/zte-zx279128s-recon) —— 这颗 SoC 的公开侦察材料。
  最早那批 DTB、分区表和 flash 事实，很大一块出自这里。
- **@cnjn** —— [linux-mainline-zte-zxslc-sr1010](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010)：
  ZX279133 的 UART 逆向与主线补丁系列，是这条线上的前序工作。E2631 的若干结论是拿它当对照才确认（或被推翻）的。
- [openwrt.style.dev/h3600](https://openwrt.style.dev/h3600/) —— 海外的 H3600 适配记录。
  2026-10-15 抓它时返回 502，而且站内并存三套互不兼容的回退说法 ⇒ 把它当线索，不要当权威。
- [恩山无线论坛上关于这台机器的讨论](https://www.right.com.cn/forum/thread-8487689-1-3.html) —— 中文社区那一侧的记录，
  包括一些没人用英文写下来的原厂行为。
- **OpenWrt 与 Linux kernel 两个上游项目**，以及 mt76 的维护者们（**@nbd168** 等）。我们只带了**一个** mt76 补丁
  （`overlay/package/kernel/mt76/patches/002-mt7915-use-caldata-file-before-efuse.patch`）和**三个** target 补丁
  （`029`/`030` 是 SPI-NAND 上的 JFFS2，`031` 是 PCIe 链路重训练那道闸）；其余全是上游原样。

上面这些若有写错或漏掉的，正确的地方是把它连同证据一起补进 `docs/01-...md`，而不是在这里加个脚注。
