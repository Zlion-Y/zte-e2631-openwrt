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
	void *fwdl_ring;	/* qid16 / 0x18024400: FwScatter bulk data */
	dma_addr_t fwdl_ring_dma;
	void *fwdl_buf;
	dma_addr_t fwdl_buf_dma;
	u32 fwdl_cidx;
	void *rx_ring;		/* 32 x 36 descriptors */
	dma_addr_t rx_ring_dma;
	void *rx_buf;		/* 32 x 1700 buffers with 16B RXD */
	dma_addr_t rx_buf_dma;
	u32 rx_cidx;
	void *dtx_ring;		/* qid18 / BAND0 TX data ring (empty: TXD undecoded) */
	dma_addr_t dtx_ring_dma;
	void *drx_ring;		/* qid1 RX data ring, 64 x 36 descriptors */
	dma_addr_t drx_ring_dma;
	void *drx_buf;		/* 64 x 1700 buffers with 16B RXD */
	dma_addr_t drx_buf_dma;
	u32 drx_cidx;
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

/* BAR0 offsets that are not in the 0x1800_0000 CR window. */
static u32 zxnpu_bar0_read(struct zxnpu_wlan *w, u32 off)
{
	if (!w->bar[0] || (resource_size_t)off + 4 > pci_resource_len(w->pdev, 0))
		return 0;
	return readl(w->bar[0] + off);
}

static void zxnpu_bar0_write(struct zxnpu_wlan *w, u32 off, u32 val)
{
	if (!w->bar[0] || (resource_size_t)off + 4 > pci_resource_len(w->pdev, 0))
		return;
	writel(val, w->bar[0] + off);
}

/*
 * FW state handshake, from ctrl_fw_state_v2 (0x1D5198) and
 * AsicGetFwSyncValue (0x140C00). MT_TOP_MISC (0x180600F0) is reached
 * through the L1 window: MT_HIF_REMAP_L1_MT7916 (0xFE260) takes
 * addr >> 16, then the value is read at 0xE0000 + (addr & 0xFFFF).
 * Bits 2:0 are the state mt76 calls FW_STATE_*: 1 = firmware download,
 * 3 = normal operation, 7 = ready including the WA MCU.
 */
#define ZXNPU_HIF_REMAP_L1_MT7916	0x000fe260
#define ZXNPU_L1_REMAP_BASE		0x000e0000
#define ZXNPU_TOP_MISC			0x180600f0

static u32 zxnpu_fw_state_read(struct zxnpu_wlan *w)
{
	zxnpu_bar0_write(w, ZXNPU_HIF_REMAP_L1_MT7916, ZXNPU_TOP_MISC >> 16);
	(void)zxnpu_bar0_read(w, ZXNPU_HIF_REMAP_L1_MT7916);
	return zxnpu_bar0_read(w, ZXNPU_L1_REMAP_BASE +
			       (ZXNPU_TOP_MISC & 0xffff)) & 0x7;
}

static void zxnpu_wlan_fw_state(struct zxnpu_wlan *w, bool commit)
{
	u32 st;

	zxnpu_wlan_append(w, "fw-state: %s\n", commit ? "COMMIT" : "read only");
	zxnpu_wlan_append(w, "  remap %08x <- %04x, read %08x\n",
			  ZXNPU_HIF_REMAP_L1_MT7916, ZXNPU_TOP_MISC >> 16,
			  ZXNPU_L1_REMAP_BASE + (ZXNPU_TOP_MISC & 0xffff));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set commit=1 to read the L1 window\n");
		return;
	}
	st = zxnpu_fw_state_read(w);
	zxnpu_wlan_append(w, "  MT_TOP_MISC state=%u (%s)\n", st,
			  st == 1 ? "firmware download" :
			  st == 3 ? "normal operation" :
			  st == 7 ? "ready (WM+WA)" : "unknown");
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
 *  - TX ring bases are WFDMA0(0x24000) + 0x300 + qid*0x10. For MT7916 the
 *    qids are FWDL=16 (0x24400), MCU_WM=17 (0x24410), BAND0=18,
 *    BAND1=19, MCU_WA=20 (0x24440); the MCU_WM ring is the command ring.
 *
 * Bulk FwScatter payloads go to qid16/FWDL (ring 0), every other command
 * to qid17/MCU_WM (ring 1); this follows GetRealPortQueueID (0x1C112C),
 * which maps type 0xEE to chip_cap[0x162] and everything else to the
 * 0x8000 control queue.
 *
 * This is a hypothesis, not a verified sequence: it is inert unless
 * enable=1 and commit=1.
 */
#define ZXNPU_FWDL_RING_NR	0
#define ZXNPU_MCU_RING_NR	1
#define ZXNPU_MCU_RING_REG	(ZXNPU_WFDMA_TX_RING0 + 0x10 * ZXNPU_MCU_RING_NR)
#define ZXNPU_MCU_DESC_SIZE	36
#define ZXNPU_MCU_RING_DEPTH	64
#define ZXNPU_MCU_BUF_SIZE	4096
#define ZXNPU_MCU_TYPE_PATCH	0x10
#define ZXNPU_MCU_CMD_PATCH	0x17
#define ZXNPU_MCU_TYPE_SCATTER	0xEE
#define ZXNPU_MCU_CMD_SCATTER	0x10
#define ZXNPU_MCU_TYPE_FINISH	0x07
#define ZXNPU_MCU_TYPE_ADDR	0x01
#define ZXNPU_MCU_CHUNK		512
#define ZXNPU_WM_FW		"zte/mt7916/WIFI_RAM_CODE_MT7916.bin"
#define ZXNPU_WA_FW		"zte/mt7916/7916_WACPU_RAM_CODE_release.bin"
#define ZXNPU_PATCH_FW		"zte/mt7916/mt7916_patch_e1_hdr.bin"
#define ZXNPU_FW_MAX_REGIONS	16
#define ZXNPU_PATCH_HDR		30

