#ifndef _MERGE_H_
#define _MERGE_H_

#include <linux/interrupt.h>

irqreturn_t acx_interrupt(int irq, void *dev_id);

void acx_create_desc_queues(acx_device_t *adev, u32 tx_queue_start,
			u32 rx_queue_start);

int acx_create_hostdesc_queues(acx_device_t *adev);
void acx_free_desc_queues(acx_device_t *adev);

void acx_log_rxbuffer(const acx_device_t *adev);
void acx_log_txbuffer(acx_device_t *adev);

void acx_op_stop(struct ieee80211_hw *hw);
int acx_op_start(struct ieee80211_hw *hw);

void acx_handle_info_irq(acx_device_t *adev);


int _acx_read_phy_reg(acx_device_t *adev, u32 reg, u8 *charbuf);
int _acx_write_phy_reg(acx_device_t *adev, u32 reg, u8 value);

void acx_irq_enable(acx_device_t *adev);
void acx_irq_disable(acx_device_t *adev);

int acx_read_eeprom_byte(acx_device_t *adev, u32 addr, u8 *charbuf);
char *acx_proc_eeprom_output(int *length, acx_device_t *adev);

void acx_up(struct ieee80211_hw *hw);

void acx_set_interrupt_mask(acx_device_t *adev);

void acx_show_card_eeprom_id(acx_device_t *adev);

unsigned int acx_tx_clean_txdesc(acx_device_t *adev);

void _acx_tx_data(acx_device_t *adev, tx_t *tx_opaque, int len,
		struct ieee80211_tx_info *info, struct sk_buff *skb);

void *_acx_get_txbuf(acx_device_t * adev, tx_t * tx_opaque);
void acx_process_rxdesc(acx_device_t *adev);

void acx_delete_dma_regions(acx_device_t *adev);
int acx_reset_dev(acx_device_t *adev);
int acx_verify_init(acx_device_t *adev);

void acx_clean_txdesc_emergency(acx_device_t *adev);
void acx_irq_work(struct work_struct *work);

u32 acx_read_cmd_type_status(acx_device_t *adev);
void acx_write_cmd_type_status(acx_device_t *adev, u16 type, u16 status);
void acx_init_mboxes(acx_device_t *adev);
int acx_write_fw(acx_device_t *adev, const firmware_image_t *fw_image,
			u32 offset);
int acx_validate_fw(acx_device_t *adev, const firmware_image_t *fw_image,
			u32 offset);
int acxmem_upload_fw(acx_device_t *adev);

/* wrappers on acx_upload_radio(adev, filename */
int acxmem_upload_radio(acx_device_t *adev);
int acxpci_upload_radio(acx_device_t *adev);

void acx_power_led(acx_device_t * adev, int enable);


#if defined(CONFIG_ACX_MAC80211_PCI) \
 || defined(CONFIG_ACX_MAC80211_MEM)


void acxmem_update_queue_indicator(acx_device_t *adev, int txqueue);

static inline txdesc_t* acx_get_txdesc(acx_device_t *adev, int index)
{
	return (txdesc_t*) (((u8*) adev->tx.desc_start)
			+ index * adev->tx.desc_size);
}

static inline txdesc_t* acx_advance_txdesc(acx_device_t *adev,
					txdesc_t* txdesc, int inc)
{
	return (txdesc_t*) (((u8*) txdesc)
			+ inc * adev->tx.desc_size);
}

#else /* !(CONFIG_ACX_MAC80211_PCI || CONFIG_ACX_MAC80211_MEM) */

static inline void acxmem_update_queue_indicator(acx_device_t *adev,
			int txqueue)
{ }

static inline txdesc_t* acx_advance_txdesc(acx_device_t *adev,
			txdesc_t* txdesc, int inc)
{ return (txdesc_t*) NULL; }

/* empty stub here, real one in merge.c */
#define ACX_FREE_QUEUES(adev, _dir_)

#endif /* !(CONFIG_ACX_MAC80211_PCI || CONFIG_ACX_MAC80211_MEM) */

#endif /* _MERGE_H_ */
