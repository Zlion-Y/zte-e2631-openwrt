// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S vendor-native MT7916 (CSP_WLAN) G3 skeleton.
 *
 * This driver deliberately binds only the companion PCIe function
 * 14c3:790a, which mt76 leaves unbound in the current single-HIF image. It is
 * a probe/firmware audit stage, not an AP driver: with enable=0 it refuses the
 * device, and even when enabled it only enables PCI, maps the BARs, reads
 * config space and checks the vendor firmware files. No MT7916 register is
 * written yet.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define ZXNPU_WLAN_VENDOR	0x14c3
#define ZXNPU_WLAN_DEVICE	0x790a

/* MT7916 MAC CR space starts at 0x18000000 and BAR0 maps it 1:1. */
#define ZXNPU_CR_BASE		0x18000000u
#define ZXNPU_CR(off)		((off) - ZXNPU_CR_BASE)
#define ZXNPU_WFDMA_GLO_CFG	0x18024208
#define ZXNPU_WFDMA_INT_ENA	0x1802420c
#define ZXNPU_WFDMA_RING_LO	0x18024640	/* six rings 0x18024640-0x18024650 */
#define ZXNPU_WFDMA_RING_HI	0x18024680	/* six rings 0x18024680-0x18024694 */
#define ZXNPU_WFDMA_PCIE_CFG	0x180242f4
#define ZXNPU_WFDMA_PCIE_CFG2	0x180282f4
#define ZXNPU_WFDMA_TX_BASE	0x1802702c
#define ZXNPU_WFDMA_RX_BASE	0x18027030

static bool zxnpu_wlan_enable;
module_param_named(enable, zxnpu_wlan_enable, bool, 0644);
MODULE_PARM_DESC(enable, "probe the 790a companion function and audit firmware files");

static bool zxnpu_wlan_commit;
module_param_named(commit, zxnpu_wlan_commit, bool, 0644);
MODULE_PARM_DESC(commit, "allow the WFDMA init plan to actually write registers");

struct zxnpu_wlan {
	struct pci_dev *pdev;
	void __iomem *bar[3];
	struct proc_dir_entry *proc;
	struct mutex lock;
	void *mcu_ring;		/* 64 x 36 self-describing descriptors */
	dma_addr_t mcu_ring_dma;
	void *mcu_buf;		/* 1 KiB command buffer */
	dma_addr_t mcu_buf_dma;
	u32 mcu_cidx;
	void *rx_ring;		/* 32 x 36 descriptors */
	dma_addr_t rx_ring_dma;
	void *rx_buf;		/* 32 x 1700 buffers with 16B RXD */
	dma_addr_t rx_buf_dma;
	u32 rx_cidx;
	char status[4096];
};

static struct zxnpu_wlan *zxnpu_wlan_singleton;

static const char * const zxnpu_fw_names[] = {
	"zte/mt7916/WIFI_RAM_CODE_MT7916.bin",
	"zte/mt7916/7916_WACPU_RAM_CODE_release.bin",
	"zte/mt7916/mt7916_patch_e1_hdr.bin",
	"zte/mt7916/MT7916_EEPROM.bin",
};

static void zxnpu_wlan_append(struct zxnpu_wlan *w, const char *fmt, ...)
{
	va_list args;
	size_t len = strlen(w->status);

	if (len >= sizeof(w->status) - 1)
		return;
	va_start(args, fmt);
	vsnprintf(w->status + len, sizeof(w->status) - len, fmt, args);
	va_end(args);
}

static void zxnpu_wlan_audit_firmware(struct zxnpu_wlan *w)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(zxnpu_fw_names); i++) {
		const struct firmware *fw;
		int ret;

		ret = request_firmware_direct(&fw, zxnpu_fw_names[i],
					      &w->pdev->dev);
		if (ret) {
			zxnpu_wlan_append(w, "fw %s: missing (%d)\n",
					  zxnpu_fw_names[i], ret);
			continue;
		}
		zxnpu_wlan_append(w, "fw %s: %zu bytes crc32=%08x\n",
				  zxnpu_fw_names[i], fw->size,
				  crc32_le(~0U, fw->data, fw->size) ^ ~0U);
		release_firmware(fw);
	}
}

static u32 zxnpu_cr_read(struct zxnpu_wlan *w, u32 cr)
{
	u32 off = ZXNPU_CR(cr);

	if (!w->bar[0] || (resource_size_t)off + 4 > pci_resource_len(w->pdev, 0))
		return 0;
	return readl(w->bar[0] + off);
}

static void zxnpu_cr_write(struct zxnpu_wlan *w, u32 cr, u32 val)
{
	u32 off = ZXNPU_CR(cr);

	if (!w->bar[0] || (resource_size_t)off + 4 > pci_resource_len(w->pdev, 0))
		return;
	writel(val, w->bar[0] + off);
}

