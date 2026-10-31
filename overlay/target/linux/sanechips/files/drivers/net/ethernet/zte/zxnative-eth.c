// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S native wired data plane (P2).
 *
 * A standalone vendor-behaviour rewrite of the switch/GMAC/DMA layer: the
 * register sequences come from the stock firmware and from the reverse
 * engineering recorded in this repository, not from any other Linux driver.
 * It binds the same "zte,zx279128s-gmac" node, so as long as the baseline
 * zx279128s-eth driver is built in this module stays inert; it is the
 * development vehicle for replacing that baseline.
 *
 * Gated by zxnative_eth.enable=0: with the parameter at its default the
 * probe returns -ENODEV before touching any register.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <net/flow_offload.h>

#include "zx279128s-eth.h"
#include "zx-ffe-ops.h"

/* Default on: the driver links ahead of the baseline, so with enable=y it
 * wins the boot probe race and the baseline (which suppresses bind attrs
 * and can never be rebound at runtime) stays as the boot-time fallback if
 * this probe declines. enable=0 at boot hands the node to the baseline. */
static bool zxnative_enable = true;
module_param_named(enable, zxnative_enable, bool, 0644);
MODULE_PARM_DESC(enable, "probe the switch/GMAC with the native rewrite (boot-time race winner; baseline serves if declined)");

/* ---- BMU ------------------------------------------------------------ */

static int zxna_bmu_alloc(struct zx_eth_adapter *a)
{
	void __iomem *base = a->base;
	u32 val;
	int ret;

	if (a->bmu_alloc_pending) {
		if (readl(base + ZX_BMU_BUF_ALLOC) & ZX_BMU_BUF_ALLOC_REQ) {
			a->bmu_alloc_fail++;
			return -EBUSY;
		}
		a->bmu_alloc_pending = false;
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID) {
			a->bmu_alloc_late++;
			return FIELD_GET(ZX_BMU_BUF_ID_BP, val);
		}
	}

	val = readl(base + ZX_BMU_BUF_ALLOC);
	writel(val | ZX_BMU_BUF_ALLOC_REQ, base + ZX_BMU_BUF_ALLOC);
	ret = readl_poll_timeout_atomic(base + ZX_BMU_BUF_ALLOC, val,
					!(val & ZX_BMU_BUF_ALLOC_BUSY), 1, 1000);
	if (!ret) {
		val = readl(base + ZX_BMU_BUF_ID);
		if (val & ZX_BMU_BUF_ID_VALID)
			return FIELD_GET(ZX_BMU_BUF_ID_BP, val);
		ret = -ENOSPC;
	}

	a->bmu_alloc_pending = true;
	a->bmu_alloc_fail++;
	return ret;
}

static int zxna_bmu_free(struct zx_eth_adapter *a, u16 bp)
{
	u32 val;
	int ret = 0;

	spin_lock_bh(&a->bmu_free_lock);
	if (!a->bmu_free_slots) {
		ret = readl_poll_timeout_atomic(a->base + ZX_BMU_FREE_STATUS,
						val, val & ZX_BMU_FREE_SLOTS,
						1, 5000);
		if (ret) {
			a->bmu_free_fail++;
			goto out;
		}
		a->bmu_free_slots = FIELD_GET(ZX_BMU_FREE_SLOTS, val);
	}
	a->bmu_free_slots--;
	writel(bp, a->base + ZX_BMU_BUF_FREE);
out:
	spin_unlock_bh(&a->bmu_free_lock);
	return ret;
}

/* ---- RX release ----------------------------------------------------- */

static int zxna_rx_release(struct zx_eth_adapter *a, int q, int class, u32 n)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout_atomic(a->base + ZX_RX_REL_CTRL, val,
					!(val & ZX_RX_REL_BUSY), 1, 5000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_RX_REL_COUNT, n) |
	       FIELD_PREP(ZX_RX_REL_CLASS, class) |
	       FIELD_PREP(ZX_RX_REL_QUEUE, q), a->base + ZX_RX_REL_DATA);
	writel(ZX_RX_REL_BUSY, a->base + ZX_RX_REL_CTRL);
	return 0;
}

static void zxna_rx_consume(__le32 *desc, u32 rel[2])
{
	rel[((u8 *)desc)[6] & 1]++;
	desc[0] = 0;
	desc[1] = 0;
	desc[2] = 0;
	desc[3] = 0;
}

/* ---- MAC helpers ---------------------------------------------------- */

static void zxna_port_speed(struct zx_eth_adapter *a, int port, int speed,
			    int duplex)
{
	void __iomem *reg = a->base + ZX_MAC_PORT(port) + ZX_MAC_CTRL;
	u32 val = readl(reg);

	if (speed == SPEED_1000) {
		val &= ~(ZX_MAC_CTRL_MII | ZX_MAC_CTRL_100);
		val |= ZX_MAC_CTRL_FD;
	} else {
		if (duplex == DUPLEX_FULL) {
			val |= ZX_MAC_CTRL_MII | ZX_MAC_CTRL_FD;
		} else {
			val &= ~ZX_MAC_CTRL_FD;
			val |= ZX_MAC_CTRL_MII;
		}
		if (speed == SPEED_100)
			val |= ZX_MAC_CTRL_100;
		else
			val &= ~ZX_MAC_CTRL_100;
	}
	writel(val, reg);
}

