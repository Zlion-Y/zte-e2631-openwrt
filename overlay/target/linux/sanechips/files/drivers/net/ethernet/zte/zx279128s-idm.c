// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s IDM: the WiFi <-> switch fabric injection/delivery engine
 *
#include <linux/bitfield.h>
 * The block at 0x921c8000 (vendor DT alias "idm") carries two RX rings and
 * one TX ring of 8-byte descriptors.  Frames the packet processor forwards
 * to a wireless "port" (fabric ports 0x18-0x1f) arrive in the RX rings;
 * frames coming from a wireless device are pushed through the TX ring so
 * the classifier sees them as if they had entered the fabric on that port.
 *
 * Register semantics reverse engineered from the vendor tm.ko (workspace
 * notes: re-notes-tm.md, re-notes-kernel.md, re-notes-portmap.md).  The
 * wireless bridge ports are wired up in zx_idm_fabric_init(), transcribed
 * from the vendor's bring-up sequence.
 *
 * Copyright (C) 2026 E2631 OpenWrt project
 */

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <net/flow_offload.h>
#include <net/netfilter/nf_flow_table.h>

#include "zx279128s-eth.h"

/* ---- Registers (offsets from the IDM block base) ---------------------- */

/*
 * All inside the gmac 4 MiB window the driver already maps: the fabric,
 * NPP and the IDM block are one address space (the vendor modules
 * address it as enc=(phys-0x92000000)>>2).
 */
#define ZX_IDM_BLOCK		0x1c8000
#define ZX_IDM_WINDOW		0x4000
#define ZX_NPP_GREG_AUTHEN	0x1c0048	/* per-port authen bits */
#define ZX_NPP_GREG_CLOSED	0x1c004c	/* per-port closed bits */
#define ZX_NPP_WIFI_Q1_PROTO	0x1c0090	/* +4: second word, 0 = raw */
#define ZX_SIPC_CTRL		0x1cc000	/* bit0 rx_en, bit2 cpu_up_en */
#define ZX_IDM_RXA_BASE		0x04	/* ring A descriptor list address */
#define ZX_IDM_RXB_BASE		0x08	/* ring B descriptor list address */
#define ZX_IDM_KICK		0x44	/* write: (new descriptors) << 16 */
#define ZX_IDM_DONE		0x4c	/* read: completed count, low 16, clears */

/*
 * RX descriptor (8 bytes): word 0 is the frame bus address, word 1 carries
 * what the hardware fills in: bits 13:0 length, bits 18:16 the fabric port
 * the frame came from, bit 31 "frame destined for a netdev".  The driver
 * clears word 1 on refill; the hardware sets it again.
 */
#define ZX_IDM_RX_LEN		GENMASK(13, 0)
#define ZX_IDM_RX_PORT		GENMASK(18, 16)
#define ZX_IDM_RX_FORDEV	BIT(31)

/*
 * TX descriptor: word 0 is the frame bus address (buffers keep the vendor's
 * 0x20 metadata header in front of the frame; the descriptor points at the
 * frame itself), word 1 is the length with the ingress port in bits 30:28
 * and bit 31 set.
 */
#define ZX_IDM_TX_PORT		GENMASK(30, 28)
#define ZX_IDM_TX_VALID		BIT(31)

/* ---- Ring and buffer geometry ----------------------------------------- */

#define ZX_IDM_RX_DESCS		2048
#define ZX_IDM_TX_DESCS		1024
#define ZX_IDM_BUFS		(2 * ZX_IDM_RX_DESCS)
#define ZX_IDM_BUF_SIZE		2048
#define ZX_IDM_BUF_HEAD		0x20	/* metadata header before the frame */
#define ZX_IDM_BUF_SPACE	(ZX_IDM_BUF_SIZE - ZX_IDM_BUF_HEAD)

/*
 * One registered wireless netdev.  The port numbering follows the vendor:
 * wlan0-3 are fabric ports 0x18-0x1b, wlan5g0-3 are 0x1c-0x1f, and the
 * packet modifier egress field (4 bits) carries what the vendor's
 * zte_api_sw_get_wlan_port() reports: 6 = 2.4 GHz, 7 = 5 GHz.  The fabric
 * resolves the exact VAP from its MAC table; with zx_wifi_egress_vap the
 * egress field carries 8 + the VAP index instead, for the case where the
 * fabric routes those VAP ports (0x18-0x1f & 0xf) straight to the IDM.
 */
