/*
 * Copyright (C) 2012-2014 Luigi Rizzo. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * $Id: if_igb_netmap.h 10878 2012-04-12 22:28:48Z luigi $
 *
 * netmap support for: igb (linux version)
 * For details on netmap support please see ixgbe_netmap.h
 */


#include <bsd_glue.h>
#include <net/netmap.h>
#include <netmap/netmap_kern.h>

static struct netmap_adapter *na_arr[4];
static unsigned int na_arr_used = 0;

#define SOFTC_T	igb_adapter

#define igb_driver_name netmap_igb_driver_name
char netmap_igb_driver_name[] = "igb" NETMAP_LINUX_DRIVER_SUFFIX;

/*
 * Adapt to different versions of the driver.
 * E1000_TX_DESC_ADV etc. have dropped the _ADV suffix at some point.
 * Also the first argument is now a pointer not the object.
 */
#ifdef NETMAP_LINUX_HAVE_IGB_RD32
#define READ_TDH(_adapter, _txr)	igb_rd32(&(_adapter)->hw, E1000_TDH((_txr)->reg_idx))
#elif defined(E1000_READ_REG)
#define READ_TDH(_adapter, _txr)	E1000_READ_REG(&(_adapter)->hw, E1000_TDH((_txr)->reg_idx))
#elif defined rd32
static inline u32 READ_TDH(struct igb_adapter *adapter, struct igb_ring *txr)
{
	struct e1000_hw *hw = &adapter->hw;
	return rd32(E1000_TDH(txr->reg_idx));
}
#else
#define	READ_TDH(_adapter, _txr)	readl((_txr)->head)
#endif

#ifndef E1000_TX_DESC_ADV
#define	E1000_TX_DESC_ADV(_r, _i)	IGB_TX_DESC(&(_r), _i)
#define	E1000_RX_DESC_ADV(_r, _i)	IGB_RX_DESC(&(_r), _i)
#else /* up to 3.2, approximately */
#define	igb_tx_buffer			igb_buffer
#define	tx_buffer_info			buffer_info
#define	igb_rx_buffer			igb_buffer
#define	rx_buffer_info			buffer_info
#endif


/*
 * Register/unregister. We are already under netmap lock.
 * Only called on the first register or the last unregister.
 */
static int
igb_netmap_reg(struct netmap_adapter *na, int onoff)
{
	struct ifnet *ifp = na->ifp;
	struct SOFTC_T *adapter = netdev_priv(ifp);

	/* protect against other reinit */
	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		usleep_range(1000, 2000);

	if ((na->kopen_flags & KOPEN_FLG_OPENED_IN_KERNEL)
			&& !(na->kopen_flags & KOPEN_FLG_OPENED_IN_USER)) {
		/* Do this only when opened in kernel only. */
		if (onoff) {
			nm_set_native_flags(na);
		} else {
			nm_clear_native_flags(na);
		}
		clear_bit(__IGB_RESETTING, &adapter->state);
		return (0);
	}

	if (netif_running(adapter->netdev))
		igb_down(adapter);

	/* enable or disable flags and callbacks in na and ifp */
	if (onoff) {
		nm_set_native_flags(na);
	} else {
		nm_clear_native_flags(na);
	}
	if (netif_running(adapter->netdev))
		igb_up(adapter);
	else
		igb_reset(adapter); // XXX is it needed ?

	clear_bit(__IGB_RESETTING, &adapter->state);
	return (0);
}


/*
 * Reconcile kernel and user view of the transmit ring.
 */