static void zxna_port_enable(struct zx_eth_adapter *a, int port, bool enable)
{
	void __iomem *reg = a->base + ZX_MAC_PORT(port) + ZX_MAC_CTRL;
	u32 val = readl(reg);

	if (enable)
		val |= ZX_MAC_CTRL_EN;
	else
		val &= ~ZX_MAC_CTRL_EN;
	writel(val, reg);
}

/* ---- MAC routing / trap tables -------------------------------------- */

/* MAC in the layout of the switch MAC registers: bytes 2-5, then 0-1 */
static void zxna_mac_words(const u8 *mac, u32 w[2])
{
	w[0] = (u32)mac[2] << 24 | (u32)mac[3] << 16 |
	       (u32)mac[4] << 8 | mac[5];
	w[1] = (u32)mac[0] << 8 | mac[1];
}

/*
 * Frames addressed to one of our MACs go to the CPU whatever their type.
 * With flow offload active the stock driver moves these addresses to the
 * ONU routed table instead; this driver has no offload yet, so the trap
 * table is the path (zx_update_mac_tables in the baseline driver).
 */
static void zxna_set_trap_dmac(struct zx_eth_adapter *a, int idx, const u8 *mac)
{
	u32 w[2];

	zxna_mac_words(mac, w);
	writel(w[0], a->base + ZX_SPA_TRAP_DMAC + idx * 8);
	writel(w[1], a->base + ZX_SPA_TRAP_DMAC + idx * 8 + 4);
}

static void zxna_update_mac_tables(struct zx_eth_adapter *a)
{
	static const u8 none[ETH_ALEN];
	const u8 *addrs[ZX_NUM_PORTS];
	int i, j, n = 0;

	mutex_lock(&a->ppe_lock);
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct net_device *dev = a->ports[i];

		if (!dev)
			continue;
		for (j = 0; j < n; j++)
			if (ether_addr_equal(addrs[j], dev->dev_addr))
				break;
		if (j == n)
			addrs[n++] = dev->dev_addr;
	}
	for (i = 0; i < ZX_SPA_ONU_MAC_NUM; i++) {
		u32 w[2];

		zxna_mac_words(none, w);
		writel(w[0], a->base + ZX_SPA_ONU_MAC + i * 8);
		writel(w[1], a->base + ZX_SPA_ONU_MAC + i * 8 + 4);
	}
	for (i = 0; i < ZX_SPA_TRAP_DMAC_NUM; i++)
		zxna_set_trap_dmac(a, i, i < n ? addrs[i] : none);
	mutex_unlock(&a->ppe_lock);
}

/* ---- phylink -------------------------------------------------------- */

static struct zx_eth_priv *zxna_pl_priv(struct phylink_config *config)
{
	return netdev_priv(to_net_dev(config->dev));
}

static void zxna_mac_config(struct phylink_config *config, unsigned int mode,
			    const struct phylink_link_state *state)
{
}

static void zxna_mac_link_down(struct phylink_config *config,
			       unsigned int mode, phy_interface_t interface)
{
	struct zx_eth_priv *priv = zxna_pl_priv(config);

	zxna_port_enable(priv->adapter, priv->port, false);
}

static void zxna_mac_link_up(struct phylink_config *config,
			     struct phy_device *phy, unsigned int mode,
			     phy_interface_t interface, int speed, int duplex,
			     bool tx_pause, bool rx_pause)
{
	struct zx_eth_priv *priv = zxna_pl_priv(config);

	if (speed > 0)
		zxna_port_speed(priv->adapter, priv->port, speed, duplex);
	zxna_port_enable(priv->adapter, priv->port, true);
}

static const struct phylink_mac_ops zxna_mac_ops = {
	.mac_config	= zxna_mac_config,
	.mac_link_down	= zxna_mac_link_down,
	.mac_link_up	= zxna_mac_link_up,
};

/* ---- indirect table access ----------------------------------------- */

static int zxna_tm_write(struct zx_eth_adapter *a, u32 table, u32 index,
			 const u32 data[4])
{
	u32 val;
	int i, ret;

	ret = readl_poll_timeout(a->base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_TM_IND_INDEX, index) |
	       FIELD_PREP(ZX_TM_IND_TABLE, table), a->base + ZX_TM_IND_CMD);
	for (i = 3; i >= 0; i--)
		writel(data[i], a->base + ZX_TM_IND_DATA(i));
	return 0;
}

static int zxna_tm_read0(struct zx_eth_adapter *a, u32 table, u32 index,
			 u32 *data)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(a->base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	writel(FIELD_PREP(ZX_TM_IND_INDEX, index) |
	       FIELD_PREP(ZX_TM_IND_TABLE, table) | ZX_TM_IND_READ,
	       a->base + ZX_TM_IND_CMD);
	ret = readl_poll_timeout(a->base + ZX_TM_IND_STATUS, val,
				 val & ZX_TM_IND_READY, 1, 1000);
	if (ret)
		return ret;
	*data = readl(a->base + ZX_TM_IND_DATA(0));
	return 0;
}

