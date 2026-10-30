# native-wired-1.2/1.3：trap-DMAC 表与 shutdown 钩子（2026-10-30）

> 把基线驱动 `zx_update_mac_tables()` 的那一层移植进 P2 驱动：
> 每个端口 MAC 写进 SPA trap-DMAC 表（两条），ONU 路由 MAC 表清空
> （本驱动没有 flow offload），保证“发往本机的帧无论以太网类型都到
> CPU”。同时补上 `ppe_lock` 的 mutex 初始化（表读写要加锁）。

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-1.2/e2631-openwrt-native-wired-1.2.bin` |
| 大小 | 20,972,052 B |
| sha256 | `210bccf4b85ff4615c82c68013900294f453e401208f51a2ec33a14667d4e334` |
| 版本 | `OpenWrt native-wired-1.2` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 基线 | 021 移除，zxnative-eth 接管（同 native-wired-1.0） |
| 1.3 追加 | shutdown 钩子（掩中断/关 DMA/停端口/清 BMU） |
| 1.3 文件 | `out/native-wired-1.3/e2631-openwrt-native-wired-1.3.bin` |
| 1.3 sha256 | `c58318a7f5379df92471f03142f9e25cfd6316e846609d18fc134d57f216f5b7` |

未刷机。

## 2. 改动

- `zxna_mac_words` / `zxna_set_trap_dmac` / `zxna_update_mac_tables`：
  与基线相同的字序（bytes 2-5 在前、0-1 在后）；
- probe 在 netdev/MAC 就绪后、NAPI 注册前调用一次；
- `ndo_set_mac_address` 改变地址后重写表；
- `mutex_init(&a->ppe_lock)`，避免首次加锁崩。

## 3. 边界

- 没有 flow offload，所以 ONU 路由表保持全零、trap 表持有我们的 MAC；
  这与基线“PPE 关闭”时的行为一致；
- 1.3 追加 shutdown 钩子：重启/kexec 前掩中断、关 DMA、停端口、清 BMU，避免下一代内核接手仍在写的 DMA 内存；
- 未真机验证；未刷机。
