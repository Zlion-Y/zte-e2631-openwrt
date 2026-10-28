// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE zx279128s IDM: the WiFi <-> switch fabric injection/delivery engine
 *
 * The block at 0x921c8000 (vendor DT alias "idm") carries one RX ring
 * (2048 x 8B descriptors) and one TX ring (1024 x 8B).  Frames the packet
 * processor forwards to a wireless "port" (fabric ports 0x18-0x1f) arrive
 * in the RX ring; frames pushed through the TX ring enter the fabric as if
 * they had been received on that port.
 *
 * Register semantics transcribed from the vendor plat-zxylzb_9128S.ko
 * (tm_pon_npp_init / pon_npp_idm_init / idm_net_tx; workspace notes
 * re-notes-tm.md SS5 and the annotated plat disassembly).  The details the
 * first (ffe-full-0.2) revision got wrong, each with its hardware symptom:
 *  - RX buffers must be SUBMITTED one by one through IDM+0x48 (the engine
 *    keeps its own ring; writing the shared descriptors alone delivers
 *    nothing) - symptom: downlink to the radio blackholed once the flow
 *    went hardware-offloaded;
 *  - IDM+0x04 is the TX ring base, IDM+0x08 the RX ring base (0.2
 *    programmed 0x04 as a second RX ring, so injected uplink never reached
 *    the fabric either);
 *  - the engine params (+0x10/0x18/0x1c/0x24), the ring depth (+0x0c), the
 *    enable bits (+0x00) and the NPP global masks are all part of the
 *    bring-up, not optional.
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

/* ---- Registers (offsets from the IDM block base 0x921c8000) ----------- */

/*
 * All inside the gmac 4 MiB window the driver already maps (the fabric,
 * NPP and the IDM block are one address space; the vendor modules address
 * it as enc=(phys-0x92000000)>>2).
 */
#define ZX_IDM_BLOCK		0x1c8000
#define ZX_NPP_GREG_AUTHEN	0x1c0048	/* per-port authen bits */
#define ZX_NPP_GREG_CLOSED	0x1c004c	/* per-port closed bits */
#define ZX_NPP_WIFI_Q1_PROTO	0x1c0090	/* +4: second word, 0 = raw */
#define ZX_SIPC_CTRL		0x1cc000	/* bit0 rx_en, bit2 cpu_up_en */

/* NPP global masks/enable (tm_pon_npp_init) */
#define ZX_NPP_INTEN		0x1c0004	/* = 0xffffffff */
#define ZX_NPP_MASK0		0x1c0008	/* = 0x00ffffff */
#define ZX_NPP_MASK1		0x1c000c	/* = 0x000fffff */
#define ZX_NPP_CFG40		0x1c0040	/* |= 0x300 */
#define ZX_NPP_CFG48		0x1c0048	/* = 0 */
#define ZX_NPP_PPB		0x1d0008	/* = 128 (npp + 0x10008) */

/* IDM block */
#define ZX_IDM_CFG00		0x00	/* |= 0xf0000; = ~0xff00000|0x2000000 */
#define ZX_IDM_TX_BASE		0x04	/* TX ring bus address (1024 x 8B) */
#define ZX_IDM_RX_BASE		0x08	/* RX ring bus address (2048 x 8B) */
#define ZX_IDM_DEPTH		0x0c	/* = 0x04000800 */
#define ZX_IDM_CFG10		0x10	/* = 64 */
#define ZX_IDM_CFG18		0x18	/* = 64 */
#define ZX_IDM_CFG1C		0x1c	/* = 5000 */
#define ZX_IDM_CFG24		0x24	/* = 31 */
#define ZX_IDM_CFG40		0x40	/* ring alignment mark (0 when aligned) */
#define ZX_IDM_KICK		0x44	/* write: (new tx desc) << 16; read low16: tx done */
#define ZX_IDM_RXBUF		0x48	/* RX buffer submission port (bus addr) */
#define ZX_IDM_RXDONE		0x4c	/* read: completed rx count, low 16, read-clear */