static int
igb_netmap_txsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct ifnet *ifp = na->ifp;
	struct netmap_ring *ring = kring->ring;
	u_int ring_nr = kring->ring_id;
	u_int nm_i;	/* index into the netmap ring */
	u_int nic_i;	/* index into the NIC ring */
	u_int n;
	u_int const lim = kring->nkr_num_slots - 1;
	u_int const head = kring->rhead;
	/* generate an interrupt approximately every half ring */
	u_int report_frequency = kring->nkr_num_slots >> 1;

	/* device-specific */
	struct SOFTC_T *adapter = netdev_priv(ifp);
	struct igb_ring* txr = adapter->tx_ring[ring_nr];

	rmb();	// XXX not in ixgbe ?

	/*
	 * First part: process new packets to send.
	 */
	if (!netif_carrier_ok(ifp)) {
		goto out;
	}

	nm_i = kring->nr_hwcur;
	if (nm_i != head) {	/* we have new packets to send */
		uint32_t olinfo_status=0;

		nic_i = netmap_idx_k2n(kring, nm_i);
		for (n = 0; nm_i != head; n++) {
			struct netmap_slot *slot = &ring->slot[nm_i];
			u_int len = slot->len;
			uint64_t paddr;
			void *addr = PNMB(na, slot, &paddr);

			/* device-specific */
			union e1000_adv_tx_desc *curr =
			    E1000_TX_DESC_ADV(*txr, nic_i);
			int flags = (slot->flags & NS_REPORT ||
				nic_i == 0 || nic_i == report_frequency) ?
				E1000_TXD_CMD_RS : 0;

			NM_CHECK_ADDR_LEN(na, addr, len);

			if (slot->flags & NS_BUF_CHANGED) {
				/* buffer has changed, reload map */
				// netmap_reload_map(pdev, DMA_TO_DEVICE, old_paddr, addr);
			}
			slot->flags &= ~(NS_REPORT | NS_BUF_CHANGED);
			netmap_sync_map(na, (bus_dma_tag_t) na->pdev, &paddr, len, NR_TX);

			/* Fill the slot in the NIC ring. */
			curr->read.buffer_addr = htole64(paddr);
			// XXX check olinfo and cmd_type_len
			curr->read.olinfo_status =
			    htole32(olinfo_status |
                                (len<< E1000_ADVTXD_PAYLEN_SHIFT));
			curr->read.cmd_type_len = htole32(len | flags |
				E1000_ADVTXD_DTYP_DATA | E1000_ADVTXD_DCMD_DEXT |
				E1000_ADVTXD_DCMD_IFCS | E1000_TXD_CMD_EOP);
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		kring->nr_hwcur = head;

		wmb();	/* synchronize writes to the NIC ring */

		/* (re)start the tx unit up to slot nic_i (excluded) */
		txr->next_to_use = nic_i;
		writel(nic_i, txr->tail);
		mmiowb(); // XXX why do we need this ?
	}

	/*
	 * Second part: reclaim buffers for completed transmissions.
	 */
	if (flags & NAF_FORCE_RECLAIM || nm_kr_txempty(kring)) {
		/* record completed transmissions using TDH */
		nic_i = READ_TDH(adapter, txr);
		if (nic_i >= kring->nkr_num_slots) { /* XXX can it happen ? */
			D("TDH wrap %d", nic_i);
			nic_i -= kring->nkr_num_slots;
		}
		kring->nr_hwtail = nm_prev(netmap_idx_n2k(kring, nic_i), lim);
	}
out:

	return 0;
}


/*
 * Reconcile kernel and user view of the receive ring.
 */
