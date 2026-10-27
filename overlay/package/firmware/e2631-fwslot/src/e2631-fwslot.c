// SPDX-License-Identifier: GPL-2.0-only
/*
 * e2631-fwslot - select and flash the ZTE ZXHN E2631 firmware slots from Linux.
 *
 * E2631 keeps two whole-system firmware slots in SPI NAND:
 *   slot 0 @ 0x00700000 .. 0x02300000   (28 MiB)
 *   slot 1 @ 0x02300000 .. 0x03F00000   (28 MiB)
 * and the vendor bootloader (cspboot) starts the valid slot with the higher
 * version-header serial; equal serials start slot 0.  Only CRC1 of the header
 * is checked by the bootloader, and CRC1 covers [0x00:0xa4] - which is before
 * the serial - so changing the serial never invalidates it.
 *
 * Images use the vendor upgrade package format, so one file installs from the
 * vendor web page or from OpenWrt sysupgrade, and the two systems can swap in
 * place.  Unlike the SR1010, an E2631 package carries no bootloader
 * (header +0x8c == 0) and its kernel extent is a plaintext uImage; only the
 * stock rootfs is AES-128-ECB at rest, and that decryption lives in the vendor
 * kernel's MTD read path, so a mainline kernel reads plain bytes.
 *
 * "switch" checks the other slot against the CRCs recorded in its header and
 * then rewrites only that header with a higher serial, so the next boot
 * selects it.  This is the way back to the vendor system without touching a
 * byte of its firmware body.
 *
 * Slot roles are NOT fixed by the slot number (v1.4, checklist P0-0): cspboot
 * boots "valid header CRC1 + higher serial, ties to slot 0", so the vendor
 * system and OpenWrt can sit in either slot and have swapped before
 * (known-issues ROOTFS-SLOT-ASSUMPTION, 2026-10-24).  Every write path judges
 * the TARGET SLOT BY ITS CONTENT, read from its version header:
 *
 *   - "upgrade <pkg>" rewrites the RUNNING slot in place - the other slot is
 *     the way back, so it must hold an intact vendor image (or
 *     --no-fallback-check must accept otherwise);
 *   - "install <pkg>" writes the OTHER slot - if that slot holds a vendor or
 *     unidentified image it is refused unless --replace-other-system is
 *     passed, and then the slot's first MiB is parked on userdata before any
 *     erase (no backup, no write);
 *   - "upgrade-check" runs the same two judgments without writing anything;
 *     sysupgrade calls it before services go down and again from the ramfs.
 *
 * The identity comes from the version string the header carries: "OpenWrt"
 * for this port (mkfactory --version since v1.3-r3), "V1.0.0." for the
 * vendor scheme.  Anything else is treated as another system - including the
 * pre-r3 builds of this port, which stamped the template's vendor string;
 * for those the guard asks for --replace-other-system, which is the
 * conservative answer.
 *
 * Reads and writes stay inside the two slot windows.  Where the running system
 * maps a slot as one partition that partition is used; where it does not (the
 * vendor table only exposes the first 3.25 MiB of each slot) the whole-flash
 * device is used instead and every offset is checked against the slot.
 * Bootloader, tag, wifi and config partitions are never touched.
 */
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mtd/mtd-user.h>

#define VER_HDR_SIZE	0x200
#define OUTER_HDR_SIZE	0x14
#define KERNEL_MAGIC_SIZE 32	/* 33333333 cccccccc 88888888 dddddddd + 16 x ff */
#define UBOOT_SIZE	0	/* E2631 upgrade packages carry no bootloader */

/*
 * Version header fields this tool reads or writes, relative to the start of the
 * 512-byte header. Fields it must not disturb - the upgrade keys at +0x08 and
 * +0x0c, +0xb8 which the vendor switch path wants zero, the hardware string at
 * +0x110 and the slot address table at +0x1e0 - are copied through from the
 * package untouched.
 */
#define H_VERSION	0x10
#define H_FW_SIZE	0x30	/* body size; also where the header lands when
				 * the body is block aligned, which stock is.
				 */
#define H_KERNEL_SIZE	0x34
#define H_KERNEL_FILEOFS 0x38
#define H_KERNEL_CRC	0x3c
#define H_FS_SIZE	0x40
#define H_FS_FILEOFS	0x44
#define H_FS_CRC	0x48
#define H_SLOT0		0x4c
#define H_SLOT1		0x5c
#define H_PRODUCT	0x6c
#define H_BOOT_INCLUDED 0x8c
#define H_CRC		0xa4	/* CRC32 over [0x00:0xa4] - checked by cspboot */
#define H_BUILD		0xa8
#define H_INNER_MAGIC	0xf4
#define H_SLOT_START	0x1f0
#define H_SERIAL	0x1f4
#define H_NEW		0x1f8
#define H_CRC2		0x1fc	/* CRC32 over [0x00:0x1fc] - covers the serial */

#define PRODUCT		"ZXHN E2631 V1.0.0"

/* The two firmware slots, by absolute offset in the whole flash. */
static const uint32_t slot_start[2] = { 0x00700000, 0x02300000 };
static const uint32_t slot_span = 0x01C00000;

/* Partition labels this tool expects in the board device tree. Looking up by
 * name rather than by /sys/.../offset keeps working on kernels that do not
 * export the partition offset.
 */
static const char *slot_label[2] = { "slot0", "slot1" };

static const uint8_t outer_magic[16] = {
	0x99,0x99,0x99,0x99, 0x44,0x44,0x44,0x44,
	0x55,0x55,0x55,0x55, 0xaa,0xaa,0xaa,0xaa,
};
static const uint8_t inner_magic[16] = {
	0x33,0x33,0x33,0x33, 0x66,0x66,0x66,0x66,
	0x99,0x99,0x99,0x99, 0xcc,0xcc,0xcc,0xcc,
};

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc32_update(uint32_t c, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len--)
		c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c;
}

static uint32_t crc32(const void *buf, size_t len)
{
	return crc32_update(0xffffffffu, buf, len) ^ 0xffffffffu;
}

static uint32_t rd32(const uint8_t *p, size_t ofs)
{
	return p[ofs] | p[ofs + 1] << 8 | p[ofs + 2] << 16 |
	       (uint32_t)p[ofs + 3] << 24;
}

