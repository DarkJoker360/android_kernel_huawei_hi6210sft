/*
 *  Hisilicon K3 soc camera ISP driver source file
 *
 *  CopyRight (C) Hisilicon Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 - 1307  USA
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <asm/io.h>
#include <asm/bug.h>
#include <linux/poll.h>
#include <linux/interrupt.h>
//#include <linux/android_pmem.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/time.h>

#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/workqueue.h>
#include <linux/semaphore.h>

#include <mach/boardid.h>
#include "mini_cam_util.h"
#include "mini_cam_dbg.h"
#include "mini_k3_isp.h"
#include "mini_k3_ispv1.h"
#include "mini_k3_ispv1_afae.h"
#include "mini_k3_isp_io.h"
/*< DTS2013072406684 zhoupengtao 00124583 2013-07-24 add for export driver interface begin */
#include "mini_hwa_cam_tune_common.h"
/* DTS2013072406684 zhoupengtao 00124583 2013-07-24 add for export driver interface end >*/
#include "camera_agent.h"
#define DEBUG_DEBUG 0
#define LOG_TAG "K3_ISPV1_TUNE_OPS"
#include "mini_cam_log.h"

mini_k3_isp_data * mini_this_ispdata;
static bool camera_ajustments_flag;

static int scene_target_y_low = DEFAULT_TARGET_Y_LOW;
static int scene_target_y_high = DEFAULT_TARGET_Y_HIGH;
static bool fps_lock;

/* changed by y00215412 for DTS2012082002461 2012-11-05 */
static bool ispv1_change_frame_rate(
	camera_frame_rate_state *state, camera_frame_rate_dir direction, mini_camera_sensor *sensor);
static int ispv1_set_frame_rate(camera_frame_rate_mode mode, mini_camera_sensor *sensor);

/* added by w00225854 for DTS2012102902756 2013-01-26 */
static void ispv1_uv_stat_work_func(struct work_struct *work);
static struct workqueue_struct *uv_stat_work_queue;
static struct work_struct uv_stat_work;
/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
static struct workqueue_struct *aeag_work_queue = NULL;
static struct work_struct aeag_work;
/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
static inline mini_effect_params *get_effect_ptr(void)
{
	return mini_this_ispdata->sensor->effect;
}
/*
 * For anti-shaking, y36721 todo
 * ouput_width-2*blocksize
 * ouput_height-2*blocksize
*/
int mini_ispv1_set_anti_shaking_block(int blocksize)
{
	mini_camera_rect_s out, stat;
	u8 reg1;

	print_debug("enter %s", __func__);

	/* bit4: 0-before yuv downscale;1-after yuv dcw, but before yuv upscale */
	reg1 = GETREG8(REG_ISP_SCALE6_SELECT);
	reg1 |= (1 << 4);
	SETREG8(REG_ISP_SCALE6_SELECT, reg1);

	/* y36721 todo */
	out.left = 0;
	out.top = 0;

	out.width = blocksize;
	out.height = blocksize;

	mini_k3_isp_antishaking_rect_out2stat(&out, &stat);

	SETREG8(REG_ISP_ANTI_SHAKING_BLOCK_SIZE, (stat.width & 0xff00) >> 8);
	SETREG8(REG_ISP_ANTI_SHAKING_BLOCK_SIZE + 1, stat.width & 0xff);

	/*
	 * y36721 todo
	 * should set REG_ISP_ANTI_SHAKING_ABSSTART_POSITION_H and V
	 * REG_ISP_ANTI_SHAKING_ABSSTART_POSITION_H
	 * REG_ISP_ANTI_SHAKING_ABSSTART_POSITION_V
	 */

	return 0;
}

int mini_ispv1_set_anti_shaking(camera_anti_shaking flag)
{
	print_debug("enter %s", __func__);

	if (flag)
		SETREG8(REG_ISP_ANTI_SHAKING_ENABLE, 1);
	else
		SETREG8(REG_ISP_ANTI_SHAKING_ENABLE, 0);

	return 0;
}

/* read out anti-shaking coordinate */
int mini_ispv1_get_anti_shaking_coordinate(mini_coordinate_s *coordinate)
{
	u8 reg0;
	mini_camera_rect_s out, stat;

	print_debug("enter %s", __func__);

	reg0 = GETREG8(REG_ISP_ANTI_SHAKING_ENABLE);

	if ((reg0 & 0x1) != 1) {
		print_error("anti_shaking not working!");
		return -1;
	}

	GETREG16(REG_ISP_ANTI_SHAKING_WIN_LEFT, stat.left);
	GETREG16(REG_ISP_ANTI_SHAKING_WIN_TOP, stat.top);
	stat.width = 0;
	stat.height = 0;

	mini_k3_isp_antishaking_rect_stat2out(&out, &stat);

	coordinate->x = out.left;
	coordinate->y = out.top;

	return 0;

}

/* Added for ISO, target Y will not change with ISO */
int mini_ispv1_set_iso(camera_iso iso)
{
	int max_iso, min_iso;
	int max_gain, min_gain;
	int retvalue = 0;
	mini_camera_sensor *sensor;
    mini_ae_params_s *this_ae;
	if (NULL == mini_this_ispdata) {
		print_info("mini_this_ispdata is NULL");
		return -1;
	}
	sensor = mini_this_ispdata->sensor;

	print_debug("enter %s", __func__);
	return  camera_agent_set_ISOLEVEL( sensor->sensor_index, iso);//by wind

	/* ISO is to change sensor gain, but is not same */
	switch (iso) {
	case CAMERA_ISO_AUTO:
		this_ae = &sensor->effect->ae_param;
			/*iso limit is also sensor related, sensor itself should provide a method to define its iso limitation*/
			/*if sensor does not  provide this method,make sure that the default iso shuold be set here */
		max_iso = this_ae->iso_max;
		min_iso = this_ae->iso_min;
		break;

	case CAMERA_ISO_100:
		/* max and min iso should be fixed ISO100, DTS2012040905370 y36721 2012-04-10 revised */
		max_iso = (100 + (100 / 8) * 2);
		min_iso = 100;
		break;

	case CAMERA_ISO_200:
		/* max and min iso should be fixed ISO200 */
		max_iso = (200 + 200 / 8);
		min_iso = (200 - 200 / 8);
		break;

	case CAMERA_ISO_400:
		/* max and min iso should be fixed ISO400 */
		max_iso = (400 + 400 / 8);
		min_iso = (400 - 400 / 8);
		break;

	case CAMERA_ISO_800:
		/* max and min iso should be fixed ISO800 */
		max_iso = 775;
		min_iso = 650;
		break;

	case CAMERA_ISO_1600:
		/* max and min iso should be fixed ISO1600 */
		max_iso = 1600;
		min_iso = 1400;
		break;

	default:
		retvalue = -1;
		goto out;
		break;
	}

	max_gain = max_iso * 0x10 / 100;
	min_gain = min_iso * 0x10 / 100;

	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, begin */
	if( (sensor->frmsize_list[sensor->preview_frmsize_index].summary == false)
	&&(true == sensor->support_summary)){
		max_gain = max_gain * 2;
	}
	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, end */

	/* set to ISP registers */
	SETREG8(REG_ISP_MAX_GAIN, (max_gain & 0xff00) >> 8);
	SETREG8(REG_ISP_MAX_GAIN + 1, max_gain & 0xff);
	SETREG8(REG_ISP_MIN_GAIN, (min_gain & 0xff00) >> 8);
	SETREG8(REG_ISP_MIN_GAIN + 1, min_gain & 0xff);

	sensor->min_gain = min_gain;
	sensor->max_gain = max_gain;

out:
	return retvalue;
}

int inline mini_ispv1_iso2gain(int iso, bool summary)
{
	int gain;

	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, begin */
	mini_camera_sensor *sensor = mini_this_ispdata->sensor;

	if ((summary == false)
	&&(true == sensor->support_summary)){
		iso *= 2;
	}
	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, end */

	gain = iso * 0x10 / 100;
	return gain;
}

int inline mini_ispv1_gain2iso(int gain, bool summary)
{
	int iso;

	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, begin */
	mini_camera_sensor *sensor = mini_this_ispdata->sensor;

	iso = gain * 100 / 0x10;
	if( (summary == false)
	&&(true == sensor->support_summary)){
		iso /= 2;
	}
	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, end */

	/* modified by h00206029 2012-05-11 */
	iso = (iso + 5) / 10 * 10;
	return iso;
}

u32 mini_get_writeback_cap_gain(void)
{
	u8 reg5 = GETREG8(COMMAND_REG5);
	u32 gain = 0;

	reg5 &= 0x3;
	if (reg5 == 0 || reg5 == 2) {
		GETREG16(ISP_FIRST_FRAME_GAIN, gain);
	} else if (reg5 == 1) {
		GETREG16(ISP_SECOND_FRAME_GAIN, gain);
	} else if (reg5 == 3) {
		GETREG16(ISP_THIRD_FRAME_GAIN, gain);
	}

	return gain;
}

u32 mini_get_writeback_cap_expo(void)
{
	u8 reg5 = GETREG8(COMMAND_REG5);
	u32 expo = 0;


	reg5 &= 0x3;
	if (reg5 == 0 || reg5 == 2) {
		GETREG32(ISP_FIRST_FRAME_EXPOSURE, expo);
	} else if (reg5 == 1) {
		GETREG32(ISP_SECOND_FRAME_EXPOSURE, expo);
	} else if (reg5 == 3) {
		GETREG32(ISP_THIRD_FRAME_EXPOSURE, expo);
	}

	return expo;
}

/* Only useful for ISO auto mode, Revised by y00215412 for DTS2012090702901 */
int mini_ispv1_get_actual_iso(void)
{
	int iso = 0;
	mini_isp_meta_data *meta_data = mini_this_ispdata->meta_data;

	if (!meta_data)
		goto out;

	if (mini_isp_hw_data.cur_state == STATE_PREVIEW)
		iso = meta_data->data.ae_info.iso;
	else
		iso = mini_this_ispdata->capture_iso;

out:
	iso = (iso!=0) ? iso : 100;
	return iso;
}

int mini_ispv1_get_exposure_time(void)
{
	u32 expo = 0;
	int denominator_expo_time;
	mini_isp_meta_data *meta_data = mini_this_ispdata->meta_data;

	if (!meta_data)
		goto out;

	if (mini_isp_hw_data.cur_state == STATE_PREVIEW)
		expo = meta_data->data.ae_info.expo_time;
	else
		expo = mini_this_ispdata->capture_expo_time;

out:
	expo = (expo!=0) ? expo : 10000;
	denominator_expo_time = (1000000/expo);
	return denominator_expo_time;
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_get_awb_gain;
 * Description : call by mini_ispv1_preview_done_do_tune and ispv1_hw_set_default;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : added for DTS2012061807407 by j00212990 2012-06-16 using for display attribute of main camera;
 **************************************************************************
 */
u32 mini_ispv1_get_awb_gain(int withShift)
{
	u16 b_gain, r_gain;
	u32 return_val;
	if (withShift) {
		GETREG16(REG_ISP_AWB_GAIN_B, b_gain);
		GETREG16(REG_ISP_AWB_GAIN_R, r_gain);
	} else {
		GETREG16(REG_ISP_AWB_ORI_GAIN_B, b_gain);
		GETREG16(REG_ISP_AWB_ORI_GAIN_R, r_gain);
	}
	return_val = (b_gain << 16) | r_gain;
	return return_val;
}

u32 mini_ispv1_get_expo_line(void)
{
	u32 ret;

	ret = get_writeback_expo();
	return ret;
}

u32 mini_ispv1_get_sensor_vts(void)
{
	mini_camera_sensor *sensor;
	u32 frame_index;
	u32 full_fps, fps;
	u32 basic_vts;
	u32 vts;

	if (NULL == mini_this_ispdata) {
		print_info("mini_this_ispdata is NULL");
		return 0;
	}
	sensor = mini_this_ispdata->sensor;

	if (mini_isp_hw_data.cur_state == STATE_PREVIEW)
		frame_index = sensor->preview_frmsize_index;
	else
		frame_index = sensor->capture_frmsize_index;

	full_fps = sensor->frmsize_list[frame_index].fps;
	basic_vts = sensor->frmsize_list[frame_index].vts;
	fps = sensor->fps;
	vts = basic_vts * full_fps / fps;

	return vts;
}

/* added by y00231328 for DTS2012102604572 to set CCM gain when awb and ae changed  2012-11-1 start. */
u32 mini_ispv1_get_current_ccm_rgain(void)
{
	return GETREG8(REG_ISP_CCM_PREGAIN_R);
}

u32 mini_ispv1_get_current_ccm_bgain(void)
{
	return GETREG8(REG_ISP_CCM_PREGAIN_B);
}
/* added by y00231328 for DTS2012102604572 to set CCM gain when awb and ae changed  2012-11-1 end. */

/* Added for EV: exposure compensation.
 * Just set as this: ev+2 add 40%, ev+1 add 20% to current target Y
 * should config targetY and step
 */
static void ispv1_calc_ev(u8 *target_low, u8 *target_high, int ev)
{
	int target_y_low = DEFAULT_TARGET_Y_LOW;
	int target_y_high = DEFAULT_TARGET_Y_HIGH;
	mini_effect_params *effect = get_effect_ptr();
	mini_ae_target_s *this_target_y = effect->ae_param.target_y;

	switch (ev) {
	case -2:
		target_y_low = this_target_y[0].default_target_y_low;
		target_y_high= this_target_y[0].default_target_y_high;
		break;

	case -1:
		target_y_low = this_target_y[1].default_target_y_low;
		target_y_high = this_target_y[1].default_target_y_high;
		break;

	case 0:
		target_y_low = this_target_y[2].default_target_y_low;
		target_y_high = this_target_y[2].default_target_y_high;
		break;

	case 1:
		target_y_low = this_target_y[3].default_target_y_low;
		target_y_high = this_target_y[3].default_target_y_high;
		break;

	case 2:
		target_y_low = this_target_y[4].default_target_y_low;
		target_y_high = this_target_y[4].default_target_y_high;
		break;

	default:
		print_error("ev invalid");
		break;
	}

	*target_low = target_y_low;
	*target_high = target_y_high;
}

/* Added for EV: exposure compensation.
 * Just set as this: ev+2 add 40%, ev+1 add 20% to current target Y
 * should config targetY and step
 */
int mini_ispv1_set_ev(int ev)
{
	/* added by j00212990 */
#ifdef OVISP_DEBUG_MODE
	return 0;
#endif
	u8 target_y_low, target_y_high;
	int ret = 0;

	print_debug("enter %s", __func__);
	return camera_agent_set_EV(mini_this_ispdata->sensor->sensor_index,ev);//by wind
	/* ev is target exposure compensation value, decided by exposure time and sensor gain */
	ispv1_calc_ev(&target_y_low, &target_y_high, ev);

	if ((ev == 0) && (mini_this_ispdata->ev != 0)) {
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_low);
		msleep(100);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
	} else {
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
	}

	scene_target_y_low = target_y_low;
	scene_target_y_high = target_y_high;
	/* added for focus AE by s00061250 2013-09-22 */
	mini_save_target_high();

	return ret;
}

