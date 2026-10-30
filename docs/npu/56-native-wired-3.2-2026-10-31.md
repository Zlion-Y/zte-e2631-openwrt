# 56 — native-wired-3.2：wcid 分配器接入 ctl（sta-add/del/list）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-3.2/e2631-openwrt-native-wired-3.2.bin` |
| sha256 | `c5714d5d723b90dd3803ffd0a648f7e3e3e7b577c5f196c477eb2e6a1c5fb6db` |
| 版本 | `OpenWrt native-wired-3.2` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. 交付（P5 关联管理入口）

- **`sta-add <mac 12hex> [band]`**：调用 zxna_wcid_alloc（R4g 布局镜像）
  分配 wcid 并标记 ctx 关联（魔数 0x00040001）；VAP 缺席时拒绝；
- **`sta-del <wcid>`**：释放（清零 ctx）；
- **`sta-list`**：遍历 ctx 镜像打印已关联 STA（wcid/band/addr/key-set）；
- 真机 P5 关联管理handler 将调用同一对 allocator——ctl 命令即
  关联管理的用户态测试入口。

## 3. 状态

ctx 镜像从死代码变为可调用接口（wlanmgr 亦可包装）。L2-L6 等用户
解除禁刷放行清单 36。
