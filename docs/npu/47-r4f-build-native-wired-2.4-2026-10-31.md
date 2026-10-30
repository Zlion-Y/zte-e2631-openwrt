# 47 — R4f 构建记录：native-wired-2.4（TMAC→TXD 映射闭环）（2026-10-31）

## 1. 产物

| 项 | 值 |
|---|---|
| 文件 | `out/native-wired-2.4/e2631-openwrt-native-wired-2.4.bin` |
| sha256 | `aac085610e06f5dc92edc3ad7c67360d667e97c31be7152c51d0f6234d604752` |
| 版本 | `OpenWrt native-wired-2.4` |
| verify_pkg / verify_img | ALL CHECKS PASSED (1 warning) / BOOT-READY |

未刷机。

## 2. R4f 交付（doc 45 §6：TMAC→TXD 映射闭环）

- dw1 bit13（#8192）= r7+465 标志（1b339c）；
- **r4+24 速率字** = `mtf_tx_rate_to_tmi_rate`(1b3340) 返回值 lsl16
  ——TMI 速率编码（connac2 TXD rate 同位域）；
- r4+28 = 模板 +30→lsl11、+34→lsl16、+33→lsl20；
- 定速率特殊路径内部：+43→bit5、r8+2949→lsl24、#20480/#4；
- skb 衔接确认：r4 = skb push 后 TMAC 区，环描述符引用 skb 数据，
  kick = CPU_IDX 写 +8。
- data-tx 打印更新（R4f 位图入固件）。

**结论：52B TMAC 模板 → TXD 的全部位路径可离线追溯。** 构造真实帧
剩余依赖 = P5 AP 状态（wcid/加密密钥/速率选择）。

## 3. 下一步

- R5 真机验收（L2→L4，需用户解除禁刷）；
- R4d 尾（P5 内）：wcid/加密字段取值 + NAPI 接线 + VAP。