static void zxnpu_mcu_fill_desc(void *ring, dma_addr_t ring_dma, u32 slot,
				u32 buf_dma, u32 len)
{
	u8 *d = (u8 *)ring + slot * ZXNPU_MCU_DESC_SIZE;
	u32 next = (slot + 1) % ZXNPU_MCU_RING_DEPTH;

	memset(d, 0, ZXNPU_MCU_DESC_SIZE);
	put_unaligned_le32(16, d + 0);
	put_unaligned_le32(buf_dma, d + 4);
	put_unaligned_le32(buf_dma + len, d + 8);
	put_unaligned_le32(buf_dma, d + 12);
	put_unaligned_le32(len, d + 24);
	put_unaligned_le32(ring_dma + next * ZXNPU_MCU_DESC_SIZE, d + 28);
}

/*
 * ring 0 = qid16 FWDL (0x18024400), ring 1 = qid17 MCU_WM (0x18024410).
 * GetRealPortQueueID (0x1C112C) sends type 0xEE to chip_cap[0x162] (the
 * FWDL queue) and every other command to the 0x8000 control queue, so
 * bulk FwScatter payloads use the FWDL ring and command messages use
 * MCU_WM.
 */
static int zxnpu_mcu_submit_ring(struct zxnpu_wlan *w, int ring,
				 const u8 *msg, int len, bool commit,
				 bool verbose)
{
	void *desc = ring ? w->mcu_ring : w->fwdl_ring;
	dma_addr_t desc_dma = ring ? w->mcu_ring_dma : w->fwdl_ring_dma;
	void *buf = ring ? w->mcu_buf : w->fwdl_buf;
	dma_addr_t buf_dma = ring ? w->mcu_buf_dma : w->fwdl_buf_dma;
	u32 *cidx = ring ? &w->mcu_cidx : &w->fwdl_cidx;
	u32 reg = ZXNPU_WFDMA_TX_RING0 + 0x10 * ring;
	u32 slot, didx, prev;

	if (!desc || !buf || len <= 0 || len > ZXNPU_MCU_BUF_SIZE)
		return -EINVAL;
	slot = (*cidx) % ZXNPU_MCU_RING_DEPTH;
	memset(buf, 0, ZXNPU_MCU_BUF_SIZE);
	memcpy(buf, msg, len);
	zxnpu_mcu_fill_desc(desc, desc_dma, slot, (u32)buf_dma, len);
	dma_wmb();

	prev = zxnpu_cr_read(w, reg + 12);
	if (verbose)
		zxnpu_wlan_append(w, "  submit qid%d slot=%u len=%d: base=%08x cnt=%u cidx=%u didx=%u\n",
				  ring ? 17 : 16, slot, len,
				  zxnpu_cr_read(w, reg),
				  zxnpu_cr_read(w, reg + 4),
				  zxnpu_cr_read(w, reg + 8), prev);
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to write\n");
		return 0;
	}

	{
		u32 glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);

		/* hif_set_WFDMA(2): WFDMA_EN | TX DMA EN */
		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x5);
	}
	zxnpu_cr_write(w, reg, (u32)desc_dma);
	zxnpu_cr_write(w, reg + 4, ZXNPU_MCU_RING_DEPTH);
	(*cidx)++;
	zxnpu_cr_write(w, reg + 8, *cidx);
	if (!readl_poll_timeout(w->bar[0] + ZXNPU_CR(reg + 12), didx,
				(didx % ZXNPU_MCU_RING_DEPTH) ==
				(*cidx % ZXNPU_MCU_RING_DEPTH), 100, 50000))
		zxnpu_wlan_append(w, "  didx advanced to %u\n", didx);
	else
		zxnpu_wlan_append(w, "  didx still %u (last was %u)\n",
				  zxnpu_cr_read(w, reg + 12), prev);
	return 0;
}

static int zxnpu_mcu_submit(struct zxnpu_wlan *w, const u8 *msg, int len,
			    bool commit, bool verbose)
{
	return zxnpu_mcu_submit_ring(w, ZXNPU_MCU_RING_NR, msg, len, commit,
				     verbose);
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

/*
 * mcu-dl-regions <file> <manifest>: the multi-region form of mcu-dl.
 * The list is comma-separated regions: "addr len offset [flags]" in hex;
 * for every line the driver sends
 *   AddressLen(addr, len, 0x80000000 | flags)
 * followed by ceil(len / 512) FwScatter commands built from
 * file[offset .. offset+len). This is the shape load_fw_v1 uses; only
 * the per-band region table (doc 14 6.14) is still missing, so the
 * table can be supplied by hand until it is decoded.
 */
static void zxnpu_wlan_mcu_regions(struct zxnpu_wlan *w, const char *args,
				   bool commit)
{
	const struct firmware *fw;
	char name[128], manifest[128];
	char *cur, *line;
	int ret;

	if (sscanf(args, "%127s %127s", name, manifest) != 2) {
		zxnpu_wlan_append(w, "usage: mcu-dl-regions <file> <addr len off [flags],...>\n");
		return;
	}
	ret = request_firmware_direct(&fw, name, &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "mcu-dl-regions: %s missing (%d)\n", name, ret);
		return;
	}
	cur = manifest;
	while ((line = strsep(&cur, ",")) && *line) {
		unsigned long addr, len, off, flags = 0, done = 0;
		u8 msg[24 + ZXNPU_MCU_CHUNK];

		if (sscanf(line, "%lx %lx %lx %lx", &addr, &len, &off,
			   &flags) < 3) {
			zxnpu_wlan_append(w, "  bad region '%s'\n", line);
			break;
		}
		if (off + len > fw->size) {
			zxnpu_wlan_append(w, "  region beyond file: %s\n", line);
			break;
		}
		zxnpu_wlan_append(w, "  region addr=%08lx len=%lu off=%lu flags=%lx\n",
				  addr, len, off, flags);

		memset(msg, 0, sizeof(msg));
		put_unaligned_le16(12, msg + 0);
		msg[2] = (addr == 0x04000000) ? 0x05 : 0x01;
		msg[4] = ZXNPU_MCU_CMD_PATCH;
		put_unaligned_le32(addr, msg + 24);
		put_unaligned_le32(len, msg + 28);
		put_unaligned_le32(0x80000000 | flags, msg + 32);
		if (zxnpu_mcu_submit(w, msg, 36, commit, true))
			break;
		if (!commit)
			continue;
		msleep(10);

		while (done < len) {
			unsigned int chunk = min_t(unsigned long, ZXNPU_MCU_CHUNK,
						   len - done);

			memset(msg, 0, 24 + chunk);
			put_unaligned_le16(chunk, msg + 0);
			msg[2] = 0xEE;
			msg[4] = 0x10;
			memcpy(msg + 24, fw->data + off + done, chunk);
			if (zxnpu_mcu_submit_ring(w, ZXNPU_FWDL_RING_NR, msg, 24 + chunk, true, false))
				goto out;
			done += chunk;
			usleep_range(500, 1000);
		}
		zxnpu_wlan_append(w, "  region done (%lu B)\n", done);
	}
out:
	release_firmware(fw);
}

