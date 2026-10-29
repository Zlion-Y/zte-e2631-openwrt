# npu-0.1 构建记录（2026-10-29）

> 这是 **A + W1** 的第一个可刷入检查点，不是原厂全栈重写完成。
> 固件里的有线/无线数据面仍是 v1.8.0 的现有实现；本版只增加一个无 MMIO 的
> npu 骨架驱动，用来验证分支构建、打包、刷写链路。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.1.bin` |
| 大小 | 20,972,052 B（0x1400214） |
| sha256 | `b48fb67bfe04bcb6b74763770570986da10d05c8fb09384a60b4af40cd8be447` |
| 版本 | `OpenWrt npu-0.1`（rootfs `/etc/e2631-version`，vendor 头也盖同一版本） |
| 槽位 | 包体 0x1400000 / 槽 0x1c00000；版本头落 slot+0x1400000 |
| 构建 | `npu` 分支，base `main` 251ac0c；内核 6.18.55 |

**未刷入**：设备 192.168.5.3 仍在原厂系统；本轮没有写 flash、没有重启。

## 2. 本版新增

- `overlay/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-npu.c`
  - 平台驱动，匹配 `zte,zx279128s-npu-scaffold`。
  - 只创建 `/proc/zx-npu/status`，**不映射/不读写任何寄存器**。
  - 显示 `npu-0.1-scaffold`、状态、DT 节点和后续计划组件。
- 板 DTS 增加 `npu-scaffold` 节点（无 reg/clocks/interrupts，避免与现有 gmac/PPE 冲突）。
- `patches-6.18/037-...patch`：Kconfig `ZX_NPU` + Makefile `zx279128s-npu.o`。
- `apply.sh`：`CONFIG_ZX_NPU=y`、四条断言（源/补丁/config/DT）。
- `overlay-removed.txt`：列出 ffe-full 的 idm/035/036/991 及首启默认值，防止旧树残留。
- `etc/e2631-version` → `OpenWrt npu-0.1`。

## 3. 构建见证

```
CONFIG_ZX279128S_ETH=y
CONFIG_ZX_NPU=y
# CONFIG_ZX_FFE is not set
```

System.map：

```
c0605c48 t zx_npu_remove
c0605c74 t zx_npu_probe
c0605d08 t zx_npu_status_show
```

ffe-full 污染检查：

```
grep -E ' zx_idm_|zx_wifi_flow_offload' System.map  → 无
```

DTB：

```
npu-scaffold
zte,zx279128s-npu-scaffold
```

rootfs：

```
unsquashfs -cat .../root etc/e2631-version
OpenWrt npu-0.1
```

## 4. 双验证

`node host/verify_pkg.mjs out/e2631-openwrt-npu-0.1.bin --dts .../zx279128s-zte-zxhn-e2631.dts`：

```
ALL CHECKS PASSED (warnings: 1)
```

唯一 warning 是 uImage `ih_hcrc` 与标准算法不一致；`ih_dcrc` 通过，且
vendor 镜像也有同样差异（这是既有已知项，不是本版引入）。

`verify_img`（由 `release.mjs` 跑）：

```
BOOT-READY: no static blocker found
```

## 5. 复现

```sh
# 1. 把 overlay 应用到 base tree（会清理 overlay-removed 里的 ffe-full 残留）
bash apply.sh /home/builder/openwrt

# 2. 干净内核 + 重新 prepare（关键：否则 build_dir 里可能残留 035 的 idm.o）
cd /home/builder/openwrt
make target/linux/clean
make target/linux/prepare V=sc

# 3. 构建 + collect（不打包，WSL 没有 node）
cd /mnt/d/AI/Qoder/E2631-OpenWRT/build
bash scripts/build.sh --no-pack

# 4. Windows 侧打包 + 验证
node host/release.mjs --dir D:/AI/Qoder/E2631-OpenWRT/out/build-<stamp> \
  --template build/seed/vendor-verhdr-8b8.bin \
  --out out/e2631-openwrt-npu-0.1.bin --version "OpenWrt npu-0.1"
node host/verify_pkg.mjs out/e2631-openwrt-npu-0.1.bin --dts build/overlay/.../zx279128s-zte-zxhn-e2631.dts
```

## 6. 这个版本不是什么

- 不是“原厂逆向完成”：有线 FFE/NPU/IDM/TM/PPE 未替换，无线仍是 mt76/mac80211。
- 不是“无线加速可用”：`zx-npu` 只暴露状态，不碰 IDM/WiFi。
- 不是最终默认镜像：后续 G1-G5 门禁通过前，不应把它宣传为完成品。

## 7. 下一步

- G1：有线自研驱动 probe/ping/1G（P1 register-diff 后）。
- G2：FFE IPv4 学习/卸载/conntrack。
- G3：MT7916 自研 driver 固件启动 + 一帧收发（见 `03-wireless-model`）。
- G4/G5：最小 AP、IDM 加速。
- 本检查点的作用：以后每次替换数据面，都用同一套 `verify_pkg`/`verify_img`
  和同一槽位/回退路径，先保证“可刷、可回退”，再谈功能。