static void ispv1_init_sensor_config(mini_camera_sensor *sensor)
{
	/* Enable AECAGC */
	//SETREG8(REG_ISP_AECAGC_MANUAL_ENABLE, AUTO_AECAGC);//by wind 
	SETREG8(REG_ISP_AECAGC_MANUAL_ENABLE, MANUAL_AECAGC);//by wind 

	/* new firmware support isp write all sensors's AEC/AGC registers. */
	if ((0 == sensor->aec_addr[0] && 0 == sensor->aec_addr[1] && 0 == sensor->aec_addr[2]) ||
		(0 == sensor->agc_addr[0] && 0 == sensor->agc_addr[1])) {
		SETREG8(REG_ISP_AECAGC_WRITESENSOR_ENABLE, ISP_WRITESENSOR_DISABLE);
	} else {
		SETREG8(REG_ISP_AECAGC_WRITESENSOR_ENABLE, ISP_WRITESENSOR_ENABLE);
	}
	/* changed by y00231328. differ from sensor type and bayer order. */
	SETREG8(REG_ISP_TOP6, sensor->sensor_rgb_type);

	SETREG16(REG_ISP_AEC_ADDR0, sensor->aec_addr[0]);
	SETREG16(REG_ISP_AEC_ADDR1, sensor->aec_addr[1]);
	SETREG16(REG_ISP_AEC_ADDR2, sensor->aec_addr[2]);

	SETREG16(REG_ISP_AGC_ADDR0, sensor->agc_addr[0]);
	SETREG16(REG_ISP_AGC_ADDR1, sensor->agc_addr[1]);

	if (0 == sensor->aec_addr[0])
		SETREG8(REG_ISP_AEC_MASK_0, 0x00);
	else
		SETREG8(REG_ISP_AEC_MASK_0, 0xff);

	if (0 == sensor->aec_addr[1])
		SETREG8(REG_ISP_AEC_MASK_1, 0x00);
	else
		SETREG8(REG_ISP_AEC_MASK_1, 0xff);

	if (0 == sensor->aec_addr[2])
		SETREG8(REG_ISP_AEC_MASK_2, 0x00);
	else
		SETREG8(REG_ISP_AEC_MASK_2, 0xff);

	if (0 == sensor->agc_addr[0])
		SETREG8(REG_ISP_AGC_MASK_H, 0x00);
	else
		SETREG8(REG_ISP_AGC_MASK_H, 0xff);

	if (0 == sensor->agc_addr[1])
		SETREG8(REG_ISP_AGC_MASK_L, 0x00);
	else
		SETREG8(REG_ISP_AGC_MASK_L, 0xff);

	/* changed by y00231328. some sensor such as S5K4E1, expo and gain active function is like sony type */
	if (sensor->sensor_type == SENSOR_LIKE_OV)
		SETREG8(REG_ISP_AGC_SENSOR_TYPE, SENSOR_OV);
	else if (sensor->sensor_type == SENSOR_LIKE_SONY)
		SETREG8(REG_ISP_AGC_SENSOR_TYPE, SENSOR_SONY);
	else
		SETREG8(REG_ISP_AGC_SENSOR_TYPE, sensor->sensor_type);
    if (sensor->sensor_index == CAMERA_SENSOR_PRIMARY) {    // decide VSYNC, default 0x00 for main MIPI, 0x04 for sub MIPI
        SETREG8(0x63c2a, 0x00);
    } else {
        SETREG8(0x63c2a, 0x00);
		//SETREG8(0x63c2a, 0x04);
    }
    if (sensor->isp_idi_skip== true) {
        SETREG8(0x1c5a4,0x01);
    } else {
        SETREG8(0x1c5a4,0x00);
    }
}

/* Added for anti_banding. y36721 todo */
int mini_ispv1_set_anti_banding(camera_anti_banding banding)
{
	u32 op = 0;

	print_debug("enter %s", __func__);

	return camera_agent_set_ANTIFLICKER(mini_this_ispdata->sensor->sensor_index,banding);//by wind
        /* Modified  by w00199382 for isp 2.2 , 2013/1/8, begin */

	switch (banding) {
	case CAMERA_ANTI_BANDING_OFF:
		op = 0;
		SETREG8(REG_ISP_BANDFILTER_SHORT_EN, 0x1);
                SETREG8(REG_ISP_AUTO_BANDING_DETEC_EN, 0x0);
		break;

	case CAMERA_ANTI_BANDING_50Hz:
		op = 2;
		SETREG8(REG_ISP_BANDFILTER_SHORT_EN, 0x0);
                SETREG8(REG_ISP_AUTO_BANDING_DETEC_EN, 0x0);
		break;

	case CAMERA_ANTI_BANDING_60Hz:
		op = 1;
		SETREG8(REG_ISP_BANDFILTER_SHORT_EN, 0x0);
                SETREG8(REG_ISP_AUTO_BANDING_DETEC_EN, 0x0);
		break;

	case CAMERA_ANTI_BANDING_AUTO:
		/* y36721 todo */
                SETREG8(REG_ISP_AUTO_BANDING_DETEC_EN, 0x1);
		break;

	default:
		return -1;
	}

        /* Modified  by w00199382 for isp 2.2 , 2013/1/8, end */

	SETREG8(REG_ISP_BANDFILTER_EN, 0x1);
	SETREG8(REG_ISP_BANDFILTER_FLAG, op);

	return 0;
}

int mini_ispv1_get_anti_banding(void)
{
	u32 op = 0;
	camera_anti_banding banding;

	print_debug("enter %s", __func__);

	op = GETREG8(REG_ISP_BANDFILTER_FLAG);

	switch (op) {
	case 0:
		banding = CAMERA_ANTI_BANDING_OFF;
		break;

	case 1:
		banding = CAMERA_ANTI_BANDING_60Hz;
		break;

	case 2:
		banding = CAMERA_ANTI_BANDING_50Hz;
		break;
	default:
		return -1;
	}

	return banding;
}

#if 0
/* blue,green,red gains */
static u16 isp_mwb_gain[CAMERA_WHITEBALANCE_MAX][3] = {
	{0x0000, 0x0000, 0x0000}, /* AWB not care about it */
	{0x012c, 0x0080, 0x0089}, /* INCANDESCENT 2800K */
	{0x00f2, 0x0080, 0x00b9}, /* FLUORESCENT 4200K */
	{0x00a0, 0x00a0, 0x00a0}, /* WARM_FLUORESCENT, y36721 todo */
	{0x00d1, 0x0080, 0x00d2}, /* DAYLIGHT 5000K */
	{0x00b0, 0x0080, 0x00ec}, /* CLOUDY_DAYLIGHT 6500K*/
	{0x00a0, 0x00a0, 0x00a0}, /* TWILIGHT, y36721 todo */
	{0x0168, 0x0080, 0x0060}, /* CANDLELIGHT, 2300K */
};
#endif

#if 1
/* Added for awb */
int mini_ispv1_set_awb(camera_white_balance awb_mode)
{
	mini_effect_params *effect = get_effect_ptr();
	mini_awb_gain_t *mwb_gain = effect->mwb_gain;
	print_debug("enter %s", __func__);
	/* default is auto, ...... */

	return  camera_agent_set_awb(mini_this_ispdata->sensor->sensor_index,awb_mode);
#ifdef OVISP_DEBUG_MODE
	return 0;
#endif

	switch (awb_mode) {
	case CAMERA_WHITEBALANCE_AUTO:
		//SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x1);
		/*  Awb mode, should set Curve AWB , revise by c00220250 for DTS2013080706116 */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x2);
        /* revise end */
		break;

	case CAMERA_WHITEBALANCE_INCANDESCENT:
	case CAMERA_WHITEBALANCE_FLUORESCENT:
	case CAMERA_WHITEBALANCE_WARM_FLUORESCENT:
	case CAMERA_WHITEBALANCE_DAYLIGHT:
	case CAMERA_WHITEBALANCE_CLOUDY_DAYLIGHT:
	case CAMERA_WHITEBALANCE_TWILIGHT:
	case CAMERA_WHITEBALANCE_CANDLELIGHT:
		//SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x1);	/* y36721 fix it */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, awb_mode + 2);
		SETREG16(REG_ISP_AWB_MANUAL_GAIN_BLUE(awb_mode - 1),
			 mwb_gain[awb_mode].b_gain);
		SETREG16(REG_ISP_AWB_MANUAL_GAIN_GREEN(awb_mode - 1),
			 mwb_gain[awb_mode].gb_gain);
		SETREG16(REG_ISP_AWB_MANUAL_GAIN_RED(awb_mode - 1),
			 mwb_gain[awb_mode].r_gain);
		break;

	default:
		print_error("unknow awb mode\n");
		return -1;
		break;
	}

	return 0;
}
#else
int mini_ispv1_set_awb(camera_white_balance awb_mode)
{
	print_info("enter %s, awb mode", __func__, awb_mode);
	/* default is auto, ...... */

	switch (awb_mode) {
	case CAMERA_WHITEBALANCE_AUTO:
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		/*  Awb mode, should set CT-based AWB */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x3);
		break;

	case CAMERA_WHITEBALANCE_INCANDESCENT:
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		/*  Awb mode, should set CT-based AWB */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x4);
		break;
	case CAMERA_WHITEBALANCE_DAYLIGHT:
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		/*  Awb mode, should set CT-based AWB */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x5);
		break;

	case CAMERA_WHITEBALANCE_FLUORESCENT:
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		/*  Awb mode, should set CT-based AWB */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x6);
		break;

	case CAMERA_WHITEBALANCE_CLOUDY_DAYLIGHT:
		SETREG8(REG_ISP_AWB_MANUAL_ENABLE, 0x0);
		/*  Awb mode, should set CT-based AWB */
		SETREG8(REG_ISP_AWB_METHOD_TYPE, 0x7);
		break;

	default:
		print_error("unknow awb mode\n");
		return -1;
		break;
	}

	return 0;
}
#endif

/* Added for sharpness, y36721 todo */
int mini_ispv1_set_sharpness(camera_sharpness sharpness)
{
	print_debug("enter %s", __func__);

	return 0;
}

/* Added for saturation, y36721 todo */
int mini_ispv1_set_saturation(camera_saturation saturation)
{
	print_debug("enter %s, %d", __func__, saturation);
	mini_this_ispdata->saturation = saturation;

	return 0;
}

static int ispv1_set_saturation_done(camera_saturation saturation)
{
	print_debug("enter %s, %d", __func__, saturation);

	switch (saturation) {
	case CAMERA_SATURATION_L2:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x10);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x10);
		/* modify by zkf78283 end */
		break;

	case CAMERA_SATURATION_L1:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x28);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x28);
		/* modify by zkf78283 end */
		break;

	case CAMERA_SATURATION_H0:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x40);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x40);
		/* modify by zkf78283 end */
		break;

	case CAMERA_SATURATION_H1:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x58);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x58);
		/* modify by zkf78283 end */
		break;

	case CAMERA_SATURATION_H2:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x70);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x70);
		/* modify by zkf78283 end */
		break;

	default:
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) & (~ISP_SDE_ENABLE));
		break;
	}

	return 0;
}

int mini_ispv1_set_contrast(camera_contrast contrast)
{
	print_debug("enter %s, %d", __func__, contrast);
	mini_this_ispdata->contrast = contrast;

	return 0;
}

/* y00215412 DTS2012081007721 2012-08-10 revise begin */
static int ispv1_set_contrast_done(camera_contrast contrast)
{
	print_debug("enter %s, %d", __func__, contrast);

	SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
    /* modify by zkf78283 begin */
	//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_CONTRAST_ENABLE);
    /* modify by zkf78283 end */

	mini_ispv1_switch_contrast(STATE_PREVIEW, contrast);

    return 0;
}

int mini_ispv1_switch_contrast(camera_state state, camera_contrast contrast)
{
	if (state == STATE_PREVIEW) {
		switch (contrast) {
		case CAMERA_CONTRAST_L2:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_PREVIEW_L2);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_L1:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_PREVIEW_L1);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H0:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_PREVIEW_H0);
			SETREG8(REG_ISP_TOP5, (GETREG8(REG_ISP_TOP5) | SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H1:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_PREVIEW_H1);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H2:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_PREVIEW_H2);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		default:
			print_error("%s, not supported contrast %d", __func__, contrast);
			break;
		}
	} else if (state == STATE_CAPTURE) {
		switch (contrast) {
		case CAMERA_CONTRAST_L2:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_CAPTURE_L2);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_L1:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_CAPTURE_L1);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H0:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_CAPTURE_H0);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H1:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_CAPTURE_H1);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		case CAMERA_CONTRAST_H2:
			SETREG8(REG_ISP_SDE_CONTRAST, SDE_CONTRAST_CAPTURE_H2);
			SETREG8(REG_ISP_TOP5, GETREG8(REG_ISP_TOP5) & (~SDE_MANUAL_OFFSET_ENABLE));
			break;

		default:
			print_error("%s, not supported contrast %d", __func__, contrast);
			break;
		}
	}

	return 0;
}
/* y00215412 DTS2012081007721 2012-08-10 revise end */

void mini_ispv1_set_fps_lock(int lock)
{
	print_debug("enter %s", __func__);
	fps_lock = lock;
}

static void ispv1_change_fps(camera_frame_rate_mode mode)
{
	print_debug("enter :%s, mode :%d", __func__, mode);
	if (fps_lock == true) {
		print_error("fps is locked");
		return;
	}
	if (mode >= CAMERA_FRAME_RATE_MAX)
		print_error("inviable camera_frame_rate_mode: %d", mode);

	if (CAMERA_FRAME_RATE_FIX_MAX == mode) {
		mini_this_ispdata->sensor->fps_min = mini_isp_hw_data.fps_max;
		mini_this_ispdata->sensor->fps_max = mini_isp_hw_data.fps_max;
		ispv1_set_frame_rate(CAMERA_FRAME_RATE_FIX_MAX, mini_this_ispdata->sensor);
	} else if (CAMERA_FRAME_RATE_FIX_MIN == mode) {
		mini_this_ispdata->sensor->fps_min = mini_isp_hw_data.fps_min;
		mini_this_ispdata->sensor->fps_max = mini_isp_hw_data.fps_min;
		ispv1_set_frame_rate(CAMERA_FRAME_RATE_FIX_MIN, mini_this_ispdata->sensor);
	} else if (CAMERA_FRAME_RATE_AUTO == mode) {
		mini_this_ispdata->sensor->fps_min = mini_isp_hw_data.fps_min;
		mini_this_ispdata->sensor->fps_max = mini_isp_hw_data.fps_max;
	}

	mini_this_ispdata->fps_mode = mode;
}

static void ispv1_change_max_exposure(mini_camera_sensor *sensor, camera_max_exposrure mode)
{
	u8 frame_index, fps;
	u32 vts;
	u32 max_exposure;

	frame_index = sensor->preview_frmsize_index;
	fps = sensor->frmsize_list[frame_index].fps;
	vts = sensor->frmsize_list[frame_index].vts;

	if (CAMERA_MAX_EXPOSURE_LIMIT == mode)
		max_exposure = (fps * vts / 100 - 14);
	else
		max_exposure = ((fps * vts / sensor->fps) - 14);

	SETREG32(REG_ISP_MAX_EXPOSURE, max_exposure);
	SETREG32(REG_ISP_MAX_EXPOSURE_SHORT, max_exposure);
}