/*
 * RX descriptor (8 bytes): word 0 = buffer bus address written by the CPU;
 * on completion the hardware fills word 1: bits 13:0 length, bits 18:16 the
 * fabric channel the frame came from, bit 31 "frame destined for a netdev".
 * The frame itself is DMAed to word0 + 16 (the first 16 bytes of the buffer
 * carry engine metadata), so the submission points at data - 16 with the
 * skb reserving 32 bytes, exactly as the vendor does.
 */
#define ZX_IDM_RX_LEN		GENMASK(13, 0)
#define ZX_IDM_RX_PORT		GENMASK(18, 16)
#define ZX_IDM_RX_FORDEV	BIT(31)

/*
 * TX descriptor: word 0 = frame bus address, word 1 = length[13:0] (min 33)
 * with the fabric channel in bits 30:28 and the valid bit 31.
 */
#define ZX_IDM_TX_PORT		GENMASK(30, 28)
#define ZX_IDM_TX_VALID		BIT(31)
#define ZX_IDM_TX_MINLEN	33

/* ---- Ring and buffer geometry (vendor-exact) -------------------------- */

#define ZX_IDM_RX_DESCS		2048
#define ZX_IDM_TX_DESCS		1024
#define ZX_IDM_RING_SIZE	(24 * 1024)	/* 16K RX + 8K TX as one area */
#define ZX_IDM_TX_OFF		0x4000
#define ZX_IDM_BUF_SIZE		2048
#define ZX_IDM_BUF_HEAD		16		/* engine metadata before the frame */

/*
 * One registered wireless netdev: fabric port 0x18 + n (2.4G VAPs 0x18-0x1b,
 * 5G VAPs 0x1c-0x1f), the same value as the classifier egress and the RX/TX
 * channel number (the vendor wlan_to_idm_map: wlan0-3 -> (idm1, 0-3),
 * wlan5g0-3 -> (idm1, 4-7); the channel is the low 3 bits).
 */
struct zx_idm_wifi {
	struct net_device	*dev;
	u8	port;		/* fabric port 0x18 + n */
	u8	idm_off;	/* channel number, 0-7 */
};

struct zx_idm {
	/* singleton: the SoC has one fabric and one IDM block */
	struct zx_eth_adapter	*adapter;
	void __iomem		*base;		/* npp_base + 0x8000 */

	void			*ring;		/* 24K coherent descriptor area */
	dma_addr_t		ring_dma;
	struct zx_idm_desc {
		__le32	w0;
		__le32	w1;
	} __packed		*rx;		/* = ring */
	struct zx_idm_desc	*tx;		/* = ring + 0x4000 */

	void			*bufs;		/* RX frame buffers, coherent */
	dma_addr_t		bufs_dma;

	struct sk_buff		*tx_skb[ZX_IDM_TX_DESCS];
	u32			tx_cursor;	/* oldest tx descriptor not completed */
	u32			tx_queued;	/* filled, not yet kicked */
	u32			rx_pos;		/* next rx descriptor to check */

