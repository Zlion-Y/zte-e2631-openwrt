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

/* The MCU starts posting unsolicited events as soon as the radio is tuned
 * (eid 0x00 with a ~220-byte payload, several per second). The full hex dump
 * of every consumed event is what makes those readable, but it also fills the
 * 16 KiB status buffer in about 60 events and then every append after that is
 * silently dropped - which is how 3.87's radio-mac result disappeared. Off by
 * default: one line per event with eid/seq is enough to follow a command. */
static int zxnpu_wlan_evt_log;
module_param_named(evt_log, zxnpu_wlan_evt_log, int, 0644);
MODULE_PARM_DESC(evt_log, "hex dump every consumed MCU event (default 0: eid/seq only)");

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
	/* qid20 / 0x18024440: the MCU_WA command ring. mt7915_mcu_send_message
	 * hands EVERY command here once MT76_STATE_MCU_RUNNING is set (right
	 * after load_firmware), and WA answers on its own event ring (qid1
	 * RX). The MCU_WM ring is a boot-time-only ring: patch sem, AddrLen
	 * and FwStart are answered there by the ROM/fresh image, but the
	 * running firmware services commands on WA. This is why 3.75-3.77
	 * saw qid17 drain perfectly and never got a single reply. */
	void *wac_ring;
	dma_addr_t wac_ring_dma;
	void *wac_bufs;
	dma_addr_t wac_bufs_dma;
	u32 wac_cidx;
	bool wac_ring_armed;
	void *rx_ring;		/* qid0 / 0x18024500: MCU_WM event ring, 32 x 36 desc */
	dma_addr_t rx_ring_dma;
	void *rx_buf;		/* 32 x 1700 buffers with 16B RXD */
	dma_addr_t rx_buf_dma;
	u32 rx_cidx;
	/* qid1 / 0x18024510: MCU_WA event ring. The MT7915 command path hands
	 * every command to WA once the firmware is running
	 * (mt7915_mcu_send_message picks MT_MCUQ_WA when MT76_STATE_MCU_RUNNING
	 * is set) and WA answers on its own ring, so without this one the
	 * commands are consumed and the replies land nowhere. */
	void *wa_ring;
	dma_addr_t wa_ring_dma;
	void *wa_buf;
	dma_addr_t wa_buf_dma;
	u32 wa_cidx;
	void *dtx_ring;		/* qid18 / BAND0 TX data ring (empty: TXD undecoded) */
	void *dband_ring;	/* qid19 / BAND1 TX data ring (empty: TXD undecoded) */
	dma_addr_t dband_ring_dma;
	dma_addr_t dtx_ring_dma;
	void *drx_ring;		/* qid1 RX data ring, 64 x 36 descriptors */
	dma_addr_t drx_ring_dma;
	void *drx_buf;		/* 64 x 1700 buffers with 16B RXD */
	dma_addr_t drx_buf_dma;
	u32 drx_cidx;
	/* MT7916_EEPROM.bin: the full 0x1000 v2 map the vendor image ships.
	 * mt7915_eeprom_load_default() is the same fallback in mt76, and it is
	 * what makes flash_mode true, i.e. the map has to be uploaded to the
	 * firmware later. Parsed out of it: chip id, MAC, per-band select and
	 * tx path (chainmask). */
	u8 *eeprom;
	bool flash_mode;
	u8 mac[6];
	u8 band_sel[2];		/* MT_EE_V2_BAND_SEL_* per band */
	u8 band_path[2];	/* tx path count per band, 0 = band unused */
	u8 chainmask;
	struct zxnpu_vap *vaps[ZXNPU_VAP_MAX];
	char status[16384];
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
/* TX ring selectors ARE the WFDMA ring numbers, i.e. the values of
 * enum mt7915_txq_id: FWDL = 16, MCU_WM = 17, BAND0 = 18, BAND1 = 19,
 * MCU_WA = 20. Ring 4 therefore lands on 0x18024440, which is qid20. */
#define ZXNPU_TX_RING_FWDL	0	/* qid16, 0x18024400 */
#define ZXNPU_TX_RING_WM	1	/* qid17, 0x18024410 */
#define ZXNPU_TX_RING_WA_CMD	4	/* qid20, 0x18024440 */
/* Which ring commands ride. mt7915_mcu_send_message picks MT_MCUQ_FWDL for
 * FW_SCATTER, MT_MCUQ_WA once MT76_STATE_MCU_RUNNING is set, and MT_MCUQ_WM
 * before that. Everything this driver did so far - patch semaphore,
 * TARGET_ADDRESS_LEN, FwStart - happens pre-boot, hence both cores' boot
 * handshakes were answered on qid17. The moment the firmware reports
 * FW_STATE_RDY the command surface moves to WA (qid20) and qid17 goes
 * silent by design: a drained ring with no answer was the correct
 * behaviour for a command on the wrong core. */
static int zxnpu_wlan_mcu_ring = ZXNPU_MCU_RING_NR;
module_param_named(mcu_ring, zxnpu_wlan_mcu_ring, int, 0644);
MODULE_PARM_DESC(mcu_ring,
		 "TX ring for MCU commands: 0=qid16 FWDL, 1=qid17 MCU_WM, 4=qid20 MCU_WA (post-RDY default in mt76)");
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
/* Vendor scatter chunk: 4096 - chip_cap[0x134]=64 = 4032 (mt_load_fw and
 * mt76 both cap there). 512-byte frames got consumed exactly three times
 * by the ROM DL engine and then it stopped talking - the vendor frame
 * size is the one wire format nobody argues with. */
#define ZXNPU_MCU_CHUNK		4032
#define ZXNPU_MCU_SLOT		(ZXNPU_MCU_CHUNK + 64)	/* room for the 32B Andes header */
#define ZXNPU_WM_FW		"zte/mt7916/WIFI_RAM_CODE_MT7916.bin"
#define ZXNPU_WA_FW		"zte/mt7916/7916_WACPU_RAM_CODE_release.bin"
#define ZXNPU_PATCH_FW		"zte/mt7916/mt7916_patch_e1_hdr.bin"
#define ZXNPU_FW_MAX_REGIONS	16

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
static void zxnpu_mcu_fill_desc_last(void *ring, u32 slot, u32 buf_dma,
				     u32 len, bool last);
static int zxnpu_mcu_submit_ring_last(struct zxnpu_wlan *w, int ring,
				      const u8 *msg, int len, bool commit,
				      bool verbose, bool last);

/* bit30 is REQUIRED on every TX descriptor - clearing it stops the WFDMA
 * fetcher entirely (3.60: txdrain timeout, zero frames fetched). Whatever
 * the datasheet calls it, on this silicon it is the fetch-enable bit. */
static void zxnpu_mcu_fill_desc_last(void *ring, u32 slot, u32 buf_dma,
				     u32 len, bool last)
{
	u8 *d = (u8 *)ring + slot * ZXNPU_MCU_DESC_SIZE;

	memset(d, 0, ZXNPU_MCU_DESC_SIZE);
	put_unaligned_le32(buf_dma, d + 0);
	put_unaligned_le32(0x40000000u | ((len & 0x3fff) << 16), d + 4);
}

/* One selector -> one set of ring resources. The three MCU TX rings share
 * the same descriptor shape and depth; only their bank differs. */
struct zxnpu_tx_ring {
	void *desc;
	dma_addr_t desc_dma;
	void *bufs;
	dma_addr_t bufs_dma;
	u32 *cidx;
	bool *armed;
};

static int zxnpu_tx_ring_qid(int ring)
{
	return ring == ZXNPU_TX_RING_WA_CMD ? 20 : 16 + ring;
}

static bool zxnpu_tx_ring_get(struct zxnpu_wlan *w, int ring,
			      struct zxnpu_tx_ring *r)
{
	memset(r, 0, sizeof(*r));
	switch (ring) {
	case ZXNPU_TX_RING_FWDL:
		r->desc = w->fwdl_ring;
		r->desc_dma = w->fwdl_ring_dma;
		r->bufs = w->fwdl_bufs;
		r->bufs_dma = w->fwdl_bufs_dma;
		r->cidx = &w->fwdl_cidx;
		r->armed = &w->fwdl_ring_armed;
		return true;
	case ZXNPU_TX_RING_WM:
		r->desc = w->mcu_ring;
		r->desc_dma = w->mcu_ring_dma;
		r->bufs = w->mcu_bufs;
		r->bufs_dma = w->mcu_bufs_dma;
		r->cidx = &w->mcu_cidx;
		r->armed = &w->mcu_ring_armed;
		return true;
	case ZXNPU_TX_RING_WA_CMD:
		r->desc = w->wac_ring;
		r->desc_dma = w->wac_ring_dma;
		r->bufs = w->wac_bufs;
		r->bufs_dma = w->wac_bufs_dma;
		r->cidx = &w->wac_cidx;
		r->armed = &w->wac_ring_armed;
		return true;
	}
	return false;
}

/* mt76_dma_queue_reset + sync_idx semantics for one TX ring: unused
 * descriptors carry DMA_DONE (bit31) so the engine skips them; indices
 * zeroed; ring_size/desc_base programmed last (mt76 order). Runs once
 * per ring via the armed flag. */
