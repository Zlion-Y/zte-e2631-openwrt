## 更新日志

1. **WAN 侧放行 80/443**：盒子挂在主路由下时，`http://<wan-ip>` 与 `ssh root@<wan-ip>` 均可直连（私网段 22/80/443）。
2. **在线升级版本比较修复**：按数字逐段比较版本（1.10 > 1.9），非更新版本默认拒刷；不再把 v1.3 报成 v1.7 的"更新"。
3. **47 MiB userdata 分区可用了**：偏移常量手抄错误（66384896 ≠ 0x3f00000=66060288）导致初始化永远拒绝，已修。
4. **UBIFS 魔数判断修复（5 处）**：盘上为小端 `31 18 10 06`，此前按大端比较导致永远判"未格式化"；preflight 顺带改为读 UBI 卷。
5. **`USE_PROCD=0` 空转修复（2 处）**：rc.common 判 `[ -n "$USE_PROCD" ]`，字符串 "0" 非空导致 extroot 激活门与 netune 调优从未执行。
6. **版本号改为三位**（v1.8.0 起，tag 与 `/etc/e2631-version` 统一）。

## 实测（2026-10-29，真机）

- 有线：4 口齐全，WAN 1000M full，rx/tx/CRC 错误 0；硬件卸载 IPv4 生效（ppe_active=1，54 条流，skip/fail 0）
- 无线：双频自动广播，2.4G = HE40（ch1）、5G = HE80（ch36）；RF 校准为原厂 caldata
- IPv6：wan6 拨号正常，外网 ping6 通
- 47 MiB 分区：格式化 376 块 → 42.6 MiB 卷 → 挂载 37.5 MiB，读写通过；**默认关闭**，需手动初始化
- WAN 侧：`ssh` / `http` 直连实测通过
- 双槽：槽 0 原厂完好（serial 78），运行槽 = 槽 1（serial 80）

## 刷写

1. 原厂网页（supgrade.html）或 OpenWrt 内 `sysupgrade` 均可；升级目标为**运行槽**，写前自动校验另一槽原厂镜像完好。
2. **首启为开放 AP + 无 root 密码**：先连 WiFi 或从 WAN 侧登录后台，**立即设置无线密码与 root 密码**。
3. 开机后无线约 70 秒内自动就绪（2.4G/5G 双频）；47 MiB 存储需手动执行一次
   `E2631_CONFIRM=ERASE_USERDATA_PARTITION /sbin/e2631-userdata-init --apply` 并重启启用。
4. 已知问题与限制见 [docs/known-issues.md](https://github.com/Zlion-Y/zte-e2631-openwrt/blob/main/docs/known-issues.md)。

**SHA256**：`56610856ca44075a54d0afb85697589b1757d6e05bd2a5f4de5c7705502678b7`