struct zx_idm_wifi {
	struct net_device	*dev;
	u8	port;		/* fabric port 0x18 + n, TX descriptor ingress */
	u8	egress;		/* packet modifier egress field value */
	u8	idm_off;	/* index in the vendor wlan_to_idm_map */
};

struct zx_idm {
	/* singleton: the SoC has one fabric and one IDM block */
	struct zx_eth_adapter	*adapter;
	void __iomem		*base;

	__le32			*rxa;		/* ring descriptors, coherent */
	dma_addr_t		rxa_dma;
	__le32			*rxb;
	dma_addr_t		rxb_dma;
	__le32			*tx;
	dma_addr_t		tx_dma;

	void			*bufs;		/* the frame buffers, coherent */
	dma_addr_t		bufs_dma;
	u16			buf_free[ZX_IDM_BUFS];
	u32			buf_free_count;

	struct sk_buff		*tx_skb[ZX_IDM_TX_DESCS];
	u32			tx_head;	/* next descriptor to fill */
	u32			tx_tail;	/* oldest descriptor not completed */
	u32			tx_queued;	/* descriptors added, not kicked */

	u32			rx_pos[2];	/* next descriptor to check, per ring */
	u32			rx_frames;
	u32			rx_dropped;
	u32			rx_novap;
	u32			tx_dropped;
	u32			inject_full;
	u32			kick_writes;

	bool			ready;

	spinlock_t		lock;		/* rings, buffers, the wifi map */

	int			wifi_count;
	struct zx_idm_wifi	wifi[8];
};

static struct zx_idm zx_idm;
static bool zx_wifi_offload = true;
module_param_named(wifi_offload, zx_wifi_offload, bool, 0644);
MODULE_PARM_DESC(wifi_offload, "program wireless flows into the packet processor");

/*
 * egress field mode: false (default) writes the fabric port 0x18-0x1f
 * the vendor's fastHashRule uses; true writes only its low nibble (8-15,
 * wlan class bit cleared) - in case the high bits of the field turn out
 * to mean something else on this board.  The device test decides.
 */
static bool zx_wifi_egress_low;
module_param_named(wifi_egress_low, zx_wifi_egress_low, bool, 0644);
MODULE_PARM_DESC(wifi_egress_low, "write only the low nibble of the fabric port into the egress field");

struct zx_eth_adapter *zx_idm_adapter(void)
{
	return zx_idm.adapter;
}

/* ---- Buffer pool ------------------------------------------------------- */

static u32 zx_idm_buf_alloc(struct zx_idm *idm)
{
	if (!idm->buf_free_count)
		return ZX_IDM_BUFS;
	return idm->buf_free[--idm->buf_free_count];
}

static void zx_idm_buf_free(struct zx_idm *idm, u32 idx)
{
	if (idx < ZX_IDM_BUFS && idm->buf_free_count < ZX_IDM_BUFS)
		idm->buf_free[idm->buf_free_count++] = idx;
}

static dma_addr_t zx_idm_buf_dma(struct zx_idm *idm, u32 idx)
{
	return idm->bufs_dma + idx * ZX_IDM_BUF_SIZE;
}

static void *zx_idm_buf_ptr(struct zx_idm *idm, u32 idx)
{
	return idm->bufs + idx * ZX_IDM_BUF_SIZE;
}

static void zx_idm_refill(struct zx_idm *idm, __le32 *ring, u32 pos)
{
	u32 idx = zx_idm_buf_alloc(idm);
	__le32 *desc = ring + pos * 2;

	desc[0] = cpu_to_le32(zx_idm_buf_dma(idm, idx));
	/* keep the buffer index around: stash it in the descriptor word 1
	 * bits the hardware does not use, so RX never has to divide.
	 */
	desc[1] = cpu_to_le32(FIELD_PREP(GENMASK(29, 20), idx));
	wmb();
}

