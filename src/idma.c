/* src/idma.c - low level rx and tx management
 *
 * --------------------------------------------------------------------
 *
 * Copyright (C) 2003  ACX100 Open Source Project
 *
 *   The contents of this file are subject to the Mozilla Public
 *   License Version 1.1 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.mozilla.org/MPL/
 *
 *   Software distributed under the License is distributed on an "AS
 *   IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 *   implied. See the License for the specific language governing
 *   rights and limitations under the License.
 *
 *   Alternatively, the contents of this file may be used under the
 *   terms of the GNU Public License version 2 (the "GPL"), in which
 *   case the provisions of the GPL are applicable instead of the
 *   above.  If you wish to allow the use of your version of this file
 *   only under the terms of the GPL and not to allow others to use
 *   your version of this file under the MPL, indicate your decision
 *   by deleting the provisions above and replace them with the notice
 *   and other provisions required by the GPL.  If you do not delete
 *   the provisions above, a recipient may use your version of this
 *   file under either the MPL or the GPL.
 *
 * --------------------------------------------------------------------
 *
 * Inquiries regarding the ACX100 Open Source Project can be
 * made directly to:
 *
 * acx100-users@lists.sf.net
 * http://acx100.sf.net
 *
 * --------------------------------------------------------------------
 */

#include <linux/config.h>
#include <linux/version.h>

#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>
#if WIRELESS_EXT > 12
#include <net/iw_handler.h>
#endif /* WE > 12 */

#include <wlan_compat.h>

#include <linux/pci.h>



/*================================================================*/
/* Project Includes */

#include <p80211hdr.h>
#include <p80211mgmt.h>
#include <acx100_conv.h>
#include <acx100.h>
#include <p80211types.h>
#include <acx100_helper.h>
#include <acx100_helper2.h>
#include <idma.h>
#include <ihw.h>
#include <monitor.h>

/* these used to be increased to 32 each,
 * but several people had memory allocation issues,
 * so back to 16 again... */
#if (WLAN_HOSTIF==WLAN_USB)
#define TXBUFFERNO 10
#define RXBUFFERNO 10
#else
#define RXBUFFERNO 16
#define TXBUFFERNO 16
#endif

#define MINFREE_TX 3
spinlock_t tx_lock;
spinlock_t rx_lock;

#ifdef ACX_DEBUG
#if (WLAN_HOSTIF==WLAN_USB)
extern void acx100usb_dump_bytes(void *,int);
#endif
#endif
#if (WLAN_HOSTIF==WLAN_USB)
extern void acx100usb_tx_data(wlandevice_t *,void *);
#endif


/*----------------------------------------------------------------
* acx100_free_desc_queues
*
*	Releases the queues that have been allocated, the
*	others have been initialised to NULL in acx100.c so this
*	function can be used if only part of the queues were
*	allocated.
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_free_desc_queues(TIWLAN_DC *pDc)
{
	FN_ENTER;

	if (NULL != pDc->pTxHostDescQPool) {
		pci_free_consistent(0,
				    pDc->TxHostDescQPoolSize,
				    pDc->pTxHostDescQPool,
				    pDc->TxHostDescQPoolPhyAddr);
		pDc->pTxHostDescQPool = NULL;
		pDc->TxHostDescQPoolSize = 0;
	}

	if (NULL != pDc->pFrameHdrQPool) {
#if (WLAN_HOSTIF!=WLAN_USB)
		pci_free_consistent(0,
				    pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
#else
		kfree(pDc->pFrameHdrQPool);
#endif
		pDc->pFrameHdrQPool = NULL;
		pDc->FrameHdrQPoolSize = 0;
	}
	
	if (NULL != pDc->pTxBufferPool) {
		pci_free_consistent(0,
				    pDc->TxBufferPoolSize,
				    pDc->pTxBufferPool,
				    pDc->TxBufferPoolPhyAddr);
		pDc->pTxBufferPool = NULL;
		pDc->TxBufferPoolSize = 0;
	}

	pDc->pTxDescQPool = NULL;
	pDc->tx_pool_count = 0;


	if (NULL != pDc->pRxHostDescQPool) {
		pci_free_consistent(0,
				    pDc->RxHostDescQPoolSize,
				    pDc->pRxHostDescQPool,
				    pDc->RxHostDescQPoolPhyAddr);
		pDc->pRxHostDescQPool = NULL;
		pDc->RxHostDescQPoolSize = 0;
	}
	if (NULL != pDc->pRxBufferPool) {
		pci_free_consistent(0,
				    pDc->RxBufferPoolSize,
				    pDc->pRxBufferPool,
				    pDc->RxBufferPoolPhyAddr);
		pDc->pRxBufferPool = NULL;
		pDc->RxBufferPoolSize = 0;
	}

	pDc->pRxDescQPool = NULL;
	pDc->rx_pool_count = 0;

	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_delete_dma_regions
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_delete_dma_regions(wlandevice_t *priv)
{
	FN_ENTER;
#if (WLAN_HOSTIF!=WLAN_USB)
	/* disable radio Tx/Rx. Shouldn't we use the firmware commands
	 * here instead? Or are we that much down the road that it's no
	 * longer possible here? */
	acx100_write_reg16(priv, priv->io[IO_ACX_ENABLE], 0);

	/* used to be a for loop 1000, do scheduled delay instead */
	acx100_schedule(HZ / 10);
#endif
	acx100_free_desc_queues(&priv->dc);

	FN_EXIT(0, 0);
	return 0;
}

/*----------------------------------------------------------------
* acx100_create_dma_regions
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_create_dma_regions(wlandevice_t *priv)
{
	QueueConfig_t qcfg;
	
	acx100_memmap_t MemMap;
	struct TIWLAN_DC *pDc;

	FN_ENTER;

	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);

	pDc = &priv->dc;
	pDc->priv = priv;

	/* read out the acx100 physical start address for the queues */
	if (!acx100_interrogate(priv, &MemMap, ACX1xx_IE_MEMORY_MAP)) {
		acxlog(L_BINSTD, "ctlMemoryMapRead returns error\n");
		FN_EXIT(1, 2);
		return 2;
	}

	/* # of items in Rx and Tx queues */
	priv->TxQueueNo = TXBUFFERNO;
	priv->RxQueueNo = RXBUFFERNO;

	/* calculate size of queues */
	qcfg.AreaSize = cpu_to_le32(
			(sizeof(struct txdescriptor) * TXBUFFERNO +
	                 sizeof(struct rxdescriptor) * RXBUFFERNO + 8)
			);
	qcfg.NumTxQueues = 1;  /* number of tx queues */

	/* sets the beginning of the tx descriptor queue */
	pDc->ui32ACXTxQueueStart = le32_to_cpu(MemMap.QueueStart);
	qcfg.TxQueueStart = cpu_to_le32(pDc->ui32ACXTxQueueStart);
	qcfg.TxQueuePri = 0;

#if (WLAN_HOSTIF==WLAN_USB)
	qcfg.NumTxDesc = TXBUFFERNO;
	qcfg.NumRxDesc = RXBUFFERNO;
#endif

	/* sets the beginning of the rx descriptor queue, after the tx descrs */
	pDc->ui32ACXRxQueueStart = priv->TxQueueNo * sizeof(struct txdescriptor) + le32_to_cpu(MemMap.QueueStart);
	qcfg.RxQueueStart = cpu_to_le32(pDc->ui32ACXRxQueueStart);
	qcfg.QueueOptions = 1;		/* auto reset descriptor */

	/* sets the end of the rx descriptor queue */
	qcfg.QueueEnd = cpu_to_le32(priv->RxQueueNo * sizeof(struct rxdescriptor) + pDc->ui32ACXRxQueueStart);

	/* sets the beginning of the next queue */
	qcfg.HostQueueEnd = cpu_to_le32(le32_to_cpu(qcfg.QueueEnd) + 8);

	acxlog(L_BINDEBUG, "<== Initialize the Queue Indicator\n");

	if (!acx100_configure_length(priv, &qcfg, ACX1xx_IE_QUEUE_CONFIG, sizeof(QueueConfig_t)-4)){ //0x14 + (qcfg.vale * 8))) {
		acxlog(L_BINSTD, "ctlQueueConfigurationWrite returns error\n");
		goto error;
	}

	if (acx100_create_tx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_tx_host_desc_queue returns error\n");
		goto error;
	}
	if (acx100_create_rx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue returns error\n");
		goto error;
	}

	pDc->pTxDescQPool = NULL;
	pDc->pRxDescQPool = NULL;

	acx100_create_tx_desc_queue(pDc);
	acx100_create_rx_desc_queue(pDc);
	if (!acx100_interrogate(priv, &MemMap, ACX1xx_IE_MEMORY_MAP)) {
		acxlog(L_BINSTD, "Failed to read memory map\n");
		goto error;
	}

#if (WLAN_HOSTIF==WLAN_USB)
	if (!acx100_configure(priv, &MemMap, ACX1xx_IE_MEMORY_MAP)) {
		acxlog(L_BINSTD,"Failed to write memory map\n");
		goto error;
	}