/*
 * fw-start <option-hex> [override-hex]: MtCmdFwStartReq (mt7916.ko
 * 0x1CC1D0) is type 0x02 / cmd 0x17 with a two-word payload
 * {option, override_addr}. load_fw_v1 sets bit0 of option when a region
 * carried the override flag (feature_set 0x20) and bit2 for band 1.
 */
static void zxnpu_wlan_fw_start(struct zxnpu_wlan *w, const char *args, bool commit)
{
	u8 msg[24 + 8];
	unsigned long option = 0, override = 0;

	if (sscanf(args, "%lx %lx", &option, &override) < 1) {
		zxnpu_wlan_append(w, "usage: fw-start <option-hex> [override-hex]\n");
		return;
	}
	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(8, msg + 0);
	msg[2] = 0x02;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	put_unaligned_le32(option, msg + 24);
	put_unaligned_le32(override, msg + 28);
	zxnpu_wlan_append(w, "fw-start option=%08lx override=%08lx\n",
			  option, override);
	zxnpu_mcu_submit(w, msg, 32, commit, true);
}

/* fw-info [file]: patch header fields and MTK firmware trailer. */
static void zxnpu_wlan_fw_info(struct zxnpu_wlan *w, const char *args)
{
	static const char * const names[] = {
		"zte/mt7916/mt7916_patch_e1_hdr.bin",
		"zte/mt7916/WIFI_RAM_CODE_MT7916.bin",
		"zte/mt7916/7916_WACPU_RAM_CODE_release.bin",
	};
	char name[128];
	int i;

	if (sscanf(args, "%127s", name) == 1) {
		const struct firmware *fw;
		int ret = request_firmware_direct(&fw, name, &w->pdev->dev);

		if (ret) {
			zxnpu_wlan_append(w, "fw-info: %s missing (%d)@N@", name, ret);
			return;
		}
		zxnpu_wlan_append(w, "fw-info %s size=%zu@N@", name, fw->size);
		if (fw->size >= 0x40 && get_unaligned_le32(fw->data + 0x20) == 0x11223344) {
			zxnpu_wlan_append(w, "  patch: date=%.16s platform=%.4s dst=%08x size_field=%08x@N@",
					  fw->data, fw->data + 16,
					  get_unaligned_le32(fw->data + 0x24),
					  get_unaligned_le32(fw->data + 0x28));
		} else if (fw->size >= 36) {
			const u8 *t = fw->data + fw->size - 36;

			zxnpu_wlan_append(w, "  trailer: chip=%02x n_region=%u fmt=%u date=%.15s crc=%08x@N@",
					  t[0], t[2], t[3], t + 17,
					  get_unaligned_le32(t + 32));
		}
		release_firmware(fw);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++)
		zxnpu_wlan_fw_info(w, names[i]);
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

/*
 * R4 data-plane rings (doc 38 §3 R4 / doc 14 §6.5.3). The register block
 * layout is the verified one: BASE/CNT/CPU_IDX/DMA_IDX at +0/+4/+8/+0xC.
 * The WiFi data queues ride qid18 (BAND0 TX), qid19 (BAND1 TX) and
 * qid1/qid2 (RX data) per the qid map pinned in doc 35; the ring→VAP
 * names come from the l1profile main_ifname pair (wlan0/wlan5g0).
 *
 * The RX side reuses the vendor prefill convention verified in
 * pci_init_txrx_ring_mem: a 16-byte RXD header per buffer (buf[0..3] =
 * DMA address, buf[6] = length low, buf[7] = 0x40 | length high). The
 * TX side only allocates and programs the ring here: the connac2 TXD
 * format has not been decoded from the vendor binary yet, so the TX
 * ring stays empty until that lands (documented gap, doc 42).
 */
#define ZXNPU_DATA_TX_REG	(ZXNPU_WFDMA_TX_RING0 + 0x10 * 2)
#define ZXNPU_DATA_RX_REG	(ZXNPU_WFDMA_RX_RING0 + 0x10 * 1)
#define ZXNPU_DATA_DEPTH	64
#define ZXNPU_DATA_BUF_SIZE	1700

static void zxnpu_wlan_data_init(struct zxnpu_wlan *w, bool commit)
{
	u32 glo;
	int i;

	zxnpu_wlan_append(w, "data-init: %s\n",
			  commit ? "COMMIT" : "dry run");
	if (!w->drx_ring || !w->drx_buf || !w->dtx_ring) {
		zxnpu_wlan_append(w, "  no data memory (probe with enable=1)\n");
		return;
	}
	for (i = 0; i < ZXNPU_DATA_DEPTH; i++) {
		u8 *d = (u8 *)w->drx_ring + i * ZXNPU_MCU_DESC_SIZE;
		u8 *buf = (u8 *)w->drx_buf + i * ZXNPU_DATA_BUF_SIZE;
		dma_addr_t bd = w->drx_buf_dma + i * ZXNPU_DATA_BUF_SIZE;

		memset(d, 0, ZXNPU_MCU_DESC_SIZE);
		put_unaligned_le32(16, d + 0);
		put_unaligned_le32(bd, d + 4);
		put_unaligned_le32(bd + ZXNPU_DATA_BUF_SIZE, d + 8);
		put_unaligned_le32(bd, d + 12);
		put_unaligned_le32(ZXNPU_DATA_BUF_SIZE, d + 24);
		put_unaligned_le32(w->drx_ring_dma +
				   ((i + 1) % ZXNPU_DATA_DEPTH) * ZXNPU_MCU_DESC_SIZE,
				   d + 28);

		memset(buf, 0, ZXNPU_MCU_RXD_SIZE);
		put_unaligned_le32(bd, buf);
		buf[6] = ZXNPU_DATA_BUF_SIZE & 0xff;
		buf[7] = 0x40 | ((ZXNPU_DATA_BUF_SIZE >> 8) & 0x3f);
	}
	memset(w->dtx_ring, 0, ZXNPU_DATA_DEPTH * ZXNPU_MCU_DESC_SIZE);
	dma_wmb();
	zxnpu_wlan_append(w, "  rx %08x: base=%08x cnt=%u cidx=%u didx=%u\n",
			  ZXNPU_DATA_RX_REG,
			  zxnpu_cr_read(w, ZXNPU_DATA_RX_REG),
			  zxnpu_cr_read(w, ZXNPU_DATA_RX_REG + 4),
			  zxnpu_cr_read(w, ZXNPU_DATA_RX_REG + 8),
			  zxnpu_cr_read(w, ZXNPU_DATA_RX_REG + 12));
	zxnpu_wlan_append(w, "  tx %08x: base=%08x cnt=%u (TXD format not decoded; ring kept empty)\n",
			  ZXNPU_DATA_TX_REG,
			  zxnpu_cr_read(w, ZXNPU_DATA_TX_REG),
			  zxnpu_cr_read(w, ZXNPU_DATA_TX_REG + 4));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: commit=1 to enable data RX DMA\n");
		return;
	}
	glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);
	zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x7);
	zxnpu_cr_write(w, ZXNPU_DATA_RX_REG, (u32)w->drx_ring_dma);
	zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 4, ZXNPU_DATA_DEPTH);
	zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 8, 0);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG, (u32)w->dtx_ring_dma);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 4, ZXNPU_DATA_DEPTH);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 8, 0);
	w->drx_cidx = 0;
}

