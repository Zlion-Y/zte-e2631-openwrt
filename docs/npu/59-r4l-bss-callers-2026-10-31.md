# 59 — R4l 补全：AsicBssInfoUpdate 全调用方映射（BSS bring-up 入口）（2026-10-31）

`AsicBssInfoUpdate`(0x140198，arch_ops 分发) 的 **11 个调用方**全部定位
——BSS bring-up 的触发点集合：

| 调用方 | 触发时机 |
|---|---|
| `net_ad_set_bss_info`(0x288658) | **BSS bring-up 主入口**（HcGetOmacIdx + 零 mem + AsicBssInfoUpdate）——P5 的 wlanmgr 对位 |
| `MT_ATEBssInfoUpdate`(0x2A7E80) | ATE 测试模式（固件测试帧） |
| `MT_ATEDevInfoUpdate`(0x2A7C90) | ATE 设备信息 |
| `Set_BeaconPeriod_Proc`(0x23D24) | Beacon 周期 ioctl |
| `Set_DtimPeriod_Proc`(0x23F28) | DTIM 周期 ioctl |
| `hw_ctrl_flow_v1_link_down`(0xAACB4) | 链路 down |
| `0xAAE10 / 0xAB364 / 0xAB4D4` | hw_ctrl_flow_v1 系（up/restart 族） |
| `MtUpdateBcnToMcuV2`(0x1BAF80) | **Beacon 更新**（memcpy + AsicBssInfoUpdate + 帧内容） |
| `Set_EntryLifeCheck_Proc`(0x2CAC8) | 条目生命周期 ioctl |

**P5 对位**：`wlanmgr bss-set` → `net_ad_set_bss_info` 等价物 =
BSS bring-up 主入口；`MtUpdateBcnToMcuV2` 的 memcpy + AsicBssInfoUpdate
链 = beacon/CSA 编程路径（P5）。

**离线解码至此覆盖 BSS bring-up 全部触发点。** 剩余 = beacon 帧
内容构造（P5，需 wcid/BSS 状态）+ 真机验证。
