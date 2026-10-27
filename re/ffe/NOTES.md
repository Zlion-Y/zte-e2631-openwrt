# FFE 逆向笔记（ffe 分支，2026-10-28）

证据基线：`_scratch/vendor-kernel-vmlinux-4.1.25.bin`（解压）→
`vmlinux-to-elf` 重建符号（kallsyms，基址 c0008000）→ `re/ffe/symbols.txt`
（261 符号）/`ksym-all.txt`（21946 符号）；反汇编 `disasm/*.S`（ARM 交叉 objdump，
脚本 `_scratch/wsl/re-disasm.sh` 可复跑）。结构体重建：`ffe-reconstructed.h`。

## 已还原

1. **流键模型**（ffe_calc_key_len c04119f4）：基长 16B + 每注册协议追加各自
   键长（64B 描述符表，键长在 -0x18，最多 16 层）——可插拔协议层，这就是
   v4/v6/L2 全覆盖的机制。`ffe_register_proto`/`ffe_register_packet_layer` 注册。
2. **学习流程**（ffe_learn_pkt c0410958）：协议 parse 回调逐层填 16B 签名 →
   128B 步长流槽表匹配（身份字段在 +0x34）→ `ffe_copy_flow_key` → 协议 learn
   回调尾调；槽内 conntrack 指针供 `ffe_flush_flow_by_conntrack` 联动。
3. **NPU 层是插件框架**：`npu_register_driver`（驱动结构链入表：name/ops 在
   +0x00/+0x04/+0x0c，链 +0x08/+0x1c，cap 掩码 +0x28/+0x3c）；能力插件
   `npu_cap_{nat6,pps,mc}_*`（IPv6 NAT / 统计 / 组播，各带 front/backend
   create/match）；`npu_mc_*` 带硬件活跃流查询/删除回调。
4. **无线挂入**：mt7916.ko（E2631 与 F670L 同构）导入 `idm_skb_stack_push/pop`
   + `Kernel_ASEND`；数据面不经 tm.ko 导出（tm 只被导 2 个限速 API）。
5. **控制面**：`/proc/zte/sys/ffe/cmd`（ffe_proc_cmd_write），按协议
   `ffe_proto_{npu,hash}_proc_opts`。
6. **ASEND**：栈帧上构造 Tx 消息链（"Tmsgcn"）后交给出口 DMA/NPU——厂商版
   `dev_queue_xmit` 替身。

## 对移植的含义

- 硬件层（PPE 表）我方 ppe.c 已掌握且就是 vendor 的 spa_* 目标——移植成本在
  **控制面**：FFE 的学习模型、协议插件、flush 联动、proc 语义。
- `ffe-reconstructed.h` 是骨架驱动 `zx-ffe` 的结构依据：本分支把它落成 6.18
  代码（ffe 下，配置 `CONFIG_ZX_FFE`）。
- 未还原（后续迭代）：128B 槽内 proto_state 布局、npu cap 各 front/backend
  的寄存器序列、idm 栈与 NPP 环的确切耦合（需要 tm.ko 反汇编深入 +
  真机 trace）。
