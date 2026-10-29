/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zx279128s-slotpart decision core - pure logic, no kernel headers, so the
 * host unit tests (tests/test-slotpart-decisions.c) compile the exact code
 * the kernel runs.  The kernel parser includes this file; it is never built
 * standalone.
 *
 * What cspboot does at every boot: read both 28 MiB slot headers, keep the
 * one whose CRC1 (over the first 0xa4 bytes of the 512-byte version header)
 * is valid, and of those the one with the higher serial - ties go to slot 0.
 * The kernel's partition table must follow that decision, because "rootfs"
 * lives at slot+0x400000 and the OTHER system's header at slot+0x1400000,
 * both inside whichever slot is NOT booting ours.  The v1.3 slotfix tried to
 * read cspboot's bootpara block from RAM; the zImage decompressor uses that
 * page for its own page tables (dumped on hardware: 0x40007000 holds section
 * descriptors), so the block never survives to the kernel.  This parser
 * instead re-derives the same answer from the flash itself, at partition
 * parse time, where the mtd read path exists.
 *
 * Every failure is "change nothing": no valid header, an unreadable slot or
 * any surprise keeps the built-in (slot-1) layout, which is what every image
 * up to v1.4 booted from.
 */
#ifndef ZX_SLOTPART_CORE_H
#define ZX_SLOTPART_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <string.h>
#endif

#define ZX_SP_HDR_SIZE		0x200
#define ZX_SP_CRC_COVER		0xa4	/* CRC1 covers [0x00:0xa4) */
#define ZX_SP_CRC_OFS		0xa4
#define ZX_SP_MAGIC_OFS		0xf4
#define ZX_SP_SERIAL_OFS	0x1f4

static const uint8_t zx_sp_inner_magic[16] = {
	0x33, 0x33, 0x33, 0x33, 0x66, 0x66, 0x66, 0x66,
	0x99, 0x99, 0x99, 0x99, 0xcc, 0xcc, 0xcc, 0xcc,
};

struct zx_slotpart_in {
	uint32_t base[2];	/* absolute flash offsets of slot 0 / slot 1 */
	uint32_t fs_delta;	/* rootfs = base + delta (0x400000) */
	uint32_t hdr_delta;	/* other header = base + delta (0x1400000) */
	int have_hdr[2];	/* 1: the 512 bytes read back; 0: unreadable */
	uint8_t hdr[2][ZX_SP_HDR_SIZE];
};

struct zx_slotpart_out {
	int active;		/* 0 or 1; -1 = keep the built-in layout */
	int changed;		/* 1: rootfs/other-header offsets moved */
	uint32_t rootfs_ofs;	/* absolute, valid when changed */
	uint32_t other_hdr_ofs;
	const char *note;
};

/* The standard CRC-32 the firmware headers use (reflected IEEE, init and
 * final xor) - the same value e2631-fwslot's crc32() computes and cspboot
 * checks as CRC1.
 */
static uint32_t zx_sp_crc32(const uint8_t *buf, unsigned len)
{
	uint32_t table[256], c, r = 0xffffffffu;
	unsigned i, k;

	for (i = 0; i < 256; i++) {
		c = i;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		table[i] = c;
	}
	for (i = 0; i < len; i++) {
		r ^= buf[i];
		r = table[r & 0xff] ^ (r >> 8);
	}
	return r ^ 0xffffffffu;
}

static uint32_t zx_sp_rd32(const uint8_t *p, unsigned ofs)
{
	return (uint32_t)p[ofs] | (uint32_t)p[ofs + 1] << 8 |
	       (uint32_t)p[ofs + 2] << 16 | (uint32_t)p[ofs + 3] << 24;
}

/* Is this slot's header one cspboot would accept?  Inner magic, then CRC1 -
 * exactly the two checks slot_scan() in e2631-fwslot makes. */
static int zx_sp_header_valid(const uint8_t *hdr)
{
	if (memcmp(hdr + ZX_SP_MAGIC_OFS, zx_sp_inner_magic, 16))
		return 0;
	return zx_sp_crc32(hdr, ZX_SP_CRC_COVER) == zx_sp_rd32(hdr, ZX_SP_CRC_OFS);
}

/* Decide which slot is booting and whether the partition offsets must move.
 * Always returns 0; the caller treats out->active < 0 as "keep the default".
 */
static int zx_slotpart_decide(const struct zx_slotpart_in *in,
			      struct zx_slotpart_out *out)
{
	uint32_t serial[2] = { 0, 0 };
	int valid[2] = { 0, 0 }, s;

	memset(out, 0, sizeof(*out));
	out->active = -1;

	for (s = 0; s < 2; s++) {
		if (!in->have_hdr[s])
			continue;
		if (!zx_sp_header_valid(in->hdr[s]))
			continue;
		valid[s] = 1;
		serial[s] = zx_sp_rd32(in->hdr[s], ZX_SP_SERIAL_OFS);
	}

	if (!valid[0] && !valid[1]) {
		out->note = "no valid version header in either slot; keeping the built-in layout";
		return 0;
	}
	if (valid[0] && valid[1])
		out->active = serial[1] > serial[0] ? 1 : 0;
	else
		out->active = valid[0] ? 0 : 1;

	if (out->active == 1) {
		out->changed = 0;
		out->note = "slot 1 is booting; the built-in layout already matches";
		return 0;
	}
	out->changed = 1;
	out->rootfs_ofs = in->base[0] + in->fs_delta;
	out->other_hdr_ofs = in->base[0] + in->hdr_delta;
	out->note = "slot 0 is booting; moving rootfs and the other system's header into slot 0";
	return 0;
}

#endif /* ZX_SLOTPART_CORE_H */