static void zxnpu_wlan_data_recv(struct zxnpu_wlan *w)
{
	u32 didx, cidx, n, i;

	if (!w->drx_ring || !w->drx_buf) {
		zxnpu_wlan_append(w, "data-recv: no rx memory\n");
		return;
	}
	didx = zxnpu_cr_read(w, ZXNPU_DATA_RX_REG + 12) % ZXNPU_DATA_DEPTH;
	cidx = w->drx_cidx % ZXNPU_DATA_DEPTH;
	n = (didx + ZXNPU_DATA_DEPTH - cidx) % ZXNPU_DATA_DEPTH;
	zxnpu_wlan_append(w, "data-recv: cidx=%u didx=%u pending=%u\n",
			  cidx, didx, n);
	for (i = 0; i < n && i < 8; i++) {
		u32 slot = (cidx + i) % ZXNPU_DATA_DEPTH;
		u8 *buf = (u8 *)w->drx_buf + slot * ZXNPU_DATA_BUF_SIZE;
		int j;

		zxnpu_wlan_append(w, "  slot %u:", slot);
		for (j = 0; j < 32; j++)
			zxnpu_wlan_append(w, " %02x", buf[j]);
		zxnpu_wlan_append(w, "\n");
	}
	if (n > 8)
		zxnpu_wlan_append(w, "  ... %u more\n", n - 8);
	if (n) {
		w->drx_cidx = didx;
		if (zxnpu_wlan_commit)
			zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 8, didx);
	}
}

/*
 * TX path (doc 43 R4b + doc 44 R4c decode): the vendor TX architecture is
 * mapped — the TMAC is a 52-byte block built by memzero + byte stores, the
 * skb gets it pushed and is queued to the data ring, and
 * pci_kick_out_data_tx (0x1E117C) is a pure doorbell: one hif_io_write32
 * of the CPU_IDX into the ring block's +8 register (the same
 * BASE/CNT/CPU_IDX/DMA_IDX layout doc 14 §6.5.3 verified).
 *
 * R4c traced the 52-byte layout with value sources: +4/+5 = u16 at
 * sta+3552, +7 = 1, +24 = arg5 (u32), +28 = 0x18, +31/+40 = signed
 * sta+14, +45 = sta+41, +48 = sta+3528 (typed, compared against 2),
 * +52 = sta+3420, +53 = 1; zeros at +6/+9/+11/+12/+15/+17/+20/+21.
 * write_tmac_info_offload_pkt (0x1B05E8) is the OFFLOAD/mgmt path
 * (HcGetBcnQueueIdx / HcGetMgmtQueueIdx / HcGetBandByWdev /
 * asic_write_tmac_info_fixed_rate for fixed-rate frames); the DATA
 * TMAC builder is the asic_write_tmac_info_fixed_rate family
 * (0x142404) — byte semantics still pending (R4d), so this prints the
 * template and refuses to transmit.
 */
