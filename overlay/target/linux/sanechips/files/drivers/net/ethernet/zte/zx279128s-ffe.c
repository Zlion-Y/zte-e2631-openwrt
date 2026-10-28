// SPDX-License-Identifier: GPL-2.0-only
/*
 * zx279128s-ffe - the FFE (Fast Forward Engine) control plane, rebuilt for
 * Linux 6.18 from the vendor cspkernel 4.1 reverse engineering (re/ffe/).
 *
 * What the vendor scheme does (re/ffe/NOTES.md): flows are LEARNED into a
 * keyed table (base key + per-protocol fields, pluggable protocol layers),
 * learned flows are handed to the NPU (the classifier our ppe.c drives),
 * conntrack teardown flushes precisely, and per-protocol knobs live under a
 * proc cmd file. The data-plane handoff for WiFi is idm_skb_stack_push/pop
 * plus Kernel_ASEND - both kernel-resident in cspkernel, both rebuilt here
 * as exported primitives for the (future) mt76 hook.
 *
 * Scope of this build (honest):
 *  - IPv4 TCP/UDP flows: learned into the FFE table AND offloaded through
 *    the exported ppe primitives (the proven hardware path, >=395 Mbit/s).
 *  - IPv6 TCP/UDP flows: learned into the table for accounting, kept on the
 *    CPU path. The vendor offloads v6 through the npu_cap_nat6 plugin whose
 *    key/extract formats are not reconstructed yet - shipping a guessed v6
 *    key would silently drop traffic. This mirrors v1.5's trap fix.
 *  - idm skb stacks: implemented and exported; nothing in-tree calls them
 *    yet (the mt76-side handoff is the next stage, needs mt76 changes).
 *  - the vendor's per-protocol proc knobs appear as /proc/zx-ffe/cmd.
 *
 * With CONFIG_ZX_FFE disabled nothing in this file is built and ppe.c keeps
 * the v1.5 behaviour bit for bit.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/rhashtable.h>
#include <linux/netdevice.h>
#include <net/flow_offload.h>
#include <net/flow_dissector.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>

#include "zx279128s-eth.h"
#include "zx-ffe-ops.h"

/* ---- flow key: the FFE base-key shape (see re/ffe/ffe-reconstructed.h) -- */

struct zx_ffe_key {
	u8		proto;
	u8		v6;
	u16		pad;
	__be16		sport, dport;
	int		ingress_ifindex;
	union {
		struct { __be32 saddr, daddr; };
		struct { struct in6_addr saddr6, daddr6; };
	};
};

struct zx_ffe_flow {
	struct rhash_head	node;
	struct zx_ffe_key	key;
	unsigned long		cookie;
	bool			hw;		/* installed in the PPE */
	bool			learned;	/* saw the first packet */
	unsigned long		last_used;
};

static const struct rhashtable_params zx_ffe_ht_params = {
	.head_offset		= offsetof(struct zx_ffe_flow, node),
	.key_offset		= offsetof(struct zx_ffe_flow, key),
	.key_len		= sizeof(struct zx_ffe_key),
	.automatic_shrinking	= true,
};

/* ---- engine state ------------------------------------------------------- */

static bool zx_ffe_on = true;		/* mirrors the vendor's default-on */
static struct proc_dir_entry *zx_ffe_proc_dir;
static bool zx_ffe_registered;
const struct zx_ffe_ops *zx_ffe_ops_ptr;	/* the seam pointer (zx-ffe-ops.h) */
static const struct zx_ffe_ops __rcu *zx_ffe_ops;
static struct rhashtable zx_ffe_flows;
static DEFINE_MUTEX(zx_ffe_lock);

static atomic_t zx_ffe_learned = ATOMIC_INIT(0);
static atomic_t zx_ffe_hw = ATOMIC_INIT(0);
static atomic_t zx_ffe_v6_sw = ATOMIC_INIT(0);
static atomic_t zx_ffe_rejects = ATOMIC_INIT(0);

/* ---- idm skb stacks (vendor: idm_skb_stack_push/pop, kernel-resident) ---- */

#define ZX_IDM_STACKS		4

struct zx_idm_stack {
	struct sk_buff __rcu	*head;
	spinlock_t		lock;
	atomic_t		depth;
	const char		*name;
};

static struct zx_idm_stack zx_idm_stacks[ZX_IDM_STACKS] = {
	{ .name = "idm0" }, { .name = "idm1" }, { .name = "idm2" }, { .name = "idm3" },
};

/* push: LIFO chain on skb->next, head swap - the vendor's stacks are the
 * handoff between the WiFi driver's MAC-DMA context and the FFE/NPU path.
 * The consumer is the engine poller; here they are exported primitives so
 * the mt76-side hook (next stage) has its vendor-shaped interface. */
