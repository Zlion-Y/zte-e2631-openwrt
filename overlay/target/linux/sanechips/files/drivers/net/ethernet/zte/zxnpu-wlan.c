// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S vendor-native MT7916 (CSP_WLAN) driver, R-A stage.
 *
 * Binds both PCIe functions of the MT7916 die and follows mt76's split:
 * 14c3:7906 is the main function that owns the hardware (BAR0, the wfsys
 * reset that makes the MCU re-execute its ROM code, and the WFDMA rings),
 * 14c3:790a is hif2 only. With enable=0 the driver parks the device (probe
 * succeeds, the proc/VAP control plane exists, no PCI or BAR access), and
 * even when enabled it writes no register during probe: the reset and the
 * WFDMA bring-up are commit-gated ctl commands so a wedge cannot turn into
 * a boot loop.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/iopoll.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define ZXNPU_WLAN_VENDOR	0x14c3
#define ZXNPU_WLAN_DEVICE	0x790a
#define ZXNPU_WLAN_DEVICE_MAIN	0x7906

/* MT7916 MAC CR space starts at 0x18000000 and BAR0 maps it 1:1. */
#define ZXNPU_CR_BASE		0x18000000u
#define ZXNPU_CR(off)		((off) - ZXNPU_CR_BASE)
#define ZXNPU_WFDMA_GLO_CFG	0x18024208
#define ZXNPU_WFDMA_INT_ENA	0x18024240
#define ZXNPU_WFDMA_RING_LO	0x18024640	/* six rings 0x18024640-0x18024650 */
#define ZXNPU_WFDMA_RING_HI	0x18024680	/* six rings 0x18024680-0x18024694 */
#define ZXNPU_WFDMA_RING_ENA	0x18024260
/* mt7916_reg[] (mt76 mmio.c): INT_SOURCE_CSR=0xd4200 is write-1-clear;
 * mt7915_init_hardware clears all pending sources before arming DMA.
 * WFDMA0_RST=0x18024100: bits 4/5 (LOGIC_RST/DMASHDL_ALL_RST) read back
 * set on the live vendor box - reset-asserted-on-write, self-clearing
 * engine reset per mt7915_dma_disable(rst=true). */
#define ZXNPU_WFDMA_INT_SOURCE	0x18024200
#define ZXNPU_WFDMA_RST		0x18024100
#define ZXNPU_WFDMA_RST_ALL	0x00000030u
#define ZXNPU_WFDMA_PCIE_CFG	0x180242f4
#define ZXNPU_WFDMA_PCIE_CFG2	0x180282f4
#define ZXNPU_WFDMA_TX_BASE	0x1802702c
#define ZXNPU_WFDMA_RX_BASE	0x18027030

static bool zxnpu_wlan_enable;
module_param_named(enable, zxnpu_wlan_enable, bool, 0644);
MODULE_PARM_DESC(enable, "probe both MT7916 PCIe functions (7906 main + 790a comp) and audit firmware files");

static bool zxnpu_wlan_commit;
module_param_named(commit, zxnpu_wlan_commit, bool, 0644);
MODULE_PARM_DESC(commit, "allow the WFDMA init plan to actually write registers");

#define ZXNPU_VAP_MAX		2
struct zxnpu_vap;

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
	/* Per-slot data buffers: every descriptor must own its bytes.
	 * A single shared buffer is rewritten while earlier descriptors
	 * are still pending, and with a 128-slot ring the MCU reads
	 * self-contradictory data and stops consuming (doc 62 s59). */
	u32 ring_stall;		/* consecutive submits without didx progress */
	void *mcu_bufs;
	dma_addr_t mcu_bufs_dma;
	void *fwdl_bufs;
	dma_addr_t fwdl_bufs_dma;
	u32 fwdl_cidx;
	bool fwdl_ring_armed;
	bool mcu_ring_armed;
	void *rx_ring;		/* 32 x 36 descriptors */
	dma_addr_t rx_ring_dma;
	void *rx_buf;		/* 32 x 1700 buffers with 16B RXD */
	dma_addr_t rx_buf_dma;
	u32 rx_cidx;
	void *dtx_ring;		/* qid18 / BAND0 TX data ring (empty: TXD undecoded) */
	void *dband_ring;	/* qid19 / BAND1 TX data ring (empty: TXD undecoded) */
	dma_addr_t dband_ring_dma;
	dma_addr_t dtx_ring_dma;
	void *drx_ring;		/* qid1 RX data ring, 64 x 36 descriptors */
	dma_addr_t drx_ring_dma;
	void *drx_buf;		/* 64 x 1700 buffers with 16B RXD */
	dma_addr_t drx_buf_dma;
	u32 drx_cidx;
	struct zxnpu_vap *vaps[ZXNPU_VAP_MAX];
	char status[4096];
	/* persistent probe evidence (identity, BARs, firmware CRCs): status[]
	 * is per-ctl-command scratch and used to lose this on the first
	 * command after boot. */
	char probe_log[2048];
	bool is_main;		/* 7906 main function vs 790a companion */
};

/* The MT7916 card exposes two PCIe functions of one die: 7906 (main) and
 * 790a (companion). The vendor reverse work suggests the connac CR bus and
 * the HIF ring window may split between them (doc 62 B1); the CR reads
 * through 790a all returned zero, so both functions are bound and probed
 * read-only to compare their windows side by side. */
static struct zxnpu_wlan *zxnpu_wlan_main;
static struct zxnpu_wlan *zxnpu_wlan_comp;

/* The control surface talks to the instance that owns the hardware: the
 * main function (7906). mt76's mt7915_pci_probe treats 7906 as the device
 * and 790a as hif2 only - and the wfsys reset, which is what makes the MCU
 * re-run its ROM code, is only performed on the main function. Everything
 * below therefore runs on zxnpu_wlan_main once it has probed; the companion
 * stays bound as evidence/secondary window. */
static struct zxnpu_wlan *zxnpu_wlan_active(void)
{
	return zxnpu_wlan_main ?: zxnpu_wlan_comp;
}

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

/* same bounds, but into the persistent probe log */
static void zxnpu_wlan_pl_append(struct zxnpu_wlan *w, const char *fmt, ...)
{
	va_list args;
	size_t len = strlen(w->probe_log);

	if (len >= sizeof(w->probe_log) - 1)
		return;
	va_start(args, fmt);
	vsnprintf(w->probe_log + len, sizeof(w->probe_log) - len, fmt, args);
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
			zxnpu_wlan_pl_append(w, "fw %s: missing (%d)\n",
					     zxnpu_fw_names[i], ret);
			continue;
		}
		zxnpu_wlan_pl_append(w, "fw %s: %zu bytes crc32=%08x\n",
				     zxnpu_fw_names[i], fw->size,
				     crc32_le(~0U, fw->data, fw->size) ^ ~0U);
		release_firmware(fw);
	}
}

/* CR addressing, following mt76 (mt7915/mmio.c __mt7915_reg_addr +
 * __mt7915_reg_remap_addr) rather than a fixed window table:
 *
 *  - the WFDMA0 / WFDMA_EXT_CSR block (0x1802_xxxx) is decoded directly at
 *    bar0+0xd0000. That is the same translation as mt76's mt7916_reg[]
 *    (WFDMA0_ADDR = 0xd4000), and it is what the live vendor box answers:
 *    0x18024208 -> bar0+0xd4208;
 *  - everything else goes through the L1 window: write (addr >> 16) into
 *    MT_HIF_REMAP_L1_MT7916[15:0], read it back so the write is pushed
 *    through the HIF, then touch bar0+0xe0000+(addr & 0xffff).
 *
 * The boot ROM leaves L1 pointed at 0x1806, which is why a fixed table
 * (0x1806->0xe0000, 0x1800->0xf0000) looked right: MT_TOP_MISC read back
 * fine while MT_TOP_PWR_CTRL through the invented 0xf0000 window was dead.
 * The wfsys reset needs L1 base 0x7000 (MT_WF_SUBSYS_RST 0x7000_2600) and
 * PCIe interrupt enable needs 0x7403, so the base has to be per access.
 */
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

#define ZXNPU_HIF_REMAP_L1_MT7916	0x000fe260
#define ZXNPU_HIF_REMAP_BASE_L1		0x000e0000

/* Point the L1 window at a 64 KiB segment. mt76 keeps the high half of the
 * register intact (it holds a second field), writes only [15:0] and reads
 * back to push. */
static void zxnpu_l1_select(struct zxnpu_wlan *w, u16 base)
{
	u32 v = zxnpu_bar0_read(w, ZXNPU_HIF_REMAP_L1_MT7916);

	if ((v & 0xffff) == base)
		return;
	zxnpu_bar0_write(w, ZXNPU_HIF_REMAP_L1_MT7916,
			 (v & 0xffff0000) | base);
	zxnpu_bar0_read(w, ZXNPU_HIF_REMAP_L1_MT7916);
}

/* Directly decoded bar0 block (no remap register involved). */
static bool zxnpu_cr_direct(u32 cr)
{
	return (cr & 0xffff0000) == 0x18020000;
}

/* mt7916_reg_map[] (mt76 mt7915/mmio.c): fixed physical->bar0 translations
 * for the blocks that are cheaper to decode directly than through L1. Only
 * the entries this driver needs so far; get_mac/efuse work will add the WF
 * LMAC blocks.
 */
struct zxnpu_reg_map {
	u32 phys;
	u32 maps;
	u32 size;
};
static const struct zxnpu_reg_map zxnpu_reg_map[] = {
	{ 0x00400000, 0x80000, 0x10000 },	/* WF_MCU_SYSRAM */
	{ 0x00410000, 0x90000, 0x10000 },	/* WF_MCU_SYSRAM (configure cr) */
	{ 0x820c0000, 0x08000, 0x04000 },	/* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x0c000, 0x02000 },	/* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x0e000, 0x02000 },	/* WF_UMAC_TOP (PP) */
	{ 0x820d0000, 0x30000, 0x10000 },	/* WF_LMAC_TOP (WF_WTBLON) */
};

/* bar0 offset from the map table, or -1 when the address is not covered. */
static u32 zxnpu_cr_mapped_off(u32 cr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(zxnpu_reg_map); i++) {
		u32 ofs;

		if (cr < zxnpu_reg_map[i].phys)
			continue;
		ofs = cr - zxnpu_reg_map[i].phys;
		if (ofs >= zxnpu_reg_map[i].size)
			continue;
		return zxnpu_reg_map[i].maps + ofs;
	}
	return (u32)-1;
}

/* bar0 offset for the directly decoded block; -1 for everything else. */
static u32 zxnpu_cr_bar_off(u32 cr)
{
	if (zxnpu_cr_direct(cr))
		return 0xd0000 + (cr & 0xffff);
	return (u32)-1;
}

/* CR access through the MAIN (7906) function's BAR: the live vendor box
 * maps every ring register there (doc 62 §39); the companion's BAR0
 * decodes a different window and its DMA never sees our descriptors.
 * Falls back to the local BAR before the main function has probed. */
static u32 zxnpu_cr_read(struct zxnpu_wlan *w, u32 cr);
static void zxnpu_cr_write(struct zxnpu_wlan *w, u32 cr, u32 val);
static u32 zxnpu_main_cr_read(struct zxnpu_wlan *w, u32 cr)
{
	if (zxnpu_wlan_main && zxnpu_wlan_main->bar[0] && !w->is_main)
		return zxnpu_cr_read(zxnpu_wlan_main, cr);
	return zxnpu_cr_read(w, cr);
}
static void zxnpu_main_cr_write(struct zxnpu_wlan *w, u32 cr, u32 val)
{
	if (zxnpu_wlan_main && zxnpu_wlan_main->bar[0] && !w->is_main) {
		zxnpu_cr_write(zxnpu_wlan_main, cr, val);
		return;
	}
	zxnpu_cr_write(w, cr, val);
}