	u32			rx_frames;
	u32			rx_dropped;
	u32			rx_novap;
	u32			rx_unicast;
	u32			rx_ch[8];
	u32			inject_ok;
	u32			inject_full;
	u32			tx_kicks;
	u32			rx_submit;

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
 * Egress field mode: false (default) writes the fabric port 0x18-0x1f the
 * vendor fastHashRule uses; true writes only the low nibble (8-15, the wlan
 * class bit cleared).  The live downlink test decides - the portmap reverse
 * engineering and the hardware test historically disagreed.
 */
static bool zx_wifi_egress_low;
module_param_named(wifi_egress_low, zx_wifi_egress_low, bool, 0644);
MODULE_PARM_DESC(wifi_egress_low, "write only the low nibble of the fabric port into the egress field");

/* the nf_flow_table hook (patch 036) counts its own invocations here */
u32 zx_idm_hook_fired;
EXPORT_SYMBOL_GPL(zx_idm_hook_fired);

struct zx_eth_adapter *zx_idm_adapter(void)
{
	return zx_idm.adapter;
}

/* ---- Buffer pool ------------------------------------------------------- */

static dma_addr_t zx_idm_buf_dma(struct zx_idm *idm, u32 pos)
{
	/* slot pos owns the buffer at pos for its whole life */
	return idm->bufs_dma + pos * ZX_IDM_BUF_SIZE;
}

static void *zx_idm_buf_ptr(struct zx_idm *idm, u32 pos)
{
	return idm->bufs + pos * ZX_IDM_BUF_SIZE;
}

/* Submit one filled buffer to the engine: descriptor word 0 in the shared
 * ring AND the bus address through the IDM+0x48 port.  The engine walks its
 * internal ring in submission order, so the buffer's slot in the ring array
 * is also its slot in the skb table.
 */
static void zx_idm_rx_submit(struct zx_idm *idm, u32 pos, dma_addr_t bus)
{
	idm->rx[pos].w0 = cpu_to_le32(bus);
	idm->rx[pos].w1 = 0;
	wmb();
	writel(bus, idm->base + ZX_IDM_RXBUF);
	idm->rx_submit++;
}

/* ---- TX --------------------------------------------------------------- */

static void zx_idm_tx_kick(struct zx_idm *idm)
{
	if (idm->tx_queued) {
		writel(idm->tx_queued << 16, idm->base + ZX_IDM_KICK);
		idm->tx_kicks++;
		idm->tx_queued = 0;
	}
}

static void zx_idm_tx_reclaim(struct zx_idm *idm)
{
	u32 done = readl(idm->base + ZX_IDM_KICK) & 0xffff;

	while (done--) {
		struct sk_buff *skb = idm->tx_skb[idm->tx_cursor];

		if (!skb)
			break;
		idm->tx_skb[idm->tx_cursor] = NULL;
		dma_unmap_single(idm->adapter->dev,
				 le32_to_cpu(idm->tx[idm->tx_cursor].w0),
				 skb->len, DMA_TO_DEVICE);
		consume_skb(skb);
		idm->tx_cursor = (idm->tx_cursor + 1) % ZX_IDM_TX_DESCS;
	}
}

/* Hand one frame to the fabric on the VAP's channel.  Called in softirq
 * context from the flowtable hook.  Returns 0 (queued) or -EBUSY.
 */
static int zx_idm_inject(struct zx_idm *idm, struct sk_buff *skb,
			 struct zx_idm_wifi *w)
{
	dma_addr_t bus;
	u32 len, cursor;