static void zxnpu_wlan_queue_init(struct zxnpu_wlan *w, int ring)
{
	struct zxnpu_tx_ring r;
	u32 reg = ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_RING_STRIDE * ring;
	int i;

	if (!zxnpu_tx_ring_get(w, ring, &r))
		return;
	if (*r.armed || !r.desc)
		return;
	for (i = 0; i < ZXNPU_MCU_RING_DEPTH; i++)
		put_unaligned_le32(0x80000000u, (u8 *)r.desc +
				   i * ZXNPU_MCU_DESC_SIZE + 4);
	zxnpu_main_cr_write(w, reg + 8, 0);
	zxnpu_main_cr_write(w, reg + 12, 0);
	zxnpu_main_cr_write(w, reg + 4, ZXNPU_MCU_RING_DEPTH);
	zxnpu_main_cr_write(w, reg, (u32)r.desc_dma);
	*r.armed = true;
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
	/* The pulse is not always honoured: a second reset on an MCU that
	 * kept a half-finished DL session left fw-state untouched and the
	 * command engine dead for the whole run (3.51 run 2). Retry until
	 * TOP_MISC actually moves. */
	{
		int try_, moved = 0;

		for (try_ = 0; try_ < 5 && !moved; try_++) {
			zxnpu_cr_write(w, ZXNPU_WF_SUBSYS_RST, rst | BIT(0));
			msleep(20);
			zxnpu_cr_write(w, ZXNPU_WF_SUBSYS_RST, rst & ~BIT(0));
			msleep(20);
			after = zxnpu_fw_state_read(w);
			if (after != before)
				moved = 1;
			else
				zxnpu_wlan_append(w, "  reset try %d: fw-state stuck at %u, retrying\n",
						  try_ + 1, after);
		}
		zxnpu_wlan_append(w, "  MT_WF_SUBSYS_RST post = %08x (%s)\n",
				  zxnpu_cr_read(w, ZXNPU_WF_SUBSYS_RST),
				  moved ? "state moved" : "state NEVER moved");
	}
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
	w->wac_ring_armed = false;
	w->fwdl_cidx = 0;
	w->mcu_cidx = 0;
	w->wac_cidx = 0;
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

	/* host-side rings: qid16 FWDL (0x18024400), qid17 MCU_WM
	 * (0x18024410) and qid20 MCU_WA (0x18024440); the ROM listens on
	 * FWDL for firmware data, the boot handshake rides MCU_WM, and the
	 * running firmware takes commands on MCU_WA (mt7915_dma_init
	 * allocates all three for exactly that reason). */
	w->fwdl_ring_armed = false;
	w->mcu_ring_armed = false;
	w->wac_ring_armed = false;
	zxnpu_wlan_queue_init(w, ZXNPU_TX_RING_FWDL);
	zxnpu_wlan_queue_init(w, ZXNPU_TX_RING_WM);
	zxnpu_wlan_queue_init(w, ZXNPU_TX_RING_WA_CMD);

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
 * Extended-command form. mt76 encodes a command as
 *   MCU_EXT_CMD(x) = MCU_CMD(EXT_CID) | FIELD_PREP(__MCU_CMD_FIELD_EXT_ID, x)
 * and mt76_connac2_mcu_fill_message() splits it into two TXD bytes: cid =
 * 0xed (MCU_CMD_EXT_CID) and ext_cid = x. Whenever ext_cid is non-zero it
 * also sets set_query (0 = MCU_Q_QUERY, 1 = MCU_Q_SET) and ext_cid_ack = 1;
 * a command with no ext_cid keeps MCU_Q_NA. Nothing else in the TXD changes
 * on this path: s2d_index stays MCU_S2D_H2N (0) unless the command carries
 * __MCU_CMD_FIELD_WA, which is the WA-directed form.
 */
static void zxnpu_mcu_txd_header_ext(u8 *msg, int payload_len, u8 cid,
				     u8 ext_cid, u8 set_query, u8 s2d, u8 seq)
{
	zxnpu_mcu_txd_header(msg, payload_len, cid, seq);
	msg[38] = set_query;
	msg[41] = ext_cid;
	msg[42] = s2d;				/* 0 = H2N (WM), 2 = H2C (WA) */
	msg[43] = ext_cid ? 1 : 0;		/* ext_cid_ack */
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
	return zxnpu_mcu_submit_ring_last(w, ring, msg, len, commit, verbose,
					  true);
}

static int zxnpu_mcu_submit_ring_last(struct zxnpu_wlan *w, int ring,
				      const u8 *msg, int len, bool commit,
				      bool verbose, bool last)
{
	struct zxnpu_tx_ring r;
	void *buf;
	dma_addr_t buf_dma;
	u32 reg = ZXNPU_WFDMA_TX_RING0 + ZXNPU_WFDMA_RING_STRIDE * ring;
	u32 slot, didx, prev;

	if (!w->bar[0]) {
		zxnpu_wlan_append(w, "  parked: enable=0 (reload with enable=1)\n");
		return 0;
	}
	if (!zxnpu_tx_ring_get(w, ring, &r) || !r.desc || !r.bufs ||
	    len <= 0 || len > ZXNPU_MCU_SLOT)
		return -EINVAL;
	zxnpu_wlan_queue_init(w, ring);

	/* flow control: never let the producer lap the consumer. Bounded to
	 * 40 ms per message - the earlier 1 s budget turned a stalled MCU into
	 * a one-hour download. didx is kept modulo the ring depth while cidx
	 * free-runs, so the distance has to be taken modulo depth too. */
	{
		int spins;

		for (spins = 0; spins < 200; spins++) {
			u32 didx_now = zxnpu_main_cr_read(w, reg + 12);
			u32 cidx_now = *r.cidx;

			if (((cidx_now - didx_now) &
			     (ZXNPU_MCU_RING_DEPTH - 1)) <
			    (ZXNPU_MCU_RING_DEPTH - 8))
				break;
			usleep_range(150, 250);
		}
	}

	slot = (*r.cidx) % ZXNPU_MCU_RING_DEPTH;
	buf = (u8 *)r.bufs + (u32)slot * ZXNPU_MCU_SLOT;
	buf_dma = r.bufs_dma + (dma_addr_t)slot * ZXNPU_MCU_SLOT;
	memset(buf, 0, ZXNPU_MCU_SLOT);
	memcpy(buf, msg, len);
	zxnpu_mcu_fill_desc_last(r.desc, slot, (u32)buf_dma, len, last);
	dma_wmb();

	prev = zxnpu_main_cr_read(w, reg + 12);
	if (verbose)
		zxnpu_wlan_append(w, "  submit qid%d slot=%u len=%d: base=%08x cnt=%u cidx=%u didx=%u\n",
				  zxnpu_tx_ring_qid(ring), slot, len,
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
	zxnpu_main_cr_write(w, reg, (u32)r.desc_dma);
	zxnpu_main_cr_write(w, reg + 4, ZXNPU_MCU_RING_DEPTH);
	(*r.cidx)++;
	/* cidx must live in the same modulo-depth space as the engine's didx.
	 * Free-running it looked harmless until the first wrap: from then on
	 * the engine's own (cidx - didx) arithmetic saw a whole extra lap of
	 * queued frames and treated the ring as full, so it stopped fetching
	 * mid-region (3.72: WM r4 stalled at didx 81->0, "outstanding 87",
	 * and every later command went unanswered). */
	if (*r.cidx >= ZXNPU_MCU_RING_DEPTH)
		*r.cidx = 0;
	zxnpu_main_cr_write(w, reg + 8, *r.cidx);
	didx = zxnpu_main_cr_read(w, reg + 12);
	if (didx != prev) {
		w->ring_stall = 0;
		if (verbose)
			zxnpu_wlan_append(w, "  didx advanced to %u\n", didx);
		return 0;
	}

	/* A didx that has not moved yet is not a failure: the engine picks
	 * frames up in bursts and the reading here is one instant after the
	 * doorbell. This used to return -EIO after 512 such instants, which
	 * is how 3.73 lost WA region 1 ("failed at -5") while the ring was
	 * in fact draining - ring-sync reported cidx==didx immediately after.
	 * The real detector is wait_txdrain(), which gives the ring 30 s and
	 * reports how much is still outstanding; nothing else should fail a
	 * download on a momentary index. */
	if (++w->ring_stall == 512) {
		zxnpu_wlan_append(w, "  qid%d didx has not moved for 512 submits (drain decides)\n",
				  zxnpu_tx_ring_qid(ring));
		w->ring_stall = 0;
	}
	if (verbose)
		zxnpu_wlan_append(w, "  didx still %u (stall %u)\n", didx,
				  w->ring_stall);
	return 0;
}

static int zxnpu_mcu_submit(struct zxnpu_wlan *w, const u8 *msg, int len,
			    bool commit, bool verbose)
{
	return zxnpu_mcu_submit_ring(w, zxnpu_wlan_mcu_ring, msg, len, commit,
				     verbose);
}

static void zxnpu_wlan_mcu_send(struct zxnpu_wlan *w, bool commit)
{
	u8 msg[ZXNPU_MCU_TXD_LEN + 4];

	zxnpu_wlan_append(w, "mcu-send PatchSemGet (cid %02x, qid %d): %s\n",
			  0x10, zxnpu_tx_ring_qid(zxnpu_wlan_mcu_ring),
			  commit ? "COMMIT" : "dry run");
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
	char *cur, *line, *sp;
	u8 *msg;
	int ret;

	/* the manifest is the rest of the line: "%127s" for it would stop at
	 * the first space and leave a single-field "region" that sscanf
	 * rejects ("bad region '0220a800'") */
	sp = strchr(args, ' ');
	if (!sp || sscanf(args, "%127s", name) != 1) {
		zxnpu_wlan_append(w, "usage: mcu-dl-regions <file> <addr len off [flags],...>\n");
		return;
	}
	strscpy(manifest, sp + 1, sizeof(manifest));
	ret = request_firmware_direct(&fw, name, &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "mcu-dl-regions: %s missing (%d)\n", name, ret);
		return;
	}
	msg = kmalloc(ZXNPU_MCU_SLOT, GFP_KERNEL);
	if (!msg) {
		release_firmware(fw);
		zxnpu_wlan_append(w, "mcu-dl-regions: no memory\n");
		return;
	}
	cur = manifest;
	while ((line = strsep(&cur, ",")) && *line) {
		unsigned long addr, len, off, flags = 0, done = 0;

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
	kfree(msg);
	release_firmware(fw);
}

/*
 * fw-start <option-hex> [override-hex]: MtCmdFwStartReq (mt7916.ko
 * 0x1CC1D0) is type 0x02 / cmd 0x17 with a two-word payload
 * {option, override_addr}. load_fw_v1 sets bit0 of option when a region
 * carried the override flag (feature_set 0x20) and bit2 for band 1.
 */
static u16 zxnpu_wlan_andes_seq;
static u8 zxnpu_wlan_mcu_seq;
/* Per-AddrLen response wait in ms. The waits ARE the protocol: fire-and-
 * forget desyncs the DL sessions and wedges the MCU solid (3.63 - even
 * wfsys-reset stops working afterwards). 2s keeps a run under a minute
 * while still matching responses; raise it only for slow-MCU debugging. */
static int zxnpu_wlan_dl_wait_ms = 2000;
module_param_named(dl_wait_ms, zxnpu_wlan_dl_wait_ms, int, 0644);

static int zxnpu_wlan_mcu_wait_event_st(struct zxnpu_wlan *w, u8 seq,
					int timeout_ms, int *status);

static void zxnpu_wlan_fw_start(struct zxnpu_wlan *w, const char *args, bool commit)
{
	/* mt76_connac_mcu_start_firmware: 64-byte connac2 TXD, cid 0x02
	 * (FW_START_REQ), 8-byte payload {option, entry}. The old 32-byte
	 * Andes header form was never answered. */
	u8 msg[ZXNPU_MCU_TXD_LEN + 8];
	u8 seq = ++zxnpu_wlan_mcu_seq;
	unsigned long option = 0, override = 0;
	int ret, st = -1;

	if (sscanf(args, "%lx %lx", &option, &override) < 1) {
		zxnpu_wlan_append(w, "usage: fw-start <option-hex> [override-hex]\n");
		return;
	}
	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header(msg, 8, 0x02, seq);
	put_unaligned_le32(option, msg + ZXNPU_MCU_TXD_LEN);
	put_unaligned_le32(override, msg + ZXNPU_MCU_TXD_LEN + 4);
	zxnpu_wlan_append(w, "fw-start option=%08lx override=%08lx seq=%u\n",
			  option, override, seq & 0xf);
	ret = zxnpu_mcu_submit(w, msg, sizeof(msg), commit, true);
	if (!ret && commit && zxnpu_wlan_dl_wait_ms > 0)
		ret = zxnpu_wlan_mcu_wait_event_st(w, seq,
						   zxnpu_wlan_dl_wait_ms, &st);
	zxnpu_wlan_append(w, "  fw-start: ret=%d status=%d\n", ret, st);
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
		zxnpu_wlan_queue_init(w, zxnpu_wlan_mcu_ring);
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

/* ---------------------------------------------------------------- eeprom */

/* The vendor image ships the whole MT7916 map at
 * /lib/firmware/zte/mt7916/MT7916_EEPROM.bin: exactly 4096 bytes = MT_EE_MAX_V2.
 * That is the same fallback mt76 uses (mt7915_eeprom_load_default()), and the
 * offsets prove it is a plain mt7915 v2 map rather than a private blob:
 *   0x000 chip id 0x7916     0x004 MAC
 *   0x190 WIFI_CONF per band 0x441/0x445 TX0_POWER_2G_V2 / 5G_V2
 * The efuse was tried first: MCU_EXT_QUERY(EFUSE_ACCESS) with the full
 * 24-byte mt7915_mcu_eeprom_info request is acknowledged but answers with no
 * payload for every address, so the file is the authoritative source here.
 * It is also what sets flash_mode, which obliges the driver to push the map
 * back to the firmware (MCU_EXT_CMD_EFUSE_BUFFER_MODE) before any TX.
 */
#define ZXNPU_EEPROM_FILE	"zte/mt7916/MT7916_EEPROM.bin"
#define ZXNPU_EEPROM_SIZE	0x1000
#define ZXNPU_EE_CHIP_ID	0x000
#define ZXNPU_EE_MAC_ADDR	0x004
#define ZXNPU_EE_WIFI_CONF	0x190
#define ZXNPU_EE_BAND_SEL	GENMASK(7, 6)
#define ZXNPU_EE_TX_PATH	GENMASK(2, 0)

static const char *zxnpu_wlan_band_name(u8 sel)
{
	switch (sel) {
	case 0:
		return "2GHz";
	case 1:
		return "5GHz";
	case 2:
		return "6GHz";
	default:
		return "5+6GHz";
	}
}

static int zxnpu_wlan_eeprom_load(struct zxnpu_wlan *w)
{
	const struct firmware *fw = NULL;
	u16 chip;
	int ret, band;
	u8 chain = 0;

	if (w->eeprom)
		return 0;
	ret = request_firmware_direct(&fw, ZXNPU_EEPROM_FILE, &w->pdev->dev);
	if (ret) {
		zxnpu_wlan_append(w, "eeprom: %s unavailable (%d)\n",
				  ZXNPU_EEPROM_FILE, ret);
		return ret;
	}
	if (fw->size < ZXNPU_EEPROM_SIZE) {
		zxnpu_wlan_append(w, "eeprom: %s is %zu bytes, need %#x\n",
				  ZXNPU_EEPROM_FILE, fw->size, ZXNPU_EEPROM_SIZE);
		release_firmware(fw);
		return -EINVAL;
	}
	w->eeprom = kzalloc(ZXNPU_EEPROM_SIZE, GFP_KERNEL);
	if (!w->eeprom) {
		release_firmware(fw);
		return -ENOMEM;
	}
	memcpy(w->eeprom, fw->data, ZXNPU_EEPROM_SIZE);
	release_firmware(fw);

	chip = get_unaligned_le16(w->eeprom + ZXNPU_EE_CHIP_ID);
	if (chip != 0x7916 && chip != 0x7915) {
		zxnpu_wlan_append(w, "eeprom: chip id %04x is not 7915/7916\n",
				  chip);
		kfree(w->eeprom);
		w->eeprom = NULL;
		return -EINVAL;
	}
	memcpy(w->mac, w->eeprom + ZXNPU_EE_MAC_ADDR, ETH_ALEN);
	for (band = 0; band < 2; band++) {
		u8 v = w->eeprom[ZXNPU_EE_WIFI_CONF + band];

		w->band_sel[band] = FIELD_GET(ZXNPU_EE_BAND_SEL, v);
		w->band_path[band] = FIELD_GET(ZXNPU_EE_TX_PATH, v);
		if (w->band_path[band] > 4)
			w->band_path[band] = 4;
		if (w->band_path[band])
			chain |= (BIT(w->band_path[band]) - 1) <<
				 (band ? w->band_path[0] : 0);
	}
	w->chainmask = chain;
	w->flash_mode = true;
	zxnpu_wlan_append(w,
			  "eeprom: chip=%04x mac=%pM band0=%s/%u band1=%s/%u chainmask=%x flash_mode=1\n",
			  chip, w->mac,
			  zxnpu_wlan_band_name(w->band_sel[0]), w->band_path[0],
			  zxnpu_wlan_band_name(w->band_sel[1]), w->band_path[1],
			  w->chainmask);
	return 0;
}

/* eeprom-show [offset-hex] [len] */static void zxnpu_wlan_eeprom_show(struct zxnpu_wlan *w, const char *args)
{
	unsigned int off = 0, len = 64;
	int i, j;

	sscanf(args, "%x %u", &off, &len);
	if (!w->eeprom) {
		zxnpu_wlan_append(w, "eeprom-show: not loaded (run eeprom-init)\n");
		return;
	}
	if (off >= ZXNPU_EEPROM_SIZE)
		off = 0;
	if (!len || len > 256)
		len = 64;
	if (off + len > ZXNPU_EEPROM_SIZE)
		len = ZXNPU_EEPROM_SIZE - off;
	for (i = 0; i < (int)len; i += 16) {
		zxnpu_wlan_append(w, "ee[%04x]:", off + i);
		for (j = 0; j < 16 && i + j < (int)len; j++)
			zxnpu_wlan_append(w, " %02x", w->eeprom[off + i + j]);
		zxnpu_wlan_append(w, "\n");
	}
}

/* Send one extended command with a payload larger than the 64 bytes mcu-ext
 * can put on the stack, and wait for its ack. */
static int zxnpu_wlan_mcu_ext_big(struct zxnpu_wlan *w, u8 ext, u8 q,
				  const u8 *pl, int pl_len, u8 s2d,
				  int timeout_ms)
{
	u8 *msg;
	u8 seq;
	int ret;

	msg = kmalloc(ZXNPU_MCU_TXD_LEN + pl_len, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	seq = ++zxnpu_wlan_mcu_seq;
	memset(msg, 0, ZXNPU_MCU_TXD_LEN);
	zxnpu_mcu_txd_header_ext(msg, pl_len, 0xed, ext, q, s2d, seq);
	memcpy(msg + ZXNPU_MCU_TXD_LEN, pl, pl_len);
	ret = zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + pl_len,
			       zxnpu_wlan_commit, false);
	kfree(msg);
	if (ret || !zxnpu_wlan_commit)
		return ret;
	return zxnpu_wlan_mcu_wait_event_st(w, seq, timeout_ms, NULL);
}

/*
 * eeprom-upload: hand the map to the firmware.
 *
 * mt7915_mcu_set_eeprom() branches on flash_mode: with a real efuse it merely
 * selects EE_MODE_EFUSE, but when the map came from a file it has to be pushed
 * page by page (mt7915_mcu_set_eeprom_flash):
 *
 *	MCU_EXT_CMD(EFUSE_BUFFER_MODE) = cid 0xed, ext 0x21
 *	header struct mt7915_mcu_eeprom { u8 buffer_mode; u8 format;
 *					  __le16 len; }
 *	  buffer_mode = EE_MODE_BUFFER (1)
 *	  format      = ((total_pages - 1) << 5) | (page_index << 2) |
 *			EE_FORMAT_WHOLE (1)
 *	  len         = page size, 0x400 except for the last page
 *	followed by len bytes of map.
 *
 * Without this the firmware has no calibration at all: RX may work but every
 * TX attempt is refused, so it is not optional once flash_mode is set.
 */
#define ZXNPU_EE_PAGE			0x400
#define ZXNPU_EE_BUFFER_MODE		0x01
#define ZXNPU_EE_FORMAT_WHOLE		0x01
#define ZXNPU_MCU_EXT_EFUSE_BUFFER	0x21

static int zxnpu_wlan_eeprom_upload(struct zxnpu_wlan *w)
{
	int i, total, ret = 0;
	u8 *page;

	if (!w->eeprom) {
		zxnpu_wlan_append(w, "eeprom-upload: run eeprom-init first\n");
		return -EINVAL;
	}
	total = DIV_ROUND_UP(ZXNPU_EEPROM_SIZE, ZXNPU_EE_PAGE);
	page = kmalloc(4 + ZXNPU_EE_PAGE, GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	zxnpu_wlan_append(w,
			  "eeprom-upload: %d pages of %#x via ext 0x%02x: %s\n",
			  total, ZXNPU_EE_PAGE, ZXNPU_MCU_EXT_EFUSE_BUFFER,
			  zxnpu_wlan_commit ? "COMMIT" : "dry run");
	for (i = 0; i < total; i++) {
		int len = ZXNPU_EE_PAGE;

		if (i == total - 1 && ZXNPU_EEPROM_SIZE % ZXNPU_EE_PAGE)
			len = ZXNPU_EEPROM_SIZE % ZXNPU_EE_PAGE;
		page[0] = ZXNPU_EE_BUFFER_MODE;
		page[1] = ((total - 1) << 5) | (i << 2) | ZXNPU_EE_FORMAT_WHOLE;
		put_unaligned_le16(len, page + 2);
		memcpy(page + 4, w->eeprom + i * ZXNPU_EE_PAGE, len);
		ret = zxnpu_wlan_mcu_ext_big(w, ZXNPU_MCU_EXT_EFUSE_BUFFER,
					     3 /* MCU_Q_NA */, page, 4 + len, 0,
					     5000);
		zxnpu_wlan_append(w,
				  "  page %d/%d len=%#x fmt=%02x: ret=%d (didx=%u cidx=%u)\n",
				  i + 1, total, len, page[1], ret,
				  zxnpu_cr_read(w, ZXNPU_WFDMA_RX_RING0 + 0x10 + 12),
				  zxnpu_cr_read(w, ZXNPU_WFDMA_RX_RING0 + 0x10 + 8));
		if (ret == -ETIMEDOUT) {
			/* the last page makes the firmware commit what it has
			 * written; its ack has been observed to land well past
			 * the 2 s command budget, so give it its own retry
			 * instead of failing the whole upload */
			zxnpu_wlan_append(w, "  page %d: retrying\n", i + 1);
			ret = zxnpu_wlan_mcu_ext_big(w,
						     ZXNPU_MCU_EXT_EFUSE_BUFFER,
						     3, page, 4 + len, 0, 5000);
			zxnpu_wlan_append(w, "  page %d: retry ret=%d\n",
					  i + 1, ret);
		}
		if (ret)
			break;
	}
	kfree(page);
	return ret;
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
 * mcu-ext <cid> <ext_cid> [query|set|na] [hex]
 *
 * The command form a *running* firmware expects. mcu-cmd sends the bare
 * Andes 32-byte header that the ROM download handler parses; once WM+WA
 * report fw-state 7 the MCU decodes the 64-byte connac2 TXD instead, and
 * everything beyond PATCH_SEM_CONTROL is an extended command (cid 0xed +
 * ext_cid). Example, mt7915_mcu_init_firmware's first post-download call:
 *
 *	wait:  mcu-ext ed 13 set 00000000     (FW_LOG_2_HOST, WM, ctrl 0)
 *	check: mcu-ext ed 13 query 00000000
 *
 * The reply is matched on seq only (mt7915_mcu_parse_response does the
 * same); buf[32] is ext_eid, printed as such rather than as a status.
 */
static void zxnpu_wlan_mcu_ext(struct zxnpu_wlan *w, const char *args,
			       bool commit, u8 s2d)
{
	u8 msg[ZXNPU_MCU_TXD_LEN + 64];
	u8 payload[64];
	unsigned int cid, ext_cid;
	const char *t3, *hex = NULL;
	u8 set_query = 3;			/* MCU_Q_NA */
	u8 seq;
	int n = 0, ret, eeid = -1;

	if (sscanf(args, "%x %x", &cid, &ext_cid) != 2) {
		zxnpu_wlan_append(w, "usage: mcu-ext <cid> <ext_cid> [query|set|na] [hex]\n");
		return;
	}
	/* third token: optional query/set/na selector, otherwise a hex payload */
	t3 = strchr(args, ' ');
	t3 = t3 ? strchr(t3 + 1, ' ') : NULL;
	if (t3) {
		t3++;
		if (!strncmp(t3, "query", 5)) {
			set_query = 0;		/* MCU_Q_QUERY */
			hex = strchr(t3, ' ');
		} else if (!strncmp(t3, "set", 3)) {
			set_query = 1;		/* MCU_Q_SET */
			hex = strchr(t3, ' ');
		} else if (!strncmp(t3, "na", 2)) {
			set_query = 3;		/* MCU_Q_NA */
			hex = strchr(t3, ' ');
		} else {
			hex = t3;
		}
	}
	if (hex) {
		while (*hex == ' ')
			hex++;
		while (hex[0] && hex[1] && n < (int)sizeof(payload)) {
			unsigned int hi = hex[0], lo = hex[1];

			if (hi == ' ') {
				hex++;
				continue;
			}
			hi = (hi >= '0' && hi <= '9') ? hi - '0' :
			     (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 :
			     (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : 0xff;
			lo = (lo >= '0' && lo <= '9') ? lo - '0' :
			     (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 :
			     (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : 0xff;
			if (hi == 0xff || lo == 0xff)
				break;
			payload[n++] = (hi << 4) | lo;
			hex += 2;
		}
	}
	seq = ++zxnpu_wlan_mcu_seq;
	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header_ext(msg, n, (u8)cid, (u8)ext_cid, set_query, s2d, seq);
	memcpy(msg + ZXNPU_MCU_TXD_LEN, payload, n);
	zxnpu_wlan_append(w, "mcu-ext cid=%02x ext=%02x q=%u s2d=%u len=%d seq=%u: %s\n",
			  cid, ext_cid, set_query, s2d, n, seq & 0xf,
			  commit ? "COMMIT" : "dry run");
	ret = zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + n, commit, true);
	if (!ret && commit && zxnpu_wlan_dl_wait_ms > 0) {
		ret = zxnpu_wlan_mcu_wait_event_st(w, seq,
						   zxnpu_wlan_dl_wait_ms, &eeid);
		zxnpu_wlan_append(w, "mcu-ext: ret=%d ext_eid=%d\n", ret, eeid);
	}
}


/*
 * efuse: the first command that returns *data* rather than an ack.
 *
 * MCU_EXT_QUERY(EFUSE_ACCESS) takes {addr, valid=0} and answers
 * {addr, valid, data[16]} (mt7915_mcu_eeprom_info). It is mt7915_mcu_get_eeprom()
 * and it is the gate in front of everything else: the map holds the chip id,
 * the MAC address and the band capabilities, none of which can be invented.
 * The response layout is confirmed at runtime rather than assumed - the raw
 * payload is printed so the first boot tells us whether the {addr, valid}
 * echo is present or the data starts at offset 0.
 */
#define ZXNPU_MCU_EXT_EFUSE_ACCESS	0x01
#define ZXNPU_EFUSE_BLOCK		16

/* defined with the event-ring code further down; the efuse reader above it
 * needs the payload-capturing form */
static int zxnpu_wlan_mcu_wait_event_buf(struct zxnpu_wlan *w, u8 seq,
					 int timeout_ms, int *status,
					 u8 *out, int out_cap, int *out_len);

static int zxnpu_wlan_efuse_block(struct zxnpu_wlan *w, u32 addr, u8 *out,
				  bool commit, bool verbose)
{
	u8 msg[ZXNPU_MCU_TXD_LEN + 8];
	u8 payload[8];
	u8 rbuf[64];
	u8 seq;
	int ret, got = 0, i;

	put_unaligned_le32(addr, payload + 0);
	put_unaligned_le32(0, payload + 4);
	seq = ++zxnpu_wlan_mcu_seq;
	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header_ext(msg, sizeof(payload), 0xed,
				 ZXNPU_MCU_EXT_EFUSE_ACCESS, 0 /* MCU_Q_QUERY */,
				 2, seq);
	memcpy(msg + ZXNPU_MCU_TXD_LEN, payload, sizeof(payload));
	ret = zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + sizeof(payload),
			       commit, false);
	if (ret || !commit)
		return ret;
	ret = zxnpu_wlan_mcu_wait_event_buf(w, seq, 500, NULL, rbuf,
					    sizeof(rbuf), &got);
	if (ret)
		return ret;
	if (verbose) {
		zxnpu_wlan_append(w, "  efuse[%04x] got=%d raw:", addr, got);
		for (i = 0; i < got; i++)
			zxnpu_wlan_append(w, " %02x", rbuf[i]);
		zxnpu_wlan_append(w, "\n");
	}
	/* Layout is learned, not assumed: mt7915_mcu_eeprom_info is
	 * {addr, valid, data[16]}, so the echoed address must be the first
	 * word. If it is not there the window above is the answer and this
	 * block is reported as "no data" rather than as bytes of garbage. */
	if (get_unaligned_le32(rbuf) == addr) {
		memcpy(out, rbuf + 8, ZXNPU_EFUSE_BLOCK);
		return 0;
	}
	return -ENODATA;
}

/* efuse-dump [start-hex] [blocks] */
static void zxnpu_wlan_efuse_dump(struct zxnpu_wlan *w, const char *args,
				  bool commit)
{
	unsigned int start = 0, nblk = 16;
	u32 a;
	int i, j;

	if (sscanf(args, "%x %u", &start, &nblk) < 1)
		start = 0;
	if (!nblk || nblk > 256)
		nblk = 16;
	start &= ~(u32)(ZXNPU_EFUSE_BLOCK - 1);
	zxnpu_wlan_append(w, "efuse-dump: start=%04x blocks=%u%s\n",
			  start, nblk, commit ? "" : " (dry run)");
	for (i = 0; i < (int)nblk; i++) {
		u8 blk[ZXNPU_EFUSE_BLOCK];
		int ret;

		a = start + (u32)i * ZXNPU_EFUSE_BLOCK;
		ret = zxnpu_wlan_efuse_block(w, a, blk, commit, true);
		if (ret) {
			zxnpu_wlan_append(w, "  stop at %04x ret=%d\n", a, ret);
			break;
		}
		zxnpu_wlan_append(w, "efuse[%04x]:", a);
		for (j = 0; j < ZXNPU_EFUSE_BLOCK; j++)
			zxnpu_wlan_append(w, " %02x", blk[j]);
		zxnpu_wlan_append(w, "\n");
	}
}

/*
 * mcu-tri   <type> <cmd> <band> [hex]   -> [64B connac2 TXD][32B Andes][payload]
 * mcu-andes <type> <cmd> <band> [hex]   -> [32B Andes][payload]
 *
 * Two candidate wire layouts for a command that a *running* firmware should
 * answer. The 64B-TXD-only form (mcu-ext) is drained but never answered, so
 * the question is what surrounds it. vendor's AndesAllocCmdMsgGeneric
 * reserves chip_cap[0x134] (= 64) bytes in front of the command buffer, and
 * MtCmdFwLog2Host fills an Andes header with +0 = band (0 = WM, 2 = WA),
 * +2 = 0xed (MCU_CMD_EXT_CID) and +3 = the command id - so the stock frame
 * is very likely TXD + Andes + payload, not either one alone.
 *
 * band is also the connac2 s2d_index (0 = H2N / WM, 2 = H2C / WA), so it is
 * written to the TXD as well. Nothing here decides which layout is right;
 * sending both and watching for a seq-matched event does.
 */
static void zxnpu_wlan_mcu_frame(struct zxnpu_wlan *w, const char *args,
				 bool commit, bool with_txd)
{
	u8 msg[ZXNPU_MCU_TXD_LEN + 32 + 64];
	u8 payload[64];
	unsigned int type, cmd, band = 0;
	const char *p;
	u8 seq;
	int n = 0, off = 0, ret, eeid = -1;
	unsigned long total;

	if (sscanf(args, "%x %x %x", &type, &cmd, &band) < 2) {
		zxnpu_wlan_append(w, "usage: mcu-<tri|andes> <type> <cmd> [band] [hex]\n");
		return;
	}
	/* hex payload starts after the three numeric tokens */
	p = args;
	{
		int t;

		for (t = 0; t < 3 && p; t++) {
			const char *sp = strchr(p, ' ');

			if (!sp)
				p = NULL;
			else
				p = sp + 1;
		}
	}
	if (p) {
		const char *h = p;

		while (h[0] && h[1] && n < (int)sizeof(payload)) {
			unsigned int hi, lo;

			if (h[0] == ' ') {
				h++;
				continue;
			}
			hi = (h[0] >= '0' && h[0] <= '9') ? h[0] - '0' :
			     (h[0] >= 'a' && h[0] <= 'f') ? h[0] - 'a' + 10 :
			     (h[0] >= 'A' && h[0] <= 'F') ? h[0] - 'A' + 10 : 0xff;
			lo = (h[1] >= '0' && h[1] <= '9') ? h[1] - '0' :
			     (h[1] >= 'a' && h[1] <= 'f') ? h[1] - 'a' + 10 :
			     (h[1] >= 'A' && h[1] <= 'F') ? h[1] - 'A' + 10 : 0xff;
			if (hi == 0xff || lo == 0xff)
				break;
			payload[n++] = (hi << 4) | lo;
			h += 2;
		}
	}
	seq = ++zxnpu_wlan_mcu_seq;
	memset(msg, 0, sizeof(msg));
	if (with_txd) {
		zxnpu_mcu_txd_header_ext(msg, 32 + n, (u8)type, (u8)cmd, 1,
					 (u8)band, seq);
		off = ZXNPU_MCU_TXD_LEN;
	}
	put_unaligned_le16((u16)band, msg + off + 0);
	msg[off + 2] = (u8)type;
	msg[off + 3] = (u8)cmd;
	put_unaligned_le16(seq, msg + off + 6);
	put_unaligned_le16(0x8000u, msg + off + 20);
	msg[off + 23] = 3;			/* wait for the reply */
	memcpy(msg + off + 32, payload, n);
	total = off + 32 + n;

	zxnpu_wlan_append(w, "mcu-%s type=%02x cmd=%02x band=%u len=%lu seq=%u: %s\n",
			  with_txd ? "tri" : "andes", type, cmd, band,
			  total, seq & 0xf, commit ? "COMMIT" : "dry run");
	ret = zxnpu_mcu_submit(w, msg, (int)total, commit, true);
	if (!ret && commit && zxnpu_wlan_dl_wait_ms > 0) {
		ret = zxnpu_wlan_mcu_wait_event_st(w, seq,
						   zxnpu_wlan_dl_wait_ms, &eeid);
		zxnpu_wlan_append(w, "mcu-%s: ret=%d ext_eid=%d\n",
				  with_txd ? "tri" : "andes", ret, eeid);
	}
}

/*
 * mcu-finish: inject PATCH_FINISH_REQ (cid 0x07) with the 64-byte connac2
 * TXD. Probe command - does Finish re-arm the ROM DL handler after a RAM
 * window completes (the patch window needs it, mt76's RAM flow never sends
 * one per region)?
 */
static void zxnpu_wlan_mcu_finish(struct zxnpu_wlan *w, bool commit)
{
	u8 msg[ZXNPU_MCU_TXD_LEN];
	u8 seq = ++zxnpu_wlan_mcu_seq;
	int ret, st = -1;

	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header(msg, 0, 0x07, seq);
	ret = zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN, commit, true);
	if (!ret && commit && zxnpu_wlan_dl_wait_ms > 0)
		ret = zxnpu_wlan_mcu_wait_event_st(w, seq,
						   zxnpu_wlan_dl_wait_ms, &st);
	zxnpu_wlan_append(w, "mcu-finish: ret=%d status=%d seq=%u\n", ret, st,
			  seq & 0xf);
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
/* After consuming N events from an RX ring the host must tell the engine
 * how many buffers it has posted, via CPU_IDX. Three candidate encodings,
 * because the wrong one wedges event delivery after the first event:
 *   1 = didx + depth   (what this driver did since 3.46: free-running;
 *                       difference becomes exactly depth = "ring full")
 *   2 = (didx - 1) mod depth  (mt76_dma_kick_queue writes q->head, and
 *                       mt76_dma_rx_fill never fills the last slot, so
 *                       head == (tail + ndesc - 1) % ndesc) <- CORRECT
 *   0 = don't touch CPU_IDX at all
 * Selectable at runtime so one boot can A/B them. 3.80 measured it: with
 * 1 the WA event ring delivered exactly ONE reply per fresh arming and then
 * went dead while the command ring kept draining; with 2 three commands in a
 * row were all answered (seq 05/06/07) and ring1 read cidx=2 didx=3. */
static int zxnpu_wlan_rx_repost = 2;
module_param_named(rx_repost, zxnpu_wlan_rx_repost, int, 0644);
MODULE_PARM_DESC(rx_repost,
		 "RX ring CPU_IDX after consuming events: 0=leave, 1=didx+depth, 2=(didx-1)%%depth (mt76)");

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
	/* same RX announce semantics as the MCU event ring: post every slot */
	zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 8, ZXNPU_DATA_DEPTH);
	w->drx_cidx = ZXNPU_DATA_DEPTH;
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG, (u32)w->dtx_ring_dma);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 4, ZXNPU_DATA_DEPTH);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 8, 0);
	/* BAND1 TX (qid19): same shape, same TXD gap as BAND0 (doc 42) */
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x10, (u32)w->dband_ring_dma);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x14, ZXNPU_DATA_DEPTH);
	zxnpu_cr_write(w, ZXNPU_DATA_TX_REG + 0x18, 0);
	w->drx_cidx = ZXNPU_DATA_DEPTH;
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
		/* 64 bytes: the Andes event TLV (status codes) lives at +32 */
		for (j = 0; j < 64; j++)
			zxnpu_wlan_append(w, " %02x", buf[j]);
		zxnpu_wlan_append(w, "\n");
	}
	if (n > 8)
		zxnpu_wlan_append(w, "  ... %u more\n", n - 8);
	if (n) {
		/* same tail + full-ring-announce model as the MCU event ring */
		w->drx_cidx = didx;
		if (zxnpu_wlan_commit)
			zxnpu_cr_write(w, ZXNPU_DATA_RX_REG + 8,
				       didx + ZXNPU_DATA_DEPTH);
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


static void zxnpu_wlan_rx_init(struct zxnpu_wlan *w, int ring, bool commit)
{
	u32 reg = ZXNPU_WFDMA_RX_RING0 + 0x10 * ring;
	void *ringmem = ring ? w->wa_ring : w->rx_ring;
	dma_addr_t ringdma = ring ? w->wa_ring_dma : w->rx_ring_dma;
	void *bufmem = ring ? w->wa_buf : w->rx_buf;
	dma_addr_t bufdma = ring ? w->wa_buf_dma : w->rx_buf_dma;
	u32 *pcidx = ring ? &w->wa_cidx : &w->rx_cidx;
	int i;

	zxnpu_wlan_append(w, "rx-init ring%d (%08x): %s\n", ring, reg,
			  commit ? "COMMIT" : "dry run");
	if (!ringmem || !bufmem) {
		zxnpu_wlan_append(w, "  no rx memory (probe with enable=1)\n");
		return;
	}
	for (i = 0; i < ZXNPU_MCU_RX_DEPTH; i++) {
		u8 *d = (u8 *)ringmem + i * ZXNPU_MCU_DESC_SIZE;
		u8 *buf = (u8 *)bufmem + i * ZXNPU_MCU_RX_BUF_SIZE;
		dma_addr_t bd = bufdma + i * ZXNPU_MCU_RX_BUF_SIZE;

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
			  reg,
			  zxnpu_cr_read(w, reg),
			  zxnpu_cr_read(w, reg + 4),
			  zxnpu_cr_read(w, reg + 8),
			  zxnpu_cr_read(w, reg + 12));
	if (!commit) {
		zxnpu_wlan_append(w, "  dry run: commit=1 to enable RX DMA\n");
		return;
	}
	{
		u32 glo = zxnpu_cr_read(w, ZXNPU_WFDMA_GLO_CFG);

		zxnpu_cr_write(w, ZXNPU_WFDMA_GLO_CFG, glo | 0x7);
	}
	zxnpu_cr_write(w, reg, (u32)ringdma);
	zxnpu_cr_write(w, reg + 4, ZXNPU_MCU_RX_DEPTH);
	/* CPU_IDX on an RX ring is the host's POSTED-BUFFER announcement, not a
	 * consume counter: mt76_dma_rx_fill() writes q->head (= ndesc) after
	 * refilling every slot, and the engine delivers while DMA_IDX chases
	 * it. Writing 0 here announced zero free slots, so the engine never
	 * delivered anything - the ROM stalled on its first DL response and
	 * qid16 froze after three scatter frames (3.46). */
	zxnpu_cr_write(w, reg + 8, ZXNPU_MCU_RX_DEPTH);
	*pcidx = 0;
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


/* rx-raw [ring] [first] [count]: hexdump ring buffer slots regardless of
 * DMA_IDX. The event rings are the only evidence channel for "did the MCU
 * write the reply but not bump the index", which looks identical to "no
 * reply at all" through scan_ring. */
static void zxnpu_wlan_rx_raw(struct zxnpu_wlan *w, int ring, int first,
			      int count)
{
	u32 reg = ZXNPU_WFDMA_RX_RING0 + 0x10 * ring;
	void *bufmem = ring ? w->wa_buf : w->rx_buf;
	int i, j;

	if (!bufmem) {
		zxnpu_wlan_append(w, "rx-raw: no rx memory\n");
		return;
	}
	zxnpu_wlan_append(w, "rx-raw ring%d: base=%08x cnt=%u cidx=%u didx=%u\n",
			  ring, zxnpu_cr_read(w, reg), zxnpu_cr_read(w, reg + 4),
			  zxnpu_cr_read(w, reg + 8), zxnpu_cr_read(w, reg + 12));
	for (i = first; i < first + count && i < ZXNPU_MCU_RX_DEPTH; i++) {
		u8 *b = (u8 *)bufmem + i * ZXNPU_MCU_RX_BUF_SIZE;
		bool zero = true;

		for (j = 0; j < 68; j++)
			if (b[j])
				zero = false;
		zxnpu_wlan_append(w, "  slot[%d]%s:", i, zero ? " (empty)" : "");
		if (zero)
			zxnpu_wlan_append(w, "\n");
		else {
			for (j = 0; j < 68; j++)
				zxnpu_wlan_append(w, " %02x", b[j]);
			zxnpu_wlan_append(w, "\n");
		}
	}
}

/* Drain one MCU event ring and report whether @seq showed up there. ring 0
 * is the WM ring (0x18024500), ring 1 the WA ring (0x18024510); events are
 * 36-byte Andes frames with eid at +28, seq at +29, ext_eid at +32. */
static int zxnpu_wlan_mcu_scan_ring(struct zxnpu_wlan *w, int ring, u8 seq,
				    int *ext_eid, u8 *out, int out_cap,
				    int *out_len)
{
	u32 reg = ZXNPU_WFDMA_RX_RING0 + 0x10 * ring;
	void *bufmem = ring ? w->wa_buf : w->rx_buf;
	void *desc = ring ? w->wa_ring : w->rx_ring;
	dma_addr_t bufdma = ring ? w->wa_buf_dma : w->rx_buf_dma;
	u32 *pcidx = ring ? &w->wa_cidx : &w->rx_cidx;
	u32 didx, tail, n, i, post;
	int found = 0;

	if (!bufmem)
		return 0;
	didx = zxnpu_cr_read(w, reg + 12);
	tail = *pcidx;
	n = didx - tail;
	if (n > ZXNPU_MCU_RX_DEPTH)
		n = ZXNPU_MCU_RX_DEPTH;
	if (!n)
		return 0;
	for (i = 0; i < n; i++) {
		u32 slot = (tail + i) % ZXNPU_MCU_RX_DEPTH;
		u8 *buf = (u8 *)bufmem + slot * ZXNPU_MCU_RX_BUF_SIZE;
		int j;

		/* one line per consumed event: the DL responses may lag the
		 * command by tens of seconds and a recycled 4-bit seq makes
		 * silent consumption unreadable. WM keeps the historical
		 * "evt[" tag so older run logs stay greppable. The byte
		 * window is behind evt_log: the tuned firmware posts several
		 * unsolicited events per second and a hex dump of each one
		 * buries the result that matters (see the param's comment). */
		if (ring)
			zxnpu_wlan_append(w, "  evtwa[%u] eid=%02x seq=%02x",
					  tail + i, buf[28], buf[29]);
		else
			zxnpu_wlan_append(w, "  evt[%u] eid=%02x seq=%02x",
					  tail + i, buf[28], buf[29]);
		if (zxnpu_wlan_evt_log) {
			for (j = 0; j < 24; j++)
				zxnpu_wlan_append(w, " %02x", buf[j]);
			zxnpu_wlan_append(w, " |");
			/* 36 header bytes plus 32 of payload: query answers
			 * (efuse blocks, register reads) carry their data past
			 * +36, and a 36-byte window made every such answer look
			 * empty. */
			for (j = 24; j < 68; j++)
				zxnpu_wlan_append(w, " %02x", buf[j]);
		}
		zxnpu_wlan_append(w, "\n");
		/* Hand the slot back: mt76_dma_rx_fill() rewrites ctrl (WITH
		 * LAST_SEC0, WITHOUT DMA_DONE) whenever it refills a consumed
		 * descriptor, and mt76_dma_rx_process() re-fills after every
		 * batch. Leaving the consumed descriptor in its delivered
		 * state retires one slot per event. */
		if (desc) {
			u8 *d = (u8 *)desc + slot * ZXNPU_MCU_DESC_SIZE;

			put_unaligned_le32(bufdma +
					   (dma_addr_t)slot * ZXNPU_MCU_RX_BUF_SIZE,
					   d + 0);
			put_unaligned_le32(0x40000000u |
					   (ZXNPU_MCU_RX_BUF_SIZE << 16), d + 4);
		}
		if (buf[29] == (seq & 0xf)) {
			found = 1;
			/* +32 is ext_eid, not a status: connac2 has no
			 * status byte for TARGET_ADDRESS_LEN_REQ
			 * (mt76_connac2_mcu_rxd puts ext_eid there, and
			 * mt7915_mcu_parse_response compares seq only).
			 * Reading it as "status 5 = rejected" cost 3.69/3.70
			 * two retry loops and a hang. */
			if (ext_eid)
				*ext_eid = buf[32];
			/* The TLV payload starts right after the 36-byte
			 * connac2 header (+36). Do NOT trust the length field
			 * at +24: it reads 0x0014 both for an 8-byte FW_LOG
			 * reply and for an efuse reply, so it is not the frame
			 * length. Copy a fixed window and let the caller
			 * validate it (efuse checks that the echoed address
			 * matches). */
			if (out && out_len) {
				memcpy(out, buf + 36, out_cap);
				*out_len = out_cap;
			}
		}
	}
	/* consumed tail advances to didx, then the posted-buffer count is
	 * re-announced. The encoding matters: didx + depth leaves the engine
	 * reading "difference == depth", i.e. a full ring with no home for
	 * the next event, which is exactly the shape of the 3.78 runs -
	 * one event answered, then silence on every ring. */
	*pcidx = didx;
	if (zxnpu_wlan_commit && zxnpu_wlan_rx_repost) {
		if (zxnpu_wlan_rx_repost == 2)
			post = (didx + ZXNPU_MCU_RX_DEPTH - 1) %
			       ZXNPU_MCU_RX_DEPTH;
		else
			post = didx + ZXNPU_MCU_RX_DEPTH;
		dma_wmb();
		zxnpu_cr_write(w, reg + 8, post);
	}
	return found;
}

static int zxnpu_wlan_mcu_wait_event_buf(struct zxnpu_wlan *w, u8 seq,
					 int timeout_ms, int *status,
					 u8 *out, int out_cap, int *out_len)
{
	int waited = 0;

	if (!w->rx_buf && !w->wa_buf)
		return -ENODEV;

	if (out_len)
		*out_len = 0;
	while (waited < timeout_ms) {
		int f0 = zxnpu_wlan_mcu_scan_ring(w, 0, seq, status,
						  out, out_cap, out_len);
		int f1 = zxnpu_wlan_mcu_scan_ring(w, 1, seq, status,
						  out, out_cap, out_len);

		if (f0 || f1)
			return 0;
		msleep(20);
		waited += 20;
	}
	zxnpu_wlan_append(w, "  wait_event seq=%u: timeout (%dms)\n",
			  seq & 0xf, timeout_ms);
	return -ETIMEDOUT;
}

static int zxnpu_wlan_mcu_wait_event_st(struct zxnpu_wlan *w, u8 seq,
					int timeout_ms, int *status)
{
	return zxnpu_wlan_mcu_wait_event_buf(w, seq, timeout_ms, status,
					     NULL, 0, NULL);
}

static int zxnpu_wlan_mcu_wait_event(struct zxnpu_wlan *w, u8 seq, int timeout_ms)
{
	return zxnpu_wlan_mcu_wait_event_st(w, seq, timeout_ms, NULL);
}

static void zxnpu_wlan_mcu_recv(struct zxnpu_wlan *w, int ring)
{
	u32 reg = ZXNPU_WFDMA_RX_RING0 + 0x10 * ring;
	void *bufmem = ring ? w->wa_buf : w->rx_buf;
	u32 *pcidx = ring ? &w->wa_cidx : &w->rx_cidx;
	u32 didx, cidx, n, i;

	if (!bufmem) {
		zxnpu_wlan_append(w, "mcu-recv: no rx memory\n");
		return;
	}
	didx = zxnpu_cr_read(w, reg + 12) % ZXNPU_MCU_RX_DEPTH;
	cidx = *pcidx % ZXNPU_MCU_RX_DEPTH;
	n = (didx + ZXNPU_MCU_RX_DEPTH - cidx) % ZXNPU_MCU_RX_DEPTH;
	zxnpu_wlan_append(w, "mcu-recv ring%d: cidx=%u didx=%u pending=%u\n",
			  ring, cidx, didx, n);
	for (i = 0; i < n; i++) {
		u32 slot = (cidx + i) % ZXNPU_MCU_RX_DEPTH;
		u8 *buf = (u8 *)bufmem + slot * ZXNPU_MCU_RX_BUF_SIZE;
		int j;

		zxnpu_wlan_append(w, "  slot %u:", slot);
		for (j = 0; j < 32; j++)
			zxnpu_wlan_append(w, " %02x", buf[j]);
		zxnpu_wlan_append(w, "\n");
	}
	if (n) {
		/* Consume: tail moves to didx, and the announce register keeps
		 * a full ring of slots in flight (tail + DEPTH) - the engine
		 * compares free-running counters and addresses slots modulo
		 * the depth (the live vendor ring sat at cidx=didx=0x2e). */
		*pcidx = didx;
		if (zxnpu_wlan_commit)
			zxnpu_cr_write(w, reg + 8,
				       didx + ZXNPU_MCU_RX_DEPTH);
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
	u8 *msg;	/* 4KB with the vendor chunk size - heap, not stack */
	unsigned int done = 0, total;
	char name[128];
	int ret;

	msg = kmalloc(ZXNPU_MCU_SLOT, GFP_KERNEL);
	if (!msg) {
		zxnpu_wlan_append(w, "mcu-dl: no memory for message buffer\n");
		return;
	}

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
		/* Patch auto mode, per the vendor's parse_patch_v2 (mt7916.ko
		 * 0x1df9e0) and mt76's struct mt76_connac2_patch_sec: a 0x60-byte
		 * header whose desc.n_region (BE32 @0x2c) counts 64-byte sections
		 * at 0x60, each {type BE, offs BE, size BE, dst BE, ...}. The old
		 * auto mode read desc.subsys (LE32 @0x24 = 0x04000000) as the
		 * target address and sent a bogus TARGET_ADDRESS_LEN_REQ - the
		 * ROM fetched two scatter frames and froze with fw-state 1. The
		 * real destination lives in section.info.addr: 0x900000 for this
		 * E1 patch, which is also what makes init_download pick cmd 0x05
		 * (PATCH_START_REQ) instead of 0x01. */
		u32 nsec, i;
		u8 sem_seq, al_seq = 0, fin_seq, rel_seq, pc_seq;

		if (fw->size < 0xa0 ||
		    get_unaligned_le32(fw->data + 0x20) != 0x11223344) {
			zxnpu_wlan_append(w, "mcu-dl: no dst given and no patch header\n");
			release_firmware(fw);
			return;
		}
		nsec = get_unaligned_be32(fw->data + 0x2c);
		zxnpu_wlan_append(w, "mcu-dl %s: patch, %u section(s) (%s)\n",
				  name, nsec, commit ? "COMMIT" : "dry run");

		/* mt7915_load_firmware: "Always restart MCU firmware" before the
		 * download - NIC_POWER_CTRL power_mode=1 puts the MCU firmware
		 * subsystem into the clean download state, then the host polls
		 * TOP_MISC for FW_STATE_FW_DOWNLOAD. Without it the ROM DL
		 * handler carried stale session state and rejected WM r1 with
		 * status 5 (3.56). */
		if (commit) {
			u32 st;
			int i;

			memset(msg, 0, ZXNPU_MCU_SLOT);
			pc_seq = ++zxnpu_wlan_mcu_seq;
			zxnpu_mcu_txd_header(msg, 4, 0x04, pc_seq);
			msg[ZXNPU_MCU_TXD_LEN] = 1;	/* power_mode = 1 */
			zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, true, true);
			if (zxnpu_wlan_dl_wait_ms > 0)
				zxnpu_wlan_mcu_wait_event(w, pc_seq, zxnpu_wlan_dl_wait_ms);
			for (i = 0; i < 100; i++) {
				st = zxnpu_fw_state_read(w);
				if (st == 1)
					break;
				msleep(20);
			}
			zxnpu_wlan_append(w, "  power-ctrl done, fw-state=%u (want 1)\n", st);
		}

		memset(msg, 0, ZXNPU_MCU_SLOT);
		/* PATCH_SEM_CONTROL, payload 1 = get the semaphore */
		sem_seq = ++zxnpu_wlan_mcu_seq;
		zxnpu_mcu_txd_header(msg, 4, 0x10, sem_seq);
		put_unaligned_le32(1, msg + ZXNPU_MCU_TXD_LEN);
		if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, commit, true))
			goto out;
		if (!commit) {
			release_firmware(fw);
			kfree(msg);
			return;
		}
		if (zxnpu_wlan_dl_wait_ms > 0)
			zxnpu_wlan_mcu_wait_event(w, sem_seq, zxnpu_wlan_dl_wait_ms);

		for (i = 0; i < nsec && i < 8; i++) {
			const u8 *sec = fw->data + 0x60 + i * 64;
			u32 type = get_unaligned_be32(sec + 0);
			u32 offs = get_unaligned_be32(sec + 4);
			u32 size = get_unaligned_be32(sec + 8);
			u32 addr = get_unaligned_be32(sec + 12);
			unsigned int sent = 0;

			if ((type & 0xffff) != 2)
				continue;
			if (offs > fw->size || size > fw->size - offs) {
				zxnpu_wlan_append(w, "  [%u] section beyond file\n", i);
				break;
			}
			zxnpu_wlan_append(w, "  [%u] addr=%08x len=%u off=%u\n",
					  i, addr, size, offs);

			memset(msg, 0, ZXNPU_MCU_SLOT);
			/* 0x900000 = PATCH_START_REQ (0x05) territory, everything
			 * else uses TARGET_ADDRESS_LEN_REQ (0x01) */
			al_seq = ++zxnpu_wlan_mcu_seq;
			zxnpu_mcu_txd_header(msg, 12, addr == 0x900000 ? 0x05 : 0x01, al_seq);
			put_unaligned_le32(addr, msg + ZXNPU_MCU_TXD_LEN);
			put_unaligned_le32(size, msg + ZXNPU_MCU_TXD_LEN + 4);
			put_unaligned_le32(0x80000000u, msg + ZXNPU_MCU_TXD_LEN + 8);
			if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 12, true, true))
				goto out;
			if (zxnpu_wlan_dl_wait_ms > 0)
				zxnpu_wlan_mcu_wait_event(w, al_seq, zxnpu_wlan_dl_wait_ms);

			while (sent < size) {
				unsigned int chunk = min_t(unsigned int, ZXNPU_MCU_CHUNK,
							   size - sent);

				memcpy(msg, fw->data + offs + sent, chunk);
				if (zxnpu_mcu_submit_ring(w, zxnpu_wlan_fw_ring, msg, chunk,
							  true, false))
					goto out;
				sent += chunk;
				usleep_range(500, 1000);
			}
		}

		memset(msg, 0, ZXNPU_MCU_SLOT);
		/* PATCH_FINISH_REQ, no payload */
		fin_seq = ++zxnpu_wlan_mcu_seq;
		zxnpu_mcu_txd_header(msg, 0, 0x07, fin_seq);
		if (zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN, true, true))
			goto out;
		if (zxnpu_wlan_dl_wait_ms > 0)
			zxnpu_wlan_mcu_wait_event(w, fin_seq, zxnpu_wlan_dl_wait_ms);
		/* SEM release (op 2): load_patch_v1 ends PatchFinishReq with a
		 * second MtCmdPatchSemGet - the loader must give the semaphore
		 * back or the MCU sits holding it through the WM/WA phase. */
		memset(msg, 0, ZXNPU_MCU_SLOT);
		rel_seq = ++zxnpu_wlan_mcu_seq;
		zxnpu_mcu_txd_header(msg, 4, 0x10, rel_seq);
		put_unaligned_le32(2, msg + ZXNPU_MCU_TXD_LEN);
		zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, true, true);
		if (zxnpu_wlan_dl_wait_ms > 0)
			zxnpu_wlan_mcu_wait_event(w, rel_seq, zxnpu_wlan_dl_wait_ms);
		/* vendor load_patch_v1 re-GETs the semaphore after the release -
		 * the status tells whether the patch actually activated (1 =
		 * already patched) or fell back to the bare ROM (2). */
		memset(msg, 0, ZXNPU_MCU_SLOT);
		rel_seq = ++zxnpu_wlan_mcu_seq;
		zxnpu_mcu_txd_header(msg, 4, 0x10, rel_seq);
		put_unaligned_le32(1, msg + ZXNPU_MCU_TXD_LEN);
		zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, true, true);
		if (zxnpu_wlan_dl_wait_ms > 0)
			zxnpu_wlan_mcu_wait_event(w, rel_seq, zxnpu_wlan_dl_wait_ms);
		zxnpu_wlan_append(w, "mcu-dl: patch sections sent, finish+sem-release issued\n");
		release_firmware(fw);
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
	/* SEM release (op 2): load_patch_v1 ends PatchFinishReq with a second
	 * MtCmdPatchSemGet - the loader must give the semaphore back or the
	 * MCU sits holding it through the WM/WA phase. */
	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header(msg, 4, 0x10, 4);
	put_unaligned_le32(2, msg + ZXNPU_MCU_TXD_LEN);
	zxnpu_mcu_submit(w, msg, ZXNPU_MCU_TXD_LEN + 4, true, true);
	zxnpu_wlan_append(w, "mcu-dl: %u bytes sent, finish+sem-release issued\n",
			  done);
