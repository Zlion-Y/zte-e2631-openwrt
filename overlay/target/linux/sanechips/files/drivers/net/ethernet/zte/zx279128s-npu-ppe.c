// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S vendor-native classifier (CLA) access and hash check.
 *
 * The CLA block lives at PP+0xC000. Its indirect window is the same one the
 * wired driver uses (command | table << 22 | read << 27, status bit0 idle,
 * data port), but this module adds the parts the flow-offload path never
 * needed: the vendor hash polynomial selector register, the exact CRC
 * placement used by the stock ACL builder, and a readback of a classifier
 * entry so the software model can be checked against the hardware.
 *
 * Register map recovered from the stock tm.ko claRegTable (28-byte entries
 * of {u16 rid, u32 kind, u32 base, u32 mask, u16 shift, u16 max, u32 stride}
 * with base in 4-byte units off the PP block):
 *
 *   rid 0  PP+0xC014  cmd       mask 0x0fffffff
 *   rid 1  PP+0xC018  status    bit0 idle
 *   rid 2  PP+0xC01C  data      stride 1, 17 words
 *   rid 3  PP+0xC080  config    mask 0x000fffff
 *   rid 12 PP+0xC090  hash poly mask 0x00ff00ff
 *   rid 13 PP+0xC094  outspace  mask 0x0000000f
 *
 * Hash poly register bit pairs, in order:
 *   [1:0] [3:2] [5:4] [7:6] = hash0..hash3 polynomial select
 *   [17:16] [19:18] [21:20] [23:22] = outer hash0..3 select
 * Select 0 is CRC-32 (0x04c11db7), 1 is CRC-32C (0x1edc6f41), 2 is
 * 0xf4acfb13 and 3 is 0x32583499; the tables in tm.ko match those four
 * polynomials in the usual MSB-first form.
 *
 * Classifier key (45 bytes, from tm_acl_fast_add_v4v6 / cla_acl_hash_addr_gen
 * and matching the wired driver's zx_ppe_slots()):
 *   bit 23..30  extract rule id (IPv4 TCP/UDP uses 0x98)
 *   bit 32      direction (0 upstream)
 *   bit 33..    seven 16-bit halfwords: proto, sip hi, sip lo, dip hi,
 *               dip lo, sport, dport
 * The hardware hashes that buffer byte 44 down to byte 0, and the vendor
 * side does the same by reversing it before the table CRC. Placement of a
 * new flow scans, in order, bank0 (0..255), bank1 (0x100..0x17f), table 4
 * (0x180..0x1bf), table 5 (0x1c0..0x1ff) and the eight entries of table 6.
 *
 * Everything that writes is behind zxnpu_ppe.enable=1 and documented as an
 * experiment; the checks that matter (poly readout, hash, entry readback)
 * are read-only.
 */

#include <linux/bits.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/inet.h>
#include <linux/io.h>
#include <linux/unaligned.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define ZXNPU_PP_BASE		0x92380000
#define ZXNPU_PP_SIZE		0x40000
#define ZXNPU_CLA_OFF		0xC000
#define ZXNPU_CLA_CMD		0x14
#define ZXNPU_CLA_STATUS	0x18
#define ZXNPU_CLA_DATA		0x1C
#define ZXNPU_CLA_CONFIG	0x80
#define ZXNPU_CLA_POLY		0x90
#define ZXNPU_CLA_OUTSPACE	0x94
#define ZXNPU_CLA_INDEX_MASK	GENMASK(21, 0)
#define ZXNPU_CLA_TABLE_SHIFT	22
#define ZXNPU_CLA_READ		BIT(27)
#define ZXNPU_CLA_WORDS		17

/* classifier hash banks, as in zx279128s-eth.h */
#define ZXNPU_CLA_HASH0		2	/* 256 entries */
#define ZXNPU_CLA_HASH1		3	/* 128 entries */
#define ZXNPU_ACL_SLOTS		520
#define ZXNPU_HASH0_SIZE	256
#define ZXNPU_PPE_RULE		0x98	/* IPv4 TCP/UDP extract rule id */
#define ZXNPU_PPE_INDEX		9

static const u32 zxnpu_hash_polys[4] = {
	0x04c11db7, 0x1edc6f41, 0xf4acfb13, 0x32583499,
};

static bool zxnpu_ppe_enable;
module_param_named(enable, zxnpu_ppe_enable, bool, 0644);
MODULE_PARM_DESC(enable, "allow classifier table writes from /proc/zx-npu-ppe/cla");

struct zxnpu_ppe {
	struct device *dev;
	void __iomem *pp;
	struct proc_dir_entry *proc;
	struct mutex lock;
	u32 crc[4][256];
	char out[8192];
};

static struct zxnpu_ppe *zxnpu_ppe_singleton;

static void zxnpu_ppe_out(struct zxnpu_ppe *ppe, const char *fmt, ...)
{
	va_list args;
	size_t len = strlen(ppe->out);

	if (len >= sizeof(ppe->out) - 1)
		return;
	va_start(args, fmt);
	vsnprintf(ppe->out + len, sizeof(ppe->out) - len, fmt, args);
	va_end(args);
}

static void zxnpu_crc_init(struct zxnpu_ppe *ppe)
{
	int p, i, b;

	for (p = 0; p < 4; p++) {
		for (i = 0; i < 256; i++) {
			u32 c = (u32)i << 24;

			for (b = 0; b < 8; b++)
				c = (c & BIT(31)) ? (c << 1) ^ zxnpu_hash_polys[p]
						   : c << 1;
			ppe->crc[p][i] = c;
		}
	}
}

/*
 * MSB-first CRC over the 45-byte key with the buffer walked last byte
 * first; the result is truncated to 16 bits by the stock code.
 */
static u16 zxnpu_hash(const u32 *tab, const u8 *key, int len)
{
	u32 crc = 0;
	int i;

	for (i = len - 1; i >= 0; i--)
		crc = tab[(crc >> 24) ^ key[i]] ^ (crc << 8);
	return crc & 0xffff;
}

static void zxnpu_put_bits(u8 *buf, int pos, int width, u32 val)
{
	int i;

	for (i = 0; i < width; i++, pos++)
		if (val & BIT(i))
			buf[pos / 8] |= BIT(pos % 8);
}

static void zxnpu_acl_key(u16 halfwords[7], u8 key[45])
{
	int i;

	memset(key, 0, 45);
	zxnpu_put_bits(key, 23, 8, ZXNPU_PPE_RULE);
	for (i = 0; i < 7; i++)
		zxnpu_put_bits(key, 33 + 16 * i, 16, halfwords[i]);
}

static u16 zxnpu_acl_slot(struct zxnpu_ppe *ppe, int poly, const u8 *key)
{
	return zxnpu_hash(ppe->crc[poly], key, 45);
}

static int zxnpu_cla_wait(struct zxnpu_ppe *ppe)
{
	u32 value;

	return readl_poll_timeout(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_STATUS,
				  value, value & BIT(0), 10, 1000);
}

static int zxnpu_cla_cmd(struct zxnpu_ppe *ppe, u32 table, u32 index,
			 bool read)
{
	u32 cmd;

	if (table > 9 || index > ZXNPU_CLA_INDEX_MASK)
		return -EINVAL;
	if (zxnpu_cla_wait(ppe))
		return -ETIMEDOUT;
	cmd = (index & ZXNPU_CLA_INDEX_MASK) | (table << ZXNPU_CLA_TABLE_SHIFT);
	if (read)
		cmd |= ZXNPU_CLA_READ;
	writel(cmd, ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_CMD);
	return 0;
}

/*
 * The classifier reloads the data window when word 0 arrives, so word 0 is
 * written last; the status can still read idle right after the command.
 */
static int zxnpu_cla_write(struct zxnpu_ppe *ppe, u32 table, u32 index,
			   const u32 *data, int n)
{
	int i, ret;

	ret = zxnpu_cla_cmd(ppe, table, index, false);
	if (ret)
		return ret;
	for (i = ZXNPU_CLA_WORDS - 1; i >= 0; i--)
		writel(i < n ? data[i] : 0,
		       ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_DATA + i * 4);
	for (i = 0; i < 64 && (readl(ppe->pp + ZXNPU_CLA_OFF +
				     ZXNPU_CLA_STATUS) & BIT(0)); i++)
		;
	return zxnpu_cla_wait(ppe);
}

static int zxnpu_cla_read(struct zxnpu_ppe *ppe, u32 table, u32 index,
			  u32 *data, int n)
{
	int i, ret;

	ret = zxnpu_cla_cmd(ppe, table, index, true);
	if (ret)
		return ret;
	for (i = 0; i < 64 && (readl(ppe->pp + ZXNPU_CLA_OFF +
				     ZXNPU_CLA_STATUS) & BIT(0)); i++)
		;
	ret = zxnpu_cla_wait(ppe);
	if (ret)
		return ret;
	for (i = 0; i < n; i++)
		data[i] = readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_DATA + i * 4);
	return 0;
}

