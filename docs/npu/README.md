# npu 分支文档索引

> 分支：`npu`（从 `main` 251ac0c 起）。`main`/`ffe`/`ffe-full` 不改。
> 目标：A + W1——保留 OpenWrt 通用用户态，重写 ZX279128S 有线 + MT7916 私有栈。

| 文档 | 内容 |
|---|---|
| [00-参考仓库逆向分析与实施计划](../npu-参考仓库逆向分析与实施计划-2026-10-29.md) | 三个参考仓库、边界、架构、P0-P7、验证方式 |
| [01-原厂运行态基线](01-vendor-runtime-baseline-2026-10-29.md) | 只读 telnet 实测：模块栈、FFE/NPU、IDM0/1、`npu_cap`、kallsyms |
| [02-可行性](02-feasibility-2026-10-29.md) | 7,760 函数/2.59 MiB、9 个 ZTE import、三档可达性、G1-G5 门禁 |
| [03-MT7916 私有栈模型](03-wireless-model-2026-10-29.md) | 12 个入口反汇编调用图、固件路径、WFDMA/MCU/TX/RX、G3 设计 |
| [04-npu-0.1 构建记录](04-build-npu-0.1-2026-10-29.md) | 可刷检查点、sha256、verify_pkg/verify_img、复现命令 |
| [05-FFE 覆盖表](05-ffe-coverage-2026-10-29.md) | 261 符号分类、W1 必做/可延后/未知项 |
| [06-P1 register-diff](06-register-diff-2026-10-29.md) | E2631 vs ZX279133 地址/IDM/TM/PP/IRQ 差异、8 个写寄存器门禁、只读审计计划 |
| [07-npu-0.2 构建记录](07-build-npu-0.2-2026-10-29.md) | 只读审计固件、sha256、`/proc/zx-npu-audit/audit` 用法 |
| [08-npu-0.3 构建记录](08-build-npu-0.3-2026-10-29.md) | `zxnpu0` + 自研 IDM RX/TX 环、默认关闭、上机实验步骤 |
| [09-npu-0.4 构建记录](09-build-npu-0.4-2026-10-29.md) | PPE/CLA 低层访问、`/proc/zx-npu-ppe/cla`、默认关闭 |
| [10-npu-0.5 构建记录](10-build-npu-0.5-2026-10-29.md) | MT7916 G3 骨架：只绑 790a、只读 PCIe/固件审计 |
| [11-npu-0.6 构建记录](11-build-npu-0.6-2026-10-29.md) | vendor MT7916 固件/DBDC 进镜像、rootfs 几何 |
| [12-npu-0.7 构建记录](12-build-npu-0.7-2026-10-29.md) | CLA 寄存器表/多项式/45B key/520 槽规格；poly、slots、entry 自检 |
| [13-npu-0.8 构建记录](13-build-npu-0.8-2026-10-29.md) | MT7916 G3.2：CR 探针、WFDMA 22 写计划、patch 命令/固件头 |
| [14-G3.3 Andes MCU 计划](14-G3.3-andes-mcu-plan-2026-10-29.md) | 命令消息 24B 头、三条 patch 命令；§6 解码：MCU TX=AndesSendCmdMsg 队列+tasklet、fw 子表五槽、环寄存器 4 连寄存器布局 |
| [15-npu-0.9 构建记录](15-build-npu-0.9-2026-10-29.md) | Andes 三条 patch 命令的字节级编码器（`ctl` 写 `mcu`） |
| [register-diff CSV](../../re/npu/register-diff-e2631-vs-zx279133.csv) | 机器可读差异表（block/item/e2631/reference/status/action/evidence） |

## 当前状态

- **npu-0.1 / 0.2 已构建并验证**：0.1 是骨架检查点，0.2 增加只读审计；
  两者数据面仍是 v1.8.0（PPE + mt76/mac80211），不是“原厂逆向完成”。
- 0.2 产物：`out/e2631-openwrt-npu-0.2.bin`，sha256 `10e7d2d5…`；
  `/proc/zx-npu-audit/audit` 写 `run` 才读白名单寄存器，从不写硬件。
- 0.3 产物：`out/e2631-openwrt-npu-0.3.bin`，sha256 `7f550713…`；
  `zxnpu0` + 自研 IDM 环已编入，默认 `enable=0`、不碰硬件。
- 0.4 产物：`out/e2631-openwrt-npu-0.4.bin`，sha256 `7c0500a0…`；
  PPE/CLA 低层访问已编入，默认 `enable=0`。
- 0.5 产物：`out/e2631-openwrt-npu-0.5.bin`，sha256 `a3955389…`；
  W 侧 G3 骨架只绑 790a，默认不 probe、不写 MT7916 寄存器。
- 0.6 产物：`out/e2631-openwrt-npu-0.6.bin`，sha256 `4fd565c6…`；
  vendor MT7916 固件/DBDC 已进 `/lib/firmware/zte/mt7916/`。
- 0.7 产物：`out/npu-0.7/e2631-openwrt-npu-0.7.bin`，sha256 `bcadc201…`；
  CLA 哈希规格全部钉死，`/proc/zx-npu-ppe/cla` 可在线核对硬件槽。
- 0.8 产物：`out/npu-0.8/e2631-openwrt-npu-0.8.bin`；W 侧 CR 探针 +
  WFDMA 计划 + patch 命令，写寄存器需 enable+commit 双开。
- 0.9 产物：`out/npu-0.9/e2631-openwrt-npu-0.9.bin`（20,972,052 B），
  sha256 `46529515…`；verify_pkg ALL CHECKS PASSED + BOOT-READY。
  Andes 三条 patch 命令的字节级编码器编入 `/proc/zx-csp-wlan/ctl` 写 `mcu`。
- **P1 第一版已出**：IDM `+0x08/+0x04` RX A/B、`+0x40/+0x44` TX kick/done、
  `+0x48/+0x4C` RX submit/done 已用 `plat` 反汇编钉死；BMU 自由栈/池基址偏移
  已补到指令级；CLA/SBRG/PM/SPA 的间接口地址、命令字格式
  `index|table<<22|rw<<27`、CLA config 位序已确认。仍未知：
  table/字段语义、SPA hash/match RAM 数据布局、PPU mcode、BMU 运行期尺寸。
- 下一步：P1 的 tm/CLA/SPA/PM 注释 + 只读审计 → G1 有线自研驱动 → G2 FFE →
  G3 MT7916 固件启动。

## 快速复现

```sh
bash apply.sh /home/builder/openwrt
cd /home/builder/openwrt && make target/linux/clean && make target/linux/prepare V=sc
cd /mnt/d/AI/Qoder/E2631-OpenWRT/build && bash scripts/build.sh --no-pack
# Windows:
node host/release.mjs --dir out/build-<stamp> --template build/seed/vendor-verhdr-8b8.bin \
  --out out/e2631-openwrt-npu-0.1.bin --version "OpenWrt npu-0.1"
node host/verify_pkg.mjs out/e2631-openwrt-npu-0.1.bin --dts overlay/target/linux/sanechips/dts/zx279128s-zte-zxhn-e2631.dts
```
