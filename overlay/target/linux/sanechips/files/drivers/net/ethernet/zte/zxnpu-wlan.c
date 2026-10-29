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
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
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
	} else if (!strncmp(cmd, "commit", 6)) {
		commit = zxnpu_wlan_commit;
		if (!commit)
			zxnpu_wlan_append(w, "commit refused: set zxnpu_wlan.commit=1\n");
		zxnpu_wlan_wfdma(w, commit);
	} else {
		zxnpu_wlan_append(w, "commands: probe | fw | plan | commit\n");
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
	mutex_init(&w->lock);
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