static void zxnpu_wlan_data_tx(struct zxnpu_wlan *w)
{
	zxnpu_wlan_append(w, "data-tx: TMAC template printer, no transmission@N@");
	zxnpu_wlan_append(w, "  chain: TMAC(52B, memzero+fill) -> skb push -> data ring@N@");
	zxnpu_wlan_append(w, "  (qid18 BAND0 %08x) -> pci_kick_out_data_tx(0x1E117C)@N@",
			  ZXNPU_DATA_TX_REG);
	zxnpu_wlan_append(w, "  kick = CPU_IDX write to ring block +8 (doc 14 §6.5.3)@N@");
	zxnpu_wlan_append(w, "  TMAC layout (offset -> source, R4c trace):@N@");
	zxnpu_wlan_append(w, "    +4/5 u16 sta+3552 | +7 =1 | +24 u32 arg5 | +28 =0x18@N@");
	zxnpu_wlan_append(w, "    +31/+40 s8 sta+14 | +45 sta+41 | +48 sta+3528 (typed)@N@");
	zxnpu_wlan_append(w, "    +52 sta+3420 | +53 =1 | zeros +6/+9/+11/+12/+15/+17/+20/+21@N@");
	zxnpu_wlan_append(w, "  offload/mgmt path: HcGetMgmtQueueIdx/BcnQueueIdx +@N@");
	zxnpu_wlan_append(w, "  asic_write_tmac_info_fixed_rate(0x142404) = data TMAC family (R4d)@N@");
	zxnpu_wlan_append(w, "  sta+8 cap-word bits 16..31 = the layer_cap bit mask family@N@");
	zxnpu_wlan_append(w, "  (same semantics as vendor npu_cap/zwzzrl layer_cap tables)@N@");
}

/* The ring-to-VAP plan: names from the l1profile main_ifname pair, queue
 * numbers from the doc 35 qid map. NAPI attaches in P5, when the VAP
 * netdevs exist for it to poll on behalf of.
 */
static void zxnpu_wlan_dataplane(struct zxnpu_wlan *w)
{
	zxnpu_wlan_append(w, "dataplane map (skeleton):\n");
	zxnpu_wlan_append(w, "  BAND0 (wlan0):  TX qid18 %08x, RX data qid1 %08x\n",
			  ZXNPU_WFDMA_TX_RING0 + 0x10 * 2,
			  ZXNPU_DATA_RX_REG);
	zxnpu_wlan_append(w, "  BAND1 (wlan5g0): TX qid19 %08x (ring alloc in P5)\n",
			  ZXNPU_WFDMA_TX_RING0 + 0x10 * 3);
	zxnpu_wlan_append(w, "  MCU events:      RX qid0 %08x (rx-init/mcu-recv)\n",
			  ZXNPU_MCU_RX_REG);
	zxnpu_wlan_append(w, "  depth: %u, buf %u B, NAPI attaches in P5\n",
			  ZXNPU_DATA_DEPTH, ZXNPU_DATA_BUF_SIZE);
}

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

static void zxnpu_wlan_mcu_dl(struct zxnpu_wlan *w, const char *args, bool commit)
{
	const struct firmware *fw;
	unsigned long dst;
	unsigned int offset = 0;
	u8 msg[24 + ZXNPU_MCU_CHUNK];
	unsigned int done = 0, total;
	char name[128];
	int ret;

	int nargs = sscanf(args, "%127s %lx %u", name, &dst, &offset);

	if (nargs < 1) {
		zxnpu_wlan_append(w, "usage: mcu-dl <file> [dst-hex] [offset]\n");
		return;
	}
	ret = request_firmware_direct(&fw, name, &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "mcu-dl: %s missing (%d)\n", name, ret);
		return;
	}
	if (nargs < 2) {
		/* parse_patch_v1 strips exactly 30 header bytes: the image sent to
		 * the MCU starts at +0x1E and still contains the 0x11223344 word. */
		if (fw->size > ZXNPU_PATCH_HDR &&
		    get_unaligned_le32(fw->data + 0x20) == 0x11223344) {
			dst = get_unaligned_le32(fw->data + 0x24);
			offset = ZXNPU_PATCH_HDR;
			zxnpu_wlan_append(w, "  auto: patch dst=%08lx offset=%d\n",
					  dst, ZXNPU_PATCH_HDR);
		} else {
			zxnpu_wlan_append(w, "mcu-dl: no dst given and no patch header\n");
			release_firmware(fw);
			return;
		}
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
		if (zxnpu_mcu_submit_ring(w, ZXNPU_FWDL_RING_NR, msg, 24 + chunk, true, false))
			goto out;
		done += chunk;
		if (!(done % (64 * ZXNPU_MCU_CHUNK)))
			zxnpu_wlan_append(w, "  ... %u/%u\n", done, total);
		usleep_range(1000, 2000);
	}

	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(4, msg + 0);
	msg[2] = ZXNPU_MCU_TYPE_FINISH;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	if (zxnpu_mcu_submit(w, msg, 28, true, true))
		goto out;
	zxnpu_wlan_append(w, "mcu-dl: %u bytes sent, finish issued\n", done);
out:
	release_firmware(fw);
}

/*
 * parse_fw_v3 (mt7916.ko 0x1D6858) + load_fw_v1 (0x1D5A48) in one place.
 *
 * The image ends with a 36-byte trailer and n_region 40-byte descriptors
 * (mt76's struct mt76_connac2_fw_region); descriptors are walked backwards
 * from the trailer and the payloads sit contiguously at the front of the
 * file:
 *
 *   [payload 0][payload 1]...[padding][descriptor x n][trailer]
 *
 * feature_set bits: 0 encrypt, 1..2 key idx, 4 encr mode, 5 override addr,
 * 6 non-downloadable. load_fw_v1 turns those into the AddressLen mode word
 * 0x80000000 | (fs & 7) | (fs & 1 ? 8) | (fs & 0x10 ? 0x40) and band 1
 * (the WA image) adds 0x10, while FwStart gets bit0 (override) and bit2
 * (WA working PDA CR4). This matches upstream mt76_connac2_load_ram.
 */
struct zxnpu_fw_region {
	u32 addr;
	u32 len;
	u32 off;
	u8 feature;
};