/* last modified for DTS2012091900181 */
int mini_ispv1_set_scene(camera_scene scene)
{
	u8 target_y_low = scene_target_y_low;
	u8 target_y_high = scene_target_y_high;
	mini_effect_params *effect = get_effect_ptr();
	u8 *uv_saturation = effect->uv_saturation;

	/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
	u8 uv_adjust_ctrl = GETREG8(REG_ISP_UV_ADJUST);
	/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
#ifdef OVISP_DEBUG_MODE
	return 0;
#endif

	print_debug("enter %s, scene:%d", __func__, scene);
	camera_agent_set_scene_mode(mini_this_ispdata->sensor->sensor_index,scene);//by wind
	mini_this_ispdata->scene = scene;//by wind
	return 0;//by wind

	switch (scene) {
	case CAMERA_SCENE_AUTO:
    /*<DTS2013072900413 k00138829 20130729 for scene detection kernel modification begin */
	case CAMERA_SCENE_HWAUTO:
    /* DTS2013072900413 k00138829 20130729 for scene detection kernel modification end >*/
		print_info("case CAMERA_SCENE_AUTO ");
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = mini_isp_hw_data.flash_mode;
		/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_AUTO]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(mini_this_ispdata->awb_mode);
		break;
	/*< DTS2013072406684 zhoupengtao 00124583 2013-07-24 add for export driver interface begin */
	case CAMERA_SCENE_HDR:
		print_info("CAMERA_SCENE_HDR ");
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
		/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_ACTION]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;
	/* DTS2013072406684 zhoupengtao 00124583 2013-07-24 add for export driver interface end >*/
	case CAMERA_SCENE_ACTION:
		print_info("case CAMERA_SCENE_ACTION ");
		/*
		 * Reduce max exposure time 1/100s
		 * Pre-focus recommended. (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MAX);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_LIMIT);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
		/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_ACTION]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_PORTRAIT:
		print_info("case CAMERA_SCENE_PORTRAIT ");
		/*
		 * Pre-tuned color matrix for better skin tone recommended. (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = mini_isp_hw_data.flash_mode;
		/* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_PORTRAIT]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_LANDSPACE:
		print_info("case CAMERA_SCENE_LANDSPACE ");
		/*
		 * Focus mode set to infinity
		 * Manual AWB to daylight (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
             //SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_LANDSPACE]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_INFINITY); /* y00215412 revise 2012-08-10 */
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_NIGHT:
		print_info("case CAMERA_SCENE_NIGHT ");
		/*
		 * Increase max exposure time��֡�ʽ��͵�5fps����
		 * Turn off the flash
		 * Turn off the UV adjust
		 * Focus mode set to infinity
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MIN);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_DISABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl & (~ISP_UV_SATURATE_ADJUST_ENABLE));
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_NIGHT]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE); /* y00215412 revise 2012-08-10 */
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_NIGHT_PORTRAIT:
		print_info("case CAMERA_SCENE_NIGHT_PORTRAIT ");
		/*
		 * Increase max exposure time
		 * Turn off UV adjust
		 * Turn on the flash (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MIN);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_DISABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl & (~ISP_UV_SATURATE_ADJUST_ENABLE));
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_NIGHT_PORTRAIT]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_THEATRE:
		print_info("case CAMERA_SCENE_THEATRE ");
		/*
		 * Increase max exposure time��֡�ʽ��͵�5fps����
		 * Turn off the flash
		 * Turn off UV adjust
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MIN);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_DISABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl & (~ISP_UV_SATURATE_ADJUST_ENABLE));
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_THEATRE]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_INFINITY);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_BEACH:
		print_info("case CAMERA_SCENE_BEACH ");
		/*
		 * Reduce AE target
		 * Manual AWB set to daylight (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		ispv1_calc_ev(&target_y_low, &target_y_high, -2);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_BEACH]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_SNOW:
		print_info("case CAMERA_SCENE_SNOW ");
		/*
		 * Increase AE target
		 * Enable EDR mode
		 * Manual AWB set to daylight (optional)
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		ispv1_calc_ev(&target_y_low, &target_y_high, 2);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
             //SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_SNOW]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_FIREWORKS:
		print_info("case CAMERA_SCENE_FIREWORKS ");
		/*
		 * Increase max exposure time��֡�ʽ��͵�5fps����
		 * Focus mode set to infinity
		 * Turn off the flash
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MIN);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_FIREWORKS]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_INFINITY); /* y00215412 revise 2012-08-10 */
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_CANDLELIGHT:
		print_info("case CAMERA_SCENE_CANDLELIGHT ");
		/*
		 * Increase max exposure time��֡�ʽ��͵�5fps����
		 * Turn off the flash
		 * Turn off UV adjust
		 * AWB set to manual with fixed AWB gain with yellow/warm tone
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_FIX_MIN);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_DISABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl & (~ISP_UV_SATURATE_ADJUST_ENABLE));
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_CANDLELIGHT]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_CONTINUOUS_PICTURE);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_CANDLELIGHT);
		break;

	case CAMERA_SCENE_FLOWERS:
		print_info("case CAMERA_SCENE_FLOWER ");
		/*
		 * Turn off the flash
		 * set focus mode to Macro,
		 * revised by y00215412 2012-06-27 for ev bug from scene snow/beach to flowers
		 */
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_FLOWERS]);
		mini_this_ispdata->flash_mode = CAMERA_FLASH_OFF;
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_MACRO);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		break;

	case CAMERA_SCENE_SUNSET:
	case CAMERA_SCENE_STEADYPHOTO:
	case CAMERA_SCENE_SPORTS:
	case CAMERA_SCENE_BARCODE:
	default:
		print_info("This scene not supported yet. ");
		ispv1_change_fps(CAMERA_FRAME_RATE_AUTO);
		ispv1_change_max_exposure(mini_this_ispdata->sensor, CAMERA_MAX_EXPOSURE_RESUME);
		SETREG8(REG_ISP_TARGET_Y_LOW, target_y_low);
		SETREG8(REG_ISP_TARGET_Y_HIGH, target_y_high);
		mini_this_ispdata->flash_mode = mini_isp_hw_data.flash_mode;
		mini_this_ispdata->scene = CAMERA_SCENE_AUTO;
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 begin */
    		//SETREG8(REG_ISP_UV_ADJUST, UV_ADJUST_ENABLE);
		SETREG8(REG_ISP_UV_ADJUST, uv_adjust_ctrl | ISP_UV_SATURATE_ADJUST_ENABLE);
             /* add by c00220250 for DTSDTS2013112503400, 2013-11-29 end */
		SETREG8(REG_ISP_UV_SATURATION, uv_saturation[CAMERA_SCENE_SUNSET]);
		mini_ispv1_set_focus_mode(CAMERA_FOCUS_AUTO);
		mini_ispv1_set_awb(CAMERA_WHITEBALANCE_AUTO);
		goto out;
	}

	mini_this_ispdata->scene = scene;

out:
	return 0;
}

int mini_ispv1_set_brightness(camera_brightness brightness)
{
	print_debug("enter %s, %d", __func__, brightness);
	mini_this_ispdata->brightness = brightness;

	return 0;
}

/* y00215412 DTS2012081007721 2012-08-10 revise begin */
static int ispv1_set_brightness_done(camera_brightness brightness)
{
	print_debug("enter %s, %d", __func__, brightness);

	switch (brightness) {
	case CAMERA_BRIGHTNESS_L2:
		/* modify by zkf78283 beign */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_BRIGHTNESS_ENABLE);
		SETREG8(REG_ISP_SDE_SIGN_SET, GETREG8(REG_ISP_SDE_SIGN_SET) | ISP_BRIGHTNESS_SIGN_NEGATIVE);
		/* modify by zkf78283 beign */
		break;

	case CAMERA_BRIGHTNESS_L1:
		/* modify by zkf78283 beign */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_BRIGHTNESS_ENABLE);
		SETREG8(REG_ISP_SDE_SIGN_SET, GETREG8(REG_ISP_SDE_SIGN_SET) | ISP_BRIGHTNESS_SIGN_NEGATIVE);
		/* modify by zkf78283 end */
		break;

	case CAMERA_BRIGHTNESS_H0:
		/* modify by zkf78283 beign */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_BRIGHTNESS_ENABLE);
		SETREG8(REG_ISP_SDE_SIGN_SET, GETREG8(REG_ISP_SDE_SIGN_SET) & (~ISP_BRIGHTNESS_SIGN_NEGATIVE));
		/* modify by zkf78283 end */
		break;

	case CAMERA_BRIGHTNESS_H1:
		/* modify by zkf78283 beign */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_BRIGHTNESS_ENABLE);
		SETREG8(REG_ISP_SDE_SIGN_SET, GETREG8(REG_ISP_SDE_SIGN_SET) & (~ISP_BRIGHTNESS_SIGN_NEGATIVE));
		/* modify by zkf78283 end */
		break;

	case CAMERA_BRIGHTNESS_H2:
		/* modify by zkf78283 beign */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, GETREG8(REG_ISP_SDE_CTRL) | ISP_BRIGHTNESS_ENABLE);
		SETREG8(REG_ISP_SDE_SIGN_SET, GETREG8(REG_ISP_SDE_SIGN_SET) & (~ISP_BRIGHTNESS_SIGN_NEGATIVE));
		/* modify by zkf78283 end */
		break;

	default:
		print_error("%s, not supported brightness %d", __func__, brightness);
		break;
	}

	mini_ispv1_switch_brightness(STATE_PREVIEW, brightness);

	return 0;
}

int mini_ispv1_switch_brightness(camera_state state, camera_brightness brightness)
{
	if (state == STATE_PREVIEW) {
		switch (brightness) {
		case CAMERA_BRIGHTNESS_L2:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_PREVIEW_L2);
			break;

		case CAMERA_BRIGHTNESS_L1:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_PREVIEW_L1);
			break;

		case CAMERA_BRIGHTNESS_H0:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_PREVIEW_H0);
			break;

		case CAMERA_BRIGHTNESS_H1:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_PREVIEW_H1);
			break;

		case CAMERA_BRIGHTNESS_H2:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_PREVIEW_H2);
			break;

		default:
			break;
		}
	} else if (state == STATE_CAPTURE) {
		switch (brightness) {
		case CAMERA_BRIGHTNESS_L2:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_CAPTURE_L2);
			break;

		case CAMERA_BRIGHTNESS_L1:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_CAPTURE_L1);
			break;

		case CAMERA_BRIGHTNESS_H0:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_CAPTURE_H0);
			break;

		case CAMERA_BRIGHTNESS_H1:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_CAPTURE_H1);
			break;

		case CAMERA_BRIGHTNESS_H2:
			SETREG8(REG_ISP_SDE_BRIGHTNESS, SDE_BRIGHTNESS_CAPTURE_H2);
			break;

		default:
			break;
		}
	}

	return 0;
}
/* y00215412 DTS2012081007721 2012-08-10 revise end */

/* Add effect, h00206029 20120204 */
int mini_ispv1_set_effect(camera_effects effect)
{
	print_debug("enter %s, %d", __func__, effect);
	mini_this_ispdata->effect = effect;

	return 0;
}

static int ispv1_set_effect_done(camera_effects effect)
{
	print_debug("enter %s, %d", __func__, effect);

	switch (effect) {
	case CAMERA_EFFECT_NONE:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, ISP_CONTRAST_ENABLE | ISP_BRIGHTNESS_ENABLE | ISP_SATURATION_ENABLE);
		SETREG8(REG_ISP_SDE_CTRL0C_CONTRAST, GETREG8(REG_ISP_SDE_CTRL0C_CONTRAST) & ~0x3);
		SETREG8(REG_ISP_SDE_CONTRAST, 0x80);
		SETREG8(REG_ISP_SDE_BRIGHTNESS, 0x0);
		SETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00, GETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00) & ~0x3);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x40);
		SETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01, GETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL11_UVMAXTRIX01, 0x0);
		SETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10, GETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL13_UVMAXTRIX10, 0x0);
		SETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11, GETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11) & ~0x3);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x40);
		SETREG8(REG_ISP_SDE_U_REG, 0x0);
		SETREG8(REG_ISP_SDE_V_REG, 0x0);
		SETREG8(REG_ISP_SDE_SIGN_SET, 0x0);
		SETREG8(REG_ISP_SDE_CTRL1A_HTHRE, GETREG8(REG_ISP_SDE_CTRL1A_HTHRE) & ~0x3f);
		SETREG8(REG_ISP_SDE_CTRL1B_HGAIN, 0x4);
		/* modify by zkf78283 end */
		break;

	case CAMERA_EFFECT_MONO:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, ISP_MONO_EFFECT_ENABLE);
		SETREG8(REG_ISP_SDE_CTRL0C_CONTRAST, GETREG8(REG_ISP_SDE_CTRL0C_CONTRAST) & ~0x3);
		SETREG8(REG_ISP_SDE_CONTRAST, 0x80);
		SETREG8(REG_ISP_SDE_BRIGHTNESS, 0x0);
		SETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00, GETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00) & ~0x3);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x0);
		SETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01, GETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL11_UVMAXTRIX01, 0x0);
		SETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10, GETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL13_UVMAXTRIX10, 0x0);
		SETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11, GETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11) & ~0x3);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x0);
		SETREG8(REG_ISP_SDE_U_REG, 0x0);
		SETREG8(REG_ISP_SDE_V_REG, 0x0);
		SETREG8(REG_ISP_SDE_SIGN_SET, 0x0);
		SETREG8(REG_ISP_SDE_CTRL1A_HTHRE, GETREG8(REG_ISP_SDE_CTRL1A_HTHRE) & ~0x3f);
		SETREG8(REG_ISP_SDE_CTRL1B_HGAIN, 0x4);
		/* modify by zkf78283 end */
		break;

	case CAMERA_EFFECT_NEGATIVE:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, ISP_NEGATIVE_EFFECT_ENABLE);
		SETREG8(REG_ISP_SDE_CTRL0C_CONTRAST, GETREG8(REG_ISP_SDE_CTRL0C_CONTRAST) | 0x2);
		SETREG8(REG_ISP_SDE_CONTRAST, 0x80);
		SETREG8(REG_ISP_SDE_BRIGHTNESS, 0xff);
		SETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00, GETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00) | 0x2);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x40);
		SETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01, GETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL11_UVMAXTRIX01, 0x0);
		SETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10, GETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL13_UVMAXTRIX10, 0x0);
		SETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11, GETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11) | 0x2);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x40);
		SETREG8(REG_ISP_SDE_U_REG, 0x0);
		SETREG8(REG_ISP_SDE_V_REG, 0x0);
		SETREG8(REG_ISP_SDE_SIGN_SET, 0x0);
		SETREG8(REG_ISP_SDE_CTRL1A_HTHRE, GETREG8(REG_ISP_SDE_CTRL1A_HTHRE) & ~0x3f);
		SETREG8(REG_ISP_SDE_CTRL1B_HGAIN, 0x84);
		/* modify by zkf78283 end */
		break;

	case CAMERA_EFFECT_SEPIA:
		/* modify by zkf78283 begin */
		SETREG8(REG_ISP_TOP2, GETREG8(REG_ISP_TOP2) | ISP_SDE_ENABLE);
		//SETREG8(REG_ISP_SDE_CTRL, ISP_FIX_U_ENABLE | ISP_FIX_V_ENABLE);
		//SETREG8(REG_ISP_SDE_U_REG, 0x30);
		//SETREG8(REG_ISP_SDE_V_REG, 0xb0);
		SETREG8(REG_ISP_SDE_CTRL0C_CONTRAST, GETREG8(REG_ISP_SDE_CTRL0C_CONTRAST) & ~0x3);
		SETREG8(REG_ISP_SDE_CONTRAST, 0x80);
		SETREG8(REG_ISP_SDE_BRIGHTNESS, 0x0);
		SETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00, GETREG8(REG_ISP_SDE_CTRL0E_UVMAXTRIX00) & ~0x3);
		SETREG8(REG_ISP_SDE_U_SATURATION, 0x0);
		SETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01, GETREG8(REG_ISP_SDE_CTRL10_UVMAXTRIX01) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL11_UVMAXTRIX01, 0x0);
		SETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10, GETREG8(REG_ISP_SDE_CTRL12_UVMAXTRIX10) & ~0x3);
		SETREG8(REG_ISP_SDE_CTRL13_UVMAXTRIX10, 0x0);
		SETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11, GETREG8(REG_ISP_SDE_CTRL14_UVMAXTRIX11) & ~0x3);
		SETREG8(REG_ISP_SDE_V_SATURATION, 0x0);
		SETREG8(REG_ISP_SDE_U_REG, 0x8f);
		SETREG8(REG_ISP_SDE_V_REG, 0x0e);
		SETREG8(REG_ISP_SDE_SIGN_SET, 0x0);
		SETREG8(REG_ISP_SDE_CTRL1A_HTHRE, GETREG8(REG_ISP_SDE_CTRL1A_HTHRE) & ~0x3f);
		SETREG8(REG_ISP_SDE_CTRL1B_HGAIN, 0x4);
		/* modify by zkf78283 end */
		break;

        /* Modified  by w00199382 for DTS2012110805572, 2012/12/24, begin */
	default:
                /*if the effect not support we will do nothing same as k3v2*/
                break;
        /* Modified  by w00199382 for DTS2012110805572, 2012/12/24, end */
	}

	return 0;
}

