/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
 
#ifdef CONFIG_HUAWEI_KERNEL
/* Open debug log for development version, will be closed after TR5 */
#ifdef CONFIG_DYNAMIC_DEBUG
#undef CONFIG_DYNAMIC_DEBUG
#endif
#ifndef DEBUG
#define DEBUG
#endif
#endif
#include <linux/module.h>
#include <sound/hw_audio_log.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/mfd/wcd9xxx/pdata.h>
#include <linux/firmware.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/jack.h>
#include "wcd-mbhc-v2.h"
#include "wcd9xxx-mbhc.h"
#include "msm8x16_wcd_registers.h"
#include "msm8916-wcd-irq.h"
#include "msm8x16-wcd.h"
#include "wcdcal-hwdep.h"
#include <sound/hw_audio_info.h>

#define WCD_MBHC_JACK_MASK (SND_JACK_HEADSET | SND_JACK_OC_HPHL | \
			   SND_JACK_OC_HPHR | SND_JACK_LINEOUT | \
			   SND_JACK_UNSUPPORTED)
#ifdef CONFIG_HUAWEI_KERNEL
/* do not report the undesirable button type : btn3,btn4 */
#define WCD_MBHC_JACK_BUTTON_MASK (SND_JACK_BTN_0 | SND_JACK_BTN_1 | \
				  SND_JACK_BTN_2)
#define WCD_MBHC_JACK_BUTTON_MASK_HUAWEI_IGNORE (SND_JACK_BTN_3 | SND_JACK_BTN_4 | \
				  SND_JACK_BTN_5 | SND_JACK_BTN_6 | SND_JACK_BTN_7)
#else
#define WCD_MBHC_JACK_BUTTON_MASK (SND_JACK_BTN_0 | SND_JACK_BTN_1 | \
				  SND_JACK_BTN_2 | SND_JACK_BTN_3 | \
				  SND_JACK_BTN_4)
#endif
#define OCP_ATTEMPT 1
#define HS_DETECT_PLUG_TIME_MS (3 * 1000)
#define SPECIAL_HS_DETECT_TIME_MS (2 * 1000)
#define MBHC_BUTTON_PRESS_THRESHOLD_MIN 250
#define GND_MIC_SWAP_THRESHOLD 4
#define WCD_FAKE_REMOVAL_MIN_PERIOD_MS 100
#define HS_VREF_MIN_VAL 1400
#define FW_READ_ATTEMPTS 15
#define FW_READ_TIMEOUT 4000000
#define FAKE_REM_RETRY_ATTEMPTS 3       // RETRY TIME when Elect Remove happened

static int removal_st;

static int det_extn_cable_en = 0;
static u16 bad_jack_old_HW = 0;         //if HPHL have 499 ohm resistance this will be set to 1
static u16 mic_or_hphr_inserted = 0;    //if mic or HPHR detected when correct plug headset may be AT test device

#ifdef CONFIG_HUAWEI_DSM
static unsigned long timeout_jiffies = 0;
static bool ispress = false;
#endif
module_param(det_extn_cable_en, int,
		S_IRUGO | S_IWUSR | S_IWGRP);
MODULE_PARM_DESC(det_extn_cable_en, "enable/disable extn cable detect");

#define WCD_MBHC_RSC_LOCK(mbhc)			\
{							\
	ad_logn("%s: Acquiring BCL\n", __func__);	\
	mutex_lock(&mbhc->codec_resource_lock);		\
	ad_logn("%s: Acquiring BCL done\n", __func__);	\
}

#define WCD_MBHC_RSC_UNLOCK(mbhc)			\
{							\
	ad_logn("%s: Release BCL\n", __func__);	\
	mutex_unlock(&mbhc->codec_resource_lock);	\
}

#define WCD_MBHC_RSC_ASSERT_LOCKED(mbhc)		\
{							\
	WARN_ONCE(!mutex_is_locked(&mbhc->codec_resource_lock), \
		  "%s: BCL should have acquired\n", __func__); \
}

enum wcd_mbhc_cs_mb_en_flag {
	WCD_MBHC_EN_CS = 0,
	WCD_MBHC_EN_MB,
	WCD_MBHC_EN_PULLUP,
	WCD_MBHC_EN_NONE,
};

static void wcd_configure_cap(struct wcd_mbhc *mbhc, bool micbias2)
{
	u16 micbias1;
	struct snd_soc_codec *codec = mbhc->codec;

	micbias1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MICB_1_EN);
	ad_logd("\n %s: micbias1 %x micbias2 = %d\n", __func__, micbias1,
			micbias2);
	if ((micbias1 & 0x80) && micbias2) {
		if ((mbhc->micbias1_cap_mode == MICBIAS_EXT_BYP_CAP) ||
			(mbhc->micbias2_cap_mode == MICBIAS_EXT_BYP_CAP))
			snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x40, (MICBIAS_EXT_BYP_CAP << 6));
		else
			snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x40, (MICBIAS_NO_EXT_BYP_CAP << 6));
	} else if (micbias2) {
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x40, (mbhc->micbias2_cap_mode << 6));
	} else if (micbias1 & 0x80) {
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x40, (mbhc->micbias1_cap_mode << 6));
	} else {
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x40, 0x00);
	}
}

static void wcd_mbhc_jack_report(struct wcd_mbhc *mbhc,
				struct snd_soc_jack *jack, int status, int mask)
{
	/*
	 * Don't report LINEOUT event.
	 * If lineout event report device will show headset icon on the top of screen
	 * the icon will make uses confused even if music will play with spk.
	 */
	if((status != 0)&& det_extn_cable_en)
	{
		status &= ~SND_JACK_LINEOUT;
		ad_logn("%s: Do Not Reporting JACK_LINEOUT status:%x\n", __func__, status);
		if (0 == status)
		{
			return;
		}
	}
	snd_soc_jack_report_no_dapm(jack, status, mask);
}

static void __hphocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status,
				int irq)
{
	struct snd_soc_codec *codec;

	ad_logn("%s: clear ocp status %x\n", __func__, jack_status);
	codec = mbhc->codec;
	if (mbhc->hph_status & jack_status) {
		mbhc->hph_status &= ~jack_status;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				    mbhc->hph_status, WCD_MBHC_JACK_MASK);
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x00);

		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x10);
		/*
		 * reset retry counter as PA is turned off signifying
		 * start of new OCP detection session
		 */
		if (mbhc->intr_ids->hph_left_ocp)
			mbhc->hphlocp_cnt = 0;
		else
			mbhc->hphrocp_cnt = 0;
		wcd9xxx_spmi_enable_irq(irq);
	}
}

static void hphrocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status)
{
	__hphocp_off_report(mbhc, SND_JACK_OC_HPHR,
			    mbhc->intr_ids->hph_right_ocp);
}

static void hphlocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status)
{
	__hphocp_off_report(mbhc, SND_JACK_OC_HPHL,
			    mbhc->intr_ids->hph_left_ocp);
}

static void wcd_program_hs_vref(const struct wcd_mbhc *mbhc)
{
	struct wcd_mbhc_plug_type_cfg *plug_type_cfg;
	struct snd_soc_codec *codec = mbhc->codec;
	struct snd_soc_card *card = codec->card;
	u32 reg_val;

	plug_type_cfg = WCD_MBHC_CAL_PLUG_TYPE_PTR(mbhc->mbhc_cfg->calibration);
	reg_val = ((plug_type_cfg->v_hs_max - HS_VREF_MIN_VAL) / 100);

	ad_dev_logd(card->dev, "%s: reg_val  = %x\n", __func__, reg_val);
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN3_CTL,
			0x03, reg_val);
}

static void wcd_program_btn_threshold(const struct wcd_mbhc *mbhc, bool micbias)
{
	struct wcd_mbhc_btn_detect_cfg *btn_det;
	struct snd_soc_codec *codec = mbhc->codec;
	struct snd_soc_card *card = codec->card;
	u16 i;
	u32 course, fine, reg_val;
	u16 reg_addr = MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL;
	s16 *btn_voltage;

	if (mbhc->mbhc_cfg->calibration == NULL) {
		ad_dev_loge(card->dev, "%s: calibration data is NULL\n", __func__);
		return;
	}

	btn_det = WCD_MBHC_CAL_BTN_DET_PTR(mbhc->mbhc_cfg->calibration);

	if (micbias)
		btn_voltage = ((void *)&btn_det->_v_btn_low) +
			(sizeof(btn_det->_v_btn_low[0]) * btn_det->num_btn);
	else
		btn_voltage = btn_det->_v_btn_low;
	for (i = 0; i <  btn_det->num_btn; i++) {
		course = (btn_voltage[i] / 100);
		fine = ((btn_voltage[i] % 100) / 12);

		reg_val = (course << 5) | (fine << 2);
		snd_soc_update_bits(codec, reg_addr, 0xFC, reg_val);
		ad_logd("%s: course: %d fine: %d reg_addr: %x reg_val: %x\n",
				__func__, course, fine, reg_addr, reg_val);
		reg_addr++;
	}
}

static void wcd_enable_curr_micbias(const struct wcd_mbhc *mbhc,
				const enum wcd_mbhc_cs_mb_en_flag cs_mb_en)
{
	struct snd_soc_codec *codec = mbhc->codec;
	ad_logn("%s: enter, cs_mb_en: %d mbhc->current_plug %x \n", 
				__func__, cs_mb_en, mbhc->current_plug);
	if (mbhc->current_plug == MBHC_PLUG_TYPE_NONE && removal_st) {
		ad_loge("%s :: Avoid enabling source when plug is removed",__func__);
		return;
	}
	switch (cs_mb_en) {
	case WCD_MBHC_EN_CS:
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MICB_2_EN,
			0xC0, 0x00);
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x30, 0x30);
		/* Program Button threshold registers as per CS */
		wcd_program_btn_threshold(mbhc, false);
		break;
	case WCD_MBHC_EN_MB:
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0xB0, 0x80);
		/* Disable PULL_UP_EN & enable MICBIAS */
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MICB_2_EN,
			0xC0, 0x80);
		/* Program Button threshold registers as per MICBIAS */
		wcd_program_btn_threshold(mbhc, true);
		break;
	case WCD_MBHC_EN_PULLUP:
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0xB0, 0xB0);
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MICB_2_EN,
			0xC0, 0x40);
		/* Program Button threshold registers as per MICBIAS */
		wcd_program_btn_threshold(mbhc, true);
		break;
	case WCD_MBHC_EN_NONE:
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0xB0, 0x80);
		snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MICB_2_EN,
			0xC0, 0x00);
		break;
	default:
		ad_logd("%s: Invalid parameter", __func__);
		break;
	}

	ad_logd("%s: exit\n", __func__);
}