static void wr32(uint8_t *p, size_t ofs, uint32_t v)
{
	p[ofs] = v; p[ofs + 1] = v >> 8;
	p[ofs + 2] = v >> 16; p[ofs + 3] = v >> 24;
}

static void die(const char *msg)
{
	fprintf(stderr, "e2631-fwslot: %s: %s\n", msg, strerror(errno));
	exit(1);
}

static long mtd_sysfs_u(int index, const char *attr)
{
	char path[64], buf[32];
	int fd, n;

	snprintf(path, sizeof(path), "/sys/class/mtd/mtd%d/%s", index, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return strtol(buf, NULL, 0);
}

static void mtd_sysfs_s(int index, const char *attr, char *out, size_t len)
{
	char path[64], buf[128];
	int fd, n;

	out[0] = 0;
	snprintf(path, sizeof(path), "/sys/class/mtd/mtd%d/%s", index, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	if (n && buf[n - 1] == '\n')
		buf[n - 1] = 0;
	snprintf(out, len, "%.*s", (int)(len - 1), buf);
}

/* Find the MTD partition carrying the given label. */
static int mtd_find_by_label(const char *label)
{
	DIR *d = opendir("/sys/class/mtd");
	struct dirent *e;
	int found = -1;
	char name[64];

	if (!d)
		die("open /sys/class/mtd");
	while ((e = readdir(d))) {
		int idx;

		if (sscanf(e->d_name, "mtd%d", &idx) != 1 || strchr(e->d_name, 'r'))
			continue;
		mtd_sysfs_s(idx, "name", name, sizeof(name));
		if (!strcmp(name, label)) {
			found = idx;
			break;
		}
	}
	closedir(d);
	return found;
}

/* Fallback: locate a partition by its absolute flash offset, if the kernel
 * exports it. Returns -1 when the attribute is unavailable.
 */
static int mtd_find_by_offset(uint32_t offset)
{
	DIR *d = opendir("/sys/class/mtd");
	struct dirent *e;
	int found = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		int idx;

		if (sscanf(e->d_name, "mtd%d", &idx) != 1 || strchr(e->d_name, 'r'))
			continue;
		if (mtd_sysfs_u(idx, "offset") == (long)offset) {
			found = idx;
			break;
		}
	}
	closedir(d);
	return found;
}

static int slot_mtd(int slot)
{
	int idx = mtd_find_by_label(slot_label[slot]);

	if (idx < 0)
		idx = mtd_find_by_offset(slot_start[slot]);
	return idx;
}

struct mtd {
	int fd;
	struct mtd_info_user info;
	/* Every offset below is relative to the slot; base translates it. */
	uint64_t base;
	uint32_t span;
};

static void mtd_open(struct mtd *m, int index, bool writable)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/mtd%d", index);
	m->fd = open(path, writable ? O_RDWR : O_RDONLY);
	if (m->fd < 0)
		die(path);
	if (ioctl(m->fd, MEMGETINFO, &m->info) < 0)
		die("MEMGETINFO");
	m->base = 0;
	m->span = m->info.size;
}

/*
 * Turn a slot-relative offset into a device offset, and stop anything that
 * would leave the slot: through the whole-flash device an unchecked offset
 * would land in the neighbouring slot or in the bootloader.
 */
static uint64_t slot_ofs(struct mtd *m, uint64_t ofs)
{
	if (ofs >= m->span) {
		fprintf(stderr, "e2631-fwslot: offset %#llx is past the slot, refusing\n",
			(unsigned long long)ofs);
		exit(1);
	}
	return m->base + ofs;
}

static bool mtd_bad(struct mtd *m, uint64_t ofs)
{
	long long o = slot_ofs(m, ofs);

	return ioctl(m->fd, MEMGETBADBLOCK, &o) > 0;
}

/*
 * Both cspboot and the kernel locate the payload by skipping the blocks the
 * bad block table lists, so a block that cannot be written has to be listed
 * there, or everything behind it would be read from the wrong place.
 */
static void mtd_mark_bad(struct mtd *m, uint64_t ofs)
{
	long long o = slot_ofs(m, ofs);

	if (ioctl(m->fd, MEMSETBADBLOCK, &o) < 0)
		perror("  could not mark the block bad");
}

static int mtd_erase(struct mtd *m, uint64_t ofs)
{
	struct erase_info_user e = { .start = slot_ofs(m, ofs), .length = m->info.erasesize };

	ioctl(m->fd, MEMUNLOCK, &e);
	return ioctl(m->fd, MEMERASE, &e);
}

static void mtd_read(struct mtd *m, uint64_t ofs, void *buf, size_t len)
{
	if (lseek(m->fd, slot_ofs(m, ofs), SEEK_SET) < 0 ||
	    read(m->fd, buf, len) != (ssize_t)len)
		die("mtd read");
}

/* A device that contains the whole slot but is not the slot itself: under the
 * vendor firmware no partition spans a slot (kernel1/kernel2 cover its first
 * 3.25 MiB), and only the master flash reaches the payload and the version
 * header at the end of it.
 */
static int mtd_find_covering(uint64_t offset, uint32_t size)
{
	DIR *d = opendir("/sys/class/mtd");
	struct dirent *e;
	int found = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		int idx;

		if (sscanf(e->d_name, "mtd%d", &idx) != 1 || strchr(e->d_name, 'r'))
			continue;
		if (mtd_sysfs_u(idx, "offset") == 0 &&
		    mtd_sysfs_u(idx, "size") >= (long)(offset + size)) {
			found = idx;
			break;
		}
	}
	closedir(d);
	return found;
}

/*
 * Open the slot as a window of slot_span bytes whose offsets start at zero,
 * whichever way the running system happens to map the flash. Returns the mtd
 * number, or -1 when nothing on this system reaches the slot.
 */
static int slot_open(struct mtd *m, int slot, bool writable)
{
	int idx = slot_mtd(slot);

	if (idx >= 0) {
		mtd_open(m, idx, writable);
		if (m->info.size == slot_span)
			return idx;
		close(m->fd);
	}

	idx = mtd_find_covering(slot_start[slot], slot_span);
	if (idx < 0) {
		fprintf(stderr,
			"e2631-fwslot: nothing maps slot %d (%#x + %#x) on this system.\n",
			slot, slot_start[slot], slot_span);
		return -1;
	}
	mtd_open(m, idx, writable);
	m->base = slot_start[slot];
	m->span = slot_span;
	if (writable)
		fprintf(stderr, "  slot %d reached through mtd%d (whole flash, from %#llx)\n",
			slot, idx, (unsigned long long)slot_start[slot]);
	return idx;
}

