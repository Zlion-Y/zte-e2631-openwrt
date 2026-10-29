# npu-0.2 构建记录：只读审计检查点（2026-10-29）

> 这是 npu 分支的第二个可刷检查点，仍然**不是 vendor-native A+W 数据面**。
> 数据面仍是 v1.8.0；本版只新增 P1 只读审计模块，用于把寄存器差异表
> 变成可上机核对的工具。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.2.bin` |
| 大小 | 20,972,052 B（0x1400214） |
| sha256 | `10e7d2d52e9e9ac1d648fdd2a7d68354c00b8b511a06725963043d1758fcff47` |
| 版本 | `OpenWrt npu-0.2` |
| verify_pkg | ALL CHECKS PASSED（1 个既有 uImage `ih_hcrc` warning） |
| verify_img | BOOT-READY: no static blocker found |

未刷机；设备仍在原厂系统。

## 2. 新增

- `zx279128s-npu-audit.c`（内置，`CONFIG_ZX_NPU_AUDIT=y`）：
  - 匹配 `zte,zx279128s-npu-audit`，创建 `/proc/zx-npu-audit/audit`。
  - 默认不读任何寄存器；向该文件写 `run` 才执行一次白名单读。
  - 用 `ioremap()` 映射固定窗口（NPP `0x921C0000`、IDM `0x921C8000`、
    TM `0x92340000`、PP `0x92380000`），**不做任何写操作**。
  - 读回：NPP `00/04/48/4c`；IDM `00/10/18/1c/24/40/44/48/4c/80`；
    BMU `04/08/58/5c/e8/ec/f4/f8/fc`；CLA `c080/c088/c090`。
  - 结果写 `/proc/zx-npu-audit/audit`，供 P1 对照。
- 板 DTS `npu-audit` 节点（无 reg/clocks/interrupts）。
- 补丁 `038-zte-add-the-ZX279128S-NPU-read-only-audit.patch`。
- `apply.sh`：`CONFIG_ZX_NPU_AUDIT=y` + 四条断言。

## 3. 构建见证

```
CONFIG_ZX_NPU=y
CONFIG_ZX_NPU_AUDIT=y
# CONFIG_ZX_FFE is not set
```

System.map：

```
c0605c48 t zx_npu_remove
c0605c74 t zx_npu_probe
c0605d08 t zx_npu_status_show
c0605d84 t zx_npu_audit_remove
c0605d8c t zx_npu_audit_probe
```

DTB：`npu-scaffold` + `npu-audit` 节点都在；无 `zx_idm_*`/`zx_wifi_flow_offload`。

## 4. 上机使用方法（尚未执行）

刷入后：

```sh
cat /proc/zx-npu-audit/audit          # 初始为 "not run"（或空）
echo run > /proc/zx-npu-audit/audit   # 显式触发一次只读
cat /proc/zx-npu-audit/audit          # 读回 P1 寄存器
```

如果某次读取导致挂死，只能断电；这是 P1 审计的已知风险，所以默认不读、
必须显式写 `run`。

## 5. 下一步

- 用 audit 输出核对 P1：IDM `+0x08` RX / `+0x04` TX、`+0x40/+0x44`、
  `+0x48/+0x4c`、BMU `0x0104C040` 等。
- 写 `zxnpu-idm` 环形引擎（基于 ffe-full 真机 6018 帧的 RX 逻辑，
  改正 TX/RX 基址后重写），作为 A/W 数据面的 fabric 层。
- 之后接 PPE/FFE 与 MT7916 host driver。