static int wcd_event_notify(struct notifier_block *self, unsigned long val,
				void *data)
{
	struct snd_soc_codec *codec = (struct snd_soc_codec *)data;
	struct msm8x16_wcd_priv *msm8x16_wcd = snd_soc_codec_get_drvdata(codec);
	struct wcd_mbhc *mbhc = &msm8x16_wcd->mbhc;
	enum wcd_notify_event event = (enum wcd_notify_event)val;
	u16 micbias2;

	ad_logn("%s: event %d\n", __func__, event);
	micbias2 = (snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MICB_2_EN) & 0x80);
	switch (event) {
	/* MICBIAS usage change */
	case WCD_EVENT_PRE_MICBIAS_2_ON:
		if (mbhc->micbias_enable) {
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MICB_1_CTL,
					0x60, 0x60);
			snd_soc_write(codec,
					MSM8X16_WCD_A_ANALOG_MICB_1_VAL,
					0xC0);
			/*
			 * Special headset needs MICBIAS as 2.7V so wait for
			 * 50 msec for the MICBIAS to reach 2.7 volts.
			 */
			msleep(50);
			if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_auto_zeroing)
				mbhc->mbhc_cb->set_auto_zeroing(codec, true);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MICB_1_CTL,
					0x60, 0x00);
		}
		/* Disable current source if micbias enabled */
		wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		mbhc->is_hs_recording = true;
		/* configure cap settings properly when micbias is enabled */
		wcd_configure_cap(mbhc, true);
		break;
	/* MICBIAS usage change */
	case WCD_EVENT_PRE_MICBIAS_2_OFF:
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_auto_zeroing)
			mbhc->mbhc_cb->set_auto_zeroing(codec, false);
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_micbias_value)
			mbhc->mbhc_cb->set_micbias_value(codec);
		mbhc->is_hs_recording = false;
		/* Enable PULL UP if PA's are enabled */
		if ((test_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state)) ||
				(test_bit(WCD_MBHC_EVENT_PA_HPHR,
					  &mbhc->event_state)))
			/* enable pullup and cs, disable mb */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		else
			/* enable current source and disable mb, pullup*/
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);

		/* configure cap settings properly when micbias is disabled */
		wcd_configure_cap(mbhc, false);
		break;
	case WCD_EVENT_POST_HPHL_PA_OFF:
		if (mbhc->hph_status & SND_JACK_OC_HPHL)
			hphlocp_off_report(mbhc, SND_JACK_OC_HPHL);
		clear_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, pullup & enable cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		break;
	case WCD_EVENT_POST_HPHR_PA_OFF:
		if (mbhc->hph_status & SND_JACK_OC_HPHR)
			hphrocp_off_report(mbhc, SND_JACK_OC_HPHR);
		clear_bit(WCD_MBHC_EVENT_PA_HPHR, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, pullup & enable cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		break;
	case WCD_EVENT_PRE_HPHL_PA_ON:
		set_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, enable pullup & cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		break;
	case WCD_EVENT_PRE_HPHR_PA_ON:
		set_bit(WCD_MBHC_EVENT_PA_HPHR, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, enable pullup & cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		break;
	default:
		break;
	}
	return 0;
}

static int wcd_cancel_btn_work(struct wcd_mbhc *mbhc)
{
	int r;

	r = cancel_delayed_work_sync(&mbhc->mbhc_btn_dwork);
	/*
	 * if scheduled mbhc.mbhc_btn_dwork is canceled from here,
	 * we have to unlock from here instead btn_work
	 */
	if (r)
		wcd9xxx_spmi_unlock_sleep();
	return r;
}

static bool wcd_swch_level_remove(struct wcd_mbhc *mbhc)
{
	u16 result2;
	struct snd_soc_codec *codec = mbhc->codec;

	result2 = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
	return (result2 & 0x10) ? true : false;
}

/* should be called under interrupt context that hold suspend */
static void wcd_schedule_hs_detect_plug(struct wcd_mbhc *mbhc,
					    struct work_struct *work)
{
	ad_logn("%s: scheduling correct_swch_plug\n", __func__);
	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);
	mbhc->hs_detect_work_stop = false;
	wcd9xxx_spmi_lock_sleep();
	schedule_work(work);
}

/* called under codec_resource_lock acquisition */
static void wcd_cancel_hs_detect_plug(struct wcd_mbhc *mbhc,
					 struct work_struct *work)
{
	ad_logn("%s: Canceling correct_plug_swch\n", __func__);
	mbhc->hs_detect_work_stop = true;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	if (cancel_work_sync(work)) {
		ad_logn("%s: correct_plug_swch is canceled\n",
			 __func__);
		wcd9xxx_spmi_unlock_sleep();
	}
	WCD_MBHC_RSC_LOCK(mbhc);
}

static void wcd_mbhc_clr_and_turnon_hph_padac(struct wcd_mbhc *mbhc)
{
	bool pa_turned_on = false;
	u8 wg_time;
	struct snd_soc_codec *codec = mbhc->codec;

	wg_time = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_WG_TIME);
	wg_time += 1;

	if (test_and_clear_bit(WCD_MBHC_HPHR_PA_OFF_ACK,
			       &mbhc->hph_pa_dac_state)) {
		ad_logn("%s: HPHR clear flag and enable PA\n", __func__);
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN,
				    0x10, 0x10);
		pa_turned_on = true;
	}
	if (test_and_clear_bit(WCD_MBHC_HPHL_PA_OFF_ACK,
			       &mbhc->hph_pa_dac_state)) {
		ad_logn("%s: HPHL clear flag and enable PA\n", __func__);
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN,
				    0x20, 0x20);
		pa_turned_on = true;
	}

	if (pa_turned_on) {
		ad_logn("%s: PA was turned off by MBHC and not by DAPM\n",
			 __func__);
		usleep_range(wg_time * 1000, wg_time * 1000 + 50);
	}
}

static bool wcd_mbhc_is_hph_pa_on(struct snd_soc_codec *codec)
{
	u8 hph_reg_val = 0;
	hph_reg_val = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN);

	return (hph_reg_val & 0x30) ? true : false;
}

static void wcd_mbhc_set_and_turnoff_hph_padac(struct wcd_mbhc *mbhc)
{
	u8 wg_time;
	struct snd_soc_codec *codec = mbhc->codec;

	wg_time = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_WG_TIME);
	wg_time += 1;

	/* If headphone PA is on, check if userspace receives
	* removal event to sync-up PA's state */
	if (wcd_mbhc_is_hph_pa_on(codec)) {
		ad_logn("%s PA is on, setting PA_OFF_ACK\n", __func__);
		set_bit(WCD_MBHC_HPHL_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
		set_bit(WCD_MBHC_HPHR_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
	} else {
		ad_logn("%s PA is off\n", __func__);
	}
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN,
			    0x30, 0x00);
	usleep_range(wg_time * 1000, wg_time * 1000 + 50);
}

static void wcd_mbhc_calc_impedance(struct wcd_mbhc *mbhc, uint32_t *zl,
	uint32_t *zr)
{
	struct snd_soc_codec *codec = mbhc->codec;
	s16 impedance_l, impedance_r;
	s16 impedance_l_fixed;
	s16 reg0, reg1, reg2;
	bool high = false;

	ad_logn("%s: enter\n", __func__);

	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);
	reg0 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_DBNC_TIMER);
	reg1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN2_ZDETH_CTL);
	reg2 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2);

	/* disable FSM */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x80, 0x00);
	/*
	 * Enable legacy electrical detection current sources
	 * and disable fast ramp and enable manual switching
	 * of extra capacitance
	 */
	ad_logd("%s: Setup for impedance det\n", __func__);

	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
			0x06, 0x02);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_DBNC_TIMER,
			0x02, 0x02);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN2_ZDETH_CTL,
			0x20, 0x00);

	ad_logd("%s: Start performing impedance detection\n",
		 __func__);

	/* Enable ZDET_L_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x08, 0x08);
	usleep_range(2000, 2100);
	/* Read Left impedance value from Result1 */
	impedance_l = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	/* Enable ZDET_R_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x0C, 0x04);
	usleep_range(2000, 2100);
	/* Read Right impedance value from Result1 */
	impedance_r = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x04, 0x00);

	if (impedance_l > 1) {
		high = true;
		goto exit;
	}

	/*
	 * As the result is 0 impedance is < 200 use
	 * RAMP to measure impedance further.
	 */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0xFF, 0x00);

	/* Enable RAMP_L , RAMP_R & ZDET_CHG*/
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
			0x03, 0x03);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x02, 0x02);
	usleep_range(50000, 50100);
	/* Enable ZDET_DISCHG_CAP_CTL  to add extra capacitance */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x01, 0x01);
	usleep_range(5000, 5100);

	/* Enable ZDET_L_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x08, 0x08);
	usleep_range(2000, 2100);
	/* Read Left impedance value from Result1 */
	impedance_l = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	/* Enable ZDET_R_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x0C, 0x04);
	usleep_range(2000, 2100);
	/* Read Right impedance value from Result1 */
	impedance_r = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x04, 0x00);

	if (!mbhc->mbhc_cfg->mono_stero_detection) {
		/* Set ZDET_CHG to 0  to discharge ramp */
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
				0x02, 0x00);
		usleep_range(40000, 40100);
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
				0x03, 0x00);
		goto exit;
	}

	/* Disable Set ZDET_CONN_RAMP_L and enable ZDET_CONN_FIXED_L */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
			0x02, 0x00);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN1_ZDETM_CTL,
			0x02, 0x02);
	/* Set ZDET_CHG to 0  */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x02, 0x00);
	usleep_range(40000, 40100);

	/* Set ZDET_CONN_RAMP_R to 0  */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
			0x01, 0x00);
	/* Enable ZDET_L_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x08, 0x08);
	usleep_range(2000, 2100);
	/* Read Left impedance value from Result1 */
	impedance_l_fixed = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	/* Disable ZDET_L_MEAS_EN */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x08, 0x00);
	/*
	 * impedance_l is equal to impedance_l_fixed then headset is stereo
	 * otherwise headset is mono
	 */

	/* Enable ZDET_CHG  */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x02, 0x02);
	usleep_range(10000, 10100);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
			0x02, 0x02);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN1_ZDETM_CTL,
			0x02, 0x00);
	/* Set ZDET_CHG to 0  to discharge HPHL */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0x02, 0x00);
	usleep_range(40000, 40100);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_BTN0_ZDETL_CTL,
			0x02, 0x00);