static int slot_open_or_die(struct mtd *m, int slot, bool writable)
{
	int idx = slot_open(m, slot, writable);

	if (idx < 0)
		exit(1);
	return idx;
}

/* Find this slot's version header by scanning erase blocks. Only what cspboot
 * goes by: the inner magic, then CRC1.
 */
static long slot_scan(struct mtd *m, uint32_t *serial)
{
	uint8_t hdr[VER_HDR_SIZE];
	uint32_t bs = m->info.erasesize;

	for (uint64_t ofs = 0; ofs + VER_HDR_SIZE <= m->span; ofs += bs) {
		if (mtd_bad(m, ofs))
			continue;
		mtd_read(m, ofs, hdr, sizeof(hdr));
		if (memcmp(hdr + H_INNER_MAGIC, inner_magic, 16))
			continue;
		if (crc32(hdr, H_CRC) != rd32(hdr, H_CRC))
			continue;
		if (serial)
			*serial = rd32(hdr, H_SERIAL);
		return ofs;
	}
	return -1;
}

static uint32_t max_serial(void)
{
	uint32_t best = 0;

	for (int s = 0; s < 2; s++) {
		struct mtd m;
		uint32_t serial;

		slot_open_or_die(&m, s, false);
		if (slot_scan(&m, &serial) >= 0 && serial > best)
			best = serial;
		close(m.fd);
	}
	return best;
}

/*
 * cspboot puts the slot it booted into the kernel command line, as the trailing
 * seven fields after the literal "U-Boot":
 *   console=... mem=256M; U-Boot V1.0.0 20250325110003 0x700000 0x0 0xaf 0xa3
 * The fourth is the flash base address of the running slot.
 */
static long running_slot_offset(void)
{
	char buf[4096], tag[32], ver[32], date[64];
	unsigned long base = 0, x0 = 0, x1 = 0, x2 = 0;
	FILE *f = fopen("/proc/cmdline", "r");
	char *p;
	int n;

	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "U-Boot");
	if (p) {
		if (sscanf(p, "%31s %31s %63s %lx %lx %lx %lx", tag, ver, date,
			   &base, &x0, &x1, &x2) >= 4)
			return (long)base;
		return -1;
	}

	/*
	 * OpenWrt gets here: CONFIG_CMDLINE_FORCE replaces the vendor bootargs,
	 * and this is exactly the system that has to be able to switch back.
	 * The partition the root filesystem lives on says where we are - its
	 * sysfs offset is an absolute flash offset, so it falls inside one slot.
	 *
	 * Only trust that on our own image. A vendor system without the trailer
	 * would answer with the single "rootfs" of its static table (slot A),
	 * which says nothing about the slot it really booted; the whole-slot
	 * partitions exist in the E2631 device tree and nowhere else.
	 */
	{
		int idx;
		long ofs;

		if (mtd_find_by_label("slot0") < 0)
			return -1;
		idx = mtd_find_by_label("rootfs");
		if (idx < 0)
			return -1;
		ofs = mtd_sysfs_u(idx, "offset");
		if (ofs < 0)
			return -1;
		return ofs;
	}
}

static int slot_index(long offset)
{
	int s;

	/* Range, not equality: the rootfs of a slot is not at the slot's start. */
	for (s = 0; s < 2; s++)
		if (offset >= (long)slot_start[s] && offset < (long)(slot_start[s] + slot_span))
			return s;
	return -1;
}

struct region {
	const char *fail;
	uint64_t start, len;
	uint32_t want, crc;
};

static void region_feed(struct region *r, uint64_t pos, const uint8_t *buf,
			size_t n)
{
	uint64_t lo = pos > r->start ? pos : r->start;
	uint64_t end = r->start + r->len;
	uint64_t hi = pos + n < end ? pos + n : end;

	if (lo < hi)
		r->crc = crc32_update(r->crc, buf + (lo - pos), hi - lo);
}

/*
 * Check a slot the way the header describes it: header CRCs, then the kernel
 * and root filesystem regions, read in good blocks from the start of the slot.
 * The package records file offsets; the slot only knows the distance between
 * them, so the rootfs sits at kstart + (foff - koff).
 */
static const char *slot_verify(struct mtd *m, const uint8_t *hdr)
{
	uint32_t bs = m->info.erasesize;
	uint64_t body_len = rd32(hdr, H_FW_SIZE);
	uint32_t koff = rd32(hdr, H_KERNEL_FILEOFS);
	uint32_t foff = rd32(hdr, H_FS_FILEOFS);
	uint64_t kstart = UBOOT_SIZE + KERNEL_MAGIC_SIZE;
	uint64_t pos = 0, ofs = 0;
	const char *bad = NULL;
	struct region r[2];
	uint8_t *buf;

	if (crc32(hdr, H_CRC) != rd32(hdr, H_CRC) ||
	    crc32(hdr, H_CRC2) != rd32(hdr, H_CRC2))
		return "version header CRC mismatch";
	if (foff < koff)
		return "bad firmware layout";
	if (rd32(hdr, H_BOOT_INCLUDED) != 0)
		return "package claims to update the bootloader";

	r[0] = (struct region){ "kernel CRC mismatch", kstart,
			       rd32(hdr, H_KERNEL_SIZE),
			       rd32(hdr, H_KERNEL_CRC), 0xffffffffu };
	r[1] = (struct region){ "root filesystem CRC mismatch",
			       kstart + (foff - koff), rd32(hdr, H_FS_SIZE),
			       rd32(hdr, H_FS_CRC), 0xffffffffu };
	if (body_len > m->span)
		return "bad firmware layout";
	for (int i = 0; i < 2; i++)
		if (r[i].start + r[i].len > body_len)
			return "bad firmware layout";

	buf = malloc(bs);
	if (!buf)
		return "out of memory";
	while (pos < body_len) {
		size_t n = body_len - pos < bs ? body_len - pos : bs;

		if (ofs + bs > m->span) {
			bad = "payload runs past the end of the slot";
			break;
		}
		if (mtd_bad(m, ofs)) {
			ofs += bs;
			continue;
		}
		if (pread(m->fd, buf, n, slot_ofs(m, ofs)) != (ssize_t)n) {
			bad = "read error";
			break;
		}
		for (int i = 0; i < 2; i++)
			region_feed(&r[i], pos, buf, n);
		pos += n;
		ofs += bs;
	}
	free(buf);
	if (bad)
		return bad;
	for (int i = 0; i < 2; i++)
		if ((r[i].crc ^ 0xffffffffu) != r[i].want)
			return r[i].fail;
	return NULL;
}