/*
 * Slot -> (table, index), matching cla_set_hash_table: 0..255 table 2,
 * 0x100..0x17f table 3, 0x180..0x1bf table 4, 0x1c0..0x1ff table 5,
 * 0x200..0x207 table 6.
 */
static int zxnpu_slot_map(u32 slot, u32 *table, u32 *index)
{
	if (slot >= ZXNPU_ACL_SLOTS)
		return -EINVAL;
	if (slot >= 512) {
		*table = 6;
		*index = slot - 512;
	} else if (slot >= 448) {
		*table = 5;
		*index = slot - 448;
	} else if (slot >= 384) {
		*table = 4;
		*index = slot - 384;
	} else if (slot >= 256) {
		*table = 3;
		*index = slot - 256;
	} else {
		*table = 2;
		*index = slot;
	}
	return 0;
}

static int zxnpu_entry_read(struct zxnpu_ppe *ppe, u32 slot, u32 w[15])
{
	u32 table, index;

	if (zxnpu_slot_map(slot, &table, &index))
		return -EINVAL;
	return zxnpu_cla_read(ppe, table, index, w, 15);
}

static int zxnpu_entry_write(struct zxnpu_ppe *ppe, u32 slot, const u32 w[15])
{
	u32 table, index;

	if (zxnpu_slot_map(slot, &table, &index))
		return -EINVAL;
	return zxnpu_cla_write(ppe, table, index, w, 15);
}

