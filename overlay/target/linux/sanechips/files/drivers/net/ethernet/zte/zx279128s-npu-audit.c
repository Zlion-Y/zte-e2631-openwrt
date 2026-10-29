// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S npu read-only register audit.
 *
 * P1 register-diff evidence is only useful if it can be checked against the
 * live chip. This driver intentionally does not write any register: it maps the
 * four accelerator windows and reads a small documented set when userspace
 * writes "run" to /proc/zx-npu-audit/audit. The mapping uses ioremap(), not
 * request_mem_region(), because the existing gmac driver owns those windows.
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define ZX_NPU_NPP_BASE		0x921C0000
#define ZX_NPU_IDM_BASE		0x921C8000
#define ZX_NPU_TM_BASE		0x92340000
#define ZX_NPU_PP_BASE		0x92380000
#define ZX_NPU_NPP_SIZE		0x8000
#define ZX_NPU_IDM_SIZE		0x4000
#define ZX_NPU_TM_SIZE		0x40000
#define ZX_NPU_PP_SIZE		0x40000
#define ZX_NPU_BMU_OFF		0x8000
#define ZX_NPU_CLA_OFF		0xC000

static char zx_npu_audit_buf[8192];
static DEFINE_MUTEX(zx_npu_audit_lock);

static void zx_npu_audit_add(const char *fmt, ...)
{
	va_list args;
	size_t len = strlen(zx_npu_audit_buf);

	if (len >= sizeof(zx_npu_audit_buf) - 1)
		return;
	va_start(args, fmt);
	vsnprintf(zx_npu_audit_buf + len, sizeof(zx_npu_audit_buf) - len,
		  fmt, args);
	va_end(args);
}

static void __iomem *zx_npu_map(phys_addr_t phys, size_t size)
{
	void __iomem *p = ioremap(phys, size);

	if (!p)
		zx_npu_audit_add("map 0x%08llx+0x%zx failed\n",
				 (unsigned long long)phys, size);
	return p;
}

static void zx_npu_audit_run(void)
{
	void __iomem *npp, *idm, *tm, *pp;

	memset(zx_npu_audit_buf, 0, sizeof(zx_npu_audit_buf));
	zx_npu_audit_add("npu-0.2 read-only audit (no register writes)\n");
	zx_npu_audit_add("warning: reads are opt-in; a wedged read needs a power cycle\n");

	npp = zx_npu_map(ZX_NPU_NPP_BASE, ZX_NPU_NPP_SIZE);
	idm = zx_npu_map(ZX_NPU_IDM_BASE, ZX_NPU_IDM_SIZE);
	tm = zx_npu_map(ZX_NPU_TM_BASE, ZX_NPU_TM_SIZE);
	pp = zx_npu_map(ZX_NPU_PP_BASE, ZX_NPU_PP_SIZE);

	if (npp)
		zx_npu_audit_add("npp: 00=%08x 04=%08x 48=%08x 4c=%08x\n",
				 readl(npp + 0x00), readl(npp + 0x04),
				 readl(npp + 0x48), readl(npp + 0x4c));
	if (idm)
		zx_npu_audit_add("idm: 00=%08x 10=%08x 18=%08x 1c=%08x 24=%08x\n",
				 readl(idm + 0x00), readl(idm + 0x10),
				 readl(idm + 0x18), readl(idm + 0x1c),
				 readl(idm + 0x24));
	if (idm)
		zx_npu_audit_add("idm: 40=%08x 44=%08x 48=%08x 4c=%08x 80=%08x\n",
				 readl(idm + 0x40), readl(idm + 0x44),
				 readl(idm + 0x48), readl(idm + 0x4c),
				 readl(idm + 0x80));
	if (tm)
		zx_npu_audit_add("tm/bmu: 04=%08x 08=%08x 58=%08x 5c=%08x\n",
				 readl(tm + ZX_NPU_BMU_OFF + 0x04),
				 readl(tm + ZX_NPU_BMU_OFF + 0x08),
				 readl(tm + ZX_NPU_BMU_OFF + 0x58),
				 readl(tm + ZX_NPU_BMU_OFF + 0x5c));
	if (tm)
		zx_npu_audit_add("tm/bmu: e8=%08x ec=%08x f4=%08x f8=%08x fc=%08x\n",
				 readl(tm + ZX_NPU_BMU_OFF + 0xe8),
				 readl(tm + ZX_NPU_BMU_OFF + 0xec),
				 readl(tm + ZX_NPU_BMU_OFF + 0xf4),
				 readl(tm + ZX_NPU_BMU_OFF + 0xf8),
				 readl(tm + ZX_NPU_BMU_OFF + 0xfc));
	if (pp)
		zx_npu_audit_add("pp/cla: c080=%08x c088=%08x c090=%08x\n",
				 readl(pp + ZX_NPU_CLA_OFF + 0x80),
				 readl(pp + ZX_NPU_CLA_OFF + 0x88),
				 readl(pp + ZX_NPU_CLA_OFF + 0x90));

	if (npp)
		iounmap(npp);
	if (idm)
		iounmap(idm);
	if (tm)
		iounmap(tm);
	if (pp)
		iounmap(pp);
}

static ssize_t zx_npu_audit_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&zx_npu_audit_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos,
				      zx_npu_audit_buf,
				      strlen(zx_npu_audit_buf));
	mutex_unlock(&zx_npu_audit_lock);
	return ret;
}

static ssize_t zx_npu_audit_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char cmd[8];

	if (count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';

	if (!strncmp(cmd, "run", 3)) {
		mutex_lock(&zx_npu_audit_lock);
		zx_npu_audit_run();
		mutex_unlock(&zx_npu_audit_lock);
		return count;
	}
	return -EINVAL;
}

static const struct proc_ops zx_npu_audit_ops = {
	.proc_read = zx_npu_audit_read,
	.proc_write = zx_npu_audit_write,
	.proc_lseek = default_llseek,
};

static int zx_npu_audit_probe(struct platform_device *pdev)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("zx-npu-audit", NULL);
	if (!dir)
		return -ENOMEM;
	if (!proc_create("audit", 0644, dir, &zx_npu_audit_ops)) {
		proc_remove(dir);
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, dir);
	dev_info(&pdev->dev, "read-only audit ready (write run to /proc/zx-npu-audit/audit)\n");
	return 0;
}

static void zx_npu_audit_remove(struct platform_device *pdev)
{
	proc_remove(platform_get_drvdata(pdev));
}

static const struct of_device_id zx_npu_audit_of_match[] = {
	{ .compatible = "zte,zx279128s-npu-audit" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_npu_audit_of_match);

static struct platform_driver zx_npu_audit_driver = {
	.probe = zx_npu_audit_probe,
	.remove = zx_npu_audit_remove,
	.driver = {
		.name = "zx279128s-npu-audit",
		.of_match_table = zx_npu_audit_of_match,
	},
};
module_platform_driver(zx_npu_audit_driver);

MODULE_DESCRIPTION("ZTE ZX279128S npu read-only register audit");
MODULE_LICENSE("GPL");