static int zxna_tm_init(struct zx_eth_adapter *a)
{
	static const u32 queue[4] = { 0x00800400, 0, 0, 0 };
	static const u32 red[4] = { 0x00200020, 0, 0, 0 };
	static const u32 sched[4] = { 0xff803fff, 0x0100ff80, 0x00100200, 32 };
	u32 cpu_queue[4] = { 0 };
	u32 used, limit;
	int i, ret;

	for (i = 0; i < 16; i++) {
		ret = zxna_tm_read0(a, ZX_TM_TABLE_QUEUE_USE, i, &used);
		if (ret)
			return ret;
		used = (used & 0x7ff) + ((used >> 11) & 0x7fff);
		limit = 1023 + used;
		cpu_queue[0] = min(limit, 0x7ffU) |
			       (limit - min(limit, 0x7ffU)) << 11;
		ret = zxna_tm_write(a, ZX_TM_TABLE_QUEUE, i, cpu_queue);
		if (ret)
			return ret;
	}
	for (i = 16; i < 400; i++) {
		ret = zxna_tm_write(a, ZX_TM_TABLE_QUEUE, i, queue);
		if (ret)
			return ret;
	}
	for (i = 0; i < 384; i++) {
		ret = zxna_tm_write(a, ZX_TM_TABLE_RED, i, red);
		if (ret)
			return ret;
		ret = zxna_tm_write(a, ZX_TM_TABLE_SCHED, i, sched);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- hardware initialisation ---------------------------------------- */

static void zxna_pp_init(struct zx_eth_adapter *a)
{
	void __iomem *base = a->base;

	writel(2, base + ZX_PP_RESET);
	usleep_range(1000, 2000);
	writel(0x000200ff, base + ZX_SBRG_PORT_CTRL);
	writel(0x0000dfdf, base + 0x388008);
	writel(0x00000000, base + ZX_SBRG_LEARN);
	writel(0xff5ffdff, base + 0x388340);
	writel(0x0000003e, base + 0x388344);
	writel(0x0000003f, base + 0x388380);
	writel(0xaaaaaaaa, base + 0x38863c);
	writel(0x00005555, base + 0x3881c4);
	writel(0x000bf874, base + 0x388188);
	writel(0x000000ff, base + 0x3882c0);
	writel(0x0000ffff, base + 0x388300);
	writel(0x0000003e, base + 0x388304);
	writel(0x0000309a, base + 0x38c080);
	writel(0, base + 0x38c088);
	writel(1, base + 0x38c0cc);
	writel(0, base + 0x3a0010);
	writel(0, base + 0x3a0014);
}

static void zxna_dma_enable(struct zx_eth_adapter *a, bool enable)
{
	u32 val = readl(a->base + ZX_DMA_CTRL);

	val &= ~ZX_DMA_CTRL_EN;
	val |= ZX_DMA_CTRL_BIT21 | (enable ? ZX_DMA_CTRL_EN : 0);
	writel(val, a->base + ZX_DMA_CTRL);
	readl(a->base + ZX_DMA_CTRL);
}

static void zxna_hw_reset(struct zx_eth_adapter *a)
{
	int i;

	writel(0, a->base + ZX_GLB_RESET);
	fsleep(10);
	writel(0xffffffff, a->base + ZX_GLB_RESET);
	for (i = 0; i < ZX_NUM_PORTS; i++)
		zxna_port_enable(a, i, false);
	zxna_dma_enable(a, false);
	writel(0, a->base + ZX_BMU_CTRL);
}

static int zxna_hw_init(struct zx_eth_adapter *a)
{
	static const int port_to_sw_idx[8] = { 0, 1, 2, 3, 4, -1, 5, 6 };
	void __iomem *base = a->base;
	__be16 *tab;
	u32 val;
	int i, ret;

	zxna_hw_reset(a);
	writel(~0, base + ZX_TM_INT_MASK);
	msleep(20);

	writel(readl(base + ZX_GLB_CLK_CTRL) & ~ZX_GLB_CLK_CTRL_BITS,
	       base + ZX_GLB_CLK_CTRL);
	writel(GENMASK(ZX_NUM_PORTS - 1, 0), base + ZX_GLB_PORT_CLK_EN);
	zxna_pp_init(a);

	tab = a->tab0;
	for (i = 0; i < ZX_TAB0_ENTRIES; i++)
		tab[i] = cpu_to_be16(i);
	tab = a->tab1;
	for (i = 0; i < ZX_TAB1_ENTRIES; i++)
		tab[i] = cpu_to_be16(i);

	dma_wmb();
	ret = zxna_tm_init(a);
	if (ret)
		return dev_err_probe(a->dev, ret, "TM table setup timed out\n");

	writel((u32)a->tab0_dma, base + ZX_TM_TAB0_ADDR);
	writel((u32)a->tab1_dma, base + ZX_TM_TAB1_ADDR);
	writel(0x08000800, base + ZX_TM_TAB_LIMIT);
	writel((u32)a->buf_pool_dma, base + ZX_TM_BUF_START);
	writel((u32)a->buf_pool_dma + ZX_BUF_POOL_SIZE, base + ZX_TM_BUF_END);
	writel((u32)a->rx_ring_dma, base + ZX_TM_BUF_F0);
	writel(16, base + ZX_TM_CONFIG_START);

	writel(1, base + ZX_DMA_BASE + 0x28);
	writel(1, base + ZX_DMA_BASE + 0x2c);
	writel(ZX_RX_IRQ_USECS * 50, base + ZX_DMA_RX_IRQ_TIMER);
	writel(ZX_RX_IRQ_FRAMES, base + ZX_DMA_RX_IRQ_FRAMES);
	writel(0x7f, base + ZX_DMA_BASE + 0x04);
	writel(32, base + ZX_DMA_BASE + 0x20);
	writel(32, base + ZX_DMA_BASE + 0x24);
	writel(0x00131217, base + ZX_DMA_BASE + 0x388);
	writel(readl(base + ZX_TM_CTRL) | ZX_TM_CTRL_BIT6, base + ZX_TM_CTRL);

	writel(0, base + ZX_BMU_CTRL);
	writel(0x0104c040, base + ZX_BMU_CFG1);
	writel(0x0104c040, base + ZX_BMU_CFG2);
	writel(ZX_NUM_BUFFERS / 32 - 1, base + ZX_BMU_LIMIT1);
	writel(15, base + ZX_BMU_LIMIT2);
	writel(ZX_NUM_BUFFERS << 16, base + ZX_BMU_THRES1);
	writel(0x2000000, base + ZX_BMU_THRES2);
	writel(0, base + ZX_BMU_BUF_ALLOC);
	writel(ZX_BMU_CTRL_EN, base + ZX_BMU_CTRL);
	writel(17, base + 0x3a00e0);

	memset(a->tx_ring, 0, ZX_NUM_TX_DESC * ZX_DESC_SIZE);
	memset(a->rx_ring, 0, ZX_RX_QUEUES * ZX_RX_QUEUE_STRIDE);
	a->tx_cur_idx = 0;
	for (i = 0; i < ZX_RX_QUEUES; i++) {
		a->rx_cur_idx[i] = 0;
		a->rx_synced[i] = false;
	}
	a->tx_pending = 0;
	a->tx_complete_cnt = 0;
	a->tx_done_excess = 0;
	a->bmu_free_slots = 0;

	writel((u32)a->buf_pool_dma + ZX_AREA50_OFFSET, base + ZX_DMA_AREA50);
	writel((u32)a->tx_ring_dma, base + ZX_DMA_TX_RING);
	writel(0x00400040, base + ZX_DMA_RING_CFG);
	readl(base + ZX_DMA_TX_DONE);
	dma_wmb();

	writel(ZX_SW_FWD_CTRL_ON, base + ZX_SW_FWD_CTRL);
	for (i = 0; i < 8; i++)
		if (port_to_sw_idx[i] >= 0)
			writel(1, base + ZX_SPA_PORT_EN(port_to_sw_idx[i]));

	writel(0xffffffff, base + ZX_SPA_UP_REG_PKT_EN);
	writel(0xffffffff, base + ZX_SPA_UP_REG_PKT_EN + 4);
	writel(0xffffffff, base + ZX_SPA_DN_REG_PKT_EN);
	writel(0xffffffff, base + ZX_SPA_DN_REG_PKT_EN + 4);
	writel(ETH_P_IP << 16 | ETH_P_IPV6, base + ZX_SPA_TRAP_ETH_TYPE);
	writel(ETH_P_ARP << 16 | ETH_P_8021Q, base + ZX_SPA_TRAP_ETH_TYPE + 4);
	writel(0, base + ZX_SPA_TRAP_PROTO);

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		val = readl(base + ZX_SW_PORT_FILTER(i));
		val &= ~ZX_SW_PORT_FILTER_BITS;
		writel(val, base + ZX_SW_PORT_FILTER(i));
	}

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		void __iomem *pb = base + ZX_MAC_PORT(i);

		writel(0x00bbe000, pb + ZX_MAC_CTRL);
		writel(0x80000001, pb + 0x08);
		writel(0x0000fffe, pb + 0x04);
		writel(0x00011200, pb + 0xe0);
		writel(50, pb + 0xd00);
		writel(168, pb + 0xd30);
		writel(0x00300002, pb + 0x70);
		writel(0x4000, pb + 0xb4);
		writel(0x0010ff11, pb + 0xb00);
		zxna_port_speed(a, i, SPEED_1000, DUPLEX_FULL);
		zxna_port_enable(a, i, false);
	}
	return 0;
}