static void zxnpu_ppe_hex(struct zxnpu_ppe *ppe, const u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		zxnpu_ppe_out(ppe, "%02x", buf[i]);
	zxnpu_ppe_out(ppe, "\n");
}

static int zxnpu_parse_hex(const char *s, u8 *out, int len)
{
	int i, hi, lo;
	char c;

	for (i = 0; i < len; i++) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!s[0] || !s[1])
			return -EINVAL;
		c = s[0];
		hi = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		c = s[1];
		lo = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (hi << 4) | lo;
		s += 2;
	}
	return 0;
}

static void zxnpu_dump_poly(struct zxnpu_ppe *ppe)
{
	u32 poly = readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_POLY);
	u32 space = readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_OUTSPACE);
	int i;

	zxnpu_ppe_out(ppe, "cla_poly(%08x):", poly);
	for (i = 0; i < 4; i++)
		zxnpu_ppe_out(ppe, " hash%d=%u", i, (poly >> (2 * i)) & 3);
	for (i = 0; i < 4; i++)
		zxnpu_ppe_out(ppe, " outer%d=%u", i, (poly >> (16 + 2 * i)) & 3);
	zxnpu_ppe_out(ppe, "\ncla_outspace(%08x): outer_hash_num=%u outer_space_sel=%u\n",
		      space, space & 3, (space >> 2) & 3);
}

static void zxnpu_ppe_dump(struct zxnpu_ppe *ppe)
{
	zxnpu_ppe_out(ppe, "CLA config (read-only dump)\n");
	zxnpu_ppe_out(ppe, "c080=%08x c088=%08x c090=%08x c094=%08x\n",
		      readl(ppe->pp + ZXNPU_CLA_OFF + 0x80),
		      readl(ppe->pp + ZXNPU_CLA_OFF + 0x88),
		      readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_POLY),
		      readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_OUTSPACE));
	zxnpu_ppe_out(ppe, "c0c8=%08x c0cc=%08x c0d0=%08x status=%08x\n",
		      readl(ppe->pp + ZXNPU_CLA_OFF + 0xc8),
		      readl(ppe->pp + ZXNPU_CLA_OFF + 0xcc),
		      readl(ppe->pp + ZXNPU_CLA_OFF + 0xd0),
		      readl(ppe->pp + ZXNPU_CLA_OFF + ZXNPU_CLA_STATUS));
	zxnpu_dump_poly(ppe);
}

/*
 * slots <proto> <sip> <dip> <sport> <dport>
 * Prints the 45-byte key, the 16-bit hash of all four polynomials, the two
 * placement candidates used by the wired offload, and what the classifier
 * currently holds in those two slots.
 */