static int zxnpu_fw_parse(const struct firmware *fw,
			  struct zxnpu_fw_region *regs, int max,
			  char *ver, size_t verlen, char *date, size_t datelen)
{
	const u8 *t, *tab;
	u32 off = 0;
	int i, n;

	if (fw->size < 36)
		return -EINVAL;
	t = fw->data + fw->size - 36;
	if (t[3] != 2)
		return -EINVAL;
	n = t[2];
	if (!n || n > max || fw->size < 36 + (size_t)n * 40)
		return -EINVAL;
	tab = t - n * 40;
	for (i = 0; i < n; i++) {
		const u8 *e = tab + i * 40;

		regs[i].addr = get_unaligned_le32(e + 16);
		regs[i].len = get_unaligned_le32(e + 20);
		regs[i].feature = e[24];
		regs[i].off = off;
		if (off + regs[i].len > fw->size)
			return -EINVAL;
		off += regs[i].len;
	}
	snprintf(ver, verlen, "%.10s", (const char *)t + 7);
	snprintf(date, datelen, "%.15s", (const char *)t + 17);
	return n;
}

static u32 zxnpu_fw_mode(u8 feature, bool wa)
{
	u32 mode = 0x80000000u | (feature & 0x07);

	if (feature & 0x01)
		mode |= 0x08;
	if (feature & 0x10)
		mode |= 0x40;
	if (wa)
		mode |= 0x10;
	return mode;
}

static int zxnpu_mcu_addrlen(struct zxnpu_wlan *w, u32 addr, u32 len,
			     u32 mode, bool commit)
{
	u8 msg[24 + 12];

	memset(msg, 0, sizeof(msg));
	put_unaligned_le16(12, msg + 0);
	msg[2] = (addr == 0x04000000) ? 0x05 : 0x01;
	msg[4] = ZXNPU_MCU_CMD_PATCH;
	put_unaligned_le32(addr, msg + 24);
	put_unaligned_le32(len, msg + 28);
	put_unaligned_le32(mode, msg + 32);
	return zxnpu_mcu_submit(w, msg, sizeof(msg), commit, false);
}

static int zxnpu_mcu_scatter(struct zxnpu_wlan *w, const u8 *data, u32 len,
			     bool commit)
{
	u8 msg[24 + ZXNPU_MCU_CHUNK];
	u32 done = 0;

	while (done < len) {
		u32 chunk = min_t(u32, ZXNPU_MCU_CHUNK, len - done);

		memset(msg, 0, 24 + chunk);
		put_unaligned_le16(chunk, msg + 0);
		msg[2] = ZXNPU_MCU_TYPE_SCATTER;
		msg[4] = ZXNPU_MCU_CMD_SCATTER;
		memcpy(msg + 24, data + done, chunk);
		if (zxnpu_mcu_submit_ring(w, ZXNPU_FWDL_RING_NR, msg, 24 + chunk, commit, false))
			return -EIO;
		done += chunk;
		if (!commit)
			break;
		usleep_range(500, 1000);
	}
	return 0;
}

static int zxnpu_fw_send(struct zxnpu_wlan *w, const struct firmware *fw,
			 bool wa, bool commit)
{
	struct zxnpu_fw_region regs[ZXNPU_FW_MAX_REGIONS];
	char ver[16], date[24];
	u32 override = 0, option;
	int i, n;

	n = zxnpu_fw_parse(fw, regs, ZXNPU_FW_MAX_REGIONS, ver, sizeof(ver),
			   date, sizeof(date));
	if (n < 0) {
		zxnpu_wlan_append(w, "  bad firmware image (%d)\n", n);
		return n;
	}
	zxnpu_wlan_append(w, "  %s %s%s: %d regions\n", wa ? "WA" : "WM",
			  ver, date, n);
	for (i = 0; i < n; i++) {
		struct zxnpu_fw_region *r = &regs[i];
		u32 mode = zxnpu_fw_mode(r->feature, wa);
		int ret;

		zxnpu_wlan_append(w, "  [%d] addr=%08x len=%u off=%u fs=%02x mode=%08x\n",
				  i, r->addr, r->len, r->off, r->feature, mode);
		if (r->feature & 0x40)
			continue;
		if (r->feature & 0x20)
			override = r->addr;
		ret = zxnpu_mcu_addrlen(w, r->addr, r->len, mode, commit);
		if (ret)
			return ret;
		if (!commit)
			continue;
		msleep(10);
		ret = zxnpu_mcu_scatter(w, fw->data + r->off, r->len, true);
		if (ret)
			return ret;
	}
	option = (override ? 0x1 : 0) | (wa ? 0x4 : 0);
	zxnpu_wlan_append(w, "  fw-start option=%08x override=%08x\n",
			  option, override);
	{
		u8 msg[24 + 8];

		memset(msg, 0, sizeof(msg));
		put_unaligned_le16(8, msg + 0);
		msg[2] = 0x02;
		msg[4] = ZXNPU_MCU_CMD_PATCH;
		put_unaligned_le32(option, msg + 24);
		put_unaligned_le32(override, msg + 28);
		return zxnpu_mcu_submit(w, msg, sizeof(msg), commit, true);
	}
}

/* fw-dl wm|wa: patch sem, region download and FwStart for one MCU image. */
static void zxnpu_wlan_fw_dl(struct zxnpu_wlan *w, const char *args, bool commit)
{
	const struct firmware *fw;
	char name[16];
	bool wa;
	int ret;

	if (sscanf(args, "%15s", name) != 1 ||
	    (strcmp(name, "wm") && strcmp(name, "wa"))) {
		zxnpu_wlan_append(w, "usage: fw-dl wm|wa\n");
		return;
	}
	wa = !strcmp(name, "wa");
	ret = request_firmware_direct(&fw, wa ? ZXNPU_WA_FW : ZXNPU_WM_FW,
				      &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "fw-dl: %s missing (%d)\n",
				  wa ? ZXNPU_WA_FW : ZXNPU_WM_FW, ret);
		return;
	}
	zxnpu_wlan_append(w, "fw-dl %s (%zu B) %s\n", name, fw->size,
			  commit ? "COMMIT" : "dry run");
	ret = zxnpu_fw_send(w, fw, wa, commit);
	if (ret) {
		zxnpu_wlan_append(w, "fw-dl: failed at %d\n", ret);
	} else if (commit) {
		u32 st = 0;
		int i;

		for (i = 0; i < 150; i++) {
			st = zxnpu_fw_state_read(w);
			if (st == 3 || st == 7)
				break;
			msleep(10);
		}
		zxnpu_wlan_append(w, "  fw-state after start: %u%s\n", st,
				  st == 7 ? " (WM+WA ready)" :
				  st == 3 ? " (normal operation)" : "");
	}
	release_firmware(fw);
}