static void zxnpu_wlan_probe_regs(struct zxnpu_wlan *w)
{
	static const u32 ids[] = {
		0x18000000, 0x18000008, 0x18000010, 0x18000028,
		ZXNPU_WFDMA_GLO_CFG, ZXNPU_WFDMA_INT_ENA,
		ZXNPU_WFDMA_TX_BASE, ZXNPU_WFDMA_RX_BASE,
		ZXNPU_WFDMA_PCIE_CFG, ZXNPU_WFDMA_PCIE_CFG2,
	};
	int i;

	zxnpu_wlan_append(w, "cr probe (bar0 len %llu):\n",
			  (unsigned long long)pci_resource_len(w->pdev, 0));
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		zxnpu_wlan_append(w, "  cr[%08x]=%08x\n", ids[i],
				  zxnpu_cr_read(w, ids[i]));
}

/*
 * The stock hif_init_WFDMA (mt7916.ko 0x195b6c) writes a fixed ring credit
 * table: 0x18024680..0x18024694 take (1..6) << 22 | 4 and 0x18024640..
 * 0x18024650 take (6..10) << 22 | 4, then 0x1802420c = ~0 and the PCIe
 * window register 0x180242f4 (0x180282f4 on the second MAC generation)
 * takes 0x811c0000. The global config sets bit28 and bit21 and clears
 * bit27, and the TX base register clears 0x1fc0000|0x20000 and sets
 * 0x2800000. This function only prints that plan unless commit=1 and the
 * caller asks for it.
 */
/*
 * WFDMA ring register blocks, from chip_show_dma_info (mt7916.ko 0x1A9188):
 * TX rings 0..4 live at 0x18024400 + 0x10*n and RX rings 0..5 at
 * 0x18024500 + 0x10*n; each block is BASE/CNT/CPU_IDX/DMA_IDX. The second
 * MAC generation mirrors the same blocks at +0x4000 (0x1802_84xx/85xx).
 */
#define ZXNPU_WFDMA_TX_RING0	0x18024400
#define ZXNPU_WFDMA_RX_RING0	0x18024500
#define ZXNPU_WFDMA_TX_RINGS	5
#define ZXNPU_WFDMA_RX_RINGS	6
#define ZXNPU_WFDMA_BAND1	0x4000

static void zxnpu_wlan_dump_ring(struct zxnpu_wlan *w, const char *tag,
				 u32 base)
{
	zxnpu_wlan_append(w, "  %s %08x: base=%08x cnt=%u cidx=%u didx=%u\n",
			  tag, base,
			  zxnpu_cr_read(w, base),
			  zxnpu_cr_read(w, base + 4),
			  zxnpu_cr_read(w, base + 8),
			  zxnpu_cr_read(w, base + 12));
}

static void zxnpu_wlan_rings(struct zxnpu_wlan *w)
{
	int i;

	zxnpu_wlan_append(w, "wfdma rings (read-only):\n");
	for (i = 0; i < ZXNPU_WFDMA_TX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "tx",
				     ZXNPU_WFDMA_TX_RING0 + 0x10 * i);
	for (i = 0; i < ZXNPU_WFDMA_RX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "rx",
				     ZXNPU_WFDMA_RX_RING0 + 0x10 * i);
	zxnpu_wlan_append(w, "wfdma rings, +0x4000 mirror:\n");
	for (i = 0; i < ZXNPU_WFDMA_TX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "tx",
				     ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_BAND1 +
				     0x10 * i);
	for (i = 0; i < ZXNPU_WFDMA_RX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "rx",
				     ZXNPU_WFDMA_RX_RING0 + ZXNPU_WFDMA_BAND1 +
				     0x10 * i);
}

/*
 * The vendor doorbell for host->MCU commands is a write to 0x54000108
 * (pci_trigger_int_to_mcu, mt7916.ko 0x191DA0). The 0x54000000 window is
 * not part of the 0x1802_xxxx CR space, so find out which mapped BAR owns
 * it: read offset 0/8/0x108 of every memory BAR and print what is there.
 * Read-only; used to pin the second register window before any send path.
 */
static void zxnpu_wlan_mcuwin(struct zxnpu_wlan *w)
{
	int i;

	zxnpu_wlan_append(w, "mcu window probe (read-only):\n");
	for (i = 0; i < 3; i++) {
		resource_size_t len = pci_resource_len(w->pdev, i);
		unsigned long flags = pci_resource_flags(w->pdev, i);

		if (!w->bar[i] || !(flags & IORESOURCE_MEM) || len < 0x10c) {
			zxnpu_wlan_append(w, "  bar%d: unmapped/short (len %llu flags %lx)\n",
					  i, (unsigned long long)len, flags);
			continue;
		}
		zxnpu_wlan_append(w, "  bar%d: +0=%08x +8=%08x +0x108=%08x\n", i,
				  readl(w->bar[i]),
				  readl(w->bar[i] + 8),
				  readl(w->bar[i] + 0x108));
	}
}