	spin_lock_bh(&idm->lock);
	zx_idm_tx_reclaim(idm);
	if (idm->tx_queued >= ZX_IDM_TX_DESCS - 1) {
		idm->inject_full++;
		spin_unlock_bh(&idm->lock);
		return -EBUSY;
	}
	cursor = (idm->tx_cursor + idm->tx_queued) % ZX_IDM_TX_DESCS;
	len = max_t(u32, skb->len, ZX_IDM_TX_MINLEN);
	bus = dma_map_single(idm->adapter->dev, skb->data, skb->len,
			     DMA_TO_DEVICE);
	if (dma_mapping_error(idm->adapter->dev, bus)) {
		spin_unlock_bh(&idm->lock);
		return -EBUSY;
	}
	idm->tx_skb[cursor] = skb;
	idm->tx[cursor].w0 = cpu_to_le32(bus);
	idm->tx[cursor].w1 = cpu_to_le32(len |
			FIELD_PREP(ZX_IDM_TX_PORT, w->idm_off) | ZX_IDM_TX_VALID);
	wmb();
	idm->tx_queued++;
	zx_idm_tx_kick(idm);
	idm->inject_ok++;
	/* nobody else may poll: with no wired traffic the napi never fires
	 * on its own, and the TX completion (and the RX rings) need it
	 */
	napi_schedule(&idm->adapter->napi);
	spin_unlock_bh(&idm->lock);
	return 0;
}

/* ---- RX --------------------------------------------------------------- */

/* Deliver fabric-forwarded frames to their wireless netdev.  The completion
 * count from IDM+0x4c bounds the walk: the engine fills descriptors in
 * submission order.
 */
static int zx_idm_rx_ring(struct zx_idm *idm, int budget)
{
	int work = 0;

	while (work < budget) {
		u32 pos = idm->rx_pos % ZX_IDM_RX_DESCS;
		u32 w1 = le32_to_cpu(idm->rx[pos].w1);
		u32 len = FIELD_GET(ZX_IDM_RX_LEN, w1);
		u32 chan = FIELD_GET(ZX_IDM_RX_PORT, w1);
		struct zx_idm_wifi *w = NULL;
		struct sk_buff *skb;
		int i;

		if (!w1)
			return work;	/* hardware has not filled it yet */

		idm->rx_pos++;

		for (i = 0; i < idm->wifi_count; i++)
			if (idm->wifi[i].idm_off == chan && idm->wifi[i].dev) {
				w = &idm->wifi[i];
				break;
			}

		if (!w) {
			idm->rx_novap++;
		} else if (len < ETH_HLEN ||
			   len > ZX_IDM_BUF_SIZE - ZX_IDM_BUF_HEAD) {
			idm->rx_dropped++;
		} else {
			/* copy the frame out of the coherent arena: the
			 * buffer is hardware-owned and stays submitted */
			skb = napi_alloc_skb(&idm->adapter->napi, len);
			if (!skb) {
				idm->rx_dropped++;
			} else {
				skb_put_data(skb,
					     zx_idm_buf_ptr(idm, pos) + ZX_IDM_BUF_HEAD,
					     len);
				skb->dev = w->dev;
				skb->pkt_type = PACKET_HOST;
				idm->rx_ch[chan & 7]++;
				/* the frame is already routed (the packet
				 * modifier rewrote the headers); hand it
				 * straight to the VAP's transmit path,
				 * past netfilter
				 */
				if (is_unicast_ether_addr(eth_hdr(skb)->h_dest))
					idm->rx_unicast++;
				dev_queue_xmit(skb);
				idm->rx_frames++;
				work++;
			}
		}

		/* resubmit the slot's own buffer */
		zx_idm_rx_submit(idm, pos, zx_idm_buf_dma(idm, pos));
	}
	return work;
}

int zx_idm_poll(struct zx_eth_adapter *adapter, int budget)
{
	struct zx_idm *idm = &zx_idm;
	u32 done;
	int work;

	if (!idm->ready || adapter != idm->adapter)
		return 0;

	spin_lock_bh(&idm->lock);
	zx_idm_tx_reclaim(idm);
	done = readl(idm->base + ZX_IDM_RXDONE) & 0xffff;
	work = done ? zx_idm_rx_ring(idm, min_t(u32, budget, done)) : 0;
	spin_unlock_bh(&idm->lock);
	return work;
}

/* ---- The wireless netdev map ------------------------------------------ */

/* Parse the device naming: OpenWrt's phy0-apN (2.4 GHz VAP N) and phy1-apN
 * (5 GHz), or the vendor's wlanN / wlan5gN.  Returns 0 with the entry
 * filled in.
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
 * nf_flow_table ingress hook (kernel patch 036).  Returns NF_STOLEN when
 * the frame was queued into the fabric, NF_ACCEPT to fall back to the
 * software flowtable path.
 */
/*
 * Whether the flowtable hook (patch 036) should offer this ingress device
 * to the engine: a registered VAP, or a bridge that has VAPs behind it
 * (bridged wireless clients enter the stack on the bridge master, where
 * ieee80211_ptr is NULL - the first revision's condition never fired for
 * them).
 */
bool zx_idm_hook_wlan_ingress(const struct net_device *in)
{
	int i;

	if (!zx_idm.ready || !zx_idm.wifi_count)
		return false;
	for (i = 0; i < zx_idm.wifi_count; i++)
		if (zx_idm.wifi[i].dev == in)
			return true;
	return netif_is_bridge_master(in);
}
EXPORT_SYMBOL_GPL(zx_idm_hook_wlan_ingress);

int zx_idm_flow_xmit(struct sk_buff *skb, struct nf_flowtable *ft,
		     struct flow_offload *flow)
{
	struct zx_idm_wifi *w = NULL;
	struct net_device *port;
	int i;

	zx_idm_hook_fired++;
	if (!zx_idm.ready)
		return NF_ACCEPT;