exit:
	/*
	 * Enable the FSM and button source back for
	 * button detection.
	 */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
			0xFF, 0xB0);
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN2_ZDETH_CTL, reg1);
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DBNC_TIMER, reg0);
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2, reg2);
	if (mbhc->mbhc_cb && mbhc->mbhc_cb->compute_impedance)
		mbhc->mbhc_cb->compute_impedance(impedance_l, impedance_r,
					 zl, zr, high);

	ad_logn("%s: RL %d milliohm, RR %d milliohm\n", __func__, *zl, *zr);
	ad_logn("%s: Impedance detection completed\n", __func__);
}

int wcd_mbhc_get_impedance(struct wcd_mbhc *mbhc, uint32_t *zl,
			uint32_t *zr)
{
	*zl = mbhc->zl;
	*zr = mbhc->zr;

	if (*zl && *zr)
		return 0;
	else
		return -EINVAL;
}

static void wcd_mbhc_report_plug(struct wcd_mbhc *mbhc, int insertion,
				enum snd_jack_types jack_type)
{
	struct snd_soc_codec *codec = mbhc->codec;
	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

#ifdef CONFIG_HUAWEI_DSM
	if (insertion)
	{
		timeout_jiffies = jiffies + msecs_to_jiffies(DSM_AUDIO_LESS_HS_GAP); // timeout after 500ms
	}
	else
	{
		if(time_before(jiffies, timeout_jiffies))
		{
			if(ispress)
			{
				audio_dsm_report_num(DSM_AUDIO_HANDSET_PRESS_RELEASE_ERROR, DSM_AUDIO_MESG_PRESS_RELEASE_ERROR);
			}
			else
			{
				audio_dsm_report_num(DSM_AUDIO_HANDSET_PLUG_RELEASE_ERROR, DSM_AUDIO_MESG_INSURT_TIME_SHORT);
			}
		}
	}
	
	ispress = false;
#endif
	ad_logn("%s: enter insertion %d hph_status %x\n",
		 __func__, insertion, mbhc->hph_status);
	if (!insertion) {
		/* Report removal */
		mbhc->hph_status &= ~jack_type;
		/*
		 * cancel possibly scheduled btn work and
		 * report release if we reported button press
		 */
		if (wcd_cancel_btn_work(mbhc)) {
			ad_logd("%s: button press is canceled\n", __func__);
		} else if (mbhc->buttons_pressed) {
			ad_logd("%s: release of button press%d\n",
				 __func__, jack_type);
			wcd_mbhc_jack_report(mbhc, &mbhc->button_jack, 0,
					    mbhc->buttons_pressed);
			mbhc->buttons_pressed &=
				~WCD_MBHC_JACK_BUTTON_MASK;
		}

		if (mbhc->micbias_enable)
			mbhc->micbias_enable = false;

		mbhc->zl = mbhc->zr = 0;
		ad_logn("%s: Reporting removal %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				mbhc->hph_status, WCD_MBHC_JACK_MASK);
		wcd_mbhc_set_and_turnoff_hph_padac(mbhc);
		hphrocp_off_report(mbhc, SND_JACK_OC_HPHR);
		hphlocp_off_report(mbhc, SND_JACK_OC_HPHL);
		mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	} else {
		/*
		 * Report removal of current jack type.
		 * Headphone to headset shouldn't report headphone
		 * removal.
		 */
		if (mbhc->mbhc_cfg->detect_extn_cable &&
		    (mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH ||
		    jack_type == SND_JACK_LINEOUT) &&
		    (mbhc->hph_status && mbhc->hph_status != jack_type)) {

		if (mbhc->micbias_enable)
			mbhc->micbias_enable = false;

			mbhc->zl = mbhc->zr = 0;
			ad_logn("%s: Reporting removal (%x)\n",
				 __func__, mbhc->hph_status);
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					    0, WCD_MBHC_JACK_MASK);

			if (mbhc->hph_status == SND_JACK_LINEOUT) {

				ad_logd("%s: Enable micbias\n", __func__);
				/* Disable current source and enable micbias */
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
				ad_logd("%s: set up elec removal detection\n",
					  __func__);
				snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
					0x01, 0x00);
				usleep_range(200, 210);
				wcd9xxx_spmi_enable_irq(
					mbhc->intr_ids->mbhc_hs_rem_intr);
			}
			mbhc->hph_status &= ~(SND_JACK_HEADSET |
						SND_JACK_LINEOUT |
						SND_JACK_UNSUPPORTED);
		}

		if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET &&
			jack_type == SND_JACK_HEADPHONE)
			mbhc->hph_status &= ~SND_JACK_HEADSET;

		/* Report insertion */
		mbhc->hph_status |= jack_type;

		if (jack_type == SND_JACK_HEADPHONE)
			mbhc->current_plug = MBHC_PLUG_TYPE_HEADPHONE;
		else if (jack_type == SND_JACK_UNSUPPORTED)
			mbhc->current_plug = MBHC_PLUG_TYPE_GND_MIC_SWAP;
		else if (jack_type == SND_JACK_HEADSET) {
			mbhc->current_plug = MBHC_PLUG_TYPE_HEADSET;
			mbhc->jiffies_atreport = jiffies;
		}
		else if (jack_type == SND_JACK_LINEOUT)
			mbhc->current_plug = MBHC_PLUG_TYPE_HIGH_HPH;

		if (mbhc->impedance_detect)
			wcd_mbhc_calc_impedance(mbhc,
					&mbhc->zl, &mbhc->zr);
		ad_logn("%s: Reporting insertion %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				    mbhc->hph_status, WCD_MBHC_JACK_MASK);
		wcd_mbhc_clr_and_turnon_hph_padac(mbhc);
	}
	ad_logn("%s: leave hph_status %x\n", __func__, mbhc->hph_status);
}

static void wcd_mbhc_find_plug_and_report(struct wcd_mbhc *mbhc,
					 enum wcd_mbhc_plug_type plug_type)
{
	struct snd_soc_codec *codec = mbhc->codec;

	ad_logn("%s: enter current_plug(%d) new_plug(%d)\n",
		 __func__, mbhc->current_plug, plug_type);

	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

    /* don't report headset event if headset type don't change */
	if (mbhc->current_plug == plug_type) {
		ad_logn("%s: cable already reported, exit\n", __func__);
		goto exit;
	}

	if (plug_type == MBHC_PLUG_TYPE_HEADPHONE) {
		/*
		 * Nothing was reported previously
		 * report a headphone or unsupported
		 * Enable CS for headphone to correct slow insertion of headset.
		 */
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL, 0x30, 0x30);
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_HEADPHONE);
	} else if (plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP) {
			if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE)
				wcd_mbhc_report_plug(mbhc, 0,
						SND_JACK_HEADPHONE);
			if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET)
				wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADSET);
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_UNSUPPORTED);
	} else if (plug_type == MBHC_PLUG_TYPE_HEADSET) {
		/*
		 * If Headphone was reported previously, this will
		 * only report the mic line
		 */
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_HEADSET);
	} else if (plug_type == MBHC_PLUG_TYPE_HIGH_HPH) {
		/* enable elec insertion interrupts just on new HW(HPHL don't have 499 ohm resistance) */
		if (mbhc->mbhc_cfg->detect_extn_cable && (!bad_jack_old_HW) && det_extn_cable_en) {
			/* High impedance device found. Report as LINEOUT */
			wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
			ad_logn("%s: setup mic trigger for further detection\n",
				 __func__);

			/* Disable HW FSM and current source */
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
					0xB0, 0x0);
			/* Setup for insertion detection */
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
					0x01, 0x01);
			/*
			 * Enable HPHL trigger and MIC Schmitt triggers
			 * and request for elec insertion interrupts
			 */
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
					0x06, 0x06);
			wcd9xxx_spmi_enable_irq(
					mbhc->intr_ids->mbhc_hs_ins_intr);
		} else {
			wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
		}
	} else {
		WARN(1, "Unexpected current plug_type %d, plug_type %d\n",
		     mbhc->current_plug, plug_type);
	}
exit:
	ad_logn("%s: leave\n", __func__);
}

/* To determine if cross connection occured */
static bool wcd_check_cross_conn(struct wcd_mbhc *mbhc)
{
	u16 swap_res;
	struct snd_soc_codec *codec = mbhc->codec;
	enum wcd_mbhc_plug_type plug_type = mbhc->current_plug;
	s16 reg1;

	if (wcd_swch_level_remove(mbhc)) {
		ad_logd("%s: Switch level is low\n", __func__);
		return false;
	}

	reg1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2);
	/*
	 * Check if there is any cross connection,
	 * Micbias and schmitt trigger (HPHL-HPHR)
	 * needs to be enabled.
	 */
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
			0x6, 0x4);
	/* read reg MBHC_RESULT_2 value with cross connection bit */
	swap_res = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
	ad_logn("%s: swap_res %x\n", __func__, swap_res);
	if (!(swap_res & 0x0C)) {
		plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
		ad_logn("%s: Cross connection identified\n", __func__);
	} else {
		ad_logn("%s: No Cross connection found\n", __func__);
	}

	/* Disable schmitt trigger and restore micbias */
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2, reg1);
	ad_logn("%s: leave, plug type: %d\n", __func__,  plug_type);

	return (plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP) ? true : false;
}