#endif

	MemMap.PoolStart = cpu_to_le32((le32_to_cpu(MemMap.QueueEnd) + 0x1f + 4) & ~0x1f);

	if (!acx100_configure(priv, &MemMap, ACX1xx_IE_MEMORY_MAP)) {
		acxlog(L_BINSTD, "ctlMemoryMapWrite returns error\n");
		goto error;
	}

	if (!acx100_init_memory_pools(priv, (acx100_memmap_t *) &MemMap)) {
		acxlog(L_BINSTD, "acx100_init_memory_pools returns error\n");
		goto error;
	}

	FN_EXIT(1, 0);
	return 0;

error:
	acxlog(0xffff, "dma error!!\n");
	acx100_schedule(60 * HZ);
	acx100_free_desc_queues(pDc);

	FN_EXIT(1, 1);
	return 1;
}


/*----------------------------------------------------------------
* acx111_create_dma_regions
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx111_create_dma_regions(wlandevice_t *priv)
{
	struct ACX111MemoryConfig memconf;
	struct ACX111QueueConfig queueconf;
	struct TIWLAN_DC *pDc;

	FN_ENTER;

	pDc = &priv->dc;
	pDc->priv = priv;

	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);

	/* ### Calculate memory positions and queue sizes #### */

	priv->TxQueueNo = TXBUFFERNO;
	priv->RxQueueNo = RXBUFFERNO;

	acxlog(L_BINDEBUG, "<== Initialize the Queue Indicator\n");


	/* ### Set up the card #### */

	memset(&memconf, 0, sizeof(memconf));

	/* hm, I hope this is correct */
	memconf.no_of_stations = cpu_to_le16(1);

	/* specify the memory block size. Default is 256 */
	memconf.memory_block_size = cpu_to_le16(priv->memblocksize); 	

	/* let's use 50%/50% for tx/rx */
	memconf.tx_rx_memory_block_allocation = 10;


	/* set the count of our queues */
	memconf.count_rx_queues = 1; /* TODO place this in constants */
	memconf.count_tx_queues = 1;

	if(memconf.count_rx_queues != 1 || memconf.count_tx_queues != 1) {
		acxlog(L_STD, 
			"%s: Requested more queues than supported.  Please adapt the structure! rxbuffers:%d txbuffers:%d\n", 
			__func__, memconf.count_rx_queues, memconf.count_tx_queues);
		goto error;
	}

	/* uhoh, hope this is correct-> BusMaster Indirect Memory Organization = 0 */
	/* other option is to set to Generic Bus Slave = 2 */
	memconf.options = 0;

	/* let's use 25% for fragmentations and 75% for frame transfers */
	memconf.fragmentation = 0x0f; 

	// Set up our host descriptor pool + data pool
	if (acx111_create_tx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx111_create_tx_host_desc_queue returns error\n");
		goto error;
	}
	if (acx100_create_rx_host_desc_queue(pDc)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue returns error\n");
		goto error;
	}

	/* RX descriptor queue config */
	memconf.rx_queue1_count_descs = RXBUFFERNO;
	memconf.rx_queue1_reserved2 = 7; /* must be set to 7 */
	memconf.rx_queue1_host_rx_start = pDc->RxHostDescQPoolPhyAddr;

	/* TX descriptor queue config */
	memconf.tx_queue1_count_descs = TXBUFFERNO;
	memconf.tx_queue1_attributes = 15; /* highest priority */

	acxlog(L_STD, "%s: set up acx111 queue memory configuration (queue configs + descriptors)\n", __func__);
	if (acx100_configure(priv, &memconf, ACX1xx_IE_QUEUE_CONFIG /*0x03*/) == 0) {
		acxlog(L_STD, "setting up the memory configuration failed!\n");
		goto error;
	}

	/* read out queueconfig */
	memset(&queueconf, 0xff, sizeof(queueconf));

	if (!acx100_interrogate(priv, &queueconf, ACX1xx_IE_MEMORY_CONFIG_OPTIONS /*0x05*/)) {
		acxlog(L_BINSTD, "read queuehead returns error\n");
	}

	acxlog(L_BINSTD, "dump queue head:\n");
	acxlog(L_BINSTD, "length: %d\n", queueconf.len);
	acxlog(L_BINSTD, "tx_memory_block_address (from card): %X\n", queueconf.tx_memory_block_address);
	acxlog(L_BINSTD, "rx_memory_block_address (from card): %X\n", queueconf.rx_memory_block_address);

	acxlog(L_BINSTD, "rx1_queue address (from card): %X\n", queueconf.rx1_queue_address);
	acxlog(L_BINSTD, "tx1_queue address (from card): %X\n", queueconf.tx1_queue_address);

	pDc->pTxDescQPool = (struct txdescriptor *) (priv->iobase2 +
				     queueconf.tx1_queue_address);
	pDc->ui32ACXTxQueueStart = queueconf.tx1_queue_address;

	pDc->pRxDescQPool = (struct rxdescriptor *) (priv->iobase2 +
				     queueconf.rx1_queue_address);
	pDc->ui32ACXRxQueueStart = queueconf.rx1_queue_address;

	acx100_create_tx_desc_queue(pDc);
	acx100_create_rx_desc_queue(pDc);

	FN_EXIT(1, 0);
	return 0;

error:
	acx100_free_desc_queues(pDc);

	FN_EXIT(1, 1);
	return 1;
}


/*----------------------------------------------------------------
* acx100_dma_tx_data
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_dma_tx_data(wlandevice_t *priv, struct txdescriptor *tx_desc)
{
	struct txhostdescriptor *header;
	struct txhostdescriptor *payload;
#if (WLAN_HOSTIF!=WLAN_USB)
	unsigned long flags;
	int i;
#endif

	FN_ENTER;
	/* header and payload are located in adjacent descriptors */
	header = tx_desc->host_desc;
	payload = tx_desc->host_desc + 1;

	/* let chip do RTS/CTS handshaking before sending
	 * in case packet size exceeds threshold */
	if (tx_desc->total_length > priv->rts_threshold)
		tx_desc->Ctl2 |= DESC_CTL2_RTS;

	/* set rate */
	if (WLAN_GET_FC_FTYPE(((p80211_hdr_t*)header->data)->a3.fc) == WLAN_FTYPE_MGMT) {
		tx_desc->rate = 20;	/* 2Mbps for MGMT pkt compatibility */
	} else {
		tx_desc->rate = priv->txrate_curr;
	}

#if (WLAN_HOSTIF!=WLAN_USB)
	/* need to protect access to Ctl field */
	spin_lock_irqsave(&tx_lock, flags);
#endif

	if (CHIPTYPE_ACX100 == priv->chip_type) {
		if (1 == priv->preamble_flag)
			tx_desc->Ctl |= ACX100_CTL_PREAMBLE; /* set Preamble */
		/* It seems as if the Preamble setting was actually REVERSED:
		 * bit 0 should most likely actually be ACTIVATED
		 * for Short Preamble, not the other way around as before!
		 * This caused many Tx error 0x20 errors with APs
		 * that don't support Short Preamble, since we were
		 * thinking we are setting Long Preamble, when in fact
		 * it was Short Preamble.
		 * The flag reversal theory has been sort of confirmed
		 * by throughput measurements:
		 * ~ 680K/s with flag disabled
		 * ~ 760K/s with flag enabled
		 */

		/* set autodma and reclaim and 1st mpdu */
		tx_desc->Ctl |= ACX100_CTL_AUTODMA | ACX100_CTL_RECLAIM | ACX100_CTL_FIRSTFRAG;
	} else if(priv->chip_type == CHIPTYPE_ACX111) {
		/* ACX111 */
		/* TODO Find a final solution */
		tx_desc->Ctl = 0;
		tx_desc->rate = 3;
		
		if(header->length != sizeof(struct framehdr)) {
			acxlog(L_DATA, "UHOH, packet has a different length than struct framehdr (0x%X vs. 0x%X)\n", sizeof(struct framehdr), header->length );
			
			/* copy the data at the right place */
			memcpy(header->data + header->length, payload->data, payload->length);
		}
		
		header->length += payload->length;
	}

#if (WLAN_HOSTIF!=WLAN_USB)
	/* sets Ctl ACX100_CTL_OWN to zero telling that the descriptors are now owned by the acx100 */
	header->Ctl &= (UINT16) ~ACX100_CTL_OWN;
	payload->Ctl &= (UINT16) ~ACX100_CTL_OWN;
	tx_desc->Ctl &= (UINT16) ~ACX100_CTL_OWN;
	spin_unlock_irqrestore(&tx_lock, flags);

	tx_desc->tx_time = jiffies;
	acx100_write_reg16(priv, priv->io[IO_ACX_INT_TRIG], 0x4);
#else
	tx_desc->tx_time = jiffies;
	acx100usb_tx_data(priv, tx_desc);