/*
 * Minimal host->MCU attempt, built on what the stock driver shows:
 *
 *  - the ring blocks are BASE/CNT/CPU_IDX/DMA_IDX at +0/+4/+8/+0xC
 *    (mt_asic_init_txrx_ring, mt7916.ko 0x1E0270);
 *  - an MCU command is an skb whose head is a 24-byte Andes header
 *    (type at +2, cmd at +4) followed by the payload, and the ring
 *    descriptor is the same 36-byte shape as the RX descriptors:
 *    [0]=16, [4]=current VA, [8]=end VA, [12]=skb VA, [24]=len,
 *    [28]=next (pci_kick_out_cmd_msg, 0x1E19C0);
 *  - the WFDMA data rings are 0x18024400 + 0x10*n; ring 4 is the last
 *    one the stock dumper covers and is the MTK MCU ring convention.
 *
 * This is a hypothesis, not a verified sequence: it is inert unless
 * enable=1 and commit=1, and it only ever touches ring 4.
 */
#define ZXNPU_MCU_RING_NR	4
#define ZXNPU_MCU_RING_REG	(ZXNPU_WFDMA_TX_RING0 + 0x10 * ZXNPU_MCU_RING_NR)
#define ZXNPU_MCU_DESC_SIZE	36
#define ZXNPU_MCU_RING_DEPTH	64
#define ZXNPU_MCU_BUF_SIZE	4096
#define ZXNPU_MCU_TYPE_PATCH	0x10
#define ZXNPU_MCU_CMD_PATCH	0x17

static void zxnpu_mcu_fill_desc(struct zxnpu_wlan *w, u32 slot, u32 buf_dma,
				u32 len)
{
	u8 *d = (u8 *)w->mcu_ring + slot * ZXNPU_MCU_DESC_SIZE;
	u32 next = (slot + 1) % ZXNPU_MCU_RING_DEPTH;

	memset(d, 0, ZXNPU_MCU_DESC_SIZE);
	put_unaligned_le32(16, d + 0);
	put_unaligned_le32(buf_dma, d + 4);
	put_unaligned_le32(buf_dma + len, d + 8);
	put_unaligned_le32(buf_dma, d + 12);
	put_unaligned_le32(len, d + 24);
	put_unaligned_le32(w->mcu_ring_dma + next * ZXNPU_MCU_DESC_SIZE, d + 28);
}

static int zxnpu_mcu_submit(struct zxnpu_wlan *w, const u8 *msg, int len,
			  bool commit, bool verbose)
{
	u32 slot, didx, prev;

	if (!w->mcu_ring || !w->mcu_buf || len <= 0 || len > ZXNPU_MCU_BUF_SIZE)
		return -EINVAL;
	slot = w->mcu_cidx % ZXNPU_MCU_RING_DEPTH;
	memset(w->mcu_buf, 0, ZXNPU_MCU_BUF_SIZE);
	memcpy(w->mcu_buf, msg, len);
	zxnpu_mcu_fill_desc(w, slot, (u32)w->mcu_buf_dma, len);
	dma_wmb();

	prev = zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 12);
	if (verbose)
		zxnpu_wlan_append(w, "  submit slot=%u len=%d: reg %08x base=%08x cnt=%u cidx=%u didx=%u\n",
				  slot, len, ZXNPU_MCU_RING_REG,
				  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG),
				  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 4),
				  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 8),
				  prev);
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to write\n");
		return 0;
	}

	{
		u32 glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);

		/* hif_set_WFDMA(2): WFDMA_EN | TX DMA EN */
		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x5);
	}
	zxnpu_cr_write(w, ZXNPU_MCU_RING_REG, (u32)w->mcu_ring_dma);
	zxnpu_cr_write(w, ZXNPU_MCU_RING_REG + 4, ZXNPU_MCU_RING_DEPTH);
	w->mcu_cidx++;
	zxnpu_cr_write(w, ZXNPU_MCU_RING_REG + 8, w->mcu_cidx);
	if (!readl_poll_timeout(w->bar[0] + ZXNPU_CR(ZXNPU_MCU_RING_REG + 12),
				didx, (didx % ZXNPU_MCU_RING_DEPTH) ==
				(w->mcu_cidx % ZXNPU_MCU_RING_DEPTH), 100,
				50000))
		zxnpu_wlan_append(w, "  didx advanced to %u\n", didx);
	else
		zxnpu_wlan_append(w, "  didx still %u (last was %u)\n",
				  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 12), prev);
	return 0;
}