static int ispv1_set_effect_saturation_done(camera_effects effect, camera_saturation saturation, camera_contrast contrast, camera_brightness brightness)
{
        /* Modified  by w00199382 for isp 2.2 , 2012/11/2, begin */
	if(effect != CAMERA_EFFECT_NONE) {
		ispv1_set_effect_done(effect);
	} else {
		ispv1_set_effect_done(effect);
		ispv1_set_brightness_done(brightness);
		ispv1_set_contrast_done(contrast);
		ispv1_set_saturation_done(saturation);
	}
	return 0;
        /* Modified  by w00199382 for isp 2.2 , 2012/11/2, end */

}

/*
 * before start_preview or start_capture, it should be called to update size information
 * please see ISP manual or software manual.
 */
int mini_ispv1_update_LENC_scale(u32 inwidth, u32 inheight)
{
	u32 scale;

	print_debug("enter %s", __func__);

	scale = (0x100000 * 3) / inwidth;
	SETREG16(REG_ISP_LENC_BRHSCALE, scale);

	scale = (0x100000 * 3) / inheight;
	SETREG16(REG_ISP_LENC_BRVSCALE, scale);

	scale = (0x100000 * 4) / inwidth;
	SETREG16(REG_ISP_LENC_GHSCALE, scale);

	scale = (0x80000 * 4) / inheight;
	SETREG16(REG_ISP_LENC_GVSCALE, scale);

	return 0;

}

//#define AP_WRITE_AE_TIME_PRINT
/*
 * For not OVT sensors,  MCU won't write exposure and gain back to sensor directly.
 * In this case, exposure and gain need Host to handle.
 */
 /* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
#ifdef SUPPORT_ZSL_FLASH
extern u32 skip_ap_write_ae_flag;
#endif
void mini_ispv1_cmd_id_do_ecgc(struct work_struct *work)
{
	u32 expo, gain;
	mini_camera_sensor *sensor = mini_this_ispdata->sensor;
	u8 wait_flag;
#ifdef AP_WRITE_AE_TIME_PRINT
	struct timeval tv_start, tv_end;
	do_gettimeofday(&tv_start);
#endif

	print_debug("enter %s, cmd id 0x%x", __func__, mini_isp_hw_data.aec_cmd_id);

	/* added by y00215412 for hdr movie mode */
	if (mini_ispv1_is_hdr_movie_mode() == true)
	{
		wait_flag =  GETREG8(REG_ISP_I2C_WAIT_SIGNAL);

		if (wait_flag&ISP_FIRMWARE_I2C_WAIT){
			wait_flag&= ~ISP_FIRMWARE_I2C_WAIT;
			SETREG8(REG_ISP_I2C_WAIT_SIGNAL, wait_flag);
		}
		else
		{
			print_error("firmware do no set  0x1c5a2 = 0x%x  ", GETREG8(REG_ISP_I2C_WAIT_SIGNAL));
		}
		return;
	}
	expo = get_writeback_expo();
	gain = get_writeback_gain();
#ifdef SUPPORT_ZSL_FLASH
	if(skip_ap_write_ae_flag==1)
	{
		/*firmware will up the signal and wait for drv down the flag then set sensor use i2c*/
		wait_flag =  GETREG8(REG_ISP_I2C_WAIT_SIGNAL);

		if (wait_flag&ISP_FIRMWARE_I2C_WAIT){
			wait_flag&= ~ISP_FIRMWARE_I2C_WAIT;
			SETREG8(REG_ISP_I2C_WAIT_SIGNAL, wait_flag);
		}
		else
		{
			print_error("firmware do no set  0x1c5a2 = 0x%x  ", GETREG8(REG_ISP_I2C_WAIT_SIGNAL));
		}
		/* Modified  by w00199382 for isp 2.2 , 2013/7/27, begin*/

		return;
	}
#endif
	if (CMD_WRITEBACK_EXPO_GAIN == mini_isp_hw_data.aec_cmd_id ||
		CMD_WRITEBACK_EXPO == mini_isp_hw_data.aec_cmd_id ||
		CMD_WRITEBACK_GAIN == mini_isp_hw_data.aec_cmd_id) {
			/* changed by y00231328.some sensor such as S5K4E1, expo and gain should be set together in holdon mode */
			if (sensor->set_exposure_gain) {
				sensor->set_exposure_gain(expo, gain);
			} else {
				if (sensor->set_exposure)
					sensor->set_exposure(expo);
				if (sensor->set_gain)
					sensor->set_gain(gain);
			}

			/* Modified  by w00199382 for isp 2.2 , 2013/7/27, begin*/

			/*firmware will up the signal and wait for drv down the flag then set sensor use i2c*/
			wait_flag =  GETREG8(REG_ISP_I2C_WAIT_SIGNAL);

			if (wait_flag&ISP_FIRMWARE_I2C_WAIT){
				wait_flag&= ~ISP_FIRMWARE_I2C_WAIT;
				SETREG8(REG_ISP_I2C_WAIT_SIGNAL, wait_flag);
			}
			else
			{
				print_error("firmware do no set  0x1c5a2 = 0x%x  ", GETREG8(REG_ISP_I2C_WAIT_SIGNAL));
			}
			/* Modified  by w00199382 for isp 2.2 , 2013/7/27, begin*/


	} else {
		print_error("%s:unknow cmd id", __func__);
	}

#ifdef AP_WRITE_AE_TIME_PRINT
	do_gettimeofday(&tv_end);
	print_info("expo 0x%x, gain 0x%x, aec_cmd_id 0x%x, used %dus",
		expo, gain, mini_isp_hw_data.aec_cmd_id,
		(int)((tv_end.tv_sec - tv_start.tv_sec)*1000000 + (tv_end.tv_usec - tv_start.tv_usec)));
#endif
}
/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
/* changed by y00215412 for DTS2012082002461 2012-11-05. */
static int frame_rate_level;
static camera_frame_rate_state fps_state = CAMERA_FPS_STATE_HIGH;

int mini_ispv1_get_frame_rate_level()
{
	return frame_rate_level;
}

void mini_ispv1_set_frame_rate_level(int level)
{
	frame_rate_level = level;
	print_info("%s: level %d", __func__, level);
}

camera_frame_rate_state mini_ispv1_get_frame_rate_state(void)
{
	print_debug("%s state %d", __func__, fps_state);
	return fps_state;
}

void mini_ispv1_set_frame_rate_state(camera_frame_rate_state state)
{
	print_debug("%s: state %d", __func__, state);
	fps_state = state;
}

static int ispv1_set_frame_rate(camera_frame_rate_mode mode, mini_camera_sensor *sensor)
{
	int frame_rate_level = 0;
	u16 vts, fullfps, fps;
	u32 max_fps, min_fps;

	fullfps = sensor->frmsize_list[sensor->preview_frmsize_index].fps;
	if (CAMERA_FRAME_RATE_FIX_MAX == mode) {
		max_fps = (mini_isp_hw_data.fps_max > fullfps) ? fullfps : mini_isp_hw_data.fps_max;
		min_fps = max_fps;
		frame_rate_level = 0;
	} else if (CAMERA_FRAME_RATE_FIX_MIN == mode) {
		min_fps = (mini_isp_hw_data.fps_min < fullfps) ? mini_isp_hw_data.fps_min : fullfps;
		max_fps = (mini_isp_hw_data.fps_max > fullfps) ? fullfps : mini_isp_hw_data.fps_max;
		frame_rate_level = max_fps - min_fps;
	}

	fps = fullfps - frame_rate_level;

	sensor->fps = fps;

	/* rules: vts1*fps1 = vts2*fps2 */
	vts = sensor->frmsize_list[sensor->preview_frmsize_index].vts;
	vts = vts * fullfps / fps;

	if (sensor->set_vts) {
		sensor->set_vts(vts);
	} else {
		print_error("set_vts null");
		goto error;
	}

	SETREG32(REG_ISP_MAX_EXPOSURE, (vts - 14));
	mini_ispv1_set_frame_rate_level(frame_rate_level);
	return 0;
error:
	return -1;
}

static bool ispv1_change_frame_rate(
	camera_frame_rate_state *state, camera_frame_rate_dir direction, mini_camera_sensor *sensor)
{
	int frame_rate_level = mini_ispv1_get_frame_rate_level();
	u16 vts, fullfps, fps;
	bool level_changed = false;
	u32 max_fps, min_fps, mid_fps;
	u32 shift_level, max_level;

	fullfps = sensor->frmsize_list[sensor->preview_frmsize_index].fps;
	max_fps = (mini_isp_hw_data.fps_max > fullfps) ? fullfps : mini_isp_hw_data.fps_max;
	mid_fps = (mini_isp_hw_data.fps_mid > fullfps) ? fullfps : mini_isp_hw_data.fps_mid;
	min_fps = (mini_isp_hw_data.fps_min < fullfps) ? mini_isp_hw_data.fps_min : fullfps;

	if (mid_fps > max_fps)
		mid_fps = max_fps;

	if (min_fps > max_fps)
		min_fps = max_fps;

	max_level = max_fps - min_fps;

	print_debug("enter %s: state %d, frame_rate_level %d", __func__, *state, frame_rate_level);

	/* add by y00215412 2012-11-05, state CAMERA_EXPO_PRE_REDUCEx is highest priority. */
	if (*state == CAMERA_EXPO_PRE_REDUCE1) {
		/* desired level should go to FPS_HIGH level */
		level_changed = true;
		shift_level = max_fps - mid_fps;
		frame_rate_level -= shift_level;
		*state = CAMERA_FPS_STATE_HIGH;
		goto framerate_set_done;
	} else if (*state == CAMERA_EXPO_PRE_REDUCE2) {
		/* desired level should go to FPS_MID level */
		level_changed = true;
		shift_level = mid_fps - min_fps;
		frame_rate_level -= shift_level;
		*state = CAMERA_FPS_STATE_MIDDLE;
		goto framerate_set_done;
	}

	if (CAMERA_FRAME_RATE_DOWN == direction) {
		if (frame_rate_level >= max_level) {
			print_debug("Has arrival max frame_rate level");
			return true;
		} else if (frame_rate_level == 0) {
			shift_level = max_fps - mid_fps;
			frame_rate_level += shift_level;
			*state = CAMERA_FPS_STATE_MIDDLE;
			level_changed = true;
		} else {
			shift_level = mid_fps - min_fps;
			frame_rate_level += shift_level;
			*state = CAMERA_FPS_STATE_LOW;
			level_changed = true;
		}
	} else if (CAMERA_FRAME_RATE_UP == direction) {
		if (0 == frame_rate_level) {
			print_debug("Has arrival min frame_rate level");
			return true;
		} else if (frame_rate_level == max_fps - mid_fps) {
			shift_level = max_fps - mid_fps;
			frame_rate_level -= shift_level;
			*state = CAMERA_FPS_STATE_HIGH;
			level_changed = true;
		} else {
			shift_level = mid_fps - min_fps;
			frame_rate_level -= shift_level;
			*state = CAMERA_FPS_STATE_MIDDLE;
			level_changed = true;
		}
	}

framerate_set_done:
	fps = fullfps - frame_rate_level;
	if ((fps > sensor->fps_max) || (fps < sensor->fps_min)) {
		print_debug("auto fps:%d", fps);
		return true;
	}

	if (true == level_changed) {
		if ((sensor->fps_min <= fps) && (sensor->fps_max >= fps)) {
			sensor->fps = fps;
		} else if (sensor->fps_min > fps) {
			frame_rate_level = 0;
			sensor->fps = fullfps;
			fps = fullfps;
		} else {
			print_error("can't do auto fps, level:%d, cur_fps:%d, tar_fps:%d, ori_fps:%d, max_fps:%d, min_fps:%d",
				frame_rate_level, sensor->fps, fps, fullfps, sensor->fps_max, sensor->fps_min);
			goto error_out;
		}

		/* rules: vts1*fps1 = vts2*fps2 */
		vts = sensor->frmsize_list[sensor->preview_frmsize_index].vts;
		vts = vts * fullfps / fps;

		/* y36721 2012-04-23 add begin to avoid preview flicker when frame rate up */
		if ((vts - 14) < (get_writeback_expo() / 0x10)) {
			SETREG32(REG_ISP_MAX_EXPOSURE, (vts - 14));
			print_warn("current expo too large");
			if (*state == CAMERA_FPS_STATE_HIGH)
				*state = CAMERA_EXPO_PRE_REDUCE1;
			else
				*state = CAMERA_EXPO_PRE_REDUCE2;
			return true;
		}
		/* y36721 2012-04-23 add end */

		if (sensor->set_vts) {
			sensor->set_vts(vts);
		} else {
			print_error("set_vts null");
			goto error_out;
		}
		SETREG32(REG_ISP_MAX_EXPOSURE, (vts - 14));
		mini_ispv1_set_frame_rate_level(frame_rate_level);
	}
	return true;

error_out:
	return false;
}