static u32 zx_idm_desc_buf(u32 w1)
{
	return FIELD_GET(GENMASK(29, 20), w1);
}

/* ---- TX --------------------------------------------------------------- */

static void zx_idm_tx_kick(struct zx_idm *idm)
{
	if (idm->tx_queued) {
		writel(idm->tx_queued << 16, idm->base + ZX_IDM_KICK);
		idm->kick_writes++;
		idm->tx_queued = 0;
	}
}

static void zx_idm_tx_reclaim(struct zx_idm *idm)
{
	u32 done = readl(idm->base + ZX_IDM_DONE) & 0xffff;

	while (done--) {
		struct sk_buff *skb = idm->tx_skb[idm->tx_tail];

		if (!skb)
			break;
		idm->tx_skb[idm->tx_tail] = NULL;
		dma_unmap_single(idm->adapter->dev, le32_to_cpu(idm->tx[idm->tx_tail * 2]),
				 skb->len, DMA_TO_DEVICE);
		consume_skb(skb);
		idm->tx_tail = (idm->tx_tail + 1) % ZX_IDM_TX_DESCS;
	}
}

/* Hand one frame to the fabric on the VAP's port.  Called in softirq
 * context from the flowtable hook.  Returns 0 (queued) or -EBUSY.
 */
static int zx_idm_inject(struct zx_idm *idm, struct sk_buff *skb,
			 struct zx_idm_wifi *w)
{
	dma_addr_t dma;
	u32 next;
	int ret = 0;

	spin_lock_bh(&idm->lock);
	zx_idm_tx_reclaim(idm);
	next = (idm->tx_head + 1) % ZX_IDM_TX_DESCS;
	if (next == idm->tx_tail) {
		idm->inject_full++;
		ret = -EBUSY;
		goto out;
	}
	dma = dma_map_single(idm->adapter->dev, skb->data, skb->len,
			     DMA_TO_DEVICE);
	if (dma_mapping_error(idm->adapter->dev, dma)) {
		idm->tx_dropped++;
		ret = -EBUSY;
		goto out;
	}
	idm->tx_skb[idm->tx_head] = skb;
	idm->tx[idm->tx_head * 2] = cpu_to_le32(dma);
	idm->tx[idm->tx_head * 2 + 1] = cpu_to_le32(skb->len |
			FIELD_PREP(ZX_IDM_TX_PORT, w->idm_off) | ZX_IDM_TX_VALID);
	wmb();
	idm->tx_head = next;
	idm->tx_queued++;
	zx_idm_tx_kick(idm);
	/* nobody else may poll: with no wired traffic the napi never fires
	 * on its own, and the TX completion (and the RX rings) need it
	 */
	napi_schedule(&idm->adapter->napi);
out:
	spin_unlock_bh(&idm->lock);
	return ret;
}

/* ---- RX --------------------------------------------------------------- */

/* Deliver fabric-forwarded frames to their wireless netdev. */
static int zx_idm_rx_ring(struct zx_idm *idm, __le32 *ring, int ring_idx,
			  int budget)
{
	int work = 0;

	while (work < budget) {
		u32 pos = idm->rx_pos[ring_idx] % ZX_IDM_RX_DESCS;
		__le32 *desc = ring + pos * 2;
		u32 w1 = le32_to_cpu(desc[1]);
		u32 len = FIELD_GET(ZX_IDM_RX_LEN, w1);
		u32 port = FIELD_GET(ZX_IDM_RX_PORT, w1);
		u32 buf = zx_idm_desc_buf(w1);
		struct zx_idm_wifi *w = NULL;
		struct sk_buff *skb;
		int i;

		if (!w1 || !len)
			return work;	/* hardware has not filled it yet */

		idm->rx_pos[ring_idx]++;

		for (i = 0; i < idm->wifi_count; i++)
			if (idm->wifi[i].idm_off == port && idm->wifi[i].dev) {
				w = &idm->wifi[i];
				break;
			}

		if (!w) {
			idm->rx_novap++;
		} else if (len < ETH_HLEN || len > ZX_IDM_BUF_SPACE ||
			   buf >= ZX_IDM_BUFS || !(w1 & ZX_IDM_RX_FORDEV)) {
			idm->rx_dropped++;
		} else {
			/* copy out of the coherent arena: build_skb() would
			 * hand dma memory to kfree() at skb release time
			 */
			skb = napi_alloc_skb(&idm->adapter->napi, len);
			if (!skb) {
				idm->rx_dropped++;
			} else {
				skb_put_data(skb, zx_idm_buf_ptr(idm, buf), len);
				skb->dev = w->dev;
				skb->pkt_type = PACKET_HOST;
				skb->protocol = ((struct ethhdr *)skb->data)->h_proto;
				skb_reset_mac_header(skb);
				skb_clear_tstamp(skb);
				dev_queue_xmit(skb);
				idm->rx_frames++;
				work++;
			}
		}

		/* the buffer is gone either way: hand a fresh one to the ring */
		if (buf < ZX_IDM_BUFS)
			zx_idm_buf_free(idm, buf);
		zx_idm_refill(idm, ring, pos);
	}
	return work;
}