/* fw-plan wm|wa: offline decode of the trailer and region table. */
static void zxnpu_wlan_fw_plan(struct zxnpu_wlan *w, const char *args)
{
	struct zxnpu_fw_region regs[ZXNPU_FW_MAX_REGIONS];
	const struct firmware *fw;
	char name[16], ver[16], date[24];
	bool wa;
	int i, n, ret;

	if (sscanf(args, "%15s", name) != 1 ||
	    (strcmp(name, "wm") && strcmp(name, "wa"))) {
		zxnpu_wlan_append(w, "usage: fw-plan wm|wa\n");
		return;
	}
	wa = !strcmp(name, "wa");
	ret = request_firmware_direct(&fw, wa ? ZXNPU_WA_FW : ZXNPU_WM_FW,
				      &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "fw-plan: %s missing (%d)\n",
				  wa ? ZXNPU_WA_FW : ZXNPU_WM_FW, ret);
		return;
	}
	zxnpu_wlan_append(w, "fw-plan %s (%zu B)\n", name, fw->size);
	n = zxnpu_fw_parse(fw, regs, ZXNPU_FW_MAX_REGIONS, ver, sizeof(ver),
			   date, sizeof(date));
	if (n < 0) {
		zxnpu_wlan_append(w, "  bad firmware image (%d)\n", n);
		release_firmware(fw);
		return;
	}
	zxnpu_wlan_append(w, "  ver=%s date=%s regions=%d\n", ver, date, n);
	for (i = 0; i < n; i++)
		zxnpu_wlan_append(w, "  [%d] addr=%08x len=%u off=%u fs=%02x%s\n",
				  i, regs[i].addr, regs[i].len, regs[i].off,
				  regs[i].feature,
				  regs[i].feature & 0x40 ? " (skip)" : "");
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

/*
 * Event-side expectations, from the vendor event decode (doc 37 §2.2):
 * mtf_rx_event_handler (0x1B5E54, arch_ops[448]) reads the event header as
 * +2 = eid (0xEE/0xED vendor specials, 0x17-family responses to the FWDL
 * commands), +3/+4 = seq/status fields; AndesMTRxProcessEvent is the FWDL
 * response matcher. This prints what each download stage should observe in
 * the event ring so the operator can match `mcu-recv` frames against it —
 * pure documentation in proc form, no waiting, no behavior change.
 */
static void zxnpu_wlan_ack_expect(struct zxnpu_wlan *w)
{
	zxnpu_wlan_append(w, "ack expectations (doc 37 §2.2 decode):\n");
	zxnpu_wlan_append(w, "  event hdr: +2=eid, +3=seq/flag, +4=status (24B Andes header mirror)\n");
	zxnpu_wlan_append(w, "  FWDL sequence expectations, in order:\n");
	zxnpu_wlan_append(w, "    SemGet(type 0x10)      -> rsp: semaphore granted\n");
	zxnpu_wlan_append(w, "    AddressLen(type 0x01)  -> rsp per scatter segment (check fw-dl didx advance)\n");
	zxnpu_wlan_append(w, "    Finish(type 0x07)      -> rsp: ROM patch latched\n");
	zxnpu_wlan_append(w, "    WM: 9 segments, FwStart option=00000001 override=0220a800\n");
	zxnpu_wlan_append(w, "    WA: 3 segments, FwStart option=00000004\n");
	zxnpu_wlan_append(w, "  fw-state transition: 1 (download) -> 7 (WM+WA ready)\n");
	zxnpu_wlan_append(w, "  match frames from `mcu-recv` against +2 eid; 0xEE/0xED = vendor specials\n");
}

/*
 * L1 profile resolution (doc 40, R2): the vendor driver hardcodes
 * /etc/wireless/mt7916/l1profile.dat.dbdc (mt7916.ko strings section, with
 * "load l1profile succeed/failed" logs) and walks the INDEX<N>_* keys —
 * per-band driver profile paths, init scripts, the EEPROM source
 * (name/offset/size), interface naming and the SKU tables. We ship the same
 * files under zte/mt7916/; this command resolves them the vendor way and
 * prints the per-band bring-up plan. Read-only file parsing.
 */
