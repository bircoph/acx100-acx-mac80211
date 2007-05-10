/**** (legal) claimer in README
** Copyright (C) 2003  ACX100 Open Source Project
*/

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>
#include <net/iw_handler.h>

#include "acx.h"


/***********************************************************************
*/

/* channel frequencies
 * TODO: Currently, every other 802.11 driver keeps its own copy of this. In
 * the long run this should be integrated into ieee802_11.h or wireless.h or
 * whatever IEEE802.11x framework evolves */
static const u16 acx_channel_freq[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442,
	2447, 2452, 2457, 2462, 2467, 2472, 2484,
};

/***********************************************************************
*/
static int
acx_ioctl_get_name(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	static const char *const names[] =
	    { "IEEE 802.11b+/g+", "IEEE 802.11b+" };

	strcpy(wrqu->name, names[IS_ACX111(adev) ? 0 : 1]);

	return OK;
}


/***********************************************************************
** acx_ioctl_set_freq
*/
static int
acx_ioctl_set_freq(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int channel = -1;
	unsigned int mult = 1;
	int result;

	FN_ENTER;

	if (wrqu->freq.e == 0 && wrqu->freq.m <= 1000) {
		/* Setting by channel number */
		channel = wrqu->freq.m;
	} else {
		/* If setting by frequency, convert to a channel */
		int i;

		for (i = 0; i < (6 - wrqu->freq.e); i++)
			mult *= 10;

		for (i = 1; i <= 14; i++)
			if (wrqu->freq.m == acx_channel_freq[i - 1] * mult)
				channel = i;
	}

	if (channel > 14) {
		result = -EINVAL;
		goto end;
	}

	acx_sem_lock(adev);

	adev->channel = channel;
	/* hmm, the following code part is strange, but this is how
	 * it was being done before... */
	log(L_IOCTL, "Changing to channel %d\n", channel);
	SET_BIT(adev->set_mask, GETSET_CHANNEL);

	result = -EINPROGRESS;	/* need to call commit handler */

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
*/
static inline int
acx_ioctl_get_freq(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	wrqu->freq.e = 0;
	wrqu->freq.m = adev->channel;
	return OK;
}


/***********************************************************************
** acx_ioctl_set_mode
*/
static int
acx_ioctl_set_mode(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	acx_sem_lock(adev);

	switch (wrqu->mode) {
	case IW_MODE_AUTO:
		adev->mode = ACX_MODE_OFF;
		break;
	case IW_MODE_MONITOR:
		adev->mode = ACX_MODE_MONITOR;
		break;
	case IW_MODE_ADHOC:
		adev->mode = ACX_MODE_0_ADHOC;
		break;
	case IW_MODE_INFRA:
		adev->mode = ACX_MODE_2_STA;
		break;
	case IW_MODE_MASTER:
		printk("acx: master mode (HostAP) is very, very "
		       "experimental! It might work partially, but "
		       "better get prepared for nasty surprises "
		       "at any time\n");
		adev->mode = ACX_MODE_3_AP;
		break;
	case IW_MODE_REPEAT:
	case IW_MODE_SECOND:
	default:
		result = -EOPNOTSUPP;
		goto end_unlock;
	}

	log(L_ASSOC, "new adev->mode=%d\n", adev->mode);
	SET_BIT(adev->set_mask, GETSET_MODE);
	result = -EINPROGRESS;

      end_unlock:
	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
*/
static int
acx_ioctl_get_mode(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result = 0;

	switch (adev->mode) {
	case ACX_MODE_OFF:
		wrqu->mode = IW_MODE_AUTO;
		break;
	case ACX_MODE_MONITOR:
		wrqu->mode = IW_MODE_MONITOR;
		break;
	case ACX_MODE_0_ADHOC:
		wrqu->mode = IW_MODE_ADHOC;
		break;
	case ACX_MODE_2_STA:
		wrqu->mode = IW_MODE_INFRA;
		break;
	case ACX_MODE_3_AP:
		wrqu->mode = IW_MODE_MASTER;
		break;
	default:
		result = -EOPNOTSUPP;
	}
	return result;
}

/***********************************************************************
*/
/* maps bits from acx111 rate to rate in Mbits */
static const unsigned int acx111_rate_tbl[] = {
	1000000,		/* 0 */
	2000000,		/* 1 */
	5500000,		/* 2 */
	6000000,		/* 3 */
	9000000,		/* 4 */
	11000000,		/* 5 */
	12000000,		/* 6 */
	18000000,		/* 7 */
	22000000,		/* 8 */
	24000000,		/* 9 */
	36000000,		/* 10 */
	48000000,		/* 11 */
	54000000,		/* 12 */
	500000,			/* 13, should not happen */
	500000,			/* 14, should not happen */
	500000,			/* 15, should not happen */
};

static int
acx_ioctl_set_rts(struct net_device *ndev,
		  struct iw_request_info *info,
		  union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->rts;
	acx_device_t *adev = ndev2adev(ndev);
	int val = vwrq->value;

	if (vwrq->disabled)
		val = 2312;
	if ((val < 0) || (val > 2312))
		return -EINVAL;

	adev->rts_threshold = val;
	return OK;
}

static inline int
acx_ioctl_get_rts(struct net_device *ndev,
		  struct iw_request_info *info,
		  union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->rts;
	acx_device_t *adev = ndev2adev(ndev);

	vwrq->value = adev->rts_threshold;
	vwrq->disabled = (vwrq->value >= 2312);
	vwrq->fixed = 1;
	return OK;
}


#if ACX_FRAGMENTATION
static int
acx_ioctl_set_frag(struct net_device *ndev,
		   struct iw_request_info *info,
		   struct iw_param *vwrq, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int val = vwrq->value;

	if (vwrq->disabled)
		val = 32767;
	else if ((val < 256) || (val > 2347))
		return -EINVAL;

	adev->frag_threshold = val;
	return OK;
}

static inline int
acx_ioctl_get_frag(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->frag;
	acx_device_t *adev = ndev2adev(ndev);

	vwrq->value = adev->frag_threshold;
	vwrq->disabled = (vwrq->value >= 2347);
	vwrq->fixed = 1;
	return OK;
}
#endif


/***********************************************************************
** acx_ioctl_set_encode
*/
static int
acx_ioctl_set_encode(struct net_device *ndev,
		     struct iw_request_info *info,
		     union iwreq_data *wrqu, char *extra)
{
	struct iw_point *dwrq = &wrqu->encoding;
	acx_device_t *adev = ndev2adev(ndev);
	int index;
	int result;

	FN_ENTER;

	log(L_IOCTL, "set encoding flags=0x%04X, size=%d, key: %s\n",
	    dwrq->flags, dwrq->length, extra ? "set" : "No key");

	acx_sem_lock(adev);

	index = (dwrq->flags & IW_ENCODE_INDEX) - 1;

	if (dwrq->length > 0) {
		/* if index is 0 or invalid, use default key */
		if ((index < 0) || (index > 3))
			index = (int)adev->wep_current_index;

		if (0 == (dwrq->flags & IW_ENCODE_NOKEY)) {
			if (dwrq->length > 29)
				dwrq->length = 29;	/* restrict it */

			if (dwrq->length > 13) {
				/* 29*8 == 232, WEP256 */
				adev->wep_keys[index].size = 29;
			} else if (dwrq->length > 5) {
				/* 13*8 == 104bit, WEP128 */
				adev->wep_keys[index].size = 13;
			} else if (dwrq->length > 0) {
				/* 5*8 == 40bit, WEP64 */
				adev->wep_keys[index].size = 5;
			} else {
				/* disable key */
				adev->wep_keys[index].size = 0;
			}

			memset(adev->wep_keys[index].key, 0,
			       sizeof(adev->wep_keys[index].key));
			memcpy(adev->wep_keys[index].key, extra, dwrq->length);
		}
	} else {
		/* set transmit key */
		if ((index >= 0) && (index <= 3))
			adev->wep_current_index = index;
		else if (0 == (dwrq->flags & IW_ENCODE_MODE)) {
			/* complain if we were not just setting
			 * the key mode */
			result = -EINVAL;
			goto end_unlock;
		}
	}

	adev->wep_enabled = !(dwrq->flags & IW_ENCODE_DISABLED);

	if (dwrq->flags & IW_ENCODE_OPEN) {
		adev->auth_alg = WLAN_AUTH_ALG_OPENSYSTEM;
		adev->wep_restricted = 0;

	} else if (dwrq->flags & IW_ENCODE_RESTRICTED) {
		adev->auth_alg = WLAN_AUTH_ALG_SHAREDKEY;
		adev->wep_restricted = 1;
	}

	/* set flag to make sure the card WEP settings get updated */
	SET_BIT(adev->set_mask, GETSET_WEP);

	log(L_IOCTL, "len=%d, key at 0x%p, flags=0x%X\n",
	    dwrq->length, extra, dwrq->flags);

	for (index = 0; index <= 3; index++) {
		if (adev->wep_keys[index].size) {
			log(L_IOCTL, "index=%d, size=%d, key at 0x%p\n",
			    adev->wep_keys[index].index,
			    (int)adev->wep_keys[index].size,
			    adev->wep_keys[index].key);
		}
	}
	result = -EINPROGRESS;

      end_unlock:
	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_encode
*/
static int
acx_ioctl_get_encode(struct net_device *ndev,
		     struct iw_request_info *info,
		     union iwreq_data *wrqu, char *extra)
{
	struct iw_point *dwrq = &wrqu->encoding;
	acx_device_t *adev = ndev2adev(ndev);
	int index = (dwrq->flags & IW_ENCODE_INDEX) - 1;

	FN_ENTER;

	if (adev->wep_enabled == 0) {
		dwrq->flags = IW_ENCODE_DISABLED;
	} else {
		if ((index < 0) || (index > 3))
			index = (int)adev->wep_current_index;

		dwrq->flags = (adev->wep_restricted == 1) ?
		    IW_ENCODE_RESTRICTED : IW_ENCODE_OPEN;
		dwrq->length = adev->wep_keys[index].size;

		memcpy(extra, adev->wep_keys[index].key,
		       adev->wep_keys[index].size);
	}

	/* set the current index */
	SET_BIT(dwrq->flags, index + 1);

	log(L_IOCTL, "len=%d, key=%p, flags=0x%X\n",
	    dwrq->length, dwrq->pointer, dwrq->flags);

	FN_EXIT1(OK);
	return OK;
}


/***********************************************************************
*/
static int
acx_ioctl_set_power(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->power;
	acx_device_t *adev = ndev2adev(ndev);
	int result = -EINPROGRESS;

	FN_ENTER;

	log(L_IOCTL, "set 802.11 powersave flags=0x%04X\n", vwrq->flags);

	acx_sem_lock(adev);

	if (vwrq->disabled) {
		CLEAR_BIT(adev->ps_wakeup_cfg, PS_CFG_ENABLE);
		SET_BIT(adev->set_mask, GETSET_POWER_80211);
		goto end;
	}
	if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_TIMEOUT) {
		u16 ps_timeout = (vwrq->value * 1024) / 1000;

		if (ps_timeout > 255)
			ps_timeout = 255;
		log(L_IOCTL, "setting PS timeout value to %d time units "
		    "due to %dus\n", ps_timeout, vwrq->value);
		adev->ps_hangover_period = ps_timeout;
	} else if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_PERIOD) {
		u16 ps_periods = vwrq->value / 1000000;

		if (ps_periods > 255)
			ps_periods = 255;
		log(L_IOCTL, "setting PS period value to %d periods "
		    "due to %dus\n", ps_periods, vwrq->value);
		adev->ps_listen_interval = ps_periods;
		CLEAR_BIT(adev->ps_wakeup_cfg, PS_CFG_WAKEUP_MODE_MASK);
		SET_BIT(adev->ps_wakeup_cfg, PS_CFG_WAKEUP_EACH_ITVL);
	}

	switch (vwrq->flags & IW_POWER_MODE) {
		/* FIXME: are we doing the right thing here? */
	case IW_POWER_UNICAST_R:
		CLEAR_BIT(adev->ps_options, PS_OPT_STILL_RCV_BCASTS);
		break;
	case IW_POWER_MULTICAST_R:
		SET_BIT(adev->ps_options, PS_OPT_STILL_RCV_BCASTS);
		break;
	case IW_POWER_ALL_R:
		SET_BIT(adev->ps_options, PS_OPT_STILL_RCV_BCASTS);
		break;
	case IW_POWER_ON:
		break;
	default:
		log(L_IOCTL, "unknown PS mode\n");
		result = -EINVAL;
		goto end;
	}

	SET_BIT(adev->ps_wakeup_cfg, PS_CFG_ENABLE);
	SET_BIT(adev->set_mask, GETSET_POWER_80211);
      end:
	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
*/
static int
acx_ioctl_get_power(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->power;
	acx_device_t *adev = ndev2adev(ndev);

	FN_ENTER;

	log(L_IOCTL, "Get 802.11 Power Save flags = 0x%04X\n", vwrq->flags);
	vwrq->disabled = ((adev->ps_wakeup_cfg & PS_CFG_ENABLE) == 0);
	if (vwrq->disabled)
		goto end;

	if ((vwrq->flags & IW_POWER_TYPE) == IW_POWER_TIMEOUT) {
		vwrq->value = adev->ps_hangover_period * 1000 / 1024;
		vwrq->flags = IW_POWER_TIMEOUT;
	} else {
		vwrq->value = adev->ps_listen_interval * 1000000;
		vwrq->flags = IW_POWER_PERIOD | IW_POWER_RELATIVE;
	}
	if (adev->ps_options & PS_OPT_STILL_RCV_BCASTS)
		SET_BIT(vwrq->flags, IW_POWER_ALL_R);
	else
		SET_BIT(vwrq->flags, IW_POWER_UNICAST_R);
      end:
	FN_EXIT1(OK);
	return OK;
}


/***********************************************************************
** acx_ioctl_get_txpow
*/
static inline int
acx_ioctl_get_txpow(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->power;
	acx_device_t *adev = ndev2adev(ndev);

	FN_ENTER;

	vwrq->flags = IW_TXPOW_DBM;
	vwrq->disabled = 0;
	vwrq->fixed = 1;
	vwrq->value = adev->tx_level_dbm;

	log(L_IOCTL, "get txpower:%d dBm\n", adev->tx_level_dbm);

	FN_EXIT1(OK);
	return OK;
}


/***********************************************************************
** acx_ioctl_set_txpow
*/
static int
acx_ioctl_set_txpow(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->power;
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	log(L_IOCTL, "set txpower:%d, disabled:%d, flags:0x%04X\n",
	    vwrq->value, vwrq->disabled, vwrq->flags);

	acx_sem_lock(adev);

	if (vwrq->disabled != adev->tx_disabled) {
		SET_BIT(adev->set_mask, GETSET_TX);
	}

	adev->tx_disabled = vwrq->disabled;
	if (vwrq->value == -1) {
		if (vwrq->disabled) {
			adev->tx_level_dbm = 0;
			log(L_IOCTL, "disable radio tx\n");
		} else {
			/* adev->tx_level_auto = 1; */
			log(L_IOCTL, "set tx power auto (NIY)\n");
		}
	} else {
		adev->tx_level_dbm = vwrq->value <= 20 ? vwrq->value : 20;
		/* adev->tx_level_auto = 0; */
		log(L_IOCTL, "set txpower=%d dBm\n", adev->tx_level_dbm);
	}
	SET_BIT(adev->set_mask, GETSET_TXPOWER);

	result = -EINPROGRESS;

	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_range
*/
static int
acx_ioctl_get_range(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_point *dwrq = &wrqu->data;
	struct iw_range *range = (struct iw_range *)extra;
	acx_device_t *adev = ndev2adev(ndev);
	int i, n;

	FN_ENTER;

	if (!dwrq->pointer)
		goto end;

	dwrq->length = sizeof(struct iw_range);
	memset(range, 0, sizeof(struct iw_range));
	n = 0;
	for (i = 1; i <= 14; i++) {
		if (adev->reg_dom_chanmask & (1 << (i - 1))) {
			range->freq[n].i = i;
			range->freq[n].m = acx_channel_freq[i - 1] * 100000;
			range->freq[n].e = 1;	/* units are MHz */
			n++;
		}
	}
	range->num_channels = n;
	range->num_frequency = n;

	range->min_rts = 0;
	range->max_rts = 2312;

#if ACX_FRAGMENTATION
	range->min_frag = 256;
	range->max_frag = 2312;
#endif

	range->encoding_size[0] = 5;
	range->encoding_size[1] = 13;
	range->encoding_size[2] = 29;
	range->num_encoding_sizes = 3;
	range->max_encoding_tokens = 4;

	range->min_pmp = 0;
	range->max_pmp = 5000000;
	range->min_pmt = 0;
	range->max_pmt = 65535 * 1000;
	range->pmp_flags = IW_POWER_PERIOD;
	range->pmt_flags = IW_POWER_TIMEOUT;
	range->pm_capa = IW_POWER_PERIOD | IW_POWER_TIMEOUT | IW_POWER_ALL_R;

	if (IS_ACX100(adev)) {	/* ACX100 has direct radio programming - arbitrary levels, so offer a lot */
		for (i = 0; i <= IW_MAX_TXPOWER - 1; i++)
			range->txpower[i] = 20 * i / (IW_MAX_TXPOWER - 1);
		range->num_txpower = IW_MAX_TXPOWER;
		range->txpower_capa = IW_TXPOW_DBM;
	} else {
		int count =
		    min(IW_MAX_TXPOWER, (int)adev->cfgopt_power_levels.len);
		for (i = 0; i <= count; i++)
			range->txpower[i] = adev->cfgopt_power_levels.list[i];
		range->num_txpower = count;
		/* this list is given in mW */
		range->txpower_capa = IW_TXPOW_MWATT;
	}

	range->we_version_compiled = WIRELESS_EXT;
	range->we_version_source = 0x9;

	range->retry_capa = IW_RETRY_LIMIT;
	range->retry_flags = IW_RETRY_LIMIT;
	range->min_retry = 1;
	range->max_retry = 255;

	range->r_time_flags = IW_RETRY_LIFETIME;
	range->min_r_time = 0;
	/* FIXME: lifetime ranges and orders of magnitude are strange?? */
	range->max_r_time = 65535;

	if (IS_USB(adev))
		range->sensitivity = 0;
	else if (IS_ACX111(adev))
		range->sensitivity = 3;
	else
		range->sensitivity = 255;

	for (i = 0; i < adev->rate_supported_len; i++) {
		range->bitrate[i] = (adev->rate_supported[i] & ~0x80) * 500000;
		/* never happens, but keep it, to be safe: */
		if (range->bitrate[i] == 0)
			break;
	}
	range->num_bitrates = i;

	range->max_qual.qual = 100;
	range->max_qual.level = 100;
	range->max_qual.noise = 100;
	/* TODO: better values */
	range->avg_qual.qual = 90;
	range->avg_qual.level = 80;
	range->avg_qual.noise = 2;

      end:
	FN_EXIT1(OK);
	return OK;
}


/***********************************************************************
** Private functions
*/

/***********************************************************************
** acx_ioctl_get_nick
*/
static inline int
acx_ioctl_get_nick(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	struct iw_point *dwrq = &wrqu->data;
	acx_device_t *adev = ndev2adev(ndev);

	strcpy(extra, adev->nick);
	dwrq->length = strlen(extra) + 1;

	return OK;
}


/***********************************************************************
** acx_ioctl_set_nick
*/
static int
acx_ioctl_set_nick(struct net_device *ndev,
		   struct iw_request_info *info,
		   union iwreq_data *wrqu, char *extra)
{
	struct iw_point *dwrq = &wrqu->data;
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

#if WIRELESS_EXT >= 21
	/* WE 21 gives real ESSID strlen, not +1 (trailing zero):
	 * see LKML "[patch] drivers/net/wireless: correct reported ssid lengths" */
	len += 1;
#endif

	acx_sem_lock(adev);

	if (dwrq->length > IW_ESSID_MAX_SIZE + 1) {
		result = -E2BIG;
		goto end_unlock;
	}

	/* extra includes trailing \0, so it's ok */
	strcpy(adev->nick, extra);
	result = OK;

      end_unlock:
	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_retry
*/
static int
acx_ioctl_get_retry(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->retry;
	acx_device_t *adev = ndev2adev(ndev);
	unsigned int type = vwrq->flags & IW_RETRY_TYPE;
	unsigned int modifier = vwrq->flags & IW_RETRY_MODIFIER;
	int result;

	FN_ENTER;

	acx_sem_lock(adev);

	/* return the short retry number by default */
	if (type == IW_RETRY_LIFETIME) {
		vwrq->flags = IW_RETRY_LIFETIME;
		vwrq->value = adev->msdu_lifetime;
	} else if (modifier == IW_RETRY_MAX) {
		vwrq->flags = IW_RETRY_LIMIT | IW_RETRY_MAX;
		vwrq->value = adev->long_retry;
	} else {
		vwrq->flags = IW_RETRY_LIMIT;
		if (adev->long_retry != adev->short_retry)
			SET_BIT(vwrq->flags, IW_RETRY_MIN);
		vwrq->value = adev->short_retry;
	}

	/* can't be disabled */
	vwrq->disabled = (u8) 0;
	result = OK;

	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_set_retry
*/
static int
acx_ioctl_set_retry(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->retry;
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	if (!vwrq) {
		result = -EFAULT;
		goto end;
	}
	if (vwrq->disabled) {
		result = -EINVAL;
		goto end;
	}

	acx_sem_lock(adev);

	result = -EINVAL;
	if (IW_RETRY_LIMIT == (vwrq->flags & IW_RETRY_TYPE)) {
		printk("old retry limits: short %d long %d\n",
		       adev->short_retry, adev->long_retry);
		if (vwrq->flags & IW_RETRY_MAX) {
			adev->long_retry = vwrq->value;
		} else if (vwrq->flags & IW_RETRY_MIN) {
			adev->short_retry = vwrq->value;
		} else {
			/* no modifier: set both */
			adev->long_retry = vwrq->value;
			adev->short_retry = vwrq->value;
		}
		printk("new retry limits: short %d long %d\n",
		       adev->short_retry, adev->long_retry);
		SET_BIT(adev->set_mask, GETSET_RETRY);
		result = -EINPROGRESS;
	} else if (vwrq->flags & IW_RETRY_LIFETIME) {
		adev->msdu_lifetime = vwrq->value;
		printk("new MSDU lifetime: %d\n", adev->msdu_lifetime);
		SET_BIT(adev->set_mask, SET_MSDU_LIFETIME);
		result = -EINPROGRESS;
	}

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/************************ private ioctls ******************************/


/***********************************************************************
** acx_ioctl_set_debug
*/
#if ACX_DEBUG
static int
acx_ioctl_set_debug(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	unsigned int debug_new = *((unsigned int *)extra);
	int result = -EINVAL;

	log(L_ANY, "setting debug from %04X to %04X\n", acx_debug, debug_new);
	acx_debug = debug_new;

	result = OK;
	return result;

}
#endif


/***********************************************************************
** acx_ioctl_list_reg_domain
*/
static int
acx_ioctl_list_reg_domain(struct net_device *ndev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	int i = 1;
	const char *const *entry = acx_reg_domain_strings;

	printk("dom# chan# domain/country\n");
	while (*entry)
		printk("%4d %s\n", i++, *entry++);
	return OK;
}


/***********************************************************************
** acx_ioctl_set_reg_domain
*/
static int
acx_ioctl_set_reg_domain(struct net_device *ndev,
			 struct iw_request_info *info,
			 union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	if ((*extra < 1) || ((size_t) * extra > acx_reg_domain_ids_len)) {
		result = -EINVAL;
		goto end;
	}

	acx_sem_lock(adev);

	adev->reg_dom_id = acx_reg_domain_ids[*extra - 1];
	SET_BIT(adev->set_mask, GETSET_REG_DOMAIN);

	result = -EINPROGRESS;

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_reg_domain
*/
static int
acx_ioctl_get_reg_domain(struct net_device *ndev,
			 struct iw_request_info *info,
			 union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int dom, i;

	/* no locking */
	dom = adev->reg_dom_id;

	for (i = 1; i <= acx_reg_domain_ids_len; i++) {
		if (acx_reg_domain_ids[i - 1] == dom) {
			log(L_IOCTL, "regulatory domain is currently set "
			    "to %d (0x%X): %s\n", i, dom,
			    acx_reg_domain_strings[i - 1]);
			*extra = i;
			break;
		}
	}

	return OK;
}


/***********************************************************************
** acx_ioctl_set_short_preamble
*/
static const char *const
 preamble_modes[] = {
	"off",
	"on",
	"auto (peer capability dependent)",
	"unknown mode, error"
};

static int
acx_ioctl_set_short_preamble(struct net_device *ndev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int i;
	int result;

	FN_ENTER;

	if ((unsigned char)*extra > 2) {
		result = -EINVAL;
		goto end;
	}

	acx_sem_lock(adev);

	adev->preamble_mode = (u8) * extra;
	switch (adev->preamble_mode) {
	case 0:		/* long */
		adev->preamble_cur = 0;
		break;
	case 1:
		/* short, kick incapable peers */
		adev->preamble_cur = 1;
		for (i = 0; i < ARRAY_SIZE(adev->sta_list); i++) {
			client_t *clt = &adev->sta_list[i];
			if (!clt->used)
				continue;
			if (!(clt->cap_info & WF_MGMT_CAP_SHORT)) {
				clt->used = CLIENT_EMPTY_SLOT_0;
			}
		}
		switch (adev->mode) {
		case ACX_MODE_2_STA:
			if (adev->ap_client && !adev->ap_client->used) {
				/* We kicked our AP :) */
				SET_BIT(adev->set_mask, GETSET_RESCAN);
			}
		}
		break;
	case 2:		/* auto. short only if all peers are short-capable */
		adev->preamble_cur = 1;
		for (i = 0; i < ARRAY_SIZE(adev->sta_list); i++) {
			client_t *clt = &adev->sta_list[i];
			if (!clt->used)
				continue;
			if (!(clt->cap_info & WF_MGMT_CAP_SHORT)) {
				adev->preamble_cur = 0;
				break;
			}
		}
		break;
	}
	printk("new short preamble setting: configured %s, active %s\n",
	       preamble_modes[adev->preamble_mode],
	       preamble_modes[adev->preamble_cur]);
	result = OK;

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_short_preamble
*/
static int
acx_ioctl_get_short_preamble(struct net_device *ndev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);

	acx_sem_lock(adev);

	printk("current short preamble setting: configured %s, active %s\n",
	       preamble_modes[adev->preamble_mode],
	       preamble_modes[adev->preamble_cur]);

	*extra = (char)adev->preamble_mode;

	acx_sem_unlock(adev);

	return OK;
}


/***********************************************************************
** acx_ioctl_set_antenna
**
** TX and RX antenna can be set separately but this function good
** for testing 0-4 bits
*/
static int
acx_ioctl_set_antenna(struct net_device *ndev,
		      struct iw_request_info *info,
		      union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);

	acx_sem_lock(adev);

	printk("old antenna value: 0x%02X (COMBINED bit mask)\n" "Rx antenna selection:\n" "0x00 ant. 1\n" "0x40 ant. 2\n" "0x80 full diversity\n" "0xc0 partial diversity\n" "0x0f dwell time mask (in units of us)\n" "Tx antenna selection:\n" "0x00 ant. 2\n"	/* yep, those ARE reversed! */
	       "0x20 ant. 1\n"
	       "new antenna value: 0x%02X\n", adev->antenna, (u8) * extra);

	adev->antenna = (u8) * extra;
	SET_BIT(adev->set_mask, GETSET_ANTENNA);

	acx_sem_unlock(adev);

	return -EINPROGRESS;
}


/***********************************************************************
** acx_ioctl_get_antenna
*/
static int
acx_ioctl_get_antenna(struct net_device *ndev,
		      struct iw_request_info *info,
		      union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);

	/* no locking. it's pointless to lock a single load */
	printk("current antenna value: 0x%02X (COMBINED bit mask)\n" "Rx antenna selection:\n" "0x00 ant. 1\n" "0x40 ant. 2\n" "0x80 full diversity\n" "0xc0 partial diversity\n" "Tx antenna selection:\n" "0x00 ant. 2\n"	/* yep, those ARE reversed! */
	       "0x20 ant. 1\n", adev->antenna);

	return 0;
}


/***********************************************************************
** acx_ioctl_set_rx_antenna
**
** 0 = antenna1; 1 = antenna2; 2 = full diversity; 3 = partial diversity
** Could anybody test which antenna is the external one?
*/
static int
acx_ioctl_set_rx_antenna(struct net_device *ndev,
			 struct iw_request_info *info,
			 union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	if (*extra > 3) {
		result = -EINVAL;
		goto end;
	}

	printk("old antenna value: 0x%02X\n", adev->antenna);

	acx_sem_lock(adev);

	adev->antenna &= 0x3f;
	SET_BIT(adev->antenna, (*extra << 6));
	SET_BIT(adev->set_mask, GETSET_ANTENNA);
	printk("new antenna value: 0x%02X\n", adev->antenna);
	result = -EINPROGRESS;

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_set_tx_antenna
**
** Arguments: 0 == antenna2; 1 == antenna1;
** Could anybody test which antenna is the external one?
*/
static int
acx_ioctl_set_tx_antenna(struct net_device *ndev,
			 struct iw_request_info *info,
			 union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	FN_ENTER;

	if (*extra > 1) {
		result = -EINVAL;
		goto end;
	}

	printk("old antenna value: 0x%02X\n", adev->antenna);

	acx_sem_lock(adev);

	adev->antenna &= ~0x30;
	SET_BIT(adev->antenna, ((*extra & 0x01) << 5));
	SET_BIT(adev->set_mask, GETSET_ANTENNA);
	printk("new antenna value: 0x%02X\n", adev->antenna);
	result = -EINPROGRESS;

	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_wlansniff
**
** can we just remove this in favor of monitor mode? --vda
*/
static int
acx_ioctl_wlansniff(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	unsigned int *params = (unsigned int *)extra;
	unsigned int enable = (unsigned int)(params[0] > 0);
	int result;

	FN_ENTER;

	acx_sem_lock(adev);

	/* not using printk() here, since it distorts kismet display
	 * when printk messages activated */
	log(L_IOCTL, "setting monitor to: 0x%02X\n", params[0]);

	switch (params[0]) {
	case 0:
		/* no monitor mode. hmm, should we simply ignore it
		 * or go back to enabling adev->netdev->type ARPHRD_ETHER? */
		break;
	case 1:
		adev->monitor_type = ARPHRD_IEEE80211_PRISM;
		break;
	case 2:
		adev->monitor_type = ARPHRD_IEEE80211;
		break;
	}

	if (params[0]) {
		adev->mode = ACX_MODE_MONITOR;
		SET_BIT(adev->set_mask, GETSET_MODE);
	}

	if (enable) {
		adev->channel = params[1];
		SET_BIT(adev->set_mask, GETSET_RX);
	}
	result = -EINPROGRESS;

	acx_sem_unlock(adev);

	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_unknown11
** FIXME: looks like some sort of "iwpriv kick_sta MAC" but it's broken
*/
static int
acx_ioctl_unknown11(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
#ifdef BROKEN
	struct iw_param *vwrq = &wrqu->param;
	acx_device_t *adev = ndev2adev(ndev);
	unsigned long flags;
	client_t client;
	int result;

	acx_sem_lock(adev);
	acx_lock(adev, flags);

	acx_l_transmit_disassoc(adev, &client);
	result = OK;

	acx_unlock(adev, flags);
	acx_sem_unlock(adev);

	return result;
#endif
	return -EINVAL;
}


/***********************************************************************
** debug helper function to be able to debug various issues relatively easily
*/
static int
acx_ioctl_dbg_set_masks(struct net_device *ndev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	const unsigned int *params = (unsigned int *)extra;
	int result;

	acx_sem_lock(adev);

	log(L_IOCTL, "setting flags in settings mask: "
	    "get_mask %08X set_mask %08X\n"
	    "before: get_mask %08X set_mask %08X\n",
	    params[0], params[1], adev->get_mask, adev->set_mask);
	SET_BIT(adev->get_mask, params[0]);
	SET_BIT(adev->set_mask, params[1]);
	log(L_IOCTL, "after: get_mask %08X set_mask %08X\n",
	    adev->get_mask, adev->set_mask);
	result = -EINPROGRESS;	/* immediately call commit handler */

	acx_sem_unlock(adev);

	return result;
}


/***********************************************************************
* acx_ioctl_set_rates
*
* This ioctl takes string parameter. Examples:
* iwpriv wlan0 SetRates "1,2"
*	use 1 and 2 Mbit rates, both are in basic rate set
* iwpriv wlan0 SetRates "1,2 5,11"
*	use 1,2,5.5,11 Mbit rates. 1 and 2 are basic
* iwpriv wlan0 SetRates "1,2 5c,11c"
*	same ('c' means 'CCK modulation' and it is a default for 5 and 11)
* iwpriv wlan0 SetRates "1,2 5p,11p"
*	use 1,2,5.5,11 Mbit, 1,2 are basic. 5 and 11 are using PBCC
* iwpriv wlan0 SetRates "1,2,5,11 22p"
*	use 1,2,5.5,11,22 Mbit. 1,2,5.5 and 11 are basic. 22 is using PBCC
*	(this is the maximum acx100 can do (modulo x4 mode))
* iwpriv wlan0 SetRates "1,2,5,11 22"
*	same. 802.11 defines only PBCC modulation
*	for 22 and 33 Mbit rates, so there is no ambiguity
* iwpriv wlan0 SetRates "1,2,5,11 6o,9o,12o,18o,24o,36o,48o,54o"
*	1,2,5.5 and 11 are basic. 11g OFDM rates are enabled but
*	they are not in basic rate set.	22 Mbit is disabled.
* iwpriv wlan0 SetRates "1,2,5,11 6,9,12,18,24,36,48,54"
*	same. OFDM is default for 11g rates except 22 and 33 Mbit,
*	thus 'o' is optional
* iwpriv wlan0 SetRates "1,2,5,11 6d,9d,12d,18d,24d,36d,48d,54d"
*	1,2,5.5 and 11 are basic. 11g CCK-OFDM rates are enabled
*	(acx111 does not support CCK-OFDM, driver will reject this cmd)
* iwpriv wlan0 SetRates "6,9,12 18,24,36,48,54"
*	6,9,12 are basic, rest of 11g rates is enabled. Using OFDM
*/
#include "setrate.c"

/* disallow: 33Mbit (unsupported by hw) */
/* disallow: CCKOFDM (unsupported by hw) */
static int acx111_supported(int mbit, int modulation, void *opaque)
{
	if (mbit == 33)
		return -ENOTSUPP;
	if (modulation == DOT11_MOD_CCKOFDM)
		return -ENOTSUPP;
	return OK;
}

static const u16 acx111mask[] = {
	[DOT11_RATE_1] = RATE111_1,
	[DOT11_RATE_2] = RATE111_2,
	[DOT11_RATE_5] = RATE111_5,
	[DOT11_RATE_11] = RATE111_11,
	[DOT11_RATE_22] = RATE111_22,
	/* [DOT11_RATE_33] = */
	[DOT11_RATE_6] = RATE111_6,
	[DOT11_RATE_9] = RATE111_9,
	[DOT11_RATE_12] = RATE111_12,
	[DOT11_RATE_18] = RATE111_18,
	[DOT11_RATE_24] = RATE111_24,
	[DOT11_RATE_36] = RATE111_36,
	[DOT11_RATE_48] = RATE111_48,
	[DOT11_RATE_54] = RATE111_54,
};

static u32 acx111_gen_mask(int mbit, int modulation, void *opaque)
{
	/* lower 16 bits show selected 1, 2, CCK and OFDM rates */
	/* upper 16 bits show selected PBCC rates */
	u32 m = acx111mask[rate_mbit2enum(mbit)];
	if (modulation == DOT11_MOD_PBCC)
		return m << 16;
	return m;
}

static int verify_rate(u32 rate, int chip_type)
{
	/* never happens. be paranoid */
	if (!rate)
		return -EINVAL;

	/* disallow: mixing PBCC and CCK at 5 and 11Mbit
	 ** (can be supported, but needs complicated handling in tx code) */
	if ((rate & ((RATE111_11 + RATE111_5) << 16))
	    && (rate & (RATE111_11 + RATE111_5))
	    ) {
		return -ENOTSUPP;
	}
	if (CHIPTYPE_ACX100 == chip_type) {
		if (rate &
		    ~(RATE111_ACX100_COMPAT + (RATE111_ACX100_COMPAT << 16)))
			return -ENOTSUPP;
	}
	return 0;
}

static int
acx_ioctl_set_rates(struct net_device *ndev,
		    struct iw_request_info *info,
		    union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	unsigned long flags;
	int result;
	u32 brate = 0, orate = 0;	/* basic, operational rate set */

	FN_ENTER;

	log(L_IOCTL, "set_rates %s\n", extra);
	result = fill_ratemasks(extra, &brate, &orate,
				acx111_supported, acx111_gen_mask, NULL);
	if (result)
		goto end;
	SET_BIT(orate, brate);
	log(L_IOCTL, "brate %08X orate %08X\n", brate, orate);

	result = verify_rate(brate, adev->chip_type);
	if (result)
		goto end;
	result = verify_rate(orate, adev->chip_type);
	if (result)
		goto end;

	acx_sem_lock(adev);
	acx_lock(adev, flags);

	adev->rate_basic = brate;
	adev->rate_oper = orate;
	/* TODO: ideally, we shall monitor highest basic rate
	 ** which was successfully sent to every peer
	 ** (say, last we checked, everybody could hear 5.5 Mbits)
	 ** and use that for bcasts when we want to reach all peers.
	 ** For beacons, we probably shall use lowest basic rate
	 ** because we want to reach all *potential* new peers too */
	adev->rate_bcast = 1 << lowest_bit(brate);
	if (IS_ACX100(adev))
		adev->rate_bcast100 = acx_rate111to100(adev->rate_bcast);
	adev->rate_auto = !has_only_one_bit(orate);
	acx_l_update_client_rates(adev, orate);
	/* TODO: get rid of ratevector, build it only when needed */
	acx_l_update_ratevector(adev);

	/* Do/don't do tx rate fallback; beacon contents and rate */
	SET_BIT(adev->set_mask, SET_RATE_FALLBACK | SET_TEMPLATES);
	result = -EINPROGRESS;

	acx_unlock(adev, flags);
	acx_sem_unlock(adev);
      end:
	FN_EXIT1(result);
	return result;
}


/***********************************************************************
** acx_ioctl_get_phy_chan_busy_percentage
*/
static int
acx_ioctl_get_phy_chan_busy_percentage(struct net_device *ndev,
				       struct iw_request_info *info,
				       union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	struct {
		u16 type;
		u16 len;
		u32 busytime;
		u32 totaltime;
	} ACX_PACKED usage;
	int result;

	acx_sem_lock(adev);

	if (OK != acx_s_interrogate(adev, &usage, ACX1xx_IE_MEDIUM_USAGE)) {
		result = NOT_OK;
		goto end_unlock;
	}

	usage.busytime = le32_to_cpu(usage.busytime);
	usage.totaltime = le32_to_cpu(usage.totaltime);
	printk("%s: busy percentage of medium since last invocation: %d%% "
	       "(%u of %u microseconds)\n",
	       ndev->name,
	       usage.busytime / ((usage.totaltime / 100) + 1),
	       usage.busytime, usage.totaltime);

	result = OK;

      end_unlock:
	acx_sem_unlock(adev);

	return result;
}


/***********************************************************************
** acx_ioctl_set_ed_threshold
*/
static inline int
acx_ioctl_set_ed_threshold(struct net_device *ndev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);

	acx_sem_lock(adev);

	printk("old ED threshold value: %d\n", adev->ed_threshold);
	adev->ed_threshold = (unsigned char)*extra;
	printk("new ED threshold value: %d\n", (unsigned char)*extra);
	SET_BIT(adev->set_mask, GETSET_ED_THRESH);

	acx_sem_unlock(adev);

	return -EINPROGRESS;
}


/***********************************************************************
** acx_ioctl_set_cca
*/
static inline int
acx_ioctl_set_cca(struct net_device *ndev,
		  struct iw_request_info *info,
		  union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;

	acx_sem_lock(adev);

	printk("old CCA value: 0x%02X\n", adev->cca);
	adev->cca = (unsigned char)*extra;
	printk("new CCA value: 0x%02X\n", (unsigned char)*extra);
	SET_BIT(adev->set_mask, GETSET_CCA);
	result = -EINPROGRESS;

	acx_sem_unlock(adev);

	return result;
}


/***********************************************************************
*/
static const char *const
 scan_modes[] = { "active", "passive", "background" };

static void acx_print_scan_params(acx_device_t * adev, const char *head)
{
	printk("%s: %smode %d (%s), min chan time %dTU, "
	       "max chan time %dTU, max scan rate byte: %d\n",
	       adev->ndev->name, head,
	       adev->scan_mode, scan_modes[adev->scan_mode],
	       adev->scan_probe_delay, adev->scan_duration, adev->scan_rate);
}

static int
acx_ioctl_set_scan_params(struct net_device *ndev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;
	const int *params = (int *)extra;

	acx_sem_lock(adev);

	acx_print_scan_params(adev, "old scan parameters: ");
	if ((params[0] != -1) && (params[0] >= 0) && (params[0] <= 2))
		adev->scan_mode = params[0];
	if (params[1] != -1)
		adev->scan_probe_delay = params[1];
	if (params[2] != -1)
		adev->scan_duration = params[2];
	if ((params[3] != -1) && (params[3] <= 255))
		adev->scan_rate = params[3];
	acx_print_scan_params(adev, "new scan parameters: ");
	SET_BIT(adev->set_mask, GETSET_RESCAN);
	result = -EINPROGRESS;

	acx_sem_unlock(adev);

	return result;
}

static int
acx_ioctl_get_scan_params(struct net_device *ndev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);
	int result;
	int *params = (int *)extra;

	acx_sem_lock(adev);

	acx_print_scan_params(adev, "current scan parameters: ");
	params[0] = adev->scan_mode;
	params[1] = adev->scan_probe_delay;
	params[2] = adev->scan_duration;
	params[3] = adev->scan_rate;
	result = OK;

	acx_sem_unlock(adev);

	return result;
}


/***********************************************************************
*/
static int
acx100_ioctl_set_led_power(struct net_device *ndev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	static const char *const led_modes[] = { "off", "on", "LinkQuality" };

	acx_device_t *adev = ndev2adev(ndev);
	int result;

	acx_sem_lock(adev);

	printk("%s: power LED status: old %d (%s), ",
	       ndev->name, adev->led_power, led_modes[adev->led_power]);
	adev->led_power = extra[0];
	if (adev->led_power > 2)
		adev->led_power = 2;
	printk("new %d (%s)\n", adev->led_power, led_modes[adev->led_power]);

	if (adev->led_power == 2) {
		printk("%s: max link quality setting: old %d, ",
		       ndev->name, adev->brange_max_quality);
		if (extra[1])
			adev->brange_max_quality = extra[1];
		printk("new %d\n", adev->brange_max_quality);
	}

	SET_BIT(adev->set_mask, GETSET_LED_POWER);

	result = -EINPROGRESS;

	acx_sem_unlock(adev);

	return result;
}


/***********************************************************************
*/
static inline int
acx100_ioctl_get_led_power(struct net_device *ndev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	acx_device_t *adev = ndev2adev(ndev);

	acx_sem_lock(adev);

	extra[0] = adev->led_power;
	if (adev->led_power == 2)
		extra[1] = adev->brange_max_quality;
	else
		extra[1] = -1;

	acx_sem_unlock(adev);

	return OK;
}


/***********************************************************************
*/
static int
acx111_ioctl_info(struct net_device *ndev,
		  struct iw_request_info *info,
		  union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->param;
	if (!IS_PCI(ndev2adev(ndev)))
		return OK;
	return acx111pci_ioctl_info(ndev, info, vwrq, extra);
}


/***********************************************************************
*/
static int
acx100_ioctl_set_phy_amp_bias(struct net_device *ndev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu, char *extra)
{
	struct iw_param *vwrq = &wrqu->param;
	if (!IS_PCI(ndev2adev(ndev))) {
		printk("acx: set_phy_amp_bias() is not supported on USB\n");
		return OK;
	}
	return acx100pci_ioctl_set_phy_amp_bias(ndev, info, vwrq, extra);
}

/***********************************************************************
*/

/* if you plan to reorder something, make sure to reorder all other places
 * accordingly! */
/* SET/GET convention: SETs must have even position, GETs odd */
#define ACX100_IOCTL SIOCIWFIRSTPRIV
enum {
	ACX100_IOCTL_DEBUG = ACX100_IOCTL,
	ACX100_IOCTL_GET__________UNUSED1,
	ACX100_IOCTL_SET_PLED,
	ACX100_IOCTL_GET_PLED,
	ACX100_IOCTL_SET_RATES,
	ACX100_IOCTL_LIST_DOM,
	ACX100_IOCTL_SET_DOM,
	ACX100_IOCTL_GET_DOM,
	ACX100_IOCTL_SET_SCAN_PARAMS,
	ACX100_IOCTL_GET_SCAN_PARAMS,
	ACX100_IOCTL_SET_PREAMB,
	ACX100_IOCTL_GET_PREAMB,
	ACX100_IOCTL_SET_ANT,
	ACX100_IOCTL_GET_ANT,
	ACX100_IOCTL_RX_ANT,
	ACX100_IOCTL_TX_ANT,
	ACX100_IOCTL_SET_PHY_AMP_BIAS,
	ACX100_IOCTL_GET_PHY_CHAN_BUSY,
	ACX100_IOCTL_SET_ED,
	ACX100_IOCTL_GET__________UNUSED3,
	ACX100_IOCTL_SET_CCA,
	ACX100_IOCTL_GET__________UNUSED4,
	ACX100_IOCTL_MONITOR,
	ACX100_IOCTL_TEST,
	ACX100_IOCTL_DBG_SET_MASKS,
	ACX111_IOCTL_INFO,
	ACX100_IOCTL_DBG_SET_IO,
	ACX100_IOCTL_DBG_GET_IO
};


static const iw_handler acx_ioctl_private_handler[] = {
#if ACX_DEBUG
	[ACX100_IOCTL_DEBUG - ACX100_IOCTL] = acx_ioctl_set_debug,
#endif
	[ACX100_IOCTL_SET_PLED - ACX100_IOCTL] = acx100_ioctl_set_led_power,
	[ACX100_IOCTL_GET_PLED - ACX100_IOCTL] = acx100_ioctl_get_led_power,
	[ACX100_IOCTL_SET_RATES - ACX100_IOCTL] = acx_ioctl_set_rates,
	[ACX100_IOCTL_LIST_DOM - ACX100_IOCTL] = acx_ioctl_list_reg_domain,
	[ACX100_IOCTL_SET_DOM - ACX100_IOCTL] = acx_ioctl_set_reg_domain,
	[ACX100_IOCTL_GET_DOM - ACX100_IOCTL] = acx_ioctl_get_reg_domain,
	[ACX100_IOCTL_SET_SCAN_PARAMS - ACX100_IOCTL] =
	    acx_ioctl_set_scan_params,
	[ACX100_IOCTL_GET_SCAN_PARAMS - ACX100_IOCTL] =
	    acx_ioctl_get_scan_params,
	[ACX100_IOCTL_SET_PREAMB - ACX100_IOCTL] = acx_ioctl_set_short_preamble,
	[ACX100_IOCTL_GET_PREAMB - ACX100_IOCTL] = acx_ioctl_get_short_preamble,
	[ACX100_IOCTL_SET_ANT - ACX100_IOCTL] = acx_ioctl_set_antenna,
	[ACX100_IOCTL_GET_ANT - ACX100_IOCTL] = acx_ioctl_get_antenna,
	[ACX100_IOCTL_RX_ANT - ACX100_IOCTL] = acx_ioctl_set_rx_antenna,
	[ACX100_IOCTL_TX_ANT - ACX100_IOCTL] = acx_ioctl_set_tx_antenna,
	[ACX100_IOCTL_SET_PHY_AMP_BIAS - ACX100_IOCTL] =
	    acx100_ioctl_set_phy_amp_bias,
	[ACX100_IOCTL_GET_PHY_CHAN_BUSY - ACX100_IOCTL] =
	    acx_ioctl_get_phy_chan_busy_percentage,
	[ACX100_IOCTL_SET_ED - ACX100_IOCTL] = acx_ioctl_set_ed_threshold,
	[ACX100_IOCTL_SET_CCA - ACX100_IOCTL] = acx_ioctl_set_cca,
	[ACX100_IOCTL_MONITOR - ACX100_IOCTL] = acx_ioctl_wlansniff,
	[ACX100_IOCTL_TEST - ACX100_IOCTL] = acx_ioctl_unknown11,
	[ACX100_IOCTL_DBG_SET_MASKS - ACX100_IOCTL] = acx_ioctl_dbg_set_masks,
	[ACX111_IOCTL_INFO - ACX100_IOCTL] = acx111_ioctl_info,
};


static const struct iw_priv_args acx_ioctl_private_args[] = {
#if ACX_DEBUG
      {cmd:ACX100_IOCTL_DEBUG,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetDebug"},
#endif
      {cmd:ACX100_IOCTL_SET_PLED,
	      set_args:IW_PRIV_TYPE_BYTE | 2,
	      get_args:0,
      name:"SetLEDPower"},
      {cmd:ACX100_IOCTL_GET_PLED,
	      set_args:0,
	      get_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 2,
      name:"GetLEDPower"},
      {cmd:ACX100_IOCTL_SET_RATES,
	      set_args:IW_PRIV_TYPE_CHAR | 256,
	      get_args:0,
      name:"SetRates"},
      {cmd:ACX100_IOCTL_LIST_DOM,
	      set_args:0,
	      get_args:0,
      name:"ListRegDomain"},
      {cmd:ACX100_IOCTL_SET_DOM,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetRegDomain"},
      {cmd:ACX100_IOCTL_GET_DOM,
	      set_args:0,
	      get_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
      name:"GetRegDomain"},
      {cmd:ACX100_IOCTL_SET_SCAN_PARAMS,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
	      get_args:0,
      name:"SetScanParams"},
      {cmd:ACX100_IOCTL_GET_SCAN_PARAMS,
	      set_args:0,
	      get_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
      name:"GetScanParams"},
      {cmd:ACX100_IOCTL_SET_PREAMB,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetSPreamble"},
      {cmd:ACX100_IOCTL_GET_PREAMB,
	      set_args:0,
	      get_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
      name:"GetSPreamble"},
      {cmd:ACX100_IOCTL_SET_ANT,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetAntenna"},
      {cmd:ACX100_IOCTL_GET_ANT,
	      set_args:0,
	      get_args:0,
      name:"GetAntenna"},
      {cmd:ACX100_IOCTL_RX_ANT,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetRxAnt"},
      {cmd:ACX100_IOCTL_TX_ANT,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetTxAnt"},
      {cmd:ACX100_IOCTL_SET_PHY_AMP_BIAS,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetPhyAmpBias"},
      {cmd:ACX100_IOCTL_GET_PHY_CHAN_BUSY,
	      set_args:0,
	      get_args:0,
      name:"GetPhyChanBusy"},
      {cmd:ACX100_IOCTL_SET_ED,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetED"},
      {cmd:ACX100_IOCTL_SET_CCA,
	      set_args:IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1,
	      get_args:0,
      name:"SetCCA"},
      {cmd:ACX100_IOCTL_MONITOR,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
	      get_args:0,
      name:"monitor"},
      {cmd:ACX100_IOCTL_TEST,
	      set_args:0,
	      get_args:0,
      name:"Test"},
      {cmd:ACX100_IOCTL_DBG_SET_MASKS,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
	      get_args:0,
      name:"DbgSetMasks"},
      {cmd:ACX111_IOCTL_INFO,
	      set_args:0,
	      get_args:0,
      name:"GetAcx111Info"},
      {cmd:ACX100_IOCTL_DBG_SET_IO,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 4,
	      get_args:0,
      name:"DbgSetIO"},
      {cmd:ACX100_IOCTL_DBG_GET_IO,
	      set_args:IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,
	      get_args:0,
      name:"DbgGetIO"},
};


const struct iw_handler_def acx_ioctl_handler_def = {
	.num_standard = ARRAY_SIZE(acx_ioctl_handler),
	.num_private = ARRAY_SIZE(acx_ioctl_private_handler),
	.num_private_args = ARRAY_SIZE(acx_ioctl_private_args),
	.standard = (iw_handler *) acx_ioctl_handler,
	.private = (iw_handler *) acx_ioctl_private_handler,
	.private_args = (struct iw_priv_args *)acx_ioctl_private_args,
#if IW_HANDLER_VERSION > 5
	.get_wireless_stats = acx_e_get_wireless_stats
#endif /* IW > 5 */
};
