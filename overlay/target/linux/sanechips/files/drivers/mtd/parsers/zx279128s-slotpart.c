// SPDX-License-Identifier: GPL-2.0-only
/*
 * zx279128s-slotpart - slot-aware MTD partition parser for the ZTE ZXHN E2631.
 *
 * The board carries two whole-system 28 MiB slots and cspboot picks the boot
 * slot by version-header serial, not by position ("the two systems can swap
 * in place").  The built-in partition table describes the slot-1 case - what
 * every image up to v1.4 booted - and a slot-0 boot panicked with
 * "VFS: Unable to mount root fs" (known-issues ROOTFS-SLOT-ASSUMPTION).
 *
 * This parser takes over the "partitions" node (compatible
 * "zte,zx279128s-slotpart"), reads BOTH slot headers through the mtd read
 * path - which the early v1.3 fixup could not: SPI NAND is not memory
 * mapped, and cspboot's bootpara block in RAM never survives the zImage
 * decompressor - and picks the boot slot the way cspboot does: valid CRC1,
 * higher serial, ties to slot 0 (see zx279128s-slotpart-core.c).  Slot 1
 * means the DT table stands; slot 0 moves exactly two partitions, "rootfs"
 * and "slot1-header", into slot 0 - the row COUNT and every label stay put,
 * so CONFIG_CMDLINE's root=/dev/mtdblockN keeps pointing at the same row.
 *
 * Any failure keeps the built-in layout: missing DT data falls back to the
 * table below, unreadable or invalid headers to the slot-1 case.  The parser
 * never panics, never retries, and never writes.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "zx279128s-slotpart-core.c"

/* The built-in (slot-1) layout, for the case the DT does not describe it.
 * Must stay row-for-row identical to the dts partitions node.
 */
static const struct {
	const char *label;
	uint32_t ofs, size;
	uint32_t mask;		/* MTD_WRITEABLE for read-only partitions */
} zx_slotpart_fallback[] = {
	{ "bootloader",		0x0000000, 0x0100000, MTD_WRITEABLE },
	{ "tag",		0x0100000, 0x0100000, MTD_WRITEABLE },
	{ "wifi",		0x0200000, 0x0100000, 0 },
	{ "usercfg",		0x0300000, 0x0200000, MTD_WRITEABLE },
	{ "defcfg",		0x0500000, 0x0200000, MTD_WRITEABLE },
	{ "slot0",		0x0700000, 0x1c00000, 0 },
	{ "slot1",		0x2300000, 0x1c00000, 0 },
	{ "rootfs",		0x2700000, 0x1000000, 0 },
	{ "slot1-header",	0x3700000, 0x020000, MTD_WRITEABLE },
	{ "userdata",		0x3f00000, 0x2f00000, 0 },
};

struct zx_slotpart_part {
	char *label;
	uint64_t ofs, size;
	uint32_t mask;
};

/* Names are kstrdup'ed into the returned array and only freed by .cleanup,
 * which the mtd core calls when the partitions go away.
 */
static void zx_slotpart_cleanup(const struct mtd_partition *pparts, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		kfree(pparts[i].name);
	kfree(pparts);
}

/* Read the fixed partitions out of the "partitions" node as the default
 * table.  Returns the count, 0 when the DT does not describe one.
 */
static int zx_slotpart_of_parts(struct device_node *np,
				struct zx_slotpart_part **out)
{
	struct device_node *child;
	struct zx_slotpart_part *parts;
	int nr = 0;

	for_each_child_of_node(np, child)
		if (of_property_present(child, "reg"))
			nr++;
	if (!nr)
		return 0;
	parts = kcalloc(nr, sizeof(*parts), GFP_KERNEL);
	if (!parts)
		return -ENOMEM;
	nr = 0;
	for_each_child_of_node(np, child) {
		struct zx_slotpart_part *p = &parts[nr];
		u32 reg[2];
		const char *label;

		if (!of_property_present(child, "reg"))
			continue;
		if (of_property_read_u32_array(child, "reg", reg, 2))
			continue;
		p->ofs = reg[0];
		p->size = reg[1];
		p->mask = of_property_read_bool(child, "read-only") ?
			  MTD_WRITEABLE : 0;
		if (!of_property_read_string(child, "label", &label)) {
			p->label = kstrdup(label, GFP_KERNEL);
			if (!p->label)
				goto err;
		}
		nr++;
	}
	*out = parts;
	return nr;
err:
	while (nr--)
		kfree(parts[nr].label);
	kfree(parts);
	return -ENOMEM;
}