static u32 zxnpu_cr_read(struct zxnpu_wlan *w, u32 cr)
{
	u32 off;

	if (!w->bar[0])
		return 0;
	if (!zxnpu_cr_direct(cr)) {
		off = zxnpu_cr_mapped_off(cr);
		if (off == (u32)-1) {
			zxnpu_l1_select(w, cr >> 16);
			off = ZXNPU_HIF_REMAP_BASE_L1 + (cr & 0xffff);
		}
	} else {
		off = 0xd0000 + (cr & 0xffff);
	}
	if (off + 4 > 0x100000)
		return 0;
	return readl(w->bar[0] + off);
}

static void zxnpu_cr_write(struct zxnpu_wlan *w, u32 cr, u32 val)
{
	u32 off;

	if (!w->bar[0])
		return;
	if (!zxnpu_cr_direct(cr)) {
		off = zxnpu_cr_mapped_off(cr);
		if (off == (u32)-1) {
			zxnpu_l1_select(w, cr >> 16);
			off = ZXNPU_HIF_REMAP_BASE_L1 + (cr & 0xffff);
		}
	} else {
		off = 0xd0000 + (cr & 0xffff);
	}
	if (off + 4 > 0x100000)
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
#define ZXNPU_TOP_MISC			0x180600f0

static u32 zxnpu_fw_state_read(struct zxnpu_wlan *w)
{
	/* MT_TOP_MISC lives in the 0x1806 segment, so the accessor points
	 * the L1 window there itself - no manual remap bookkeeping. */
	return zxnpu_cr_read(w, ZXNPU_TOP_MISC) & 0x7;
}

static void zxnpu_wlan_fw_state(struct zxnpu_wlan *w, bool commit)
{
	u32 st;

	zxnpu_wlan_append(w, "fw-state: %s\n", commit ? "COMMIT" : "read only");
	zxnpu_wlan_append(w, "  remap %08x <- %04x, read %08x\n",
			  ZXNPU_HIF_REMAP_L1_MT7916, ZXNPU_TOP_MISC >> 16,
			  ZXNPU_HIF_REMAP_BASE_L1 + (ZXNPU_TOP_MISC & 0xffff));
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

/* out receives the text; hw supplies the register window (the two PCIe
 * functions of the MT7916 die may carry different bus windows - doc 62 B1). */
static void zxnpu_wlan_probe_regs(struct zxnpu_wlan *out,
				  struct zxnpu_wlan *hw)
{
	static const u32 ids[] = {
		0x18000000, 0x18000008, 0x18000010, 0x18000028,
		ZXNPU_WFDMA_GLO_CFG, ZXNPU_WFDMA_INT_ENA,
		ZXNPU_WFDMA_TX_BASE, ZXNPU_WFDMA_RX_BASE,
		ZXNPU_WFDMA_PCIE_CFG, ZXNPU_WFDMA_PCIE_CFG2,
	};
	int i;

	zxnpu_wlan_append(out, "cr probe (%s, bar0 len %llu):\n",
			  hw->is_main ? "7906 main" : "790a comp",
			  (unsigned long long)pci_resource_len(hw->pdev, 0));
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		zxnpu_wlan_append(out, "  cr[%08x]=%08x (bar0+%05x)\n", ids[i],
				  zxnpu_cr_read(hw, ids[i]),
				  zxnpu_cr_bar_off(ids[i]));
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
 * WFDMA ring registers: each TX/RX ring owns a 0x10-stride block of
 * BASE/CNT/CPU_IDX/DMA_IDX at 0x18024400 + n*0x10 (TX) and 0x18024500 +
 * n*0x10 (RX). The live vendor system's FWDL ring reads base=4a6c7000,
 * cnt=128, cidx=did=0x2e exactly in this layout (doc 62 §26); the 0x20
 * "stride" and the packed-pair region at 0x18024B00 were artifacts of a
 * strided scan and a misread base_hi (3.22-3.23).
 */
#define ZXNPU_WFDMA_TX_RING0	0x18024400
#define ZXNPU_WFDMA_RX_RING0	0x18024500
#define ZXNPU_WFDMA_RING_STRIDE	0x10
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

	zxnpu_wlan_append(w, "wfdma rings (0x10 stride BASE/CNT/CIDX/DIDX):\n");
	for (i = 0; i < ZXNPU_WFDMA_TX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "tx",
				     ZXNPU_WFDMA_TX_RING0 +
				     ZXNPU_WFDMA_RING_STRIDE * i);
	for (i = 0; i < ZXNPU_WFDMA_RX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "rx",
				     ZXNPU_WFDMA_RX_RING0 +
				     ZXNPU_WFDMA_RING_STRIDE * i);
	zxnpu_wlan_append(w, "wfdma rings, +0x4000 mirror:\n");
	for (i = 0; i < ZXNPU_WFDMA_TX_RINGS; i++)
		zxnpu_wlan_dump_ring(w, "tx",
				     ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_BAND1 +
				     ZXNPU_WFDMA_RING_STRIDE * i);
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
	/* MT7916 owns 64-bit BARs at indices 0, 2, 4; w->bar[i] mirrors that
	 * (table[1] is the upper half of BAR0 and was never requested). */
	static const unsigned int bar_idx[3] = { 0, 2, 4 };
	int i;

	zxnpu_wlan_append(w, "mcu window probe (read-only):\n");
	for (i = 0; i < 3; i++) {
		resource_size_t len = pci_resource_len(w->pdev, bar_idx[i]);
		unsigned long flags = pci_resource_flags(w->pdev, bar_idx[i]);

		if (!w->bar[i] || !(flags & IORESOURCE_MEM) || len < 0x10c) {
			zxnpu_wlan_append(w, "  bar%d: unmapped/short (len %llu flags %lx)\n",
					  bar_idx[i], (unsigned long long)len,
					  flags);
			continue;
		}
		zxnpu_wlan_append(w, "  bar%d: +0=%08x +8=%08x +0x108=%08x\n",
				  bar_idx[i],
				  readl(w->bar[i]),
				  readl(w->bar[i] + 8),
				  readl(w->bar[i] + 0x108));
	}
}

/*
 * Direct window readers (read-only, doc 62 B1 recon): the three BARs of
 * each function carry different bus windows - the 1MB BAR0 reads zero on
 * this cold chip while the 32KB BAR2 answers (0x00511163 / "GGGG" pattern).
 * `win <bar> <off> [count]` dumps dwords from any mapped bar; `scan` walks
 * every mapped bar of both functions sparsely and prints only non-zero
 * hits, so one command maps out where the chip actually answers.
 */
static void __iomem *zxnpu_wlan_bar_of(struct zxnpu_wlan *w, int bar,
				       resource_size_t *len)
{
	static const unsigned int ridx[3] = { 0, 2, 4 };
	void __iomem *p;

	if (bar < 0 || bar > 2)
		return NULL;
	p = w->bar[bar];
	if (p && len)
		*len = pci_resource_len(w->pdev, ridx[bar]);
	return p;
}

static void zxnpu_wlan_win_read(struct zxnpu_wlan *w, const char *args)
{
	unsigned int bar, count = 1;
	unsigned long off;
	void __iomem *p;
	resource_size_t len;
	int n;

	if (sscanf(args, "%u %lx %x", &bar, &off, &count) < 2 || bar > 2) {
		zxnpu_wlan_append(w, "usage: win <bar 0..2> <off-hex> [count <=64]\n");
		return;
	}
	/* dead-window reads can wedge the AXI bridge: the 3.14 incident ran
	 * ~3000 of them in a resched-less loop and hard-locked the SoC
	 * (console included). Keep every command small enough to bail out
	 * of, and yield between reads. */
	if (count > 64)
		count = 64;
	p = zxnpu_wlan_bar_of(w, bar, &len);
	if (!p || off + 4 > len) {
		zxnpu_wlan_append(w, "win: bar%u unmapped or offset %lx beyond %llu\n",
				  bar, off, (unsigned long long)len);
		return;
	}
	for (n = 0; n < count && off + 4 <= len; n++, off += 4) {
		zxnpu_wlan_append(w, "bar%u+%05lx = %08x\n",
				  bar, off, readl(p + off));
		cond_resched();
	}
}

/* Scan ONE bar of ONE function with a hard-bounded read budget. Never used
 * on the 1MB BAR0: dead-window reads wedge the AXI bridge (3.14 incident);
 * only the proven-alive 32KB BAR2 gets walked, sparsely. */
static void zxnpu_wlan_scan_bar(struct zxnpu_wlan *out, struct zxnpu_wlan *hw,
				u32 stride, u32 limit)
{
	void __iomem *p = hw->bar[1];	/* BAR2, 32KB */
	u32 off, hits = 0;

	if (!p)
		return;
	if (limit > 0x8000)
		limit = 0x8000;
	for (off = 0; off + 4 <= limit; off += stride) {
		u32 v = readl(p + off);

		if (v) {
			if (hits < 24)
				zxnpu_wlan_append(out, "  %s bar2+%04x = %08x\n",
						  hw->is_main ? "7906" : "790a",
						  off, v);
			hits++;
		}
		cond_resched();
	}
	zxnpu_wlan_append(out, "  %s bar2: %u non-zero hits (stride %u, range %x)\n",
			  hw->is_main ? "7906" : "790a", hits, stride, limit);
}

static void zxnpu_wlan_bus_scan(struct zxnpu_wlan *w)
{
	struct zxnpu_wlan *other = (w == zxnpu_wlan_main) ?
				   zxnpu_wlan_comp : zxnpu_wlan_main;

	zxnpu_wlan_append(w, "bus scan (BAR2 only - BAR0 is cold and reads there wedge the bridge):\n");
	zxnpu_wlan_scan_bar(w, w, 0x80, 0x8000);
	if (other)
		zxnpu_wlan_scan_bar(w, other, 0x80, 0x8000);
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
/* Runtime knob: the FWDL scatter data ring. qid16 is mt76's MT_MCUQ_FWDL,
 * but the live 3.31 tests showed qid16 never fetches while qid17 (WM) is
 * consumed command by command - the ROM-stage scatter may ride the WM ring
 * (the vendor's GetRealPortQueueID resolves it from chip_cap[0x162], which
 * we have not decoded). fw_ring lets the on-device test switch without a
 * rebuild. */
static int zxnpu_wlan_fw_ring = ZXNPU_FWDL_RING_NR;
module_param_named(fw_ring, zxnpu_wlan_fw_ring, int, 0644);
MODULE_PARM_DESC(fw_ring, "TX ring for FW_SCATTER data (0=qid16 FWDL, 1=qid17 WM)");
/* Commands go to qid17 (MCU_WM, ring 1) and firmware scatter data to qid16
 * (FWDL, ring 0) - that is mt76's split: mt7915_mcu_patch_sem_ctrl() and
 * friends use MT_MCUQ_WM, mt7915_mcu_send_firmware() uses MT_MCUQ_FWDL, and
 * the ROM answers the WM ring before any WM firmware runs (a working mt76
 * proves it). Older builds of this driver sent everything through ring 0. */
#define ZXNPU_MCU_RING_NR	1
#define ZXNPU_MCU_RING_REG	(ZXNPU_WFDMA_TX_RING0 + 0x10 * ZXNPU_MCU_RING_NR)
#define ZXNPU_MCU_DESC_SIZE	16
/* The live vendor FWDL ring runs 128 deep (CNT=0x80 at 0x18024404,
 * doc 62 §26) - mirror it. */
#define ZXNPU_MCU_RING_DEPTH	128
#define ZXNPU_MCU_BUF_SIZE	4096
#define ZXNPU_MCU_TYPE_PATCH	0x10
#define ZXNPU_MCU_CMD_PATCH	0x17
#define ZXNPU_MCU_TYPE_SCATTER	0xEE
#define ZXNPU_MCU_CMD_SCATTER	0x10
#define ZXNPU_MCU_TYPE_FINISH	0x07
#define ZXNPU_MCU_TYPE_ADDR	0x01
#define ZXNPU_MCU_CHUNK		512
#define ZXNPU_MCU_SLOT		(ZXNPU_MCU_CHUNK + 64)	/* room for the 32B Andes header */
#define ZXNPU_WM_FW		"zte/mt7916/WIFI_RAM_CODE_MT7916.bin"
#define ZXNPU_WA_FW		"zte/mt7916/7916_WACPU_RAM_CODE_release.bin"
#define ZXNPU_PATCH_FW		"zte/mt7916/mt7916_patch_e1_hdr.bin"
#define ZXNPU_FW_MAX_REGIONS	16
#define ZXNPU_PATCH_HDR		30

/* The 36B ring item has two overlapping views: the host-side bookkeeping
 * (pci_init_txrx_ring_mem: [0]=16, [4]=cur VA, [12]=skb, [28]=next) and the
 * hardware view the WFDMA DMA actually reads, written by
 * pci_kick_out_cmd_msg (0x1E19C0):
 *   word0 (+0)  = payload buffer DMA address (copied from item +20)
 *   word1 (+4)  = 0x40 << 24 | len << 16 - byte7 bit6 is the last-segment
 *                 flag, bytes 6-7 carry the length (same encoding as the
 *                 RXD buffer header, doc 14 §6.4)
 * The old layout wrote the bookkeeping view into the hardware words, so the
 * DMA fetched a descriptor whose buffer address and length were garbage and
 * DIDX never moved (3.19). */
/* The WFDMA DMA consumes 16-BYTE descriptors (mt76 struct mt76_desc,
 * authoritative field order):
 *   +0 buf0 = payload DMA, +4 ctrl = SD_LEN0(len<<16, bits 29-16) |
 *   LAST_SEC0(bit 30), +8 buf1 = 0, +12 info = addr-high (0 on 32-bit).
 * (3.25 had ctrl at +12 - wrong word; 3.19-3.24 had the right word but
 * a 36-byte stride.) */
static void zxnpu_mcu_fill_desc(void *ring, dma_addr_t ring_dma, u32 slot,
				u32 buf_dma, u32 len)
{
	u8 *d = (u8 *)ring + slot * ZXNPU_MCU_DESC_SIZE;

	memset(d, 0, ZXNPU_MCU_DESC_SIZE);
	put_unaligned_le32(buf_dma, d + 0);
	put_unaligned_le32(0x40000000u | ((len & 0x3fff) << 16), d + 4);
}

/* mt76_dma_queue_reset + sync_idx semantics for one TX ring: unused
 * descriptors carry DMA_DONE (bit31) so the engine skips them; indices
 * zeroed; ring_size/desc_base programmed last (mt76 order). Runs once
 * per ring via the armed flag. */
static void zxnpu_wlan_queue_init(struct zxnpu_wlan *w, int ring)
{
	void *desc = ring ? w->mcu_ring : w->fwdl_ring;
	dma_addr_t desc_dma = ring ? w->mcu_ring_dma : w->fwdl_ring_dma;
	u32 reg = ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_RING_STRIDE * ring;
	int i;

	if (ring ? w->mcu_ring_armed : w->fwdl_ring_armed)
		return;
	for (i = 0; i < ZXNPU_MCU_RING_DEPTH; i++)
		put_unaligned_le32(0x80000000u, (u8 *)desc +
				   i * ZXNPU_MCU_DESC_SIZE + 4);
	zxnpu_main_cr_write(w, reg + 8, 0);
	zxnpu_main_cr_write(w, reg + 12, 0);
	zxnpu_main_cr_write(w, reg + 4, ZXNPU_MCU_RING_DEPTH);
	zxnpu_main_cr_write(w, reg, (u32)desc_dma);
	if (ring)
		w->mcu_ring_armed = true;
	else
		w->fwdl_ring_armed = true;
}

/* ---------------------------------------------------------------------
 * wfsys reset and the WFDMA bring-up mt76 performs on the MAIN function.
 *
 * mt7915_wfsys_reset's mt7916 branch is a plain pulse of
 * MT_WF_SUBSYS_RST (0x7000_2600, CBTOP1 -> L1 base 0x7000). Releasing
 * it makes the MCU re-execute its ROM code, which is the precondition
 * for the whole FWDL handshake: until now this driver only ever bound
 * 790a (hif2, no reset), so the ROM never re-ran (doc 62 §43).
 *
 * The rest mirrors mt7915_dma_init/dma_enable/dma_start for this chip:
 *   dma_disable(rst) -> predicate the 0xd4xxx ring block, pulse the
 *   WFDMA reset, drop the DMA enable bits; then prefetch partitioning,
 *   BUSY_ENA, HIF idle poll, and finally TX/RX DMA enable.
 * --------------------------------------------------------------------- */
#define ZXNPU_WF_SUBSYS_RST	0x70002600
#define ZXNPU_PCIE_MAC_INT_EN	0x74030188	/* CBTOP1, L1 base 0x7403 */
#define ZXNPU_WFDMA_RST_DTX_PTR	0x1802420c
#define ZXNPU_WFDMA_PRI_DLY_0	0x180242f0
#define ZXNPU_WFDMA_PRI_DLY_1	0x180242f4
#define ZXNPU_WFDMA_PRI_DLY_2	0x180242f8
#define ZXNPU_WFDMA_BUSY_ENA	0x1802413c
#define ZXNPU_WFDMA_HIF_MISC	0x18027044
#define ZXNPU_WFDMA_GLO_TX_EN	BIT(0)
#define ZXNPU_WFDMA_GLO_RX_EN	BIT(2)
#define ZXNPU_WFDMA_GLO_OMIT_RX_PFET2	BIT(21)
#define ZXNPU_WFDMA_GLO_OMIT_RX_INFO	BIT(27)
#define ZXNPU_WFDMA_GLO_OMIT_TX_INFO	BIT(28)

/* Prefetch SRAM partitioning, mt7915_dma_prefetch for mt7916:
 * PREFETCH(base, depth) = base << 16 | depth. The last three entries use
 * base+0x40 on mt7916 because the MAIN_WA ring exists there. */
struct zxnpu_prefetch {
	u32 reg;
	u32 val;
};
static const struct zxnpu_prefetch zxnpu_prefetch[] = {
	{ 0x18024640, (0x000 << 16) | 0x4 },	/* MCUQ_FWDL  (qid16) */
	{ 0x18024644, (0x040 << 16) | 0x4 },	/* MCUQ_WM    (qid17) */
	{ 0x18024648, (0x080 << 16) | 0x4 },	/* TXQ BAND0  (qid18) */
	{ 0x1802464c, (0x0c0 << 16) | 0x4 },	/* TXQ BAND1  (qid19) */
	{ 0x18024650, (0x100 << 16) | 0x4 },	/* MCUQ_WA    (qid20) */
	{ 0x18024680, (0x140 << 16) | 0x4 },	/* RXQ MCU (WM) rxq0 */
	{ 0x18024684, (0x180 << 16) | 0x4 },	/* RXQ MCU_WA   rxq1 */
	{ 0x18024688, (0x1c0 << 16) | 0x4 },	/* RXQ MAIN_WA  rxq2 */
	{ 0x1802468c, (0x200 << 16) | 0x4 },	/* RXQ BAND1_WA rxq3 */
	{ 0x18024690, (0x240 << 16) | 0x4 },	/* RXQ MAIN     rxq4 */
	{ 0x18024694, (0x280 << 16) | 0x4 },	/* RXQ BAND1    rxq5 */
};

static void zxnpu_wlan_dma_disable(struct zxnpu_wlan *w, bool rst)
{
	u32 v;

	if (rst) {
		v = zxnpu_cr_read(w, ZXNPU_WFDMA_RST);
		zxnpu_cr_write(w, ZXNPU_WFDMA_RST, v & ~ZXNPU_WFDMA_RST_ALL);
		zxnpu_cr_write(w, ZXNPU_WFDMA_RST, v | ZXNPU_WFDMA_RST_ALL);
	}
	v = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);
	zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG,
		       v & ~(ZXNPU_WFDMA_GLO_TX_EN | ZXNPU_WFDMA_GLO_RX_EN |
			     ZXNPU_WFDMA_GLO_OMIT_TX_INFO |
			     ZXNPU_WFDMA_GLO_OMIT_RX_INFO |
			     ZXNPU_WFDMA_GLO_OMIT_RX_PFET2));
}

/*
 * The one command that changes the MCU's state. Dry run prints the plan
 * and the register values either side of the pulse; commit=1 does it.
 * Returns the FW_STATE before/after so the next step (DIDX advance) has a
 * baseline to compare against.
 */
static void zxnpu_wlan_wfsys_reset(struct zxnpu_wlan *w, bool commit)
{
	u32 before, after, rst;

	if (!w->bar[0]) {
		zxnpu_wlan_append(w, "wfsys-reset: parked, no BAR0 (enable=0)\n");
		return;
	}
	before = zxnpu_fw_state_read(w);
	rst = zxnpu_cr_read(w, ZXNPU_WF_SUBSYS_RST);
	zxnpu_wlan_append(w, "wfsys-reset (mt7916: MT_WF_SUBSYS_RST %08x): %s\n",
			  ZXNPU_WF_SUBSYS_RST, commit ? "COMMIT" : "dry run");
	zxnpu_wlan_append(w, "  fw-state before = %u\n", before);
	zxnpu_wlan_append(w, "  MT_WF_SUBSYS_RST pre = %08x\n", rst);
	if (!commit) {
		zxnpu_wlan_append(w, "  plan: set bit0, msleep(20), clear bit0, msleep(20)\n");
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to reset\n");
		return;
	}
	zxnpu_cr_write(w, ZXNPU_WF_SUBSYS_RST, rst | BIT(0));
	msleep(20);
	zxnpu_cr_write(w, ZXNPU_WF_SUBSYS_RST, rst & ~BIT(0));
	msleep(20);
	zxnpu_wlan_append(w, "  MT_WF_SUBSYS_RST post = %08x\n",
			  zxnpu_cr_read(w, ZXNPU_WF_SUBSYS_RST));
	after = zxnpu_fw_state_read(w);
	zxnpu_wlan_append(w, "  fw-state after = %u (%s)\n", after,
			  before == after ? "unchanged - MCU may still be in ROM" :
			  after == 1 ? "firmware download" :
			  after == 3 ? "normal operation" :
			  after == 7 ? "ready (WM+WA)" : "changed");
	/* The reset invalidates any ring the host armed before it, and the
	 * ROM zeroes the ring registers - the software counters must follow
	 * or the next submit kicks cpu_idx to a stale absolute value the
	 * engine reads as thousands of pending descriptors. */
	w->fwdl_ring_armed = false;
	w->mcu_ring_armed = false;
	w->fwdl_cidx = 0;
	w->mcu_cidx = 0;
}

/* mt7915_dma_init + dma_enable + dma_start, minus the queues this driver
 * does not use yet: predicate the ring block with a reset, program the
 * prefetch partitions, poll HIF idle, then turn the DMA engines on and
 * open the PCIe interrupt master switch. */
static void zxnpu_wlan_dma_init(struct zxnpu_wlan *w, bool commit)
{
	u32 glo, v, busy;
	int i;

	zxnpu_wlan_append(w, "dma-init (mt7916 WFDMA0 bring-up): %s\n",
			  commit ? "COMMIT" : "dry run");
	if (!w->bar[0]) {
		zxnpu_wlan_append(w, "  parked: no BAR0 (enable=0)\n");
		return;
	}
	zxnpu_wlan_append(w, "  fw-state = %u\n", zxnpu_fw_state_read(w));
	glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);
	zxnpu_wlan_append(w, "  %08x glo=%08x\n", ZXNPU_WFDMA_GLO_CFG, glo);
	for (i = 0; i < ARRAY_SIZE(zxnpu_prefetch); i++)
		zxnpu_wlan_append(w, "  prefetch %08x <- %08x\n",
				  zxnpu_prefetch[i].reg,
				  zxnpu_prefetch[i].val);
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to program\n");
		return;
	}

	/* reset + disable, then INT_SOURCE (write-1-clear) */
	zxnpu_wlan_dma_disable(w, true);
	zxnpu_cr_write(w, ZXNPU_WFDMA_INT_SOURCE, ~0u);

	/* reset dma indices and the delay-interrupt timers */
	zxnpu_cr_write(w, ZXNPU_WFDMA_RST_DTX_PTR, ~0u);
	zxnpu_cr_write(w, ZXNPU_WFDMA_PRI_DLY_0, 0);
	zxnpu_cr_write(w, ZXNPU_WFDMA_PRI_DLY_1, 0);
	zxnpu_cr_write(w, ZXNPU_WFDMA_PRI_DLY_2, 0);
	for (i = 0; i < ARRAY_SIZE(zxnpu_prefetch); i++)
		zxnpu_cr_write(w, zxnpu_prefetch[i].reg,
			       zxnpu_prefetch[i].val);

	/* host-side rings: qid16 FWDL (0xd4400) and qid17 MCU_WM (0xd4410);
	 * the ROM listens on FWDL for firmware data, and mt76 sends the patch
	 * handshake commands on MCU_WM. */
	w->fwdl_ring_armed = false;
	w->mcu_ring_armed = false;
	zxnpu_wlan_queue_init(w, 0);
	zxnpu_wlan_queue_init(w, 1);

	/* wait for the WFDMA HIF to report idle, then enable TX/RX DMA */
	zxnpu_cr_write(w, ZXNPU_WFDMA_BUSY_ENA,
		       zxnpu_cr_read(w, ZXNPU_WFDMA_BUSY_ENA) | 0x7);
	if (readl_poll_timeout(w->bar[0] + zxnpu_cr_bar_off(ZXNPU_WFDMA_HIF_MISC),
			       busy, !(busy & BIT(0)), 100, 20000))
		zxnpu_wlan_append(w, "  hif misc still busy (%08x)\n",
				  zxnpu_cr_read(w, ZXNPU_WFDMA_HIF_MISC));
	else
		zxnpu_wlan_append(w, "  hif misc idle\n");

	glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);
	glo |= ZXNPU_WFDMA_GLO_TX_EN | ZXNPU_WFDMA_GLO_RX_EN |
	       ZXNPU_WFDMA_GLO_OMIT_TX_INFO | ZXNPU_WFDMA_GLO_OMIT_RX_PFET2;
	zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo);
	zxnpu_cr_write(w, ZXNPU_WFDMA_INT_ENA, 0);

	/* master switch of PCIe interrupt enable (mt7915_pci_probe) */
	v = zxnpu_cr_read(w, ZXNPU_PCIE_MAC_INT_EN);
	zxnpu_cr_write(w, ZXNPU_PCIE_MAC_INT_EN, 0xff);
	zxnpu_wlan_append(w, "  pcie mac int ena %08x -> ff (was %08x)\n",
			  ZXNPU_PCIE_MAC_INT_EN, v);
	/* vendor live values the 3.30 diff against 192.168.5.1 confirmed:
	 * HOST_CONFIG bit0 (PDMA_BAND, mt76 sets it when hif2 exists) and
	 * the WFDMA-to-host window in both halves (0x811c811c - the plain
	 * 811c0000 left the low half zero, the vendor box runs with both). */
	v = zxnpu_cr_read(w, 0x18027030);
	zxnpu_cr_write(w, 0x18027030, v | BIT(0));
	zxnpu_cr_write(w, 0x180242f4, 0x811c811c);
	zxnpu_wlan_append(w, "  host config %08x (bit0 set), pcie win 242f4=%08x\n",
			  zxnpu_cr_read(w, 0x18027030),
			  zxnpu_cr_read(w, 0x180242f4));
	zxnpu_wlan_append(w, "  glo=%08x rst=%08x fw-state=%u\n",
			  zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG),
			  zxnpu_cr_read(w, ZXNPU_WFDMA_RST),
			  zxnpu_fw_state_read(w));
}