out:
	release_firmware(fw);
	kfree(msg);
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


/* Wait until the MCU has drained the FWDL ring (cidx == didx). The ring is
 * the one scatter actually uses (fw_ring), not unconditionally ring 0.
 *
 * Both indices live in the ring's own module-`depth` space: the engine keeps
 * didx modulo ZXNPU_MCU_RING_DEPTH, while our software cidx free-runs. 3.71
 * compared them raw, so from frame 128 on nothing could ever match and every
 * drain reported a timeout even though the ring was empty (3.71 stage B:
 * "cidx=175 didx=47" after 175 submitted frames - 47 + 128 == 175). */
static u32 zxnpu_wlan_ring_used(u32 cidx, u32 didx)
{
	return (cidx - didx) & (ZXNPU_MCU_RING_DEPTH - 1);
}

static int zxnpu_wlan_wait_txdrain(struct zxnpu_wlan *w, int timeout_ms)
{
	u32 reg = ZXNPU_WFDMA_TX_RING0 +
		  ZXNPU_WFDMA_RING_STRIDE * zxnpu_wlan_fw_ring;
	u32 cidx = zxnpu_wlan_fw_ring ? w->mcu_cidx : w->fwdl_cidx;
	int waited = 0;

	while (waited < timeout_ms) {
		u32 didx = zxnpu_main_cr_read(w, reg + 12);

		if (!zxnpu_wlan_ring_used(cidx, didx))
			return 0;
		msleep(20);
		waited += 20;
	}
	zxnpu_wlan_append(w, "  txdrain timeout (qid%d cidx=%u didx=%u outstanding=%u)\n",
			  zxnpu_wlan_fw_ring ? 17 : 16, cidx,
			  zxnpu_main_cr_read(w, reg + 12),
			  zxnpu_wlan_ring_used(cidx,
			      zxnpu_main_cr_read(w, reg + 12)));
	return -ETIMEDOUT;
}

