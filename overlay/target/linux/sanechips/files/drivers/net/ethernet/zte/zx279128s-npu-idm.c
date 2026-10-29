// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE ZX279128S vendor-native IDM fabric engine.
 *
 * The IDM block at 0x921c8000 is the WiFi/fabric injection and delivery
 * engine. This driver owns one RX ring (2048 x 8B at +0x08) and one TX ring
 * (1024 x 8B at +0x04, placed at base+0x4000), both with the vendor doorbell
 * protocol:
 *
 *   TX kick/ack  +0x40   (write queued << 16; write completions as ack)
 *   TX done      +0x44   (read low 16)
 *   RX submit    +0x48   (write 0x10000 for every descriptor handed over)
 *   RX done      +0x4c   (read low 16)
 *
 * This is deliberately a standalone netdev (zxnpu0) rather than a mac80211
 * shim: the same engine will later feed the vendor-native MT7916 host driver.
 * It is disabled by default; zxnpu_idm.enable=1 plus "ip link set zxnpu0 up"
 * performs the first register write.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_packet.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define ZXNPU_NPP_BASE		0x921C0000
#define ZXNPU_IDM_BASE		0x921C8000
#define ZXNPU_NPP_SIZE		0x40000
#define ZXNPU_IDM_SIZE		0x4000

#define ZXNPU_NPP_INTEN		0x00004
#define ZXNPU_NPP_MASK0		0x00008
#define ZXNPU_NPP_MASK1		0x0000c
#define ZXNPU_NPP_CFG40		0x00040
#define ZXNPU_NPP_GREG_AUTHEN	0x00048
#define ZXNPU_NPP_GREG_CLOSED	0x0004c
#define ZXNPU_NPP_WIFI_Q1	0x00090
#define ZXNPU_NPP_PPB		0x10008
#define ZXNPU_SIPC_CTRL		0x0c000

#define ZXNPU_IDM_CFG00		0x00
#define ZXNPU_IDM_TX_BASE	0x04
#define ZXNPU_IDM_RX_BASE	0x08
#define ZXNPU_IDM_DEPTH		0x0c
#define ZXNPU_IDM_CFG10		0x10
#define ZXNPU_IDM_CFG18		0x18
#define ZXNPU_IDM_CFG1C		0x1c
#define ZXNPU_IDM_CFG24		0x24
#define ZXNPU_IDM_TX_DOORBELL	0x40
#define ZXNPU_IDM_TX_DONE	0x44
#define ZXNPU_IDM_RX_SUBMIT	0x48
#define ZXNPU_IDM_RX_DONE	0x4c
#define ZXNPU_IDM_RX_CMD	0x00010000

#define ZXNPU_RX_DESCS		2048
#define ZXNPU_TX_DESCS		1024
#define ZXNPU_TX_OFF		0x4000
#define ZXNPU_RING_SIZE		(ZXNPU_RX_DESCS * 8 + ZXNPU_TX_DESCS * 8)
#define ZXNPU_BUF_SIZE		2048
#define ZXNPU_BUF_HEAD		16
#define ZXNPU_TX_MINLEN		33
#define ZXNPU_TX_CHANNEL	0
#define ZXNPU_NAPI_WEIGHT	64

#define ZXNPU_RX_LEN		GENMASK(13, 0)
#define ZXNPU_RX_CHAN		GENMASK(18, 16)
#define ZXNPU_TX_PORT		GENMASK(30, 28)
#define ZXNPU_TX_VALID		BIT(31)

struct zxnpu_idm_desc {
	__le32 w0;
	__le32 w1;
} __packed;

struct zxnpu_idm {
	struct device *dev;
	struct net_device *ndev;
	struct napi_struct napi;
	struct proc_dir_entry *proc;
	void __iomem *npp;
	void __iomem *idm;

	struct zxnpu_idm_desc *ring;
	dma_addr_t ring_dma;
	struct zxnpu_idm_desc *rx;
	struct zxnpu_idm_desc *tx;

	void *bufs;
	dma_addr_t bufs_dma;

	struct sk_buff *tx_skb[ZXNPU_TX_DESCS];
	u32 tx_len[ZXNPU_TX_DESCS];
	u32 tx_cursor;
	u32 tx_head;
	u32 tx_queued;
	u32 tx_inflight;
	u32 tx_last_done;
	u32 rx_pos;