/* #define PLATFORM_TYPE_PAD_S10 */
#define BOARD_ID_CS_U9510		0x67
#define BOARD_ID_CS_U9510E		0x66
#define BOARD_ID_CS_T9510E		0x06

/*
 * change by y00215412 for flash capture 2012-06-27
 * revised by y00215412 for WB otp DTS2012071307611 2012-07-13
 * flash_awb_gain is marked by Golden Module.
 */
mini_awb_gain_t mini_flash_platform_awb[FLASH_PLATFORM_MAX] =
{
	{0xc8, 0x80, 0x80, 0x104}, /* U9510 */
	{0xd6, 0x80, 0x80, 0x104}, /* U9510E/T9510E, recording AWB is 0xd0,0xfc, change a little */
	{0xd0, 0x80, 0x80, 0x100}, /* s10 */
};

/* changed by y00215412 DTS2012122102699 for U9508 two type flash LEDs 2012-12-21. */
static void ispv1_cal_capture_awb(
	mini_awb_gain_t *preview_awb, mini_awb_gain_t *flash_awb, mini_awb_gain_t *capture_awb,
	u32 weight_env, u32 weight_flash)
{
	capture_awb->b_gain = (weight_flash * flash_awb->b_gain + weight_env * preview_awb->b_gain) / 0x100;
	capture_awb->gb_gain = (weight_flash * flash_awb->gb_gain + weight_env * preview_awb->gb_gain) / 0x100;
	capture_awb->gr_gain = (weight_flash * flash_awb->gr_gain + weight_env * preview_awb->gr_gain) / 0x100;
	capture_awb->r_gain = (weight_flash * flash_awb->r_gain + weight_env * preview_awb->r_gain) / 0x100;
}

void mini_ispv1_get_wb_value(mini_awb_gain_t *awb)
{
	GETREG16(MANUAL_AWB_GAIN_B, awb->b_gain);
	GETREG16(MANUAL_AWB_GAIN_GB, awb->gb_gain);
	GETREG16(MANUAL_AWB_GAIN_GR, awb->gr_gain);
	GETREG16(MANUAL_AWB_GAIN_R, awb->r_gain);
}

void mini_ispv1_set_wb_value(mini_awb_gain_t *awb)
{
	SETREG16(MANUAL_AWB_GAIN_B, awb->b_gain);
	SETREG16(MANUAL_AWB_GAIN_GB, awb->gb_gain);
	SETREG16(MANUAL_AWB_GAIN_GR, awb->gr_gain);
	SETREG16(MANUAL_AWB_GAIN_R, awb->r_gain);
}

static void ispv1_set_zsl_wb_value(mini_awb_gain_t *awb)
{
	SETREG16(0x65310, awb->b_gain);
	SETREG16(0x65312, awb->gb_gain);
	SETREG16(0x65314, awb->gr_gain);
	SETREG16(0x65316, awb->r_gain);
}

/* added by y00215412 DTS2012122102699 for U9508 two type flash LEDs 2012-12-21. */
static void ispv1_cal_weight(
	u32 lum_env, u32 lum_flash,
	u32 *weight_env, u32 *weight_flash)
{
	if ((lum_flash + lum_env) != 0) {
		*weight_env = 0x100 * lum_env / (lum_flash + lum_env);
		*weight_flash = 0x100 * lum_flash / (lum_flash + lum_env);
	} else {
		*weight_env = 0x100;
		*weight_flash = 0;
	}
}

static void ispv1_cal_flash_awb(
	u32 weight_env, u32 weight_flash,
	mini_awb_gain_t *preview_awb, mini_awb_gain_t *preflash_awb, mini_awb_gain_t *flash_awb)
{
	if (weight_flash != 0) {
		flash_awb->b_gain = abs((int)preflash_awb->b_gain * 0x100 - (int)preview_awb->b_gain * weight_env) / weight_flash;
		flash_awb->gb_gain = abs((int)preflash_awb->gb_gain * 0x100 - (int)preview_awb->gb_gain * weight_env) / weight_flash;
		flash_awb->gr_gain = abs((int)preflash_awb->gr_gain * 0x100 - (int)preview_awb->gr_gain * weight_env) / weight_flash;
		flash_awb->r_gain = abs((int)preflash_awb->r_gain * 0x100 - (int)preview_awb->r_gain * weight_env) / weight_flash;
	} else {
		flash_awb->b_gain = preview_awb->b_gain;
		flash_awb->gb_gain = preview_awb->gb_gain;
		flash_awb->gr_gain = preview_awb->gr_gain;
		flash_awb->r_gain = preview_awb->r_gain;
	}
}

static bool ispv1_check_awb_match(mini_awb_gain_t *flash_awb, mini_awb_gain_t *led0, mini_awb_gain_t *led1, u32 weight_env)
{
	mini_awb_gain_t diff_led0, diff_led1;
	bool ret = true;

	/* if calculated flash_awb is differ a lot from preset_flash_awb, use preset_flash_awb. */
	diff_led0.b_gain = abs((int)led0->b_gain - (int)flash_awb->b_gain);
	diff_led0.r_gain = abs((int)led0->r_gain - (int)flash_awb->r_gain);

	diff_led1.b_gain = abs((int)led1->b_gain - (int)flash_awb->b_gain);
	diff_led1.r_gain = abs((int)led1->r_gain - (int)flash_awb->r_gain);

	if ((diff_led0.b_gain >= led0->b_gain / 3 || diff_led0.r_gain >= led0->r_gain / 3) &&
		(diff_led1.b_gain >= led1->b_gain / 3 || diff_led1.r_gain >= led1->r_gain / 3)) {
		ret = false;
		print_info("differ a lot, cal awb[B 0x%x, R 0x%x]", flash_awb->b_gain, flash_awb->r_gain);
	}

	return ret;
}

static bool ispv1_need_rcc(mini_camera_sensor *sensor)
{
	bool ret = false;
	if (sensor->isp_location == CAMERA_USE_K3ISP && sensor->effect->rcc.rcc_enable == true)
		ret = true;
	return ret;
}

/* added by y00215412 DTS2012062808862 2012-07-03 */
/* last modified for DTS2012121301534 by j00212990 in 2012-12-19 */
static void ispv1_cal_ratio_lum(
	mini_aec_data_t *preview_ae, mini_aec_data_t *preflash_ae,
	mini_aec_data_t *capflash_ae, u32 *preview_ratio_lum, mini_flash_weight_t *weight)
{
	mini_aec_data_t ratio_ae;
	int delta_lum, delta_lum_max, delta_lum_sum;
	u32 ratio = 0x100;
	mini_effect_params *effect = get_effect_ptr();
	mini_flash_capture_s *this_flash_cap = &effect->flash.flash_capture;

	print_debug("preview:[0x%x,0x%x,lum 0x%x,lumMax 0x%x,lumSum 0x%x]; preflash:[0x%x,0x%x,lum 0x%x,LumMax 0x%x,lumSum 0x%x]",
		preview_ae->gain, preview_ae->expo, preview_ae->lum, preview_ae->lum_max, preview_ae->lum_sum,
		preflash_ae->gain, preflash_ae->expo, preflash_ae->lum, preflash_ae->lum_max, preflash_ae->lum_sum);

	ratio = ratio * (preflash_ae->gain * preflash_ae->expo) / (preview_ae->gain * preview_ae->expo);
	if (ratio != 0) {
		ratio_ae.lum = preview_ae->lum * ratio / 0x100;
		ratio_ae.lum_max = preview_ae->lum_max * ratio / 0x100;
		ratio_ae.lum_sum = preview_ae->lum_sum * ratio / 0x100;
	} else {
		ratio_ae.lum = 0;
		ratio_ae.lum_max = 0;
		ratio_ae.lum_sum = 0;
	}

	/* delta_lum is lum of env part */
	delta_lum = preflash_ae->lum - ratio_ae.lum;
	if (delta_lum < 0)
		delta_lum = 0;

	delta_lum_max = preflash_ae->lum_max - ratio_ae.lum_max;
	if (delta_lum_max < 0)
		delta_lum_max = 0;

	delta_lum_sum = preflash_ae->lum_sum - ratio_ae.lum_sum;
	if (delta_lum_sum < 0)
		delta_lum_sum = 0;

	capflash_ae->lum = this_flash_cap->cap2pre_ratio * delta_lum + ratio_ae.lum;
	capflash_ae->lum_max = this_flash_cap->cap2pre_ratio * delta_lum_max + ratio_ae.lum_max;
	capflash_ae->lum_sum = this_flash_cap->cap2pre_ratio * delta_lum_sum + ratio_ae.lum_sum;

	/* if low light, we will use lum_sum to calculate weight for accuracy. */
	if (preflash_ae->lum < this_flash_cap->lowlight_th) {
		ispv1_cal_weight(ratio_ae.lum_sum, preflash_ae->lum_sum - ratio_ae.lum_sum, &(weight->preflash_env), &(weight->preflash_flash));
		ispv1_cal_weight(ratio_ae.lum_sum, capflash_ae->lum_sum - ratio_ae.lum_sum, &(weight->capflash_env), &(weight->capflash_flash));
	} else {
		ispv1_cal_weight(ratio_ae.lum, preflash_ae->lum - ratio_ae.lum, &(weight->preflash_env), &(weight->preflash_flash));
		ispv1_cal_weight(ratio_ae.lum, capflash_ae->lum - ratio_ae.lum, &(weight->capflash_env), &(weight->capflash_flash));
	}

	/* if calculated capture flash lum is zero, set it as 1 to avoid zero divide panic. */
	if (capflash_ae->lum == 0)
		capflash_ae->lum = 1;

	*preview_ratio_lum = ratio_ae.lum;
}

static u32 ispv1_cal_ratio_factor(mini_aec_data_t *preview_ae, mini_aec_data_t *preflash_ae, mini_aec_data_t *capflash_ae,
	u32 target_y_low)
{
	u32 ratio_factor = 0x100;
	u32 temp_ratio = 0x100;
	u32 capture_target_lum;
	mini_effect_params *effect = get_effect_ptr();
	mini_flash_capture_s *this_flash_cap = &effect->flash.flash_capture;

	/* added for DTS2012121301534 by j00212990 in 2012-12-08. */
	if (capflash_ae->lum != 0) {
		if (preview_ae->lum < target_y_low) {
			capture_target_lum = this_flash_cap->default_target_y;
			ratio_factor = ratio_factor * capture_target_lum / capflash_ae->lum;
			capflash_ae->lum_max = capflash_ae->lum_max * ratio_factor / 0x100;
			if (capflash_ae->lum_max > this_flash_cap->over_expo_th) {
				/* decrease capflash max lum to  FLASH_CAP_RAW_OVER_EXPO */
				temp_ratio = 0x100 * this_flash_cap->over_expo_th / capflash_ae->lum_max;
				ratio_factor = ratio_factor * temp_ratio / 0x100;
				capflash_ae->lum_max = capflash_ae->lum_max * temp_ratio / 0x100;
				print_info("%s, capflash lum_max 0x%x, temp_ratio 0x%x, new ratio_factor 0x%x",
					__func__, capflash_ae->lum_max, temp_ratio, ratio_factor);
			}
		} else {
			capture_target_lum = preview_ae->lum;
			ratio_factor = ratio_factor * capture_target_lum / capflash_ae->lum;
		}
	} else {
		ratio_factor = ISP_EXPOSURE_RATIO_MAX;
	}

	return ratio_factor;
}

/* Added for DTS2012121005733 by y00215412 2012-12-10 */
void mini_ispv1_save_aecawb_step(mini_aecawb_step_t *step)
{
	step->stable_range0 = GETREG8(REG_ISP_UNSTABLE2STABLE_RANGE);
	step->stable_range1 = GETREG8(REG_ISP_STABLE2UNSTABLE_RANGE);
	step->fast_step = GETREG8(REG_ISP_FAST_STEP);
	step->slow_step = GETREG8(REG_ISP_SLOW_STEP);
	step->awb_step = GETREG8(REG_ISP_AWB_STEP);
}

void mini_ispv1_config_aecawb_step(bool flash_mode, mini_aecawb_step_t *step)
{
	mini_effect_params *effect = get_effect_ptr();
	mini_aecawb_step_t *this_aecawb_step = &effect->flash.aecawb_step;
	if (flash_mode == true) {
		SETREG8(REG_ISP_UNSTABLE2STABLE_RANGE, (this_aecawb_step->stable_range0));
		SETREG8(REG_ISP_STABLE2UNSTABLE_RANGE, this_aecawb_step->stable_range1);
		SETREG8(REG_ISP_FAST_STEP, this_aecawb_step->fast_step);
		SETREG8(REG_ISP_SLOW_STEP, this_aecawb_step->slow_step);
		SETREG8(REG_ISP_AWB_STEP, this_aecawb_step->awb_step); //awb step
	} else {
		SETREG8(REG_ISP_UNSTABLE2STABLE_RANGE, step->stable_range0);
		SETREG8(REG_ISP_STABLE2UNSTABLE_RANGE, step->stable_range1);
		SETREG8(REG_ISP_FAST_STEP, step->fast_step);
		SETREG8(REG_ISP_SLOW_STEP, step->slow_step);
		SETREG8(REG_ISP_AWB_STEP, step->awb_step); //awb step
	}
}

/* check if current env AE status is need auto-flash. */
bool mini_ae_is_need_flash(mini_camera_sensor *sensor, mini_aec_data_t *ae_data, u32 target_y_low)
{
	u32 frame_index = sensor->preview_frmsize_index;
	u32 compare_gain;
	bool summary;
	bool ret;
    compare_gain = sensor->effect->flash.flash_capture.trigger_gain;

	summary = sensor->frmsize_list[frame_index].summary;
	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, begin */
	if( (summary == false)
	&& (true == sensor->support_summary))
		compare_gain *= 2;
	/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, end */

	if (ae_data->lum <= (target_y_low * FLASH_TRIGGER_LUM_RATIO / 0x100) ||
		ae_data->gain >= compare_gain)
		ret = true;
	else
		ret = false;

	return ret;
}

static inline void set_awbtest_policy(FLASH_AWBTEST_POLICY action)
{
	mini_isp_hw_data.awb_test = action;
}

static inline FLASH_AWBTEST_POLICY get_awbtest_policy(void)
{
	return mini_isp_hw_data.awb_test;
}