/* ---- TX ------------------------------------------------------------- */

static void zxna_tx_kick(struct zx_eth_adapter *a)
{
	if (!hrtimer_active(&a->tx_timer))
		hrtimer_start(&a->tx_timer, us_to_ktime(ZX_TX_POLL_US),
			      HRTIMER_MODE_REL);
}

static void zxna_tx_reclaim(struct zx_eth_adapter *a)
{
	u32 done = readl(a->base + ZX_DMA_TX_DONE) & 0xffff;
	u32 n = min(done, a->tx_pending);

	if (unlikely(done > a->tx_pending))
		a->tx_done_excess += done - a->tx_pending;
	a->tx_complete_cnt += n;
	a->tx_pending -= n;
	if (a->tx_pending < ZX_TX_QUEUE_LIMIT) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (a->ports[p] && netif_queue_stopped(a->ports[p]))
				netif_wake_queue(a->ports[p]);
	}
}

static netdev_tx_t zxna_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *a = priv->adapter;
	unsigned int len = skb->len;
	__le32 *desc;
	u32 tx_idx;
	u8 *buf;
	int bp;

	if (len > 1536) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_bh(&a->tx_lock);
	zxna_tx_reclaim(a);
	if (a->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		netif_stop_queue(netdev);
		zxna_tx_kick(a);
		spin_unlock_bh(&a->tx_lock);
		return NETDEV_TX_BUSY;
	}
	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_bh(&a->tx_lock);
		return NETDEV_TX_OK;
	}
	len = skb->len;
	bp = zxna_bmu_alloc(a);
	if (bp < 0 || bp >= ZX_NUM_BUFFERS) {
		spin_unlock_bh(&a->tx_lock);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	buf = (u8 *)a->buffer_pool + (u32)bp * ZX_BUF_SIZE + 16;
	if (skb_copy_bits(skb, 0, buf, len)) {
		zxna_bmu_free(a, bp);
		spin_unlock_bh(&a->tx_lock);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	tx_idx = a->tx_cur_idx % ZX_NUM_TX_DESC;
	desc = (__le32 *)((u8 *)a->tx_ring + tx_idx * ZX_DESC_SIZE);
	desc[0] = cpu_to_le32(0x80 | (ZX_PORT_TX_QUEUE(priv->port) << 20));
	desc[3] = cpu_to_le32(0x3 | ((len & 0x3fff) << 2));
	desc[1] = cpu_to_le32(0x10000);
	desc[2] = cpu_to_le32(0x21000000 | ((len & 0x3fff) << 9));
	((u8 *)desc)[7] = (u8)((bp & 0x7f) << 1);
	((u8 *)desc)[8] = (u8)(bp >> 7);

	dma_wmb();
	writel(1, a->base + ZX_DMA_TX_START);

	a->tx_cur_idx++;
	a->tx_pending++;
	if (a->tx_pending >= ZX_TX_QUEUE_LIMIT) {
		int p;

		for (p = 0; p < ZX_NUM_PORTS; p++)
			if (a->ports[p])
				netif_stop_queue(a->ports[p]);
		zxna_tx_kick(a);
	}
	spin_unlock_bh(&a->tx_lock);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- RX ------------------------------------------------------------- */

static bool zxna_own_addr(struct zx_eth_adapter *a, const u8 *addr)
{
	int i;

	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (a->ports[i] && ether_addr_equal(a->ports[i]->dev_addr, addr))
			return true;
	return false;
}

static int zxna_rx(struct zx_eth_adapter *a, int budget)
{
	int done = 0, q;

	for (q = ZX_RX_QUEUES - 1; q >= 0 && done < budget; q--) {
		u32 avail = readl(a->base + ZX_DMA_RX_QUEUE_CNT(q)) & 0xffff;
		u32 rel[2] = { 0, 0 };

		while (avail && done < budget) {
			u32 idx = a->rx_cur_idx[q] % ZX_NUM_RX_DESC;
			__le32 *desc = (__le32 *)((u8 *)a->rx_ring +
					q * ZX_RX_QUEUE_STRIDE +
					idx * ZX_DESC_SIZE);
			u16 bp, len;
			int port;
			struct sk_buff *skb;
			struct net_device *dev;

			dma_rmb();
			bp = (((u8 *)desc)[7] >> 1) |
			     ((u16)((u8 *)desc)[8] << 7);
			len = (le32_to_cpu(desc[3]) >> 2) & 0x3fff;
			port = (((u8 *)desc)[6] >> 3) ?
			       ((((u8 *)desc)[6] >> 3) - 1) : 0;

			if (bp >= ZX_NUM_BUFFERS || len < ETH_HLEN || len > 1536) {
				if (!a->rx_synced[q]) {
					u32 scan, idx2, found = 0;

					for (scan = 1; scan < ZX_NUM_RX_DESC; scan++) {
						__le32 *d2;
						u16 bp2, len2;

						idx2 = (idx + scan) % ZX_NUM_RX_DESC;
						d2 = (__le32 *)((u8 *)a->rx_ring +
						     q * ZX_RX_QUEUE_STRIDE +
						     idx2 * ZX_DESC_SIZE);
						dma_rmb();
						bp2 = (((u8 *)d2)[7] >> 1) |
						      ((u16)((u8 *)d2)[8] << 7);
						len2 = (le32_to_cpu(d2[3]) >> 2) & 0x3fff;
						if (bp2 < ZX_NUM_BUFFERS &&
						    len2 >= ETH_HLEN && len2 <= 1536) {
							found = 1;
							break;
						}
					}
					if (!found)
						break;
					a->rx_cur_idx[q] = idx2;
					a->rx_resync_cnt++;
					continue;
				}
				a->rx_bad_cnt++;
				zxna_rx_consume(desc, rel);
				a->rx_cur_idx[q]++;
				done++;
				avail--;
				if (bp && bp < ZX_NUM_BUFFERS)
					zxna_bmu_free(a, bp);
				continue;
			}
			a->rx_synced[q] = true;

			if (bp == 0) {
				a->rx_nobuf_cnt++;
				zxna_rx_consume(desc, rel);
				a->rx_cur_idx[q]++;
				done++;
				avail--;
				continue;
			}
			if (len >= ETH_HLEN) {
				u8 *d = a->buffer_pool + bp * ZX_BUF_SIZE + 16;

				if (zxna_own_addr(a, d + ETH_ALEN)) {
					a->rx_alias_cnt++;
					zxna_rx_consume(desc, rel);
					a->rx_cur_idx[q]++;
					done++;
					avail--;
					zxna_bmu_free(a, bp);
					continue;
				}
			}

			dev = port < ZX_NUM_PORTS ? a->ports[port] : NULL;
			skb = napi_alloc_skb(&a->napi, len);
			if (skb && dev && (dev->flags & IFF_UP)) {
				dma_rmb();
				memcpy(skb_put(skb, len),
				       a->buffer_pool + bp * ZX_BUF_SIZE + 16, len);
				skb->dev = dev;
				skb->protocol = eth_type_trans(skb, dev);
				napi_gro_receive(&a->napi, skb);
			} else {
				if (skb)
					dev_kfree_skb_any(skb);
			}

			zxna_rx_consume(desc, rel);
			a->rx_cur_idx[q]++;
			done++;
			avail--;
			zxna_bmu_free(a, bp);
		}
		if (rel[0] || rel[1])
			dma_wmb();
		if (rel[0])
			zxna_rx_release(a, q, 0, rel[0]);
		if (rel[1])
			zxna_rx_release(a, q, 1, rel[1]);
	}
	return done;
}

static irqreturn_t zxna_isr(int irq, void *id)
{
	struct zx_eth_adapter *a = id;

	if (!(readl(a->base + ZX_TM_INT_STATUS) & ZX_TM_INT_RX))
		return IRQ_NONE;
	writel(~0, a->base + ZX_TM_INT_MASK);
	napi_schedule(&a->napi);
	return IRQ_HANDLED;
}

static int zxna_poll(struct napi_struct *napi, int budget)
{
	struct zx_eth_adapter *a = container_of(napi, struct zx_eth_adapter, napi);
	int done;

	spin_lock_bh(&a->tx_lock);
	zxna_tx_reclaim(a);
	if (a->tx_pending >= ZX_TX_QUEUE_LIMIT)
		zxna_tx_kick(a);
	spin_unlock_bh(&a->tx_lock);
	done = budget ? zxna_rx(a, budget) : 0;

	if (done < budget && napi_complete_done(napi, done))
		writel(~ZX_TM_INT_RX, a->base + ZX_TM_INT_MASK);
	return done;
}

static enum hrtimer_restart zxna_tx_timer(struct hrtimer *timer)
{
	struct zx_eth_adapter *a = container_of(timer, struct zx_eth_adapter,
						tx_timer);

	napi_schedule(&a->napi);
	return HRTIMER_NORESTART;
}

/* ---- netdev ops ----------------------------------------------------- */

static int zxna_open(struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *a = priv->adapter;

	if (a->open_count == 0) {
		napi_enable(&a->napi);
		writel(~ZX_TM_INT_RX, a->base + ZX_TM_INT_MASK);
	}
	a->open_count++;
	phylink_start(priv->phylink);
	netif_start_queue(netdev);
	return 0;
}

static int zxna_stop(struct net_device *netdev)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *a = priv->adapter;

	netif_tx_disable(netdev);
	phylink_stop(priv->phylink);
	a->open_count--;
	if (a->open_count == 0) {
		writel(~0, a->base + ZX_TM_INT_MASK);
		hrtimer_cancel(&a->tx_timer);
		napi_disable(&a->napi);
		writel(~0, a->base + ZX_TM_INT_MASK);
		synchronize_irq(a->irq);
	}
	return 0;
}

static int zxna_set_mac(struct net_device *netdev, void *p)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	int ret = eth_mac_addr(netdev, p);

	if (ret)
		return ret;
	zxna_update_mac_tables(priv->adapter);
	return 0;
}

