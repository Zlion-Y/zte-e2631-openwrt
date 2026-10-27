/* Offline unit tests for the zx279128s-slotpart decision core (P0-3).
 *
 * The checklist's case list (bootpara magic corrupted, tuple missing, ...)
 * described the v1.3 bootpara scan, whose premise does not hold on hardware -
 * the zImage decompressor overwrites the bootpara page. These cases test the
 * replacement's real logic instead: the cspboot-equivalent choice between the
 * two slot headers read from flash, with every failure keeping the built-in
 * (slot-1) layout.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -o test-slotpart-decisions \
 *       tests/test-slotpart-decisions.c && ./test-slotpart-decisions
 */
#include <stdio.h>
#include <string.h>

#include "../overlay/target/linux/sanechips/files/drivers/mtd/parsers/zx279128s-slotpart-core.c"

static int fails;

static void wr32(uint8_t *p, unsigned ofs, uint32_t v)
{
	p[ofs] = v; p[ofs + 1] = v >> 8;
	p[ofs + 2] = v >> 16; p[ofs + 3] = v >> 24;
}

static void expect(int cond, const char *what)
{
	printf("%s  : %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		fails++;
}

/* Build a header the way the flash would hold it: inner magic at 0xf4,
 * serial at 0x1f4, CRC1 computed over [0, 0xa4). break_crc/magic make it a
 * header cspboot (and this parser) must reject. */
static uint8_t *mk_hdr(unsigned serial, int break_crc, int break_magic)
{
	static uint8_t hdr[3][ZX_SP_HDR_SIZE];
	static int n;
	uint8_t *h = hdr[n++ % 3];

	memset(h, 0, ZX_SP_HDR_SIZE);
	memcpy(h + ZX_SP_MAGIC_OFS, zx_sp_inner_magic, 16);
	wr32(h, ZX_SP_SERIAL_OFS, serial);
	if (!break_crc)
		wr32(h, ZX_SP_CRC_OFS, zx_sp_crc32(h, ZX_SP_CRC_COVER));
	if (break_magic)
		h[ZX_SP_MAGIC_OFS] ^= 0xff;
	return h;
}

static void run(const char *what, int have0, uint8_t *h0, int have1, uint8_t *h1,
		int want_active, int want_changed, uint32_t want_rootfs)
{
	struct zx_slotpart_in in;
	struct zx_slotpart_out out;

	memset(&in, 0, sizeof(in));
	in.base[0] = 0x00700000;
	in.base[1] = 0x02300000;
	in.fs_delta = 0x00400000;
	in.hdr_delta = 0x01400000;
	in.have_hdr[0] = have0;
	in.have_hdr[1] = have1;
	if (h0)
		memcpy(in.hdr[0], h0, ZX_SP_HDR_SIZE);
	if (h1)
		memcpy(in.hdr[1], h1, ZX_SP_HDR_SIZE);
	zx_slotpart_decide(&in, &out);
	printf("    (%s: active=%d changed=%d rootfs=%08x) %s\n", what,
	       out.active, out.changed, out.changed ? out.rootfs_ofs : 0,
	       out.note ? out.note : "");
	expect(out.active == want_active && out.changed == want_changed &&
	       (!want_changed || out.rootfs_ofs == want_rootfs), what);
}

int main(void)
{
	/* 1. both valid, slot 1 has the higher serial -> slot 1, no change */
	run("both valid, serial 1 > 0 -> slot1 unchanged", 1, mk_hdr(0x30, 0, 0),
	    1, mk_hdr(0x31, 0, 0), 1, 0, 0);
	/* 2. both valid, slot 0 higher -> slot 0, rootfs moves to 0xb00000 */
	run("both valid, serial 0 > 1 -> slot0, rootfs 0xb00000", 1, mk_hdr(0x31, 0, 0),
	    1, mk_hdr(0x30, 0, 0), 0, 1, 0x00b00000);
	/* 3. equal serials -> cspboot's tie goes to slot 0 */
	run("tie -> slot0 per cspboot", 1, mk_hdr(0x31, 0, 0),
	    1, mk_hdr(0x31, 0, 0), 0, 1, 0x00b00000);
	/* 4. slot 0 invalid, slot 1 valid -> slot 1 unchanged */
	run("slot0 CRC broken -> slot1 unchanged", 1, mk_hdr(0x31, 1, 0),
	    1, mk_hdr(0x30, 0, 0), 1, 0, 0);
	/* 5. slot 1 invalid, slot 0 valid -> slot 0 moved */
	run("slot1 magic broken -> slot0 moved", 1, mk_hdr(0x31, 0, 0),
	    1, mk_hdr(0x30, 1, 0), 0, 1, 0x00b00000);
	/* 6. both headers invalid -> keep the built-in layout */
	run("both invalid -> keep default", 1, mk_hdr(0x31, 1, 0),
	    1, mk_hdr(0x30, 0, 1), -1, 0, 0);
	/* 7. slot 0 unreadable (mtd_read failed), slot 1 valid -> slot 1 */
	run("slot0 unreadable -> slot1 unchanged", 0, NULL,
	    1, mk_hdr(0x30, 0, 0), 1, 0, 0);
	/* 8. slot 1 unreadable, slot 0 valid -> slot 0 moved */
	run("slot1 unreadable -> slot0 moved", 1, mk_hdr(0x31, 0, 0),
	    0, NULL, 0, 1, 0x00b00000);
	/* 9. both unreadable -> keep the built-in layout */
	run("both unreadable -> keep default", 0, NULL, 0, NULL, -1, 0, 0);
	/* 10. custom deltas are honoured (a vendor-geometry flash, say) */
	{
		struct zx_slotpart_in in;
		struct zx_slotpart_out out;

		memset(&in, 0, sizeof(in));
		in.base[0] = 0x00700000;
		in.base[1] = 0x02300000;
		in.fs_delta = 0x00340000;	/* vendor-style delta */
		in.hdr_delta = 0x01400000;
		in.have_hdr[0] = 1;
		memcpy(in.hdr[0], mk_hdr(0x40, 0, 0), ZX_SP_HDR_SIZE);
		zx_slotpart_decide(&in, &out);
		expect(out.active == 0 && out.changed == 1 &&
		       out.rootfs_ofs == 0x00a40000,
		       "custom fs-delta respected (0xa40000)");
	}
	/* 11. CRC1 coverage stops at 0xa4: flipping serial never invalidates */
	{
		uint8_t *h = mk_hdr(0x50, 0, 0);

		wr32(h, ZX_SP_SERIAL_OFS, 0x51);
		expect(zx_sp_header_valid(h) == 1,
			"serial rewrite does not invalidate CRC1 (matches cspboot)");
	}

	if (fails) {
		printf("%d FAILURES\n", fails);
		return 1;
	}
	printf("all slotpart decision cases passed\n");
	return 0;
}