static int
igb_netmap_rxsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct netmap_kring *krings = NMR(na, NR_RX);
	u_int i = kring - krings;
	struct netmap_adapter *na_bound = na->na_bound;
	struct netmap_kring *krings_bound =
		na_bound != NULL ? NMR(na_bound, NR_TX) : NULL;
	struct netmap_kring *kring_tx =
		krings_bound != NULL ? krings_bound + i : NULL;
	struct ifnet *ifp = na->ifp;
	struct netmap_ring *ring = kring->ring;
	struct netmap_ring *ring_tx = kring_tx != NULL ? kring_tx->ring : NULL;
	u_int ring_nr = kring->ring_id;
	u_int nm_i;	/* index into the netmap ring */
	u_int nic_i;	/* index into the NIC ring */
	u_int n;
	u_int const lim = kring->nkr_num_slots - 1;
	u_int const head = kring->rhead;
	int force_update = (flags & NAF_FORCE_READ) || kring->nr_kflags & NKR_PENDINTR;

	/* device-specific */
	struct SOFTC_T *adapter = netdev_priv(ifp);
	struct igb_ring *rxr = adapter->rx_ring[ring_nr];

	if (!netif_carrier_ok(ifp))
		return 0;

	if (head > lim)
		return netmap_ring_reinit(kring);

	rmb();

	/*
	 * First part: import newly received packets.
	 */
	if (netmap_no_pendintr || force_update) {
		nic_i = rxr->next_to_clean;
		nm_i = netmap_idx_n2k(kring, nic_i);

		for (n = 0; ; n++) {
			union e1000_adv_rx_desc *curr =
					E1000_RX_DESC_ADV(*rxr, nic_i);
			uint32_t staterr = le32toh(curr->wb.upper.status_error);
			struct netmap_slot *slot = &ring->slot[nm_i];
			uint64_t paddr;

			if ((staterr & E1000_RXD_STAT_DD) == 0)
				break;
			PNMB(na, slot, &paddr);
			slot->len = le16toh(curr->wb.upper.length);
			slot->flags = 0;
			netmap_sync_map(na, (bus_dma_tag_t) na->pdev, &paddr, slot->len, NR_RX);
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		if (n) { /* update the state variables */
			rxr->next_to_clean = nic_i;
			kring->nr_hwtail = nm_i;
		}
		kring->nr_kflags &= ~NKR_PENDINTR;
	}

	/* Forward packets that userspace has released to TX queue of bound
	 * interface.
	 */
	if (ring_tx) {
		u_int cur_tx = ring_tx->cur;
		u_int tail_tx = ring_tx->tail;
		u_int lim_tx = ring_tx->num_slots - 1;

		nm_i = kring->nr_hwcur;
		if (nm_i != head) {
			for (n = 0; nm_i != head; n++) {
				struct netmap_slot *slot = &ring->slot[nm_i];
				struct netmap_slot *slot_tx = &ring_tx->slot[cur_tx];
				uint32_t tmp;

				if (cur_tx + 1 == tail_tx)
					break;
				tmp = slot_tx->buf_idx;
				slot_tx->buf_idx = slot->buf_idx;
				slot_tx->len = slot->len;
				slot_tx->flags |= NS_BUF_CHANGED;
				slot->buf_idx = tmp;
				slot->flags |= NS_BUF_CHANGED;

				nm_i = nm_next(nm_i, lim);
				cur_tx = nm_next(cur_tx, lim_tx);
			}
			if (n) {
				ring_tx->head = ring_tx->cur = cur_tx;
				netmap_ksync(na_bound->ifp, NIOCTXSYNC);
			}
		}
	}

	/*
	 * Second part: skip past packets that userspace has released.
	 */
	nm_i = kring->nr_hwcur;
	if (nm_i != head) {
		nic_i = netmap_idx_k2n(kring, nm_i);
		for (n = 0; nm_i != head; n++) {
			struct netmap_slot *slot = &ring->slot[nm_i];
			uint64_t paddr;
			void *addr = PNMB(na, slot, &paddr);
			union e1000_adv_rx_desc *curr = E1000_RX_DESC_ADV(*rxr, nic_i);

			if (addr == NETMAP_BUF_BASE(na)) /* bad buf */
				goto ring_reset;

			if (slot->flags & NS_BUF_CHANGED) {
				// netmap_reload_map(pdev, DMA_FROM_DEVICE, old_paddr, addr);
				slot->flags &= ~NS_BUF_CHANGED;
			}
			curr->read.pkt_addr = htole64(paddr);
			curr->read.hdr_addr = 0;
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		kring->nr_hwcur = head;
		wmb();
		/*
		 * IMPORTANT: we must leave one free slot in the ring,
		 * so move nic_i back by one unit
		 */
		nic_i = nm_prev(nic_i, lim);
		rxr->next_to_use = nic_i;
		writel(nic_i, rxr->tail);
	}


	return 0;

ring_reset:
	return netmap_ring_reinit(kring);
}

static int
igb_netmap_deinit_rings_internally(struct SOFTC_T *adapter)
{
	struct ifnet *ifp = adapter->netdev;
	struct netmap_adapter* na = NA(ifp);

	return netmap_kclose(na);
}

static int
igb_netmap_init_rings_internally(struct SOFTC_T *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct ifnet *ifp = adapter->netdev;
	struct netmap_adapter* na = NA(ifp);
	struct nmreq req;

	if (na->kopen_priv)
		return 0;
	memset(&req, 0, sizeof(req));
	req.nr_ringid = 0;
	req.nr_flags &= ~NR_REG_MASK;
	req.nr_flags |= NR_REG_ONE_NIC;
	netmap_kopen(netdev->name, &req, 0);

	return 0;
}

static int
igb_netmap_configure_tx_ring(struct SOFTC_T *adapter, int ring_nr)
{
	struct ifnet *ifp = adapter->netdev;
	struct netmap_adapter* na = NA(ifp);
	struct netmap_slot* slot;
	struct igb_ring *txr = adapter->tx_ring[ring_nr];
	int i, si;
	void *addr;
	uint64_t paddr;

        slot = netmap_reset(na, NR_TX, ring_nr, 0);
	if (!slot)
		return 0;  // not in netmap native mode
	for (i = 0; i < na->num_tx_desc; i++) {
		union e1000_adv_tx_desc *tx_desc;
		si = netmap_idx_n2k(&na->tx_rings[ring_nr], i);
		addr = PNMB(na, slot + si, &paddr);
		tx_desc = E1000_TX_DESC_ADV(*txr, i);
		tx_desc->read.buffer_addr = htole64(paddr);
		/* actually we don't care to init the rings here */
	}
	return 1;	// success
}


static int
igb_netmap_configure_rx_ring(struct igb_ring *rxr)
{
	struct ifnet *ifp = rxr->netdev;
	struct netmap_adapter* na = NA(ifp);
	int reg_idx = rxr->reg_idx;
	struct netmap_slot* slot;
	u_int i;

	/*
	 * XXX watch out, the main driver must not use
	 * split headers. The buffer len should be written
	 * into wr32(E1000_SRRCTL(reg_idx), srrctl) with options
	 * something like
	 *	srrctl = ALIGN(buffer_len, 1024) >>
	 *		E1000_SRRCTL_BSIZEPKT_SHIFT;
	 *	srrctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
	 *	srrctl |= E1000_SRRCTL_DROP_EN;
	 */
        slot = netmap_reset(na, NR_RX, reg_idx, 0);
	if (!slot)
		return 0;	// not in native netmap mode

	for (i = 0; i < rxr->count; i++) {
		union e1000_adv_rx_desc *rx_desc;
		uint64_t paddr;
		int si = netmap_idx_n2k(&na->rx_rings[reg_idx], i);

#if 0
		// XXX the skb check can go away
		struct igb_rx_buffer *bi = &rxr->rx_buffer_info[i];
		if (bi->skb)
			D("rx buf %d was set", i);
		bi->skb = NULL; // XXX leak if set
#endif /* useless */

		PNMB(na, slot + si, &paddr);
		rx_desc = E1000_RX_DESC_ADV(*rxr, i);
		rx_desc->read.hdr_addr = 0;
		rx_desc->read.pkt_addr = htole64(paddr);
	}
	/* preserve buffers already made available to clients */
	i = rxr->count - 1 - nm_kr_rxspace(&na->rx_rings[reg_idx]);

	wmb();	/* Force memory writes to complete */
	ND("%s rxr%d.tail %d", na->name, reg_idx, i);
	rxr->next_to_use = i;
	writel(i, rxr->tail);
	return 1;	// success
}

static unsigned int igb_netmap_opened_in_kernel_only(struct SOFTC_T *adapter)
{
	struct ifnet *ifp = adapter->netdev;
	struct netmap_adapter* na = NA(ifp);

	return ((na->kopen_flags & KOPEN_FLG_OPENED_IN_KERNEL)
		&& !(na->kopen_flags & KOPEN_FLG_OPENED_IN_USER));
}

static void
igb_netmap_attach(struct SOFTC_T *adapter)
{
	struct netmap_adapter na;
	struct netmap_adapter* na_real;
	struct netmap_adapter* na_bound;

	bzero(&na, sizeof(na));

	na.ifp = adapter->netdev;
	na.pdev = &adapter->pdev->dev;
	na.num_tx_desc = adapter->tx_ring_count;
	na.num_rx_desc = adapter->rx_ring_count;
	na.nm_register = igb_netmap_reg;
	na.nm_txsync = igb_netmap_txsync;
	na.nm_rxsync = igb_netmap_rxsync;
	na.num_tx_rings = adapter->num_tx_queues;
	na.num_rx_rings = adapter->num_rx_queues;
	netmap_attach(&na);

	if (adapter->packet_switching_enable)
	{
		na_real = NA(na.ifp);
		na_arr[na_arr_used] = na_real;
		if (na_arr_used & 0x1)
		{
			na_bound = na_arr[na_arr_used - 1];
			na_bound->na_bound = na_real;
			na_real->na_bound = na_bound;
		}
		na_arr_used++;
	}
}

/* end of file */