/* y00215412 20121025 added for DTS2012102500413 */
void mini_ispv1_check_flash_prepare(void)
{


	mini_awb_gain_t *preset_awb = &(mini_isp_hw_data.flash_preset_awb);
	mini_awb_gain_t *led_awb0 = &(mini_isp_hw_data.led_awb[0]);
	mini_awb_gain_t *led_awb1 = &(mini_isp_hw_data.led_awb[1]);
	FLASH_AWBTEST_POLICY action;
    mini_effect_params *effect = get_effect_ptr();

	/* modified for DTS2013022701160 by j00212990 in 2013-02-27 */
	if ((mini_afae_ctrl_is_valid() == true) &&
		(FOCUS_STATE_CAF_RUNNING == mini_get_focus_state() ||
		FOCUS_STATE_CAF_DETECTING == mini_get_focus_state() ||
		FOCUS_STATE_AF_RUNNING == mini_get_focus_state())) {
		print_debug("enter %s, must stop focus, before turn on preflash. ", __func__);
		mini_ispv1_auto_focus(FOCUS_STOP);
	}

	/* enlarge AEC/AGC step to make AEC/AGC stable quickly. */
	mini_ispv1_config_aecawb_step(true, &mini_isp_hw_data.aecawb_step);
#if 0 //hefei delete for flash awb
	mini_camera_sensor *sensor = mini_this_ispdata->sensor;
	flash_platform_t type;
	/* get platform type */
	#ifdef PLATFORM_TYPE_PAD_S10
		type = FLASH_PLATFORM_S10;
	#else
		boardid = get_boardid();
		print_info("%s : boardid=0x%x.", __func__, boardid);

		/* if unknow board ID, should use flash params as X9510E */
		if (boardid == BOARD_ID_CS_U9510E || boardid == BOARD_ID_CS_T9510E)
			type = FLASH_PLATFORM_9510E;
		else if (boardid == BOARD_ID_CS_U9510)
			type = FLASH_PLATFORM_U9510;
		else
			type = FLASH_PLATFORM_9510E;
	#endif

	if(sensor->get_flash_awb != NULL)
		sensor->get_flash_awb(type, preset_awb);
	else
		memcpy(preset_awb, &mini_flash_platform_awb[type], sizeof(mini_awb_gain_t));
#endif
    memcpy(preset_awb, &effect->flash.gain, sizeof(mini_awb_gain_t));

	if (preset_awb->gb_gain == 0x80 && preset_awb->gr_gain == 0x80) {
		action = FLASH_AWBTEST_POLICY_FIXED;
	} else {
		led_awb0->b_gain = preset_awb->b_gain;
		led_awb0->gb_gain = 0x80;
		led_awb0->gr_gain = 0x80;
		led_awb0->r_gain = preset_awb->gb_gain;

		led_awb1->b_gain = preset_awb->gr_gain;
		led_awb1->gb_gain = 0x80;
		led_awb1->gr_gain = 0x80;
		led_awb1->r_gain = preset_awb->r_gain;

		preset_awb->b_gain = led_awb0->b_gain;
		preset_awb->gb_gain = 0x80;
		preset_awb->gr_gain = 0x80;
		preset_awb->r_gain = led_awb0->r_gain;

		action = FLASH_AWBTEST_POLICY_FREEGO;
	}

	set_awbtest_policy(action);

	/* If clearly know led type, need do noting, else will do someting. */
	if (action == FLASH_AWBTEST_POLICY_FIXED) {
		return;
	}

	mini_ispv1_set_awb_mode(MANUAL_AWB);
	mini_ispv1_set_wb_value(preset_awb);

	print_info("unsure flash led AWB, preset_awb[B 0x%x, R 0x%x]", preset_awb->b_gain, preset_awb->r_gain);
}

static void ispv1_check_flash_exit(void)
{
	mini_ispv1_config_aecawb_step(false, &mini_isp_hw_data.aecawb_step);
}

/*
 * change logs:
 * change flash testing by y00215412 DTS2012062808862 2012-07-03
 * Revised by y00231328 20121017 for DTS2012101201219
 * Revised by y00215412 20121229 for DTS2012122102699 and DTS2012122510436
 */
static void ispv1_poll_flash_lum(void)
{
	static u8 frame_count;
	static u8 frozen_frame;
	mini_k3_isp_data *ispdata = mini_this_ispdata;
	mini_camera_sensor *sensor = ispdata->sensor;

	u32 volatile cur_lum;

	mini_aec_data_t *preview_ae = &(mini_isp_hw_data.preview_ae);
	mini_aec_data_t *preflash_ae = &(mini_isp_hw_data.preflash_ae);
	mini_aec_data_t capflash_ae;
	u32 preview_ratio_lum;

	mini_awb_gain_t *preview_awb = &(mini_isp_hw_data.preview_awb);
	mini_awb_gain_t *preset_awb = &(mini_isp_hw_data.flash_preset_awb);
	mini_awb_gain_t flash_awb = {0x80, 0x80, 0x80, 0x80};
	mini_awb_gain_t capture_awb;

	static mini_awb_gain_t last_awb;
	mini_awb_gain_t cur_awb;
	mini_awb_gain_t diff_awb;
	mini_awb_gain_t *led_awb0 = &(mini_isp_hw_data.led_awb[0]);
	mini_awb_gain_t *led_awb1 = &(mini_isp_hw_data.led_awb[1]);

	mini_flash_weight_t weight;
	bool match_result;
	bool need_flash;
	bool low_ct;
	mini_effect_params *effect = get_effect_ptr();
	mini_flash_param_s *this_flash = &effect->flash;
	u32 test_max_cnt = this_flash->flash_capture.test_max_cnt;
	//u32 lowCT_th = this_flash->flash_capture.lowCT_th;

	u8 target_y_low = GETREG8(REG_ISP_TARGET_Y_LOW);
	u8 over_expo_th = GETREG8(REG_ISP_TARGET_Y_HIGH) + this_flash->aecawb_step.slow_step;

	u32 unit_area;
#ifdef SUPPORT_ZSL_FLASH
	static u32 last_cur_lum=0;
	s32 dy;
#endif

	cur_lum = get_current_y();
	mini_ispv1_get_wb_value(&cur_awb);
#ifdef SUPPORT_ZSL_FLASH
	dy = cur_lum-last_cur_lum;
	last_cur_lum = cur_lum;
#endif

	/* stage 1: skip first 2 frames */
	if (++frame_count <= 2) {
		if (get_awbtest_policy() == FLASH_AWBTEST_POLICY_FIXED) {
			mini_ispv1_set_aecagc_mode(AUTO_AECAGC);
			mini_ispv1_set_awb_mode(AUTO_AWB);
		} else {
			mini_ispv1_set_aecagc_mode(AUTO_AECAGC);

			/*
			 * first frame set manual AWB and init WB value again to ensure it take effect;
			 * second frame set Auto awb.
			 */
			if (frame_count == 1) {
				mini_ispv1_set_awb_mode(MANUAL_AWB);
				mini_ispv1_set_wb_value(preset_awb);
			} else if (frame_count == 2)
				mini_ispv1_set_awb_mode(AUTO_AWB);
		}
		return;
	}

	/* stage 2: FLASH_TESTING: if aec/agc suitable or frame_count larger than threshold, should go to FLASH_FROZEN. */
	if ((ispdata->flash_flow == FLASH_TESTING) &&
		(cur_lum <= over_expo_th || frame_count >= test_max_cnt)
		#ifdef SUPPORT_ZSL_FLASH
		&&(abs(dy)<=5)
		#endif
	) {
		mini_ispv1_set_aecagc_mode(MANUAL_AECAGC);
		print_info("AEC/AGC OK, set to manual AECAGC, frame_count %d########", frame_count);

		if (get_awbtest_policy() == FLASH_AWBTEST_POLICY_FIXED) {
			mini_ispv1_set_awb_mode(MANUAL_AWB);
			print_info("AWB set to manual, need not test flash awb########");
		}
		ispdata->flash_flow = FLASH_FROZEN;
	} else if (ispdata->flash_flow == FLASH_FROZEN) {
		/* stage 3: FLASH_FROZEN */
		frozen_frame++;

		if (get_awbtest_policy() == FLASH_AWBTEST_POLICY_FIXED) {
			/* frozen second frame, get locked AE value, maybe need adjust lum_max */
			if (frozen_frame > 1) {
				preflash_ae->gain = get_writeback_gain();
				preflash_ae->expo = get_writeback_expo();
				preflash_ae->lum = cur_lum;
				preflash_ae->lum_max = cur_lum; /* set default value */
				preflash_ae->lum_sum = cur_lum; /* set default value */

				/* if Low light, get preflash lum_max */
				if (preview_ae->lum < target_y_low) {
					unit_area = mini_ispv1_get_stat_unit_area();
					mini_ispv1_get_raw_lum_info(preflash_ae, unit_area);
				}
				goto normal_fixawb_out;
			} else {
				/* do nothing, just wait and return */
				return;
			}
		} else {
			/* check whether awb is stable. */
			if (frozen_frame == 1) {
				/* first frame, set last_awb init value as cur_awb */
				memcpy(&last_awb, &cur_awb, sizeof(mini_awb_gain_t));
				return;
			}

			/* odd frames, we compare cur_awb with last_awb, if differ too much, continue. */
			if (frozen_frame % 2 != 0 && frozen_frame < test_max_cnt) {
				diff_awb.b_gain = abs((int)cur_awb.b_gain - (int)last_awb.b_gain);
				diff_awb.r_gain = abs((int)cur_awb.r_gain - (int)last_awb.r_gain);

				if (diff_awb.b_gain < 0x3 && diff_awb.r_gain < 0x3) {
					frozen_frame = test_max_cnt;
				} else {
					/* update last_awb using cur_awb. */
					memcpy(&last_awb, &cur_awb, sizeof(mini_awb_gain_t));
					return;
				}
			}

			if (frozen_frame < test_max_cnt) {
				return;
			} else if (frozen_frame == test_max_cnt) {
				/* awb test OK or frozen_frame count reach MAX_COUNT*/
				mini_ispv1_set_awb_mode(MANUAL_AWB);
				print_info("AWB differ OK or Reach MAX_COUNT, set to manual AWB########");
				return;
			} else {
				/* get locked AE value, maybe need adjust lum_max */
				preflash_ae->gain = get_writeback_gain();
				preflash_ae->expo = get_writeback_expo();
				preflash_ae->lum = cur_lum;
				preflash_ae->lum_max = cur_lum; /* set default value */
				preflash_ae->lum_sum = cur_lum; /* set default value */

				/* if Low light, get preflash lum_max */
				if (preview_ae->lum < target_y_low) {
					unit_area = mini_ispv1_get_stat_unit_area();
					mini_ispv1_get_raw_lum_info(preflash_ae, unit_area);
				}

				/* calculate capture flash lum */
				ispv1_cal_ratio_lum(preview_ae, preflash_ae, &capflash_ae, &preview_ratio_lum, &weight);

				/* calculate flash awb */
				ispv1_cal_flash_awb(weight.preflash_env, weight.preflash_flash, preview_awb, &cur_awb, &flash_awb);

				match_result = ispv1_check_awb_match(&flash_awb, led_awb0, led_awb1, weight.capflash_env);
				need_flash = mini_ae_is_need_flash(sensor, preview_ae, target_y_low);
				if (preview_awb->b_gain >= FLASH_PREVIEW_LOWCT_TH)
					low_ct = true;
				else
					low_ct = false;

				if (low_ct == true && need_flash == false) {
					memcpy(&flash_awb, preset_awb, sizeof(mini_awb_gain_t));
				} else if (match_result == true) {
					print_info("awb freego match, before compesate awb[B 0x%x, R 0x%x]", flash_awb.b_gain, flash_awb.r_gain);
					flash_awb.b_gain -= (flash_awb.b_gain / 16);
					flash_awb.r_gain += (flash_awb.r_gain / 16);
				} else {
					memcpy(&flash_awb, preset_awb, sizeof(mini_awb_gain_t));
				}

				goto awbtest_out;
			}
		}

normal_fixawb_out:
		ispv1_cal_ratio_lum(preview_ae, preflash_ae, &capflash_ae, &preview_ratio_lum, &weight);
		memcpy(&flash_awb, preset_awb, sizeof(mini_awb_gain_t));

awbtest_out:
		ispv1_cal_capture_awb(preview_awb, &flash_awb, &capture_awb,
			weight.capflash_env, weight.capflash_flash);

		/* To Capture_cmd: update preview_ratio_lum and flash ratio factor */
		mini_isp_hw_data.preview_ratio_lum = preview_ratio_lum;

		/* configure ratio_factor */
		mini_isp_hw_data.ratio_factor = ispv1_cal_ratio_factor(preview_ae, preflash_ae, &capflash_ae, target_y_low);

		print_info("preview[gain:0x%x,expo:0x%x,lum:0x%x,lumMax:0x%x,lumSum:0x%x], awb[B 0x%x, R 0x%x], preflash_weight_env 0x%x",
			preview_ae->gain, preview_ae->expo, preview_ae->lum, preview_ae->lum_max, preview_ae->lum_sum,
			preview_awb->b_gain, preview_awb->r_gain, weight.preflash_env);
		print_info("preflash[gain:0x%x,expo:0x%x,lum:0x%x,lumMax:0x%x,lumSum:0x%x], awb[B 0x%x, R 0x%x], preview_ratio_lum 0x%x",
			preflash_ae->gain, preflash_ae->expo, preflash_ae->lum, preflash_ae->lum_max, preflash_ae->lum_sum,
			cur_awb.b_gain, cur_awb.r_gain, preview_ratio_lum);
		print_info("capflash y:0x%x, max:0x%x, ratio_factor:0x%x, awb[0x%x:0x%x], capflash weight 0x%x",
			capflash_ae.lum, capflash_ae.lum_max, mini_isp_hw_data.ratio_factor,
			capture_awb.b_gain, capture_awb.r_gain, weight.capflash_flash);

		mini_ispv1_set_wb_value(&capture_awb);
		ispv1_set_zsl_wb_value(&capture_awb);
		ispdata->flash_flow = FLASH_DONE;
		ispv1_check_flash_exit();
		frozen_frame = 0;
		frame_count = 0;
	}
}

