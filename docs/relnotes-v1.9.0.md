## 更新日志

1. **47 MiB userdata 自动接管（本次主打）**：首次启动自动格式化 → 配置迁移 → 启用，重启后 `/overlay` 落到 ~35 MiB UBIFS 卷（内部 jffs2 overlay 仅 ~4.5M），全程无需手动干预。为它修了四层问题：preinit 挂载钩子的尺寸含界检查（本板 mtd0 是 1 MiB 分区不是整片）、preinit 阶段 `/dev` 是裸 tmpfs 需自行 mknod 设备节点、激活门格式化前需先 ubidetach（init 拒绝格式化已挂载卷）、kconfig 首次展开会吞种子里的 `=y`（bootstrap 二次断言）。
2. **槽位布局扩展**：rootfs 窗口 16M → 20M，版本头 `slot+0x1400000` → `slot+0x1800000`。基础镜像此前只剩 117 KB 余量，任何改动都会爆；现在内置插件后仍留 ~3.4M。
3. **常用插件内置（开箱即用，中文界面）**：SmartDNS（DNS 分流加速）、nlbwmon（按设备流量统计）、UPnP（miniupnpd-nftables）、WOL 网络唤醒（luci-app-wol + etherwake）、adblock-fast（去广告，启用一个屏蔽列表即可）。连同 7 个内核模块（tun/tproxy/socket/inet_diag 等）一起烧进 ROM。
4. **sing-box 代理栈（homeproxy）**：sing-box 1.14.0 全功能 + luci-app-homeproxy。全功能二进制 54.6 MB（压缩 19.9M），超出任何槽位配置的容量，因此**以 apk 形式从 userdata 卷安装**（Release 附件提供三个包），不占 ROM；订阅导入/节点管理/中文界面齐全。
5. **原厂网页直刷保持可用**：升级包版本串透传官方模板（V1.0.0.8B8.8000 ≥ 运行中 7B5），24M 包过原厂 integrity 与版本门。
6. **netdev backlog 调优**：`net.core.netdev_max_backlog=4096`（zwzzrl 实测的廉价缓解；本机全栈路径更对症，收益待复测）。

## 实测（2026-11-07，真机）

- userdata 全链：清卷 → 自动采纳（ubiformat 376 块 → 42.6 MiB 卷）→ 配置迁移 → 重启接管 → `/overlay` = UBIFS 35.5 MiB 可用，配置零丢失
- 内置插件：零安装即在（SmartDNS/nlbwmon/UPnP/WOL/adblock-fast + LuCI 中文页 ×4 + WOL）
- 代理链：sing-box 1.14.0 运行、九云订阅 31 节点导入 ✓（节点可用性取决于机场与线路——测试线路对该机场端口段存在管理性拒绝，与固件无关）
- 无线：双频自动广播，2.4G = HE40（ch1）、5G = HE80（ch36）；RF 校准为原厂 caldata
- 内存：~77 MiB / 248 MiB（内置插件 + sing-box 全家运行后）
- 双槽：槽 0 原厂完好，运行槽 = 槽 1；原厂网页直刷门（integrity/版本串）通过

## 刷写

1. **原厂网页（supgrade.html）直刷**：24.0 MiB 包过 integrity 与版本门；也可在 OpenWrt 内 `sysupgrade`（原地升级，配置保留）。
2. **首启全自动**：userdata 自动采纳（无需再手动 e2631-userdata-init），重启一次即接管 35 MiB；无线约 70 秒内自动就绪。**首启为开放 AP + 无 root 密码**，请立即设置。
3. **内置插件**开箱即用（LuCI 中文菜单）。adblock-fast 需启用一个屏蔽列表（配置页勾选，建议 hagezi/jsdelivr 源）。
4. **homeproxy 代理栈安装**：Release 附件提供 `sing-box`、`luci-app-homeproxy`、`luci-i18n-homeproxy-zh-cn` 三个 apk——传到盒子后
   `apk add --no-network --allow-untrusted /tmp/*.apk` 即可（~15 MiB，装在 userdata 卷，固件升级不丢）。
5. 已知问题与限制见 [docs/known-issues.md](https://github.com/Zlion-Y/zte-e2631-openwrt/blob/main/docs/known-issues.md)。

**SHA256**：`8f9e3530c74c0e40979615963f1188d90254c81979eb9fef3fab9f1bdcc15853`
