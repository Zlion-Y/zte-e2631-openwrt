/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * re/ffe/ffe-reconstructed.h - data structures reconstructed from the
 * vendor cspkernel 4.1.25 binary (vmlinux-csp.elf, kallsyms base c0008000).
 *
 * NOT vendor source code: every field below is annotated with the
 * disassembly evidence it was reconstructed from. Companion notes:
 * re/ffe/NOTES.md. Disassembly: re/ffe/disasm/*.S (ARM cross objdump).
 */
#ifndef _RE_FFE_RECONSTRUCTED_H
#define _RE_FFE_RECONSTRUCTED_H

/*
 * ---- flow key ------------------------------------------------------------
 * ffe_calc_key_len (c04119f4): base length 16, then one stride-64 entry per
 * registered protocol adds its extra key length; entry field at -0x18 is the
 * per-protocol key length. Max 16 protocol layers (loop cap).
 * ffe_learn_pkt (c0410958): a 16-byte signature word-group ([0]..[12]) is
 * OR-composed from per-packet material before slot lookup.
 */
struct ffe_key_proto_desc {		/* 64-byte stride, indexed by proto id */
	/* +0x00 */ const char		*name;
	/* +0x08 */ int			(*parse)(void *pkt, void *key);	/* [r0+8] called via blx */
	/* +0x18 */ u32			key_len;	/* read at [desc - 0x18] */
	/* +0x28 */ u32			flags;
	/* +0x40 */ int			(*learn)(void *flow, void *pkt, void *slot); /* [r6+0x40] tail call */
};

#define FFE_KEY_BASE_LEN	16		/* ffe_calc_key_len base */
#define FFE_KEY_MAX_PROTOS	16		/* loop cap, cmp r1,#16 */
#define FFE_PROTO_MAX_ID	15		/* ffe_learn_pkt: cmp r0,#15 */

/*
 * ---- flow / slots --------------------------------------------------------
 * ffe_learn_pkt: flow slots live in a stride-128 table (add r3,r3,r0,lsl #7);
 * slot-identity compare at [slot + 0x34]. The learned signature is a
 * 4x32-bit word group copied into the slot (ffe_copy_flow_key).
 */
struct ffe_flow_slot {
	/* +0x00 */ u32	sig[4];			/* OR-composed key signature */
	/* +0x20 */ void		*conntrack;	/* flush hooks (ffe_flush_flow_by_conntrack) */
	/* +0x34 */ u64		slot_id;		/* identity compare at +0x34 */
	/* +0x80+ */ u8		proto_state[48];/* per-protocol learn results */
};

/*
 * ---- NPU driver contract -------------------------------------------------
 * npu_register_driver (c041b4f0): validates ops, links a driver object into a
 * list (list_add pattern at +0x08/+0x1c), name/ops at +0x00/+0x04/+0x0c,
 * capability mask around +0x28/+0x3c. npu_cap_setup (c041c4ac) attaches
 * capability instances - the tree shows caps as pluggable offload plugins:
 *   npu_cap_nat6_*   IPv6 NAT offload
 *   npu_cap_pps_*    per-packet-statistics offload
 *   npu_mc_*         multicast offload (hwactive flow query/delete cbs)
 *   npu_flow_retry_* retry path
 * Each cap has frontend_create/backend_create/match hooks.
 */

/*
 * ---- idm skb stack (wireless handoff) ------------------------------------
 * imported by mt7916.ko on both E2631 and F670L:
 *   idm_skb_stack_push / _idm_skb_stack_push / idm_skb_stack_pop
 * Lock-free LIFO handoff of skbs between the WiFi driver (MAC DMA context)
 * and the FFE/NPU fast path. Definition is kernel-resident (no module
 * defines it); semantics reconstructed: push links skb->next-style chain
 * under an xchg head, pop swaps the head out.
 */

/*
 * ---- Kernel_ASEND (accelerated transmit) ---------------------------------
 * Kernel_ASEND (c03e5cf0, 512B): builds a tx-message chain ("Tmsgcn" string)
 * - a list of descriptors on the stack frame (str lr,[sp], str rN,[sp,#n]...)
 * - then hands the chain to the egress DMA/NPU path. This is the vendor
 * replacement for dev_queue_xmit on the accelerated path.
 */

/*
 * ---- the engine's own proc knob -----------------------------------------
 * /proc/zte/sys/ffe/cmd (ffe_proc_cmd_write): per-protocol enable knobs,
 *   ffe_proto_{npu,hash}_proc_opts - NPU offload and hash behaviour per proto.
 */

#endif
