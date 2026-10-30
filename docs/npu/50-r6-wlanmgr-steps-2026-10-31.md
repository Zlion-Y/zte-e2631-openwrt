# 50 — R6：wlanmgr commit 流验证 + 清单 36 序列脚本化（native-wired-2.7）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.7/e2631-openwrt-native-wired-2.7.bin` |
| sha256 | `ecb48907134ce3c03cb7a3b304e1e15c8e2b84e1e184c9e203b4b1d893e9ff80` |
| 版本 | `OpenWrt native-wired-2.7` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |
| 见证 | rootfs `/sbin/wlanmgr` 0755；wlanmgr 离线测试 13/13 ok |

未刷机。

## 2. R6 交付（doc 38 禁刷路径增量）

1. **wlanmgr 序列脚本化**：step1（只读面）/ step2（FWDL 链，需
   commit=1）/ step3（数据面）——清单 36 §2 的可执行编码；
2. **wlanmgr 离线测试**（tests/wlanmgr-test.sh，13 项全过）：九个只读
   子命令 → ctl 命令映射逐项核验；commit 转发核验；step2 尾命令
   （fw-state）+ 四步链静态断言；step3 尾命令（dataplane）断言；
   方法 = WLANMGR_CTL/WLANMGR_SYS 双桩（stub 控制文件 + /sys 覆盖）；
3. need_commit 门禁（step2/step3 在 commit≠1 时拒绝）——与
   zxnpu_wlan.commit 参数联动。

## 3. 真机用法（清单 36 执行时）

```sh
wlanmgr step1   # 只读面：probe/rings/fw-state
wlanmgr step2   # FWDL 链：mcu-dl patch → fw-dl wm/wa → state=7
wlanmgr step3   # 数据面：data-init/data-recv/dataplane
```

## 4. 边界

- step2/step3 在真机上写寄存器——须按清单 36 顺序（step1 通过才 step2）；
- 未刷机。