static void zxnpu_wlan_mcu_send(struct zxnpu_wlan *w, bool commit)
{
	u8 msg[24 + 4];

	zxnpu_wlan_append(w, "mcu-send PatchSemGet (type %02x cmd %02x, ring %d): %s\n",
			  ZXNPU_MCU_TYPE_PATCH, ZXNPU_MCU_CMD_PATCH,
			  ZXNPU_MCU_RING_NR, commit ? "COMMIT" : "dry run");
	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(4, msg + 0);
	msg[2] = ZXNPU_MCU_TYPE_PATCH;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	put_unaligned_le32(0, msg + 24);
	zxnpu_mcu_submit(w, msg, sizeof(msg), commit, true);
}

/* mcu-cmd <type> <cmd> <hex payload>: raw diagnostic command. */
static void zxnpu_wlan_mcu_cmd(struct zxnpu_wlan *w, const char *args, bool commit)
{
	u8 msg[24 + 128];
	unsigned int type, cmd;
	u8 payload[128];
	int n = 0;
	const char *hex;

	if (sscanf(args, "%u %u", &type, &cmd) != 2) {
		zxnpu_wlan_append(w, "usage: mcu-cmd <type> <cmd> <hex>\n");
		return;
	}
	hex = strchr(args, ' ');
	hex = hex ? strchr(hex + 1, ' ') : NULL;
	if (hex) {
		hex++;
		while (hex[0] && hex[1] && n < (int)sizeof(payload)) {
			unsigned int hi, lo;
			char c;

			c = hex[0];
			hi = (c >= '0' && c <= '9') ? c - '0' :
			     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
			     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0xff;
			c = hex[1];
			lo = (c >= '0' && c <= '9') ? c - '0' :
			     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
			     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0xff;
			if (hi == 0xff || lo == 0xff)
				break;
			payload[n++] = (hi << 4) | lo;
			hex += 2;
		}
	}
	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(n, msg + 0);
	msg[2] = type;
	msg[4] = cmd;
	memcpy(msg + 24, payload, n);
	zxnpu_wlan_append(w, "mcu-cmd type=%u cmd=%u payload=%d\n", type, cmd, n);
	zxnpu_mcu_submit(w, msg, 24 + n, commit, true);
}


/*
 * RX/event ring 0 (0x18024500). The stock ring memory init prefills every
 * buffer with a 16-byte RXD (DMA address at +0, length at +6/+7 with
 * 0x40 in the high byte) and chains the 36-byte descriptors. Which ring
 * carries MCU events is a hypothesis: ring 0 is the first one the stock
 * dumper covers and the MTK event convention. mcu-recv only prints what
 * lands there; rx-init is gated like mcu-send.
 */
#define ZXNPU_MCU_RX_REG	ZXNPU_WFDMA_RX_RING0
#define ZXNPU_MCU_RX_DEPTH	32
#define ZXNPU_MCU_RX_BUF_SIZE	1700
#define ZXNPU_MCU_RXD_SIZE	16

static void zxnpu_wlan_rx_init(struct zxnpu_wlan *w, bool commit)
{
	int i;

	zxnpu_wlan_append(w, "rx-init ring0: %s\n", commit ? "COMMIT" : "dry run");
	if (!w->rx_ring || !w->rx_buf) {
		zxnpu_wlan_append(w, "  no rx memory (probe with enable=1)\n");
		return;
	}
	for (i = 0; i < ZXNPU_MCU_RX_DEPTH; i++) {
		u8 *d = (u8 *)w->rx_ring + i * ZXNPU_MCU_DESC_SIZE;
		u8 *buf = (u8 *)w->rx_buf + i * ZXNPU_MCU_RX_BUF_SIZE;
		dma_addr_t bd = w->rx_buf_dma + i * ZXNPU_MCU_RX_BUF_SIZE;

		memset(d, 0, ZXNPU_MCU_DESC_SIZE);
		put_unaligned_le32(16, d + 0);
		put_unaligned_le32(bd, d + 4);
		put_unaligned_le32(bd + ZXNPU_MCU_RX_BUF_SIZE, d + 8);
		put_unaligned_le32(bd, d + 12);
		put_unaligned_le32(ZXNPU_MCU_RX_BUF_SIZE, d + 24);
		put_unaligned_le32(w->rx_ring_dma +
				   ((i + 1) % ZXNPU_MCU_RX_DEPTH) * ZXNPU_MCU_DESC_SIZE,
				   d + 28);

		memset(buf, 0, ZXNPU_MCU_RXD_SIZE);
		put_unaligned_le32(bd, buf);
		buf[6] = ZXNPU_MCU_RX_BUF_SIZE & 0xff;
		buf[7] = 0x40 | ((ZXNPU_MCU_RX_BUF_SIZE >> 8) & 0x3f);
	}
	dma_wmb();
	zxnpu_wlan_append(w, "  reg %08x: base=%08x cnt=%u cidx=%u didx=%u\n",
			  ZXNPU_MCU_RX_REG,
			  zxnpu_cr_read(w, ZXNPU_MCU_RX_REG),
			  zxnpu_cr_read(w, ZXNPU_MCU_RX_REG + 4),
			  zxnpu_cr_read(w, ZXNPU_MCU_RX_REG + 8),
			  zxnpu_cr_read(w, ZXNPU_MCU_RX_REG + 12));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: commit=1 to enable RX DMA\n");
		return;
	}
	{
		u32 glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);

		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x7);
	}
	zxnpu_cr_write(w, ZXNPU_MCU_RX_REG, (u32)w->rx_ring_dma);
	zxnpu_cr_write(w, ZXNPU_MCU_RX_REG + 4, ZXNPU_MCU_RX_DEPTH);
	zxnpu_cr_write(w, ZXNPU_MCU_RX_REG + 8, 0);
	w->rx_cidx = 0;
}