	for (i = 0; i < zx_idm.wifi_count; i++)
		if (zx_idm.wifi[i].dev == skb->dev) {
			w = &zx_idm.wifi[i];
			break;
		}
	if (!w) {
		/* bridge ingress: the source MAC's FDB port names the VAP
		 * the frame came from; without it the fabric still matches
		 * the flow (the key is the 5-tuple), so fall back to the
		 * first registered channel */
		if (!IS_ENABLED(CONFIG_BRIDGE) ||
		    !netif_is_bridge_master(skb->dev))
			return NF_ACCEPT;
		rtnl_lock();
		port = br_fdb_find_port(skb->dev, eth_hdr(skb)->h_source, 0);
		rtnl_unlock();
		for (i = 0; i < zx_idm.wifi_count; i++)
			if (zx_idm.wifi[i].dev == port) {
				w = &zx_idm.wifi[i];
				break;
			}
		if (!w)
			w = &zx_idm.wifi[0];
	}

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
 * wireless netdev itself or the bridge, in which case the kernel bridge FDB
 * tells us which VAP holds the destination MAC.
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
		/* br_fdb_find_port() wants RTNL (it asserts on 6.18); the
		 * flow-offload add path runs sleepable in a work queue */
		rtnl_lock();
		br = netdev_master_upper_dev_get_rcu((struct net_device *)
						     zx_idm.wifi[0].dev);
		if (!br || br != odev) {
			rtnl_unlock();
			return -EOPNOTSUPP;
		}
		port = br_fdb_find_port(odev, dst, 0);
		rtnl_unlock();
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

