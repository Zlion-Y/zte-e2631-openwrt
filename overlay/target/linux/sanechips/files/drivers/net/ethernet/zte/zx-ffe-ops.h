/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zx-ffe-ops.h - the seam between the PPE hardware layer (zx279128s-ppe.c)
 * and the FFE control plane (zx279128s-ffe.c, CONFIG_ZX_FFE).
 *
 * The vendor's cspkernel 4.1 carries the FFE (Fast Forward Engine) as the
 * control plane over the same classifier hardware; this board's port keeps
 * the hardware layer in zx279128s-ppe.c and lets the FFE module own the
 * flow learn/delete/stats decisions when enabled. With CONFIG_ZX_FFE=n the
 * FFE object is not built and ppe.c behaves exactly as v1.5.
 *
 * Both sides are built-in and never unload: the ops pointer is a plain
 * global, declarations are unconditional, and the #ifdef only gates whether
 * ppe.c consults the seam.
 */
#ifndef _ZX_FFE_OPS_H
#define _ZX_FFE_OPS_H

#include <net/flow_offload.h>

struct zx_eth_adapter;

struct zx_ffe_ops {
	int (*flow_add)(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);
	int (*flow_del)(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);
	int (*flow_stats)(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);
	bool (*enabled)(void);
};

/* the hardware primitives ppe.c owns; the FFE control plane calls them */
int zx_ppe_flow_add(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);
int zx_ppe_flow_del(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);
int zx_ppe_flow_stats(struct zx_eth_adapter *adapter, struct flow_cls_offload *f);

/* plain pointer on purpose: built-in on both sides, never unloads */
extern const struct zx_ffe_ops *zx_ffe_ops_ptr;
const struct zx_ffe_ops *zx_ffe_active(void);
int zx_ffe_ops_register(const struct zx_ffe_ops *ops);
void zx_ffe_ops_unregister(void);

/* the vendor's idm_skb_stack_push/pop - WiFi<->FFE skb handoff primitives */
void zx_idm_skb_stack_push(int id, struct sk_buff *skb);
struct sk_buff *zx_idm_skb_stack_pop(int id);

#endif
