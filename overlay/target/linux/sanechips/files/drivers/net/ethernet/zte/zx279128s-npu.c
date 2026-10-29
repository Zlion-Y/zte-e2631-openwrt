// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S npu scaffold.
 *
 * This is the first checkpoint of the npu branch: it proves the build and
 * packaging path for the vendor-native stack without touching any register.
 * The wired (gmac/switch/PPE/FFE/IDM/TM) and MT7916 CSP_WLAN drivers replace
 * the existing data path in later stages; until then this driver only exposes
 * a status file so an image can be told apart from the v1.8.0 baseline.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define ZX_NPU_VERSION "npu-0.1-scaffold"

static struct platform_device *zx_npu_pdev;

static int zx_npu_status_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "version: %s\n", ZX_NPU_VERSION);
	seq_puts(s, "state: scaffold; no MMIO access; data path unchanged\n");
	if (zx_npu_pdev && zx_npu_pdev->dev.of_node)
		seq_printf(s, "of_node: %pOF\n", zx_npu_pdev->dev.of_node);
	seq_puts(s, "planned: gmac/switch/ppe/ffe/idm/tm/bmu; mt7916 csp_wlan\n");
	return 0;
}

static int zx_npu_probe(struct platform_device *pdev)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("zx-npu", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create_single("status", 0444, dir, zx_npu_status_show)) {
		proc_remove(dir);
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, dir);
	zx_npu_pdev = pdev;
	dev_info(&pdev->dev, "%s ready (no MMIO access)\n", ZX_NPU_VERSION);
	return 0;
}

static void zx_npu_remove(struct platform_device *pdev)
{
	struct proc_dir_entry *dir = platform_get_drvdata(pdev);

	proc_remove(dir);
	if (zx_npu_pdev == pdev)
		zx_npu_pdev = NULL;
}

static const struct of_device_id zx_npu_of_match[] = {
	{ .compatible = "zte,zx279128s-npu-scaffold" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_npu_of_match);

static struct platform_driver zx_npu_driver = {
	.probe = zx_npu_probe,
	.remove = zx_npu_remove,
	.driver = {
		.name = "zx279128s-npu-scaffold",
		.of_match_table = zx_npu_of_match,
	},
};
module_platform_driver(zx_npu_driver);

MODULE_DESCRIPTION("ZTE ZX279128S npu scaffold (no MMIO access)");
MODULE_LICENSE("GPL");