static void zxnpu_wlan_mcu_recv(struct zxnpu_wlan *w)
{
	u32 didx, cidx, n, i;

	if (!w->rx_ring || !w->rx_buf) {
		zxnpu_wlan_append(w, "mcu-recv: no rx memory\n");
		return;
	}
	didx = zxnpu_cr_read(w, ZXNPU_MCU_RX_REG + 12) % ZXNPU_MCU_RX_DEPTH;
	cidx = w->rx_cidx % ZXNPU_MCU_RX_DEPTH;
	n = (didx + ZXNPU_MCU_RX_DEPTH - cidx) % ZXNPU_MCU_RX_DEPTH;
	zxnpu_wlan_append(w, "mcu-recv: cidx=%u didx=%u pending=%u\n",
			  cidx, didx, n);
	for (i = 0; i < n; i++) {
		u32 slot = (cidx + i) % ZXNPU_MCU_RX_DEPTH;
		u8 *buf = (u8 *)w->rx_buf + slot * ZXNPU_MCU_RX_BUF_SIZE;
		int j;

		zxnpu_wlan_append(w, "  slot %u:", slot);
		for (j = 0; j < 32; j++)
			zxnpu_wlan_append(w, " %02x", buf[j]);
		zxnpu_wlan_append(w, "\n");
	}
	if (n) {
		w->rx_cidx = didx;
		if (zxnpu_wlan_commit)
			zxnpu_cr_write(w, ZXNPU_MCU_RX_REG + 8, didx);
	}
}

/*
 * patch-dl <file> <dst-hex> [offset]: the download sequence recovered from
 * load_patch_v1 (0x1D5C0C):
 *
 *   SemGet type 0x10 cmd 0x17 {sem}
 *   AddressLen type 0x01 cmd 0x17 {dst, total, 0x80000000}
 *   FwScatter type 0xEE cmd 0x10 {chunk of the file}
 *   Finish type 0x07 cmd 0x17 {}
 *
 * The chunk size is 512 B to keep the on-stack message under the 1 KiB frame limit (the stock limit is 4096 - chip_cap[0x134]).
 * Everything is inert unless enable=1 and commit=1.
 */
#define ZXNPU_MCU_TYPE_SCATTER	0xEE
#define ZXNPU_MCU_CMD_SCATTER	0x10
#define ZXNPU_MCU_TYPE_FINISH	0x07
#define ZXNPU_MCU_TYPE_ADDR	0x01
#define ZXNPU_MCU_CHUNK		512