	u64 rx_frames;
	u64 rx_dropped;
	u64 rx_channels[8];
	u64 tx_frames;
	u64 tx_dropped;
	u64 tx_kicks;
	u64 tx_done;

	bool ready;
	spinlock_t lock;
};

static struct zxnpu_idm *zxnpu_idm_singleton;
static bool zxnpu_idm_enable;
module_param_named(enable, zxnpu_idm_enable, bool, 0644);
MODULE_PARM_DESC(enable, "perform the first IDM register write when zxnpu0 is opened");

static dma_addr_t zxnpu_buf_dma(struct zxnpu_idm *idm, u32 pos)
{
	return idm->bufs_dma + pos * ZXNPU_BUF_SIZE + ZXNPU_BUF_HEAD;
}

static void *zxnpu_buf_ptr(struct zxnpu_idm *idm, u32 pos)
{
	return idm->bufs + pos * ZXNPU_BUF_SIZE + ZXNPU_BUF_HEAD;
}

static void zxnpu_rx_submit(struct zxnpu_idm *idm, u32 pos)
{
	idm->rx[pos].w0 = cpu_to_le32(zxnpu_buf_dma(idm, pos));
	idm->rx[pos].w1 = 0;
	wmb();
	writel(ZXNPU_IDM_RX_CMD, idm->idm + ZXNPU_IDM_RX_SUBMIT);
}

static void zxnpu_tx_reclaim(struct zxnpu_idm *idm)
{
	u32 done = readl(idm->idm + ZXNPU_IDM_TX_DONE) & 0xffff;
	u32 completed = done - idm->tx_last_done;

	idm->tx_last_done = done;
	if (completed > idm->tx_inflight)
		completed = idm->tx_inflight;

	while (completed--) {
		struct sk_buff *skb = idm->tx_skb[idm->tx_cursor];
		u32 len = idm->tx_len[idm->tx_cursor];

		if (!skb)
			break;
		dma_unmap_single(idm->dev,
				 le32_to_cpu(idm->tx[idm->tx_cursor].w0),
				 len, DMA_TO_DEVICE);
		idm->tx_skb[idm->tx_cursor] = NULL;
		idm->tx_len[idm->tx_cursor] = 0;
		consume_skb(skb);
		idm->tx_cursor = (idm->tx_cursor + 1) % ZXNPU_TX_DESCS;
		idm->tx_inflight--;
		idm->tx_done++;
	}
}

static int zxnpu_rx_walk(struct zxnpu_idm *idm, int budget)
{
	int work = 0;

	while (work < budget && idm->ready) {
		u32 pos = idm->rx_pos % ZXNPU_RX_DESCS;
		u32 w1;
		u32 len, chan;
		struct sk_buff *skb;

		dma_rmb();
		w1 = le32_to_cpu(idm->rx[pos].w1);
		if (!w1)
			break;
		len = FIELD_GET(ZXNPU_RX_LEN, w1);
		chan = FIELD_GET(ZXNPU_RX_CHAN, w1);
		idm->rx_pos = (idm->rx_pos + 1) % ZXNPU_RX_DESCS;

		if (len < ETH_HLEN || len > ZXNPU_BUF_SIZE - ZXNPU_BUF_HEAD) {
			idm->rx_dropped++;
			goto resubmit;
		}

		skb = napi_alloc_skb(&idm->napi, len);
		if (!skb) {
			idm->rx_dropped++;
			goto resubmit;
		}
		skb_put_data(skb, zxnpu_buf_ptr(idm, pos), len);
		skb->dev = idm->ndev;
		skb->pkt_type = PACKET_HOST;
		skb_reset_mac_header(skb);
		skb->protocol = eth_hdr(skb)->h_proto;
		idm->rx_frames++;
		if (chan < ARRAY_SIZE(idm->rx_channels))
			idm->rx_channels[chan]++;
		netif_receive_skb(skb);
		work++;

resubmit:
		zxnpu_rx_submit(idm, pos);
	}
	return work;
}

