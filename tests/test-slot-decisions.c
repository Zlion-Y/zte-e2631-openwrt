/* Offline unit tests for the P0-0 slot decisions in e2631-fwslot.c.
 *
 * The guards and the identity classifier are pure functions over slot state,
 * so the whole checklist matrix (slot states x judgment paths) runs on the
 * host with no flash device at all: the harness includes the tool's source
 * and drives install_guard()/upgrade_guard()/header_identity() directly.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -o test-slot-decisions \
 *       tests/test-slot-decisions.c && ./test-slot-decisions
 */
#include <stdio.h>
#include <string.h>

#define main fwslot_unused_main
#include "../overlay/package/firmware/e2631-fwslot/src/e2631-fwslot.c"
#undef main

static int fails;

static void expect(int cond, const char *what)
{
	printf("%s  : %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		fails++;
}

/* expect_refused / expect_allowed name the judgment, not just the bool, so a
 * failure says which path broke. */
static void expect_refused(const char *why, const char *what)
{
	expect(why != NULL, what);
}

static void expect_allowed(const char *why, const char *what)
{
	expect(why == NULL, what);
}

static struct slot_state mk(enum slot_identity id, int intact, int known)
{
	struct slot_state s;

	memset(&s, 0, sizeof(s));
	s.known = known;
	s.identity = id;
	s.intact = intact;
	return s;
}

/* Build a header the way classifier sees it: inner magic, version string,
 * valid CRC1 - or deliberately broken variants for ID_EMPTY. */
static uint8_t *hdr_for(enum slot_identity id, int break_crc)
{
	static uint8_t hdr[4][VER_HDR_SIZE];
	static int n;
	uint8_t *h = hdr[n++ % 4];

	memset(h, 0, VER_HDR_SIZE);
	memcpy(h + H_INNER_MAGIC, inner_magic, 16);
	switch (id) {
	case ID_OURS:
		memcpy(h + H_VERSION, "OpenWrt v1.4", 12);
		break;
	case ID_VENDOR:
		memcpy(h + H_VERSION, "V1.0.0.8B8.8000", 15);
		break;
	case ID_UNKNOWN:
		memcpy(h + H_VERSION, "SomethingElse 1.0", 17);
		break;
	default:
		break;
	}
	wr32(h, H_CRC, break_crc ? 0x12345678 : crc32(h, H_CRC));
	return h;
}

int main(void)
{
	struct slot_state vendor = mk(ID_VENDOR, 1, 1);
	struct slot_state vendor_broken = mk(ID_VENDOR, 0, 1);
	struct slot_state ours = mk(ID_OURS, 1, 1);
	struct slot_state empty = mk(ID_EMPTY, 0, 1);
	struct slot_state unknown = mk(ID_UNKNOWN, 1, 1);
	struct slot_state unreachable = mk(ID_EMPTY, 0, 0);

	/* ---- install: writing the OTHER slot ---------------------------- */
	expect_refused(install_guard(&vendor, false),
		"install over vendor, no flag -> refused");
	expect_allowed(install_guard(&vendor, true),
		"install over vendor, --replace-other-system -> allowed");
	expect_allowed(install_guard(&empty, false),
		"install into an empty slot -> allowed (nothing destroyed)");
	expect_allowed(install_guard(&ours, false),
		"install over our own previous build -> allowed");
	expect_refused(install_guard(&unknown, false),
		"install over an unidentified image -> refused");
	expect_allowed(install_guard(&unknown, true),
		"install over an unidentified image with the flag -> allowed");

	/* ---- upgrade: rewriting the RUNNING slot in place ---------------- */
	expect_allowed(upgrade_guard(&vendor, false),
		"upgrade with an intact vendor fallback -> allowed");
	expect_refused(upgrade_guard(&vendor_broken, false),
		"upgrade with a damaged vendor fallback -> refused");
	expect_refused(upgrade_guard(&ours, false),
		"upgrade with our own image in the other slot -> refused");
	expect_refused(upgrade_guard(&empty, false),
		"upgrade with an empty other slot -> refused");
	expect_refused(upgrade_guard(&unreachable, false),
		"upgrade with an unreadable other slot -> refused");
	expect_allowed(upgrade_guard(&vendor_broken, true),
		"upgrade over a damaged fallback with --no-fallback-check -> allowed");
	expect_allowed(upgrade_guard(&ours, true),
		"upgrade over a non-vendor fallback with --no-fallback-check -> allowed");

	/* ---- header_identity: the content judgment itself ----------------- */
	expect(header_identity(hdr_for(ID_OURS, 0)) == ID_OURS,
		"identity: \"OpenWrt ...\" -> ours");
	expect(header_identity(hdr_for(ID_VENDOR, 0)) == ID_VENDOR,
		"identity: \"V1.0.0.*\" -> vendor");
	expect(header_identity(hdr_for(ID_UNKNOWN, 0)) == ID_UNKNOWN,
		"identity: a valid header of another provenance -> unknown");
	expect(header_identity(hdr_for(ID_VENDOR, 1)) == ID_EMPTY,
		"identity: broken CRC1 -> empty");
	{
		uint8_t h[VER_HDR_SIZE];

		memset(h, 0, sizeof(h));
		memcpy(h + H_VERSION, "OpenWrt v1.4", 12);
		wr32(h, H_CRC, crc32(h, H_CRC));
		expect(header_identity(h) == ID_EMPTY,
			"identity: inner magic missing -> empty");
	}

	if (fails) {
		printf("%d FAILURES\n", fails);
		return 1;
	}
	printf("all slot-decision cases passed\n");
	return 0;
}