static void zxnpu_wlan_mcu_dl(struct zxnpu_wlan *w, const char *args, bool commit)
{
	const struct firmware *fw;
	unsigned long dst;
	unsigned int offset = 0;
	u8 msg[24 + ZXNPU_MCU_CHUNK];
	unsigned int done = 0, total;
	char name[128];
	int ret;

	if (sscanf(args, "%127s %lx %u", name, &dst, &offset) < 2) {
		zxnpu_wlan_append(w, "usage: mcu-dl <file> <dst-hex> [offset]\n");
		return;
	}
	ret = request_firmware_direct(&fw, name, &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "mcu-dl: %s missing (%d)\n", name, ret);
		return;
	}
	if (offset >= fw->size) {
		zxnpu_wlan_append(w, "mcu-dl: offset beyond file\n");
		release_firmware(fw);
		return;
	}
	total = fw->size - offset;
	zxnpu_wlan_append(w, "mcu-dl %s -> %08lx: %u bytes from +%u (%s)\n",
			  name, dst, total, offset, commit ? "COMMIT" : "dry run");

	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(4, msg + 0);
	msg[2] = ZXNPU_MCU_TYPE_PATCH;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	put_unaligned_le32(1, msg + 24);
	if (zxnpu_mcu_submit(w, msg, 28, commit, true))
		goto out;
	if (!commit)
		goto out;
	msleep(20);

	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(12, msg + 0);
	msg[2] = (dst == 0x04000000) ? 0x05 : ZXNPU_MCU_TYPE_ADDR;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	put_unaligned_le32(dst, msg + 24);
	put_unaligned_le32(total, msg + 28);
	put_unaligned_le32(0x80000000, msg + 32);
	if (zxnpu_mcu_submit(w, msg, 36, true, true))
		goto out;
	msleep(10);

	while (done < total) {
		unsigned int chunk = min_t(unsigned int, ZXNPU_MCU_CHUNK,
					   total - done);

		memset(msg, 0, 24 + chunk);
		put_unaligned_le16(chunk, msg + 0);
		msg[2] = ZXNPU_MCU_TYPE_SCATTER;
		msg[4] = ZXNPU_MCU_CMD_SCATTER;
		memcpy(msg + 24, fw->data + offset + done, chunk);
		if (zxnpu_mcu_submit(w, msg, 24 + chunk, true, false))
			goto out;
		done += chunk;
		if (!(done % (64 * ZXNPU_MCU_CHUNK)))
			zxnpu_wlan_append(w, "  ... %u/%u\n", done, total);
		usleep_range(1000, 2000);
	}

	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(0, msg + 0);
	msg[2] = ZXNPU_MCU_TYPE_FINISH;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	if (zxnpu_mcu_submit(w, msg, 24, true, true))
		goto out;
	zxnpu_wlan_append(w, "mcu-dl: %u bytes sent, finish issued\n", done);
out:
	release_firmware(fw);
}

static void zxnpu_wlan_wfdma(struct zxnpu_wlan *w, bool commit)
{
	u32 v, nv;
	int i;

	zxnpu_wlan_append(w, "wfdma: %s\n", commit ? "COMMIT" : "plan only");
	v = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);
	nv = (v & ~0x08000000u) | 0x10000000u | 0x00200000u;
	zxnpu_wlan_append(w, "  %08x: %08x -> %08x\n",
			  ZXNPU_WFDMA_GLO_CFG, v, nv);
	if (commit)
		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, nv);
	for (i = 0; i < 6; i++) {
		u32 reg = ZXNPU_WFDMA_RING_HI + 4 * i;
		u32 val = ((i + 1) << 22) | 4;

		zxnpu_wlan_append(w, "  %08x: %08x\n", reg, val);
		if (commit)
			zxnpu_cr_write(w, reg, val);
	}
	for (i = 0; i < 5; i++) {
		u32 reg = ZXNPU_WFDMA_RING_LO + 4 * i;
		u32 val = ((i + 6) << 22) | 4;

		zxnpu_wlan_append(w, "  %08x: %08x\n", reg, val);
		if (commit)
			zxnpu_cr_write(w, reg, val);
	}
	zxnpu_wlan_append(w, "  %08x: ffffffff\n", ZXNPU_WFDMA_INT_ENA);
	if (commit)
		zxnpu_cr_write(w, ZXNPU_WFDMA_INT_ENA, ~0u);
	v = zxnpu_cr_read(w, ZXNPU_WFDMA_PCIE_CFG2);
	zxnpu_wlan_append(w, "  %08x: %08x -> 811c0000\n",
			  ZXNPU_WFDMA_PCIE_CFG2, v);
	if (commit)
		zxnpu_cr_write(w, ZXNPU_WFDMA_PCIE_CFG2, 0x811c0000);
	v = zxnpu_cr_read(w, ZXNPU_WFDMA_TX_BASE);
	nv = (v & ~(0x1fc0000u | 0x20000u)) | 0x2800000u;
	zxnpu_wlan_append(w, "  %08x: %08x -> %08x\n",
			  ZXNPU_WFDMA_TX_BASE, v, nv);
	if (commit)
		zxnpu_cr_write(w, ZXNPU_WFDMA_TX_BASE, nv);
}

