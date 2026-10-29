## 更新机制设计（v1.4 起，2026-10-28 修订；v1.3 版见 git 历史）

目标：**在线更新自己的系统，任何时候都不写原厂系统；升级原地重写「当前运行槽」，
除运行槽外不动 flash 上任何地方。**

## 不变量

1. **原厂系统永远住在「非运行槽」**——不是「永远住在槽 0」（v1.3 及更早的口径，已废）。
   槽角色由 cspboot 按「header CRC1 有效且 serial 更大（平手选槽 0）」决定，原厂与
   OpenWrt 哪个在哪个槽都可能、且真实发生过换位（2026-10-24，known-issues
   ROOTFS-SLOT-ASSUMPTION）。因此：
   - `lib/upgrade/platform.sh` 调 `fwslot upgrade-check` + `fwslot upgrade`，
     **原地重写运行槽**，另一槽（原厂所在）从不是写目标；
   - `fwslot upgrade` 写槽前先验证「另一槽 = 完好的原厂镜像」（按内容判定，
     不是按槽号）——它是写坏时的唯一退路，验证不过就拒绝（`--no-fallback-check`
     可显式放弃该保护）；
   - 把 OpenWrt 装进原厂所在的槽是另一条显式路径：`fwslot install
     --replace-other-system`，写之前先把该槽头 + 前 1 MiB 备份到 userdata，
     备份失败即拒绝写入；`install` 对「原厂或无法识别」的内容默认拒绝；
   - `--allow-slot0`（v1.3：按槽号放行槽 0）已删除——按槽号判断语义本身就是错的。
2. 更新失败不可能变砖：写的是运行槽，写坏了 cspboot CRC 拒绝并回落另一槽
   （原厂，所在槽号不固定）。写槽前 `fwslot` 会预检固定偏移区的坏块；
   写完按 cspboot 的方式回读验证，不过就擦掉头保持原槽引导。
3. **升级不保留设置**（用户决定，2026-10-24）：配置档案由用户在升级前自行下载
   （系统 → 备份），升级后自行上传恢复。 userdata/extroot 上的一切（已装包、
   其上的配置）天然不受槽重写影响。

## 两条更新路径

| 路径 | 命令 | 配置 |
|---|---|---|
| **在线更新**（主路径） | `e2631-upgrade run` | 不保留（先下载备份） |
| LuCI 手动上传 | 系统→备份/升级，选 .bin | 同上 |

## 在线更新流程（e2631-upgrade）

1. `check`：GET GitHub API `releases/latest`（libustream 已入镜像），
   比对 `/etc/e2631-version`；带文件参数时仅校验该文件；
2. **镜像加速**：`uci set e2631.upd.mirror='https://ghproxy.example/'`
   （板卡页有输入框）——所有 `https://github.com/...` 下载 URL 前自动加该前缀，
   API 与其他源不受影响；sha256 校验覆盖加速后实际到达的文件；
3. `run` = 下载（.bin + SHA256SUMS，**清单强制且严格解析**，缺失/重复/格式错即拒绝
   —— zwthys-cyber fork 的加固）→ sha256 + `fwslot check` 双校验 →
   `sysupgrade`（platform_do_upgrade → `fwslot upgrade-check` + `fwslot upgrade`
   → 重启）。镜像加速前缀只接受 https。

## sysupgrade 槽适配（platform.sh）

只收**原厂格式 .bin**（supgrade.html 同款）；树产的 sysupgrade .tar 拒收
（槽内固定偏移必须过 `fwslot` 坏块检查写入器）。`platform_check_image` 与
`platform_do_upgrade` 都先跑 `fwslot upgrade-check`（运行槽可判 + 另一槽为完好
原厂），不过即止；写槽写**运行槽**，失败即止。

## 为什么不做 flash 上的配置停泊（修订记录）

首版曾把 sysupgrade.tgz 停泊在 userdata 分区头、首启自动恢复；应用户要求改为
**下载式备份**：升级全程除槽 1 外不写 flash 任何区域，userdata 的内容
（extroot 时的包与配置）完全不被升级触碰。代价：非 extroot 用户的
/etc/config 需手动备份/恢复（LuCI 两步）。

## 槽无关引导（未竟事项）

`zx279128s-slotfix`（v1.3 已在镜像里）读 cspboot 的 bootpara 失败——zImage 解压器把
0x40007000 用作页表（dump 实证），ATAG 列表也因无 memreserve 被内核复用。v2 需要
U-Boot `bdinfo` 的 bi_boot_params 地址 + DT memreserve + ATAG 解析。在 slotfix v2 落地前，
镜像应处于槽 1（上述所有路径都保证这一点）。