#endif
	/* log the packet content AFTER sending it,
	 * in order to not delay sending any further than absolutely needed */
	acxlog(L_XFER | L_DATA,
		"Tx pkt (%s): len %i hdr_len %i pyld_len %i mode %d status %d\n",
		acx100_get_packet_type_string(((p80211_hdr_t*)header->data)->a3.fc),
		tx_desc->total_length,
		header->length,
		payload->length,
		priv->macmode_joined,
		priv->status);

	if (debug & L_DATA)
	{
		acxlog(L_DATA, "802.11 header[%d]: ", header->length);
#if (WLAN_HOSTIF==WLAN_USB)
		acx100usb_dump_bytes(header->data, header->length);
#else
		for (i = 0; i < header->length; i++)
			acxlog(L_DATA, "%02x ", ((UINT8 *) header->data)[i]);
		acxlog(L_DATA, "\n");
#endif
		acxlog(L_DATA, "802.11 payload[%d]: ", payload->length);
#if (WLAN_HOSTIF==WLAN_USB)
		acx100usb_dump_bytes(payload->data, payload->length);
#else
		for (i = 0; i < payload->length; i++)
			acxlog(L_DATA, "%02x ", ((UINT8 *) payload->data)[i]);
		acxlog(L_DATA, "\n");
#endif
	}
	FN_EXIT(0, 0);
}

void acx_handle_tx_error(wlandevice_t *priv, txdesc_t *pTxDesc)
{
	char *err = "unknown error";

	/* hmm, should we handle this as a mask
	 * of *several* bits?
	 * For now I think only caring about
	 * individual bits is ok... */
	switch(pTxDesc->error) {
		case 0x01:
			err = "no Tx due to error in other fragment";
			priv->wstats.discard.fragment++;
			break;
		case 0x02:
			err = "Tx aborted";
			priv->stats.tx_aborted_errors++;
			break;
		case 0x04:
			err = "Tx desc wrong parameters";
			priv->wstats.discard.misc++;
			break;
		case 0x08:
			err = "WEP key not found";
			priv->wstats.discard.misc++;
			break;
		case 0x10:
			err = "MSDU lifetime timeout? - change 'iwconfig retry lifetime XXX'";
			priv->wstats.discard.misc++;
			break;
		case 0x20:
			err = "excessive Tx retries due to either distance too high or unable to Tx or Tx frame error - change 'iwconfig txpower XXX' or 'sens'itivity or 'retry'";
			priv->wstats.discard.retries++;
			break;
		case 0x40:
			err = "Tx buffer overflow";
			priv->stats.tx_fifo_errors++;
			break;
		case 0x80:
			err = "DMA error";
			priv->wstats.discard.misc++;
			break;
	}
	acxlog(L_STD, "Tx error occurred (error 0x%02X)!! (%s)\n", pTxDesc->error, err);
	priv->stats.tx_errors++;

#if (WLAN_HOSTIF!=WLAN_USB)
	/* TODO critical!! is this block valid? */
	/* let card know there is work to do */
	acx100_write_reg16(priv, priv->io[IO_ACX_INT_TRIG], 0x4);
#endif

#if WIRELESS_EXT > 13 /* wireless_send_event() and IWEVTXDROP are WE13 */
	if (0x30 & pTxDesc->error) {
		/* only send IWEVTXDROP in case of retry or lifetime exceeded;
		 * all other errors mean we screwed up locally */
		union iwreq_data wrqu;
		p80211_hdr_t *hdr = (p80211_hdr_t *)pTxDesc->host_desc->data;

		MAC_COPY(wrqu.addr.sa_data, hdr->a3.a1);
		wireless_send_event(priv->netdev, IWEVTXDROP, &wrqu, NULL);
	}
#endif
}

static UINT8 txrate_auto_table[] = { (UINT8)ACX_TXRATE_1, (UINT8)ACX_TXRATE_2,
	(UINT8)ACX_TXRATE_5_5, (UINT8)ACX_TXRATE_11, (UINT8)ACX_TXRATE_22PBCC };

static inline void acx_handle_txrate_auto(wlandevice_t *priv, txdesc_t *pTxDesc)
{
	acxlog(L_DEBUG, "rate %d/%d, fallback %d/%d, stepup %d/%d\n", pTxDesc->rate, priv->txrate_curr, priv->txrate_fallback_count, priv->txrate_fallback_threshold, priv->txrate_stepup_count, priv->txrate_stepup_threshold);
	if ((pTxDesc->rate < priv->txrate_curr) || (0x0 != (pTxDesc->error & 0x30))) {
		if (++priv->txrate_fallback_count
				> priv->txrate_fallback_threshold) {
			if (priv->txrate_auto_idx > 0) {
				priv->txrate_auto_idx--;
				priv->txrate_curr = txrate_auto_table[priv->txrate_auto_idx];
				acxlog(L_XFER, "falling back to Tx rate %d.\n", priv->txrate_curr);
			}
			priv->txrate_fallback_count = 0;
		}
	}
	else
	if (pTxDesc->rate == priv->txrate_curr) {
		if (++priv->txrate_stepup_count
				> priv->txrate_stepup_threshold) {
			if (priv->txrate_auto_idx < priv->txrate_auto_idx_max) {
				priv->txrate_auto_idx++;
				priv->txrate_curr = txrate_auto_table[priv->txrate_auto_idx];
				acxlog(L_XFER, "stepping up to Tx rate %d.\n", priv->txrate_curr);
			}
			priv->txrate_stepup_count = 0;
		}
	}
}

/*----------------------------------------------------------------
* acx100_log_txbuffer
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

inline void acx100_log_txbuffer(TIWLAN_DC *pDc)
{
	unsigned int i;
	txdesc_t *pTxDesc;

	FN_ENTER;
	if (debug & L_BUF)
	{
		/* no locks here, since it's entirely non-critical code */
		pTxDesc = pDc->pTxDescQPool;
		for (i = 0; i < pDc->tx_pool_count; i++)
		{
			if ((pTxDesc->Ctl & DESC_CTL_DONE) == DESC_CTL_DONE)
				acxlog(L_BUF, "txbuf %d done\n", i);
			pTxDesc = GET_NEXT_TX_DESC_PTR(pDc, pTxDesc);
		}
	}
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_clean_tx_desc
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*	This function probably resets the txdescs' status when the ACX100
*	signals the TX done IRQ (txdescs have been processed), starting with
*	the pool index of the descriptor which we would use next,
*	in order to make sure that we can be as fast as possible
*	in filling new txdescs.
*	Oops, now we have our own index, so everytime we get called we know
*	where the next packet to be cleaned is.
*	Hmm, still need to loop through the whole ring buffer now,
*	since we lost sync for some reason when ping flooding or so...
*	(somehow we don't get the IRQ for acx100_clean_tx_desc any more when
*	too many packets are being sent!)
*	FIXME: currently we only process one packet, but this gets out of
*	sync for some reason when ping flooding, so we need to loop,
*	but the previous smart loop implementation causes the ping latency
*	to rise dramatically (~3000 ms), at least on CardBus PheeNet WL-0022.
*	Dunno what to do :-\
*
*----------------------------------------------------------------*/