static int zxnpu_mcu_addrlen(struct zxnpu_wlan *w, u32 addr, u32 len,
			     u32 mode, bool commit)
{
	/* Vendor MtCmdAddressLenReq (0x1d62dc, identical in the E2631 run.ko):
	 * 64-byte connac2 TXD, cid 0x05 (PATCH_START_REQ) for the patch window
	 * at 0x900000, cid 0x01 (TARGET_ADDRESS_LEN_REQ) for every RAM region,
	 * 12-byte payload {addr, len, mode}. The vendor router
	 * (mt7916_kick_out_fwdl_msg) sends ONLY fw_dl_state==2 traffic - the
	 * FwScatter chunks - to the FWDL ring; AddrLen always rides the cmd
	 * ring, exactly as load_fw_v1 sets state 1 before each call. */
	u8 msg[ZXNPU_MCU_TXD_LEN + 12];
	u8 seq = ++zxnpu_wlan_mcu_seq;
	int ret;

	memset(msg, 0, sizeof(msg));
	zxnpu_mcu_txd_header(msg, 12, addr == 0x900000 ? 0x05 : 0x01, seq);
	put_unaligned_le32(addr, msg + ZXNPU_MCU_TXD_LEN);
	put_unaligned_le32(len, msg + ZXNPU_MCU_TXD_LEN + 4);
	put_unaligned_le32(mode, msg + ZXNPU_MCU_TXD_LEN + 8);
	/* One request, one wait. mt7915_mcu_parse_response() compares the
	 * event seq for TARGET_ADDRESS_LEN_REQ / FW_SCATTER / FW_START_REQ and
	 * nothing else - connac2 has no status byte for these commands (the
	 * word at +32 of the event is ext_eid). 3.69 read +32 as "status" and
	 * re-sent the identical AddrLen fifteen times on every non-zero value,
	 * which turned a normal ext_eid into fifteen extra commands on a ring
	 * the DL handler was not serving. Match the seq, move on. */
	ret = zxnpu_mcu_submit(w, msg, sizeof(msg), commit, false);
	if (!ret && commit && zxnpu_wlan_dl_wait_ms > 0)
		ret = zxnpu_wlan_mcu_wait_event(w, seq, zxnpu_wlan_dl_wait_ms);
	return ret;
}