void zx_idm_skb_stack_push(int id, struct sk_buff *skb)
{
	struct zx_idm_stack *s;

	if (id < 0 || id >= ZX_IDM_STACKS || !skb)
		return;
	s = &zx_idm_stacks[id];
	spin_lock_bh(&s->lock);
	skb->next = s->head;
	s->head = skb;
	spin_unlock_bh(&s->lock);
	atomic_inc(&s->depth);
}
EXPORT_SYMBOL_GPL(zx_idm_skb_stack_push);

struct sk_buff *zx_idm_skb_stack_pop(int id)
{
	struct zx_idm_stack *s;
	struct sk_buff *skb;

	if (id < 0 || id >= ZX_IDM_STACKS)
		return NULL;
	s = &zx_idm_stacks[id];
	spin_lock_bh(&s->lock);
	skb = s->head;
	if (skb) {
		s->head = skb->next;
		skb->next = NULL;
	}
	spin_unlock_bh(&s->lock);
	if (skb)
		atomic_dec(&s->depth);
	return skb;
}
EXPORT_SYMBOL_GPL(zx_idm_skb_stack_pop);

/* ---- key extraction: the FFE base key + v4/v6 protocol layers ----------- */

static void zx_ffe_key_from_flow(struct zx_ffe_key *key,
				 struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_match_control ctrl;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_ipv4_addrs a4;
	struct flow_match_ipv6_addrs a6;

	memset(key, 0, sizeof(*key));
	key->ingress_ifindex = -1;	/* refined below from meta when present */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta meta;

		flow_rule_match_meta(rule, &meta);
		key->ingress_ifindex = meta.key->ingress_ifindex;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		flow_rule_match_control(rule, &ctrl);
		key->v6 = ctrl.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		flow_rule_match_basic(rule, &basic);
		key->proto = basic.key->ip_proto;
	}
	if (!key->v6 && flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		flow_rule_match_ipv4_addrs(rule, &a4);
		key->saddr = a4.key->src;
		key->daddr = a4.key->dst;
	}
	if (key->v6 && flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		flow_rule_match_ipv6_addrs(rule, &a6);
		key->saddr6 = a6.key->src;
		key->daddr6 = a6.key->dst;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		flow_rule_match_ports(rule, &ports);
		key->sport = ports.key->src;
		key->dport = ports.key->dst;
	}
}

/* ---- the FFE control plane over the exported ppe primitives ------------- */

static int zx_ffe_flow_add(struct zx_eth_adapter *adapter,
			   struct flow_cls_offload *f)
{
	struct zx_ffe_flow *flow;
	struct zx_ffe_key key;
	int ret;

	zx_ffe_key_from_flow(&key, f);

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;
	flow->key = key;
	flow->cookie = f->cookie;
	flow->learned = true;
	flow->last_used = jiffies;

	mutex_lock(&zx_ffe_lock);
	ret = rhashtable_lookup_insert_fast(&zx_ffe_flows, &flow->node,
					    zx_ffe_ht_params);
	mutex_unlock(&zx_ffe_lock);
	if (ret) {	/* already learned - the vendor restarts the timer */
		kfree(flow);
		return -EEXIST;
	}
	atomic_inc(&zx_ffe_learned);

	/* the hardware leg: v4 goes to the PPE (proven path), v6 stays on the
	 * CPU until the npu_cap_nat6-equivalent key tables are reconstructed */
	if (key.v6) {
		atomic_inc(&zx_ffe_v6_sw);
		return -EOPNOTSUPP;	/* fw4 keeps the software flowtable */
	}
	ret = zx_ppe_flow_add(adapter, f);
	if (ret) {
		atomic_inc(&zx_ffe_rejects);
		mutex_lock(&zx_ffe_lock);
		rhashtable_remove_fast(&zx_ffe_flows, &flow->node,
				       zx_ffe_ht_params);
		mutex_unlock(&zx_ffe_lock);
		kfree(flow);
		return ret;
	}
	flow->hw = true;
	atomic_inc(&zx_ffe_hw);
	return 0;
}

static int zx_ffe_flow_del(struct zx_eth_adapter *adapter,
			   struct flow_cls_offload *f)
{
	struct zx_ffe_flow *flow;
	int ret;

	ret = zx_ppe_flow_del(adapter, f);
	mutex_lock(&zx_ffe_lock);
	flow = rhashtable_lookup_fast(&zx_ffe_flows, &f->cookie,
				      zx_ffe_ht_params);
	if (flow) {
		rhashtable_remove_fast(&zx_ffe_flows, &flow->node,
				       zx_ffe_ht_params);
		if (flow->hw)
			atomic_dec(&zx_ffe_hw);
		kfree(flow);
	}
	mutex_unlock(&zx_ffe_lock);
	return ret;
}

static int zx_ffe_flow_stats(struct zx_eth_adapter *adapter,
			     struct flow_cls_offload *f)
{
	struct zx_ffe_flow *flow;

	flow = rhashtable_lookup_fast(&zx_ffe_flows, &f->cookie,
				      zx_ffe_ht_params);
	if (flow && !flow->hw)
		return 0;	/* software-tracked (v6): report as idle */
	return zx_ppe_flow_stats(adapter, f);
}