static void zxnpu_wlan_l1profile(struct zxnpu_wlan *w)
{
	static const char * const keys[] = {
		"INDEX0_profile_path", "INDEX0_init_script",
		"INDEX0_EEPROM_offset", "INDEX0_EEPROM_size",
		"INDEX0_EEPROM_name", "INDEX0_main_ifname",
		"INDEX0_ext_ifname", "INDEX0_single_sku_path",
		"INDEX0_bf_sku_path",
	};
	static const char * const bandfw[2] = {
		"zte/mt7916/mt7916.dbdc.b0.dat",
		"zte/mt7916/mt7916.dbdc.b1.dat",
	};
	const struct firmware *fw;
	const u8 *p, *end;
	int i, b, ret;

	ret = request_firmware_direct(&fw, "zte/mt7916/l1profile.dat.dbdc",
				      &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "l1profile: missing (%d)@N@", ret);
		return;
	}
	zxnpu_wlan_append(w, "l1profile: %zu bytes (INDEX0 MT7916 dbdc)@N@",
			  fw->size);
	p = fw->data;
	end = fw->data + fw->size;
	while (p < end) {
		const u8 *nl = memchr(p, '\n', end - p);
		size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (len && p[0] != '#') {
			for (i = 0; i < ARRAY_SIZE(keys); i++) {
				size_t klen = strlen(keys[i]);

				if (len > klen + 1 &&
				    !strncmp(p, keys[i], klen) &&
				    p[klen] == '=')
					zxnpu_wlan_append(w, "  %.*s@N@",
							  (int)len, p);
			}
		}
		p += len + (nl ? 1 : 0);
	}
	release_firmware(fw);

	/* The per-band driver profiles profile_path points at; shipped
	 * alongside the l1 profile. Report their shape (line/key counts) —
	 * the operational defaults they carry are a P5 wlanmgr input.
	 */
	for (b = 0; b < 2; b++) {
		const struct firmware *pf;
		int lines = 0, kvs = 0;

		ret = request_firmware_direct(&pf, bandfw[b], &w->pdev->dev);
		if (ret) {
			zxnpu_wlan_append(w, "  band%d profile: missing (%d)@N@",
					  b, ret);
			continue;
		}
		p = pf->data;
		end = pf->data + pf->size;
		while (p < end) {
			const u8 *nl = memchr(p, '\n', end - p);
			size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

			if (len && p[0] != '#') {
				lines++;
				if (memchr(p, '=', len))
					kvs++;
			}
			p += len + (nl ? 1 : 0);
		}
		zxnpu_wlan_append(w, "  band%d profile: %zu bytes, %d lines, %d keys@N@",
				  b, pf->size, lines, kvs);
		release_firmware(pf);
	}
	zxnpu_wlan_append(w, "  note: init_script (mtwifi.lua) is vendor userspace; native wlanmgr replaces it in P5@N@");
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
	} else if (!strncmp(cmd, "fw-state", 8)) {
		zxnpu_wlan_fw_state(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "fw-plan ", 8)) {
		zxnpu_wlan_fw_plan(w, cmd + 8);
	} else if (!strncmp(cmd, "fw-dl ", 6)) {
		zxnpu_wlan_fw_dl(w, cmd + 6, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "fw-start ", 9)) {
		zxnpu_wlan_fw_start(w, cmd + 9, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "fw-info", 7)) {
		zxnpu_wlan_fw_info(w, cmd + 7);
	} else if (!strncmp(cmd, "fw", 2)) {
		zxnpu_wlan_fw_detail(w);
	} else if (!strncmp(cmd, "plan", 4)) {
		zxnpu_wlan_wfdma(w, false);
	} else if (!strncmp(cmd, "mcu", 3)) {
		zxnpu_wlan_mcu_plan(w);
	} else if (!strncmp(cmd, "l1profile", 9)) {
		zxnpu_wlan_l1profile(w);
	} else if (!strncmp(cmd, "ack-expect", 10)) {
		zxnpu_wlan_ack_expect(w);
	} else if (!strncmp(cmd, "rings", 5)) {
		zxnpu_wlan_rings(w);
	} else if (!strncmp(cmd, "mcuwin", 6)) {
		zxnpu_wlan_mcuwin(w);
	} else if (!strncmp(cmd, "mcu-cmd ", 8)) {
		zxnpu_wlan_mcu_cmd(w, cmd + 8, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl-regions ", 15)) {
		zxnpu_wlan_mcu_regions(w, cmd + 15, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl ", 7)) {
		zxnpu_wlan_mcu_dl(w, cmd + 7, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "rx-init", 7)) {
		zxnpu_wlan_rx_init(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-recv", 8)) {
		zxnpu_wlan_mcu_recv(w);
	} else if (!strncmp(cmd, "data-init", 9)) {
		zxnpu_wlan_data_init(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "data-recv", 9)) {
		zxnpu_wlan_data_recv(w);
	} else if (!strncmp(cmd, "data-tx", 7)) {
		zxnpu_wlan_data_tx(w);
	} else if (!strncmp(cmd, "dataplane", 9)) {
		zxnpu_wlan_dataplane(w);
	} else if (!strncmp(cmd, "mcu-send", 8)) {
		zxnpu_wlan_mcu_send(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "commit", 6)) {
		commit = zxnpu_wlan_commit;
		if (!commit)
			zxnpu_wlan_append(w, "commit refused: set zxnpu_wlan.commit=1\n");
		zxnpu_wlan_wfdma(w, commit);
	} else {
		zxnpu_wlan_append(w, "commands: l1profile | ack-expect | data-init | data-recv | dataplane | probe | fw | plan | mcu | rings | mcuwin | mcu-send | mcu-cmd | mcu-dl | mcu-dl-regions | fw-start | fw-info | fw-state | fw-plan wm|wa | fw-dl wm|wa | rx-init | mcu-recv | commit\n");
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
	w->fwdl_ring = dmam_alloc_coherent(&pdev->dev,
					   ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_DESC_SIZE,
					   &w->fwdl_ring_dma, GFP_KERNEL);
	w->fwdl_buf = dmam_alloc_coherent(&pdev->dev, ZXNPU_MCU_BUF_SIZE,
					  &w->fwdl_buf_dma, GFP_KERNEL);
	w->rx_ring = dmam_alloc_coherent(&pdev->dev,
					 ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_DESC_SIZE,
					 &w->rx_ring_dma, GFP_KERNEL);
	w->rx_buf = dmam_alloc_coherent(&pdev->dev,
					ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_RX_BUF_SIZE,
					&w->rx_buf_dma, GFP_KERNEL);
	w->dtx_ring = dmam_alloc_coherent(&pdev->dev,
					  ZXNPU_DATA_DEPTH * ZXNPU_MCU_DESC_SIZE,
					  &w->dtx_ring_dma, GFP_KERNEL);
	w->drx_ring = dmam_alloc_coherent(&pdev->dev,
					  ZXNPU_DATA_DEPTH * ZXNPU_MCU_DESC_SIZE,
					  &w->drx_ring_dma, GFP_KERNEL);
	w->drx_buf = dmam_alloc_coherent(&pdev->dev,
					 ZXNPU_DATA_DEPTH * ZXNPU_DATA_BUF_SIZE,
					 &w->drx_buf_dma, GFP_KERNEL);
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