static int zxna_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);

	return phylink_mii_ioctl(priv->phylink, ifr, cmd);
}

/* ---- PPE offload hook (R3 skeleton, doc 38 §2) ----------------------- */
/*
 * The flow-offload entry point for the native data plane. The PPE
 * programming it will drive is pinned by evidence: the 45-byte CLA key
 * placement with the two CRC32 banks (docs 12/16 §7), a 127-flow
 * rewrite-index capacity with -ENOSPC fallback to the CPU path, and the
 * vendor trap-DMAC discipline (never clear the IPv6 trap half - doc 16
 * §4.2; the vendor's own v6 L3 forwarding is pure CPU at 143 Mbit/s, so
 * the skeleton offloads IPv4 only).
 *
 * Until the FWDL chain passes device validation (checklist 36 §2) this
 * hook refuses everything: with zxnative_eth.offload=0 (default) it
 * returns -EOPNOTSUPP before looking at the request; with offload=1 it
 * prints the programming plan and still refuses - the classifier is not
 * touched until the real flow programming lands in a later round.
 */
static bool zxnative_offload;
module_param_named(offload, zxnative_offload, bool, 0644);
MODULE_PARM_DESC(offload, "arm the PPE flow offload (register snapshot, V4 rules, packet modifier, trap/MAC flip)");