/* since we remove swap & hph type detection,no need to use wcd_is_special_headset */
#ifndef CONFIG_HUAWEI_KERNEL
static bool wcd_is_special_headset(struct wcd_mbhc *mbhc)
{
	u16 result2;
	struct snd_soc_codec *codec = mbhc->codec;
	int delay = 0;
	bool ret = false;

	/*
	 * Enable micbias if not already enabled
	 * and disable current source if using micbias
	 */
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	ad_logn("%s: special headset, start register writes\n", __func__);
	result2 = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
	while (result2 & 0x01)  {
		if (mbhc->hs_detect_work_stop) {
			ad_logn("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			break;
		}
		delay = delay + 50;
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MICB_1_CTL,
				    0x60, 0x60);
		snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MICB_1_VAL,
			      0xC0);
		/* Wait for 50msec for MICBIAS to settle down */
		msleep(50);
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_auto_zeroing)
			mbhc->mbhc_cb->set_auto_zeroing(codec, true);
		/* Wait for 50msec for FSM to update result values */
		msleep(50);
		result2 = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
		if (!(result2 & 0x01))
			ad_logn("%s: Special headset detected in %d msecs\n",
					__func__, (delay * 2));
		if (delay == SPECIAL_HS_DETECT_TIME_MS) {
			ad_logn("%s: Spl headset didnt get detect in 4 sec\n",
					__func__);
			break;
		}
	}
	if (!(result2 & 0x01)) {
		ad_logn("%s: Headset with threshold found\n",  __func__);
		mbhc->micbias_enable = true;
		ret = true;
	}
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MICB_1_CTL, 0x60, 0x00);
	if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_micbias_value)
		mbhc->mbhc_cb->set_micbias_value(codec);
	if (mbhc->mbhc_cb && mbhc->mbhc_cb->set_auto_zeroing)
		mbhc->mbhc_cb->set_auto_zeroing(codec, false);

	ad_logn("%s: leave\n", __func__);
	return ret;
}
#endif

static void wcd_correct_swch_plug(struct work_struct *work)
{
	struct wcd_mbhc *mbhc;
	struct snd_soc_codec *codec;
	enum wcd_mbhc_plug_type plug_type = MBHC_PLUG_TYPE_INVALID;
	unsigned long timeout = 0;
	u16 result1 = 0, result2 = 0;
	bool wrk_complete = false;
	int pt_gnd_mic_swap_cnt = 0;
	bool is_pa_on = false;
	u16 micbias2 = 0;
	u16 mic_inserted = 0;
	u16 hphl_inserted = 0;
	u16 hphr_inserted = 0;
	s16 reg1 = 0;
	s16 reg2 = 0;
	s16 reg3 = 0;
	s16 reg4 = 0;
	s16 reg5 = 0;
	s16 reg6 = 0;
	int i = 0;

	ad_logd("%s: enter\n", __func__);

	mbhc = container_of(work, struct wcd_mbhc, correct_plug_swch);
	codec = mbhc->codec;

	if(((mbhc->current_plug == MBHC_PLUG_TYPE_NONE)||(mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH)) && det_extn_cable_en )
	{
		reg1=snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MICB_2_EN);
		reg2= snd_soc_read (codec, MSM8X16_WCD_A_ANALOG_MICB_1_CTL);
		reg3= snd_soc_read (codec, MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL);
		reg4=snd_soc_read (codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1);
		reg5=snd_soc_read (codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2);
		reg6=snd_soc_read (codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN);

		snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN, 0x80);
		snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MICB_2_EN, 0x00);
		snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MICB_1_CTL, 0x00);
		snd_soc_write(codec,MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL, 0x00);
		snd_soc_write(codec,MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,0xAD);
		msleep(2);
		snd_soc_write(codec,MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,0xA5);
		snd_soc_update_bits(codec,MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
							0x06, 0x02);
		msleep(100);
		i = 40;
		while(i--)
		{
			result2 = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
			if(result2 & 0x02)
				mic_inserted++;
			else
				mic_inserted = 0;
			if(result2 & 0x04)
				hphr_inserted++;
			else
				hphr_inserted = 0;
			if(result2 & 0x08)
				hphl_inserted++;
			else
				hphl_inserted = 0;
			msleep(1);
		}

		ad_logn("%s: mic_inserted: %d times,hphr_inserted: %d times,hphl_inserted: %d times,\n", __func__,mic_inserted,hphr_inserted, hphl_inserted);
		if((mic_inserted <= 3)&&(hphr_inserted <= 3))
		{
			if(hphl_inserted >= 17)
			{
				ad_logn("%s: This is bad jack with old hardware,make a mark  here \n", __func__);
				bad_jack_old_HW = 1;
			}
			else
			{
				ad_logn("%s: This is bad jack with new hardware,do nothing\n", __func__);
			}
		}
		else if((mic_inserted >= 17)||(hphr_inserted >= 17))
		{
			mic_or_hphr_inserted = 1;
			ad_logn("%s: force to headset from swap and hph \n", __func__);
			plug_type = MBHC_PLUG_TYPE_HEADSET;
			wcd_mbhc_find_plug_and_report(mbhc, plug_type);
		}

		snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MICB_2_EN,reg1);
		snd_soc_write (codec, MSM8X16_WCD_A_ANALOG_MICB_1_CTL,reg2);
		snd_soc_write (codec, MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,reg3);
		snd_soc_write (codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,reg4);
		snd_soc_write (codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,reg5);
		snd_soc_write (codec, MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN,reg6);
	}

	/* Enable micbias for detection in correct work*/
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	timeout = jiffies + msecs_to_jiffies(HS_DETECT_PLUG_TIME_MS);
	while (!time_after(jiffies, timeout)) {
		if (mbhc->hs_detect_work_stop) {
			ad_logn("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			goto exit;
		}
		/* allow sometime and re-check stop requested again */
		msleep(200);
		if (mbhc->hs_detect_work_stop) {
			ad_logn("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			goto exit;
		}
		result1 = snd_soc_read(codec,
				 MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
		result2 = snd_soc_read(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);
		ad_logn("%s: result2 = %x\n", __func__, result2);

		is_pa_on = snd_soc_read(codec,
					MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN) &
					0x30;

		if ((!(result2 & 0x01)) && (!is_pa_on)) {
			/* Check for cross connection*/
			if (wcd_check_cross_conn(mbhc)) {
				plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
				pt_gnd_mic_swap_cnt++;
				if (pt_gnd_mic_swap_cnt <
						GND_MIC_SWAP_THRESHOLD) {
					continue;
				} else if (pt_gnd_mic_swap_cnt >
						GND_MIC_SWAP_THRESHOLD) {
					/*
					 * This is due to GND/MIC switch didn't
					 * work,  Report unsupported plug.
					 */
					ad_logn("%s: switch didnt work\n",
						  __func__);
					goto report;
				} else if (mbhc->mbhc_cfg->swap_gnd_mic) {
					ad_logd("%s: US_EU gpio present, flip switch\n",
						 __func__);
					/*
					 * if switch is toggled, check again,
					 * otherwise report unsupported plug
					 */
					if (mbhc->mbhc_cfg->swap_gnd_mic(codec))
						continue;
				}
			} else {
				pt_gnd_mic_swap_cnt++;
				plug_type = MBHC_PLUG_TYPE_HEADSET;
				if (pt_gnd_mic_swap_cnt <
						GND_MIC_SWAP_THRESHOLD) {
					continue;
				} else {
					pt_gnd_mic_swap_cnt = 0;
				}
			}
		}
		if (result2 == 1) {
			ad_logn("%s: cable is extension cable\n", __func__);
			plug_type = MBHC_PLUG_TYPE_HIGH_HPH;
			wrk_complete = true;
		} else {
			ad_logn("%s: cable might be headset: %d\n", __func__,
					plug_type);
			if (!(plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP)) {
				plug_type = MBHC_PLUG_TYPE_HEADSET;
				/*
				 * Report headset only if not already reported
				 * and if there is not button press without
				 * release
				 */
				if (mbhc->current_plug !=
						MBHC_PLUG_TYPE_HEADSET &&
						!mbhc->btn_press_intr) {
					ad_logn("%s: cable is headset\n",
							__func__);
					goto report;
				}
			}
			wrk_complete = false;
		}
	}
	/* when plug type is HIGH_HPH do not process this code for ATH */
	if ((!det_extn_cable_en) && (mbhc->btn_press_intr))
	{
		ad_logn("%s: Can be slow insertion of headphone\n", __func__);
		plug_type = MBHC_PLUG_TYPE_HEADPHONE;
	}
	else if (!wrk_complete && mbhc->btn_press_intr) {
		ad_logn("%s: Can be slow insertion of headphone\n", __func__);
		plug_type = MBHC_PLUG_TYPE_HEADPHONE;
	}
	/*
	 * If plug_tye is headset, we might have already reported either in
	 * detect_plug-type or in above while loop, no need to report again
	 */
	if (!wrk_complete && plug_type == MBHC_PLUG_TYPE_HEADSET) {
		ad_logn("%s: It's neither headset nor headphone\n", __func__);
		/*
		 * Do not disable micbias if recording is going on or
		 * headset is inserted on the other side of the extn
		 * cable. If headset has been detected current source
		 * needs to be kept enabled for button detection to work.
		 * If the accessory type is invalid or unsupported, we
		 * dont need to enable either of them.
		 */
		if (plug_type == MBHC_PLUG_TYPE_HEADSET) {
			if ((mbhc->is_hs_recording || det_extn_cable_en))
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
			else if ((test_bit(WCD_MBHC_EVENT_PA_HPHL,
				&mbhc->event_state)) ||
				(test_bit(WCD_MBHC_EVENT_PA_HPHR,
					  &mbhc->event_state)))
				wcd_enable_curr_micbias(mbhc,
						WCD_MBHC_EN_PULLUP);
			else
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		} else {
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_NONE);
		}
		goto exit;
	}

/* since we remove swap & hph type detection,no need to use wcd_is_special_headset */
#ifndef CONFIG_HUAWEI_KERNEL
	if (plug_type == MBHC_PLUG_TYPE_HIGH_HPH &&
		(!det_extn_cable_en)) {
		if (wcd_is_special_headset(mbhc)) {
			ad_logn("%s: Special headset found %d\n",
					__func__, plug_type);
			plug_type = MBHC_PLUG_TYPE_HEADSET;
			goto report;
		}
	}
#endif

report:
	ad_logd("%s: Valid plug found, plug type %d wrk_cmpt %d btn_intr %d\n",
			__func__, plug_type, wrk_complete,
			mbhc->btn_press_intr);
	if(det_extn_cable_en)
	{
		if((plug_type == MBHC_PLUG_TYPE_HIGH_HPH )||(plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP))
		{
			if(mic_or_hphr_inserted)
			{
				ad_logn("%s: force the swap and HPH to headset in correct shedule", __func__);
				plug_type = MBHC_PLUG_TYPE_HEADSET;
			}
			/* set device to headset or headphone when device is not detected when mech irq */
			else if((mbhc->current_plug != plug_type) && (mbhc->current_plug != MBHC_PLUG_TYPE_NONE))
			{
    			plug_type = mbhc->current_plug;
			}
		}

		if (mbhc->current_plug != plug_type)
		{
			wcd_mbhc_find_plug_and_report(mbhc, plug_type);
		}
		else
		{
			ad_logn("%s: the plug_type has been reported", __func__);
		}
	}
	else
	{
		/* remove swap & hph type to make auto audio mmi test pass */
#ifdef CONFIG_HUAWEI_KERNEL
		if((MBHC_PLUG_TYPE_HIGH_HPH == plug_type)||
			(MBHC_PLUG_TYPE_GND_MIC_SWAP == plug_type)) {
    				ad_logn("%s: force the type change to headset from swap and hph\n", __func__);
				plug_type = MBHC_PLUG_TYPE_HEADSET;
		}
#endif

		/*
	 	* Do not disable micbias if recording is going on or
	 	* headset is inserted on the other side of the extn
	 	* cable. If headset has been detected current source
	 	* needs to be kept enabled for button detection to work.
	 	* If the accessory type is invalid or unsupported, we
	 	* dont need to enable either of them.
	 	*/
		if (plug_type == MBHC_PLUG_TYPE_HEADSET) {
			if ((mbhc->is_hs_recording || det_extn_cable_en))
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
			else if ((test_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state))
					|| (test_bit(WCD_MBHC_EVENT_PA_HPHR,
					  	&mbhc->event_state)))
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
			else
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);

		} else {
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_NONE);
		}
		WCD_MBHC_RSC_LOCK(mbhc);
		wcd_mbhc_find_plug_and_report(mbhc, plug_type);
		WCD_MBHC_RSC_UNLOCK(mbhc);
	}