static void zxnpu_ppe_slots(struct zxnpu_ppe *ppe, const char *args)
{
	struct {
		unsigned int proto;
		char sip[16], dip[16];
		unsigned int sport, dport;
	} in;
	u16 hw[7];
	u8 sipb[4], dipb[4], key[45];
	u32 entry[15];
	u16 hash[4], slot[2];
	int p, i, ret;

	if (sscanf(args, "%u %15s %15s %u %u", &in.proto, in.sip, in.dip,
		   &in.sport, &in.dport) != 5) {
		zxnpu_ppe_out(ppe, "usage: slots <proto> <sip> <dip> <sport> <dport>\n");
		return;
	}
	if (!in4_pton(in.sip, -1, sipb, -1, NULL) ||
	    !in4_pton(in.dip, -1, dipb, -1, NULL)) {
		zxnpu_ppe_out(ppe, "bad address\n");
		return;
	}
	hw[0] = in.proto;
	hw[1] = (sipb[0] << 8) | sipb[1];
	hw[2] = (sipb[2] << 8) | sipb[3];
	hw[3] = (dipb[0] << 8) | dipb[1];
	hw[4] = (dipb[2] << 8) | dipb[3];
	hw[5] = in.sport;
	hw[6] = in.dport;
	zxnpu_acl_key(hw, key);

	zxnpu_ppe_out(ppe, "key: ");
	zxnpu_ppe_hex(ppe, key, 45);
	for (p = 0; p < 4; p++)
		hash[p] = zxnpu_acl_slot(ppe, p, key);
	zxnpu_ppe_out(ppe, "hash: crc32=%04x crc32c=%04x p2=%04x p3=%04x\n",
		      hash[0], hash[1], hash[2], hash[3]);
	slot[0] = hash[0] & 0xff;
	slot[1] = ZXNPU_HASH0_SIZE + (hash[1] & 0x7f);
	zxnpu_ppe_out(ppe, "slot: bank0=%u bank1=%u\n", slot[0], slot[1]);
	for (i = 0; i < 2; i++) {
		ret = zxnpu_entry_read(ppe, slot[i], entry);
		if (ret) {
			zxnpu_ppe_out(ppe, "entry[%u]: read failed (%d)\n",
				      slot[i], ret);
			continue;
		}
		zxnpu_ppe_out(ppe, "entry[%u]:", slot[i]);
		for (p = 0; p < 15; p++)
			zxnpu_ppe_out(ppe, " %08x", entry[p]);
		zxnpu_ppe_out(ppe, "\n");
	}
}

static ssize_t zxnpu_ppe_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct zxnpu_ppe *ppe = zxnpu_ppe_singleton;
	ssize_t ret;

	if (!ppe)
		return -ENODEV;
	mutex_lock(&ppe->lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, ppe->out,
				      strlen(ppe->out));
	mutex_unlock(&ppe->lock);
	return ret;
}