/* ---------------------------------------------------------------------
 * driver own: mt7915_driver_own() from mt7915/mcu.c, which mt76 runs
 * before every MCU command. Write DRV_OWN to MT_TOP_LPCR_HOST_BAND(0),
 * wait for the MCU to drop FW_OWN_STAT, then acknowledge the band status
 * bit. MT_SWDEF_MODE is set to normal (0) first so the firmware does not
 * come up in ICAP/spectrum mode.
 * --------------------------------------------------------------------- */
#define ZXNPU_TOP_LPCR_BAND0		0x18060010
#define ZXNPU_TOP_LPCR_BAND0_STAT	0x18060014
#define ZXNPU_LPCR_DRV_OWN		BIT(1)
#define ZXNPU_LPCR_FW_OWN_STAT		BIT(2)
#define ZXNPU_LPCR_BAND_STAT		BIT(0)
/* MT_SWDEF_MODE = MT_SWDEF(0x3c) with SWDEF_BASE_ADDR 0x411400 on mt7916;
 * mt7916_reg_map {0x410000, 0x90000} makes that bar0+0x943c. */
#define ZXNPU_SWDEF_MODE		0x0041143c
#define ZXNPU_SWDEF_NORMAL_MODE		0

static int zxnpu_wlan_driver_own(struct zxnpu_wlan *w, bool commit)
{
	int i, ok = -ETIMEDOUT;
	u32 misc;

	if (!w->bar[0]) {
		zxnpu_wlan_append(w, "driver-own: parked, no BAR0 (enable=0)\n");
		return -ENODEV;
	}
	zxnpu_wlan_append(w, "driver-own (MT_TOP_LPCR_HOST_BAND0 %08x): %s\n",
			  ZXNPU_TOP_LPCR_BAND0, commit ? "COMMIT" : "dry run");
	zxnpu_wlan_append(w, "  swdef mode <- 0 (%08x, bar0+%05x)\n",
			  ZXNPU_SWDEF_MODE, zxnpu_cr_mapped_off(ZXNPU_SWDEF_MODE));
	zxnpu_wlan_append(w, "  lpcr=%08x misc=%08x fw-state=%u\n",
			  zxnpu_cr_read(w, ZXNPU_TOP_LPCR_BAND0),
			  zxnpu_cr_read(w, ZXNPU_TOP_MISC),
			  zxnpu_fw_state_read(w));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to take ownership\n");
		return 0;
	}

	zxnpu_cr_write(w, ZXNPU_SWDEF_MODE, ZXNPU_SWDEF_NORMAL_MODE);
	zxnpu_cr_write(w, ZXNPU_TOP_LPCR_BAND0, ZXNPU_LPCR_DRV_OWN);
	/* mt76_poll_msec(..., FW_OWN_STAT, 0, 500) */
	for (i = 0; i < 500; i++) {
		if (!(zxnpu_cr_read(w, ZXNPU_TOP_LPCR_BAND0) &
		      ZXNPU_LPCR_FW_OWN_STAT)) {
			ok = 0;
			break;
		}
		msleep(1);
	}
	if (ok) {
		zxnpu_wlan_append(w, "  driver own timeout: lpcr=%08x\n",
				  zxnpu_cr_read(w, ZXNPU_TOP_LPCR_BAND0));
		return ok;
	}
	zxnpu_cr_write(w, ZXNPU_TOP_LPCR_BAND0_STAT, ZXNPU_LPCR_BAND_STAT);
	misc = zxnpu_cr_read(w, ZXNPU_TOP_MISC);
	zxnpu_wlan_append(w, "  driver own ok after %d ms: lpcr=%08x misc=%08x fw-state=%u\n",
			  i, zxnpu_cr_read(w, ZXNPU_TOP_LPCR_BAND0), misc,
			  misc & 0x7);
	return 0;
}