inline void acx100_clean_tx_desc(wlandevice_t *priv)
{
	TIWLAN_DC *pDc = &priv->dc;
	txdesc_t *pTxDesc;
	UINT finger, watch;

	FN_ENTER;

	acx100_log_txbuffer(pDc);
	acxlog(L_BUF, "cleaning up Tx bufs from %d\n", pDc->tx_tail);

	spin_lock(&tx_lock);

	finger = pDc->tx_tail;
	watch = finger;

	do {
		pTxDesc = GET_TX_DESC_PTR(pDc, finger);

		/* abort if txdesc is not marked as "Tx finished" and "owned" */
		if ((pTxDesc->Ctl & DESC_CTL_DONE) != DESC_CTL_DONE)
			break;

		/* ok, need to clean / handle this descriptor */
		if (unlikely(0 != pTxDesc->error))
			acx_handle_tx_error(priv, pTxDesc);

		if (1 == priv->txrate_auto)
			acx_handle_txrate_auto(priv, pTxDesc);

		/* free it */
		pTxDesc->Ctl = ACX100_CTL_OWN;
			
		priv->TxQueueFree++;

		if ((priv->TxQueueFree >= MINFREE_TX + 3)
		&& (priv->status == ISTATUS_4_ASSOCIATED)
		&& (netif_queue_stopped(priv->netdev)))
		{
			/* FIXME: if construct is ugly:
			 * should have functions acx100_stop_queue
			 * etc. which set flag priv->tx_stopped
			 * to be checked here. */
			acxlog(L_XFER, "wake queue (avail. Tx desc %d).\n", priv->TxQueueFree);
			netif_wake_queue(priv->netdev);
		}
		/* log AFTER having done the work, faster */
		acxlog(L_BUF, "cleaned %d\n", finger);

		/* update pointer for descr to be cleaned next */
		finger = (finger + 1) % pDc->tx_pool_count;
	} while (watch != finger);

	/* remember last position */
	pDc->tx_tail = finger;

	spin_unlock(&tx_lock);

	FN_EXIT(0, 0);
	return;
}
/*----------------------------------------------------------------
* acx100_rxmonitor
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_rxmonitor(wlandevice_t *priv, struct rxbuffer *buf)
{
	unsigned long *data = (unsigned long *)buf;
	unsigned int packet_len = data[0] & 0xFFF;
	int sig_strength = (data[1] >> 16) & 0xFF;
	int sig_quality  = (data[1] >> 24) & 0xFF;
	p80211msg_lnxind_wlansniffrm_t *msg;

	int payload_offset = 0;
	unsigned int skb_len;
	struct sk_buff *skb;
	void *datap;

	FN_ENTER;

	if (!(priv->rx_config_1 & RX_CFG1_PLUS_ADDIT_HDR))
	{
		printk("rx_config_1 misses RX_CFG1_PLUS_ADDIT_HDR\n");
		FN_EXIT(0, 0);
		return;
	}

	if (priv->rx_config_1 & RX_CFG1_PLUS_ADDIT_HDR)
	{
		payload_offset += 3*4; /* status words         */
		packet_len     += 3*4; /* length is w/o status */
	}
		
	if (priv->rx_config_1 & RX_CFG1_INCLUDE_ADDIT_HDR)
		payload_offset += 4;   /* phy header   */

	/* we are in big luck: the acx100 doesn't modify any of the fields */
	/* in the 802.11-frame. just pass this packet into the PF_PACKET-  */
	/* subsystem. yeah. */

	skb_len = packet_len - payload_offset;

	if (priv->netdev->type == ARPHRD_IEEE80211_PRISM)
		skb_len += sizeof(p80211msg_lnxind_wlansniffrm_t);

		/* sanity check */
	if (skb_len > (WLAN_HDR_A4_LEN + WLAN_DATA_MAXLEN + WLAN_CRC_LEN))
	{
		printk("monitor mode panic: oversized frame!\n");
		FN_EXIT(0, 0);
		return;
	}

		/* allocate skb */
	if ( (skb = dev_alloc_skb(skb_len)) == NULL)
	{
		printk("alloc_skb failed trying to allocate %d bytes\n", skb_len);
		FN_EXIT(0, 0);
		return;
	}

	skb_put(skb, skb_len);

		/* when in raw 802.11 mode, just copy frame as-is */
	if (priv->netdev->type == ARPHRD_IEEE80211)
		datap = skb->data;
	else  /* otherwise, emulate prism header */
	{
		msg = (p80211msg_lnxind_wlansniffrm_t*)skb->data;
		datap = msg + 1;
		
		msg->msgcode = DIDmsg_lnxind_wlansniffrm;
		msg->msglen = sizeof(p80211msg_lnxind_wlansniffrm_t);
		strcpy(msg->devname, priv->netdev->name);
		
		msg->hosttime.did = DIDmsg_lnxind_wlansniffrm_hosttime;
		msg->hosttime.status = 0;
		msg->hosttime.len = 4;
		msg->hosttime.data = jiffies;
		
		msg->mactime.did = DIDmsg_lnxind_wlansniffrm_mactime;
		msg->mactime.status = 0;
		msg->mactime.len = 4;
		msg->mactime.data = data[2];
		
		msg->channel.did = DIDmsg_lnxind_wlansniffrm_channel;
		msg->channel.status = P80211ENUM_msgitem_status_no_value;
		msg->channel.len = 4;
		msg->channel.data = 0;

		msg->rssi.did = DIDmsg_lnxind_wlansniffrm_rssi;
		msg->rssi.status = P80211ENUM_msgitem_status_no_value;
		msg->rssi.len = 4;
		msg->rssi.data = 0;

		msg->sq.did = DIDmsg_lnxind_wlansniffrm_sq;
		msg->sq.status = P80211ENUM_msgitem_status_no_value;
		msg->sq.len = 4;
		msg->sq.data = 0;

		msg->signal.did = DIDmsg_lnxind_wlansniffrm_signal;
		msg->signal.status = 0;
		msg->signal.len = 4;
		msg->signal.data = sig_quality;

		msg->noise.did = DIDmsg_lnxind_wlansniffrm_noise;
		msg->noise.status = 0;
		msg->noise.len = 4;
		msg->noise.data = sig_strength;

		msg->rate.did = DIDmsg_lnxind_wlansniffrm_rate;
		msg->rate.status = P80211ENUM_msgitem_status_no_value;
		msg->rate.len = 4;
		msg->rate.data = 0;

		msg->istx.did = DIDmsg_lnxind_wlansniffrm_istx;
		msg->istx.status = 0;
		msg->istx.len = 4;
		msg->istx.data = P80211ENUM_truth_false;

		skb_len -= sizeof(p80211msg_lnxind_wlansniffrm_t);

		msg->frmlen.did = DIDmsg_lnxind_wlansniffrm_signal;
		msg->frmlen.status = 0;
		msg->frmlen.len = 4;
		msg->frmlen.data = skb_len;
	}

	memcpy(datap, ((unsigned char*)buf)+payload_offset, skb_len);

	skb->dev = priv->netdev;
	skb->dev->last_rx = jiffies;

	skb->mac.raw = skb->data;
	skb->ip_summed = CHECKSUM_NONE;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_80211_RAW);

	priv->stats.rx_packets++;
	priv->stats.rx_bytes += skb->len;

	netif_rx(skb);
	FN_EXIT(0, 0);
}

/*
 * Calculate level like the feb 2003 windows driver seems to do
 */
UINT8 acx_signal_to_winlevel(UINT8 rawlevel)
{
	/* UINT8 winlevel = (UINT8) (0.5 + 0.625 * rawlevel); */
	UINT8 winlevel = (UINT8) ((4 + (rawlevel * 5)) / 8);

	if(winlevel>100)
		winlevel=100;

	return winlevel;
}

UINT8 acx_signal_determine_quality(UINT8 signal, UINT8 noise)
{
	int qual;

	qual = (((signal - 30) * 100 / 70) + (100 - noise * 4)) / 2;

	if (qual > 100)
		return 100;
	if (qual < 0)
		return 0;
	return (UINT8)qual;
}

/*----------------------------------------------------------------
* acx100_log_rxbuffer
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

inline void acx100_log_rxbuffer(TIWLAN_DC *pDc)
{
	unsigned int i;
	struct rxhostdescriptor *pDesc;

	FN_ENTER;
	if (debug & L_BUF)
	{
		/* no locks here, since it's entirely non-critical code */
		pDesc = pDc->pRxHostDescQPool;
		for (i = 0; i < pDc->rx_pool_count; i++)
		{
#if (WLAN_HOSTIF==WLAN_USB)
			acxlog(L_DEBUG,"rxbuf %d Ctl=%X val0x14=%X\n",i,pDesc->Ctl,pDesc->Status);
#endif
			if ((pDesc->Ctl & ACX100_CTL_OWN) && (pDesc->Status & BIT31))
				acxlog(L_BUF, "rxbuf %d full\n", i);
			pDesc++;
		}
	}
	FN_EXIT(0, 0);
}

/* currently we don't need to lock anything, since we access Rx
 * descriptors from one IRQ handler only (which is locked), here.
 * Will need to use spin_lock() when adding user context Rx descriptor
 * access. */
#define RX_SPIN_LOCK(lock)
#define RX_SPIN_UNLOCK(lock)

/*------------------------------------------------------------------------------
 * acx100_process_rx_desc
 *
 * RX path top level entry point called directly and only from the IRQ handler
 *
 * Arguments:
 *
 * Returns:
 *
 * Side effects:
 *
 * Call context: Hard IRQ
 *
 * STATUS:
 *
 * Comment:
 *
 *----------------------------------------------------------------------------*/