int zx_idm_poll(struct zx_eth_adapter *adapter, int budget)
{
	struct zx_idm *idm = &zx_idm;
	int work = 0;

	if (!idm->ready || adapter != idm->adapter)
		return 0;

	spin_lock_bh(&idm->lock);
	zx_idm_tx_reclaim(idm);
	if (budget) {
		work += zx_idm_rx_ring(idm, idm->rxa, 0, budget - work);
		work += zx_idm_rx_ring(idm, idm->rxb, 1, budget - work);
	}
	spin_unlock_bh(&idm->lock);
	return work;
}

/* ---- The wireless netdev map ------------------------------------------ */

/* Parse the device naming: the vendor's wlanN (2.4 GHz VAP N) and
 * wlan5gN (5 GHz VAP N), or OpenWrt's phy0-apN / phy1-apN (the mt7916's
 * 2.4 and 5 GHz radios).  Returns 0 with the entry filled in.
 */
static int zx_idm_wifi_identify(const char *name, struct zx_idm_wifi *w)
{
	bool five = false;
	unsigned long n;
	int ret;

	if (!strncmp(name, "wlan5g", 6)) {
		five = true;
		name += 6;
	} else if (!strncmp(name, "wlan", 4)) {
		name += 4;
	} else if (!strncmp(name, "phy1-ap", 7)) {
		/* OpenWrt mt76 naming: phy1 is the 5 GHz radio of the mt7916 */
		five = true;
		name += 7;
	} else if (!strncmp(name, "phy0-ap", 7)) {
		name += 7;
	} else {
		return -EOPNOTSUPP;
	}
	ret = kstrtoul(name, 10, &n);
	if (ret || n > 3)
		return -EOPNOTSUPP;

	w->port = 0x18 + five * 4 + n;
	w->idm_off = five * 4 + n;
	w->egress = zx_wifi_egress_low ? 8 + w->idm_off : w->port;
	return 0;
}

static int zx_idm_register_wifi(struct net_device *dev)
{
	struct zx_idm_wifi w = {};
	int i, ret;

	ret = zx_idm_wifi_identify(dev->name, &w);
	if (ret)
		return ret;
	for (i = 0; i < zx_idm.wifi_count; i++)
		if (zx_idm.wifi[i].dev == dev)
			return 0;	/* already there */
	if (zx_idm.wifi_count == ARRAY_SIZE(zx_idm.wifi))
		return -ENOSPC;

	w.dev = dev;
	dev_hold(dev);
	zx_idm.wifi[zx_idm.wifi_count++] = w;
	return 0;
}

/* ---- Flow offload plumbing -------------------------------------------- */

/* The flowtable hook of a wireless netdev, called from mac80211's
 * ieee80211_netdev_setup_tc() through the patch in the mac80211 package.
 */
int zx_wifi_flow_offload(struct net_device *dev, enum tc_setup_type type,
			 void *type_data)
{
	int ret;

	if (type != TC_SETUP_FT || !zx_wifi_offload)
		return -EOPNOTSUPP;
	if (!zx_idm.ready)
		return -EOPNOTSUPP;

	ret = zx_idm_register_wifi(dev);
	if (ret)
		return ret;

	return zx_eth_setup_tc(dev, type, type_data);
}
EXPORT_SYMBOL_GPL(zx_wifi_flow_offload);