static int zx_slotpart_parse(struct mtd_info *master,
			     const struct mtd_partition **pparts,
			     struct mtd_part_parser_data *data)
{
	struct zx_slotpart_part *parts = NULL;
	struct mtd_partition *out;
	struct zx_slotpart_in in = { .have_hdr = { 0, 0 } };
	struct zx_slotpart_out dec;
	struct device_node *np, *flash;
	int nr = 0, i, ret;
	uint32_t v[2];

	flash = mtd_get_of_node(master);
	np = flash ? of_get_child_by_name(flash, "partitions") : NULL;
	if (np) {
		/* slot bases and the two deltas: the dts pins them as
		 * zte,slot0/zte,slot1 = <base size> and zte,fs-delta /
		 * zte,hdr-delta; defaults are this board's geometry.
		 */
		in.fs_delta = 0x400000;
		in.hdr_delta = 0x1400000;
		if (!of_property_read_u32_array(np, "zte,slot0", v, 2))
			in.base[0] = v[0];
		if (!of_property_read_u32_array(np, "zte,slot1", v, 2))
			in.base[1] = v[0];
		of_property_read_u32(np, "zte,fs-delta", &in.fs_delta);
		of_property_read_u32(np, "zte,hdr-delta", &in.hdr_delta);
		ret = zx_slotpart_of_parts(np, &parts);
		of_node_put(np);
		if (ret < 0)
			return ret;
		if (ret > 0)
			nr = ret;
	}
	if (!parts) {
		/* No usable DT description: the fallback table, unchanged. */
		pr_info("no partitions node data, using the built-in layout\n");
		nr = ARRAY_SIZE(zx_slotpart_fallback);
		out = kcalloc(nr, sizeof(*out), GFP_KERNEL);
		if (!out)
			return -ENOMEM;
		for (i = 0; i < nr; i++) {
			out[i].name = kstrdup(zx_slotpart_fallback[i].label,
					      GFP_KERNEL);
			out[i].offset = zx_slotpart_fallback[i].ofs;
			out[i].size = zx_slotpart_fallback[i].size;
			out[i].mask_flags = zx_slotpart_fallback[i].mask;
		}
		*pparts = out;
		return nr;
	}

	/* Read both slot headers through the normal mtd path. */
	for (i = 0; i < 2; i++) {
		size_t retlen;

		if (!in.base[i])
			continue;
		ret = mtd_read(master, in.base[i], ZX_SP_HDR_SIZE, &retlen,
			       in.hdr[i]);
		in.have_hdr[i] = ret == 0 && retlen == ZX_SP_HDR_SIZE;
		if (!in.have_hdr[i])
			pr_info("slot %d header at %08x unreadable (%d)\n",
				i, in.base[i], ret);
	}
	zx_slotpart_decide(&in, &dec);
	pr_info("%s\n", dec.note);

	if (dec.changed) {
		int moved = 0;

		for (i = 0; i < nr; i++) {
			if (!parts[i].label)
				continue;
			if (!strcmp(parts[i].label, "rootfs")) {
				parts[i].ofs = dec.rootfs_ofs;
				moved++;
			}
			if (!strcmp(parts[i].label, "slot1-header")) {
				parts[i].ofs = dec.other_hdr_ofs;
				moved++;
			}
		}
		if (moved != 2)
			pr_warn("expected to move rootfs and slot1-header, moved %d; keeping the offsets as adjusted\n",
				moved);
	}

	out = kcalloc(nr, sizeof(*out), GFP_KERNEL);
	if (!out) {
		while (nr--)
			kfree(parts[nr].label);
		kfree(parts);
		return -ENOMEM;
	}
	for (i = 0; i < nr; i++) {
		out[i].name = parts[i].label;	/* ownership moves here */
		out[i].offset = parts[i].ofs;
		out[i].size = parts[i].size;
		out[i].mask_flags = parts[i].mask;
	}
	kfree(parts);
	*pparts = out;
	return nr;
}

static const struct of_device_id zx_slotpart_of_match_table[] = {
	{ .compatible = "zte,zx279128s-slotpart" },
	{},
};
MODULE_DEVICE_TABLE(of, zx_slotpart_of_match_table);

static struct mtd_part_parser zx_slotpart_parser = {
	.parse_fn = zx_slotpart_parse,
	.cleanup = zx_slotpart_cleanup,
	.name = "zx279128s-slotpart",
	.of_match_table = zx_slotpart_of_match_table,
};
module_mtd_part_parser(zx_slotpart_parser);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZTE ZX279128S slot-aware MTD partition parser");