/* connac2 MCU TXD, byte for byte from mt76_connac_mcu.h struct
 * mt76_connac2_mcu_txd (packed, 64 bytes):
 *
 *   +0   __le32 txd[8]      word0 0x41000000|len (Q_IDX 0x20, PKT_FMT 2)
 *                           word1 0x80010000 (LONG_FORMAT, HDR_FORMAT_CMD)
 *   +32  __le16 len         payload length, minus the 32-byte txd[] array
 *   +34  __le16 pq_id       MCU_PQ_ID(MT_TX_PORT_IDX_MCU, 0x20) = 0x10000
 *   +36  u8 cid   +37 pkt_type 0xa0   +38 set_query (MCU_Q_NA)  +39 seq
 *   +40  u8 uc_d2b0_rev  +41 ext_cid  +42 s2d_index (H2N=0)  +43 ext_cid_ack
 *   +44  u32 rsv[5]
 *
 * 3.30 fixes two errors from the earlier 60-byte guess: every field after
 * txd[8] sat 24 bytes early, and pq_id dropped the RX-Q0 half (0x8000
 * instead of 0x10000). FW_SCATTER messages carry no header at all
 * (mt76_connac2_mcu_fill_message jumps out before the txd push), so the
 * scatter path must send the raw payload.
 */
#define ZXNPU_MCU_TXD_LEN	64
#define ZXNPU_MCU_TXD_PAYLOAD	(64 - 32)	/* len = total - sizeof(txd) */

