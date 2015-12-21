/*
 * Copyright (c) 2009-2011 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef BTCOEX_H
#define BTCOEX_H

#include "hw.h"

#define ATH_WLANACTIVE_GPIO_9280     5
#define ATH_BTACTIVE_GPIO_9280       6
#define ATH_BTPRIORITY_GPIO_9285     7

#define ATH_WLANACTIVE_GPIO_9300     5
#define ATH_BTACTIVE_GPIO_9300       4
#define ATH_BTPRIORITY_GPIO_9300     8

#define ATH_BTCOEX_DEF_BT_PERIOD  45
#define ATH_BTCOEX_DEF_DUTY_CYCLE 55
#define ATH_BTCOEX_BTSCAN_DUTY_CYCLE 90
#define ATH_BTCOEX_BMISS_THRESH   50

#define ATH_BT_PRIORITY_TIME_THRESHOLD 1000 
#define ATH_BT_CNT_THRESHOLD	       3
#define ATH_BT_CNT_SCAN_THRESHOLD      15

#define AR9300_NUM_BT_WEIGHTS   4
#define AR9300_NUM_WLAN_WEIGHTS 4
enum ath_stomp_type {
	ATH_BTCOEX_STOMP_ALL,
	ATH_BTCOEX_STOMP_LOW,
	ATH_BTCOEX_STOMP_NONE,
	ATH_BTCOEX_STOMP_LOW_FTP,
	ATH_BTCOEX_STOMP_MAX
};

enum ath_btcoex_scheme {
	ATH_BTCOEX_CFG_NONE,
	ATH_BTCOEX_CFG_2WIRE,
	ATH_BTCOEX_CFG_3WIRE,
	ATH_BTCOEX_CFG_MCI,
};

struct ath9k_hw_mci {
	u32 raw_intr;
	u32 rx_msg_intr;
	u32 cont_status;
	u32 gpm_addr;
	u32 gpm_len;
	u32 gpm_idx;
	u32 sched_addr;
	u32 wlan_channels[4];
	u32 wlan_cal_seq;
	u32 wlan_cal_done;
	u32 config;
	u8 *gpm_buf;
	bool ready;
	bool update_2g5g;
	bool is_2g;
	bool query_bt;
	bool unhalt_bt_gpm; 
	bool halted_bt_gpm; 
	bool need_flush_btinfo;
	bool bt_version_known;
	bool wlan_channels_update;
	u8 wlan_ver_major;
	u8 wlan_ver_minor;
	u8 bt_ver_major;
	u8 bt_ver_minor;
	u8 bt_state;
};

struct ath_btcoex_hw {
	enum ath_btcoex_scheme scheme;
	struct ath9k_hw_mci mci;
	bool enabled;
	u8 wlanactive_gpio;
	u8 btactive_gpio;
	u8 btpriority_gpio;
	u32 bt_coex_mode; 	
	u32 bt_coex_weights; 	
	u32 bt_coex_mode2; 	
	u32 bt_weight[AR9300_NUM_BT_WEIGHTS];
	u32 wlan_weight[AR9300_NUM_WLAN_WEIGHTS];
};

void ath9k_hw_btcoex_init_scheme(struct ath_hw *ah);
void ath9k_hw_btcoex_init_2wire(struct ath_hw *ah);
void ath9k_hw_btcoex_init_3wire(struct ath_hw *ah);
void ath9k_hw_btcoex_init_mci(struct ath_hw *ah);
void ath9k_hw_init_btcoex_hw(struct ath_hw *ah, int qnum);
void ath9k_hw_btcoex_set_weight(struct ath_hw *ah,
				u32 bt_weight,
				u32 wlan_weight);
void ath9k_hw_btcoex_disable(struct ath_hw *ah);
void ath9k_hw_btcoex_bt_stomp(struct ath_hw *ah,
			      enum ath_stomp_type stomp_type);

#endif