exit:
	micbias2 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MICB_2_EN);
	wcd_configure_cap(mbhc, (micbias2 & 0x80));
	wcd9xxx_spmi_unlock_sleep();
	ad_logn("%s: leave\n", __func__);
}

/* called under codec_resource_lock acquisition */
static void wcd_mbhc_detect_plug_type(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;
	long timeout = msecs_to_jiffies(50);   /* 50ms */
	enum wcd_mbhc_plug_type plug_type;
	int timeout_result;
	u16 result1, result2;
	bool cross_conn;
	int try = 0;

	ad_logn("%s: enter\n", __func__);
	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	wcd_configure_cap(mbhc, true);
	/*
	 * Wait for 50msec for FSM to complete its task.
	 * wakeup if btn pres intr occurs
	 */
	mbhc->is_btn_press = false;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	timeout_result = wait_event_interruptible_timeout(mbhc->wait_btn_press,
			mbhc->is_btn_press, timeout);

	WCD_MBHC_RSC_LOCK(mbhc);
	result1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	result2 = snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);

	if (!timeout_result) {
		ad_logn("%s No btn press interrupt\n", __func__);
		/*
		 * Check if there is any cross connection,
		 * Micbias and schmitt trigger (HPHL-HPHR)
		 * needs to be enabled.
		 */
		ad_logn("%s: result1 %x, result2 %x\n", __func__,
						result1, result2);
		if (!(result2 & 0x01)) {
			/*
			 * Cross connection result is not reliable
			 * so do check for it for 4 times to conclude
			 * cross connection occured or not.
			 */
			do {
				cross_conn = wcd_check_cross_conn(mbhc);
				try++;
			} while (try < GND_MIC_SWAP_THRESHOLD);
			if (cross_conn) {
				ad_logn("%s: cross con found, start polling\n",
					 __func__);
				plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
				goto exit;
			}
		}

		/* Read back result1 and result2 value again to reconfirm*/
		result1 = snd_soc_read(codec,
			  MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
		result2 = snd_soc_read(codec,
			  MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT);

		if (!result1 && !(result2 & 0x01))
			plug_type = MBHC_PLUG_TYPE_HEADSET;
		else if (!result1 && (result2 & 0x01))
			plug_type = MBHC_PLUG_TYPE_HIGH_HPH;
		else {
			plug_type = MBHC_PLUG_TYPE_INVALID;
			goto exit;
		}
	} else {
		if (!result1 && !(result2 & 0x01))
			plug_type = MBHC_PLUG_TYPE_HEADPHONE;
		else {
			plug_type = MBHC_PLUG_TYPE_INVALID;
			goto exit;
		}
	}
exit:
#ifdef CONFIG_HUAWEI_DSM
	if(MBHC_PLUG_TYPE_INVALID == plug_type)
	{
		audio_dsm_report_num(DSM_AUDIO_HANDSET_DECT_FAIL_ERROR_NO, DSM_AUDIO_MESG_HS_TYPE_DECT_FAIL);
	}
#endif
	ad_logn("%s: Valid plug found, plug type is %d\n",
			 __func__, plug_type);
	/* remove swap & hph type to make auto audio mmi test pass */
#ifdef CONFIG_HUAWEI_KERNEL
	if(((MBHC_PLUG_TYPE_HIGH_HPH == plug_type)||(MBHC_PLUG_TYPE_GND_MIC_SWAP == plug_type))
		&& (!det_extn_cable_en)) {
			ad_logn("%s: force the type change to headset from swap and hph\n", __func__);
			plug_type = MBHC_PLUG_TYPE_HEADSET;
	}
#endif
	if (plug_type == MBHC_PLUG_TYPE_HEADSET ||
			plug_type == MBHC_PLUG_TYPE_HEADPHONE) {
		wcd_mbhc_find_plug_and_report(mbhc, plug_type);
		wcd_schedule_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	} else {
		wcd_schedule_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	}
	ad_logn("%s: leave\n", __func__);
}

static void wcd_mbhc_swch_irq_handler(struct wcd_mbhc *mbhc)
{
	bool detection_type;
	struct snd_soc_codec *codec = mbhc->codec;

	ad_logn("%s: enter\n", __func__);

	WCD_MBHC_RSC_LOCK(mbhc);

	mbhc->in_swch_irq_handler = true;

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		ad_logd("%s: button press is canceled\n", __func__);

	detection_type = (snd_soc_read(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1)) & 0x20;
	ad_logn("%s: enter current_plug(%d) detection_type(%d)\n",
		 __func__, mbhc->current_plug, detection_type);
	/* Set the detection type appropriately */
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
			0x20, (!detection_type << 5));

	ad_logd("%s: mbhc->current_plug: %d detection_type: %d\n", __func__,
			mbhc->current_plug, detection_type);
	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);

	if ((mbhc->current_plug == MBHC_PLUG_TYPE_NONE) &&
	    detection_type) {
		/* Make sure MASTER_BIAS_CTL is enabled */
		snd_soc_update_bits(codec,
				    MSM8X16_WCD_A_ANALOG_MASTER_BIAS_CTL,
				    0x30, 0x30);
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x04, 0x04);
		if (!mbhc->mbhc_cfg->hs_ext_micbias)
			/* Enable Tx2 RBias if the headset
			 * is using internal micbias*/
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MICB_1_INT_RBIAS,
					0x10, 0x10);
		/* Remove pull down on MIC BIAS2 */
		snd_soc_update_bits(codec,
				 MSM8X16_WCD_A_ANALOG_MICB_2_EN,
				0x20, 0x00);
		/* Enable HW FSM */
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
				0x80, 0x80);
		/* Apply trim if needed on the device */
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->trim_btn_reg)
			mbhc->mbhc_cb->trim_btn_reg(codec);
		/* Enable external voltage source to micbias if present */
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(codec, true);
		mbhc->btn_press_intr = false;
		removal_st = 0;
		ad_loge("%s :: INSERT removal_st = %d ", __func__, removal_st);
		wcd_mbhc_detect_plug_type(mbhc);
	} else if ((mbhc->current_plug != MBHC_PLUG_TYPE_NONE)
			&& !detection_type) {
		/* Disable external voltage source to micbias if present */
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(codec, false);
		/* Disable HW FSM */
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
				0xB0, 0x00);
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MICB_1_EN,
				0x04, 0x00);
		wcd_configure_cap(mbhc, false);
		mbhc->btn_press_intr = false;
		bad_jack_old_HW = 0;
		mic_or_hphr_inserted = 0;
		removal_st = 1;
		ad_loge("%s :: REMOVAL removal_st = %d ", __func__, removal_st);
		if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE) {
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADPHONE);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_GND_MIC_SWAP) {
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_UNSUPPORTED);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET) {
			/* make sure to turn off Rbias */
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MICB_1_INT_RBIAS,
					0x18, 0x08);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MICB_2_EN,
					0x20, 0x20);
			wcd9xxx_spmi_disable_irq(
					mbhc->intr_ids->mbhc_hs_rem_intr);
			wcd9xxx_spmi_disable_irq(
					mbhc->intr_ids->mbhc_hs_ins_intr);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
					0x01, 0x01);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
					0x06, 0x00);
			/* Enable current source */
			if( !det_extn_cable_en )
			{
				snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
					0x30, 0x30);
			}
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADSET);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH) {
			mbhc->is_extn_cable = false;
			wcd9xxx_spmi_disable_irq(
					mbhc->intr_ids->mbhc_hs_rem_intr);
			wcd9xxx_spmi_disable_irq(
					mbhc->intr_ids->mbhc_hs_ins_intr);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
					0x01, 0x01);
			snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
					0x06, 0x00);
			/* Enable current source */
			if( !det_extn_cable_en )
			{
				snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
					0x30, 0x30);
			}
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_LINEOUT);
		}
	} else if (!detection_type) {
		/* Disable external voltage source to micbias if present */
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(codec, false);
		/* Disable HW FSM */
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
				0xB0, 0x00);
	}

	mbhc->in_swch_irq_handler = false;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	ad_logn("%s: leave\n", __func__);
}

static irqreturn_t wcd_mbhc_mech_plug_detect_irq(int irq, void *data)
{
	int r = IRQ_HANDLED;
	struct wcd_mbhc *mbhc = data;

	ad_logn("%s: enter\n", __func__);
	if (unlikely(wcd9xxx_spmi_lock_sleep() == false)) {
		ad_logw("%s: failed to hold suspend\n", __func__);
		r = IRQ_NONE;
	} else {
		/* Call handler */
		mbhc->in_mech_irq_handler = true;
		wcd_mbhc_swch_irq_handler(mbhc);
		mbhc->in_mech_irq_handler = false;
		wcd9xxx_spmi_unlock_sleep();
	}
	ad_logn("%s: leave %d\n", __func__, r);
	return r;
}