/* The uplink of a hardware-offloaded wireless flow, called from the
 * nf_flow_table ingress hook (kernel patch 035).  Returns NF_STOLEN when
 * the frame was queued into the fabric, NF_ACCEPT to fall back to the
 * software flowtable path.
 */
int zx_idm_flow_xmit(struct sk_buff *skb, struct nf_flowtable *ft,
		     struct flow_offload *flow)
{
	struct zx_idm_wifi *w = NULL;
	int i;

	if (!zx_idm.ready)
		return NF_ACCEPT;

	for (i = 0; i < zx_idm.wifi_count; i++)
		if (zx_idm.wifi[i].dev == skb->dev) {
			w = &zx_idm.wifi[i];
			break;
		}
	if (!w)
		return NF_ACCEPT;

	if (zx_idm_inject(&zx_idm, skb, w))
		return NF_ACCEPT;	/* ring full: the software path takes it */
	return NF_STOLEN;
}
EXPORT_SYMBOL_GPL(zx_idm_flow_xmit);

/* ---- Lookups zx279128s-ppe.c uses -------------------------------------- */

bool zx_idm_wifi_ingress(const struct zx_eth_adapter *adapter, int ifindex)
{
	int i;

	if (adapter != zx_idm.adapter)
		return false;
	for (i = 0; i < zx_idm.wifi_count; i++)
		if (zx_idm.wifi[i].dev &&
		    zx_idm.wifi[i].dev->ifindex == ifindex)
			return true;
	return false;
}

/*
 * Egress value of a flow whose destination is wireless.  odev is either a
 * wireless netdev itself (uplink is never the case, but harmless) or the
 * bridge, in which case the kernel bridge FDB tells us which VAP holds the
 * destination MAC.
 */
int zx_idm_wifi_egress(struct zx_eth_adapter *adapter,
		       const struct net_device *odev, const u8 *dst)
{
	struct net_device *br;
	struct net_device *port;
	struct zx_idm_wifi *w = NULL;
	int i;

	if (adapter != zx_idm.adapter || !zx_idm.wifi_count)
		return -EOPNOTSUPP;

	for (i = 0; i < zx_idm.wifi_count; i++) {
		if (zx_idm.wifi[i].dev == odev) {
			w = &zx_idm.wifi[i];
			break;
		}
	}
	if (!w) {
		if (!IS_ENABLED(CONFIG_BRIDGE) ||
		    !netif_is_bridge_master(odev))
			return -EOPNOTSUPP;
		rcu_read_lock();
		br = netdev_master_upper_dev_get_rcu((struct net_device *)
						     zx_idm.wifi[0].dev);
		if (!br || br != odev) {
			rcu_read_unlock();
			return -EOPNOTSUPP;
		}
		port = br_fdb_find_port(odev, dst, 0);
		rcu_read_unlock();
		if (!port)
			return -EOPNOTSUPP;
		for (i = 0; i < zx_idm.wifi_count; i++)
			if (zx_idm.wifi[i].dev == port) {
				w = &zx_idm.wifi[i];
				break;
			}
		if (!w)
			return -EOPNOTSUPP;
	}

	if (zx_wifi_egress_low)
		return 8 + w->idm_off;
	return w->egress;
}

/* ---- Bring-up and teardown -------------------------------------------- */

/*
 * Bring the NPP/GREG ports the injected and forwarded frames pass
 * through out of their reset state (the vendor greg_initial clears the
 * authen and closed bits of all seven logical ports), select the raw
 * wifi queue protocol and enable the SIPC receive path.  The VAP ports
 * 0x18-0x1f need no register: the fabric decodes the classifier egress
 * value straight to the IDM.  Transcribed from the vendor tm.ko
 * greg_initial/sipc_initial (re-notes-portmap.md SS3.4).
 */