inline void acx100_process_rx_desc(wlandevice_t *priv)
{
	struct rxhostdescriptor *RxPool;
	TIWLAN_DC *pDc;
	struct rxhostdescriptor *pDesc;
	UINT16 buf_len;
	int curr_idx;
	unsigned int count = 0;
	p80211_hdr_t *buf;
	int qual;

	FN_ENTER;

	pDc = &priv->dc;
	acx100_log_rxbuffer(pDc);

	/* there used to be entry count code here, but because this function is called
	 * by the interrupt handler, we are sure that this will only be entered once
	 * because the kernel locks the interrupt handler */

	RxPool = pDc->pRxHostDescQPool;

	/* First, have a loop to determine the first descriptor that's
	 * full, just in case there's a mismatch between our current
	 * rx_tail and the full descriptor we're supposed to handle. */
	while (1) {
		count++;
		if (unlikely(count > pDc->rx_pool_count))
		{ /* hmm, no luck: all descriptors empty, bail out */
			FN_EXIT(0, 0);
			return;
		}
		/* we're not in user context but in IRQ context here,
		 * so we don't need to use _irqsave() since an IRQ can
		 * only happen once anyway. user context access DOES
		 * need to prevent US from having an IRQ, however
		 * (_irqsave) */
		RX_SPIN_LOCK(&rx_lock);
		curr_idx = pDc->rx_tail;
		pDesc = &RxPool[pDc->rx_tail];
		pDc->rx_tail = (pDc->rx_tail + 1) % pDc->rx_pool_count;
		if ((pDesc->Ctl & ACX100_CTL_OWN) || (0 == (pDesc->Status & BIT31))) {
			RX_SPIN_UNLOCK(&rx_lock);
			break;
		}
		RX_SPIN_UNLOCK(&rx_lock);
	}

	/* now process descriptors, starting with the first we figured out */
	while (1)
	{
		acxlog(L_BUF, "%s: using curr_idx %d, rx_tail is now %d\n", __func__, curr_idx, pDc->rx_tail);

		/* enable for a sweet dump of the frame */
		/* acx100_dump_bytes(buf,buf_len); */

		if (priv->rx_config_1 & RX_CFG1_INCLUDE_ADDIT_HDR) {
			/* take into account additional header in front of packet */
			if(priv->chip_type == CHIPTYPE_ACX111) {
				buf = (p80211_hdr_t*)((UINT8*)&pDesc->data->buf+ 8);
			} else {
				buf = (p80211_hdr_t*)((UINT8*)&pDesc->data->buf+ 4);
			}

		}
		else
		{
			buf = (p80211_hdr_t *)&pDesc->data->buf;
		}

		buf_len = pDesc->data->mac_cnt_rcvd & 0xfff;      /* somelength */
		if ((WLAN_GET_FC_FSTYPE(buf->a3.fc) != WLAN_FSTYPE_BEACON)
		||  (debug & L_XFER_BEACON))
			acxlog(L_XFER|L_DATA, "Rx pkt %02d (%s): time %u len %i signal %d SNR %d macstat %02x phystat %02x phyrate %u mode %d status %d\n",
				curr_idx,
				acx100_get_packet_type_string(buf->a3.fc),
				pDesc->data->time,
				buf_len,
				acx_signal_to_winlevel(pDesc->data->phy_level),
				acx_signal_to_winlevel(pDesc->data->phy_snr),
				pDesc->data->mac_status,
				pDesc->data->phy_stat_baseband,
				pDesc->data->phy_plcp_signal,
				priv->macmode_joined,
				priv->status);

		/* FIXME: should check for Rx errors (pDesc->data->mac_status?
		 * discard broken packets - but NOT for monitor!)
		 * and update Rx packet statistics here */

		if (unlikely(priv->monitor)) {
			acx100_rxmonitor(priv, pDesc->data);
		} else if (buf_len >= 14) {
			acx100_rx_ieee802_11_frame(priv, pDesc);
		} else {
			acxlog(L_DEBUG | L_XFER | L_DATA,
			       "NOT receiving packet (%s): size too small (%d)\n",
			       acx100_get_packet_type_string(buf->a3.fc), buf_len);
		}

		/* Now check Rx quality level, AFTER processing packet.
		 * I tried to figure out how to map these levels to dBm
		 * values, but for the life of me I really didn't
		 * manage to get it. Either these values are not meant to
		 * be expressed in dBm, or it's some pretty complicated
		 * calculation. */

#if FROM_SCAN_SOURCE_ONLY
		/* only consider packets originating from the MAC
		 * address of the device that's managing our BSSID.
		 * Disable it for now, since it removes information (levels
		 * from different peers) and slows the Rx path. */
		if (0 == memcmp(buf->a3.a2, priv->station_assoc.mac_addr, ETH_ALEN)) {
#endif
			priv->wstats.qual.level = acx_signal_to_winlevel(pDesc->data->phy_level);
			priv->wstats.qual.noise = acx_signal_to_winlevel(pDesc->data->phy_snr);
#ifndef OLD_QUALITY
			qual = acx_signal_determine_quality(priv->wstats.qual.level, priv->wstats.qual.noise);
#else
			qual = (priv->wstats.qual.noise <= 100) ?
					100 - priv->wstats.qual.noise : 0;
#endif
			priv->wstats.qual.qual = qual;
			priv->wstats.qual.updated = 7; /* all 3 indicators updated */
#if FROM_SCAN_SOURCE_ONLY
		}
#endif

		RX_SPIN_LOCK(&rx_lock);
		pDesc->Ctl &= ~ACX100_CTL_OWN; /* Host no longer owns this */
		pDesc->Status = 0;

		/* ok, descriptor is handled, now check the next descriptor */
		curr_idx = pDc->rx_tail;
		pDesc = &RxPool[pDc->rx_tail];

		/* if next descriptor is empty, then bail out */
		if (!((pDesc->Ctl & ACX100_CTL_OWN) && (pDesc->Status & BIT31)))
		{
			RX_SPIN_UNLOCK(&rx_lock);
			break;
		}
		else
			pDc->rx_tail = (pDc->rx_tail + 1) % pDc->rx_pool_count;
		RX_SPIN_UNLOCK(&rx_lock);
	}
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_create_tx_host_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/
int acx100_create_tx_host_desc_queue(TIWLAN_DC *pDc)
{
	wlandevice_t *priv;

	UINT i;
#if (WLAN_HOSTIF!=WLAN_USB)
	UINT align_offs;
	UINT alignment;

	struct framehdr *frame_hdr_phy;
	UINT8 *frame_payload_phy;
	struct txhostdescriptor *host_desc_phy;
#endif

	struct framehdr *frame_hdr;
	UINT8 *frame_payload;

	struct txhostdescriptor *host_desc;

	FN_ENTER;

	priv = pDc->priv;

	/* allocate TX header pool */
	pDc->FrameHdrQPoolSize = (priv->TxQueueNo * sizeof(struct framehdr));

#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pFrameHdrQPool =
	      pci_alloc_consistent(0, pDc->FrameHdrQPoolSize, &pDc->FrameHdrQPoolPhyAddr))) {
		acxlog(L_BINSTD, "pDc->pFrameHdrQPool memory allocation error\n");
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->pFrameHdrQPool = 0x%8x\n", (UINT) pDc->pFrameHdrQPool);
	acxlog(L_BINDEBUG, "pDc->pFrameHdrQPoolPhyAddr = 0x%8x\n", (UINT) pDc->FrameHdrQPoolPhyAddr);
#else
	if ((pDc->pFrameHdrQPool=kmalloc(pDc->FrameHdrQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"pDc->pFrameHdrQPool memory allocation error\n");
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pFrameHdrQPool,0,pDc->FrameHdrQPoolSize);
#endif

	/* allocate TX payload pool */
	pDc->TxBufferPoolSize = priv->TxQueueNo*2 * (WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN);
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxBufferPool =
	      pci_alloc_consistent(0, pDc->TxBufferPoolSize, &pDc->TxBufferPoolPhyAddr))) {
		acxlog(L_BINSTD, "pDc->pTxBufferPool memory allocation error\n");
		pci_free_consistent(0, pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->TxBufferPoolSize = 0x%8x\n", (UINT) pDc->TxBufferPoolSize);
	acxlog(L_BINDEBUG, "pDc->TxBufferPool = 0x%8x\n", (UINT) pDc->pTxBufferPool);
	acxlog(L_BINDEBUG, "pDc->TxBufferPoolPhyAddr = 0x%8x\n", (UINT) pDc->TxBufferPoolPhyAddr);
#else
	if ((pDc->pTxBufferPool=kmalloc(pDc->TxBufferPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"pDc->pTxBufferPool memory allocation error\n");
		kfree(pDc->pFrameHdrQPool);
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxBufferPool,0,pDc->TxBufferPoolSize);
#endif

	/* allocate the TX host descriptor queue pool */
	pDc->TxHostDescQPoolSize =  priv->TxQueueNo*2 * sizeof(struct txhostdescriptor) + 3;
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxHostDescQPool =
	      pci_alloc_consistent(0, pDc->TxHostDescQPoolSize,
				  (dma_addr_t *) &pDc->TxHostDescQPoolPhyAddr))) {
		acxlog(L_BINSTD, "Failed to allocate shared memory for TxHostDesc queue\n");
		pci_free_consistent(0, pDc->FrameHdrQPoolSize,
				    pDc->pFrameHdrQPool,
				    pDc->FrameHdrQPoolPhyAddr);
		pci_free_consistent(0, pDc->TxBufferPoolSize,
				    pDc->pTxBufferPool,
				    pDc->TxBufferPoolPhyAddr);
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->pTxHostDescQPool = 0x%08x\n", (UINT) pDc->pTxHostDescQPool);
	acxlog(L_BINDEBUG, "pDc->TxHostDescQPoolPhyAddr = 0x%08x\n", pDc->TxHostDescQPoolPhyAddr);
#else
	if ((pDc->pTxHostDescQPool=kmalloc(pDc->TxHostDescQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"Failed to allocate memory for TxHostDesc queue\n");
		kfree(pDc->pFrameHdrQPool);
		kfree(pDc->pTxBufferPool);
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxHostDescQPool,0,pDc->TxHostDescQPoolSize);
#endif

#if (WLAN_HOSTIF!=WLAN_USB)
	/* check for proper alignment of TX host descriptor pool */
	alignment = (UINT) pDc->pTxHostDescQPool & 3;
	if (alignment) {
		acxlog(L_BINSTD, "%s: TxHostDescQPool not aligned properly\n", __func__);
		align_offs = 4 - alignment;
	} else {
		align_offs = 0;
	}

	host_desc = (struct txhostdescriptor *) ((UINT8 *) pDc->pTxHostDescQPool + align_offs);
	host_desc_phy = (struct txhostdescriptor *) ((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
	frame_hdr_phy = (struct framehdr *) pDc->FrameHdrQPoolPhyAddr;
	frame_payload_phy = (UINT8 *) pDc->TxBufferPoolPhyAddr;
#else
	host_desc = (struct txhostdescriptor *)pDc->pTxHostDescQPool;
#endif
	frame_hdr = (struct framehdr *) pDc->pFrameHdrQPool;
	frame_payload = (UINT8 *) pDc->pTxBufferPool;

	for (i = 0; i < priv->TxQueueNo*2 - 1; i++)
	{
		if (!(i & 1)) {
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->data_phy = (UINT8 *) frame_hdr_phy;
			frame_hdr_phy++;
			host_desc->pNext = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
#endif
			host_desc->data = (UINT8 *) frame_hdr;
			frame_hdr++;
		} else {
#if (WLAN_HOSTIF!=WLAN_USB)
			host_desc->data_phy = (UINT8 *) frame_payload_phy;
			frame_payload_phy += WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN;
#endif
			host_desc->data = (UINT8 *) frame_payload;
			frame_payload += WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN;
			host_desc->pNext = NULL;
		}

		host_desc->Ctl |= ACX100_CTL_OWN;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->desc_phy = host_desc_phy;
		host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
		host_desc_phy++;
#endif
		host_desc++;
	}
	host_desc->data = (UINT8 *) frame_payload;
	host_desc->pNext = 0;
	host_desc->Ctl |= ACX100_CTL_OWN;

#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->data_phy = (UINT8 *) frame_payload_phy;
	host_desc->desc_phy = host_desc_phy;
	host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
	//host_desc->desc_phy_next = (struct txhostdescriptor *) pDc->TxHostDescQPoolPhyAddr;
#endif

	FN_EXIT(0, 0);
	return 0;
}

int acx111_create_tx_host_desc_queue(TIWLAN_DC *pDc)
{
	wlandevice_t *priv;

	UINT i;
	UINT16 eth_body_len = WLAN_MAX_ETHFRM_LEN - WLAN_ETHHDR_LEN;
#if (WLAN_HOSTIF!=WLAN_USB)
	UINT align_offs;
	UINT alignment;

	UINT8 *frame_buffer_phy;
	struct txhostdescriptor *host_desc_phy;
#endif

	UINT8 *frame_buffer;
	struct txhostdescriptor *host_desc;

	FN_ENTER;

	priv = pDc->priv;

	/* allocate TX buffer */
	pDc->TxBufferPoolSize = (priv->TxQueueNo * sizeof(struct framehdr))
				+ priv->TxQueueNo*2 * eth_body_len;
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxBufferPool =
	      pci_alloc_consistent(0, pDc->TxBufferPoolSize, &pDc->TxBufferPoolPhyAddr))) {
		acxlog(L_BINSTD, "pDc->pTxBufferPool memory allocation error\n");
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->TxBufferPoolSize = 0x%8x\n", (UINT) pDc->TxBufferPoolSize);
	acxlog(L_BINDEBUG, "pDc->TxBufferPool = 0x%8x\n", (UINT) pDc->pTxBufferPool);
	acxlog(L_BINDEBUG, "pDc->TxBufferPoolPhyAddr = 0x%8x\n", (UINT) pDc->TxBufferPoolPhyAddr);
#else
	if ((pDc->pTxBufferPool=kmalloc(pDc->TxBufferPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"pDc->pTxBufferPool memory allocation error\n");
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxBufferPool,0,pDc->TxBufferPoolSize);
#endif

	/* allocate the TX host descriptor queue pool */
	pDc->TxHostDescQPoolSize =  priv->TxQueueNo*2 * sizeof(struct txhostdescriptor) + 3;
#if (WLAN_HOSTIF!=WLAN_USB)
	if (!(pDc->pTxHostDescQPool =
	      pci_alloc_consistent(0, pDc->TxHostDescQPoolSize,
				   &pDc->TxHostDescQPoolPhyAddr))) {
		acxlog(L_BINSTD, "Failed to allocate shared memory for TxHostDesc queue\n");
		pci_free_consistent(0, pDc->TxBufferPoolSize,
				    pDc->pTxBufferPool,
				    pDc->TxBufferPoolPhyAddr);
		FN_EXIT(1, 2);
		return 2;
	}
	acxlog(L_BINDEBUG, "pDc->pTxHostDescQPool = 0x%8x\n", (UINT) pDc->pTxHostDescQPool);
	acxlog(L_BINDEBUG, "pDc->TxHostDescQPoolPhyAddr = 0x%8x\n", pDc->TxHostDescQPoolPhyAddr);
#else
	if ((pDc->pTxHostDescQPool=kmalloc(pDc->TxHostDescQPoolSize,GFP_KERNEL))==NULL) {
		acxlog(L_STD,"Failed to allocate memory for TxHostDesc queue\n");
		kfree(pDc->pTxBufferPool);
		FN_EXIT(1,2);
		return(2);
	}
	memset(pDc->pTxHostDescQPool,0,pDc->TxHostDescQPoolSize);
#endif

#if (WLAN_HOSTIF!=WLAN_USB)
	/* check for proper alignment of TX host descriptor pool */
	alignment = (UINT) pDc->pTxHostDescQPool & 3;
	if (alignment) {
		acxlog(L_BINSTD, "%s: TxHostDescQPool not aligned properly\n", __func__);
		align_offs = 4 - alignment;
	} else {
		align_offs = 0;
	}

	host_desc = (struct txhostdescriptor *) ((UINT8 *) pDc->pTxHostDescQPool + align_offs);
	host_desc_phy = (struct txhostdescriptor *) ((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
	frame_buffer_phy = (UINT8 *) pDc->TxBufferPoolPhyAddr;
#else
	host_desc=(struct txhostdescriptor *)pDc->pTxHostDescQPool;
#endif
	frame_buffer = (UINT8 *) pDc->pTxBufferPool;

	for (i = 0; i < priv->TxQueueNo*2 - 1; i++)
	{

		host_desc->data = (UINT8 *) frame_buffer;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->data_phy = (UINT8 *) frame_buffer_phy;
#endif
		if (!(i & 1)) {
			frame_buffer += sizeof(struct framehdr);
#if (WLAN_HOSTIF!=WLAN_USB)
			frame_buffer_phy += sizeof(struct framehdr);
			host_desc->pNext = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
#endif
		} else {
			frame_buffer += eth_body_len;
#if (WLAN_HOSTIF!=WLAN_USB)
			frame_buffer_phy += eth_body_len;
#endif
			host_desc->pNext = NULL;
		}

		host_desc->Ctl |= ACX100_CTL_OWN;

		host_desc++;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->desc_phy = host_desc_phy;
		host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct txhostdescriptor));
		host_desc_phy++;
#endif
	}
	host_desc->data = (UINT8 *) frame_buffer;
	/* TODO is this correct ?? */
	host_desc->pNext = NULL;
	host_desc->Ctl |= ACX100_CTL_OWN;

#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->data_phy = (UINT8 *) frame_buffer_phy;
	host_desc->desc_phy = host_desc_phy;
	host_desc->desc_phy_next = (struct txhostdescriptor *)((UINT8 *) pDc->TxHostDescQPoolPhyAddr + align_offs);
#endif

	FN_EXIT(0, 0);
	return 0;
}

/*----------------------------------------------------------------
* acx100_create_rx_host_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

int acx100_create_rx_host_desc_queue(TIWLAN_DC *pDc)
{
	wlandevice_t *priv;

	UINT i;
#if (WLAN_HOSTIF!=WLAN_USB)
	UINT align_offs;
	UINT alignment;

	struct rxhostdescriptor *host_desc_phy;
	struct rxbuffer *data_phy;
#endif

	struct rxhostdescriptor *host_desc;
	struct rxbuffer *data;

	int result = 0;

	FN_ENTER;

	priv = pDc->priv;

	/* allocate the RX host descriptor queue pool */
	pDc->RxHostDescQPoolSize = (priv->RxQueueNo * sizeof(struct rxhostdescriptor)) + 0x3;

#if (WLAN_HOSTIF!=WLAN_USB)
	if (NULL == (pDc->pRxHostDescQPool =
	     pci_alloc_consistent(0, pDc->RxHostDescQPoolSize,
				(dma_addr_t *)  &pDc->RxHostDescQPoolPhyAddr))) {
		acxlog(L_BINSTD,
		       "Failed to allocate shared memory for RxHostDesc queue\n");
		result = 2;
		goto fail;
	}
#else
	if (NULL == (pDc->pRxHostDescQPool = kmalloc(pDc->RxHostDescQPoolSize, GFP_KERNEL))) {
		acxlog(L_STD,"Failed to allocate memory for RxHostDesc queue\n");
		result = 2;
		goto fail;
	}
	memset(pDc->pRxHostDescQPool,0,pDc->RxHostDescQPoolSize);
#endif

	/* allocate RX buffer pool */
	pDc->RxBufferPoolSize = (priv->RxQueueNo * sizeof(struct rxbuffer));
#if (WLAN_HOSTIF!=WLAN_USB)
	if (NULL == (pDc->pRxBufferPool =
	     pci_alloc_consistent(0, pDc->RxBufferPoolSize,
				  (dma_addr_t *) &pDc->RxBufferPoolPhyAddr))) {
		acxlog(L_BINSTD, "Failed to allocate shared memory for Rx buffer\n");
		pci_free_consistent(0, pDc->RxHostDescQPoolSize,
				    pDc->pRxHostDescQPool,
				    pDc->RxHostDescQPoolPhyAddr);
		result = 2;
		goto fail;
	}
#else
	if (NULL == (pDc->pRxBufferPool = kmalloc(pDc->RxBufferPoolSize,GFP_KERNEL))) {
		acxlog(L_STD,"Failed to allocate memory for Rx buffer\n");
		result=2;
		goto fail;
	}
	memset(pDc->pRxBufferPool,0,pDc->RxBufferPoolSize);
#endif

	acxlog(L_BINDEBUG, "pDc->pRxHostDescQPool = 0x%8x\n", (UINT) pDc->pRxHostDescQPool);
#if (WLAN_HOSTIF!=WLAN_USB)
	acxlog(L_BINDEBUG, "pDc->RxHostDescQPoolPhyAddr = 0x%8x\n", (UINT) pDc->RxHostDescQPoolPhyAddr);
#endif
	acxlog(L_BINDEBUG, "pDc->pRxBufferPool = 0x%8x\n", (UINT) pDc->pRxBufferPool);
#if (WLAN_HOSTIF!=WLAN_USB)
	acxlog(L_BINDEBUG, "pDc->RxBufferPoolPhyAddr = 0x%8x\n", (UINT) pDc->RxBufferPoolPhyAddr);
#endif
#if (WLAN_HOSTIF!=WLAN_USB)
	/* check for proper alignment of RX host descriptor pool */
	if ((alignment = ((UINT) pDc->pRxHostDescQPool) & 3)) {
		acxlog(L_BINSTD, "acx100_create_rx_host_desc_queue: RxHostDescQPool not aligned properly\n");
		align_offs = 4 - alignment;
	} else {
		align_offs = 0;
	}

	host_desc = (struct rxhostdescriptor *) ((UINT8 *) pDc->pRxHostDescQPool + align_offs);
	host_desc_phy = (struct rxhostdescriptor *) ((UINT8 *) pDc->RxHostDescQPoolPhyAddr + align_offs);

	priv->RxHostDescPoolStart = host_desc_phy;
#else
	host_desc = (struct rxhostdescriptor *)pDc->pRxHostDescQPool;
#endif
	data = (struct rxbuffer *) pDc->pRxBufferPool;
#if (WLAN_HOSTIF!=WLAN_USB)
	data_phy = (struct rxbuffer *) pDc->RxBufferPoolPhyAddr;
#endif

	for (i = 0; i < priv->RxQueueNo - 1; i++) {
		host_desc->data = data;
		data++;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->data_phy = data_phy;
		data_phy++;
#endif
		host_desc->length = sizeof(struct rxbuffer);

		/* FIXME: what do these mean ? */
		host_desc->val0x28 = 2;
		host_desc->Ctl &= ~0x80;
#if (WLAN_HOSTIF!=WLAN_USB)
		host_desc->desc_phy = host_desc_phy;
		host_desc->desc_phy_next = (struct rxhostdescriptor *)((UINT8 *) host_desc_phy + sizeof(struct rxhostdescriptor));
		host_desc_phy++;
#endif
		host_desc++;
	}
	host_desc->data = data;
#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->data_phy = data_phy;
#endif
	host_desc->length = sizeof(struct rxbuffer);

	host_desc->val0x28 = 2;
	host_desc->Ctl &= ~0x80;
#if (WLAN_HOSTIF!=WLAN_USB)
	host_desc->desc_phy = host_desc_phy;
	host_desc->desc_phy_next = (struct rxhostdescriptor *)((UINT8 *) pDc->RxHostDescQPoolPhyAddr + align_offs);
#endif
	result = 0;
fail:
	FN_EXIT(1, result);
	return result;
}

/*----------------------------------------------------------------
* acx100_create_tx_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_create_tx_desc_queue(TIWLAN_DC *pDc)
{
	wlandevice_t *priv;

	UINT32 mem_offs;
	UINT32 i;
	struct txdescriptor *tx_desc;
	struct txhostdescriptor *tx_hostdesc;

	UINT hostmemptr; /* remains 0 in USB case */

	FN_ENTER;

	priv = pDc->priv;
	pDc->tx_pool_count = priv->TxQueueNo;

	pDc->TxDescrSize = sizeof(struct txdescriptor);
	if(priv->chip_type == CHIPTYPE_ACX111) {
		/* the acx111 txdescriptor is 4 byte larger = 0x34 */
		pDc->TxDescrSize += 4;
	}

#if (WLAN_HOSTIF==WLAN_USB)
  /* allocate memory for TxDescriptors */
	pDc->pTxDescQPool = (struct txdescriptor *)kmalloc(pDc->tx_pool_count*sizeof(struct txdescriptor),GFP_KERNEL);
	if (!pDc->pTxDescQPool) {
		acxlog(L_STD,"Not enough memory to allocate txdescriptor queue\n");
		return;
	}
#endif

#if (WLAN_HOSTIF!=WLAN_USB)
	if(pDc->pTxDescQPool == NULL) { //calculate it
		pDc->pTxDescQPool = (struct txdescriptor *) (priv->iobase2 +
					     pDc->ui32ACXTxQueueStart);
	}

	acxlog(L_BINDEBUG, "priv->iobase2 = 0x%p\n", priv->iobase2);
	acxlog(L_BINDEBUG, "pDc->ui32ACXTxQueueStart = 0x%08x\n",
	       pDc->ui32ACXTxQueueStart);
	acxlog(L_BINDEBUG, "pDc->pTxDescQPool = 0x%08x\n",
	       (UINT32) pDc->pTxDescQPool);
#endif
	priv->TxQueueFree = priv->TxQueueNo;
	pDc->tx_head = 0;
	pDc->tx_tail = 0;
	tx_desc = pDc->pTxDescQPool;
#if (WLAN_HOSTIF!=WLAN_USB)
	mem_offs = pDc->ui32ACXTxQueueStart;
	hostmemptr = pDc->TxHostDescQPoolPhyAddr;
#else
	mem_offs = (UINT32)pDc->pTxDescQPool;
	hostmemptr = 0; /* will remain 0 for USB */
#endif
	tx_hostdesc = (struct txhostdescriptor *) pDc->pTxHostDescQPool;

	/* clear whole send pool */
	memset(pDc->pTxDescQPool, 0, pDc->tx_pool_count * pDc->TxDescrSize);

	/* loop over whole send pool */
	for (i = 0; i < pDc->tx_pool_count; i++) {

		acxlog(L_BINDEBUG, "configure card tx descriptor = 0x%08x, size: 0x%X\n", (UINT32)tx_desc, pDc->TxDescrSize);

		/* pointer to hostdesc memory */
		tx_desc->HostMemPtr = hostmemptr;
		/* initialise ctl */
		tx_desc->Ctl = DESC_CTL_INIT;
		/* point to next txdesc */
		tx_desc->pNextDesc = mem_offs + pDc->TxDescrSize;
		/* pointer to first txhostdesc */
		tx_desc->host_desc = tx_hostdesc;

		/* reserve two (hdr desc and payload desc) */
		tx_hostdesc += 2;
#if (WLAN_HOSTIF!=WLAN_USB)
		hostmemptr += 2 * sizeof(struct txhostdescriptor);
#endif
		/* go to the next */
		mem_offs += pDc->TxDescrSize;
		tx_desc = (struct txdescriptor *)(((UINT32)tx_desc) + pDc->TxDescrSize);
	}
	/* go to the last one */
	tx_desc = (struct txdescriptor *)(((UINT32)tx_desc) - pDc->TxDescrSize);
	/* tx_desc--; */
	/* and point to the first making it a ring buffer */
#if (WLAN_HOSTIF!=WLAN_USB)
	tx_desc->pNextDesc = pDc->ui32ACXTxQueueStart;
#else
	tx_desc->pNextDesc = (UINT32)pDc->pTxDescQPool;
#endif
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_create_rx_desc_queue
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

void acx100_create_rx_desc_queue(TIWLAN_DC *pDc)
{
	wlandevice_t *priv;
#if (WLAN_HOSTIF!=WLAN_USB)
	UINT32 mem_offs;
	UINT32 i;
	struct rxdescriptor *rx_desc;
#endif

	FN_ENTER;

	priv = pDc->priv;

#if (WLAN_HOSTIF!=WLAN_USB)
	if(pDc->pRxDescQPool == NULL) { /* calculate it */
		/* WHY IS IT "TxQueueNo"?
		 * Probably because Rx pool ptr should be right AFTER Tx pool */
		pDc->pRxDescQPool = (struct rxdescriptor *) ((UINT8 *) pDc->pTxDescQPool + (priv->TxQueueNo * sizeof(struct txdescriptor)));
	}
#endif
	pDc->rx_pool_count = priv->RxQueueNo;
	pDc->rx_tail = 0;
#if (WLAN_HOSTIF!=WLAN_USB)
	mem_offs = pDc->ui32ACXRxQueueStart;
	rx_desc = (struct rxdescriptor *) pDc->pRxDescQPool;

	/* clear whole receive pool */
	memset(pDc->pRxDescQPool, 0, pDc->rx_pool_count * sizeof(struct rxdescriptor));

	/* loop over whole receive pool */
	for (i = 0; i < pDc->rx_pool_count; i++) {

		acxlog(L_BINDEBUG, "configure card rx descriptor = 0x%08x\n", (UINT32)rx_desc);

		if(priv->chip_type == CHIPTYPE_ACX100) {
			rx_desc->Ctl = 0xc;
		} else if(priv->chip_type == CHIPTYPE_ACX111) {
			rx_desc->Ctl = 0x0;
		}

		/* point to next rxdesc */
		rx_desc->pNextDesc = mem_offs + sizeof(struct rxdescriptor); /* next rxdesc pNextDesc */

		/* go to the next */
		mem_offs += sizeof(struct rxdescriptor);
		rx_desc++;
	}
	/* go to the last one */
	rx_desc--;

	/* and point to the first making it a ring buffer */
	rx_desc->pNextDesc = pDc->ui32ACXRxQueueStart;
#endif
	FN_EXIT(0, 0);
}

/*----------------------------------------------------------------
* acx100_init_memory_pools
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*	FIXME: This function still needs a cleanup
*----------------------------------------------------------------*/

int acx100_init_memory_pools(wlandevice_t *priv, acx100_memmap_t *mmt)
{
	UINT32 TotalMemoryBlocks;  // var_40
        memblocksize_t MemoryBlockSize;

        acx100_memconfigoption_t MemoryConfigOption;    // var_2c

	FN_ENTER;


	/* Let's see if we can follow this:
	   first we select our memory block size (which I think is
	   completely arbitrary) */
	MemoryBlockSize.size = cpu_to_le16(priv->memblocksize);

	/* Then we alert the card to our decision of block size */
	if (!acx100_configure(priv, &MemoryBlockSize, ACX100_IE_BLOCK_SIZE)) {
		acxlog(L_BINSTD, "Ctl: MemoryBlockSizeWrite failed\n");
		FN_EXIT(1, 0);
		return 0;
	}

	/* We figure out how many total blocks we can create, using
	   the block size we chose, and the beginning and ending
	   memory pointers. IE. end-start/size */
	TotalMemoryBlocks = (le32_to_cpu(mmt->PoolEnd) - le32_to_cpu(mmt->PoolStart)) / priv->memblocksize;

	acxlog(L_DEBUG,"TotalMemoryBlocks=%d (%d bytes)\n",TotalMemoryBlocks,TotalMemoryBlocks*priv->memblocksize);

	/* This one I have no idea on */
	/* block-transfer=0x20000
	 * indirect descriptors=0x10000
	 */
#if (WLAN_HOSTIF==WLAN_USB)
	MemoryConfigOption.DMA_config = cpu_to_le32(0x20000);
#else
	MemoryConfigOption.DMA_config = cpu_to_le32(0x30000);
#endif


	/* Declare start of the Rx host pool */
	MemoryConfigOption.pRxHostDesc = cpu_to_le32((UINT32)priv->RxHostDescPoolStart);
	acxlog(L_DEBUG, "pRxHostDesc %08x, RxHostDescPoolStart %p\n", MemoryConfigOption.pRxHostDesc, priv->RxHostDescPoolStart);

	/* 50% of the allotment of memory blocks go to tx descriptors */
	priv->TxBlockNum = TotalMemoryBlocks / 2;
	MemoryConfigOption.TxBlockNum = cpu_to_le16(priv->TxBlockNum);

	/* and 50% go to the rx descriptors */
	priv->RxBlockNum = TotalMemoryBlocks - priv->TxBlockNum;
	MemoryConfigOption.RxBlockNum = cpu_to_le16(priv->RxBlockNum);

	/* size of the tx and rx descriptor queues */
	priv->TotalTxBlockSize = priv->TxBlockNum * priv->memblocksize;
	priv->TotalRxBlockSize = priv->RxBlockNum * priv->memblocksize;
	acxlog(L_DEBUG, "TxBlockNum %d RxBlockNum %d TotalTxBlockSize %d TotalTxBlockSize %d\n", MemoryConfigOption.TxBlockNum, MemoryConfigOption.RxBlockNum, priv->TotalTxBlockSize, priv->TotalRxBlockSize);


	/* align the tx descriptor queue to an alignment of 0x20 (32 bytes) */
	MemoryConfigOption.rx_mem =
		cpu_to_le32((le32_to_cpu(mmt->PoolStart) + 0x1f) & ~0x1f);

	/* align the rx descriptor queue to units of 0x20 and offset it
	   by the tx descriptor queue */
	MemoryConfigOption.tx_mem =
	    cpu_to_le32((le32_to_cpu(mmt->PoolStart) + priv->TotalRxBlockSize + 0x1f) & ~0x1f);
	acxlog(L_DEBUG, "rx_mem %08x rx_mem %08x\n", MemoryConfigOption.tx_mem, MemoryConfigOption.rx_mem);

	/* alert the device to our decision */
	if (!acx100_configure(priv, &MemoryConfigOption, ACX1xx_IE_MEMORY_CONFIG_OPTIONS)) {
		acxlog(L_DEBUG,"%s: configure memory config options failed!\n", __func__);
		FN_EXIT(1, 0);
		return 0;
	}

	/* and tell the device to kick it into gear */
	if (!acx100_issue_cmd(priv, ACX100_CMD_INIT_MEMORY, NULL, 0, 5000)) {
		acxlog(L_DEBUG,"%s: init memory failed!\n", __func__);
		FN_EXIT(1, 0);
		return 0;
	}
	FN_EXIT(1, 1);
	return 1;
}

/*----------------------------------------------------------------
* acx100_get_tx_desc
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

struct txdescriptor *acx100_get_tx_desc(wlandevice_t *priv)
{
	struct TIWLAN_DC *pDc = &priv->dc;
	struct txdescriptor *tx_desc;
	unsigned long flags;

	FN_ENTER;

	spin_lock_irqsave(&tx_lock, flags);

	tx_desc = GET_TX_DESC_PTR(pDc, pDc->tx_head);

	if (unlikely(0 == (tx_desc->Ctl & ACX100_CTL_OWN))) {
		/* whoops, descr at current index is not free, so probably
		 * ring buffer already full */
		tx_desc = NULL;
		goto error;
	}

	priv->TxQueueFree--;
	acxlog(L_BUF, "got Tx desc %d, %d remain.\n", pDc->tx_head, priv->TxQueueFree);

/*
 * This comment is probably not entirely correct, needs further discussion
 * (restored commented-out code below to fix Tx ring buffer overflow,
 * since it's much better to have a slightly less efficiently used ring
 * buffer rather than one which easily overflows):
 *
 * This doesn't do anything other than limit our maximum number of
 * buffers used at a single time (we might as well just declare
 * MINFREE_TX less descriptors when we open up.) We should just let it
 * slide here, and back off MINFREE_TX in acx100_clean_tx_desc, when given the
 * opportunity to let the queue start back up.
 */
	if (priv->TxQueueFree < MINFREE_TX)
	{
		acxlog(L_XFER, "stop queue (avail. Tx desc %d).\n", priv->TxQueueFree);
		netif_stop_queue(priv->netdev);
	}

	/* returning current descriptor, so advance to next free one */
	pDc->tx_head = (pDc->tx_head + 1) % pDc->tx_pool_count;
error:
	spin_unlock_irqrestore(&tx_lock, flags);

	FN_EXIT(0, (int)tx_desc);
	return tx_desc;
}

static char type_string[32];	/* I *really* don't care that this is static,
				   so don't complain, else... ;-) */

/*----------------------------------------------------------------
* acx100_get_packet_type_string
*
*
* Arguments:
*
* Returns:
*
* Side effects:
*
* Call context:
*
* STATUS:
*
* Comment:
*
*----------------------------------------------------------------*/

char *acx100_get_packet_type_string(UINT16 fc)
{
	char *ftype = "UNKNOWN", *fstype = "UNKNOWN";
	char *mgmt_arr[] = { "AssocReq", "AssocResp", "ReassocReq", "ReassocResp", "ProbeReq", "ProbeResp", "UNKNOWN", "UNKNOWN", "Beacon", "ATIM", "Disassoc", "Authen", "Deauthen" };
	char *ctl_arr[] = { "PSPoll", "RTS", "CTS", "Ack", "CFEnd", "CFEndCFAck" };
	char *data_arr[] = { "DataOnly", "Data CFAck", "Data CFPoll", "Data CFAck/CFPoll", "Null", "CFAck", "CFPoll", "CFAck/CFPoll" };

	FN_ENTER;
	switch (WLAN_GET_FC_FTYPE(fc)) {
	case WLAN_FTYPE_MGMT:
		ftype = "MGMT";
		if (WLAN_GET_FC_FSTYPE(fc) <= 0x0c)
			fstype = mgmt_arr[WLAN_GET_FC_FSTYPE(fc)];
		break;
	case WLAN_FTYPE_CTL:
		ftype = "CTL";
		if ((WLAN_GET_FC_FSTYPE(fc) >= 0x0a) && (WLAN_GET_FC_FSTYPE(fc) <= 0x0f))
			fstype = ctl_arr[WLAN_GET_FC_FSTYPE(fc)-0x0a];
		break;
	case WLAN_FTYPE_DATA:
		ftype = "DATA";
		if (WLAN_GET_FC_FSTYPE(fc) <= 0x07)
			fstype = data_arr[WLAN_GET_FC_FSTYPE(fc)];
		break;
	}
	sprintf(type_string, "%s/%s", ftype, fstype);
	FN_EXIT(1, (int)type_string);
	return type_string;
}
