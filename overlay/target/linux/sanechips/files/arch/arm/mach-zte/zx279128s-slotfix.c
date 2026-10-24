// SPDX-License-Identifier: GPL-2.0-only
/*
 * zx279128s-slotfix: make the rootfs partitions follow the slot cspboot booted
 *
 * cspboot never assumes which of the two 28 MiB image slots holds the system:
 * on every boot it computes this slot's kernel and fs flash offsets and
 * stashes a "bootpara" block (0x554 bytes, magic cccccccc / 55555555 /
 * aaaaaaaa / 11111111 somewhere inside) at physical 0x40007000 before jumping
 * to the kernel.  The vendor kernel builds its partition table from that
 * block.  Our DTS instead hard-wires the slot-1 window (rootfs@0x27000000's
 * partition sits at 0x02700000), so the same image written to slot 0 panics
 * at root mount and reboot-loops - first seen 2026-10-24, when the stock
 * upgrader picked slot 0 after serial churn from repeated flashes (see
 * known-issues ROOTFS-SLOT-ASSUMPTION).
 *
 * This fixup runs as an early_initcall, before any device probe: it maps the
 * block, validates the magic, and scans it for a u32 pair (K, F) with
 * F - K == 0x400000, where K is one of the two slot bases.  That delta is a
 * signature only our own packer produces (the kernel extent is padded to
 * 0x3fffe0, so the fs starts exactly slot+0x400000; every vendor image
 * carries 0x33fdcc which rounds to 0x340000).  On a match it rewrites the reg
 * values of the "rootfs" and "slot1-header" partitions to the slot that is
 * actually booting.  No struct field offsets are needed: the kernel start and
 * fs start appear as plain u32s in the block, in both slots' entries
 * (measured states: 0x700000/0xb00000 ours-in-slot-0, 0x700000/0xa40000
 * vendor-in-slot-0, 0x2300000/0x2640000 vendor-in-slot-1).
 *
 * Every failure path is "change nothing": a missing block, a magic mismatch
 * or no matching pair leaves the device tree exactly as built - the slot-1
 * layout every image up to v1.3 booted from.
 *
 * Userspace note: e2631-fwslot still assumes "our" image is the one in
 * slot 1; keep flashing into slot 1 before running "fwslot switch" until it
 * learns to detect the active slot itself.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/string.h>

#define ZX_BP_PHYS		0x40007000UL
#define ZX_BP_SIZE		0x554
#define ZX_HDR_OFF		0x01000000U	/* header sits at fs + 16 MiB */
#define ZX_OUR_DELTA		0x00400000U	/* fs - kernel, our packer only */

static const u32 zx_slot_bases[] = { 0x00700000U, 0x02300000U };

static bool blob_has(const u32 *b, unsigned n, u32 v)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (b[i] == v)
			return true;
	return false;
}

/* returns the fs offset of the slot holding OUR layout, 0 when not found */
static u32 zx_slotfix_scan(const u32 *b, unsigned n)
{
	const u32 magic[4] = { 0xcccccccc, 0x55555555, 0xaaaaaaaa, 0x11111111 };
	unsigned i, j, hits;
	u32 fs[2];

	/* the magic quadruple lives somewhere in the block */
	for (i = 0; i + 4 <= n; i++)
		if (b[i] == magic[0] && b[i + 1] == magic[1] &&
		    b[i + 2] == magic[2] && b[i + 3] == magic[3])
			break;
	if (i + 4 > n)
		return 0;

	hits = 0;
	for (j = 0; j < ARRAY_SIZE(zx_slot_bases); j++) {
		u32 k = zx_slot_bases[j];
		u32 f = k + ZX_OUR_DELTA;

		if (blob_has(b, n, k) && blob_has(b, n, f)) {
			if (hits < ARRAY_SIZE(fs))
				fs[hits] = f;
			hits++;
		}
	}
	if (!hits)
		return 0;
	if (hits > 1)
		pr_warn("zx-slotfix: BOTH slots hold our layout; defaulting to the slot-1 window\n");
	/* scan order is slot0, slot1: prefer the slot-1 window, the DT default */
	return fs[hits - 1];
}

/* rewrite the first cell of a reg property in the live tree (big endian) */
static int zx_fix_reg_offset(struct device_node *np, u32 new_off)
{
	struct property *pp;
	int len;

	if (!np)
		return -ENOENT;
	pp = of_find_property(np, "reg", &len);
	if (!pp || len != 8 || !pp->value)
		return -ENOENT;
	*((u32 *)pp->value) = cpu_to_be32(new_off);
	return 0;
}

static struct device_node *partition_by_label(const char *label)
{
	struct device_node *np;

	for_each_of_allnodes(np) {
		const char *s = NULL;

		if (!of_node_name_prefix(np, "partition"))
			continue;
		if (of_property_read_string(np, "label", &s) == 0 &&
		    s && !strcmp(s, label))
			return np;
	}
	return NULL;
}

static int __init zx_slotfix_init(void)
{
	const void *p;
	u32 fs;
	int ret;

	p = memremap(ZX_BP_PHYS, ZX_BP_SIZE, MEMREMAP_WB);
	if (!p) {
		pr_info("zx-slotfix: bootpara block unmappable, keeping the built-in slot-1 layout\n");
		return 0;
	}
	fs = zx_slotfix_scan(p, ZX_BP_SIZE / 4);
	memunmap((void __iomem *)p);

	if (!fs) {
		pr_info("zx-slotfix: no bootpara signature, keeping the built-in slot-1 layout\n");
		return 0;
	}
	pr_info("zx-slotfix: this boot's image lives at fs offset 0x%08x\n", fs);

	ret = zx_fix_reg_offset(partition_by_label("rootfs"), fs);
	if (ret)
		pr_warn("zx-slotfix: rootfs partition not patchable (%d)\n", ret);
	ret = zx_fix_reg_offset(partition_by_label("slot1-header"),
				fs + ZX_HDR_OFF);
	if (ret)
		pr_warn("zx-slotfix: slot1-header partition not patchable (%d)\n", ret);
	return 0;
}
early_initcall(zx_slotfix_init);