static void zxnpu_wlan_fw_detail(struct zxnpu_wlan *w)
{
	static const char * const names[] = {
		"zte/mt7916/mt7916_patch_e1_hdr.bin",
		"zte/mt7916/WIFI_RAM_CODE_MT7916.bin",
		"zte/mt7916/7916_WACPU_RAM_CODE_release.bin",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		const struct firmware *fw;
		int ret;

		ret = request_firmware_direct(&fw, names[i], &w->pdev->dev);
		if (ret)
			continue;
		if (fw->size >= 48)
			zxnpu_wlan_append(w,
					  "%s: date=%.16s platform=%.4s hwsw=%08x ver=%08x subtype=%08x magic=%08x\n",
					  names[i], fw->data, fw->data + 16,
					  get_unaligned_le32(fw->data + 20),
					  get_unaligned_le32(fw->data + 24),
					  get_unaligned_le32(fw->data + 28),
					  get_unaligned_le32(fw->data + 32));
		zxnpu_wlan_append(w, "%s: first16=%*phN size=%zu\n", names[i],
				  16, fw->data, fw->size);
		release_firmware(fw);
	}
}

/*
 * Andes MCU command message, from AndesInitCmdMsg (0x1B8E5C): a 24-byte
 * header in the skb head followed by the payload. Byte 2 is the type,
 * byte 4 the command; the three patch-download commands all use cmd
 * 0x17 with type 0x10 (sem get), 0x01/0x05 (address/len) and 0x07
 * (finish). The queue id at +20 comes from GetRealPortQueueID(type) and
 * is not modelled here.
 */
static void zxnpu_mcu_dump(struct zxnpu_wlan *w, u8 type, u8 cmd,
			   const u32 *payload, int words)
{
	u8 msg[24 + 16] = {};
	int i;

	put_unaligned_le16(words * 4, msg);
	msg[2] = type;
	msg[4] = cmd;
	for (i = 0; i < words; i++)
		put_unaligned_le32(payload[i], msg + 24 + 4 * i);
	zxnpu_wlan_append(w, "  mcu type=%02x cmd=%02x len=%d:", type, cmd,
			  words * 4);
	for (i = 0; i < 24 + words * 4; i++)
		zxnpu_wlan_append(w, " %02x", msg[i]);
	zxnpu_wlan_append(w, "\n");
}

static void zxnpu_wlan_mcu_plan(struct zxnpu_wlan *w)
{
	const struct firmware *fw = NULL;
	u32 payload[3];

	zxnpu_wlan_append(w, "mcu: patch download sequence (encoder only)\n");
	zxnpu_wlan_append(w, "  SemGet(type 0x10):\n");
	payload[0] = 0;
	zxnpu_mcu_dump(w, 0x10, 0x17, payload, 1);

	if (!request_firmware_direct(&fw, "zte/mt7916/mt7916_patch_e1_hdr.bin",
				     &w->pdev->dev)) {
		payload[0] = 0x04000000;
		payload[1] = fw->size;
		payload[2] = 0;
		zxnpu_wlan_append(w, "  AddressLen(type 0x01, %zu bytes at 0x04000000; segmentation not decoded):\n",
				  fw->size);
		zxnpu_mcu_dump(w, 0x01, 0x17, payload, 3);
		release_firmware(fw);
	} else {
		zxnpu_wlan_append(w, "  patch file missing\n");
	}

	zxnpu_wlan_append(w, "  Finish(type 0x07):\n");
	zxnpu_mcu_dump(w, 0x07, 0x17, NULL, 0);
}

static int zxnpu_wlan_status_show(struct seq_file *s, void *unused)
{
	struct zxnpu_wlan *w = zxnpu_wlan_singleton;

	if (!w)
		return 0;
	seq_printf(s, "%s", w->status);
	return 0;
}