struct package {
	uint8_t *data;
	size_t size;
	const uint8_t *hdr;
	const uint8_t *body;
	uint32_t body_len;
};

static void package_load(struct package *p, const char *path)
{
	FILE *f = fopen(path, "rb");
	long size;
	uint32_t hdr_ofs, body_ofs;

	if (!f)
		die(path);
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < OUTER_HDR_SIZE + VER_HDR_SIZE)
		errno = EINVAL, die("package too small");
	p->data = malloc(size);
	if (!p->data || fread(p->data, 1, size, f) != (size_t)size)
		die("read package");
	fclose(f);
	p->size = size;

	if (memcmp(p->data, outer_magic, 16))
		errno = EINVAL, die("not an E2631 upgrade package (outer magic)");
	hdr_ofs = 0x14 + rd32(p->data, 0x10);
	body_ofs = hdr_ofs + VER_HDR_SIZE;
	if (body_ofs > p->size)
		errno = EINVAL, die("truncated header");
	p->hdr = p->data + hdr_ofs;
	p->body = p->data + body_ofs;
	p->body_len = p->size - body_ofs;

	if (memcmp(p->hdr + H_INNER_MAGIC, inner_magic, 16))
		errno = EINVAL, die("bad version header magic");
	if (crc32(p->hdr, H_CRC) != rd32(p->hdr, H_CRC) ||
	    crc32(p->hdr, H_CRC2) != rd32(p->hdr, H_CRC2))
		errno = EINVAL, die("header CRC mismatch");
	if (strncmp((const char *)p->hdr + H_PRODUCT, PRODUCT, sizeof(PRODUCT) - 1))
		errno = EINVAL, die("package is not for this product");
	if (rd32(p->hdr, H_BOOT_INCLUDED))
		errno = EINVAL, die("package wants to update the bootloader");
	if (rd32(p->hdr, H_FW_SIZE) != p->body_len)
		errno = EINVAL, die("body size mismatch");
	if (rd32(p->hdr, H_SLOT0) != slot_start[0] ||
	    rd32(p->hdr, H_SLOT1) != slot_start[1])
		errno = EINVAL, die("package declares different slot addresses");

	uint32_t koff = rd32(p->hdr, H_KERNEL_FILEOFS);
	uint32_t ksz = rd32(p->hdr, H_KERNEL_SIZE);
	uint32_t foff = rd32(p->hdr, H_FS_FILEOFS);
	uint32_t fsz = rd32(p->hdr, H_FS_SIZE);

	if ((uint64_t)koff + ksz > p->size || (uint64_t)foff + fsz > p->size)
		errno = EINVAL, die("region outside package");
	if (crc32(p->data + koff, ksz) != rd32(p->hdr, H_KERNEL_CRC))
		errno = EINVAL, die("kernel CRC mismatch");
	if (crc32(p->data + foff, fsz) != rd32(p->hdr, H_FS_CRC))
		errno = EINVAL, die("rootfs CRC mismatch");
}

static uint64_t write_block(struct mtd *m, uint64_t ofs, const uint8_t *buf)
{
	uint32_t bs = m->info.erasesize;

	while (ofs < m->span) {
		if (mtd_bad(m, ofs)) {
			fprintf(stderr, "  skip bad block at %#llx\n",
				(unsigned long long)ofs);
			ofs += bs;
			continue;
		}
		if (lseek(m->fd, slot_ofs(m, ofs), SEEK_SET) < 0 ||
		    write(m->fd, buf, bs) != (ssize_t)bs) {
			fprintf(stderr, "  write failed at %#llx, marking the block bad\n",
				(unsigned long long)ofs);
			mtd_mark_bad(m, ofs);
			ofs += bs;
			continue;
		}
		return ofs + bs;
	}
	errno = ENOSPC;
	die("ran out of good blocks");
	return 0;
}

/*
 * Erase every good block of the slot, as the vendor flasher does. A header an
 * older firmware left behind would otherwise still look valid to cspboot and
 * take precedence over the new one.
 */
static void erase_slot(struct mtd *m)
{
	uint32_t bs = m->info.erasesize;

	for (uint64_t ofs = 0; ofs < m->span; ofs += bs) {
		if (mtd_bad(m, ofs))
			continue;
		if (mtd_erase(m, ofs) < 0)
			fprintf(stderr, "  erase failed at %#llx\n",
				(unsigned long long)ofs);
	}
}

/* Rewrite one slot header with a new serial: erase the block, write it whole.
 * NAND can only turn bits off, so an in-place write would land as the AND of
 * old and new - which is exactly how the vendor's own switch command breaks.
 */