/* ---- ethtool --------------------------------------------------------- */

static void zxna_get_drvinfo(struct net_device *netdev,
			     struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "zxnative-eth", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(netdev->dev.parent),
		sizeof(info->bus_info));
}

static const char zxna_stats_strings[][ETH_GSTRING_LEN] = {
	"tx_pending",
	"tx_done_excess",
};

static void zxna_get_strings(struct net_device *netdev, u32 sset, u8 *data)
{
	if (sset != ETH_SS_STATS)
		return;
	memcpy(data, zxna_stats_strings, sizeof(zxna_stats_strings));
}

static int zxna_get_sset_count(struct net_device *netdev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return ARRAY_SIZE(zxna_stats_strings);
}

static void zxna_get_ethtool_stats(struct net_device *netdev,
				   struct ethtool_stats *stats, u64 *data)
{
	struct zx_eth_priv *priv = netdev_priv(netdev);
	struct zx_eth_adapter *a = priv->adapter;

	data[0] = a->tx_pending;
	data[1] = a->tx_done_excess;
}

static const struct ethtool_ops zxna_ethtool_ops = {
	.get_drvinfo		= zxna_get_drvinfo,
	.get_strings		= zxna_get_strings,
	.get_sset_count		= zxna_get_sset_count,
	.get_ethtool_stats	= zxna_get_ethtool_stats,
};