/* added by y00215412 for dynamic y denoise changed  2012-11-7 start. */
#if 0
void mini_ispv1_switch_dns(mini_camera_sensor *sensor, camera_state state, bool flash_on, u32 expo_line)
{
	u8 ydns_value[DNS_MAX_STEP] = {
		ISP_YDENOISE_COFF_1X, /* gain 1x y denoise */
		ISP_YDENOISE_COFF_2X, /* gain 2x y denoise */
		ISP_YDENOISE_COFF_4X, /* gain 4x y denoise */
		ISP_YDENOISE_COFF_8X, /* gain 8x y denoise */
		ISP_YDENOISE_COFF_16X, /* gain 16x y denoise */
		ISP_YDENOISE_COFF_32X, /* gain 32x y denoise */
		ISP_YDENOISE_COFF_64X, /* gain 64x y denoise */
		};

	int idx;
	u32 bandstep_50Hz;
	u32 bandsteps;

	if (state == STATE_PREVIEW) {
		memset(ydns_value, ISP_YDENOISE_COFF_PREVIEW, DNS_MAX_STEP);
	} else {
		if (flash_on == false)
			idx = sensor->capture_frmsize_index;
		else
			idx = sensor->preview_frmsize_index;

		bandstep_50Hz = sensor->frmsize_list[idx].banding_step_50hz;
		bandsteps = (expo_line + bandstep_50Hz - 1) / bandstep_50Hz;
		if (bandsteps <= 1)
			ydns_value[0] = ISP_YDENOISE_COFF_1X;
		else
			ydns_value[0] = ISP_YDENOISE_COFF_2X;
	}

	for (idx = 0; idx < DNS_MAX_STEP; idx++)
		SETREG8(REG_ISP_GDNS(idx), ydns_value[idx]);
}
#endif
void mini_ispv1_switch_dns(mini_camera_sensor *sensor, camera_state state, bool flash_on, u32 expo_line)
{
	mini_effect_params *effect = sensor->effect;
	mini_denoise_s *dns;
	u8 g_dns_value[DNS_MAX_STEP];
	u8 g_dns_1x;
	u8 *g_dns;

	int idx;
	u32 bandstep_50Hz;
	u32 bandsteps;

	if (sensor->isp_location == CAMERA_USE_SENSORISP)
		return;

	dns = &(effect->dns);

	if (state == STATE_PREVIEW) {
		memcpy(g_dns_value, dns->g_dns_preview, DNS_MAX_STEP);
	} else {
		if (flash_on == false) {
			idx = sensor->capture_frmsize_index;
			g_dns_1x = dns->g_dns_capture_1band;
			g_dns = dns->g_dns_capture;
		} else {
			idx = sensor->preview_frmsize_index;
			g_dns_1x = dns->g_dns_flash_1band;
			g_dns = dns->g_dns_flash;
		}
		memcpy(g_dns_value, g_dns, DNS_MAX_STEP);

		bandstep_50Hz = sensor->frmsize_list[idx].banding_step_50hz;
		bandsteps = (expo_line + bandstep_50Hz - 1) / bandstep_50Hz;
		if (bandsteps <= 1)
			g_dns_value[0] = g_dns_1x;
	}

	for (idx = 0; idx < DNS_MAX_STEP; idx++)
		SETREG8(REG_ISP_GDNS(idx), g_dns_value[idx]);

	for (idx = 0; idx < DNS_MAX_STEP; idx++)
		SETREG8(REG_ISP_BRDNS(idx), dns->br_dns[idx]);

	for (idx = 0; idx < DNS_MAX_STEP - 1; idx++)
		SETREG8(REG_ISP_UVDNS(idx), dns->uv_dns[idx]);

}


/* revise by c00220250 for DTS2013120511750, 2013-12-06, begin */
void mini_ispv1_mannal_iso_dynamic_framerate(mini_camera_sensor *sensor)
{
	static u32 count = 0;
	u32 ret;
	u32 band_step_50HZ, band_step_60HZ;
    u16 expo_th_low2mid,  expo_th_mid2high,  expo_th_high2mid, expo_th_mid2low;
	mini_effect_params *effect = get_effect_ptr();
    u16 *auto_fps_manual_iso_th = effect->ae_param.auto_fps_manual_iso_th;
	camera_frame_rate_dir direction;

	camera_frame_rate_state state = mini_ispv1_get_frame_rate_state();

       u32 max_expo;
	u32 cur_expo = (mini_ispv1_get_expo_line() >> 4);

	GETREG32(REG_ISP_MAX_EXPOSURE, max_expo);



	expo_th_low2mid  =  auto_fps_manual_iso_th[0];
	expo_th_mid2high = auto_fps_manual_iso_th[1];
	expo_th_high2mid = auto_fps_manual_iso_th[2];
	expo_th_mid2low  = auto_fps_manual_iso_th[3];

    if((0 == expo_th_low2mid) || (0 == expo_th_mid2high) || (0 == expo_th_high2mid) ||(0 == expo_th_mid2low)){
        print_warn("%s manual iso expo thre is 0!", __FUNCTION__);
    }

	mini_ispv1_get_banding_step(sensor, sensor->preview_frmsize_index, &band_step_50HZ, &band_step_60HZ)

	print_debug("%s state:%d, cur_expo:%d, max_expo:%d, band_step_50HZ:%d, preview_index:%d",
	__FUNCTION__, state, cur_expo, max_expo, band_step_50HZ, sensor->preview_frmsize_index);

	if (((state == CAMERA_FPS_STATE_LOW) && (cur_expo * 100 < band_step_50HZ * expo_th_low2mid))
		|| ((state == CAMERA_FPS_STATE_MIDDLE) && (cur_expo * 100 < band_step_50HZ * expo_th_mid2high))) {
		direction = CAMERA_FRAME_RATE_UP;
		count++;
	} else if (((state == CAMERA_FPS_STATE_HIGH) && (cur_expo +  band_step_50HZ * expo_th_high2mid /100 > max_expo ))
		|| ((state == CAMERA_FPS_STATE_MIDDLE) && (cur_expo + band_step_50HZ * expo_th_mid2low /100 > max_expo ))) {
		direction = CAMERA_FRAME_RATE_DOWN;
		count++;
	} else {
		count = 0;
	}
	/* added for non-binning frame size auto frame rate control end */

	if (count >= AUTO_FRAME_RATE_TRIGER_COUNT) {
		/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
		if (true == mini_ispv1_get_aec_state()) {
			ret = ispv1_change_frame_rate(&state, direction, sensor);
			mini_ispv1_set_frame_rate_state(state);
			count = 0;
		}
		/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
	}
}
/* revise by c00220250 for DTS2013120511750, 2013-12-06, end */

/* added by y00215412 for dynamic y denoise changed  2012-11-7 end. */

static void ispv1_dynamic_framerate(mini_camera_sensor *sensor, camera_iso iso)
{
	static u32 count;
	u16 gain;
	int ret;
	int index;
	mini_effect_params *effect = get_effect_ptr();
	u16 gain_th_low2mid, gain_th_mid2high, gain_th_high2mid, gain_th_mid2low;
	camera_frame_rate_dir direction;
	camera_frame_rate_state state = mini_ispv1_get_frame_rate_state();
	u16 *auto_fps_th = effect->ae_param.auto_fps_th;

	gain_th_low2mid = auto_fps_th[0];
	gain_th_mid2high = auto_fps_th[1];
	gain_th_high2mid = auto_fps_th[2];
	gain_th_mid2low = auto_fps_th[3];


	if (CAMERA_ISO_AUTO != iso) {
              /* revise by c00220250 for DTS2013120511750, 2013-12-06, begin*/
		/* add by y00215412 2012-11-05. */
		if (state == CAMERA_EXPO_PRE_REDUCE1 || state == CAMERA_EXPO_PRE_REDUCE2) {
			//|| ispv1_get_frame_rate_level() != 0) {
			ispv1_change_frame_rate(&state, CAMERA_FRAME_RATE_UP, sensor);
			mini_ispv1_set_frame_rate_state(state);
			return;
		}
		mini_ispv1_mannal_iso_dynamic_framerate(sensor);
              /* revise by c00220250 for DTS2013120511750, 2013-12-06, end */
	} else {
		/* add by y00215412 2012-11-05, state CAMERA_EXPO_PRE_REDUCE is highest priority. */
		if (state == CAMERA_EXPO_PRE_REDUCE1 || state == CAMERA_EXPO_PRE_REDUCE2) {
			ret = ispv1_change_frame_rate(&state, CAMERA_FRAME_RATE_UP, sensor);
			mini_ispv1_set_frame_rate_state(state);
			return;
		}

		/*get gain from isp */
		gain = get_writeback_gain();

		/* added for non-summary frame size auto frame rate control start */
		index = sensor->preview_frmsize_index;

		/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, begin */
		if( (sensor->frmsize_list[index].summary == false)
		&&(true == sensor->support_summary)){
            gain_th_low2mid *= 2;
            gain_th_mid2high *= 2;
            gain_th_high2mid *= 2;
            gain_th_mid2low *= 2;
		}
		/* Modified  by w00199382 for DTS2013042503069, 2013/7/22, end */

		if ((state == CAMERA_FPS_STATE_LOW && gain < gain_th_low2mid)
			|| (state == CAMERA_FPS_STATE_MIDDLE && gain < gain_th_mid2high)) {
			direction = CAMERA_FRAME_RATE_UP;
			count++;
		} else if ((state == CAMERA_FPS_STATE_HIGH && gain > gain_th_high2mid)
			|| (state == CAMERA_FPS_STATE_MIDDLE && gain > gain_th_mid2low)) {
			direction = CAMERA_FRAME_RATE_DOWN;
			count++;
		} else {
			count = 0;
		}
		/* added for non-binning frame size auto frame rate control end */

		if (count >= AUTO_FRAME_RATE_TRIGER_COUNT) {
			/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
			if (true == mini_ispv1_get_aec_state()) {
				ret = ispv1_change_frame_rate(&state, direction, sensor);
				mini_ispv1_set_frame_rate_state(state);
				count = 0;
			}
			/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
		}
	}
}

/* last modified for DTS2013032202618 by j00212990 in 2013-03-22 */
void mini_ispv1_preview_done_do_tune(void)
{
	mini_k3_isp_data *ispdata = mini_this_ispdata;
	mini_camera_sensor *sensor;
	static mini_k3_last_state last_state = {CAMERA_SATURATION_MAX, CAMERA_CONTRAST_MAX, CAMERA_BRIGHTNESS_MAX, CAMERA_EFFECT_MAX};
	u8 target_y_low = GETREG8(REG_ISP_TARGET_Y_LOW);
	u32 unit_area;
    mini_effect_params *effect = get_effect_ptr();

	camera_frame_rate_state state = mini_ispv1_get_frame_rate_state();

	if (NULL == ispdata || mini_isp_hw_data.frame_count <= 1) {
		return;
	}

	sensor = ispdata->sensor;

	if ((mini_isp_hw_data.frame_count % 100) == 0) {
		print_info("fps %d, expo 0x%x, gain 0x%x, current y 0x%x(100 frames print once)",
			sensor->fps, get_writeback_expo(), get_writeback_gain(), get_current_y());
	}

	/* If it is YUV sensor, bellowing code should not be executed. */
	if (CAMERA_USE_SENSORISP == sensor->isp_location)
		return;
    /*K3 DTS2013072905886 begin*/
	if (ispdata->cur_crop_pos != ispdata->next_crop_pos) {
		mini_ispv1_refresh_yuv_crop_pos();
	}
    /*K3 DTS2013072905886 end*/
	/* save preview AE and AWB params, DTS2012062808862 2012-07-03 */
	if (false == ispdata->flash_on) {
		mini_isp_hw_data.preview_ae.gain = get_writeback_gain();
		mini_isp_hw_data.preview_ae.expo = get_writeback_expo();
		mini_isp_hw_data.preview_ae.lum = get_current_y();
		/* set default value for lum_max and lum_sum */
		mini_isp_hw_data.preview_ae.lum_max = mini_isp_hw_data.preview_ae.lum;
		mini_isp_hw_data.preview_ae.lum_sum = mini_isp_hw_data.preview_ae.lum;

		/* changed by y00215412 DTS2012122503253 2012-12-25*/
		/* modified for DTS2013022701160 by j00212990 in 2013-02-27 */
		if ((CAMERA_USE_K3ISP == sensor->isp_location) &&
			(mini_isp_hw_data.preview_ae.lum < target_y_low)) {
			unit_area = mini_ispv1_get_stat_unit_area();
			mini_ispv1_get_raw_lum_info(&mini_isp_hw_data.preview_ae, unit_area);
		}
		mini_ispv1_get_wb_value(&mini_isp_hw_data.preview_awb);
	}

	if ((FLASH_TESTING == ispdata->flash_flow) || (FLASH_FROZEN == ispdata->flash_flow)) {
		/*added by y00215412 2012-12-18 to improve framerate first to speed up FLASH_TESTING */
		if (state == CAMERA_EXPO_PRE_REDUCE1 || state == CAMERA_EXPO_PRE_REDUCE2
			|| mini_ispv1_get_frame_rate_level() != 0) {
			mini_ispv1_set_aecagc_mode(AUTO_AECAGC);
			ispv1_change_frame_rate(&state, CAMERA_FRAME_RATE_UP, sensor);
			mini_ispv1_set_frame_rate_state(state);
			mini_isp_hw_data.ae_resume = true;
		} else {
			ispv1_poll_flash_lum();
		}
	} else if (mini_afae_ctrl_is_valid() == true &&
		((mini_get_focus_state() == FOCUS_STATE_AF_PREPARING) ||
		(mini_get_focus_state() == FOCUS_STATE_AF_RUNNING) ||
		(mini_get_focus_state() == FOCUS_STATE_CAF_RUNNING))) {
		/* add by y36721 DTS2012032102247 2012-03-22. */
		print_debug("focusing metering, should not change frame rate.");
	} else if (mini_isp_hw_data.ae_resume == true && ispdata->flash_on == false) {
		mini_ispv1_set_aecagc_mode(AUTO_AECAGC);
		ispv1_change_frame_rate(&state, CAMERA_FRAME_RATE_DOWN, sensor);
		mini_ispv1_set_frame_rate_state(state);
		mini_isp_hw_data.ae_resume = false;
	} else {
		ispv1_dynamic_framerate(sensor, ispdata->iso);
	}

	if (true == camera_ajustments_flag) {
		last_state.saturation = CAMERA_SATURATION_MAX;
		last_state.contrast = CAMERA_CONTRAST_MAX;
		last_state.brightness = CAMERA_BRIGHTNESS_MAX;
		last_state.effect = CAMERA_EFFECT_MAX;
		camera_ajustments_flag = false;
	}

	/*camera_effect_saturation_done*/
	if ((ispdata->effect != last_state.effect)||(ispdata->saturation != last_state.saturation)) {
		ispv1_set_effect_saturation_done(ispdata->effect, ispdata->saturation, last_state.contrast, last_state.brightness);
		last_state.effect = ispdata->effect;
		last_state.saturation = ispdata->saturation;
	}
	/* Modified  by w00199382 for DTS2013082709380, 2013/9/17, begin */
	/*contrast_done*/
	if ((ispdata->contrast != last_state.contrast)
	&&(CAMERA_EFFECT_NONE == ispdata->effect)) {
		ispv1_set_contrast_done(ispdata->contrast);
		last_state.contrast = ispdata->contrast;
	}

	/*brightness_done*/
	if((ispdata->brightness != last_state.brightness)
	&&(CAMERA_EFFECT_NONE == ispdata->effect)){
		ispv1_set_brightness_done(ispdata->brightness);
		last_state.brightness = ispdata->brightness;
	}
	/* Modified  by w00199382 for DTS2013082709380, 2013/9/17, end */

	/* added by y00231328 for DTS2012102604572 to set CCM gain when awb and ae changed  2012-11-1 start. */
	if (sensor->awb_dynamic_ccm_gain != NULL) {
		u32 frame_index, ae_value, gain, exposure_line;
		mini_awb_gain_t awb_gain;
		mini_ccm_gain_t ccm_gain;

		GETREG16(REG_ISP_AWB_ORI_GAIN_B, awb_gain.b_gain);
		GETREG16(REG_ISP_AWB_ORI_GAIN_R, awb_gain.r_gain);

		gain= get_writeback_gain();
		exposure_line= get_writeback_expo() >> 4;
		ae_value = gain * exposure_line;

		frame_index = sensor->preview_frmsize_index;
		sensor->awb_dynamic_ccm_gain(frame_index, ae_value, &awb_gain, &ccm_gain);

		SETREG8(REG_ISP_CCM_PREGAIN_R, ccm_gain.r_gain);
		SETREG8(REG_ISP_CCM_PREGAIN_B, ccm_gain.b_gain);
	}
	/* added by y00231328 for DTS2012102604572 to set CCM gain when awb and ae changed  2012-11-1 end. */

	//mini_ispv1_wakeup_focus_schedule(false);//by wind

	/* added by w00225854 for DTS2012102902756 2013-01-26 */
	if ((ispv1_need_rcc(mini_this_ispdata->sensor) == true)
		&& (mini_isp_hw_data.frame_count % effect->rcc.frame_interval == 0)) {
		/* modified for DTS2013022701160 by j00212990 in 2013-02-27 */
		if (mini_afae_ctrl_is_valid() == false) {
			mini_ispv1_uv_stat_excute();
		} else if ((mini_get_focus_state() != FOCUS_STATE_CAF_RUNNING) &&
			(mini_get_focus_state() != FOCUS_STATE_AF_RUNNING)) {
			mini_ispv1_uv_stat_excute();
		}
	}
}