static int retitle_slot(int slot, uint32_t serial)
{
	struct mtd m;
	long hpos;
	uint32_t got;
	int idx;
	uint8_t hdr[VER_HDR_SIZE];
	uint8_t old[VER_HDR_SIZE];
	const char *why;

	/*
	 * Refuse to touch a slot this system only reaches through the whole flash.
	 * Under the vendor firmware its 4.1.25 MTD stack reads data written by our
	 * kernel with a different ECC/bad-block layout, so the scan can hand back
	 * another slot's header - and bumping "the" header it found then writes a
	 * valid-looking copy into the target slot, where cspboot prefers it over
	 * the real one and falls back. Seen on hardware on 2026-10-13. Switching is
	 * only dependable from OpenWrt, where each slot is its own partition.
	 */
	idx = slot_open(&m, slot, true);
	if (idx < 0)
		return 1;
	if (m.base != 0) {
		fprintf(stderr,
			"e2631-fwslot: this system has no slot%d partition, so its header cannot be "
			"located reliably; switch slots from OpenWrt instead\n", slot);
		close(m.fd);
		return 1;
	}
	hpos = slot_scan(&m, &got);
	if (hpos < 0) {
		fprintf(stderr, "e2631-fwslot: slot %d holds no valid firmware header\n", slot);
		close(m.fd);
		return 1;
	}
	mtd_read(&m, hpos, hdr, sizeof(hdr));
	memcpy(old, hdr, sizeof(old));

	/* cspboot does not fall back: never point it at a broken slot. */
	why = slot_verify(&m, hdr);
	if (why) {
		fprintf(stderr, "e2631-fwslot: slot %d is damaged (%s), not switching\n",
			slot, why);
		close(m.fd);
		return 1;
	}

	wr32(hdr, H_SERIAL, serial);
	/* H_CRC covers [0:0xa4] and is unaffected; H_CRC2 covers the serial. */
	wr32(hdr, H_CRC2, crc32(hdr, H_CRC2));

	/*
	 * Rewrite the header block as read-modify-write. The live vendor image
	 * has data directly after the 512-byte header inside the same erase
	 * block (0x3860200 reads back full of bytes, not erased), so writing
	 * "header + 0xff" would throw that away. NAND needs an erase before it
	 * can be written, which is why the block is read first.
	 */
	{
		uint8_t *blk = malloc(m.info.erasesize);

		if (!blk)
			die("out of memory");
		mtd_read(&m, hpos, blk, m.info.erasesize);
		memcpy(blk, hdr, VER_HDR_SIZE);
		if (mtd_erase(&m, hpos) < 0)
			die("erase header block");
		if (lseek(m.fd, slot_ofs(&m, hpos), SEEK_SET) < 0 ||
		    write(m.fd, blk, m.info.erasesize) != (ssize_t)m.info.erasesize)
			die("write header");

		/*
		 * Read it back and put the original block back if the header
		 * does not verify. This is the rollback path: a damaged header
		 * here costs the boot loader its way back to the stock slot,
		 * and nothing falls back on its own.
		 */
		mtd_read(&m, hpos, hdr, sizeof(hdr));
		why = slot_verify(&m, hdr);
		if (why) {
			fprintf(stderr, "e2631-fwslot: slot %d header reads back damaged (%s)\n",
				slot, why);
			memcpy(blk, old, VER_HDR_SIZE);
			if (mtd_erase(&m, hpos) == 0 &&
			    lseek(m.fd, slot_ofs(&m, hpos), SEEK_SET) >= 0 &&
			    write(m.fd, blk, m.info.erasesize) == (ssize_t)m.info.erasesize)
				fprintf(stderr, "e2631-fwslot: original header restored, "
					"slot %d left as it was\n", slot);
			else
				fprintf(stderr, "e2631-fwslot: RESTORE FAILED - slot %d needs "
					"its header rebuilt by hand\n", slot);
			free(blk);
			close(m.fd);
			return 1;
		}
		free(blk);
	}
	close(m.fd);
	printf("ok: slot %d header at %#lx now serial %u\n", slot,
	       (unsigned long)hpos, serial);
	return 0;
}

static int cmd_check(const char *path)
{
	struct package p;

	package_load(&p, path);
	free(p.data);
	printf("ok: valid E2631 package for %s\n", PRODUCT);
	return 0;
}

/* ---- Slot identity and the two write guards (v1.4, checklist P0-0) ----- */

enum slot_identity {
	ID_EMPTY,	/* nothing that reads back as a valid header */
	ID_VENDOR,	/* "V1.0.0.*" - the factory scheme */
	ID_OURS,	/* "OpenWrt ..." - this port since v1.3-r3 */
	ID_UNKNOWN,	/* a valid header of some other provenance */
};

static enum slot_identity header_identity(const uint8_t *hdr)
{
	if (memcmp(hdr + H_INNER_MAGIC, inner_magic, 16))
		return ID_EMPTY;
	if (crc32(hdr, H_CRC) != rd32(hdr, H_CRC))
		return ID_EMPTY;
	if (!strncmp((const char *)hdr + H_VERSION, "OpenWrt", 7))
		return ID_OURS;
	if (!strncmp((const char *)hdr + H_VERSION, "V1.0.0.", 7))
		return ID_VENDOR;
	return ID_UNKNOWN;
}

static const char *identity_name(enum slot_identity id)
{
	switch (id) {
	case ID_EMPTY:   return "empty";
	case ID_VENDOR:  return "vendor image";
	case ID_OURS:    return "our image";
	case ID_UNKNOWN: return "unidentified image";
	}
	return "unknown";
}

/* What a slot holds, decided without opening it for writing.  `intact` means
 * the header verifies the way cspboot checks it AND the body matches the
 * CRCs the header records.
 */
struct slot_state {
	bool known;		/* a device reaches the slot at all */
	enum slot_identity identity;
	bool intact;
	uint8_t hdr[VER_HDR_SIZE];
	const char *damage;	/* slot_verify() verdict when not intact */
};

static void slot_state_read(int s, struct slot_state *st)
{
	struct mtd m;
	long hpos;

	memset(st, 0, sizeof(*st));
	if (slot_open(&m, s, false) < 0)
		return;
	st->known = true;
	hpos = slot_scan(&m, NULL);
	if (hpos >= 0) {
		mtd_read(&m, hpos, st->hdr, sizeof(st->hdr));
		st->identity = header_identity(st->hdr);
		st->damage = slot_verify(&m, st->hdr);
		st->intact = st->identity != ID_EMPTY && !st->damage;
	}
	close(m.fd);
}

/* install writes the OTHER slot.  An empty or own-image target is free game;
 * a vendor or unidentified one is another system - the only fallback this
 * board has - and goes only with the explicit replace flag (and a backup,
 * taken by the caller before the first erase).
 */
static const char *install_guard(const struct slot_state *target, bool replace)
{
	switch (target->identity) {
	case ID_EMPTY:
	case ID_OURS:
		return NULL;
	case ID_VENDOR:
	case ID_UNKNOWN:
		if (!replace)
			return "target slot holds another system; pass --replace-other-system "
			       "to overwrite it (the slot head is backed up first)";
		return NULL;
	}
	return NULL;
}

/* upgrade rewrites the RUNNING slot in place.  The other slot is the only
 * way back if this write goes bad, so it must be an intact vendor image -
 * unless --no-fallback-check accepted otherwise explicitly.
 */
static const char *upgrade_guard(const struct slot_state *other, bool no_check)
{
	if (no_check)
		return NULL;
	if (!other->known)
		return "the other slot cannot be read at all, and it is the only fallback "
		       "if this write fails; pass --no-fallback-check to accept that";
	if (other->identity != ID_VENDOR)
		return "the other slot does not hold a vendor image, and it is the only "
		       "fallback if this write fails; pass --no-fallback-check to accept that";
	if (!other->intact)
		return "the other slot holds a vendor header but its body does not verify, "
		       "and it is the only fallback if this write fails; "
		       "pass --no-fallback-check to accept that";
	return NULL;
}