	return zx_wifi_egress_low ? 8 + w->idm_off : w->port;
}

/* ---- Bring-up and teardown -------------------------------------------- */

/*
 * Bring-up, transcribed from the vendor tm_pon_npp_init/pon_npp_idm_init
 * (plat-zxylzb_9128S.ko): NPP global masks first, then the engine params,
 * the enable bits, the ring bases (one 24 KiB coherent area: RX 2048 x 8B
 * at +0, TX 1024 x 8B at +0x4000), a buffer submission for every RX
 * descriptor, then the depth register.  The VAP ports 0x18-0x1f need no
 * register: the fabric decodes the classifier egress value straight to the
 * IDM.  The GREG/SIPC bits open the NPP side (see zx_idm_fabric_init).
 */
static void zx_idm_fabric_init(struct zx_eth_adapter *adapter)
{
	void __iomem *npp = adapter->base;
	void __iomem *idm = zx_idm.base;
	void __iomem *pp = adapter->base + 0x380000;	/* PP block 0x92380000 */
	u32 mask;
	int p;

	/* NPP global masks and enables (tm_pon_npp_init) */
	writel(0x00ffffff, npp + ZX_NPP_MASK0);
	writel(0x000fffff, npp + ZX_NPP_MASK1);
	writel(0xffffffff, npp + ZX_NPP_INTEN);
	writel(0, npp + ZX_NPP_CFG48);
	writel(readl(npp + ZX_NPP_CFG40) | 0x300, npp + ZX_NPP_CFG40);
	writel(128, npp + ZX_NPP_PPB);

	/* engine params (pon_npp_idm_init) */
	writel(31, idm + ZX_IDM_CFG24);
	writel(64, idm + ZX_IDM_CFG18);
	writel(5000, idm + ZX_IDM_CFG1C);
	writel(64, idm + ZX_IDM_CFG10);
	writel(readl(idm + ZX_IDM_CFG00) | 0x000f0000, idm + ZX_IDM_CFG00);
	writel((readl(idm + ZX_IDM_CFG00) & ~0x0ff00000) | 0x02000000,
	       idm + ZX_IDM_CFG00);
	writel(0, idm + ZX_IDM_CFG40);	/* ring is 32-byte aligned */

	writel(1600, pp + 0x20040);	/* pon_pp misc, vendor value 1600 */

	/* the GREG/SIPC side (greg_initial/sipc_initial) */
	for (p = 0; p <= 5; p++) {
		mask = 1u << p;
		writel(readl(npp + ZX_NPP_GREG_AUTHEN) & ~mask,
		       npp + ZX_NPP_GREG_AUTHEN);
		writel(readl(npp + ZX_NPP_GREG_CLOSED) & ~mask,
		       npp + ZX_NPP_GREG_CLOSED);
	}
	writel(readl(npp + ZX_NPP_GREG_AUTHEN) & ~BIT(6),
	       npp + ZX_NPP_GREG_AUTHEN);
	writel(readl(npp + ZX_NPP_GREG_CLOSED) & ~BIT(6),
	       npp + ZX_NPP_GREG_CLOSED);
	writel(0, npp + ZX_NPP_WIFI_Q1_PROTO);
	writel(0, npp + ZX_NPP_WIFI_Q1_PROTO + 4);
	writel(readl(npp + ZX_SIPC_CTRL) | BIT(0), npp + ZX_SIPC_CTRL);
	writel(readl(npp + ZX_SIPC_CTRL) & ~BIT(2), npp + ZX_SIPC_CTRL);
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

	idm->ring = dmam_alloc_coherent(adapter->dev, ZX_IDM_RING_SIZE,
					&idm->ring_dma, GFP_KERNEL);
	idm->bufs = dmam_alloc_coherent(adapter->dev,
					ZX_IDM_RX_DESCS * ZX_IDM_BUF_SIZE,
					&idm->bufs_dma, GFP_KERNEL);
	if (!idm->ring || !idm->bufs)
		return -ENOMEM;
	memset(idm->ring, 0, ZX_IDM_RING_SIZE);
	idm->rx = idm->ring;
	idm->tx = idm->ring + ZX_IDM_TX_OFF;

	zx_idm_fabric_init(adapter);

	writel(idm->ring_dma + ZX_IDM_TX_OFF, idm->base + ZX_IDM_TX_BASE);
	writel(idm->ring_dma, idm->base + ZX_IDM_RX_BASE);

	/* submit one buffer per RX descriptor through IDM+0x48; the slot
	 * keeps its buffer for the driver's lifetime
	 */
	for (i = 0; i < ZX_IDM_RX_DESCS; i++)
		zx_idm_rx_submit(idm, i, zx_idm_buf_dma(idm, i) + ZX_IDM_BUF_HEAD);
	writel(ZX_IDM_RX_DESCS | 0x04000000, idm->base + ZX_IDM_DEPTH);

	idm->ready = true;
	pr_info("%s: IDM ready, RX %d x 8B + TX %d x 8B descriptors, %d KiB buffers\n",
		DRV_NAME, ZX_IDM_RX_DESCS, ZX_IDM_TX_DESCS,
		(ZX_IDM_RX_DESCS * ZX_IDM_BUF_SIZE) / 1024);
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
	for (i = 0; i < ZX_IDM_TX_DESCS; i++)
		consume_skb(idm->tx_skb[i]);
	zx_idm.adapter = NULL;
}

/* ---- dma_state diagnostics -------------------------------------------- */

void zx_idm_debug(struct seq_file *m)
{
	seq_printf(m,
		   "idm rx_frames=%u rx_dropped=%u rx_novap=%u rx_submit=%u rx_unicast=%u
"
		   "idm rx_ch=%u/%u/%u/%u/%u/%u/%u/%u
"
		   "idm inject_ok=%u inject_full=%u tx_kicks=%u tx_queued=%u hook_fired=%u
"
		   "idm wifi_count=%d egress_low=%d
",
		   zx_idm.rx_frames, zx_idm.rx_dropped, zx_idm.rx_novap,
		   zx_idm.rx_submit, zx_idm.rx_unicast,
		   zx_idm.rx_ch[0], zx_idm.rx_ch[1], zx_idm.rx_ch[2], zx_idm.rx_ch[3],
		   zx_idm.rx_ch[4], zx_idm.rx_ch[5], zx_idm.rx_ch[6], zx_idm.rx_ch[7],
		   zx_idm.inject_ok, zx_idm.inject_full, zx_idm.tx_kicks,
		   zx_idm.tx_queued, zx_idm.hook_fired, zx_idm.wifi_count,
		   zx_wifi_egress_low);
}
