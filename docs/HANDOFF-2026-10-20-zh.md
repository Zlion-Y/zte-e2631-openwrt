# ZTE ZXHN E2631 OpenWrt 移植与实测：技术交接（2026-10-20）

> **2026-10-20 最新补充（已核验远端）：本地源码现已全部成功推送到远端备份分支。** 下面最初交接中关于“本地 f242afc 尚未上传”“配置迁移还只能从用户电脑取得”的描述已过时，以本补充为准。实验分支是源码归档，**不是发布分支，不能直接编译为面向普通用户的稳定固件。**

**整合后的完整源码与文档：** [handoff/consolidated-2026-10-20](https://github.com/tigaotg/zte-e2631-openwrt/tree/handoff/consolidated-2026-10-20)（包含下面所有本地历史、代码、测试、本文）；**原样保留的本地交接分支：** [handoff/local-2026-10-20](https://github.com/tigaotg/zte-e2631-openwrt/tree/handoff/local-2026-10-20)；稳定/公开展示默认分支仍为 `main`，重要 Wi-Fi 方案在 `feature/wifi-autoboot`。

### 新上传的完整开发提交链（已核对 GitHub）

| 提交 | 已提交内容 | 现有验证边界 |
|---|---|---|
| `0174f5a` | ROM `fstab` 指向固定 `/dev/ubi0_0`，取消旧设备 UUID 依赖 | 静态检查；首次新设备尚未验收 |
| `5d9d851` | `/sbin/e2631-userdata-status`：只读识别 mtd10/已挂载 UBI 卷 | 静态/诊断逻辑；不作擦写 |
| `56270ce` | `/sbin/e2631-userdata-probe`：只读预检，按整分区 SHA256 判断擦除候选 | 仅限预检；**不得将其结果当成用户数据绝对安全保证** |
| `30632aa` | `/sbin/e2631-userdata-firstboot`：`--plan/--scan` 分情况输出建议 | 只读，不自动格式化 |
| `5e454d5` | `/sbin/e2631-userdata-init`：显式授权后 `ubiformat/ubiattach/ubimkvol` | **危险写入程序；仅隔离模拟，真实设备不执行 `--apply`** |
| `d117a51`、`f242afc` | `tests/test_userdata_init.py`：9 项隔离测试及失败注入 | 9/9 本地通过；未执行真实首次初始化 |
| [`00a6b4c`](https://github.com/tigaotg/zte-e2631-openwrt/commit/00a6b4cbe0dffdfef7b135f2ce718803a3aed5c7) | `e2631-hb` 心跳需显式 `/etc/e2631-hb-enable`；新增 `e2631-userdata-migrate` 暂存式迁移草稿 | 本地模拟通过；**不具备 OverlayFS whiteout/xattr 保真或掉电原子激活保证** |

### 目前最重要的实现与风险说明

- **无线自动启动确实已实测**：START=95、延迟约25秒、MT7916 主设备绑定、2.4/5GHz 双 AP 广播、SSID/WPA2 改后重启保留。**历史冷启动 DMA 卡死和客户端关联硬死机尚未完成独立全回归**，不可称驱动彻底稳定。开发分支默认开放 SSID `OpenWrt` 且实验设备可能无 root 密码，公开部署前必须改变安全默认值。
- **Extroot 真机已经成功运行在先前初始化好的卷**：mtd10，47 MiB NAND userdata，UBI `/dev/ubi0_0`，UBIFS `/overlay`，文件系统可用约35.5MiB。尚未解决的是从**空 NAND 到可用 Extroot**的用户级初次初始化、配置/元数据迁移、掉电回滚与兼容旧设备。
- **警告：存在提前激活风险**。当前新 ROM `/etc/config/fstab` 配置 `enabled '1'`；preinit 尝试 UBI attach；`e2631-userdata-init` 只建空 volume，不准备 UBIFS/upper/work 或激活门控。即使写入操作成功，也不能认为新卷能安全接管根 overlay。
- **OverlayFS 元数据**：实机 `/overlay/upper` 中存在 19 个 chr(0,0) whiteout，且可能有 `trusted.overlay.*` xattrs；BusyBox `cp -a` 未证明能原样复制。当前 `e2631-userdata-migrate` 先用 `.e2631-stage` 暂存，再 `mv` 为 `upper` 和创建 `work`，两步不原子，不可直接用于生产迁移。
- **心跳**：`00a6b4c` 中默认关闭心跳，但已验证的候选镜像生成在此提交之前，因此镜像不包含这两项最新改动。
- **固件**：本地 `e2631-portable-f242afc-verified.bin`，20,972,052 B，SHA256 `e17e9df6818e49e1dfaf0180c4aa94bbe708d134264a10fc0deca22f23d2f43d`；静态验证成功，**未上传 GitHub Release，也未对全新设备首次启动验证**。
- **Git 构建注意**：`main` 与 `feature/wifi-autoboot` 已分叉；这个整合分支建立在上传的本地 00a6b4c 之后。禁止直接 force push 到 main、忽略源码边界或将 WIP 当正式 Release。敏感账号、token、NAND dump 与厂商凭证一律不得发布。

---


> **交接人：[@tigaotg](https://github.com/tigaotg)** ｜ **原项目：[@Zlion-Y/zte-e2631-openwrt](https://github.com/Zlion-Y/zte-e2631-openwrt)**  


## A. 阅读优先级与可直接访问的代码

- 我的 Fork：https://github.com/tigaotg/zte-e2631-openwrt
- 重要开发分支：[feature/wifi-autoboot](https://github.com/tigaotg/zte-e2631-openwrt/tree/feature/wifi-autoboot)。**最重要的 Wi-Fi 自动启动实现不在 fork 默认 main 分支，而在此分支。**
- [e2631-wifi-boot](https://github.com/tigaotg/zte-e2631-openwrt/blob/feature/wifi-autoboot/overlay/target/linux/sanechips/base-files/sbin/e2631-wifi-boot)：延迟自动初始化、标记与 AP 建立
- [e2631-wifi-load](https://github.com/tigaotg/zte-e2631-openwrt/blob/feature/wifi-autoboot/overlay/target/linux/sanechips/base-files/etc/init.d/e2631-wifi-load)：START=95、procd
- [e2631-wifi-on](https://github.com/tigaotg/zte-e2631-openwrt/blob/feature/wifi-autoboot/overlay/target/linux/sanechips/base-files/sbin/e2631-wifi-on)：MT7916 EEPROM 准备和 PCIe 绑定顺序
- [e2631-wifi-ap](https://github.com/tigaotg/zte-e2631-openwrt/blob/feature/wifi-autoboot/overlay/target/linux/sanechips/base-files/sbin/e2631-wifi-ap)：PHY/SSID/AP 配置；处理第二个 PHY 的路径 `+1`
- [DTS](https://github.com/tigaotg/zte-e2631-openwrt/blob/main/overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts) 和 [89d8d96](https://github.com/tigaotg/zte-e2631-openwrt/commit/89d8d96d0eee4e54e4ae944c3c383b4168bae08d)：PL310 和 47 MiB userdata
- [LuCI 实验无线页面](https://github.com/tigaotg/zte-e2631-openwrt/blob/feature/wifi-autoboot/overlay/target/linux/sanechips/base-files/www/luci-static/resources/view/e2631/wifi.js)：**实验性质，ACL/实际按钮链路未完整验收**。
- Fork 默认 `main` 与 `feature/wifi-autoboot` **已分叉**；截至本次检查，远端 `main=b56db2d`、`feature/wifi-autoboot=afe4aa2`，比较结果为 feature 相对 main ahead 9 / behind 2。不要盲目 force-push，PR 前请单独评估合并。
- 已有关键提交：[fbeadaa](https://github.com/tigaotg/zte-e2631-openwrt/commit/fbeadaa813504f2cee92d1fc52a343d9adcebe74)（LuCI Bootstrap 个性化署名）、[89d8d96](https://github.com/tigaotg/zte-e2631-openwrt/commit/89d8d96d0eee4e54e4ae944c3c383b4168bae08d)（userdata/PL310/UBIFS）、[29ee7b8](https://github.com/tigaotg/zte-e2631-openwrt/commit/29ee7b8)（无线 autoboot）、[0fae802](https://github.com/tigaotg/zte-e2631-openwrt/commit/0fae8029287f4f1510bebc3b6d416fd6c0bf04a4)（无线默认字段）、[6ab4488](https://github.com/tigaotg/zte-e2631-openwrt/commit/6ab448830e88dda7823bb1b983ec25349e5ab7c5)（预附加 UBI）、[afe4aa2](https://github.com/tigaotg/zte-e2631-openwrt/commit/afe4aa27375c4da027e93e8dc7c04448769c920e)（实验 LuCI 页面）。

## B. 硬件和原始调试历史（避免重复踩坑）

- **ZXHN E2631 VID31**；Sanechips **ZX279128S**（双核 ARM Cortex-A9，约 1 GHz），**256 MiB RAM**，**128 MiB SPI NAND**；无线为 PCIe **MT7916**，PCI IDs `14c3:7906` 主功能、`14c3:790a` 扩展 HIF。不能任意把 790a 当独立网卡绑定。
- 原厂 U-Boot 2013.04，串口 **115200 8N1**。实测串口 4 针中**第 2 针为路由器 TX，第 4 针为 GND**。串口/U-Boot 曾通过 `ping` 确认能和静态 IP 的 TFTP 主机通讯。这属于早期恢复/诊断路径，不代表应该使用 NAND 写入命令重做现有成果。
- NAND / A-B：原厂槽 A 约 `0x00700000` 起 28 MiB，实验槽 B 约 `0x02300000` 起 28 MiB；**保留原厂 A 槽，不做自动槽位切换/覆写**。上游 `supgrade.html` 原厂格式刷入 B 槽的流程已经验证，上游亦有救援/回退工具。传统 JFFS2 `rootfs_data` 动态拆分会影响 Linux 的 `mtdN` 编号。
- 目标构建 Linux **6.18.55**，`target/linux/sanechips`；用户态 soft-float、不能误启用 VFP；该板 USB 不可用，尝试探测 USB 控制器存在整机重启历史，建议保持禁用。SPI-NAND on-die ECC 的 JFFS2 clean marker 需要放在数据区而不是 OOB；保留上游相关补丁及 Kernel cmdline 处理。
- 真机有线正常协商 1 Gbps，上游曾记录吞吐约 305/190 Mbit/s 瓶颈，缺少 offload 和 RX 分流；**本阶段未解决**。

## C. 最重要的完成成果：开机自动 Wi-Fi，不再让早期驱动加载阻塞启动

**问题背景**：原版本在 rc.d 启动关键期加载 `mt7915e`，可能在固件 DMA/WA firmware 下载期间整机卡死、串口停止输出、SSH 尚未启动。旧版因此默认关闭无线，要求手动 `e2631-wifi-on 0` + `e2631-wifi-ap`。

**本次方案/代码**：
1. 无线从早期启动路径移出，新增/调整 `e2631-wifi-load` 为 **START=95 + procd + 默认延迟 25 秒**，让有线网络、LuCI、SSH 优先启动。
2. `e2631-wifi-on` 按明确顺序：先准备 `mediatek/mt7916_eeprom.bin`，暂关 `/sys/bus/pci/drivers_autoprobe`，`modprobe mt7915e`，**手动绑定主功能 0x7906**，再恢复 autoprobe；790a 不应抢先独立绑定导致双 HIF 状态。
3. `e2631-wifi-boot`：`/etc/e2631-wifi-noautoload` 可紧急关闭；`/etc/e2631-wifi-boot-pending` 标记上轮未完成启动，并在下一次启动时跳过一次/清理；`/etc/e2631-wifi-provisioned` 防止覆盖用户已经改过的无线配置。首次默认场景自动创建**开放 SSID `OpenWrt`**，非首次只 reload/up，不重写 SSID、加密或密码。修复默认 UCI `disabled` 字段缺失时的误判。
4. `e2631-wifi-ap` 使用 MAC80211/PHY 真实 sysfs path，为同设备第二 PHY 增加 `+1`；在此板上 `wifi reload` 有时不够，需要 `wifi down; wifi up`。

**真机实测**：
- 手动运行新 `e2631-wifi-boot 0` 成功，日志可见 `wifi-on: binding 0000:01:00.0`，`7906=mt7915e 790a=`、`MT7916 ready`；`phy0-ap0` 为 2.4 GHz channel 1，`phy1-ap0` 为 5 GHz channel 36（80 MHz）。
- 将启动服务部署在设备后执行正常 `reboot`：日志在约 **28.6s** 开始倒计时，约 **54.8s** MT7916 ready，约 **58~60s** 两个 AP 接口启动；hostapd 报告两者 `AP-ENABLED`。用户验证**不登录 LuCI 也自动广播双频 Wi-Fi**。
- 用户修改过 SSID 和 WPA2 密码，再正常重启，**实测成功保留配置**。这解决了“Wi-Fi 只能手动起/开机加载导致无法正常进入管理界面”的工程性启动问题。

**请严守验证边界**：
- 这是**启动时序/恢复性缓解 + 本设备正常重启成功**，**不是**证明驱动底层所有硬锁死已修好。`e2631-wifi-on` 历史注释仍记载三次完全断电后的首次固件下载中有成功/失败/失败，且客户端 **association** 曾让整机无 oops/AER 直接死机；**冷启动多次、客户端关联和高负载长期运行的独立回归仍缺**。
- `e2631-wifi-boot-pending` 持久状态在异常掉电下仍需审核何时创建/清除、何时安全重试；不可说已证明完全自愈。
- 开放 Wi-Fi `OpenWrt` + 测试设备可能无 root 管理密码是**严重安全隐患**，附近用户可能获取管理员权限；此方案是实验室需求，**不能未经安全调整作为普通用户默认发布设置**。

## D. PL310 L2 Cache / PCIe 保护

- DTS：`&l2cc { zte,l2c-io-read-lock; };`；`CONFIG_CACHE_L2X0_IO_LOCK=y`，上游基线已含对应 `outer_io_lock` 保护，避免重复叠加 patch。
- 真机 dmesg **`L2C: serializing device reads with maintenance operations`**，证明选项已在运行内核生效。与 MT7916 冷启/association 稳定性相关的完整因果修复仍待更多对照数据，不能把 dmesg 一条日志视为彻底解决无线死机。

## E. 47 MiB userdata → UBIFS Extroot（已实机使用，不等于首次自动安装完成）

- NAND DTS 新增 `userdata` 分区：`reg=<0x03f00000 0x02f00000>`，即 **47 MiB / 49,283,072 B**，不触及后方原厂区域。注意运行时因 `rootfs_data` 动态拆分，`userdata` **实际为 mtd10**；强烈不要假定 DTS mtd 顺序即 /dev/mtd9。
- 实机 `/sys/class/ubi/ubi0/mtd_num=10`，卷 `ubi0_0` 名称 `userdata`，`/dev/ubi0_0 /overlay ubifs rw`，根 overlayfs 用 `upperdir=/overlay/upper,workdir=/overlay/work`；日志确认 `mount_root: switched to extroot`。UBIFS 文件系统显示总约 **37.5 MiB、可用约 35.5 MiB**。
- 既有实机 Extroot 是在**已经初始化的卷上**验证，历史还依赖旧 JFFS2 early boot 辅助；**新 `79_e2631_userdata` 预初始化脚本、ROM `fstab` 的完整全新安装冷启动路径未单独验收**。新 ROM 配置意图由设备特定 UUID 改为 `device '/dev/ubi0_0'`，避免不可移植 UUID，但 UBI0 枚举/挂载时序也要验证。
- **本次探明的 fstools 行为**：`mount_root` 在 rootfs_data 前尝试 Extroot；`block.c config_load(cfg)` 优先 cfg/upper/etc/config/fstab，其次 cfg/etc/config/fstab，最后 ROM /etc/config/fstab；如有内部 JFFS2 的 fstab，可能覆盖 ROM 行为。对当前设备内部 JFFS2 实际做过**只读挂载**：内部 `upper/etc/config/fstab`、`etc/config/fstab` 以及旧 `upper/sbin/block` 均不存在；现用 ROM/UBIFS `fstab` 是旧 UUID。简单将新 ROM `enabled '0'` 会让既有 UBIFS 用户升级后可能看不见原配置；须兼容性设计。
- **空白卷初始化仍未完成**：Linux 6.18.55 UBIFS 源码确认，空白 UBI volume 在首次 **rw mount** 可执行 `create_default_filesystem`；仅 `ubimkvol` 后未格式化成 UBIFS 时，`block extroot` 的 probe 不认识它。现有本地 `e2631-userdata-init` 只是受显式 `E2631_CONFIRM=ERASE_MTD10_USERDATA_47M` 保护的 `ubiformat → ubiattach → ubimkvol`，不是完整首启。
- 设备中 `/overlay/upper` **19 个 chr 0:0 whiteout**，还可能有 `trusted.overlay.opaque` 等 xattrs；BusyBox cp -a 为 -dpR，无法保证 xattrs。此构建 coreutils 显式 `--disable-xattr`；rsync 的 xattr/ACL 是可选功能但未选择安装。任何 JFFS2→UBIFS 配置迁移**不能只用 cp -a 就标记“无损”**。

## F. 本地代码、模拟测试与**尚未上传**的工作

> 状态来自最后一次用户 WSL 输出；**不是实时连接到用户电脑的文件读取结果**。上传前请在 WSL 重新确认，以免远端误覆盖。

- 本地工作树 `~/e2631-wifi-fix/zte-e2631-openwrt`，分支 `feature/wifi-autoboot`，最后一次用户输出 `HEAD=f242afc`（`test(e2631): cover UBI initialization failure stages`）。**远端 feature 当前 HEAD 为 `afe4aa2`，GitHub 不能找到本地 f242afc，因此该提交及可能的前置本地提交尚未推送**。请先分支保护，禁止 `git push --force`。
- 已知本地脏文件仅两处：`M overlay/target/linux/sanechips/base-files/etc/init.d/e2631-hb`（在 `start_service()` 加 `[ -e /etc/e2631-hb-enable ] || return 0`，诊断心跳默认关闭）；`?? overlay/target/linux/sanechips/base-files/sbin/e2631-userdata-migrate`（未跟踪暂存式配置迁移原型）。
- `e2631-userdata-migrate` 实现仅允许 JFFS2 内部 `upper` 来源，限定 UBI0→mtd10、卷名 userdata，拒绝旧 UUID fstab、旧 `upper/sbin/block`、拒绝覆盖目标；先复制到 `.e2631-stage`，比对账户文件后 `mv` 为 `upper` 并建 `work`。测试过普通配置、隐藏文件、符号链接、失败残留拒绝和错误分区/文件系统；**完整性仍缺 whiteout/xattr、挂载真实性、断电原子提交、激活门控**，不要交给用户执行。
- `tests/test_userdata_init.py` **9 项隔离模拟测试全部通过**：未授权拒绝、预览不会写、分区检查、非空拒绝、UBI0 占用拒绝、ubiformat/ubiattach/ubimkvol 失败即停止等。仅 mock，不是首次真实 NAND 初始化验证。
- `79_e2631_userdata` 在远端 feature 已有早期版本。本机 `openwrt-base/target/linux/sanechips/base-files/lib/preinit/79_e2631_userdata` 还曾做过更严格的 `ubiattach -d 0` 与 UBI0/mtd10 检查；**具体版本一致性仍需本机核对**。
- 此前保留的 LuCI 试验文件（menu JSON、rpcd ACL JSON、wifi.js）**截至本次 GitHub 树检查已经上传到远端 feature**，不是现在还缺失的文件；但是 UI/ACL 真机全链路尚未完成验证，需安全审查 `fs.exec` 授权。
- 本地 `openwrt-base` 编译树不是 Git 完整提交的证据，不要将整个编译树、`out`、vendor 密码或 `*.bin` 敏感固件镜像盲目 `git add .`。
- 额外本地 `f242afc` 及其祖先相对远端 feature 的 DAG 关系未知；**不要试图快进覆盖远端**，应创建新 `handoff/local-2026-10-20` 分支推送做历史留存，待比对后再合并。

## G. 构建与打包成果（需与源版本对应）

- Linux 6.18.55 OpenWrt 全量构建成功。WSL 环境 PATH 含 Windows `/mnt/c/Program Files` 曾导致 GNU find `-execdir` 拒绝执行，使用仅 Linux 绝对路径的 PATH 后成功。
- 本地候选 `~/e2631-wifi-fix/out/e2631-portable-f242afc-verified.bin`：**20,972,052 字节**，SHA256 **`e17e9df6818e49e1dfaf0180c4aa94bbe708d134264a10fc0deca22f23d2f43d`**。原厂外层格式、CRC、布局、静态 BOOT-READY 与关键 squashfs 文件检查通过。**尚未做全新设备/全新 userdata 真机刷入验收、未公开上传为 GitHub Release；也不包含后续本地心跳/迁移脚本改动。**
- GitHub 有一个实验 Release **草稿** `e2631-experimental-6ab4488`，当时没公开发布，且**没有 assets**；不能说发布了固件。候选 bin 与草稿所指构建版本不要混同。
- 原仓库保留了原厂头片段 `seed/vendor-verhdr-8b8.bin`，支持 `host/release.mjs`、`tools/verify_img.mjs`、`wsl/collect.sh` 等。在正式发行前需要源码与构建树一致性、独立 hash、刷机路径检查，不建议重复无谓重编译。

## H. 后续按优先级接手

1. **保留已经验证的无线自动启动/双频 AP 与有线可管理性**，开展多次真实断电冷启动、客户端关联、压力测试；把上游 WIFI-COLD-BIND-HANG 与 WIFI-ASSOC-HARD-HANG 继续保留为待验证，而非贸然标 `fixed`。
2. **Extroot 升级兼容性 + Activation Gate**：已准备的旧 UBIFS 应正常启动；仅空白/未完成迁移的新 userdata 必须退回内部 JFFS2，禁止启动时自动格式化。
3. **元数据无损迁移、断电恢复**：whiteout、xattrs、目录不透明属性、配置、账户文件等；设计明确完成/激活边界，不能用半成品 upper/work。
4. 完成 `e2631-userdata-init` 的授权首启过程；复测 WSL tests、preinit 的 /dev 设备发现、`fstools`、`block-mount` 与最终 squashfs 内容。
5. 审查 LuCI Wi-Fi UI/ACL，并梳理 README 中新旧历史章节。当前两个分支并不一致；文档不可夸大稳定性。
6. 基于**已提交且核对的源码**重新增量编译，隔离实验代码，真机先测试、再考虑有风险提示的 Beta/Experimental Release 或提交上游 PR。