/* Park a slot's first MiB on userdata before replacing another system with
 * it.  The vendor slot is the only rollback this board has, so a replacement
 * without a backup does not happen: any failure here is fatal to the write.
 */
static void backup_slot_head(struct mtd *m, int slot, uint32_t serial)
{
	const char *dir = getenv("E2631_BACKUP_DIR");
	char path[512], tmp[512], line[512], mp[400];
	uint32_t bs = m->info.erasesize;
	uint64_t left = 1024 * 1024;
	uint8_t *buf;
	FILE *f;
	size_t n;

	mp[0] = 0;
	if (!dir || !*dir) {
		/* the mountpoint of the userdata volume, from /proc/mounts */
		f = fopen("/proc/mounts", "r");
		if (f) {
			while (fgets(line, sizeof(line), f))
				if (strstr(line, "userdata") &&
				    sscanf(line, "%*s %299s", mp) == 1 &&
				    strcmp(mp, "userdata"))
					break;
			fclose(f);
		}
		if (!mp[0])
			die("userdata is not mounted, so the other system cannot be "
			    "backed up before it is replaced; initialize userdata first "
			    "(or set E2631_BACKUP_DIR)");
		dir = mp;
	}
	/* A bounded copy with PROVABLE fits for -Werror=format-truncation:
	 * mountpoints are capped at 299 bytes when scanned from /proc/mounts,
	 * an E2631_BACKUP_DIR at 300, so dirbuf (400) holds dir plus the 13
	 * bytes of "/e2631-backup" with room; tmp/path (512) hold dirbuf plus
	 * the "/slot%d-serial%u-head.bin" shape (~41 bytes worst case) - every
	 * size relation below is one gcc can see.
	 */
	{
		char dirbuf[400];
		size_t dl;

		dl = strlen(dir);
		if (dl > 300)
			die("backup directory path too long");
		memcpy(dirbuf, dir, dl + 1);
		if (dir == mp) {
			memcpy(dirbuf + dl, "/e2631-backup", 14);
			dl += 13;
		}
		mkdir(dirbuf, 0755);
		snprintf(tmp, sizeof(tmp), "%s/slot%d-serial%u-head.bin.part",
			 dirbuf, slot, serial);
		snprintf(path, sizeof(path), "%s/slot%d-serial%u-head.bin",
			 dirbuf, slot, serial);
	}
	buf = malloc(left);
	if (!buf)
		die("out of memory");
	memset(buf, 0xff, left);
	for (uint64_t ofs = 0; ofs < left; ofs += bs) {
		if (!mtd_bad(m, ofs))
			pread(m->fd, buf + ofs, bs < left - ofs ? bs : left - ofs,
			      slot_ofs(m, ofs));	/* read errors leave 0xff */
	}
	f = fopen(tmp, "wb");
	if (!f)
		die(tmp);
	n = fwrite(buf, 1, left, f);
	fclose(f);
	free(buf);
	if (n != left)
		die("backup write came up short");
	if (rename(tmp, path))
		die("backup rename");
	fprintf(stderr, "backed up slot %d head (1 MiB) to %s\n", slot, path);
}

/* The one write path behind install and upgrade.  `inplace` selects the
 * RUNNING slot (upgrade); otherwise the target is the other slot, with
 * another-system content going only through `replace` (and a backup).
 */