static int zxnpu_idm_poll(struct napi_struct *napi, int budget)
{
	struct zxnpu_idm *idm = container_of(napi, struct zxnpu_idm, napi);
	int work;

	spin_lock_bh(&idm->lock);
	zxnpu_tx_reclaim(idm);
	work = zxnpu_rx_walk(idm, budget);
	spin_unlock_bh(&idm->lock);

	if (work < budget)
		napi_complete_done(napi, work);
	return work;
}

static netdev_tx_t zxnpu_idm_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct zxnpu_idm *idm = netdev_priv(ndev);
	dma_addr_t bus;
	u32 len, head;

	if (!idm->ready) {
		idm->tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_bh(&idm->lock);
	zxnpu_tx_reclaim(idm);
	if (idm->tx_inflight + idm->tx_queued >= ZXNPU_TX_DESCS - 1) {
		idm->tx_dropped++;
		spin_unlock_bh(&idm->lock);
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	len = max_t(u32, skb->len, ZXNPU_TX_MINLEN);
	bus = dma_map_single(idm->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(idm->dev, bus)) {
		idm->tx_dropped++;
		spin_unlock_bh(&idm->lock);
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	head = idm->tx_head;
	idm->tx_skb[head] = skb;
	idm->tx_len[head] = skb->len;
	idm->tx[head].w0 = cpu_to_le32(bus);
	idm->tx[head].w1 = cpu_to_le32(len |
			FIELD_PREP(ZXNPU_TX_PORT, ZXNPU_TX_CHANNEL) |
			ZXNPU_TX_VALID);
	idm->tx_head = (idm->tx_head + 1) % ZXNPU_TX_DESCS;
	idm->tx_queued++;
	wmb();

	/* kick once per packet: the engine consumes count<<16 per write */
	writel(idm->tx_queued << 16, idm->idm + ZXNPU_IDM_TX_DOORBELL);
	idm->tx_inflight += idm->tx_queued;
	idm->tx_queued = 0;
	idm->tx_frames++;
	idm->tx_kicks++;
	napi_schedule(&idm->napi);
	spin_unlock_bh(&idm->lock);
	return NETDEV_TX_OK;
}

static int zxnpu_idm_hw_init(struct zxnpu_idm *idm)
{
	u32 value;
	int i;

	idm->npp = ioremap(ZXNPU_NPP_BASE, ZXNPU_NPP_SIZE);
	idm->idm = ioremap(ZXNPU_IDM_BASE, ZXNPU_IDM_SIZE);
	if (!idm->npp || !idm->idm) {
		dev_err(idm->dev, "cannot map NPP/IDM windows\n");
		goto err_map;
	}

	idm->ring = dma_alloc_coherent(idm->dev, ZXNPU_RING_SIZE,
				       &idm->ring_dma, GFP_KERNEL);
	idm->bufs = dma_alloc_coherent(idm->dev,
				       ZXNPU_RX_DESCS * ZXNPU_BUF_SIZE,
				       &idm->bufs_dma, GFP_KERNEL);
	if (!idm->ring || !idm->bufs) {
		dev_err(idm->dev, "cannot allocate coherent rings\n");
		goto err_alloc;
	}
	memset(idm->ring, 0, ZXNPU_RING_SIZE);
	idm->rx = idm->ring;
	idm->tx = (struct zxnpu_idm_desc *)((u8 *)idm->ring + ZXNPU_TX_OFF);
	idm->tx_cursor = idm->tx_head = 0;
	idm->tx_queued = idm->tx_inflight = idm->tx_last_done = 0;
	idm->rx_pos = 0;

	/* NPP side: masks and the fabric's own port enables (vendor image) */
	writel(0x00ffffff, idm->npp + ZXNPU_NPP_MASK0);
	writel(0x000fffff, idm->npp + ZXNPU_NPP_MASK1);
	writel(0xffffffff, idm->npp + ZXNPU_NPP_INTEN);
	writel(0, idm->npp + ZXNPU_NPP_GREG_AUTHEN);
	writel(0, idm->npp + ZXNPU_NPP_GREG_CLOSED);
	writel(readl(idm->npp + ZXNPU_NPP_CFG40) | 0x300,
	       idm->npp + ZXNPU_NPP_CFG40);
	writel(128, idm->npp + ZXNPU_NPP_PPB);
	writel(0, idm->npp + ZXNPU_NPP_WIFI_Q1);
	writel(0, idm->npp + ZXNPU_NPP_WIFI_Q1 + 4);
	writel(readl(idm->npp + ZXNPU_SIPC_CTRL) | BIT(0),
	       idm->npp + ZXNPU_SIPC_CTRL);
	writel(readl(idm->npp + ZXNPU_SIPC_CTRL) & ~BIT(2),
	       idm->npp + ZXNPU_SIPC_CTRL);

	/* IDM engine parameters (vendor pon_npp_idm_init) */
	writel(31, idm->idm + ZXNPU_IDM_CFG24);
	writel(64, idm->idm + ZXNPU_IDM_CFG18);
	writel(5000, idm->idm + ZXNPU_IDM_CFG1C);
	writel(64, idm->idm + ZXNPU_IDM_CFG10);
	value = readl(idm->idm + ZXNPU_IDM_CFG00);
	value |= 0x000f0000;
	value = (value & ~0x0ff00000) | 0x02000000;
	writel(value, idm->idm + ZXNPU_IDM_CFG00);
	writel(0, idm->idm + ZXNPU_IDM_TX_DOORBELL);

	/* +0x08 RX base, +0x04 TX base (TX ring at base+0x4000) */
	writel(lower_32_bits(idm->ring_dma), idm->idm + ZXNPU_IDM_RX_BASE);
	writel(lower_32_bits(idm->ring_dma + ZXNPU_TX_OFF),
	       idm->idm + ZXNPU_IDM_TX_BASE);

	for (i = 0; i < ZXNPU_RX_DESCS; i++)
		zxnpu_rx_submit(idm, i);
	writel(ZXNPU_RX_DESCS | 0x04000000, idm->idm + ZXNPU_IDM_DEPTH);

	idm->ready = true;
	napi_enable(&idm->napi);
	netif_carrier_on(idm->ndev);
	netif_start_queue(idm->ndev);
	dev_info(idm->dev, "IDM ready: RX %d x 8B + TX %d x 8B\n",
		 ZXNPU_RX_DESCS, ZXNPU_TX_DESCS);
	return 0;

err_alloc:
	if (idm->bufs)
		dma_free_coherent(idm->dev, ZXNPU_RX_DESCS * ZXNPU_BUF_SIZE,
				  idm->bufs, idm->bufs_dma);
	if (idm->ring)
		dma_free_coherent(idm->dev, ZXNPU_RING_SIZE,
				  idm->ring, idm->ring_dma);
	idm->bufs = NULL;
	idm->ring = NULL;
err_map:
	if (idm->npp)
		iounmap(idm->npp);
	if (idm->idm)
		iounmap(idm->idm);
	idm->npp = NULL;
	idm->idm = NULL;
	return -ENOMEM;
}

static void zxnpu_idm_hw_exit(struct zxnpu_idm *idm)
{
	int i;

	netif_stop_queue(idm->ndev);
	netif_carrier_off(idm->ndev);
	napi_disable(&idm->napi);
	idm->ready = false;

	for (i = 0; i < ZXNPU_TX_DESCS; i++) {
		if (idm->tx_skb[i]) {
			dma_unmap_single(idm->dev,
					 le32_to_cpu(idm->tx[i].w0),
					 idm->tx_len[i], DMA_TO_DEVICE);
			consume_skb(idm->tx_skb[i]);
			idm->tx_skb[i] = NULL;
		}
	}
	if (idm->bufs)
		dma_free_coherent(idm->dev, ZXNPU_RX_DESCS * ZXNPU_BUF_SIZE,
				  idm->bufs, idm->bufs_dma);
	if (idm->ring)
		dma_free_coherent(idm->dev, ZXNPU_RING_SIZE,
				  idm->ring, idm->ring_dma);
	if (idm->npp)
		iounmap(idm->npp);
	if (idm->idm)
		iounmap(idm->idm);
	idm->bufs = NULL;
	idm->ring = NULL;
	idm->npp = NULL;
	idm->idm = NULL;
}

static int zxnpu_idm_open(struct net_device *ndev)
{
	struct zxnpu_idm *idm = netdev_priv(ndev);

	if (!zxnpu_idm_enable) {
		netdev_err(ndev, "disabled; set zxnpu_idm.enable=1 first\n");
		return -EOPNOTSUPP;
	}
	if (idm->ready)
		return 0;
	return zxnpu_idm_hw_init(idm);
}

static int zxnpu_idm_stop(struct net_device *ndev)
{
	struct zxnpu_idm *idm = netdev_priv(ndev);

	if (idm->ready)
		zxnpu_idm_hw_exit(idm);
	return 0;
}

static const struct net_device_ops zxnpu_idm_netdev_ops = {
	.ndo_open = zxnpu_idm_open,
	.ndo_stop = zxnpu_idm_stop,
	.ndo_start_xmit = zxnpu_idm_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int zxnpu_idm_status_show(struct seq_file *s, void *unused)
{
	struct zxnpu_idm *idm = zxnpu_idm_singleton;

	if (!idm)
		return 0;
	seq_printf(s, "version: npu-0.3-idm\n");
	seq_printf(s, "enable=%d ready=%d\n", zxnpu_idm_enable, idm->ready);
	seq_printf(s, "rx_frames=%llu rx_dropped=%llu rx_ch=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
		   idm->rx_frames, idm->rx_dropped,
		   idm->rx_channels[0], idm->rx_channels[1],
		   idm->rx_channels[2], idm->rx_channels[3],
		   idm->rx_channels[4], idm->rx_channels[5],
		   idm->rx_channels[6], idm->rx_channels[7]);
	seq_printf(s, "tx_frames=%llu tx_dropped=%llu tx_kicks=%llu tx_done=%llu inflight=%u queued=%u\n",
		   idm->tx_frames, idm->tx_dropped, idm->tx_kicks,
		   idm->tx_done, idm->tx_inflight, idm->tx_queued);
	return 0;
}

static int zxnpu_idm_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct zxnpu_idm *idm;
	struct proc_dir_entry *dir;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "no 32-bit DMA\n");

	ndev = alloc_etherdev(sizeof(*idm));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	strscpy(ndev->name, "zxnpu0", IFNAMSIZ);
	ndev->netdev_ops = &zxnpu_idm_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = 1500;
	idm = netdev_priv(ndev);
	idm->dev = &pdev->dev;
	idm->ndev = ndev;
	spin_lock_init(&idm->lock);
	netif_napi_add(ndev, &idm->napi, zxnpu_idm_poll);
	platform_set_drvdata(pdev, idm);
	zxnpu_idm_singleton = idm;

	dir = proc_mkdir("zx-npu-idm", NULL);
	if (dir) {
		if (!proc_create_single("status", 0444, dir,
					zxnpu_idm_status_show)) {
			proc_remove(dir);
			dir = NULL;
		}
	}
	idm->proc = dir;

	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&idm->napi);
		free_netdev(ndev);
		zxnpu_idm_singleton = NULL;
		return ret;
	}
	dev_info(&pdev->dev, "zxnpu0 registered; enable=%d\n", zxnpu_idm_enable);
	return 0;
}

static void zxnpu_idm_remove(struct platform_device *pdev)
{
	struct zxnpu_idm *idm = platform_get_drvdata(pdev);

	if (idm->ready)
		zxnpu_idm_hw_exit(idm);
	unregister_netdev(idm->ndev);
	netif_napi_del(&idm->napi);
	free_netdev(idm->ndev);
	if (idm->proc)
		proc_remove(idm->proc);
	zxnpu_idm_singleton = NULL;
}

static const struct of_device_id zxnpu_idm_of_match[] = {
	{ .compatible = "zte,zx279128s-npu-idm" },
	{ }
};
MODULE_DEVICE_TABLE(of, zxnpu_idm_of_match);

static struct platform_driver zxnpu_idm_driver = {
	.probe = zxnpu_idm_probe,
	.remove = zxnpu_idm_remove,
	.driver = {
		.name = "zx279128s-npu-idm",
		.of_match_table = zxnpu_idm_of_match,
	},
};
module_platform_driver(zxnpu_idm_driver);

MODULE_DESCRIPTION("ZTE ZX279128S vendor-native IDM fabric engine");
MODULE_LICENSE("GPL");