static int wcd_mbhc_get_button_mask(u16 btn)
{
	int mask = 0;
	switch (btn) {
	case 0:
		mask = SND_JACK_BTN_0;
		ad_logn("%s: detect SND_JACK_BTN_0 [Start/Pause btn]\n", __func__);
		break;
	case 1:
		mask = SND_JACK_BTN_1;
		ad_logn("%s: detect SND_JACK_BTN_1 [+ btn]\n", __func__);
		break;
	case 3:
		mask = SND_JACK_BTN_2;
		ad_logn("%s: detect SND_JACK_BTN_2 [- btn]\n", __func__);
		break;
	case 7:
		mask = SND_JACK_BTN_3;
		ad_logn("%s: detect SND_JACK_BTN_3\n", __func__);
		break;
	case 15:
		mask = SND_JACK_BTN_4;
		ad_logn("%s: detect SND_JACK_BTN_4\n", __func__);
		break;
	default:
		break;
	}
	return mask;
}

static irqreturn_t wcd_mbhc_hs_ins_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	struct snd_soc_codec *codec = mbhc->codec;
	u16 result2;
	bool detection_type;
	static u16 hphl_trigerred;
	static u16 mic_trigerred;

	ad_logn("%s: enter\n", __func__);
	if (!mbhc->mbhc_cfg->detect_extn_cable) {
		ad_logn("%s: Returning as Extension cable feature not enabled\n",
			__func__);
		return IRQ_HANDLED;
	}
	if (mbhc->in_mech_irq_handler){
			ad_logn("%s: Returning as mechanical interrupt happens", __func__);
			/*
			 * Only removal mechanical interrupt can come if electrical interrupt
			 * are already triggering because we enable schmitt trigger after
			 * the mecahnical insertion interrupt when plug type is found as high_hph
			 * so we can disable electrical interrupt in such case because it means
			 * that the extension cable has been removed.
			 */
			wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_ins_intr);
			wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_rem_intr);
			return IRQ_HANDLED;
	}
	WCD_MBHC_RSC_LOCK(mbhc);

	detection_type = (snd_soc_read(codec,
				MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1)) & 0x01;
	result2 = (snd_soc_read(codec,
		MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT));

	ad_logn("%s: detection_type %d, result2 %x\n", __func__,
				detection_type, result2);
	if (detection_type) {
		/* chech if both Left and MIC Schmitt triggers are triggered */
		if (result2 == 0x0A) {
			/* Go for plug type determination */
			ad_logn("%s: Go for plug type determination\n",
				  __func__);
			goto determine_plug;

		} else {
			if (result2 & 0x02) {
				mic_trigerred++;
				ad_logn("%s: Insertion MIC trigerred %d\n",
					 __func__, mic_trigerred);
				snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
					0x06, 0x00);
				msleep(20);
				snd_soc_update_bits(codec,
					MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
					0x06, 0x06);
			}
			if (result2 & 0x08) {
				hphl_trigerred++;
				ad_logn("%s: Insertion HPHL trigerred %d\n",
					 __func__, hphl_trigerred);
			}
			if (mic_trigerred && hphl_trigerred) {
				/* Go for plug type determination */
				ad_logn("%s: Go for plug type determination\n",
					 __func__);
				goto determine_plug;
			}
		}
	}
	WCD_MBHC_RSC_UNLOCK(mbhc);
	ad_logn("%s: leave\n", __func__);
	return IRQ_HANDLED;

determine_plug:
	/*
	 * Disable HPHL trigger and MIC Schmitt triggers.
	 * Setup for insertion detection.
	 */
	ad_logn("%s: Disable insertion interrupt\n", __func__);
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_ins_intr);

	snd_soc_update_bits(codec,
		MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
		0x06, 0x00);
	/* Enable HW FSM */
	snd_soc_update_bits(codec,
		MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
		0x80, 0x80);
	hphl_trigerred = 0;
	mic_trigerred = 0;
	mbhc->is_extn_cable = true;
	/*
	 * even if controls comes here, it is taking more than
	 * debounce time to detect cable as headphone, so wait
	 * for 20msec for the mic line to stabilize
	 */
	msleep(20);
	mbhc->btn_press_intr = false;
	wcd_mbhc_detect_plug_type(mbhc);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	ad_logn("%s: leave\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hs_rem_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	struct snd_soc_codec *codec = mbhc->codec;
	u16 result2;
	static u16 hphl_trigerred;
	static u16 mic_trigerred;
	unsigned long timeout;
	bool removed = true;
	int retry = 0;
	bool is_pa_on = false;
	ad_logn("%s: enter\n", __func__);

	if (mbhc->in_mech_irq_handler){
		ad_logn("%s: Returning as mechanical interrupt happens", __func__);
		/*
		 * Only removal mechanical interrupt can come if electrical interrupt
		 * are already triggering because we enable schmitt trigger after
		 * the mecahnical insertion interrupt when plug type is found as high_hph
		 * so we can disable electrical interrupt in such case because it means
		 * that the extension cable has been removed.
		 */
		wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_ins_intr);
		wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_rem_intr);
		return IRQ_HANDLED;
	}
	WCD_MBHC_RSC_LOCK(mbhc);

	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	timeout = jiffies +
		  msecs_to_jiffies(WCD_FAKE_REMOVAL_MIN_PERIOD_MS);
	do {
		retry++;
		/*
		 * read the result2 register every 10ms to look for
		 * any change in HS_COMP_RESULT bit
		 */
		usleep_range(10000, 10100);
		result2 = (snd_soc_read(codec,
			MSM8X16_WCD_A_ANALOG_MBHC_ZDET_ELECT_RESULT));
		ad_logn("%s: check result2 for fake removal: %x\n",
			 __func__, result2);
		if ((!(result2 & 0x01)) &&
			(retry > FAKE_REM_RETRY_ATTEMPTS)) {
			removed = false;
			break;
		}
	} while (!time_after(jiffies, timeout));

	ad_logn("%s: headset %s actually removed\n", __func__,
		removed ? "" : "not ");

	if (removed) {
		if (!result2) {
			/*
			 * extension cable is still plugged in
			 * report it as LINEOUT device
			 */
			goto report_unplug;
		} else {
			if (!(result2 & 0x02)) {
				mic_trigerred++;
				ad_logn("%s: Removal MIC trigerred %d\n",
					 __func__, mic_trigerred);
			}
			if (!(result2 & 0x08)) {
				hphl_trigerred++;
				ad_logn("%s: Removal HPHL trigerred %d\n",
					 __func__, hphl_trigerred);
			}
			if (mic_trigerred && hphl_trigerred) {
				/*
				 * extension cable is still plugged in
				 * report it as LINEOUT device
				 */
				goto report_unplug;
			}
		}
	}
	WCD_MBHC_RSC_UNLOCK(mbhc);
	ad_logn("%s: leave\n", __func__);
	return IRQ_HANDLED;

report_unplug:

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		ad_logn("%s: button press is canceled\n", __func__);
	/* cancel correct work function */
	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	ad_logn("%s: Report extension cable\n", __func__);
	wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
	/*
	 * If PA is enabled HPHL schmitt trigger can
	 * be unreliable, make sure to disable it
	 */
	is_pa_on = snd_soc_read(codec,
				MSM8X16_WCD_A_ANALOG_RX_HPH_CNP_EN) &
				0x30;
	if (is_pa_on)
		wcd_mbhc_set_and_turnoff_hph_padac(mbhc);
	/*
	 * Disable HPHL trigger and MIC Schmitt triggers.
	 * Setup for insertion detection.
	 */
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_rem_intr);
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_NONE);
	/* Disable HW FSM */
	snd_soc_update_bits(codec,
		MSM8X16_WCD_A_ANALOG_MBHC_FSM_CTL,
		0x80, 0x00);
	snd_soc_update_bits(codec,
		MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
		0x06, 0x06);
	/* Set the detection type appropriately */
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1,
				0x01, 0x01);
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->mbhc_hs_ins_intr);
	//wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
	hphl_trigerred = 0;
	mic_trigerred = 0;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	ad_logn("%s: leave\n", __func__);
	return IRQ_HANDLED;
}

static void wcd_btn_lpress_fn(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct wcd_mbhc *mbhc;
	struct snd_soc_codec *codec;
	s16 result1;

	ad_logd("%s: Enter\n", __func__);

	dwork = to_delayed_work(work);
	mbhc = container_of(dwork, struct wcd_mbhc, mbhc_btn_dwork);
	codec = mbhc->codec;

	result1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET) {
		ad_logd("%s: Reporting long button press event, result1: %d\n",
			 __func__, result1);
		wcd_mbhc_jack_report(mbhc, &mbhc->button_jack,
				mbhc->buttons_pressed, mbhc->buttons_pressed);
	}
	ad_logd("%s: leave\n", __func__);
	wcd9xxx_spmi_unlock_sleep();
}

static bool wcd_mbhc_fw_validate(const void *data, size_t size)
{
	u32 cfg_offset;
	struct wcd_mbhc_btn_detect_cfg *btn_cfg;
	struct firmware_cal fw;

	fw.data = (void *)data;
	fw.size = size;

	if (fw.size < WCD_MBHC_CAL_MIN_SIZE)
		return false;

	/*
	 * Previous check guarantees that there is enough fw data up
	 * to num_btn
	 */
	btn_cfg = WCD_MBHC_CAL_BTN_DET_PTR(fw.data);
	cfg_offset = (u32) ((void *) btn_cfg - (void *) fw.data);
	if (fw.size < (cfg_offset + WCD_MBHC_CAL_BTN_SZ(btn_cfg)))
		return false;

	return true;
}