static void zxnpu_mcu_txd_header(u8 *msg, int payload_len, u8 cid, u8 seq)
{
	u32 total = ZXNPU_MCU_TXD_LEN + payload_len;

	memset(msg, 0, ZXNPU_MCU_TXD_LEN);
	put_unaligned_le32(0x41000000u | (total & 0xffff), msg + 0);
	put_unaligned_le32(0x80010000u, msg + 4);
	put_unaligned_le16(payload_len + ZXNPU_MCU_TXD_PAYLOAD, msg + 32);
	/* MCU_PQ_ID(MT_TX_PORT_IDX_MCU=1, MT_TX_MCU_PORT_RX_Q0=0x20) is
	 * (1 << 15) | (0x20 << 10) = 0x10000 in 32 bits; the field is
	 * __le16 and mt76's cpu_to_le16() truncates, so the wire value is 0.
	 * (The old code wrote 0x8000 - port 1, q 0 - which is a different
	 * queue.) */
	put_unaligned_le16(0x0000, msg + 34);
	msg[36] = cid;
	msg[37] = 0xa0;				/* MCU_PKT_ID */
	msg[38] = 3;				/* MCU_Q_NA */
	msg[39] = seq & 0xf;
	/* +42 s2d_index = MCU_S2D_H2N = 0, +41 ext_cid = 0 */
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
	void *bufs = ring ? w->mcu_bufs : w->fwdl_bufs;
	dma_addr_t bufs_dma = ring ? w->mcu_bufs_dma : w->fwdl_bufs_dma;
	void *buf;
	dma_addr_t buf_dma;
	u32 *cidx = ring ? &w->mcu_cidx : &w->fwdl_cidx;
	u32 reg = ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_RING_STRIDE * ring;
	u32 slot, didx, prev;

	if (!w->bar[0]) {
		zxnpu_wlan_append(w, "  parked: enable=0 (reload with enable=1)\n");
		return 0;
	}
	if (!desc || !bufs || len <= 0 || len > ZXNPU_MCU_SLOT)
		return -EINVAL;
	zxnpu_wlan_queue_init(w, ring);

	/* flow control: never let the producer lap the consumer. Bounded to
	 * 40 ms per message - the earlier 1 s budget turned a stalled MCU into
	 * a one-hour download. */
	{
		int spins;

		for (spins = 0; spins < 200; spins++) {
			u32 didx_now = zxnpu_main_cr_read(w, reg + 12);
			u32 cidx_now = *cidx;

			if ((cidx_now - didx_now) <
			    (ZXNPU_MCU_RING_DEPTH - 8))
				break;
			usleep_range(150, 250);
		}
	}

	slot = (*cidx) % ZXNPU_MCU_RING_DEPTH;
	buf = (u8 *)bufs + (u32)slot * ZXNPU_MCU_SLOT;
	buf_dma = bufs_dma + (dma_addr_t)slot * ZXNPU_MCU_SLOT;
	memset(buf, 0, ZXNPU_MCU_SLOT);
	memcpy(buf, msg, len);
	zxnpu_mcu_fill_desc(desc, desc_dma, slot, (u32)buf_dma, len);
	dma_wmb();

	prev = zxnpu_main_cr_read(w, reg + 12);
	if (verbose)
		zxnpu_wlan_append(w, "  submit qid%d slot=%u len=%d: base=%08x cnt=%u cidx=%u didx=%u\n",
				  ring ? 17 : 16, slot, len,
				  zxnpu_cr_read(w, reg),
				  zxnpu_cr_read(w, reg + 4),
				  zxnpu_main_cr_read(w, reg + 8), prev);
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1 to write\n");
		return 0;
	}

	{
		u32 glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);

		/* keep the engines on (dma-init sets them; idempotent here) */
		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x5);
	}
	/* No WFDMA reset here any more: dma-init owns the reset pulse. Doing it
	 * per message (3.20-3.29) tore the engine down right after the ring
	 * registers were programmed, which is not what mt76 does. INT_SOURCE is
	 * write-1-clear, so dropping pending sources stays harmless. */
	zxnpu_cr_write(w, ZXNPU_WFDMA_INT_SOURCE, ~0u);
	zxnpu_main_cr_write(w, reg, (u32)desc_dma);
	zxnpu_main_cr_write(w, reg + 4, ZXNPU_MCU_RING_DEPTH);
	(*cidx)++;
	zxnpu_main_cr_write(w, reg + 8, *cidx);
	didx = zxnpu_main_cr_read(w, reg + 12);
	if (didx != prev) {
		w->ring_stall = 0;
		if (verbose)
			zxnpu_wlan_append(w, "  didx advanced to %u\n", didx);
		return 0;
	}

	if (++w->ring_stall > 512) {
		pr_err("zxnpu-wlan: ring qid%d stalled: didx frozen at %u, %u outstanding\n",
		       ring ? 17 : 16, didx, *cidx - didx);
		w->ring_stall = 0;
		return -EIO;
	}
	if (verbose)
		zxnpu_wlan_append(w, "  didx still %u (stall %u)\n", didx,
				  w->ring_stall);
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
	u8 msg[ZXNPU_MCU_TXD_LEN + 4];

	zxnpu_wlan_append(w, "mcu-send PatchSemGet (cid %02x, ring %d): %s\n",
			  0x10, ZXNPU_MCU_RING_NR, commit ? "COMMIT" : "dry run");
	memset(msg, 0, sizeof(msg));
	/* connac2 MCU TXD: cid 0x10 = PATCH_SEM_CONTROL, payload 1 = get */
	zxnpu_mcu_txd_header(msg, 4, 0x10, 1);
	put_unaligned_le32(1, msg + ZXNPU_MCU_TXD_LEN);
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
		if (len > fw->size || off > fw->size - len) {
			/* subtraction form: off+len would wrap on huge %lx
			 * values and hand the DMA engine a wild pointer */
			zxnpu_wlan_append(w, "  region beyond file: %s\n", line);
			break;
		}
		zxnpu_wlan_append(w, "  region addr=%08lx len=%lu off=%lu flags=%lx\n",
				  addr, len, off, flags);

		memset(msg, 0, sizeof(msg));
		/* connac2 MCU TXD: cid 0x05 PATCH_START_REQ for the MCU SRAM
		 * window, else 0x01 TARGET_ADDRESS_LEN_REQ */
		zxnpu_mcu_txd_header(msg, 12, (addr == 0x200000 || addr == 0x900000) ? 0x05 : 0x01, 2);
		put_unaligned_le32(addr, msg + ZXNPU_MCU_TXD_LEN);
		put_unaligned_le32(len, msg + ZXNPU_MCU_TXD_LEN + 4);
		put_unaligned_le32(0x80000000 | flags,
				   msg + ZXNPU_MCU_TXD_LEN + 8);
		if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 12, commit, true))
			break;
		if (!commit)
			continue;
		msleep(10);

		while (done < len) {
			unsigned int chunk = min_t(unsigned long, ZXNPU_MCU_CHUNK,
						   len - done);

			/* FW_SCATTER carries no MCU header at all */
			memcpy(msg, fw->data + off + done, chunk);
			if (zxnpu_mcu_submit_ring(w, zxnpu_wlan_fw_ring, msg, chunk, true, false))
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
static u16 zxnpu_wlan_andes_seq;

static void zxnpu_wlan_fw_start(struct zxnpu_wlan *w, const char *args, bool commit)
{
	u8 msg[32 + 8];
	unsigned long option = 0, override = 0;

	if (sscanf(args, "%lx %lx", &option, &override) < 1) {
		zxnpu_wlan_append(w, "usage: fw-start <option-hex> [override-hex]\n");
		return;
	}
	/* Andes FwStartReq: 32-byte header, type 2, cmd 0x17. */
	put_unaligned_le16(8, msg + 0);
	msg[2] = 2;
	msg[4] = 0x17;
	put_unaligned_le16(++zxnpu_wlan_andes_seq, msg + 6);
	put_unaligned_le16(0x8000u, msg + 20);
	msg[23] = 3;
	put_unaligned_le32(option, msg + 32);
	put_unaligned_le32(override, msg + 32 + 4);
	zxnpu_wlan_append(w, "fw-start option=%08lx override=%08lx\n",
			  option, override);
	zxnpu_mcu_submit(w, msg, sizeof(msg), commit, true);
}

/*
 * sync: re-zero the WM ring (registers + software counters) without
 * touching the ROM. After each download phase the MCU's DMA client drops
 * its didx to 0 while the cpu_idx register keeps the value we kicked;
 * the next command would then look like thousands of stale pending
 * descriptors to the engine. Run this between download phases.
 */
static void zxnpu_wlan_ring_sync(struct zxnpu_wlan *w, bool commit)
{
	zxnpu_wlan_append(w, "ring-sync: cidx sw=%u reg=%08x didx=%08x -> 0\n",
			  w->mcu_cidx,
			  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 8),
			  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 12));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: set zxnpu_wlan.commit=1\n");
		return;
	}
	/* Only act on a genuinely desynced ring: after each download phase
	 * the MCU either drains cleanly (cidx == didx, healthy - touching
	 * it breaks the next phase) or resets its DMA client (didx drops
	 * to 0 while cpu_idx keeps our last kick). */
	{
		u32 ci = zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 8);
		u32 di = zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 12);

		if (ci == di) {
			w->mcu_cidx = ci % ZXNPU_MCU_RING_DEPTH;
			zxnpu_wlan_append(w, "  drained healthy (cidx==didx), no action\n");
			return;
		}
		zxnpu_main_cr_write(w, ZXNPU_MCU_RING_REG + 8, 0);
		zxnpu_main_cr_write(w, ZXNPU_MCU_RING_REG + 12, 0);
		w->mcu_cidx = 0;
		w->fwdl_cidx = 0;
		w->fwdl_ring_armed = false;
		w->mcu_ring_armed = false;
		zxnpu_wlan_queue_init(w, ZXNPU_MCU_RING_NR);
		zxnpu_wlan_append(w, "  re-zeroed desynced ring (cidx=%08x didx=%08x)\n",
				  ci, di);
	}
	zxnpu_wlan_append(w, "  synced: cidx reg=%08x didx=%08x\n",
			  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 8),
			  zxnpu_cr_read(w, ZXNPU_MCU_RING_REG + 12));
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
	/* The Andes wire header is the head of the vendor cmd_msg
	 * (AndesInitCmdMsg): len, type, cmd, an incrementing seq, the cid
	 * echo, the GetRealPortQueueID port (0x8000 for everything but
	 * scatter) and the wait-response flag. The old 4-field header was
	 * rejected by the patch loader parser. */
	put_unaligned_le16(n, msg + 0);
	msg[2] = type;
	msg[4] = cmd;
	put_unaligned_le16(++zxnpu_wlan_andes_seq, msg + 6);
	put_unaligned_le16(cmd, msg + 8);
	put_unaligned_le16(0x8000u, msg + 20);
	msg[23] = 3;				/* wait response */
	memcpy(msg + 24, payload, n);
	zxnpu_wlan_append(w, "mcu-cmd type=%u cmd=%u payload=%d seq=%u\n",
			  type, cmd, n, zxnpu_wlan_andes_seq);
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
		/* hardware ring-item view: word0 = buffer DMA, word1 =
		 * 0x40<<24 | buffer size << 16 (pci_kick_out_cmd_msg layout) */
		put_unaligned_le32(bd, d + 0);
		put_unaligned_le32(0x40000000u | (ZXNPU_DATA_BUF_SIZE << 16), d + 4);

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
	/* BAND1 TX (qid19): same shape, same TXD gap as BAND0 (doc 42) */
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x10, (u32)w->dband_ring_dma);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x14, ZXNPU_DATA_DEPTH);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x18, 0);
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
	zxnpu_wlan_append(w, "  cipher map (R4k): sec_cfg+96 5->1/13->2/16->3 keyinfo+8@N@");
	zxnpu_wlan_append(w, "  (WEP/TKIP/AES-CCMP classes, [推断] mt76 naming)@N@");
	zxnpu_wlan_append(w, "  asic_write_tmac_info_fixed_rate(0x142404) = dispatch,@N@");
	zxnpu_wlan_append(w, "  mtf_write_tmac_info_fixed_rate(0x1B3018) = real TXD assembler:@N@");
	zxnpu_wlan_append(w, "  TMAC(52B) -> TXD words: r4+12 bit31+u12(lsl16), r4+20 flags@N@");
	zxnpu_wlan_append(w, "  (+46 b9/+45 b10/+47 b14), r4+24 rate word (r6: b6->2048,@N@");
	zxnpu_wlan_append(w, "  +1 b1 lsl14, bits5-3 phy mode); w2/w3 tail = R4f@N@");
	zxnpu_wlan_append(w, "  +13 lsl20/+41 lsl24/bit31; w1: +48 lsl24/+50 lsl16/@N@");
	zxnpu_wlan_append(w, "  +5 rate consts 0x1C50/0xBAB0; w2/w3 = R4e@N@");
	zxnpu_wlan_append(w, "  sta+8 cap-word bits 16..31 = the layer_cap bit mask family@N@");
	zxnpu_wlan_append(w, "  (same semantics as vendor npu_cap/zwzzrl layer_cap tables)@N@");
	zxnpu_wlan_append(w, "  R4f: dw1 b13=+465 flag; rate word=mtf_tx_rate_to_tmi_rate lsl16;@N@");
	zxnpu_wlan_append(w, "  +28: +30 lsl11/+34 lsl16/+33 lsl20 - TMAC->TXD map closed (doc 45 §6)@N@");
	zxnpu_wlan_append(w, "  R4e/R4g: template+0 = wcid (HcGetMaxStaNum cmp)@N@");
	zxnpu_wlan_append(w, "  per-STA ctx = arg5 + 0x13BAB0 + wcid*0x1C50 (R4g corrected:@N@");
	zxnpu_wlan_append(w, "  ctx+0 state magic 0x00040001, ctx+8 ptr -> +41 = TXD w0 b24+;@N@");
	zxnpu_wlan_append(w, "  the old '+60' read was a per-mode table, not the ctx)@N@");
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
	zxnpu_wlan_append(w, "  BAND1 (wlan5g0): TX qid19 %08x, RX data qid2 (P5)\n",
			  ZXNPU_WFDMA_TX_RING0 + 0x10 * 3);
	zxnpu_wlan_append(w, "  MCU events:      RX qid0 %08x (rx-init/mcu-recv)\n",
			  ZXNPU_MCU_RX_REG);
	zxnpu_wlan_append(w, "  BAND1 TX ring qid19: allocated and programmed by data-init\n");
	zxnpu_wlan_append(w, "  depth: %u, buf %u B, NAPI attaches with the VAPs (vap=1)\n",
			  ZXNPU_DATA_DEPTH, ZXNPU_DATA_BUF_SIZE);
}