/* FW_SCATTER wire format.
 *
 * The patch stage (mcu-dl / patch-dl) has always sent raw chunks on the
 * FWDL ring and works end to end, which is also what mt76 does: for
 * MCU_CMD(FW_SCATTER) mt76_connac2_mcu_fill_message() returns before
 * pushing any txd, so the frame is just the payload. The WM/WA path was
 * instead sending a 32-byte Andes header straight onto the wire - the
 * vendor only ever emits that header *behind* a 64-byte connac2 TXD
 * (AndesAllocCmdMsgGeneric reserves chip_cap[0x134] = 64 bytes). With the
 * header in front and no TXD, the loader counts those 32 bytes as
 * firmware data: WM r0 (126976 B) "completes" early, r1 (205824 B) leaves
 * the loader a frame and a half short, and from r2 on the MCU stops
 * serving the command ring entirely (3.68/3.69: 15 retries, all silent).
 *
 * The default is therefore the framing this silicon has already accepted.
 */
static int zxnpu_wlan_scatter_fmt;		/* 0 = raw, 1 = Andes, 2 = both */
module_param_named(scatter_fmt, zxnpu_wlan_scatter_fmt, int, 0644);
MODULE_PARM_DESC(scatter_fmt,
		 "FW_SCATTER framing: 0=raw payload (mt76/patch-proven), 1=32B Andes header, 2=64B TXD + 32B Andes");