/*
 * Used for tune ops and AF functions to get isp_data handler
 */
int mini_ispv1_tune_ops_init(mini_k3_isp_data *ispdata)
{
	mini_camera_sensor *sensor = ispdata->sensor;

	mini_this_ispdata = ispdata;

	mini_this_ispdata->current_frame = NULL;

	if (sensor->isp_location != CAMERA_USE_K3ISP)
		return -1;

	/* added for DTS2013030101733 by j00212990 in 2013-03-11 */
	sema_init(&(mini_this_ispdata->frame_sem), 0);

	/* maybe some fixed configurations, such as focus, ccm, lensc... */
	if (sensor->af_enable) {
        /*maybe malloc fail*/
		if(0 != mini_ispv1_focus_init()) {
            return -1;
		}
	}
	/* added for DTS2012102902756 by w00225854 in 2012-01-17 copy preview uv data for red clip */
	if (ispv1_need_rcc(sensor) == true)
		mini_ispv1_uv_stat_init();
	if(sensor->sensor_hdr_movie.support_hdr_movie)
		ispv1_hdr_ae_init();
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
	mini_ispv1_do_aeag_wq_init();
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
	camera_ajustments_flag = true;

	/* add by y00215412 2012-11-05 to init frame_rate_state. */
	mini_ispv1_set_frame_rate_state(CAMERA_FPS_STATE_HIGH);

	ispv1_init_sensor_config(sensor);

	mini_ispv1_save_aecawb_step(&mini_isp_hw_data.aecawb_step);

    return 0;
}

/*
 * something need to do after camera exit
 */
void mini_ispv1_tune_ops_exit(void)
{
	if (mini_this_ispdata->sensor->af_enable == 1)
		mini_ispv1_focus_exit();

	/* added for DTS2012102902756 by w00225854 in 2012-01-17 */
	if (ispv1_need_rcc(mini_this_ispdata->sensor) == true)
		mini_ispv1_uv_stat_exit();
	if(mini_this_ispdata->sensor->sensor_hdr_movie.support_hdr_movie)
		ispv1_hdr_ae_exit();
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
	mini_ispv1_aeag_exit();
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
}

/*
 * something need to do before start_preview and start_capture
 * last modified for DTS2013022701160 by j00212990 in 2013-02-27
 */
void mini_ispv1_tune_ops_prepare(camera_state state)
{
	mini_k3_isp_data *ispdata = mini_this_ispdata;
	mini_camera_sensor *sensor = ispdata->sensor;
	u32 unit_width = ispdata->pic_attr[STATE_PREVIEW].in_width / ISP_LUM_WIN_WIDTH_NUM;
	u32 unit_height = ispdata->pic_attr[STATE_PREVIEW].in_height / ISP_LUM_WIN_HEIGHT_NUM;
	mini_coordinate_s center; /* added for focus AE by s00061250 2013-06-27 */

	if (STATE_PREVIEW == state) {

		/* For AF update */
		if (sensor->af_enable)
			mini_ispv1_focus_prepare();

		if (CAMERA_USE_K3ISP == sensor->isp_location) {
			center.x = ispdata->pic_attr[state].out_width / 2;
			center.y = ispdata->pic_attr[state].out_height / 2;
			mini_ispv1_set_ae_statwin(&ispdata->pic_attr[state], &center, METERING_STATWIN_NORMAL, ispdata->zoom);
			mini_ispv1_set_stat_unit_area(unit_height * unit_width);
			/* added for DTS2013030101733 by j00212990 in 2013-03-11 */
			up(&(mini_this_ispdata->frame_sem));
		}

		ispdata->flash_flow = FLASH_DONE;
	    ispdata->next_crop_pos = 0;
		ispdata->cur_crop_pos = 0;
	} else if (STATE_CAPTURE == state) {
		/* we can add some other things to do before capture */
	}

    /* remove by c00220250 for DTS2013110200802, 2013-11-08, begin */
	/* update lens correction scale size */
	//mini_ispv1_update_LENC_scale(ispdata->pic_attr[state].in_width, ispdata->pic_attr[state].in_height);
    /* remove by c00220250 for DTS2013110200802, 2013-11-08, end */
}

/*
 * something need to do before stop_preview and stop_capture
 */
/* last modified for DTS2013030101733 by j00212990 in 2013-03-04 */
void mini_ispv1_tune_ops_withdraw(camera_state state)
{
	if (mini_this_ispdata->sensor->af_enable)
		mini_ispv1_focus_withdraw();
	/* modified for DTS2013020103989 by j00212990 2013-02-01 for flush work queue */
	if ((ispv1_need_rcc(mini_this_ispdata->sensor) == true)
		&& (down_trylock(&(mini_this_ispdata->frame_sem)) != 0)) {
			/*
			  * when need to excute rcc function and
			  * semaphore frame_sem has already been locked,
			  * flush_workqueue should be called, then relock frame_sem again
			  */
			flush_workqueue(uv_stat_work_queue);
			down(&(mini_this_ispdata->frame_sem));
	}
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
	flush_workqueue(aeag_work_queue);
	/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */

		if(mini_this_ispdata->hw_3a_switch == HW_3A_ON)
		{
			mini_ispv1_hw_3a_switch(HW_3A_OFF);
			mini_this_ispdata->hw_3a_switch = HW_3A_OFF;
		}
}

/* added by y00215412 for hdr movie mode begin */
bool mini_ispv1_is_hdr_movie_mode(void)
{
	u8 ae_ctrl_mode = GETREG8(REG_ISP_AE_CTRL_MODE); /* 1 - AP's ae . 0 - ISP ae. */
	u8 ae_write_mode = GETREG8(REG_ISP_AE_WRITE_MODE); /* 1 - ISP write sensor shutter/gain; 0 - AP write shutter/gain. */

	ae_ctrl_mode &= 0x1;
	ae_write_mode &= 0x1;

	/* AP's AE; AP write sensor shutter & gain.*/
	if (ae_ctrl_mode == AE_CTRL_MODE_AP && ae_write_mode == AE_WRITE_MODE_AP)
		return true;
	else
		return false;
}
/* added by y00215412 for hdr movie mode end */

/*
 **************************************************************************
 * FunctionName: deal_uv_data_from_preview;
 * Description : To solve red color clip by adjust the value of Refbin register.
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */

static void deal_uv_data_from_preview(u8 *pdata, u32 preview_width, u32 preview_height)
{
	u32 uv_strt_addr, uv_pixel_addr;
	u8 rect_idx, rect_idx_x, rect_idx_y;
	u32 i, j;
	u32 v_total, u_total;
	u32 v_mean, u_mean;
	u32 uv_count;
	u8 current_refbin;
	u32 first_rect_idx_row;
	u32 first_rect_idx_col;
	u32 rect_row_num;
	u32 rect_col_num;
	u8 uv_resample;
	red_clip_status rect_clip = RED_CLIP_NONE;
	mini_effect_params *effect = get_effect_ptr();
	mini_red_clip_s *this_rcc = &effect->rcc;

	if(preview_width <= this_rcc->preview_width_low)
		uv_resample = this_rcc->uv_resample_low;
	else if(preview_width >= this_rcc->preview_width_high)
		uv_resample = this_rcc->uv_resample_high;
	else
		uv_resample = this_rcc->uv_resample_middle;

	rect_row_num = preview_height * this_rcc->detect_range / (200 * this_rcc->rect_row_num);
	rect_col_num = preview_width * this_rcc->detect_range / (100 * this_rcc->rect_col_num);
	first_rect_idx_row = preview_height * (100 - this_rcc->detect_range) / 400;
	first_rect_idx_col = preview_width  * (100 - this_rcc->detect_range) / 200;

	for(rect_idx = 0; rect_idx < this_rcc->rect_row_num * this_rcc->rect_col_num; rect_idx++){
		rect_idx_x = rect_idx / this_rcc->rect_col_num;
		rect_idx_y = rect_idx % this_rcc->rect_col_num;
		uv_strt_addr = (first_rect_idx_row + rect_row_num * rect_idx_x) * preview_width
						+ first_rect_idx_col+ rect_col_num * rect_idx_y;
		v_total = 0;
		u_total = 0;
		uv_count = 0;

		for (i = 0; i < rect_row_num; i = i + uv_resample) {
			for (j = 0; j < rect_col_num; j = j + uv_resample * 2) {
				uv_pixel_addr = uv_strt_addr + i * preview_width + j;
				v_total += pdata[uv_pixel_addr / 2 * 2];
				u_total += pdata[uv_pixel_addr / 2 * 2 + 1];
				uv_count++;
			}
		}
		v_mean = v_total / uv_count;
		u_mean = u_total / uv_count;
		current_refbin = GETREG8(REG_ISP_REF_BIN);
		/*  ( (v - 128) > (u - 128) * 4  / 3 && (v - 128) > -(u - 128) * 4 / 3 && v > th_high) */
		if ((v_mean >= this_rcc->v_th_high) && (v_mean + 128 / 3 > u_mean * 4 / 3) && (v_mean + u_mean * 4 / 3 > 128 * 7 / 3)) {
			rect_clip = RED_CLIP;
			break;
		}
		/*  ( (v - 128) > (u - 128) && (v - 128) > -(u - 128) && v > th_low)   */
		if((v_mean >= this_rcc->v_th_low) && (v_mean > u_mean )&& (u_mean + v_mean > 256)){
			rect_clip = RED_CLIP_SHADE;
		}
	}

	if (rect_clip == RED_CLIP) {
		SETREG8(REG_ISP_REF_BIN, this_rcc->refbin_low);
	} else if (rect_clip == RED_CLIP_SHADE) {
		print_debug("Refbin register value do not change");
	} else {
		SETREG8(REG_ISP_REF_BIN, this_rcc->refbin_high);
	}

}
/*
 **************************************************************************
 * FunctionName: ispv1_uv_stat_work_func;
 * Description : NA;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : last modified for DTS2013030101733 by j00212990 in 2013-03-04;
 **************************************************************************
 */
static void ispv1_uv_stat_work_func(struct work_struct *work)
{
	u32 preview_width = mini_this_ispdata->pic_attr[STATE_PREVIEW].out_width;
	u32 preview_height = mini_this_ispdata->pic_attr[STATE_PREVIEW].out_height;
	u32 preview_size = preview_width * preview_height;
	mini_camera_frame_buf *frame = mini_this_ispdata->current_frame;

	if (down_trylock(&(mini_this_ispdata->frame_sem)) != 0) {
		print_warn("enter %s, frame_sem already locked", __func__);
		goto error_out2;
	}

	if (NULL == frame) {
		print_error("params error, frame 0x%pK", (void*)frame);
		goto error_out1;
	}

	if ((0 == frame->phyaddr) || (NULL != frame->viraddr)) {
		print_error("params error, frame->phyaddr 0x%pK, frame->viraddr:0x%pK",
			(void*)frame->phyaddr, frame->viraddr);
		goto error_out1;
	}

	frame->viraddr = ioremap_nocache(frame->phyaddr + preview_size, preview_size / 2);
	if (NULL == frame->viraddr) {
		print_error("%s line %d error", __func__, __LINE__);
		goto error_out1;
	}
	deal_uv_data_from_preview((u8 *)frame->viraddr, preview_width, preview_height);
	iounmap(frame->viraddr);
	frame->viraddr = NULL;

error_out1:
	up(&(mini_this_ispdata->frame_sem));

error_out2:
	return;
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_uv_stat_init;
 * Description : NA
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
int mini_ispv1_uv_stat_init(void)
{
	/* init uv stat work queue. */
	uv_stat_work_queue = create_singlethread_workqueue("uv_stat_wq");
	if (!uv_stat_work_queue) {
		print_error("create workqueue is failed in %s function at line#%d\n", __func__, __LINE__);
		return -1;
	}
	INIT_WORK(&uv_stat_work, ispv1_uv_stat_work_func);
	return 0;
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_uv_stat_excute;
 * Description : NA;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
 void mini_ispv1_uv_stat_excute(void)
{
	print_debug("enter %s", __func__);
	queue_work(uv_stat_work_queue, &uv_stat_work);
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_uv_stat_exit;
 * Description : NA;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
void mini_ispv1_uv_stat_exit(void)
{
	print_debug("enter %s", __func__);
	destroy_workqueue(uv_stat_work_queue);
}
/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, begin */
/*
 **************************************************************************
 * FunctionName: mini_ispv1_do_aeag_wq_init;
 * Description : init the aec agc work queue,this workqueue will write sensor
 * Expo Gain reg directly
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
int mini_ispv1_do_aeag_wq_init(void)
{
	/* init uv stat work queue. */
	aeag_work_queue = create_singlethread_workqueue("aeag_wq");
	if (NULL == aeag_work_queue) {
		print_error("create workqueue is failed in %s function at line#%d\n", __func__, __LINE__);
		return -1;
	}
	INIT_WORK(&aeag_work, mini_ispv1_cmd_id_do_ecgc);
	return 0;
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_aeag_excute;
 * Description : add one work to the queue;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
 int  mini_ispv1_aeag_excute(void)
{
	print_debug("enter %s", __func__);
	if (NULL == aeag_work_queue) {
		print_error("excute workqueue is failed in %s function at line#%d\n", __func__, __LINE__);
		return -1;
	}
	queue_work(aeag_work_queue, &aeag_work);
	return 0;
}

/*
 **************************************************************************
 * FunctionName: mini_ispv1_aeag_exit;
 * Description : free the workqueue resource;
 * Input       : NA;
 * Output      : NA;
 * ReturnValue : NA;
 * Other       : NA;
 **************************************************************************
 */
int mini_ispv1_aeag_exit(void)
{
	print_debug("enter %s", __func__);
	if (NULL == aeag_work_queue) {
		print_error("exit workqueue is failed in %s function at line#%d\n", __func__, __LINE__);
		return -1;
	}
	destroy_workqueue(aeag_work_queue);
	return 0;
}

/* Modified  by w00199382 for DTS2013082008650, 2013/9/04, end */
