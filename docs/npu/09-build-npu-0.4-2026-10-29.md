# npu-0.4 构建记录：PPE/CLA 低层访问（2026-10-29）

> 这是 npu 分支的第一个**有线侧**数据面基础组件：CLA 间接表访问。
> 它不是完整流表；默认关闭，读/写表项都必须显式 enable。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/e2631-openwrt-npu-0.4.bin` |
| 大小 | 20,972,052 B |
| sha256 | `7c0500a0c1de60fc1cb46e977c651382dfc630fd4ad0f802debce724448f238d` |
| 版本 | `OpenWrt npu-0.4` |
| verify_pkg | ALL CHECKS PASSED |
| verify_img | BOOT-READY |

未刷机。

## 2. 新增

`zx279128s-npu-ppe.c`（`CONFIG_ZX_NPU_PPE=y`）：

- 平台驱动 `zte,zx279128s-npu-ppe`，映射 PP `0x92380000`（0x40000）。
- `/proc/zx-npu-ppe/cla`：
  - `dump`：只读 CLA config `c080/c088/c090/c0c8/c0cc/c0d0/done`。
  - `read <table> <index>`：CLA 间接读（命令 `index | table<<22 | BIT(27)`，
    done=`PP+0xC018` bit0，data=`PP+0xC01C`）。
  - `write <table> <index> <value>`：CLA 间接写（清 bit27）；**实验性**，
    数据/命令顺序待真机确认。
- 默认 `zxnpu_ppe.enable=0`；`read`/`write` 返回 `-EACCES`，只有 `dump` 可读。

## 3. 见证

```
CONFIG_ZX_NPU=y
CONFIG_ZX_NPU_AUDIT=y
CONFIG_ZX_NPU_IDM=y
CONFIG_ZX_NPU_PPE=y
```

System.map：`zxnpu_ppe_probe/remove`；DTB：`zte,zx279128s-npu-ppe`。

## 4. 边界

- 只有低层表访问，没有 45B key / CRC16 / hash / rule / PM 重写的流表构建。
- `write` 的顺序/位宽未真机验证；不要默认启用。
- 有线数据面仍是 v1.8.0 PPE。

## 5. 下一步

- 用 `dump`/`read` 在真机核对 CLA 表基址、table 选择、index 宽度。
- 把 vendor `tm_acl_get_fastHashRule`/`cla_set_config` 的键/规则字段接进
  `zxnpu-ppe`，形成最小 vendor-native flow add/delete。
- W 侧并行：MT7916 G3 的 PCIe/固件下载骨架。