static int cmd_write(const char *path, bool inplace, int want_slot,
		     bool replace, bool no_fallback_check)
{
	struct package p;
	struct slot_state states[2];
	int target_slot, other_slot, idx, running;
	struct mtd m;
	uint8_t hdr[VER_HDR_SIZE];
	uint32_t bs, serial;
	uint64_t ofs, left;
	const uint8_t *src;
	uint8_t *blk;
	long hpos;
	const char *why;

	package_load(&p, path);

	running = slot_index(running_slot_offset());
	if (inplace) {
		if (running < 0)
			errno = ENODEV, die("upgrade rewrites the running slot, but "
				"cannot tell which slot is running, from /proc/cmdline or "
				"from the rootfs partition");
		target_slot = running;
		other_slot = running ^ 1;
	} else {
		/*
		 * The slot the running system reports is not always the slot whose bytes
		 * it booted: after a header had been written into the wrong place, cspboot
		 * validated slot 0's payload while recording slot 1 as the booted version,
		 * and the derived "other slot" then pointed at the stock image. --slot
		 * therefore exists to name the target outright.
		 */
		target_slot = want_slot >= 0 ? want_slot : (running < 0 ? -1 : running ^ 1);
		other_slot = target_slot >= 0 ? target_slot ^ 1 : -1;
		if (target_slot < 0)
			errno = ENODEV, die("cannot tell which slot is running, from "
				"/proc/cmdline or from the rootfs partition");
	}

	/*
	 * Judge the target (install) or the fallback (upgrade) by CONTENT before
	 * touching anything - slot roles are not fixed on this board.
	 */
	slot_state_read(target_slot, &states[target_slot]);
	slot_state_read(other_slot, &states[other_slot]);
	why = inplace ? upgrade_guard(&states[other_slot], no_fallback_check)
		      : install_guard(&states[target_slot], replace);
	if (why) {
		fprintf(stderr, "e2631-fwslot: slot %d currently holds: %s%s%s\n",
			inplace ? other_slot : target_slot,
			states[inplace ? other_slot : target_slot].known
				? identity_name(states[inplace ? other_slot : target_slot].identity)
				: "unreachable",
			states[inplace ? other_slot : target_slot].intact ? " (intact)" : "",
			states[inplace ? other_slot : target_slot].damage ? " (damaged body)" : "");
		die(why);
	}

	idx = slot_open_or_die(&m, target_slot, true);
	bs = m.info.erasesize;
	if (p.body_len > m.span - bs)
		errno = ENOSPC, die("package larger than the slot");

	/*
	 * The board partition table puts the root filesystem and the version
	 * header on fixed offsets inside the slot. Blocks are skipped while
	 * writing, so a single bad block in that range would move both away
	 * from where the kernel looks for them. Check before erasing: the
	 * factory bad block table is content independent, so this is valid
	 * while the slot still holds the vendor image.
	 */
	for (uint64_t o = 0, need = (p.body_len + bs) / bs * bs; o < need; o += bs)
		if (mtd_bad(&m, o)) {
			fprintf(stderr,
				"e2631-fwslot: slot %d has a bad erase block at %#llx, inside the "
				"area the fixed partition offsets rely on; refusing to write\n",
				target_slot, (unsigned long long)o);
			return 1;
		}

	/*
	 * Replacing another system: park its head on userdata first - after the
	 * erase below there is nothing left to back up.
	 */
	if (!inplace && states[target_slot].identity != ID_EMPTY &&
	    states[target_slot].identity != ID_OURS) {
		serial = max_serial() + 1;
		if (rd32(states[target_slot].hdr, H_SERIAL) > serial)
			serial = rd32(states[target_slot].hdr, H_SERIAL);
		backup_slot_head(&m, target_slot, serial);
	}

	blk = malloc(bs);
	if (!blk)
		die("out of memory");

	serial = max_serial() + 1;
	/*
	 * Honour a serial stamped into the package when it is higher: from the
	 * vendor firmware max_serial() is not trustworthy (its 4.1.25 MTD stack
	 * mis-reads blocks our kernel wrote, and it returned 16 for a slot that
	 * held 18), and cspboot selects the slot with the higher serial.
	 */
	if (rd32(p.hdr, H_SERIAL) > serial)
		serial = rd32(p.hdr, H_SERIAL);
	memcpy(hdr, p.hdr, VER_HDR_SIZE);
	wr32(hdr, H_SLOT_START, slot_start[target_slot]);
	wr32(hdr, H_SERIAL, serial);
	wr32(hdr, H_NEW, 1);
	wr32(hdr, H_CRC2, crc32(hdr, H_CRC2));

	fprintf(stderr, "flashing slot %d (mtd%d, %s), serial %u, body %#x\n",
		target_slot, idx, inplace ? "in-place upgrade" : "install",
		serial, p.body_len);

	erase_slot(&m);

	ofs = 0;
	left = p.body_len;
	src = p.body;
	while (left) {
		const uint8_t *chunk = src;
		size_t n = left < bs ? left : bs;

		if (n < bs) {
			memset(blk, 0xff, bs);
			memcpy(blk, src, n);
			chunk = blk;
		}
		ofs = write_block(&m, ofs, chunk);
		left -= n;
		src += bs;
	}

	/* The version header goes in the next good block after the body. */
	memset(blk, 0xff, bs);
	memcpy(blk, hdr, VER_HDR_SIZE);
	ofs = write_block(&m, ofs, blk);

	/*
	 * Read the slot back the way cspboot will: the header first, then the
	 * kernel and root filesystem extents it records.  A mismatch means the
	 * flash does not hold what we wrote, so this slot must not become the
	 * one the bootloader picks: erase its header and leave the other slot
	 * in charge, rather than point the boot at a broken image.
	 */
	hpos = slot_scan(&m, &serial);
	why = "no readable version header";
	if (hpos >= 0) {
		mtd_read(&m, hpos, blk, VER_HDR_SIZE);
		why = slot_verify(&m, blk);
	}
	if (why) {
		if (hpos >= 0)
			mtd_erase(&m, hpos);
		fprintf(stderr,
			"e2631-fwslot: slot %d does not read back (%s); its header is "
			"erased, so the boot stays on slot %d\n",
			target_slot, why, other_slot);
		exit(1);
	}

	free(blk);
	close(m.fd);
	free(p.data);
	printf("ok: slot %d now holds serial %u; reboot to run it\n",
	       target_slot, serial);
	return 0;
}

/* Read-only prerequisite for sysupgrade: the running slot must be known, and
 * the other slot must hold the intact vendor fallback an in-place upgrade
 * relies on.  Exit code 0 prints the state; anything else refuses before
 * sysupgrade takes the system apart.
 */
static int cmd_upgrade_check(void)
{
	struct slot_state states[2];
	int running;
	const char *why;

	running = slot_index(running_slot_offset());
	if (running < 0)
		die("upgrade-check: cannot tell which slot is running, from "
		    "/proc/cmdline or from the rootfs partition; refusing to update");
	slot_state_read(running ^ 1, &states[running ^ 1]);
	why = upgrade_guard(&states[running ^ 1], false);
	if (why) {
		fprintf(stderr, "e2631-fwslot: slot %d currently holds: %s%s%s\n",
			running ^ 1,
			states[running ^ 1].known
				? identity_name(states[running ^ 1].identity)
				: "unreachable",
			states[running ^ 1].intact ? " (intact)" : "",
			states[running ^ 1].damage ? " (damaged body)" : "");
		die(why);
	}
	printf("ok: running slot %d with an intact vendor fallback in slot %d\n",
	       running, running ^ 1);
	return 0;
}

static int cmd_switch(void)
{
	int running = slot_index(running_slot_offset());

	if (running < 0)
		errno = ENODEV, die("cannot tell which slot is running from /proc/cmdline");
	return retitle_slot(running ^ 1, max_serial() + 1);
}

static int cmd_boot(int slot)
{
	if (slot < 0 || slot > 1)
		die("bad slot number");
	return retitle_slot(slot, max_serial() + 1);
}

struct slot_info {
	int mtd;
	bool whole;		/* the partition is the slot, not a view of it */
	bool valid;
	long hpos;
	uint8_t hdr[VER_HDR_SIZE];
	const char *broken;
};

static void slot_info_read(int s, struct slot_info *si)
{
	struct mtd m;
	uint32_t serial;

	memset(si, 0, sizeof(*si));
	si->mtd = slot_open(&m, s, false);
	if (si->mtd < 0)
		return;
	si->whole = m.base == 0;
	si->hpos = slot_scan(&m, &serial);
	if (si->hpos >= 0) {
		si->valid = true;
		mtd_read(&m, si->hpos, si->hdr, sizeof(si->hdr));
		si->broken = slot_verify(&m, si->hdr);
	}
	close(m.fd);
}

static int next_slot(const struct slot_info *si)
{
	int best = -1;

	for (int s = 0; s < 2; s++) {
		if (!si[s].valid || si[s].broken)
			continue;
		if (best < 0 || rd32(si[s].hdr, H_SERIAL) > rd32(si[best].hdr, H_SERIAL))
			best = s;
	}
	return best;
}