irqreturn_t wcd_mbhc_btn_press_handler(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	struct snd_soc_codec *codec = mbhc->codec;
	u16 result1;
	int mask;
	unsigned long msec_val;

	ad_logn("%s: enter\n", __func__);
	WCD_MBHC_RSC_LOCK(mbhc);
	/* send event to sw intr handler*/
	mbhc->is_btn_press = true;
	wake_up_interruptible(&mbhc->wait_btn_press);
	if (wcd_swch_level_remove(mbhc)) {
		ad_logn("%s: Switch level is low ", __func__);
		goto done;
	}
#ifdef CONFIG_HUAWEI_DSM
	if(time_before(jiffies, timeout_jiffies))
	{
		if(!ispress)
		{
			audio_dsm_report_num(DSM_AUDIO_HANDSET_PLUG_PRESS_ERROR, DSM_AUDIO_MESG_PLUG_PRESS_ERROR);
		}
	}
	else
	{
		timeout_jiffies = jiffies + msecs_to_jiffies(DSM_AUDIO_LESS_HS_GAP);
		ispress = true;
	}
#endif
	mbhc->btn_press_intr = true;

	msec_val = jiffies_to_msecs(jiffies - mbhc->jiffies_atreport);
	ad_logn("%s: msec_val = %ld\n", __func__, msec_val);
	if (msec_val < MBHC_BUTTON_PRESS_THRESHOLD_MIN) {
		ad_logn("%s: Too short, ignore button press\n", __func__);
		goto done;
	}

	/* If switch interrupt already kicked in, ignore button press */
	if (mbhc->in_swch_irq_handler) {
		ad_logn("%s: Swtich level changed, ignore button press\n",
			 __func__);
		goto done;
	}
	if (mbhc->current_plug != MBHC_PLUG_TYPE_HEADSET) {
		if ((mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH) && (!det_extn_cable_en)) {
			ad_logd("%s: Ignore this button press\n",
					__func__);
			mbhc->btn_press_intr = false;
			mbhc->is_btn_press = false;
			goto done;
		}
		ad_logd("%s: Plug isn't headset, ignore button press\n",
				__func__);
		goto done;
	}
	result1 = snd_soc_read(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN_RESULT);
	mask = wcd_mbhc_get_button_mask(result1);
	mbhc->buttons_pressed |= mask;
	wcd9xxx_spmi_lock_sleep();
	if (schedule_delayed_work(&mbhc->mbhc_btn_dwork,
				msecs_to_jiffies(400)) == 0) {
		WARN(1, "Button pressed twice without release event\n");
		wcd9xxx_spmi_unlock_sleep();
	}
done:
	ad_logn("%s: leave\n", __func__);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_release_handler(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	int ret;

	ad_logn("%s: enter\n", __func__);
	WCD_MBHC_RSC_LOCK(mbhc);
	if (wcd_swch_level_remove(mbhc)) {
		ad_logn("%s: Switch level is low ", __func__);
		goto exit;
	}

	if (mbhc->btn_press_intr) {
		mbhc->btn_press_intr = false;
	} else {
		ad_logd("%s: This release is for fake btn press\n", __func__);
		goto exit;
	}

	/*
	 * If current plug is headphone then there is no chance to
	 * get btn release interrupt, so connected cable should be
	 * headset not headphone.
	 */
	if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE) {
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_HEADSET);
		goto exit;

	}
	if (mbhc->buttons_pressed & WCD_MBHC_JACK_BUTTON_MASK) {
		ret = wcd_cancel_btn_work(mbhc);
		if (ret == 0) {
			ad_loge("%s: Reporting long button release event\n",
				 __func__);
			wcd_mbhc_jack_report(mbhc, &mbhc->button_jack,
					0, mbhc->buttons_pressed);
		} else {
			if (mbhc->in_swch_irq_handler) {
				ad_logd("%s: Switch irq kicked in, ignore\n",
					__func__);
			} else {
				ad_loge("%s: Reporting btn press\n",
					 __func__);
				wcd_mbhc_jack_report(mbhc,
						     &mbhc->button_jack,
						     mbhc->buttons_pressed,
						     mbhc->buttons_pressed);
				ad_loge("%s: Reporting btn release\n",
					 __func__);
				wcd_mbhc_jack_report(mbhc,
						&mbhc->button_jack,
						0, mbhc->buttons_pressed);
			}
		}
		mbhc->buttons_pressed &= ~WCD_MBHC_JACK_BUTTON_MASK;
	}
	/* huawei: ignore the btn3~7, only clear the mask , donot report */
#ifdef CONFIG_HUAWEI_KERNEL
	else
	{
		ad_loge("%s: HUAWEI-- ignore reporting btn3~7 !\n",
					 __func__);
		mbhc->buttons_pressed &= ~WCD_MBHC_JACK_BUTTON_MASK_HUAWEI_IGNORE;
	}
#endif
exit:
	ad_logd("%s: leave\n", __func__);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hphl_ocp_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	struct snd_soc_codec *codec;

	ad_logn("%s: received HPHL OCP irq\n", __func__);
	if (mbhc) {
		codec = mbhc->codec;
		if ((mbhc->hphlocp_cnt < OCP_ATTEMPT) &&
		    (!mbhc->hphrocp_cnt)) {
			ad_logn("%s: retry\n", __func__);
			mbhc->hphlocp_cnt++;
			snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x00);
			snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x10);
		} else {
			wcd9xxx_spmi_disable_irq(mbhc->intr_ids->hph_left_ocp);
			mbhc->hph_status |= SND_JACK_OC_HPHL;
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					    mbhc->hph_status,
					    WCD_MBHC_JACK_MASK);
		}
	} else {
		ad_loge("%s: Bad wcd9xxx_spmi private data\n", __func__);
	}
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hphr_ocp_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	struct snd_soc_codec *codec;

	ad_logn("%s: received HPHR OCP irq\n", __func__);
	codec = mbhc->codec;
	if ((mbhc->hphrocp_cnt < OCP_ATTEMPT) &&
	    (!mbhc->hphlocp_cnt)) {
		ad_logn("%s: retry\n", __func__);
		mbhc->hphrocp_cnt++;
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x00);
		snd_soc_update_bits(codec,
				MSM8X16_WCD_A_ANALOG_RX_COM_OCP_CTL,
				0x10, 0x10);
	} else {
		wcd9xxx_spmi_disable_irq(mbhc->intr_ids->hph_right_ocp);
		mbhc->hph_status |= SND_JACK_OC_HPHR;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				    mbhc->hph_status, WCD_MBHC_JACK_MASK);
	}
	return IRQ_HANDLED;
}

static int wcd_mbhc_initialise(struct wcd_mbhc *mbhc)
{
	int ret = 0;
	struct snd_soc_codec *codec = mbhc->codec;

	ad_logn("%s: enter\n", __func__);
	/* Bring the digital block out of reset */
	snd_soc_update_bits(codec, MSM8X16_WCD_A_DIGITAL_CDC_RST_CTL,
			0x80, 0x80);
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_1, 0xB5);
	/* enable HS detection */
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2, 0xE8);
	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2, 0x18,
				(mbhc->hphl_swh << 4 | mbhc->gnd_swh << 3));

	snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_DET_CTL_2,
			0x01, 0x01);
    /* change the debounce time from 768ms to 384ms,so SW is receiving mechanical removal
       and button press interrupts at the same time and hence not reporting button press */
	snd_soc_write(codec, MSM8X16_WCD_A_ANALOG_MBHC_DBNC_TIMER, 0xA8);
	/* enable MBHC clock */
	snd_soc_update_bits(codec,
			MSM8X16_WCD_A_DIGITAL_CDC_DIG_CLK_CTL,
			0x08, 0x08);
	/* program HS_VREF value */
	wcd_program_hs_vref(mbhc);

	/* Program Button threshold registers */
	wcd_program_btn_threshold(mbhc, false);

	/*alter the HSREF to 1.7V to fix the issue of AT test */
	ad_logn("%s: det_extn_cable_en = %d\n", __func__, det_extn_cable_en);
	if(det_extn_cable_en)
	{
		snd_soc_update_bits(codec, MSM8X16_WCD_A_ANALOG_MBHC_BTN3_CTL,
				0x03, 0x03);
	}

	INIT_WORK(&mbhc->correct_plug_swch, wcd_correct_swch_plug);
	/* enable the WCD MBHC IRQ's */
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->mbhc_sw_intr);
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->mbhc_btn_press_intr);
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->mbhc_btn_release_intr);
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->hph_left_ocp);
	wcd9xxx_spmi_enable_irq(mbhc->intr_ids->hph_right_ocp);
	ad_logn("%s: leave\n", __func__);
	return ret;
}

static void wcd_mbhc_fw_read(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct wcd_mbhc *mbhc;
	struct snd_soc_codec *codec;
	const struct firmware *fw;
	struct firmware_cal *fw_data = NULL;
	int ret = -1, retry = 0;
	bool use_default_cal = false;

	dwork = to_delayed_work(work);
	mbhc = container_of(dwork, struct wcd_mbhc, mbhc_firmware_dwork);
	codec = mbhc->codec;

	while (retry < FW_READ_ATTEMPTS) {
		retry++;
		ad_logd("%s:Attempt %d to request MBHC firmware\n",
			__func__, retry);
		if (mbhc->mbhc_cb->get_hwdep_fw_cal)
			fw_data = mbhc->mbhc_cb->get_hwdep_fw_cal(codec,
					WCD9XXX_MBHC_CAL);
		if (!fw_data)
			ret = request_firmware(&fw, "wcd9320/wcd9320_mbhc.bin",
				       codec->dev);
		/*
		 * if request_firmware and hwdep cal both fail then
		 * sleep for 4sec for the userspace to send data to kernel
		 * retry for few times before bailing out
		 */
		if ((ret != 0) && !fw_data) {
			usleep_range(FW_READ_TIMEOUT, FW_READ_TIMEOUT +
						WCD9XXX_USLEEP_RANGE_MARGIN_US);
		} else {
			ad_logd("%s: MBHC Firmware read succesful\n",
					__func__);
			break;
		}
	}
	if (!fw_data)
		ad_logd("%s: using request_firmware\n", __func__);
	else
		ad_logd("%s: using hwdep cal\n", __func__);

	if (ret != 0 && !fw_data) {
		ad_loge("%s: Cannot load MBHC firmware use default cal\n",
		       __func__);
		use_default_cal = true;
	}
	if (!use_default_cal) {
		const void *data;
		size_t size;

		if (fw_data) {
			data = fw_data->data;
			size = fw_data->size;
		} else {
			data = fw->data;
			size = fw->size;
		}
		if (wcd_mbhc_fw_validate(data, size) == false) {
			ad_loge("%s: Invalid MBHC cal data size use default cal\n",
				__func__);
			if (!fw_data)
				release_firmware(fw);
		} else {
			if (fw_data) {
				mbhc->mbhc_cfg->calibration =
					(void *)fw_data->data;
				mbhc->mbhc_cal = fw_data;
			} else {
				mbhc->mbhc_cfg->calibration =
					(void *)fw->data;
				mbhc->mbhc_fw = fw;
			}
		}

	}

	(void) wcd_mbhc_initialise(mbhc);
}

