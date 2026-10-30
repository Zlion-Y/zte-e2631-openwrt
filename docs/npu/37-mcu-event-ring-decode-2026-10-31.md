# 37 — MCU 事件环格式解码（入口已定，本轮未完成）（2026-10-31）

清单 36"尚未覆盖"第一项：MCU 事件环格式与 ack 匹配。本轮只钉了入口路径，
完整解码留给下一轮（预计 1-2 小时反汇编工作量）。

## 1. 入口链（已核实）

```
RX 环（qid0..5 之一）收到 MCU 事件帧
→ AndesCmdMsgBh 出队循环（0x1B9700）对每条消息调
  hif_rx_event_process(priv, msg) = cb[0xB0C]（cb=*(priv+0xF0002B0)）
→ PCI 实现 = pci_rx_event_process（vtbl-full.map r0[0x68]，pci_core_ops_register 填）
→ 事件头解析 + Ack 匹配（SemGet/AddressLen/FwStart 的等待队列）
```

注意：`hif_rx_event_process` 名字带 rx 但**发送+收事件合一**——
AndesSendCmdMsg 入队的命令也是在这里真正发出的（doc 14 §6.5.1）。

## 2. 事件头已知线索

- Andes 命令头 24B 的接收侧镜像：len/type/seq/cmd（doc 14 §2）；
- 1.5 的 `rings` 命令已能打印 RX 环原始内容（清单 §1），真机拿到第一帧
  事件后即可与本解码对字段；
- mt76 connac2 的事件头（mt76_connac2_mcu_rxd）结构与 vendor 同源，可当
  字段命名参考，但**逐字段以 vendor 反汇编为准**（核验纪律）。

## 3. 下轮任务清单

1. 反汇编 `pci_rx_event_process`：事件头字段偏移（type/seq/len/self?）；
2. SemGet/AddressLenReq/FwStartReq 三命令的 ack 匹配逻辑（等待队列
   挂钩点 = AndesQueueHeadCmdMsg / completion）；
3. 把格式写进 `zxnpu_wlan_ack_expect()`（只记录期望，不阻塞——保持
   清单 §1 的只读纪律），1.6 构建带上；
4. 真机验收（清单 36 第 2 步）通过后，才允许动 `zxnative_eth` offload
   路径（Qoder 划的线，遵守）。

## 4. 本轮已完成（不重复做）

- zwzzrl 最新 15 commits 评估：v6 结论强化（原厂 v6 L3 纯 CPU 143M 上限、
  NPU 表项装得上但 hit 恒 0）、零拷贝/GRO 不兼容、createfail 根因——
  已采纳入 build 分支 doc 16 §7（含 tm.ko 逐指令核验）；
- sbrg 采纳的 tm.ko 双源核验（build/npu 1939177）；
- scripts/check.sh 全绿复跑确认（WSL 侧七例 + node 语法门）。