static int zxnpu_wlan_scatter_port = 0x8000;
module_param_named(scatter_port, zxnpu_wlan_scatter_port, int, 0644);
MODULE_PARM_DESC(scatter_port,
		 "Andes header +20 port; GetRealPortQueueID gives 0x8000 for control and chip_cap[0x162] for type 0xEE");
static int zxnpu_wlan_scatter_log;
module_param_named(scatter_log, zxnpu_wlan_scatter_log, int, 0644);
MODULE_PARM_DESC(scatter_log,
		 "log the first frame of each region and the FWDL didx before/after");
static int zxnpu_wlan_fw_regions;
module_param_named(fw_regions, zxnpu_wlan_fw_regions, int, 0644);
MODULE_PARM_DESC(fw_regions,
		 "download at most this many WM/WA regions (0 = all): the stop knob that keeps a bad run short");

#define ZXNPU_MCU_ANDES_LEN	32
#define ZXNPU_MCU_FMT2_EXTRA	(ZXNPU_MCU_TXD_LEN + ZXNPU_MCU_ANDES_LEN)

static int zxnpu_mcu_scatter(struct zxnpu_wlan *w, const u8 *data, u32 len,
			     bool commit)
{
	u8 *msg = kmalloc(ZXNPU_MCU_SLOT, GFP_KERNEL);
	u32 reg = ZXNPU_WFDMA_TX_RING0 +
		  ZXNPU_WFDMA_RING_STRIDE * zxnpu_wlan_fw_ring;
	u32 pmax, done = 0, frames = 0, didx0 = 0;
	int fmt = zxnpu_wlan_scatter_fmt;
	int rc = 0;

	if (!msg)
		return -ENOMEM;
	if (fmt < 0 || fmt > 2) {
		zxnpu_wlan_append(w, "  scatter_fmt %d out of range, using 0\n",
				  fmt);
		fmt = 0;
	}
	/* fmt2 carries 96 bytes of headers, so its payload has to shrink to
	 * stay inside one slot */
	pmax = fmt == 2 ? ZXNPU_MCU_SLOT - ZXNPU_MCU_FMT2_EXTRA : ZXNPU_MCU_CHUNK;
	w->ring_stall = 0;	/* per-region, so the log means something */
	if (commit)
		didx0 = zxnpu_main_cr_read(w, reg + 12);

	while (done < len) {
		u32 chunk = min_t(u32, pmax, len - done);
		bool last = done + chunk >= len;
		int flen;

		memset(msg, 0, ZXNPU_MCU_SLOT);
		if (fmt == 0) {
			memcpy(msg, data + done, chunk);
			flen = chunk;
		} else {
			u8 *a;	/* the 32B Andes header */

			if (fmt == 2) {
				/* connac2 TXD first (cid 0xEE, the value
				 * GetRealPortQueueID keys the DL queue off) */
				zxnpu_mcu_txd_header(msg,
						     (int)(ZXNPU_MCU_ANDES_LEN + chunk),
						     0xEE, ++zxnpu_wlan_mcu_seq);
				a = msg + ZXNPU_MCU_TXD_LEN;
			} else {
				a = msg;
			}
			put_unaligned_le16(ZXNPU_MCU_ANDES_LEN + chunk, a + 0);
			a[2] = 0xEE;
			a[4] = 0x10;			/* cmd, also flags */
			put_unaligned_le16(++zxnpu_wlan_andes_seq, a + 6);
			put_unaligned_le16((u16)zxnpu_wlan_scatter_port, a + 20);
			/* +23 = flags&2 ? 3 : 0; scatter's 0x10 has bit1
			 * clear, so this must be 0 (3.69 wrote 3) */
			a[23] = 0;
			memcpy(a + ZXNPU_MCU_ANDES_LEN, data + done, chunk);
			flen = (int)(a - msg) + ZXNPU_MCU_ANDES_LEN + chunk;
		}
		if (zxnpu_wlan_scatter_log && (!frames || !(frames % 128)))
			zxnpu_wlan_append(w, "  scatter f%u off=%u len=%d\n",
					  frames, done, flen);
		if (zxnpu_mcu_submit_ring_last(w, zxnpu_wlan_fw_ring, msg, flen,
					       commit, false, last)) {
			rc = -EIO;
			break;
		}
		done += chunk;
		frames++;
		if (!commit)
			break;
		usleep_range(500, 1000);
	}
	if (zxnpu_wlan_scatter_log && commit)
		zxnpu_wlan_append(w, "  scatter fmt=%d frames=%u bytes=%u didx %u->%u\n",
				  fmt, frames, done, didx0,
				  zxnpu_main_cr_read(w, reg + 12));
	kfree(msg);
	return rc;
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
	zxnpu_wlan_append(w, "  %s %s%s: %d regions\n", wa ? "WA" : "WM", ver,
			  date, n);

	/* Vendor WfMcuInit runs this between NICLoadRomPatch and
	 * NICLoadFirmware (0xAD870): read conn_infra 0x41143c, clear bits
	 * 1:0, write back - for every chip id except 0x7615/0x7622, so the
	 * MT7916 gets it. Without it the ROM DL handler refuses every
	 * command after the patch window with status 5 (3.69: 15 deterministic
	 * retries, all instant status 5; Finish rejected too). Bits 1:0 look
	 * like a WM-core hold in the conn_infra block, same family as mt76's
	 * MT_CONN_ON_MISC (0x410120f0, MT_TOP_MISC2_FW_N9_RDY GENMASK(1,0)). */
	if (!wa) {
		u32 v = zxnpu_cr_read(w, 0x41143c);

		zxnpu_wlan_append(w, "  interlude: 0x41143c=%08x -> %08x\n", v,
				  v & ~3u);
		zxnpu_cr_write(w, 0x41143c, v & ~3u);
		zxnpu_wlan_append(w, "  interlude readback %08x\n",
				  zxnpu_cr_read(w, 0x41143c));
	}

	/* mt76_connac_mcu_send_ram_firmware / vendor load_fw_v1, unified: one
	 * AddrLen + FwScatter round per region, then a single FwStartReq.
	 * feature_set 0x40 = FW_FEATURE_NON_DL (skip), 0x20 =
	 * FW_FEATURE_OVERRIDE_ADDR (download; its address becomes the
	 * FwStartReq entry point with FW_START_OVERRIDE, option bit0). The WA
	 * image adds DL_MODE_WORKING_PDA_CR4 (mode bit4, the vendor's
	 * load_fw_v1 "r9==1 ? 0x10") and FwStart option bit2. RestartDL
	 * (0xEF) is NOT part of the flow - the vendor only sends it from
	 * NICRestartFirmware for error recovery, never between regions. */
	for (i = 0; i < n; i++) {
		struct zxnpu_fw_region *r = &regs[i];
		u32 mode;
		int ret;

		if (r->feature & 0x40)
			continue;
		if (r->feature & 0x20)
			override = r->addr;

		/* mt76_connac_mcu_gen_dl_mode: DL_MODE_NEED_RSP (bit31)
		 * always, encrypt/key bits from feature_set, CR4 bit for the
		 * WA image. The WM file's regions carry feature 0 and 0x20
		 * only, so this normally resolves to 0x80000000 (+0x10 WA). */
		mode = 0x80000000u;
		if (r->feature & 0x1)
			mode |= 0x1 | 0x8;		/* ENCRYPT|RESET_SEC_IV */
		mode |= ((r->feature & 0x6) >> 1) << 1;	/* KEY_IDX */
		if (r->feature & 0x10)
			mode |= 0x40;		/* DL_CONFIG_ENCRY_MODE_SEL */
		if (wa)
			mode |= 0x10;		/* DL_MODE_WORKING_PDA_CR4 */

		zxnpu_wlan_append(w, "  [%d] addr=%08x len=%u feat=%02x mode=%08x\n",
				  i, r->addr, r->len, r->feature, mode);
		ret = zxnpu_mcu_addrlen(w, r->addr, r->len, mode, commit);
		if (ret == -ETIMEDOUT) {
			zxnpu_wlan_append(w, "  addrlen no response - MCU stopped serving the command ring\n");
			return ret;
		}
		if (ret)
			return ret;
		if (!commit)
			return 0;
		msleep(10);
		ret = zxnpu_mcu_scatter(w, fw->data + r->off, r->len, true);
		if (ret)
			return ret;
		/* didx is the only witness that the frames were actually read:
		 * a wedged loader stops consuming long before it stops acking
		 * commands. */
		if (zxnpu_wlan_scatter_log)
			zxnpu_wlan_append(w, "  didx before drain=%u\n",
					  zxnpu_main_cr_read(w,
					      ZXNPU_WFDMA_TX_RING0 +
					      ZXNPU_WFDMA_RING_STRIDE *
					      zxnpu_wlan_fw_ring + 12));
		if (zxnpu_wlan_wait_txdrain(w, 30000))
			return -ETIMEDOUT;
		if (zxnpu_wlan_fw_regions && i + 1 >= zxnpu_wlan_fw_regions) {
			zxnpu_wlan_append(w, "  stopping after %d region(s) (fw_regions)\n",
					  i + 1);
			return -ECANCELED;
		}
	}
	if (!commit)
		return 0;

	option = (override ? 0x1 : 0) | (wa ? 0x4 : 0);
	zxnpu_wlan_append(w, "  fw-start option=%08x entry=%08x\n", option,
			  override);
	{
		u8 msg[ZXNPU_MCU_TXD_LEN + 8];
		u8 seq = ++zxnpu_wlan_mcu_seq;
		int ret;

		memset(msg, 0, sizeof(msg));
		zxnpu_mcu_txd_header(msg, 8, 0x02, seq);
		put_unaligned_le32(option, msg + ZXNPU_MCU_TXD_LEN);
		put_unaligned_le32(override, msg + ZXNPU_MCU_TXD_LEN + 4);
		/* vendor load_fw_v1 ends every stage with FwStartReq on the cmd
		 * ring (state 1); mt76 sets FW_START_OVERRIDE when a region
		 * carried FW_FEATURE_OVERRIDE_ADDR - the WM's region 0
		 * (0x0220a800) does. The vendor's parse_fw_v1 hardcodes entry
		 * 0x100000 instead; if the MCU rejects 0x0220a800 that is the
		 * first knob to turn. */
		ret = zxnpu_mcu_submit(w, msg, sizeof(msg), commit, true);
		if (!ret && commit)
			/* the WM answer can lag while the MCU boots the image */
			ret = zxnpu_wlan_mcu_wait_event(w, seq, 30000);
		return ret;
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
	if (ret == -ECANCELED) {
		zxnpu_wlan_append(w, "fw-dl: stopped by fw_regions=%d, no fw-start sent\n",
				  zxnpu_wlan_fw_regions);
	} else if (ret) {
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

/*
 * ---- firmware bring-up: DEV / MAC / channel / BSS (doc 62 s84) ----------
 *
 * The MCU running (fw-state 7) and the calibration uploaded are necessary but
 * not sufficient: the firmware will drop every frame until the host has said
 * which bands the MAC owns, which channel to tune and which BSS sits there.
 * mt76 does that in mt7915_mcu_set_mac(), mt7915_mcu_add_dev_info(),
 * mt7915_mcu_set_chan_info() and mt7915_mcu_add_bss_info(); the byte layouts
 * below are those four functions, source cross-checked against
 * mt76-2026.10.08 (mt76_connac_mcu.h / mt7915/mcu.c).
 *
 * All of them are MCU_EXT_CMD (cid 0xed) with set_query = MCU_Q_SET (1) and
 * s2d = MCU_S2D_H2N (0): after MCU_RUNNING the ring is qid20 (MCU_WA) and the
 * WA core answers on its own event ring, which is the path 3.80 proved.
 *
 * Sizes are the firmware ABI and are fixed: 20 B dev_info, 4 B mac_init,
 * 76 B channel_switch, and a sta_req_hdr + TLV list for BSS_INFO_UPDATE.
 */
#define ZXNPU_MCU_EXT_CHAN_SWITCH	0x08
#define ZXNPU_MCU_EXT_DEV_INFO		0x2a
#define ZXNPU_MCU_EXT_BSS_INFO		0x26
#define ZXNPU_MCU_EXT_MAC_INIT		0x46
#define ZXNPU_MCU_EXT_RX_HDR_TRANS	0x47

#define ZXNPU_MCU_Q_SET			1
#define ZXNPU_MCU_S2D_H2N		0

#define ZXNPU_DEV_INFO_ACTIVE		0
#define ZXNPU_BSS_INFO_OMAC		0
#define ZXNPU_BSS_INFO_BASIC		1
#define ZXNPU_BSS_INFO_RF_CH		2
#define ZXNPU_BSS_INFO_BMC_RATE		8

#define ZXNPU_NETWORK_INFRA		0x00010000
#define ZXNPU_CONN_INFRA_AP		0x00010002	/* STA_TYPE_AP | INFRA */
#define ZXNPU_PHY_MODE_HT24		0x00000007	/* CCK | OFDM | HT */
#define ZXNPU_CHAN_BW_20		0

/* Chains actually populated on this band, from the eeprom select (2 on
 * band0, 3 on band1 on this board). CHAN_INFO carries both a count
 * (tx_path_num) and a mask (rx_path); mt76 derives them from mphy->chainmask
 * and mt7915_band_chainshift(). */
static u8 zxnpu_band_chains(struct zxnpu_wlan *w, int band)
{
	return w->band_path[band] ? w->band_path[band] : 1;
}

static u8 zxnpu_band_mask(struct zxnpu_wlan *w, int band)
{
	return (u8)((1u << zxnpu_band_chains(w, band)) - 1);
}

static int zxnpu_wlan_ext_set(struct zxnpu_wlan *w, u8 ext, const u8 *pl, int len)
{
	return zxnpu_wlan_mcu_ext_big(w, ext, ZXNPU_MCU_Q_SET, pl, len,
				      ZXNPU_MCU_S2D_H2N, 2000);
}

/* tlv { le16 tag, le16 len, payload } with len counting the header, the
 * encoding mt76_connac_mcu_add_tlv() uses. Returns the new offset. */
static int zxnpu_tlv_put(u8 *buf, int off, int max, u16 tag,
			 const void *payload, int len)
{
	if (off + 4 + len > max)
		return -ENOSPC;
	put_unaligned_le16(tag, buf + off);
	put_unaligned_le16(len + 4, buf + off + 2);
	memcpy(buf + off + 4, payload, len);
	return off + 4 + len;
}

/*
 * DEV_INFO_UPDATE (ext 0x2a): mt7915_mcu_add_dev_info. The firmware binds
 * omac_idx/band_idx to the address the MAC uses on that band; without it
 * BSS_INFO_UPDATE is answered but has no device to attach to.
 *   hdr: omac_idx, band_idx, tlv_num=1, is_tlv_append=1, rsv[3]
 *   tlv DEV_INFO_ACTIVE: tag, len=12, active, band_idx, omac_addr[6]
 */
static int zxnpu_wlan_fw_dev_info(struct zxnpu_wlan *w, int band, bool enable)
{
	u8 req[20];

	memset(req, 0, sizeof(req));
	req[0] = 0;			/* omac_idx: first AP slot */
	req[1] = band;
	put_unaligned_le16(1, req + 2);	/* tlv_num */
	req[4] = 1;			/* is_tlv_append */

	put_unaligned_le16(ZXNPU_DEV_INFO_ACTIVE, req + 8);
	put_unaligned_le16(12, req + 10);
	req[12] = enable ? 1 : 0;
	req[13] = band;
	memcpy(req + 14, w->mac, 6);

	return zxnpu_wlan_ext_set(w, ZXNPU_MCU_EXT_DEV_INFO, req, sizeof(req));
}

/*
 * RX_HDR_TRANS (0x47) + MAC_INIT_CTRL (0x46): mt7915_mcu_set_mac.
 * hdr_trans stays 0, i.e. the host builds the 802.11 header and the TXD, and
 * RX arrives with its 802.11 header + RX status intact - which is what the
 * host-side drain in data-recv needs. MAC_INIT_CTRL then enables the band.
 */
static int zxnpu_wlan_fw_mac(struct zxnpu_wlan *w, int band, bool enable)
{
	u8 rxt[8];
	u8 req[4];
	int ret;

	memset(rxt, 0, sizeof(rxt));	/* operation 0, hdr_trans off */
	ret = zxnpu_wlan_ext_set(w, ZXNPU_MCU_EXT_RX_HDR_TRANS, rxt,
				 sizeof(rxt));
	zxnpu_wlan_append(w, "  rx-hdr-trans band %d off: ret=%d\n", band, ret);
	if (ret)
		return ret;

	memset(req, 0, sizeof(req));
	req[0] = enable ? 1 : 0;
	req[1] = band;
	return zxnpu_wlan_ext_set(w, ZXNPU_MCU_EXT_MAC_INIT, req, sizeof(req));
}

/*
 * CHANNEL_SWITCH (0x08): mt7915_mcu_set_chan_info. 76 bytes; the firmware
 * runs the RF/BB channel set (and, with the eeprom in place, the per-channel
 * calibration) from it. switch_reason = CH_SWITCH_NORMAL (0), so no CAC/DFS
 * bypass games. Only 20 MHz for now: bw != 0 needs the matching center_ch.
 *
 * rx_path is a COUNT here, not a mask: mt7915_mcu_set_chan_info() reshapes it
 * with `if (cmd == MCU_EXT_CMD(CHANNEL_SWITCH)) req.rx_path = hweight8(...)`.
 * 3.86 sent the 0x3 mask on a 2-chain band and the WA core wedged inside the
 * tune - every later command timed out, including FW_LOG which had answered
 * minutes earlier. The timeout budget is wider for the same reason: the first
 * tune runs the calibration, not just a PLL retune.
 */
static int zxnpu_wlan_fw_chan(struct zxnpu_wlan *w, int band, unsigned int ch)
{
	u8 req[76];

	if (ch < 1 || ch > 196)
		return -EINVAL;
	memset(req, 0, sizeof(req));
	req[0] = ch & 0xff;			/* control_ch */
	req[1] = ch & 0xff;			/* center_ch (20 MHz) */
	req[2] = ZXNPU_CHAN_BW_20;
	req[3] = zxnpu_band_chains(w, band);	/* tx_path_num */
	req[4] = zxnpu_band_chains(w, band);	/* rx_path: count, not mask */
	req[5] = 0;				/* CH_SWITCH_NORMAL */
	req[6] = band;				/* band_idx */
	req[10] = band ? 1 : 0;			/* channel_band */

	return zxnpu_wlan_mcu_ext_big(w, ZXNPU_MCU_EXT_CHAN_SWITCH,
				      ZXNPU_MCU_Q_SET, req, sizeof(req),
				      ZXNPU_MCU_S2D_H2N, 5000);
}

/*
 * BSS_INFO_UPDATE (0x26): mt7915_mcu_add_bss_info. sta_req_hdr followed by
 * the TLV list. Enable sends OMAC + BASIC + RF_CH + BMC_RATE (the HE and
 * HW_AMSDU TLVs are omitted: no HE support advertised yet, and an optional
 * TLV is exactly that). Disable sends BASIC alone with active = 0.
 */
static int zxnpu_wlan_fw_bss(struct zxnpu_wlan *w, int band, bool enable)
{
	struct zxnpu_bss *bss = &zxna_bss[band];
	u8 req[128];
	u8 tlv[24];
	int off = 8, n = 0, ret;

	if (enable && !bss->channel)
		return -EINVAL;

	memset(req, 0, sizeof(req));
	req[0] = band;		/* bss_idx */
	req[1] = 0;		/* wlan_idx_lo: bmc wcid 0 */
	req[4] = 1;		/* is_tlv_append */
	req[5] = 0;		/* muar_idx: AP bss has no wcid */

	if (enable) {
		memset(tlv, 0, sizeof(tlv));
		tlv[0] = 0;		/* hw_bss_idx */
		tlv[1] = 0;		/* omac_idx */
		tlv[2] = band;		/* band_idx */
		put_unaligned_le32(ZXNPU_CONN_INFRA_AP, tlv + 4);
		off = zxnpu_tlv_put(req, off, sizeof(req), ZXNPU_BSS_INFO_OMAC,
				    tlv, 12);
		if (off < 0)
			return off;
		n++;
	}

	memset(tlv, 0, sizeof(tlv));
	put_unaligned_le32(ZXNPU_NETWORK_INFRA, tlv + 0);
	tlv[4] = enable ? 1 : 0;		/* active */
	put_unaligned_le16(100, tlv + 6);	/* bcn_interval */
	memcpy(tlv + 8, w->mac, 6);		/* bssid */
	tlv[14] = 0;				/* wmm_idx: AP = 0 */
	tlv[15] = 1;				/* dtim_period */
	tlv[16] = 0;				/* bmc_wcid_lo */
	tlv[17] = 0;				/* cipher: none */
	tlv[18] = band ? 0x06 : ZXNPU_PHY_MODE_HT24;	/* OFDM|HT vs CCK|OFDM|HT */
	tlv[19] = 0;				/* max_bssid: MBSSID off */
	tlv[20] = 0;				/* non_tx_bssid */
	tlv[21] = 0;				/* bmc_wcid_hi */
	off = zxnpu_tlv_put(req, off, sizeof(req), ZXNPU_BSS_INFO_BASIC,
			    tlv, 24);
	if (off < 0)
		return off;
	n++;

	if (enable) {
		/* RF_CH: pri/center/bw, he_all_disable since no HE */
		memset(tlv, 0, sizeof(tlv));
		tlv[0] = bss->channel & 0xff;
		tlv[1] = bss->channel & 0xff;
		tlv[2] = 0;
		tlv[3] = ZXNPU_CHAN_BW_20;
		tlv[5] = 1;			/* he_all_disable */
		off = zxnpu_tlv_put(req, off, sizeof(req), ZXNPU_BSS_INFO_RF_CH,
				    tlv, 8);
		if (off < 0)
			return off;
		n++;

		/* BMC_RATE: 2 GHz = short preamble; 5 GHz = the two
		 * translation words mt7915_mcu_bss_bmc_tlv uses. */
		memset(tlv, 0, sizeof(tlv));
		if (band) {
			put_unaligned_le16(0x2000, tlv + 0);
			put_unaligned_le16(0x2080, tlv + 2);
		} else {
			tlv[4] = 1;		/* short_preamble */
		}
		off = zxnpu_tlv_put(req, off, sizeof(req),
				    ZXNPU_BSS_INFO_BMC_RATE, tlv, 12);
		if (off < 0)
			return off;
		n++;
	}

	put_unaligned_le16(n, req + 2);	/* tlv_num */
	ret = zxnpu_wlan_ext_set(w, ZXNPU_MCU_EXT_BSS_INFO, req, off);
	if (!ret && zxnpu_wlan_commit)
		bss->active = enable;
	return ret;
}

/*
 * fw-up <band> <ch>: the four steps in the order mt7915_mac_add_interface()
 * and mt7915_bss_info_changed() apply them. Requires the full download
 * (fw-state 7) and the eeprom upload to have happened in this boot.
 */
static void zxnpu_wlan_fw_up(struct zxnpu_wlan *w, const char *args)
{
	unsigned int band = 0, ch = 0;
	int ret;

	if (sscanf(args, "%u %u", &band, &ch) < 2 || band > 1) {
		zxnpu_wlan_append(w, "usage: radio-up <band 0|1> <channel>\n");
		return;
	}
	zxnpu_wlan_append(w, "radio-up: band %u ch %u chains %u mask %#x (%s)\n",
			  band, ch, zxnpu_band_chains(w, band),
			  zxnpu_band_mask(w, band),
			  zxnpu_wlan_commit ? "COMMIT" : "dry run");
	zxnpu_wlan_append(w, "  mac %pM (eeprom; mac-set overrides)\n", w->mac);

	ret = zxnpu_wlan_fw_mac(w, band, true);
	zxnpu_wlan_append(w, "  mac-init-ctrl: ret=%d\n", ret);
	if (ret)
		return;

	ret = zxnpu_wlan_fw_dev_info(w, band, true);
	zxnpu_wlan_append(w, "  dev-info: ret=%d\n", ret);
	if (ret)
		return;

	ret = zxnpu_wlan_fw_chan(w, band, ch);
	zxnpu_wlan_append(w, "  channel-switch ch=%u: ret=%d\n", ch, ret);
	if (ret)
		return;
	if (zxnpu_wlan_commit) {
		zxna_bss[band].band = band;
		zxna_bss[band].channel = ch;
	}

	ret = zxnpu_wlan_fw_bss(w, band, true);
	zxnpu_wlan_append(w, "  bss-info-update: ret=%d\n", ret);
	if (ret)
		return;
	zxnpu_wlan_append(w, "  band %u up: ch=%u bssid %pM tx-path %u\n",
			  band, ch, w->mac, zxnpu_band_chains(w, band));
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
	} else if (!strncmp(cmd, "cr ", 3)) {
		/* commit-gated single CR read: "cr <addr-hex>" - the L1 window
		 * (HIF_REMAP_L1 = addr >> 16, value at 0xE0000 + low16) reaches
		 * any connac bus segment, incl. conn_infra 0x41143c. */
		u32 a;

		if (sscanf(cmd + 3, "%x", &a) == 1)
			zxnpu_wlan_append(w, "cr [%08x] = %08x\n", a,
					  zxnpu_cr_read(w, a));
		else
			zxnpu_wlan_append(w, "usage: cr <addr-hex>\n");
	} else if (!strncmp(cmd, "scan", 4) &&
		   (cmd[4] == '\0' || cmd[4] == '\n')) {
		zxnpu_wlan_bus_scan(w);
	} else if (!strncmp(cmd, "mcu-ring ", 9)) {
		/* mcu-ring <0|1|4>: pick the ring commands ride. mt76 moves
		 * from qid17 (MCU_WM) to qid20 (MCU_WA) the instant
		 * MT76_STATE_MCU_RUNNING is set, i.e. right after load_ram
		 * ends. Selectable at runtime so one flash can A/B all
		 * three banks instead of one hypothesis per build. */
		int ring = -1;

		if (sscanf(cmd + 9, "%d", &ring) == 1 &&
		    (ring == ZXNPU_TX_RING_FWDL || ring == ZXNPU_TX_RING_WM ||
		     ring == ZXNPU_TX_RING_WA_CMD)) {
			zxnpu_wlan_mcu_ring = ring;
			zxnpu_wlan_queue_init(w, ring);
			zxnpu_wlan_append(w, "mcu-ring: commands now ride ring %d (qid%d @ %08x), armed=%d cidx=%u didx=%u\n",
					  ring, zxnpu_tx_ring_qid(ring),
					  ZXNPU_WFDMA_TX_RING0 +
					  ZXNPU_WFDMA_RING_STRIDE * ring,
					  zxnpu_cr_read(w,
						ZXNPU_WFDMA_TX_RING0 +
						ZXNPU_WFDMA_RING_STRIDE * ring) != 0,
					  zxnpu_cr_read(w,
						ZXNPU_WFDMA_TX_RING0 +
						ZXNPU_WFDMA_RING_STRIDE * ring + 8),
					  zxnpu_cr_read(w,
						ZXNPU_WFDMA_TX_RING0 +
						ZXNPU_WFDMA_RING_STRIDE * ring + 12));
		} else {
			zxnpu_wlan_append(w, "usage: mcu-ring 0|1|4 (qid16 FWDL / qid17 MCU_WM / qid20 MCU_WA)\n");
		}
	} else if (!strncmp(cmd, "eeprom-upload", 13)) {
		zxnpu_wlan_eeprom_upload(w);
	} else if (!strncmp(cmd, "eeprom-init", 11)) {
		zxnpu_wlan_eeprom_load(w);
	} else if (!strncmp(cmd, "eeprom-show", 11)) {
		zxnpu_wlan_eeprom_show(w, cmd + 11);
	} else if (!strncmp(cmd, "efuse-dump", 10)) {
		zxnpu_wlan_efuse_dump(w, cmd + 10, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-tri ", 8)) {
		zxnpu_wlan_mcu_frame(w, cmd + 8, zxnpu_wlan_commit, true);
	} else if (!strncmp(cmd, "mcu-andes ", 10)) {
		zxnpu_wlan_mcu_frame(w, cmd + 10, zxnpu_wlan_commit, false);
	} else if (!strncmp(cmd, "mcu-ext-wa ", 11)) {
		/* same command targeted at WA: s2d_index = H2C (2), the
		 * __MCU_CMD_FIELD_WA form mt76 uses for MCU_WA_EXT_CMD */
		zxnpu_wlan_mcu_ext(w, cmd + 11, zxnpu_wlan_commit, 2);
	} else if (!strncmp(cmd, "mcu-ext ", 8)) {
		zxnpu_wlan_mcu_ext(w, cmd + 8, zxnpu_wlan_commit, 0);
	} else if (!strncmp(cmd, "mcu-cmd ", 8)) {
		zxnpu_wlan_mcu_cmd(w, cmd + 8, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-finish", 10)) {
		zxnpu_wlan_mcu_finish(w, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl-regions ", 15)) {
		zxnpu_wlan_mcu_regions(w, cmd + 15, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-dl ", 7)) {
		zxnpu_wlan_mcu_dl(w, cmd + 7, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "rx-desc", 7)) {
		zxnpu_wlan_rx_desc_dump(w);
	} else if (!strncmp(cmd, "rx-raw", 6)) {
		int ring = 0, first = 0, count = 4;

		sscanf(cmd + 6, "%d %d %d", &ring, &first, &count);
		if (ring < 0 || ring > 1)
			ring = 0;
		if (first < 0 || first >= ZXNPU_MCU_RX_DEPTH)
			first = 0;
		if (count < 1 || count > ZXNPU_MCU_RX_DEPTH)
			count = 4;
		zxnpu_wlan_rx_raw(w, ring, first, count);
	} else if (!strncmp(cmd, "rx-init", 7)) {
		int ring = 0;

		sscanf(cmd + 7, "%d", &ring);
		if (ring < 0 || ring > 3)
			ring = 0;
		zxnpu_wlan_rx_init(w, ring, zxnpu_wlan_commit);
	} else if (!strncmp(cmd, "mcu-recv", 8)) {
		int ring = 0;

		sscanf(cmd + 8, "%d", &ring);
		if (ring < 0 || ring > 3)
			ring = 0;
		zxnpu_wlan_mcu_recv(w, ring);
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
		zxna_bss[band].channel = ch;
		strscpy(zxna_bss[band].ssid, ssid, sizeof(zxna_bss[band].ssid));
		zxnpu_wlan_append(w, "bss-set: band %u staged ch=%u ssid='%s'%s\n",
				  band, ch, ssid,
				  zxnpu_wlan_commit ?
				  " (beacon programming in P5)" :
				  " (dry run)");
done_bss:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "mac-set ", 8)) {
		/* mac-set <12 hex>: override the MAC taken from the eeprom
		 * template (00:0c:43:26:60:00). The unit MAC lives in the
		 * MT7916's own efuse, which this host path cannot read. */
		u8 mac[6];

		if (zxvap_parse_mac(cmd + 8, mac)) {
			zxnpu_wlan_append(w, "usage: mac-set <12 hex digits>\n");
		} else {
			memcpy(w->mac, mac, 6);
			zxnpu_wlan_append(w, "mac-set: %pM\n", w->mac);
		}
	} else if (!strncmp(cmd, "radio-mac ", 10)) {
		unsigned int band = 0, en = 0;

		if (sscanf(cmd + 10, "%u %u", &band, &en) < 2 || band > 1) {
			zxnpu_wlan_append(w, "usage: radio-mac <band 0|1> <0|1>\n");
			goto done_fw;
		}
		zxnpu_wlan_append(w, "radio-mac band %u enable %u: ret=%d\n",
				  band, en,
				  zxnpu_wlan_fw_mac(w, band, !!en));
done_fw:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "radio-chan ", 11)) {
		unsigned int band = 0, ch = 0;
		int ret;

		if (sscanf(cmd + 11, "%u %u", &band, &ch) < 2 || band > 1) {
			zxnpu_wlan_append(w, "usage: radio-chan <band 0|1> <channel>\n");
			goto done_chan;
		}
		ret = zxnpu_wlan_fw_chan(w, band, ch);
		if (!ret && zxnpu_wlan_commit) {
			zxna_bss[band].band = band;
			zxna_bss[band].channel = ch;
		}
		zxnpu_wlan_append(w, "radio-chan band %u ch %u chains %u mask %#x: ret=%d\n",
				  band, ch, zxnpu_band_chains(w, band),
				  zxnpu_band_mask(w, band), ret);
done_chan:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "radio-bss ", 10)) {
		unsigned int band = 0, en = 0;
		int ret;

		if (sscanf(cmd + 10, "%u %u", &band, &en) < 2 || band > 1) {
			zxnpu_wlan_append(w, "usage: radio-bss <band 0|1> <0|1>\n");
			goto done_fwbss;
		}
		ret = zxnpu_wlan_fw_bss(w, band, !!en);
		zxnpu_wlan_append(w, "radio-bss band %u enable %u (ch %u): ret=%d\n",
				  band, en, zxna_bss[band].channel, ret);
done_fwbss:
		zxnpu_wlan_append(w, "\n");
	} else if (!strncmp(cmd, "radio-up ", 9)) {
		zxnpu_wlan_fw_up(w, cmd + 9);
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
		zxnpu_wlan_append(w, "commands: l1profile | ack-expect | data-init | data-recv | dataplane | probe | scan | win <bar> <off> [n] | crw <a> <v> | fw | plan | mcu | rings | mcuwin | mcu-send | mcu-cmd | mcu-dl | mcu-dl-regions | fw-start | fw-info | fw-audit | fw-state | fw-plan wm|wa | fw-dl wm|wa | rx-init | mcu-recv | mac-set | radio-mac | radio-chan | radio-bss | radio-up | driver-own | wfsys-reset | dma-init | commit\n");
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
		w->wac_ring = dmam_alloc_coherent(&pdev->dev,
						  ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_DESC_SIZE,
						  &w->wac_ring_dma, GFP_KERNEL);
		w->wac_bufs = dmam_alloc_coherent(&pdev->dev,
						  ZXNPU_MCU_RING_DEPTH * ZXNPU_MCU_SLOT,
						  &w->wac_bufs_dma, GFP_KERNEL);
		w->rx_ring = dmam_alloc_coherent(&pdev->dev,
						 ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_DESC_SIZE,
						 &w->rx_ring_dma, GFP_KERNEL);
		w->rx_buf = dmam_alloc_coherent(&pdev->dev,
						ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_RX_BUF_SIZE,
						&w->rx_buf_dma, GFP_KERNEL);
		w->wa_ring = dmam_alloc_coherent(&pdev->dev,
						 ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_DESC_SIZE,
						 &w->wa_ring_dma, GFP_KERNEL);
		w->wa_buf = dmam_alloc_coherent(&pdev->dev,
						ZXNPU_MCU_RX_DEPTH * ZXNPU_MCU_RX_BUF_SIZE,
						&w->wa_buf_dma, GFP_KERNEL);
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
		    !w->wac_ring || !w->wac_bufs ||
		    !w->rx_ring || !w->rx_buf || !w->wa_ring || !w->wa_buf ||
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
	kfree(w->eeprom);
	w->eeprom = NULL;
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