static void zx_idm_fabric_init(struct zx_eth_adapter *adapter)
{
	void __iomem *greg = adapter->base;
	u32 mask;
	int p;

	for (p = 0; p <= 5; p++) {
		mask = 1u << p;
		writel(readl(greg + ZX_NPP_GREG_AUTHEN) & ~mask,
		       greg + ZX_NPP_GREG_AUTHEN);
		writel(readl(greg + ZX_NPP_GREG_CLOSED) & ~mask,
		       greg + ZX_NPP_GREG_CLOSED);
	}
	writel(readl(greg + ZX_NPP_GREG_AUTHEN) & ~BIT(6),
	       greg + ZX_NPP_GREG_AUTHEN);
	writel(readl(greg + ZX_NPP_GREG_CLOSED) & ~BIT(6),
	       greg + ZX_NPP_GREG_CLOSED);

	writel(0, greg + ZX_NPP_WIFI_Q1_PROTO);
	writel(0, greg + ZX_NPP_WIFI_Q1_PROTO + 4);

	writel(readl(greg + ZX_SIPC_CTRL) | BIT(0), greg + ZX_SIPC_CTRL);
	writel(readl(greg + ZX_SIPC_CTRL) & ~BIT(2), greg + ZX_SIPC_CTRL);
}

int zx_idm_init(struct zx_eth_adapter *adapter)
{
	struct zx_idm *idm = &zx_idm;
	int i;

	if (zx_idm.ready || zx_idm.adapter) {
		/* the SoC has one fabric: a second gmac instance is not
		 * supported by this driver
		 */
		return adapter == zx_idm.adapter ? 0 : -EBUSY;
	}

	idm->adapter = adapter;
	idm->base = adapter->base + ZX_IDM_BLOCK;

	idm->rxa = dmam_alloc_coherent(adapter->dev, ZX_IDM_RX_DESCS * 8,
				       &idm->rxa_dma, GFP_KERNEL);
	idm->rxb = dmam_alloc_coherent(adapter->dev, ZX_IDM_RX_DESCS * 8,
				       &idm->rxb_dma, GFP_KERNEL);
	idm->tx = dmam_alloc_coherent(adapter->dev, ZX_IDM_TX_DESCS * 8,
				      &idm->tx_dma, GFP_KERNEL);
	idm->bufs = dmam_alloc_coherent(adapter->dev,
					ZX_IDM_BUFS * ZX_IDM_BUF_SIZE,
					&idm->bufs_dma, GFP_KERNEL);
	if (!idm->rxa || !idm->rxb || !idm->tx || !idm->bufs)
		return -ENOMEM;
	memset(idm->rxa, 0, ZX_IDM_RX_DESCS * 8);
	memset(idm->rxb, 0, ZX_IDM_RX_DESCS * 8);
	memset(idm->tx, 0, ZX_IDM_TX_DESCS * 8);

	for (i = 0; i < ZX_IDM_BUFS; i++)
		idm->buf_free[i] = ZX_IDM_BUFS - 1 - i;
	idm->buf_free_count = ZX_IDM_BUFS;

	for (i = 0; i < ZX_IDM_RX_DESCS; i++) {
		zx_idm_refill(idm, idm->rxa, i);
		zx_idm_refill(idm, idm->rxb, i);
	}

	writel(idm->rxa_dma, idm->base + ZX_IDM_RXA_BASE);
	writel(idm->rxb_dma, idm->base + ZX_IDM_RXB_BASE);
	zx_idm_fabric_init(adapter);

	idm->ready = true;
	pr_info("%s: IDM ready, rings A/B %d descs, %d KiB buffers\n",
		DRV_NAME, ZX_IDM_RX_DESCS,
		ZX_IDM_BUFS * ZX_IDM_BUF_SIZE / 1024);
	return 0;
}

void zx_idm_exit(struct zx_eth_adapter *adapter)
{
	struct zx_idm *idm = &zx_idm;
	int i;

	if (zx_idm.adapter != adapter)
		return;
	idm->ready = false;
	for (i = 0; i < idm->wifi_count; i++)
		if (idm->wifi[i].dev)
			dev_put(idm->wifi[i].dev);
	idm->wifi_count = 0;
	zx_idm.adapter = NULL;
}