/*
 * ---- VAP netdev skeleton (P5 pre-work, doc 38 R4) ---------------------
 *
 * Two VAPs named after the l1profile main_ifname pair (wlan0 = BAND0,
 * wlan5g0 = BAND1), created only with zxnpu_wlan.vap=1 so the default
 * firmware changes nothing. Each VAP carries a NAPI on the shared data
 * RX ring (qid1): the poll walks the ring the same way `data-recv` does
 * and, for now, counts and frees — delivering frames to the stack needs
 * the 802.11->802.3 conversion and the wcid->VAP mapping that only make
 * sense once the firmware is up (fw-state=7) and BSS exists (P5).
 *
 * TX is stubbed: the connac2 TXD template is decoded (docs 44/45) but a
 * real frame needs wcid/encryption state, so zxvap_xmit counts and drops.
 * Carrier stays off until the FWDL chain reports fw-state=7 (P5 hook).
 */


struct zxnpu_vap {
	struct net_device *ndev;
	struct zxnpu_wlan *w;
	struct napi_struct napi;
	int band;
	u64 rx_frames, tx_frames, tx_dropped;
};

static bool zxnpu_wlan_vap;
module_param_named(vap, zxnpu_wlan_vap, bool, 0644);
MODULE_PARM_DESC(vap, "register the two VAP netdevs (wlan0/wlan5g0) at probe; writable because the driver is built in - set it, then remove+rescan the PCI device to re-probe");

static int zxvap_open(struct net_device *ndev)
{
	/* carrier comes up only when the firmware reports fw-state=7 */
	netif_carrier_off(ndev);
	return 0;
}

static int zxvap_stop(struct net_device *ndev)
{
	return 0;
}

static netdev_tx_t zxvap_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct zxnpu_vap *vap = netdev_priv(ndev);

	vap->tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int zxvap_poll(struct napi_struct *napi, int budget)
{
	struct zxnpu_vap *vap = container_of(napi, struct zxnpu_vap, napi);
	struct zxnpu_wlan *w = vap->w;
	u32 didx, cidx, n;
	int work = 0;

	if (!w->drx_ring || !w->drx_buf)
		return 0;
	didx = zxnpu_cr_read(w, ZXNPU_DATA_RX_REG + 12) % ZXNPU_DATA_DEPTH;
	cidx = w->drx_cidx % ZXNPU_DATA_DEPTH;
	n = (didx + ZXNPU_DATA_DEPTH - cidx) % ZXNPU_DATA_DEPTH;
	while (work < budget && n--) {
		u32 slot = (cidx + work) % ZXNPU_DATA_DEPTH;
		u8 *d = (u8 *)w->drx_ring + slot * ZXNPU_MCU_DESC_SIZE;

		/* frame content is 802.11 (no conversion wired yet): count
		 * and resubmit the buffer; delivery lands in P5. */
		vap->rx_frames++;
		memset(d, 0, ZXNPU_MCU_DESC_SIZE);
		work++;
	}
	cidx = (cidx + work) % ZXNPU_DATA_DEPTH;
	w->drx_cidx = cidx;
	if (zxnpu_wlan_commit && work)
		zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 8, cidx);
	if (work < budget)
		napi_complete_done(napi, work);
	return work;
}

static const struct net_device_ops zxvap_netdev_ops = {
	.ndo_open		= zxvap_open,
	.ndo_stop		= zxvap_stop,
	.ndo_start_xmit		= zxvap_xmit,
	.ndo_validate_addr	= eth_validate_addr,
};

/*
 * ---- per-STA ctx state management (P5, doc 52/53 decode) --------------
 *
 * The host-side mirror of the vendor per-STA table, following the layout
 * decoded in R4g/R4h: ctx+0 = state word (magic 0x00040001 when the STA
 * is associated, per ApUpdateCapabilityAndErpIe's capability word), ctx+8
 * = pointer to the key/protect mirror block, ctx+240 = band index. The
 * vendor table lives in firmware-controlled memory (base passed as arg5
 * to the TMAC builders); our mirror is what the P5 TXD builder will read
 * when constructing real frames.
 */
#define ZXNPU_CTX_MAGIC		0x00040001
#define ZXNPU_CTX_STRIDE	0x1C50
#define ZXNPU_CTX_BASE_OFF	0x13BAB0
#define ZXNPU_WCID_MAX		243	/* 7-bit rewrite space, index 0 reserved */

struct zxnpu_sta_ctx {
	u32 state;		/* 0 = free, ZXNPU_CTX_MAGIC = associated */
	u32 band;		/* 0 = BAND0 (wlan0), 1 = BAND1 (wlan5g0) */
	u8 addr[6];
	bool key_set;
};

__maybe_unused static struct zxnpu_sta_ctx zxna_ctx[ZXNPU_WCID_MAX];
static DEFINE_MUTEX(zxna_ctx_lock);

/*
 * Per-band BSS state (doc 53 §1 decode: the band block carries the
 * protection state at +0xB85; the AP brings one BSS per band). The
 * fields mirror what the P5 BSS bring-up will configure from the
 * per-band driver profile (mt7916.dbdc.<band>.dat keys).
 */
struct zxnpu_bss {
	bool active;
	int band;
	char ssid[33];
	u32 channel;
	bool protect;		/* band block +0xB85 flag (doc 53 §1) */
	u64 beacon_cnt;
};

static __maybe_unused struct zxnpu_bss zxna_bss[2];

/* wcid 0 is reserved (index 0 per doc 16 §2.5); allocation starts at 1. */
static int zxna_wcid_alloc(struct zxnpu_vap *vap, const u8 *addr)
{
	int i;

	mutex_lock(&zxna_ctx_lock);
	for (i = 1; i < ZXNPU_WCID_MAX; i++) {
		if (zxna_ctx[i].state == 0) {
			memset(&zxna_ctx[i], 0, sizeof(zxna_ctx[i]));
			zxna_ctx[i].state = ZXNPU_CTX_MAGIC;
			zxna_ctx[i].band = vap->band;
			memcpy(zxna_ctx[i].addr, addr, 6);
			mutex_unlock(&zxna_ctx_lock);
			return i;
		}
	}
	mutex_unlock(&zxna_ctx_lock);
	return -ENOSPC;	/* 127-flow capacity ceiling, doc 16 §2.5 */
}

static void zxna_wcid_free(int wcid)
{
	if (wcid <= 0 || wcid >= ZXNPU_WCID_MAX)
		return;
	mutex_lock(&zxna_ctx_lock);
	memset(&zxna_ctx[wcid], 0, sizeof(zxna_ctx[wcid]));
	mutex_unlock(&zxna_ctx_lock);
}

/* The vendor reads the protect flag from the band block (doc 53 §1); the
 * mirror tracks it per band so the P5 TXD builder sources w0 bits24+
 * the same way.
 */
__maybe_unused static u8 zxna_band_protect[2];

static void zxnpu_wlan_vaps_init(struct zxnpu_wlan *w)
{
	static const char * const names[ZXNPU_VAP_MAX] = { "wlan0", "wlan5g0" };
	int i;

	if (!zxnpu_wlan_vap)
		return;
	for (i = 0; i < ZXNPU_VAP_MAX; i++) {
		struct zxnpu_vap *vap;
		struct net_device *ndev = alloc_netdev(sizeof(*vap), names[i],
						       NET_NAME_UNKNOWN,
						       ether_setup);

		if (!ndev)
			continue;
		vap = netdev_priv(ndev);
		vap->w = w;
		vap->band = i;
		vap->ndev = ndev;
		SET_NETDEV_DEV(ndev, &w->pdev->dev);
		ndev->netdev_ops = &zxvap_netdev_ops;
		eth_hw_addr_random(ndev);
		netif_napi_add(ndev, &vap->napi, zxvap_poll);
		netif_carrier_off(ndev);
		if (register_netdev(ndev)) {
			netif_napi_del(&vap->napi);
			free_netdev(ndev);
			continue;
		}
		w->vaps[i] = vap;
	}
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
		put_unaligned_le32(bd, d + 0);
		/* bit30 on an RX descriptor is the FREE/owned-by-host flag:
		 * the vendor ring keeps it set while the slot is empty and
		 * the DMA clears it when it delivers the event (live vendor
		 * ring: filled slot ctrl=0x06a40000, empty=0x46a40000). mt76
		 * omits it only because its WED path manages ownership
		 * differently - here it is required or the engine skips
		 * every slot as already-consumed. */
		put_unaligned_le32(0x40000000u | (ZXNPU_MCU_RX_BUF_SIZE << 16), d + 4);

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

/*
 * rx-desc: dump the live RX descriptors (coherent memory the driver owns)
 * and the first event buffers. The DMA flips the descriptor ctrl on
 * delivery, so this is the ground truth for "did anything arrive".
 */
static void zxnpu_wlan_rx_desc_dump(struct zxnpu_wlan *w)
{
	int i;

	if (!w->rx_ring || !w->rx_buf) {
		zxnpu_wlan_append(w, "rx-desc: no rx memory\n");
		return;
	}
	zxnpu_wlan_append(w, "rx-desc: ring=%08x bufs=%08x depth=%u\n",
			  (u32)w->rx_ring_dma, (u32)w->rx_buf_dma,
			  ZXNPU_MCU_RX_DEPTH);
	for (i = 0; i < 4; i++) {
		u8 *d = (u8 *)w->rx_ring + i * ZXNPU_MCU_DESC_SIZE;
		u8 *b = (u8 *)w->rx_buf + i * ZXNPU_MCU_RX_BUF_SIZE;

		zxnpu_wlan_append(w,
				  "  desc[%d]: buf0=%08x ctrl=%08x buf1=%08x info=%08x\n",
				  i, get_unaligned_le32(d),
				  get_unaligned_le32(d + 4),
				  get_unaligned_le32(d + 8),
				  get_unaligned_le32(d + 12));
		zxnpu_wlan_append(w,
				  "  buf[%d]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				  i, b[0], b[1], b[2], b[3],
				  b[4], b[5], b[6], b[7]);
	}
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
		 * the MCU starts at +0x1E and still contains the 0x11223344 word.
		 * The guard must cover the reads at +0x20..+0x27, not just the
		 * 30-byte strip (a 31-35 byte blob used to pass). */
		if (fw->size >= 0x28 &&
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
	/* PATCH_SEM_CONTROL, payload 1 = get the semaphore */
	zxnpu_mcu_txd_header(msg, 4, 0x10, 1);
	put_unaligned_le32(1, msg + ZXNPU_MCU_TXD_LEN);
	if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, commit, true))
		goto out;
	if (!commit)
		goto out;
	msleep(20);

	memset(msg, 0, sizeof(msg));
	/* TARGET_ADDRESS_LEN_REQ (cid 0x01); the MCU SRAM window uses
	 * PATCH_START_REQ (cid 0x05) instead */
	zxnpu_mcu_txd_header(msg, 12, (dst == 0x200000 || dst == 0x900000) ? 0x05 : 0x01, 2);
	put_unaligned_le32(dst, msg + ZXNPU_MCU_TXD_LEN);
	put_unaligned_le32(total, msg + ZXNPU_MCU_TXD_LEN + 4);
	put_unaligned_le32(0x80000000, msg + ZXNPU_MCU_TXD_LEN + 8);
	if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 12, true, true))
		goto out;
	msleep(10);

	while (done < total) {
		unsigned int chunk = min_t(unsigned int, ZXNPU_MCU_CHUNK,
					   total - done);

		/* FW_SCATTER is raw: mt76_connac2_mcu_fill_message returns
		 * before pushing any txd, so the skb is just the data. */
		memcpy(msg, fw->data + offset + done, chunk);
		if (zxnpu_mcu_submit_ring(w, zxnpu_wlan_fw_ring, msg, chunk, true, false))
			goto out;
		done += chunk;
		if (!(done % (128 * ZXNPU_MCU_CHUNK)))
			pr_info("zxnpu-wlan: scatter %u/%u (didx %u)\n", done,
				total, zxnpu_main_cr_read(w, ZXNPU_MCU_RING_REG + 12));
		usleep_range(1000, 2000);
	}

	memset(msg, 0, sizeof(msg));
	/* PATCH_FINISH_REQ, no payload */
	zxnpu_mcu_txd_header(msg, 0, 0x07, 3);
	if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN, true, true))
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
		/* subtraction form: the image-supplied len could wrap the u32
		 * sum and scatter-download past the firmware buffer */
		if (regs[i].len > fw->size || off > fw->size - regs[i].len)
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
	u8 msg[ZXNPU_MCU_TXD_LEN + 12];

	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header(msg, 12, (addr == 0x200000 || addr == 0x900000) ? 0x05 : 0x01, 2);
	put_unaligned_le32(addr, msg + ZXNPU_MCU_TXD_LEN);
	put_unaligned_le32(len, msg + ZXNPU_MCU_TXD_LEN + 4);
	put_unaligned_le32(mode, msg + ZXNPU_MCU_TXD_LEN + 8);
	return zxnpu_mcu_submit(w, msg, sizeof(msg), commit, false);
}