int wcd_mbhc_start(struct wcd_mbhc *mbhc,
		       struct wcd_mbhc_config *mbhc_cfg)
{
	int rc = 0;

	ad_logn("%s: enter\n", __func__);
	/* update the mbhc config */
	mbhc->mbhc_cfg = mbhc_cfg;

	if (!mbhc->mbhc_cfg->read_fw_bin ||
	    (mbhc->mbhc_cfg->read_fw_bin && mbhc->mbhc_fw) ||
	    (mbhc->mbhc_cfg->read_fw_bin && mbhc->mbhc_cal)) {
		rc = wcd_mbhc_initialise(mbhc);
	} else {
		if (!mbhc->mbhc_fw || !mbhc->mbhc_cal)
			schedule_delayed_work(&mbhc->mbhc_firmware_dwork,
				      usecs_to_jiffies(FW_READ_TIMEOUT));
		else
			ad_loge("%s: Skipping to read mbhc fw, 0x%p %p\n",
				 __func__, mbhc->mbhc_fw, mbhc->mbhc_cal);
	}
	ad_logn("%s: leave %d\n", __func__, rc);
	return rc;
}
EXPORT_SYMBOL(wcd_mbhc_start);

void wcd_mbhc_stop(struct wcd_mbhc *mbhc)
{
	ad_logn("%s: enter\n", __func__);
	mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->hph_left_ocp);
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->hph_right_ocp);

	if (mbhc->mbhc_fw || mbhc->mbhc_cal) {
		cancel_delayed_work_sync(&mbhc->mbhc_firmware_dwork);
		if (!mbhc->mbhc_cal)
			release_firmware(mbhc->mbhc_fw);
		mbhc->mbhc_fw = NULL;
		mbhc->mbhc_cal = NULL;
	}
	ad_logn("%s: leave\n", __func__);
}
EXPORT_SYMBOL(wcd_mbhc_stop);

/*
 * wcd_mbhc_init : initialize MBHC internal structures.
 *
 * NOTE: mbhc->mbhc_cfg is not YET configure so shouldn't be used
 */
int wcd_mbhc_init(struct wcd_mbhc *mbhc, struct snd_soc_codec *codec,
		      const struct wcd_mbhc_cb *mbhc_cb,
		      const struct wcd_mbhc_intr *mbhc_cdc_intr_ids,
		      bool impedance_det_en)
{
	int ret = 0;
	int hph_swh = 0;
	int gnd_swh = 0;
	struct snd_soc_card *card = codec->card;
	const char *hph_switch = "qcom,msm-mbhc-hphl-swh";
	const char *gnd_switch = "qcom,msm-mbhc-gnd-swh";
	const char *ext1_cap = "qcom,msm-micbias1-ext-cap";
	const char *ext2_cap = "qcom,msm-micbias2-ext-cap";
	const char *mbhc_cap_mark = "huawei,cable-enable-mbhc";

	ad_logn("%s: enter\n", __func__);

	ret = of_property_read_u32(card->dev->of_node, hph_switch, &hph_swh);
	if (ret) {
		ad_dev_loge(card->dev,
			"%s: missing %s in dt node\n", __func__, hph_switch);
		goto err;
	}

	ret = of_property_read_u32(card->dev->of_node, gnd_switch, &gnd_swh);
	if (ret) {
		ad_dev_loge(card->dev,
			"%s: missing %s in dt node\n", __func__, gnd_switch);
		goto err;
	}
	mbhc->micbias1_cap_mode =
		(of_property_read_bool(card->dev->of_node, ext1_cap) ?
		MICBIAS_EXT_BYP_CAP : MICBIAS_NO_EXT_BYP_CAP);

	mbhc->micbias2_cap_mode =
		(of_property_read_bool(card->dev->of_node, ext2_cap) ?
		MICBIAS_EXT_BYP_CAP : MICBIAS_NO_EXT_BYP_CAP);

	det_extn_cable_en = (of_property_read_bool(card->dev->of_node, mbhc_cap_mark) ? 1 : 0);
	ad_logn("%s: det_extn_cable_en = %d\n", __func__, det_extn_cable_en);

	mbhc->in_swch_irq_handler = false;
	mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	mbhc->is_btn_press = false;
	mbhc->codec = codec;
	mbhc->intr_ids = mbhc_cdc_intr_ids;
	mbhc->impedance_detect = impedance_det_en;
	mbhc->hphl_swh = hph_swh;
	mbhc->gnd_swh = gnd_swh;
	mbhc->micbias_enable = false;
	mbhc->mbhc_cb = mbhc_cb;
	mbhc->btn_press_intr = false;
	mbhc->is_hs_recording = false;
	mbhc->is_extn_cable = false;
	mbhc->in_mech_irq_handler = false;

	if (mbhc->intr_ids == NULL) {
		ad_loge("%s: Interrupt mapping not provided\n", __func__);
		return -EINVAL;
	}

	if (mbhc->headset_jack.jack == NULL) {
		ret = snd_soc_jack_new(codec, "Headset Jack",
				WCD_MBHC_JACK_MASK, &mbhc->headset_jack);
		if (ret) {
			ad_loge("%s: Failed to create new jack\n", __func__);
			return ret;
		}

		ret = snd_soc_jack_new(codec, "Button Jack",
				       WCD_MBHC_JACK_BUTTON_MASK,
				       &mbhc->button_jack);
		if (ret) {
			ad_loge("Failed to create new jack\n");
			return ret;
		}

		ret = snd_jack_set_key(mbhc->button_jack.jack,
				       SND_JACK_BTN_0,
				       KEY_MEDIA);
		if (ret) {
			ad_loge("%s: Failed to set code for btn-0\n",
				__func__);
			return ret;
		}

		ret = snd_jack_set_key(mbhc->button_jack.jack,
		                       SND_JACK_BTN_1,
		                       KEY_VOLUMEUP);
		if (ret) {
		        ad_loge("%s: Failed to set code for btn-1\n",
		                __func__);
		        return ret;
		}
		ret = snd_jack_set_key(mbhc->button_jack.jack,
		                       SND_JACK_BTN_2,
		                       KEY_VOLUMEDOWN);
		if (ret) {
		        ad_loge("%s: Failed to set code for btn-2\n",
		                __func__);
		        return ret;
		}
		INIT_DELAYED_WORK(&mbhc->mbhc_firmware_dwork,
				  wcd_mbhc_fw_read);
		INIT_DELAYED_WORK(&mbhc->mbhc_btn_dwork, wcd_btn_lpress_fn);
	}

	/* Register event notifier */
	mbhc->nblock.notifier_call = wcd_event_notify;
	ret = msm8x16_register_notifier(codec, &mbhc->nblock);
	if (ret) {
		ad_loge("%s: Failed to register notifier %d\n", __func__, ret);
		return ret;
	}

	init_waitqueue_head(&mbhc->wait_btn_press);
	mutex_init(&mbhc->codec_resource_lock);

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->mbhc_sw_intr,
				  wcd_mbhc_mech_plug_detect_irq,
				  "mbhc sw intr", mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d, ret = %d\n", __func__,
		       mbhc->intr_ids->mbhc_sw_intr, ret);
		goto err_mbhc_sw_irq;
	}

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->mbhc_btn_press_intr,
				  wcd_mbhc_btn_press_handler,
				  "Button Press detect",
				  mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_btn_press_intr);
		goto err_btn_press_irq;
	}

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->mbhc_btn_release_intr,
				  wcd_mbhc_release_handler,
				  "Button Release detect", mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
			mbhc->intr_ids->mbhc_btn_release_intr);
		goto err_btn_release_irq;
	}

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->mbhc_hs_ins_intr,
				  wcd_mbhc_hs_ins_irq,
				  "Elect Insert", mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_hs_ins_intr);
		goto err_mbhc_hs_ins_irq;
	}
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_ins_intr);

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->mbhc_hs_rem_intr,
				  wcd_mbhc_hs_rem_irq,
				  "Elect Remove", mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_hs_rem_intr);
		goto err_mbhc_hs_rem_irq;
	}
	wcd9xxx_spmi_disable_irq(mbhc->intr_ids->mbhc_hs_rem_intr);

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->hph_left_ocp,
				  wcd_mbhc_hphl_ocp_irq, "HPH_L OCP detect",
				  mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->hph_left_ocp);
		goto err_hphl_ocp_irq;
	}

	ret = wcd9xxx_spmi_request_irq(mbhc->intr_ids->hph_right_ocp,
				  wcd_mbhc_hphr_ocp_irq, "HPH_R OCP detect",
				  mbhc);
	if (ret) {
		ad_loge("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->hph_right_ocp);
		goto err_hphr_ocp_irq;
	}

	ad_logn("%s: leave ret %d\n", __func__, ret);
	return ret;

err_hphr_ocp_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->hph_left_ocp, mbhc);
err_hphl_ocp_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_hs_rem_intr, mbhc);
err_mbhc_hs_rem_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_hs_ins_intr, mbhc);
err_mbhc_hs_ins_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_btn_release_intr, mbhc);
err_btn_release_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_btn_press_intr, mbhc);
err_btn_press_irq:
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_sw_intr, mbhc);
err_mbhc_sw_irq:
	msm8x16_unregister_notifier(codec, &mbhc->nblock);
	mutex_destroy(&mbhc->codec_resource_lock);
err:
	ad_logn("%s: leave err ret %d\n", __func__, ret);
	return ret;
}
EXPORT_SYMBOL(wcd_mbhc_init);

void wcd_mbhc_deinit(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;

	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_sw_intr, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_btn_press_intr, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_btn_release_intr, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_hs_ins_intr, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->mbhc_hs_rem_intr, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->hph_left_ocp, mbhc);
	wcd9xxx_spmi_free_irq(mbhc->intr_ids->hph_right_ocp, mbhc);
	msm8x16_unregister_notifier(codec, &mbhc->nblock);
	mutex_destroy(&mbhc->codec_resource_lock);
}
EXPORT_SYMBOL(wcd_mbhc_deinit);

MODULE_DESCRIPTION("wcd MBHC v2 module");
MODULE_LICENSE("GPL v2");