static void json_str(const uint8_t *s, size_t max)
{
	putchar('"');
	for (size_t i = 0; i < max && s[i]; i++) {
		if (s[i] == '"' || s[i] == '\\')
			printf("\\%c", s[i]);
		else if (s[i] < 0x20 || s[i] >= 0x7f)
			printf("\\u%04x", s[i]);
		else
			putchar(s[i]);
	}
	putchar('"');
}

static int cmd_info(bool json)
{
	int running = slot_index(running_slot_offset());
	struct slot_info si[2];
	int next;

	for (int s = 0; s < 2; s++)
		slot_info_read(s, &si[s]);
	next = next_slot(si);

	if (json) {
		printf("{\"running\":%d,\"next\":%d,\"slots\":[", running, next);
		for (int s = 0; s < 2; s++) {
			printf("%s{\"slot\":%d,\"mtd\":%d,\"partition_is_slot\":%s,\"valid\":%s,\"intact\":%s",
			       s ? "," : "", s, si[s].mtd, si[s].whole ? "true" : "false",
			       si[s].valid ? "true" : "false",
			       si[s].valid && !si[s].broken ? "true" : "false");
			if (si[s].valid) {
				printf(",\"serial\":%u,\"header_at\":\"0x%lx\",\"version\":",
				       rd32(si[s].hdr, H_SERIAL),
				       (unsigned long)si[s].hpos);
				json_str(si[s].hdr + H_VERSION, 32);
				printf(",\"build\":");
				json_str(si[s].hdr + H_BUILD, 16);
				printf(",\"product\":");
				json_str(si[s].hdr + H_PRODUCT, 52);
				if (si[s].broken) {
					printf(",\"damage\":");
					json_str((const uint8_t *)si[s].broken,
						 strlen(si[s].broken));
				}
			}
			printf("}");
		}
		printf("]}\n");
		return 0;
	}

	printf("running slot: %d\n", running);
	for (int s = 0; s < 2; s++) {
		printf("slot %d: base %#x span %#x mtd%d", s, slot_start[s],
		       slot_span, si[s].mtd);
		if (si[s].mtd < 0) {
			printf(" (no device reaches the slot)\n");
			continue;
		}
		if (!si[s].whole)
			printf(" (viewed through the whole-flash mtd)");
		if (!si[s].valid) {
			printf(" (no valid version header)\n");
			continue;
		}
		printf(" header@%#lx serial=%u version=\"%.32s\"%s%s%s\n",
		       (unsigned long)si[s].hpos, rd32(si[s].hdr, H_SERIAL),
		       (const char *)si[s].hdr + H_VERSION,
		       s == running ? " [running]" : "",
		       s == next ? " [next boot]" : "",
		       si[s].broken ? "" : "");
		if (si[s].broken)
			printf("       damaged: %s (cspboot will not be pointed here)\n",
			       si[s].broken);
	}
	return 0;
}

int main(int argc, char **argv)
{
	crc_init();
	if (argc == 3 && !strcmp(argv[1], "check"))
		return cmd_check(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "upgrade-check"))
		return cmd_upgrade_check();
	if (argc >= 3 && !strcmp(argv[1], "upgrade")) {
		/* in-place on the running slot; the other slot must hold the
		 * intact vendor fallback unless --no-fallback-check says no. */
		bool nofb = false;

		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--no-fallback-check"))
				nofb = true;
			else
				die("unknown upgrade option; see usage");
		}
		return cmd_write(argv[2], true, -1, false, nofb);
	}
	if (argc >= 3 && (!strcmp(argv[1], "install") || !strcmp(argv[1], "flash"))) {
		/*
		 * install names the OTHER slot by default; --slot names the target
		 * outright for the case where the running slot the system reports is
		 * not the slot whose payload it booted. Replacing another system is
		 * a decision: --replace-other-system, after a head backup. The old
		 * --allow-slot0 named a slot number; roles are not fixed here.
		 */
		int want = -1;
		bool replace = false;

		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--replace-other-system"))
				replace = true;
			else if (!strcmp(argv[i], "--allow-slot0"))
				die("--allow-slot0 is gone: slot roles are not fixed on this "
				    "board; pass --replace-other-system to overwrite whatever "
				    "the target slot holds");
			else if (!strcmp(argv[i], "--slot") && i + 1 < argc)
				want = atoi(argv[++i]);
			else
				die("unknown install option; see usage");
		}
		if (want < -1 || want > 1)
			die("--slot takes 0 or 1");
		if (!strcmp(argv[1], "flash"))
			fprintf(stderr, "e2631-fwslot: note: 'flash' is now an alias of 'install'\n");
		return cmd_write(argv[2], false, want, replace, false);
	}
	if (argc == 2 && !strcmp(argv[1], "switch"))
		return cmd_switch();
	if (argc == 3 && !strcmp(argv[1], "boot"))
		return cmd_boot(atoi(argv[2]));
	if (argc == 2 && !strcmp(argv[1], "info"))
		return cmd_info(false);
	if (argc == 3 && !strcmp(argv[1], "info") && !strcmp(argv[2], "-j"))
		return cmd_info(true);

	fprintf(stderr,
		"usage: e2631-fwslot check <package.bin>    verify a package without writing\n"
		"       e2631-fwslot upgrade-check         verify the in-place upgrade prerequisites\n"
		"       e2631-fwslot upgrade <package.bin> [--no-fallback-check]\n"
		"                                          rewrite the RUNNING slot in place\n"
		"       e2631-fwslot install <package.bin> [--slot <0|1>] [--replace-other-system]\n"
		"                                          write the other slot (flash = alias)\n"
		"       e2631-fwslot switch                verify the other slot and make it the next boot\n"
		"       e2631-fwslot boot <0|1>            same, naming the slot explicitly\n"
		"       e2631-fwslot info [-j]             show both slots, -j as JSON\n"
		"\n"
		"Slot roles are decided by the version-header serial, not the slot\n"
		"number: cspboot starts the valid slot with the higher serial and\n"
		"ties go to slot 0, so either slot can hold the vendor system.\n"
		"'upgrade' rewrites the running slot in place and demands an intact\n"
		"vendor image in the other slot as the way back; 'install' writes\n"
		"the other slot and refuses to destroy a vendor or unidentified\n"
		"image there without --replace-other-system, which parks the slot\n"
		"head on userdata before the first erase.\n");
	return 1;
}