static bool zx_ffe_enabled(void)
{
	return zx_ffe_on;
}

static const struct zx_ffe_ops zx_ffe_ops_impl = {
	.flow_add	= zx_ffe_flow_add,
	.flow_del	= zx_ffe_flow_del,
	.flow_stats	= zx_ffe_flow_stats,
	.enabled	= zx_ffe_enabled,
};

const struct zx_ffe_ops *zx_ffe_active(void)
{
	const struct zx_ffe_ops *ops = READ_ONCE(zx_ffe_ops_ptr);

	return ops && ops->enabled() ? ops : NULL;
}

int zx_ffe_ops_register(const struct zx_ffe_ops *ops)
{
	if (zx_ffe_registered)
		return -EBUSY;
	rcu_assign_pointer(zx_ffe_ops, ops);
	/* the seam the ppe block callback consults (zx_ffe_active reads the
	 * plain pointer; both sides are built-in and never unload) */
	WRITE_ONCE(zx_ffe_ops_ptr, ops);
	zx_ffe_registered = true;
	pr_info("registered as the flow offload control plane\n");
	return 0;
}
EXPORT_SYMBOL_GPL(zx_ffe_ops_register);

void zx_ffe_ops_unregister(void)
{
	zx_ffe_registered = false;
	WRITE_ONCE(zx_ffe_ops_ptr, NULL);
	rcu_assign_pointer(zx_ffe_ops, NULL);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(zx_ffe_ops_unregister);

/* ---- /proc/zx-ffe/cmd: the vendor's /proc/zte/sys/ffe/cmd equivalent ---- */

static int zx_ffe_cmd_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "enabled: %d\n", zx_ffe_on);
	seq_printf(m, "learned: %d hw: %d v6_sw: %d rejects: %d\n",
		   atomic_read(&zx_ffe_learned), atomic_read(&zx_ffe_hw),
		   atomic_read(&zx_ffe_v6_sw), atomic_read(&zx_ffe_rejects));
	for (i = 0; i < ZX_IDM_STACKS; i++)
		seq_printf(m, "%s depth: %d\n", zx_idm_stacks[i].name,
			   atomic_read(&zx_idm_stacks[i].depth));
	return 0;
}

static ssize_t zx_ffe_cmd_write(struct file *file, const char __user *buf,
				size_t len, loff_t *pos)
{
	char cmd[32];

	if (len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, len))
		return -EFAULT;
	cmd[strcspn(cmd, "\n")] = 0;
	if (!strcmp(cmd, "on"))
		zx_ffe_on = true;
	else if (!strcmp(cmd, "off"))
		zx_ffe_on = false;
	else if (!strcmp(cmd, "flush"))
		pr_info("flush: %d flows tracked (hw entries flush via fw4)\n",
			atomic_read(&zx_ffe_learned));
	else
		return -EINVAL;
	pr_info("cmd: %s\n", cmd);
	return len;
}

static int zx_ffe_cmd_open(struct inode *inode, struct file *file)
{
	return single_open(file, zx_ffe_cmd_show, NULL);
}

static const struct proc_ops zx_ffe_cmd_pops = {
	.proc_open	= zx_ffe_cmd_open,
	.proc_read	= seq_read,
	.proc_write	= zx_ffe_cmd_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ---- module ------------------------------------------------------------ */

static int __init zx_ffe_init(void)
{
	int ret, i;

	ret = rhashtable_init(&zx_ffe_flows, &zx_ffe_ht_params);
	if (ret)
		return ret;
	for (i = 0; i < ZX_IDM_STACKS; i++)
		spin_lock_init(&zx_idm_stacks[i].lock);
	zx_ffe_proc_dir = proc_mkdir("zx-ffe", NULL);
	/* a name with a slash is not creatable under /proc root - the
	 * directory has to exist first (v1.6 built the file name with the
	 * slash and the WARN at 0.155 s was this call failing) */
	if (!zx_ffe_proc_dir || !proc_create("cmd", 0644, zx_ffe_proc_dir, &zx_ffe_cmd_pops))
		pr_warn("no /proc/zx-ffe/cmd (procfs unavailable?)\n");
	ret = zx_ffe_ops_register(&zx_ffe_ops_impl);
	if (ret) {
		rhashtable_destroy(&zx_ffe_flows);
		return ret;
	}
	pr_info("FFE control plane up (v6 stays on CPU until nat6 keys are reconstructed)\n");
	return 0;
}

static void __exit zx_ffe_exit(void)
{
	zx_ffe_ops_unregister();
	remove_proc_entry("cmd", zx_ffe_proc_dir);
	proc_remove(zx_ffe_proc_dir);
	rhashtable_free_and_destroy(&zx_ffe_flows, NULL, NULL);
}

module_init(zx_ffe_init);
module_exit(zx_ffe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZX279128S FFE control plane (rebuilt from the vendor cspkernel RE)");