static const struct net_device_ops zxna_netdev_ops = {
	.ndo_open		= zxna_open,
	.ndo_stop		= zxna_stop,
	.ndo_start_xmit		= zxna_xmit,
	.ndo_set_mac_address	= zxna_set_mac,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= zxna_ioctl,
	.ndo_setup_tc		= zx_eth_setup_tc,
};

/* ppe.c classifies ingress ports through this hook (declared in the shared
 * header); the native driver answers for its own netdev_ops. */
bool zx_eth_is_port(const struct net_device *dev)
{
	return dev->netdev_ops == &zxna_netdev_ops;
}

/* ppe.c drives the trap-DMAC table through this shared-name hook; the
 * native driver keeps its own zxna_set_trap_dmac, so forward. */
void zx_set_trap_dmac(struct zx_eth_adapter *a, int idx, const u8 *mac)
{
	zxna_set_trap_dmac(a, idx, mac);
}


/* ---- probe/remove --------------------------------------------------- */

static int zxna_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *ports;
	struct zx_eth_adapter *a;
	int i, ret, registered = 0;
	bool napi_added = false;

	if (!zxnative_enable)
		return -ENODEV;

	ports = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports)
		return dev_err_probe(dev, -ENODEV, "no ethernet-ports node\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto put_ports;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a) {
		ret = -ENOMEM;
		goto put_ports;
	}
	a->dev = dev;
	spin_lock_init(&a->tx_lock);
	spin_lock_init(&a->bmu_free_lock);
	mutex_init(&a->ppe_lock);
	zx_ppe_init(a);
	a->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(a->base)) {
		ret = PTR_ERR(a->base);
		goto put_ports;
	}
	a->irq = platform_get_irq(pdev, 0);
	if (a->irq < 0) {
		ret = a->irq;
		goto put_ports;
	}

	a->buffer_pool = dmam_alloc_coherent(dev, ZX_DMA_ARENA_SIZE,
					     &a->buf_pool_dma, GFP_KERNEL);
	if (!a->buffer_pool) {
		ret = -ENOMEM;
		goto put_ports;
	}
	a->tab0 = a->buffer_pool + ZX_TAB0_OFFSET;
	a->tab0_dma = a->buf_pool_dma + ZX_TAB0_OFFSET;
	a->tab1 = a->buffer_pool + ZX_TAB1_OFFSET;
	a->tab1_dma = a->buf_pool_dma + ZX_TAB1_OFFSET;
	a->rx_ring = a->buffer_pool + ZX_RX_OFFSET;
	a->rx_ring_dma = a->buf_pool_dma + ZX_RX_OFFSET;
	a->tx_ring = a->buffer_pool + ZX_TX_OFFSET;
	a->tx_ring_dma = a->buf_pool_dma + ZX_TX_OFFSET;

	ret = zxna_hw_init(a);
	if (ret)
		goto put_ports;

	{
		struct device_node *np;

		for_each_available_child_of_node(ports, np) {
			struct net_device *netdev;
			struct zx_eth_priv *priv;
			phy_interface_t iface;
			const char *label;
			u32 port;
			u8 mac[ETH_ALEN];

			if (of_property_read_u32(np, "reg", &port) ||
			    port >= ZX_NUM_PORTS || a->ports[port])
				continue;
			netdev = devm_alloc_etherdev(dev, sizeof(*priv));
			if (!netdev) {
				of_node_put(np);
				ret = -ENOMEM;
				goto unreg;
			}
			SET_NETDEV_DEV(netdev, dev);
			if (!of_property_read_string(np, "label", &label))
				strscpy(netdev->name, label, IFNAMSIZ);
			/* The MAC lives in an nvmem cell of the flash partition
			 * layout. That supplier binds only after the MTD stack
			 * probes - after ALL built-in initcalls - so no amount
			 * of in-probe waiting helps (3.11 proved it: 8s of
			 * msleep still deferred). Defer the whole probe and
			 * let the deferred retry run once the layout is up;
			 * safe from the boot race because the baseline driver
			 * is no longer in the image (doc 62 A1). */
			ret = of_get_mac_address(np, mac);
			if (ret == -EPROBE_DEFER) {
				of_node_put(np);
				dev_err_probe(dev, ret, "%pOF: MAC nvmem not ready\n",
					      np);
				goto unreg;
			}
			if (!ret)
				eth_hw_addr_set(netdev, mac);
			else
				eth_hw_addr_random(netdev);
			netdev->netdev_ops = &zxna_netdev_ops;
			netdev->ethtool_ops = &zxna_ethtool_ops;
			/* nft flowtable offload gates on tc_can_offload():
			 * without NETIF_F_HW_TC the kernel never calls
			 * zxna_setup_tc (TC_SETUP_FT), so the PPE never
			 * arms - the 3.26 first-run gap. */
			netdev->hw_features |= NETIF_F_HW_TC;
			netdev->features |= NETIF_F_HW_TC;
			netdev->min_mtu = 68;
			netdev->max_mtu = 1500;
			priv = netdev_priv(netdev);
			priv->netdev = netdev;
			priv->adapter = a;
			priv->port = port;
			a->ports[port] = netdev;

			ret = of_get_phy_mode(np, &iface);
			if (ret) {
				dev_err_probe(dev, ret, "%pOF: no phy-mode\n", np);
				of_node_put(np);
				goto unreg;
			}
			priv->phylink_config.dev = &netdev->dev;
			priv->phylink_config.type = PHYLINK_NETDEV;
			priv->phylink_config.mac_capabilities =
				MAC_10 | MAC_100 | MAC_1000FD;
			__set_bit(PHY_INTERFACE_MODE_INTERNAL,
				  priv->phylink_config.supported_interfaces);
			__set_bit(PHY_INTERFACE_MODE_GMII,
				  priv->phylink_config.supported_interfaces);
			priv->phylink = phylink_create(&priv->phylink_config,
						       of_fwnode_handle(np), iface,
						       &zxna_mac_ops);
			if (IS_ERR(priv->phylink)) {
				ret = PTR_ERR(priv->phylink);
				priv->phylink = NULL;
				of_node_put(np);
				goto unreg;
			}
			ret = phylink_of_phy_connect(priv->phylink, np, 0);
			if (ret) {
				of_node_put(np);
				goto unreg;
			}
		}
	}

	for (i = 0; i < ZX_NUM_PORTS; i++)
		if (a->ports[i])
			break;
	if (i == ZX_NUM_PORTS) {
		ret = -ENODEV;
		goto unreg;
	}

	zxna_update_mac_tables(a);

	netif_napi_add(a->ports[i], &a->napi, zxna_poll);
	napi_added = true;
	hrtimer_setup(&a->tx_timer, zxna_tx_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	ret = devm_request_irq(dev, a->irq, zxna_isr, 0, dev_name(dev), a);
	if (ret)
		goto unreg;

	zxna_dma_enable(a, true);

	for (i = 0; i < ZX_NUM_PORTS; i++) {
		if (!a->ports[i])
			continue;
		ret = register_netdev(a->ports[i]);
		if (ret)
			goto unreg;
		registered++;
	}

	platform_set_drvdata(pdev, a);
	dev_info(dev, "native wired data plane up (%d ports)\n", ZX_NUM_PORTS);
	of_node_put(ports);
	return 0;

unreg:
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct zx_eth_priv *priv;

		if (!a->ports[i])
			continue;
		if (registered > 0) {
			registered--;
			unregister_netdev(a->ports[i]);
		}
		priv = netdev_priv(a->ports[i]);
		if (priv->phylink) {
			phylink_disconnect_phy(priv->phylink);
			phylink_destroy(priv->phylink);
		}
	}
	if (napi_added)
		netif_napi_del(&a->napi);
	zxna_hw_reset(a);