static int zxnpu_mcu_scatter(struct zxnpu_wlan *w, const u8 *data, u32 len,
			     bool commit)
{
	/* Vendor FwScatter framing: an Andes 32-byte header with type 0xEE
	 * (the value GetRealPortQueueID redirects to the firmware-download
	 * queue) and cmd 0x10, payload at +32. mt76 sends scatter raw, which
	 * only works when the data ring is a dedicated DMA queue; on this SoC
	 * data and commands share the WM ring, so the header is what tells
	 * the loader "this is firmware data, not a command". */
	u8 msg[32 + ZXNPU_MCU_CHUNK];
	u32 done = 0;

	while (done < len) {
		u32 chunk = min_t(u32, ZXNPU_MCU_CHUNK, len - done);

		memset(msg, 0, sizeof(msg));
		put_unaligned_le16(chunk, msg + 0);
		msg[2] = 0xEE;
		msg[4] = 0x10;
		put_unaligned_le16(++zxnpu_wlan_andes_seq, msg + 6);
		put_unaligned_le16(0x8000u, msg + 20);
		msg[23] = 3;
		memcpy(msg + 32, data + done, chunk);
		if (zxnpu_mcu_submit_ring(w, zxnpu_wlan_fw_ring, msg, 32 + chunk,
					  commit, false))
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
		u8 msg[32 + 8];

		memset(msg, 0, sizeof(msg));
		zxnpu_mcu_txd_header(msg, 8, 0x02, 4);
		put_unaligned_le32(option, msg + ZXNPU_MCU_TXD_LEN);
		put_unaligned_le32(override, msg + ZXNPU_MCU_TXD_LEN + 4);
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
		if (fw->size >= 16)
			zxnpu_wlan_append(w, "%s: first16=%*phN size=%zu\n",
					  names[i], 16, fw->data, fw->size);
		else
			zxnpu_wlan_append(w, "%s: first16=(truncated) size=%zu\n",
					  names[i], fw->size);
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
	struct zxnpu_wlan *w = zxnpu_wlan_active();

	if (!w)
		return 0;
	seq_printf(s, "%s", w->status);
	return 0;
}

/* persistent probe evidence: identity, BARs, firmware CRCs (doc 62 B4/B5);
 * the main function's log is appended so one file covers both windows */
static int zxnpu_wlan_probe_show(struct seq_file *s, void *unused)
{
	struct zxnpu_wlan *w = zxnpu_wlan_active();

	if (!w)
		return 0;
	seq_printf(s, "%s", w->probe_log);
	if (zxnpu_wlan_main && zxnpu_wlan_main != w)
		seq_printf(s, "--- 7906 main ---\n%s",
			   zxnpu_wlan_main->probe_log);
	return 0;
}

/* parse 12 hex digits into a MAC (sta-add) */
static int zxvap_parse_mac(const char *s, u8 *out)
{
	int i;

	for (i = 0; i < 6; i++) {
		int hi, lo;
		char c;

		c = s[i * 2];
		hi = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		c = s[i * 2 + 1];
		lo = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (hi << 4) | lo;
	}
	return 0;
}

static ssize_t zxnpu_wlan_ctl_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct zxnpu_wlan *w = zxnpu_wlan_active();
	char cmd[128];	/* mcu-dl-regions manifests are long; 32 once rejected them with -EINVAL before anything ran */
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
		zxnpu_wlan_probe_regs(w, w);
		if (zxnpu_wlan_main && zxnpu_wlan_main != w)
			zxnpu_wlan_probe_regs(w, zxnpu_wlan_main);
	} else if (!strncmp(cmd, "fw-audit", 8)) {
		zxnpu_wlan_audit_firmware(w);
		zxnpu_wlan_append(w, "fw-audit: written to the persistent probe log (/proc/zx-csp-wlan/probe)\n");
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
	} else if (!strncmp(cmd, "mcu", 3) &&
		   (cmd[3] == '\0' || cmd[3] == '\n')) {
		/* exact match only: a bare prefix here shadowed mcuwin,
		 * mcu-cmd, mcu-dl, mcu-dl-regions, mcu-recv and mcu-send,
		 * leaving the whole MCU surface dead (doc 62 finding). */
		zxnpu_wlan_mcu_plan(w);
	} else if (!strncmp(cmd, "l1profile", 9)) {
		zxnpu_wlan_l1profile(w);
	} else if (!strncmp(cmd, "ack-expect", 10)) {
		zxnpu_wlan_ack_expect(w);
	} else if (!strncmp(cmd, "rings", 5)) {
		zxnpu_wlan_rings(w);
	} else if (!strncmp(cmd, "mcuwin", 6)) {
		zxnpu_wlan_mcuwin(w);
	} else if (!strncmp(cmd, "win ", 4)) {
		zxnpu_wlan_win_read(w, cmd + 4);
	} else if (!strncmp(cmd, "crw b", 5)) {
		/* bar-direct form: "crw b<bar> <off> <val>" - the doorbell
		 * (0x54000108 logical) lives outside the CR window and its
		 * physical home is one of the BARs; write and read back. */
		unsigned int bar;
		unsigned long off;
		u32 v;
		void __iomem *p;
		resource_size_t len;

		if (!zxnpu_wlan_commit) {
			zxnpu_wlan_append(w, "crw refused: set zxnpu_wlan.commit=1\n");
		} else if (sscanf(cmd + 5, "%u %lx %x", &bar, &off, &v) == 3 &&
			   bar <= 2 && (p = zxnpu_wlan_bar_of(w, bar, &len)) &&
			   off + 4 <= len) {
			writel(v, p + off);
			zxnpu_wlan_append(w, "crw b%u+%lx <- %08x; readback %08x\n",
					  bar, off, v, readl(p + off));
		} else {
			zxnpu_wlan_append(w, "usage: crw b<bar 0..2> <off> <val>\n");
		}
	} else if (!strncmp(cmd, "crw ", 4)) {
		/* commit-gated single CR write: "crw <addr-hex> <val-hex>".
		 * The experiment tool for the FWDL bring-up loop (PCIe window
		 * variants, per-ring enables) without a rebuild per try. */
		u32 a, v;

		if (!zxnpu_wlan_commit) {
			zxnpu_wlan_append(w, "crw refused: set zxnpu_wlan.commit=1\n");
		} else if (sscanf(cmd + 4, "%x %x", &a, &v) == 2) {
			zxnpu_cr_write(w, a, v);
			zxnpu_wlan_append(w, "crw [%08x] <- %08x; readback %08x\n",
					  a, v, zxnpu_cr_read(w, a));
		} else {
			zxnpu_wlan_append(w, "usage: crw <addr-hex> <val-hex>\n");
		}
	} else if (!strncmp(cmd, "scan", 4) &&
		   (cmd[4] == '\0' || cmd[4] == '\n')) {
		zxnpu_wlan_bus_scan(w);
	} else if (!strncmp(cmd, "mcu-cmd ", 8)) {
		zxnpu_wlan_mcu_cmd(w, cmd + 8, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl-regions ", 15)) {
		zxnpu_wlan_mcu_regions(w, cmd + 15, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl ", 7)) {
		zxnpu_wlan_mcu_dl(w, cmd + 7, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "rx-desc", 7)) {
		zxnpu_wlan_rx_desc_dump(w);
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
	} else if (!strncmp(cmd, "sta-add ", 8)) {
		/* sta-add <hex 12> <band 0|1>: allocate a wcid and mark the
		 * per-STA ctx associated (the R4g/R4h mirror layout). Real
		 * association arrives with P5; this exercises the allocator
		 * the P5 association handler will call. */
		u8 amac[6];
		char smac[24];
		unsigned int band = 0;
		int wcid;

		if (sscanf(cmd + 8, "%23s %u", smac, &band) < 1 ||
		    zxvap_parse_mac(smac, amac)) {
			zxnpu_wlan_append(w, "usage: sta-add <mac 12hex> [band]\n");
			goto done_ctl;
		}
		if (band > 1) {
			zxnpu_wlan_append(w, "sta-add: band must be 0 or 1\n");
			goto done_ctl;
		}
		wcid = -ENOSPC;
		if (w->vaps[band])
			wcid = zxna_wcid_alloc(w->vaps[band], amac);
		else
			zxnpu_wlan_append(w, "sta-add: VAP %u absent (vap=1)\n",
					  band);
		if (wcid >= 0) {
			/* link the STA to its BSS: the association state
			 * machine start (free -> associated); the protect
			 * flag propagates from the band block (doc 53 §1). */
			zxna_bss[band].active = true;
			zxnpu_wlan_append(w, "sta-add: mac %pM wcid %d band %u bss%s\n",
					  amac, wcid, band,
					  zxna_bss[band].active ?
					  " linked" : "");
		} else {
			zxnpu_wlan_append(w, "sta-add: failed (%d)\n", wcid);
		}
done_ctl:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "key-set ", 8)) {
		/* key-set <wcid>: mark the per-STA key mirror installed
		 * (the fill_wtbl_key_info_struc_v2 command path sends the
		 * material to the firmware; the host mirror tracks it).
		 * Real key material arrives with P5 association. */
		unsigned int wcid;
		int found = 0;

		if (sscanf(cmd + 8, "%u", &wcid) == 1 &&
		    wcid > 0 && wcid < ZXNPU_WCID_MAX &&
		    zxna_ctx[wcid].state == ZXNPU_CTX_MAGIC) {
			zxna_ctx[wcid].key_set = true;
			found = 1;
		}
		zxnpu_wlan_append(w, "key-set: wcid %u %s\n", wcid,
				  found ? "marked key-set" :
				  "not associated (sta-add first)");
	} else if (!strncmp(cmd, "sta-del ", 8)) {
		unsigned int wcid;

		if (sscanf(cmd + 8, "%u", &wcid) != 1) {
			zxnpu_wlan_append(w, "usage: sta-del <wcid>\n");
		} else {
			zxna_wcid_free(wcid);
			zxnpu_wlan_append(w, "sta-del: wcid %u freed\n", wcid);
		}
	} else if (!strncmp(cmd, "sta-list", 8)) {
		int i;

		zxnpu_wlan_append(w, "sta-list (ctx mirror, doc 52 layout):\n");
		mutex_lock(&zxna_ctx_lock);
		for (i = 1; i < ZXNPU_WCID_MAX; i++) {
			if (zxna_ctx[i].state != ZXNPU_CTX_MAGIC)
				continue;
			zxnpu_wlan_append(w, "  wcid %d band %u addr %pM%s\n",
					  i, zxna_ctx[i].band,
					  zxna_ctx[i].addr,
					  zxna_ctx[i].key_set ?
					  " key-set" : "");
		}
		mutex_unlock(&zxna_ctx_lock);
	} else if (!strncmp(cmd, "bss-show", 8)) {
		int i;

		zxnpu_wlan_append(w, "bss-show (per-band BSS state, doc 53 §1):\n");
		for (i = 0; i < 2; i++) {
			zxnpu_wlan_append(w, "  band %d: %s ssid='%s' ch=%u%s\n",
					  i,
					  zxna_bss[i].active ? "active" : "down",
					  zxna_bss[i].ssid,
					  zxna_bss[i].channel,
					  zxna_bss[i].protect ?
					  " protect" : "");
		}
	} else if (!strncmp(cmd, "bss-set ", 8)) {
		/* bss-set <band> <channel> <ssid up to 32>: stages the BSS
		 * config in the mirror; the beacon/CSA programming lands in
		 * P5 behind commit. */
		unsigned int band = 0, ch = 0;
		char ssid[33] = "";

		if (sscanf(cmd + 8, "%u %u %32[^\n]", &band, &ch, ssid) < 2 ||
		    band > 1 || ch < 1 || ch > 196) {
			zxnpu_wlan_append(w, "usage: bss-set <band 0|1> <ch 1..196> <ssid up to 32>\n");
			goto done_bss;
		}
		zxna_bss[band].band = band;
		zxnpu_wlan_append(w, "bss-set: band %u staged ch=%u ssid='%s'%s\n",
				  band, ch, ssid,
				  zxnpu_wlan_commit ?
				  " (beacon programming in P5)" :
				  " (dry run)");
done_bss:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "sync", 4)) {
		zxnpu_wlan_ring_sync(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "driver-own", 10)) {
		zxnpu_wlan_driver_own(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "wfsys-reset", 11)) {
		zxnpu_wlan_wfsys_reset(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "dma-init", 8)) {
		zxnpu_wlan_dma_init(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-send", 8)) {
		zxnpu_wlan_mcu_send(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "commit", 6)) {
		commit = zxnpu_wlan_commit;
		if (!commit)
			zxnpu_wlan_append(w, "commit refused: set zxnpu_wlan.commit=1\n");
		zxnpu_wlan_wfdma(w, commit);
	} else {
		zxnpu_wlan_append(w, "commands: l1profile | ack-expect | data-init | data-recv | dataplane | probe | scan | win <bar> <off> [n] | crw <a> <v> | fw | plan | mcu | rings | mcuwin | mcu-send | mcu-cmd | mcu-dl | mcu-dl-regions | fw-start | fw-info | fw-audit | fw-state | fw-plan wm|wa | fw-dl wm|wa | rx-init | mcu-recv | driver-own | wfsys-reset | dma-init | commit\n");
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

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->pdev = pdev;
	w->is_main = pdev->device == ZXNPU_WLAN_DEVICE_MAIN;
	mutex_init(&w->lock);

	/* The control plane must exist even when parked: probe succeeds with
	 * enable=0 and only skips the PCI/BAR stage, so /proc/zx-csp-wlan/
	 * and the VAP netdevs are always up and ctl reports why the hardware
	 * path is absent. Parked probe writes no hardware register. */
	if (zxnpu_wlan_enable) {
		ret = pcim_enable_device(pdev);
		if (ret)
			return ret;
		/* MT7916 has three 64-bit BARs at indices 0, 2, 4 (BAR 1/3/5
		 * are the upper halves). Requesting BIT(1) causes EINVAL
		 * because BAR 1 is not an independent resource on a 64-bit
		 * BAR device. */
		ret = pcim_iomap_regions(pdev, BIT(0) | BIT(2) | BIT(4),
					 "zxnpu-wlan");
		if (ret)
			return ret;
		/* mt7915_pci_probe does this right after iomap; without it the
		 * endpoint has no bus-master right and the WFDMA never fetches
		 * a descriptor (probe printed command=0142, MASTER bit clear -
		 * 22 rounds of DIDX=0 traced to this plus the bridge window
		 * work below). */
		pci_set_master(pdev);

		dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
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
		w->mcu_bufs = dmam_alloc_coherent(&pdev->dev,
						  ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_SLOT,
						  &w->mcu_bufs_dma, GFP_KERNEL);
		w->fwdl_bufs = dmam_alloc_coherent(&pdev->dev,
						   ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_SLOT,
						   &w->fwdl_bufs_dma, GFP_KERNEL);
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
		w->dband_ring = dmam_alloc_coherent(&pdev->dev,
						    ZXNPU_DATA_DEPTH * ZXNPU_MCU_DESC_SIZE,
						    &w->dband_ring_dma, GFP_KERNEL);
		/* one NULL here must not reach the ring-BASE programming:
		 * data-init would hand hardware a garbage DMA base (doc 62
		 * finding 7). devm unwinds whatever did allocate. */
		if (!w->mcu_ring || !w->mcu_buf || !w->fwdl_ring ||
		    !w->fwdl_buf || !w->mcu_bufs || !w->fwdl_bufs ||
		    !w->rx_ring || !w->rx_buf ||
		    !w->dtx_ring || !w->drx_ring || !w->drx_buf ||
		    !w->dband_ring)
			return -ENOMEM;
		/* w->bar[i] mirrors the real 64-bit BAR indices 0/2/4;
		 * table[1] would be BAR0's upper half (never requested). */
		w->bar[0] = pcim_iomap_table(pdev)[0];
		w->bar[1] = pcim_iomap_table(pdev)[2];
		w->bar[2] = pcim_iomap_table(pdev)[4];

		/* The root port's prefetch-window forwarding is dead on this
		 * silicon: every access inside it master-aborts and latches
		 * RMA, reads return 0 (3.13/3.14 evidence - the 1MB BAR0 was
		 * allocated in that window, the 32KB BAR2 in the ordinary MEM
		 * window and answered). Widen the root port's ordinary MEM
		 * window over the whole child range so BAR0 routes through
		 * the working path, and switch the prefetch window off. */
		{
			struct pci_dev *br = pdev->bus->self;
			u32 lo = ~0u, hi = 0;
			int bi;

			for (bi = 0; bi < 6; bi++) {
				struct resource *r = &pdev->resource[bi];

				if (resource_size(r) && r->start) {
					lo = min(lo, (u32)(r->start >> 20));
					hi = max(hi, (u32)(r->end >> 20));
				}
			}
			if (br && hi) {
				pci_write_config_word(br, PCI_MEMORY_BASE,
						      lo << 4);
				pci_write_config_word(br, PCI_MEMORY_LIMIT,
						      hi << 4);
				pci_write_config_word(br, PCI_PREF_MEMORY_BASE,
						      0xfff0);
				pci_write_config_word(br, PCI_PREF_MEMORY_LIMIT,
						      0x0000);
				zxnpu_wlan_pl_append(w, "bridge %s mem window widened to %03x00000-%03xfffff, prefetch window disabled\n",
						     pci_name(br), lo, hi);
			}
		}

		pci_read_config_byte(pdev, PCI_REVISION_ID, &revision);
		pci_read_config_word(pdev, PCI_COMMAND, &command);
		pci_read_config_word(pdev, PCI_STATUS, &status);
		zxnpu_wlan_pl_append(w, "vendor=%04x device=%04x revision=%02x\n",
				     pdev->vendor, pdev->device, revision);
		zxnpu_wlan_pl_append(w, "command=%04x status=%04x bars=%d\n",
				     command, status, pci_select_bars(pdev,
				     IORESOURCE_MEM));
		for (i = 0; i < 3; i++) {
			/* real BAR indices are 0/2/4 (64-bit BARs) */
			unsigned int ridx = i * 2;

			zxnpu_wlan_pl_append(w, "bar%d: start=%pa len=%pa%s\n",
					     ridx,
					     &pdev->resource[ridx].start,
					     &pdev->resource[ridx].end,
					     w->bar[i] ? "" : " (unmapped)");
		}
		zxnpu_wlan_probe_regs(w, w);
	} else {
		zxnpu_wlan_pl_append(w, "parked: enable=0, no PCI/BAR access (reload with enable=1)\n");
	}
	zxnpu_wlan_pl_append(w, "state: R-A, main function owns the hardware (no register written at probe)\n");
	zxnpu_wlan_audit_firmware(w);

	/* one control surface: the main function (7906) publishes
	 * /proc/zx-csp-wlan/ because it is the instance that will run the
	 * wfsys reset and the WFDMA bring-up; the companion's evidence shows
	 * up in its own probe log and the dual-window cr dump. */
	if (w->is_main) {
		dir = proc_mkdir("zx-csp-wlan", NULL);
		if (dir) {
			if (!proc_create_single("status", 0444, dir,
						zxnpu_wlan_status_show) ||
			    !proc_create_single("probe", 0444, dir,
						zxnpu_wlan_probe_show) ||
			    !proc_create("ctl", 0200, dir, &zxnpu_wlan_ctl_ops)) {
				proc_remove(dir);
				dir = NULL;
			}
		}
	} else {
		dir = NULL;
		zxnpu_wlan_pl_append(w, "func: 790a companion (hif2 only; control surface lives on 7906)\n");
	}
	w->proc = dir;
	if (w->is_main)
		zxnpu_wlan_main = w;
	else
		zxnpu_wlan_comp = w;
	pci_set_drvdata(pdev, w);
	if (w->is_main)
		zxnpu_wlan_vaps_init(w);
	dev_info(&pdev->dev, "R-A ready (%s commit=%d vaps=%d)\n",
		 w->is_main ? "main" : "comp", zxnpu_wlan_commit,
		 zxnpu_wlan_vap);
	return 0;
}

static void zxnpu_wlan_vaps_exit(struct zxnpu_wlan *w)
{
	int i;

	for (i = 0; i < ZXNPU_VAP_MAX; i++) {
		struct zxnpu_vap *vap = w->vaps[i];

		if (!vap)
			continue;
		unregister_netdev(vap->ndev);
		netif_napi_del(&vap->napi);
		free_netdev(vap->ndev);
		w->vaps[i] = NULL;
	}
}

static void zxnpu_wlan_remove(struct pci_dev *pdev)
{
	struct zxnpu_wlan *w = pci_get_drvdata(pdev);

	if (!w)
		return;
	/* VAPs are alloc_netdev (not devm): leave them registered and the
	 * enable-toggle remove/rescan iteration would free the adapter under
	 * live netdevs with dangling ->w pointers (doc 62 finding 2). */
	zxnpu_wlan_vaps_exit(w);
	if (w->proc)
		proc_remove(w->proc);
	if (w->is_main)
		zxnpu_wlan_main = NULL;
	else
		zxnpu_wlan_comp = NULL;
}

static const struct pci_device_id zxnpu_wlan_ids[] = {
	{ PCI_DEVICE(ZXNPU_WLAN_VENDOR, ZXNPU_WLAN_DEVICE_MAIN) },
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