static ssize_t zxnpu_ppe_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct zxnpu_ppe *ppe = zxnpu_ppe_singleton;
	char cmd[512];
	unsigned int table, index, value, slot;
	u32 got[ZXNPU_CLA_WORDS];
	u8 raw[60];
	int ret = 0;

	if (!ppe)
		return -ENODEV;
	if (count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	mutex_lock(&ppe->lock);
	memset(ppe->out, 0, sizeof(ppe->out));

	if (!strncmp(cmd, "dump", 4)) {
		zxnpu_ppe_dump(ppe);
		ret = count;
		goto out;
	}
	if (!strncmp(cmd, "poly", 4)) {
		zxnpu_dump_poly(ppe);
		ret = count;
		goto out;
	}
	if (!strncmp(cmd, "slots ", 6)) {
		zxnpu_ppe_slots(ppe, cmd + 6);
		ret = count;
		goto out;
	}
	if (!strncmp(cmd, "entry ", 6)) {
		if (sscanf(cmd + 6, "%u", &slot) != 1 ||
		    zxnpu_entry_read(ppe, slot, got)) {
			zxnpu_ppe_out(ppe, "entry <slot 0..519> failed\n");
			ret = -EINVAL;
			goto out;
		}
		zxnpu_ppe_out(ppe, "entry[%u]:", slot);
		for (index = 0; index < 15; index++)
			zxnpu_ppe_out(ppe, " %08x", got[index]);
		zxnpu_ppe_out(ppe, "\n");
		ret = count;
		goto out;
	}

	if (!zxnpu_ppe_enable) {
		zxnpu_ppe_out(ppe, "disabled; set zxnpu_ppe.enable=1 first\n");
		ret = -EACCES;
		goto out;
	}

	if (sscanf(cmd, "read %u %u", &table, &index) == 2) {
		if (zxnpu_cla_read(ppe, table, index, got, 4))
			zxnpu_ppe_out(ppe, "read table=%u index=%u failed\n",
				      table, index);
		else
			zxnpu_ppe_out(ppe, "cla[%u][%u]=%08x %08x %08x %08x\n",
				      table, index, got[0], got[1], got[2],
				      got[3]);
		ret = count;
		goto out;
	}
	if (sscanf(cmd, "write %u %u %i", &table, &index, &value) == 3) {
		u32 w[ZXNPU_CLA_WORDS] = { value };

		if (zxnpu_cla_write(ppe, table, index, w, 1))
			zxnpu_ppe_out(ppe, "write table=%u index=%u failed\n",
				      table, index);
		else
			zxnpu_ppe_out(ppe, "cla[%u][%u] <- %08x\n", table, index,
				      value);
		ret = count;
		goto out;
	}
	if (!strncmp(cmd, "entryw ", 7)) {
		char *hex = strchr(cmd + 7, ' ');
		u32 w[15];

		if (sscanf(cmd + 7, "%u", &slot) != 1 || !hex ||
		    zxnpu_parse_hex(hex + 1, raw, 60)) {
			zxnpu_ppe_out(ppe, "usage: entryw <slot> <60 hex bytes>\n");
			ret = -EINVAL;
			goto out;
		}
		for (index = 0; index < 15; index++)
			w[index] = get_unaligned_le32(raw + 4 * index);
		if (zxnpu_entry_write(ppe, slot, w))
			zxnpu_ppe_out(ppe, "entryw %u failed\n", slot);
		else
			zxnpu_ppe_out(ppe, "entryw %u written\n", slot);
		ret = count;
		goto out;
	}
	zxnpu_ppe_out(ppe, "commands: dump | poly | slots <proto> <sip> <dip> <sport> <dport> | entry <slot> | read <table> <index> | write <table> <index> <value> | entryw <slot> <60 hex bytes>\n");
	ret = count;
out:
	mutex_unlock(&ppe->lock);
	return ret;
}

static const struct proc_ops zxnpu_ppe_ops = {
	.proc_read = zxnpu_ppe_read,
	.proc_write = zxnpu_ppe_write,
	.proc_lseek = default_llseek,
};

static int zxnpu_ppe_probe(struct platform_device *pdev)
{
	struct zxnpu_ppe *ppe;
	struct proc_dir_entry *dir;

	ppe = devm_kzalloc(&pdev->dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return -ENOMEM;
	ppe->dev = &pdev->dev;
	ppe->pp = devm_ioremap(&pdev->dev, ZXNPU_PP_BASE, ZXNPU_PP_SIZE);
	if (!ppe->pp)
		return -ENOMEM;
	mutex_init(&ppe->lock);
	zxnpu_crc_init(ppe);
	platform_set_drvdata(pdev, ppe);
	zxnpu_ppe_singleton = ppe;

	dir = proc_mkdir("zx-npu-ppe", NULL);
	if (dir) {
		if (!proc_create("cla", 0644, dir, &zxnpu_ppe_ops)) {
			proc_remove(dir);
			dir = NULL;
		}
	}
	ppe->proc = dir;
	dev_info(&pdev->dev, "CLA/hash access ready (enable=%d)\n",
		 zxnpu_ppe_enable);
	return 0;
}

static void zxnpu_ppe_remove(struct platform_device *pdev)
{
	struct zxnpu_ppe *ppe = platform_get_drvdata(pdev);

	if (ppe->proc)
		proc_remove(ppe->proc);
	zxnpu_ppe_singleton = NULL;
}

static const struct of_device_id zxnpu_ppe_of_match[] = {
	{ .compatible = "zte,zx279128s-npu-ppe" },
	{ }
};
MODULE_DEVICE_TABLE(of, zxnpu_ppe_of_match);

static struct platform_driver zxnpu_ppe_driver = {
	.probe = zxnpu_ppe_probe,
	.remove = zxnpu_ppe_remove,
	.driver = {
		.name = "zx279128s-npu-ppe",
		.of_match_table = zxnpu_ppe_of_match,
	},
};
module_platform_driver(zxnpu_ppe_driver);

MODULE_DESCRIPTION("ZTE ZX279128S vendor-native classifier access and hash check");
MODULE_LICENSE("GPL");