put_ports:
	of_node_put(ports);
	return ret;
}

static void zxna_remove(struct platform_device *pdev)
{
	struct zx_eth_adapter *a = platform_get_drvdata(pdev);
	int i;

	if (!a)
		return;
	/* The unbind/rebind cycle is a normal iteration step on the device
	 * (enable flip + rebind), so every port must leave the registry and
	 * every async entry (IRQ mask, tx hrtimer, shared NAPI, phylink) must
	 * be retired before devm frees the adapter: unregistering only the
	 * first port used to leave the other three dangling on freed memory.
	 * NAPI is deleted AFTER the unregister loop so a still-up port's
	 * ndo_stop can napi_disable it one last time. */
	writel(~0, a->base + ZX_TM_INT_MASK);
	hrtimer_cancel(&a->tx_timer);
	for (i = 0; i < ZX_NUM_PORTS; i++) {
		struct zx_eth_priv *priv;

		if (!a->ports[i])
			continue;
		unregister_netdev(a->ports[i]);
		priv = netdev_priv(a->ports[i]);
		if (priv->phylink) {
			phylink_disconnect_phy(priv->phylink);
			phylink_destroy(priv->phylink);
			priv->phylink = NULL;
		}
	}
	netif_napi_del(&a->napi);
	zxna_hw_reset(a);
}

/*
 * The switch keeps writing received frames into the DMA arena without the
 * CPU; stop it before a reboot or kexec hands that memory to the next
 * kernel (same reason as the baseline driver's shutdown hook).
 */
static void zxna_shutdown(struct platform_device *pdev)
{
	struct zx_eth_adapter *a = platform_get_drvdata(pdev);
	int i;

	if (!a)
		return;
	writel(~0, a->base + ZX_TM_INT_MASK);
	zxna_dma_enable(a, false);
	for (i = 0; i < ZX_NUM_PORTS; i++)
		zxna_port_enable(a, i, false);
	writel(0, a->base + ZX_BMU_CTRL);
}

static const struct of_device_id zxna_of_match[] = {
	{ .compatible = "zte,zx279128s-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, zxna_of_match);

static struct platform_driver zxna_driver = {
	.probe = zxna_probe,
	.remove = zxna_remove,
	.shutdown = zxna_shutdown,
	.driver = {
		.name = "zxnative-eth",
		.of_match_table = zxna_of_match,
	},
};
module_platform_driver(zxna_driver);

MODULE_DESCRIPTION("ZTE ZX279128S native wired data plane (P2)");
MODULE_LICENSE("GPL");