static ssize_t zxnpu_wlan_ctl_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct zxnpu_wlan *w = zxnpu_wlan_singleton;
	char cmd[32];
	bool commit;

	if (!w)
		return -ENODEV;
	if (count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	mutex_lock(&w->lock);
	memset(w->status, 0, sizeof(w->status));
	if (!strncmp(cmd, "probe", 5)) {
		zxnpu_wlan_probe_regs(w);
	} else if (!strncmp(cmd, "fw", 2)) {
		zxnpu_wlan_fw_detail(w);
	} else if (!strncmp(cmd, "plan", 4)) {
		zxnpu_wlan_wfdma(w, false);
	} else if (!strncmp(cmd, "mcu", 3)) {
		zxnpu_wlan_mcu_plan(w);
	} else if (!strncmp(cmd, "rings", 5)) {
		zxnpu_wlan_rings(w);
	} else if (!strncmp(cmd, "mcuwin", 6)) {
		zxnpu_wlan_mcuwin(w);
	} else if (!strncmp(cmd, "mcu-cmd ", 8)) {
		zxnpu_wlan_mcu_cmd(w, cmd + 8, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl ", 7)) {
		zxnpu_wlan_mcu_dl(w, cmd + 7, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "rx-init", 7)) {
		zxnpu_wlan_rx_init(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-recv", 8)) {
		zxnpu_wlan_mcu_recv(w);
	} else if (!strncmp(cmd, "mcu-send", 8)) {
		zxnpu_wlan_mcu_send(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "commit", 6)) {
		commit = zxnpu_wlan_commit;
		if (!commit)
			zxnpu_wlan_append(w, "commit refused: set zxnpu_wlan.commit=1\n");
		zxnpu_wlan_wfdma(w, commit);
	} else {
		zxnpu_wlan_append(w, "commands: probe | fw | plan | mcu | rings | mcuwin | mcu-send | mcu-cmd | mcu-dl | rx-init | mcu-recv | commit\n");
	}
	mutex_unlock(&w->lock);
	return count;
}

static const struct proc_ops zxnpu_wlan_ctl_ops = {
	.proc_write = zxnpu_wlan_ctl_write,
	.proc_lseek = default_llseek,
};

static int zxnpu_wlan_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct zxnpu_wlan *w;
	struct proc_dir_entry *dir;
	u8 revision;
	u16 command, status;
	int i, ret;

	if (!zxnpu_wlan_enable)
		return -ENODEV;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	ret = pcim_iomap_regions(pdev, BIT(0) | BIT(1) | BIT(2),
				 "zxnpu-wlan");
	if (ret)
		return ret;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->pdev = pdev;
	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	mutex_init(&w->lock);
	w->mcu_ring = dmam_alloc_coherent(&pdev->dev,
					  ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_DESC_SIZE,
					  &w->mcu_ring_dma, GFP_KERNEL);
	w->mcu_buf = dmam_alloc_coherent(&pdev->dev, ZXNPU_MCU_BUF_SIZE,
					 &w->mcu_buf_dma, GFP_KERNEL);
	w->rx_ring = dmam_alloc_coherent(&pdev->dev,
					 ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_DESC_SIZE,
					 &w->rx_ring_dma, GFP_KERNEL);
	w->rx_buf = dmam_alloc_coherent(&pdev->dev,
					ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_RX_BUF_SIZE,
					&w->rx_buf_dma, GFP_KERNEL);
	for (i = 0; i < 3; i++)
		w->bar[i] = pcim_iomap_table(pdev)[i];

	pci_read_config_byte(pdev, PCI_REVISION_ID, &revision);
	pci_read_config_word(pdev, PCI_COMMAND, &command);
	pci_read_config_word(pdev, PCI_STATUS, &status);
	zxnpu_wlan_append(w, "vendor=%04x device=%04x revision=%02x\n",
			  pdev->vendor, pdev->device, revision);
	zxnpu_wlan_append(w, "command=%04x status=%04x bars=%d\n",
			  command, status, pci_select_bars(pdev,
			  IORESOURCE_MEM));
	for (i = 0; i < 3; i++)
		zxnpu_wlan_append(w, "bar%d: start=%pa len=%pa%s\n", i,
				  &pdev->resource[i].start,
				  &pdev->resource[i].end,
				  w->bar[i] ? "" : " (unmapped)");
	zxnpu_wlan_append(w, "state: G3 skeleton, no MT7916 register written\n");
	zxnpu_wlan_audit_firmware(w);

	dir = proc_mkdir("zx-csp-wlan", NULL);
	if (dir) {
		if (!proc_create_single("status", 0444, dir,
					zxnpu_wlan_status_show) ||
		    !proc_create("ctl", 0200, dir, &zxnpu_wlan_ctl_ops)) {
			proc_remove(dir);
			dir = NULL;
		}
	}
	w->proc = dir;
	zxnpu_wlan_singleton = w;
	pci_set_drvdata(pdev, w);
	dev_info(&pdev->dev, "G3.2 ready (commit=%d)\n", zxnpu_wlan_commit);
	return 0;
}

static void zxnpu_wlan_remove(struct pci_dev *pdev)
{
	struct zxnpu_wlan *w = pci_get_drvdata(pdev);

	if (w && w->proc)
		proc_remove(w->proc);
	zxnpu_wlan_singleton = NULL;
}

static const struct pci_device_id zxnpu_wlan_ids[] = {
	{ PCI_DEVICE(ZXNPU_WLAN_VENDOR, ZXNPU_WLAN_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, zxnpu_wlan_ids);

static struct pci_driver zxnpu_wlan_driver = {
	.name = "zxnpu-wlan",
	.id_table = zxnpu_wlan_ids,
	.probe = zxnpu_wlan_probe,
	.remove = zxnpu_wlan_remove,
};
module_pci_driver(zxnpu_wlan_driver);

MODULE_DESCRIPTION("ZTE ZX279128S vendor-native MT7916 G3 skeleton");
MODULE_LICENSE("GPL");
