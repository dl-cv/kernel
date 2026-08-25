// SPDX-License-Identifier: GPL-2.0
/*
 * imx334 driver
 *
 * Copyright (C) 2020 Rockchip Electronics Co., Ltd.
 * V0.0X01.0X03 add enum_frame_interval function.
 * V0.0X01.0X04
 *	1.add parse mclk pinctrl.
 *	2.add set flip ctrl.
 * V0.0X01.0X05 add quick stream on/off
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/mfd/syscon.h>
#include <linux/rk-preisp.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x05)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define IMX334_LINK_FREQ_445		445500000// 891Mbps
#define IMX334_LINK_FREQ_594		594000000// 1188Mbps
#define IMX334_LINK_FREQ_891		891000000// 1782Mbps

#define IMX334_LANES			4

#define PIXEL_RATE_WITH_445M_10BIT	(IMX334_LINK_FREQ_445 * 2 / 10 * 4)
#define PIXEL_RATE_WITH_594M_12BIT	(IMX334_LINK_FREQ_594 * 2 / 12 * 4)
#define PIXEL_RATE_WITH_891M_10BIT	(IMX334_LINK_FREQ_891 * 2 / 10 * 4)
#define PIXEL_RATE_WITH_891M_12BIT	(IMX334_LINK_FREQ_891 * 2 / 12 * 4)

#define IMX334_XVCLK_FREQ_37		37125000
#define IMX334_XVCLK_FREQ_74		74250000

#define CHIP_ID				0x30
#define IMX334_REG_CHIP_ID		0x302c

#define IMX334_REG_CTRL_MODE		0x3000
#define IMX334_REG_GROUP_HOLD		0x3001
#define IMX334_GROUP_HOLD_START		0x01
#define IMX334_GROUP_HOLD_END		0x00
#define IMX334_MODE_SW_STANDBY		0x1
#define IMX334_MODE_STREAMING		0x0

#define IMX334_LF_GAIN_REG_L		0x30E8

#define IMX334_SF1_GAIN_REG_L		0x30EA

#define IMX334_LF_EXPO_REG_H		0x305A
#define IMX334_LF_EXPO_REG_M		0x3059
#define IMX334_LF_EXPO_REG_L		0x3058

#define IMX334_SF1_EXPO_REG_H		0x305E
#define IMX334_SF1_EXPO_REG_M		0x305D
#define IMX334_SF1_EXPO_REG_L		0x305C

#define IMX334_RHS1_REG_H		0x306a
#define IMX334_RHS1_REG_M		0x3069
#define IMX334_RHS1_REG_L		0x3068

#define	IMX334_EXPOSURE_MIN		5
#define	IMX334_EXPOSURE_STEP		1
#define IMX334_VTS_MAX			0xfffff

#define IMX334_REG_GAIN			0x30e8
#define IMX334_GAIN_MIN			0x00
#define IMX334_GAIN_MAX			0xf0
#define IMX334_GAIN_STEP		1
#define IMX334_GAIN_DEFAULT		0x30

#define IMX334_REG_TEST_PATTERN	0x5e00
#define	IMX334_TEST_PATTERN_ENABLE	0x80
#define	IMX334_TEST_PATTERN_DISABLE	0x0

#define IMX334_REG_VTS_H		0x3032
#define IMX334_REG_VTS_M		0x3031
#define IMX334_REG_VTS_L		0x3030

#define IMX334_FETCH_EXP_H(VAL)		(((VAL) >> 16) & 0x0F)
#define IMX334_FETCH_EXP_M(VAL)		(((VAL) >> 8) & 0xFF)
#define IMX334_FETCH_EXP_L(VAL)		((VAL) & 0xFF)

#define IMX334_FETCH_RHS1_H(VAL)	(((VAL) >> 16) & 0x0F)
#define IMX334_FETCH_RHS1_M(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX334_FETCH_RHS1_L(VAL)	((VAL) & 0xFF)

#define IMX334_FETCH_VTS_H(VAL)		(((VAL) >> 16) & 0x0F)
#define IMX334_FETCH_VTS_M(VAL)		(((VAL) >> 8) & 0xFF)
#define IMX334_FETCH_VTS_L(VAL)		((VAL) & 0xFF)

#define IMX334_VREVERSE_REG	0x304f
#define IMX334_HREVERSE_REG	0x304e

#define REG_DELAY			0xFFFE
#define REG_NULL			0xFFFF

#define IMX334_REG_VALUE_08BIT		1
#define IMX334_REG_VALUE_16BIT		2
#define IMX334_REG_VALUE_24BIT		3

#define OF_CAMERA_HDR_MODE		"rockchip,camera-hdr-mode"
#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"
#define OF_IMX334_TRIGGER_MODE		"trigger-mode"
#define OF_IMX334_XVS_PULSE_US		"rockchip,xvs-pulse-us"
#define OF_IMX334_XHS_DUTY_NS		"rockchip,xhs-duty-ns"
#define OF_IMX334_XHS_PERIOD_NS		"rockchip,xhs-period-ns"
#define OF_IMX334_XVS_PERIOD_NS		"rockchip,xvs-period-ns"

#define IMX334_XHS_PERIOD_NS_DEFAULT	14815U
#define IMX334_XHS_DUTY_NS_DEFAULT	200U
#define IMX334_XVS_PERIOD_NS_DEFAULT	33333333U
#define IMX334_XVS_PULSE_US_DEFAULT	15U
#define IMX334_XVS_PULSE_US_MIN		1U
#define IMX334_XVS_PULSE_US_MAX		1000U
#define IMX334_TRIGGER_BURST_COUNT_DEFAULT	3U
#define IMX334_TRIGGER_BURST_COUNT_MAX	8U
#define IMX334_STANDBY_EXIT_US		18000U
#define IMX334_EXPOSURE_OFFSET_NS	1468ULL

#ifndef V4L2_CID_USER_IMX334_BASE
#define V4L2_CID_USER_IMX334_BASE	(V4L2_CID_USER_BASE + 0x10d0)
#endif
#define V4L2_CID_IMX334_OP_MODE		(V4L2_CID_USER_IMX334_BASE + 0x1)
#define V4L2_CID_IMX334_LIGHT_SOURCE_ENABLE \
	(V4L2_CID_USER_IMX334_BASE + 0x2)
#define V4L2_CID_IMX334_LIGHT_SOURCE_ACTIVE_LEVEL \
	(V4L2_CID_USER_IMX334_BASE + 0x3)
#define V4L2_CID_IMX334_LIGHT_SOURCE_ADVANCE_US \
	(V4L2_CID_USER_IMX334_BASE + 0x4)
#define V4L2_CID_IMX334_LIGHT_SOURCE_OFF_DELAY_US \
	(V4L2_CID_USER_IMX334_BASE + 0x5)

#define IMX334_NAME			"imx334"

#define BRL				2200
#define RHS1_MAX			4397 // <2*BRL && 4n+1
#define SHR1_MIN			9

static const char * const imx334_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define IMX334_NUM_SUPPLIES ARRAY_SIZE(imx334_supply_names)

enum imx334_op_mode {
	IMX334_FREE_RUN = 0,
	IMX334_XVS_XHS_ONE_SHOT = 1,
};

struct regval {
	u16 addr;
	u8 val;
};

struct imx334_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *global_reg_list;
	const struct regval *reg_list;
	u32 hdr_mode;
	u32 vclk_freq;
	u32 bpp;
	u32 mipi_freq_idx;
	u32 vc[PAD_MAX];
};

struct imx334 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct gpio_desc	*light_source_gpio;
	struct pwm_device	*xvs_pwm;
	struct pwm_device	*xhs_pwm;
	struct regulator_bulk_data supplies[IMX334_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*op_mode_ctrl;
	struct v4l2_ctrl	*light_source_enable_ctrl;
	struct v4l2_ctrl	*light_source_active_level_ctrl;
	struct v4l2_ctrl	*light_source_advance_us_ctrl;
	struct v4l2_ctrl	*light_source_off_delay_us_ctrl;
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct imx334_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32			cur_vts;
	u32			cur_vclk_freq;
	u32			cur_mipi_freq_idx;
	enum rkmodule_sync_mode sync_mode;
	enum imx334_op_mode active_mode;
	enum imx334_op_mode pending_mode;
	u32 xvs_pulse_us;
	u32 xvs_period_ns;
	u32 xhs_period_ns;
	u32 xhs_duty_ns;
	bool xhs_enabled;
	bool trigger_busy;
	u32 trigger_burst_count;
	u64 trigger_request_count;
	u64 trigger_complete_count;
	u64 trigger_fail_count;
	u64 trigger_busy_count;
	u64 trigger_start_ns;
	u64 trigger_end_ns;
	u64 last_trigger_ns;
	bool light_source_enabled;
	bool light_source_active_level;
	u32 light_source_advance_us;
	u32 light_source_off_delay_us;
};

#define to_imx334(sd) container_of(sd, struct imx334, subdev)

static const struct regval imx334_10_3840x2160_global_regs[] = {
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x300C, 0x5B},// BCWAIT_TIME[7:0]
	{0x300D, 0x40},// CPWAIT_TIME[7:0]
	{0x3050, 0x00},// ADBIT[0]
	{0x316A, 0x7E},// INCKSEL4[1:0]
	{0x319D, 0x00},// MDBIT
	{0x31A1, 0x0F},// XVS/XHS input Hi-Z in slave mode
	{0x3288, 0x21},// -
	{0x328A, 0x02},// -
	{0x3414, 0x05},// -
	{0x3416, 0x18},// -
	{0x341D, 0x01},//
	{0x35AC, 0x0E},// -
	{0x3648, 0x01},// -
	{0x364A, 0x04},// -
	{0x364C, 0x04},// -
	{0x3678, 0x01},// -
	{0x367C, 0x31},// -
	{0x367E, 0x31},// -
	{0x3708, 0x02},// -
	{0x3714, 0x01},// -
	{0x3715, 0x02},// -
	{0x3716, 0x02},// -
	{0x3717, 0x02},// -
	{0x371C, 0x3D},// -
	{0x371D, 0x3F},// -
	{0x372C, 0x00},// -
	{0x372D, 0x00},// -
	{0x372E, 0x46},// -
	{0x372F, 0x00},// -
	{0x3730, 0x89},// -
	{0x3731, 0x00},// -
	{0x3732, 0x08},// -
	{0x3733, 0x01},// -
	{0x3734, 0xFE},// -
	{0x3735, 0x05},// -
	{0x375D, 0x00},// -
	{0x375E, 0x00},// -
	{0x375F, 0x61},// -
	{0x3760, 0x06},// -
	{0x3768, 0x1B},// -
	{0x3769, 0x1B},// -
	{0x376A, 0x1A},// -
	{0x376B, 0x19},// -
	{0x376C, 0x18},// -
	{0x376D, 0x14},// -
	{0x376E, 0x0F},// -
	{0x3776, 0x00},// -
	{0x3777, 0x00},// -
	{0x3778, 0x46},// -
	{0x3779, 0x00},// -
	{0x377A, 0x08},// -
	{0x377B, 0x01},// -
	{0x377C, 0x45},// -
	{0x377D, 0x01},// -
	{0x377E, 0x23},// -
	{0x377F, 0x02},// -
	{0x3780, 0xD9},// -
	{0x3781, 0x03},// -
	{0x3782, 0xF5},// -
	{0x3783, 0x06},// -
	{0x3784, 0xA5},// -
	{0x3788, 0x0F},// -
	{0x378A, 0xD9},// -
	{0x378B, 0x03},// -
	{0x378C, 0xEB},// -
	{0x378D, 0x05},// -
	{0x378E, 0x87},// -
	{0x378F, 0x06},// -
	{0x3790, 0xF5},// -
	{0x3792, 0x43},// -
	{0x3794, 0x7A},// -
	{0x3796, 0xA1},// -
	{0x3E04, 0x0E},// -
	{REG_NULL, 0x00},
};

/*
 *IMX334LQR All-pixel scan CSI-2_4lane 37.125Mhz
 *AD:10bit Output:10bit 891Mbps Master Mode 30fps
 *Tool ver : Ver4.0
 */
static const struct regval imx334_linear_10_3840x2160_regs[] = {
	{0x302E, 0x18},
	{0x302F, 0x0f},
	{0x3030, 0xCA},// VMAX[19:0]
	{0x3031, 0x08},//
	{0x3034, 0x4c},
	{0x3035, 0x04},
	{0x3048, 0x00},// WDMODE[0]
	{0x3049, 0x00},// WDSEL[1:0]
	{0x304A, 0x00},// WD_SET1[2:0]
	{0x304B, 0x01},// WD_SET2[3:0]
	{0x304C, 0x14},// OPB_SIZE_V[5:0]
	{0x3058, 0x05},// SHR0[19:0]
	{0x3059, 0x00},//
	{0x3068, 0x8B},// RHS1[19:0]
	{0x3069, 0x00},//{
	{0x3076, 0x84},
	{0x3077, 0x08},
	{0x315a, 0x06},
	{0x319e, 0x02},
	{0x31D7, 0x00},// XVSMSKCNT_INT[1:0]
	{0x3200, 0x11},// FGAINEN[0]
	{0x341C, 0x47},// ADBIT1[8:0]
	{0x3a18, 0x7f},
	{0x3a1a, 0x37},
	{0x3a1c, 0x37},
	{0x3a1e, 0xf7},
	{0x3a1f, 0x00},
	{0x3a20, 0x3f},
	{0x3a22, 0x6f},
	{0x3a24, 0x3f},
	{0x3a26, 0x5f},
	{0x3a28, 0x2f},
	{REG_NULL, 0x00},
};

/*
 *All-pixel scan CSI-2_4lane 37.125Mhz
 *AD:10bit Output:10bit 1782Mbps Master Mode DOL HDR 2frame VC
 *Tool ver : Ver3.0
 */



/*
 *IMX334LQR All-pixel scan CSI-2_4lane 37.125Mhz
 *AD:12bit Output:12bit 1188Mbps Master Mode 30fps
 *Tool ver : Ver4.0
 */


/*
 *All-pixel scan CSI-2_4lane 74.25Mhz
 *AD:12bit Output:12bit 1782Mbps Master Mode DOL HDR 2frame VC
 *Tool ver : Ver3.0
 */


static const struct imx334_mode supported_modes[] = {
	{
		.width = 3864,
		.height = 2180,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0600,
		.hts_def = 0x044C * 4,
		.vts_def = 0x08CA,
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.global_reg_list = imx334_10_3840x2160_global_regs,
		.reg_list = imx334_linear_10_3840x2160_regs,
		.hdr_mode = NO_HDR,
		.vclk_freq = IMX334_XVCLK_FREQ_37,
		.bpp = 10,
		.mipi_freq_idx = 0,
		.vc[PAD0] = 0,
	},
};

static const s64 link_freq_menu_items[] = {
	IMX334_LINK_FREQ_445,
	IMX334_LINK_FREQ_594,
	IMX334_LINK_FREQ_891,
};

static const char * const imx334_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* Write registers up to 4 at a time */
static int imx334_write_reg(struct i2c_client *client, u16 reg,
			    int len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int imx334_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		if (unlikely(regs[i].addr == REG_DELAY))
			usleep_range(regs[i].val, regs[i].val * 2);
		else
			ret = imx334_write_reg(client, regs[i].addr,
					       IMX334_REG_VALUE_08BIT,
					       regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int imx334_read_reg(struct i2c_client *client, u16 reg, unsigned int len,
			   u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret, i;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
		if (ret == ARRAY_SIZE(msgs))
			break;
	}
	if (ret != ARRAY_SIZE(msgs) && i == 3)
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

static int imx334_mode_switch(struct imx334 *imx334,
			      enum imx334_op_mode new_mode);
static int __imx334_start_stream(struct imx334 *imx334, bool setup_controls);
static int __imx334_stop_stream(struct imx334 *imx334);
static u32 imx334_exposure_us_to_lines(struct imx334 *imx334,
					u32 exposure_us);
static u32 imx334_lines_to_exposure_us(struct imx334 *imx334, u32 lines);

static const char *imx334_op_mode_name(enum imx334_op_mode mode)
{
	switch (mode) {
	case IMX334_FREE_RUN:
		return "free_run";
	case IMX334_XVS_XHS_ONE_SHOT:
		return "xvs_xhs_one_shot";
	default:
		return "unknown";
	}
}

static int imx334_parse_run_mode(const char *buf, enum imx334_op_mode *mode)
{
	if (sysfs_streq(buf, "free_run") || sysfs_streq(buf, "normal") ||
	    sysfs_streq(buf, "continuous") || sysfs_streq(buf, "0")) {
		*mode = IMX334_FREE_RUN;
		return 0;
	}

	if (sysfs_streq(buf, "xvs_xhs_one_shot") ||
	    sysfs_streq(buf, "master_fast_trigger") ||
	    sysfs_streq(buf, "fast_trigger") || sysfs_streq(buf, "trigger") ||
	    sysfs_streq(buf, "1")) {
		*mode = IMX334_XVS_XHS_ONE_SHOT;
		return 0;
	}

	return -EINVAL;
}

static struct pwm_device *imx334_devm_pwm_get_optional(struct device *dev,
						       const char *con_id)
{
	struct pwm_device *pwm;
	int ret;

	pwm = devm_pwm_get(dev, con_id);
	if (IS_ERR(pwm)) {
		ret = PTR_ERR(pwm);
		if (ret == -ENOENT || ret == -ENODEV || ret == -EINVAL)
			return NULL;
	}

	return pwm;
}

static int imx334_apply_pwm(struct pwm_device *pwm, u64 period_ns,
			    u64 duty_ns, bool enable)
{
	struct pwm_state state;
	struct pwm_args args;

	if (!pwm)
		return -ENODEV;
	if (!period_ns || duty_ns >= period_ns)
		return -EINVAL;

	pwm_get_state(pwm, &state);
	pwm_get_args(pwm, &args);
	state.period = period_ns;
	state.duty_cycle = enable ? duty_ns : 0;
	state.polarity = args.polarity;
	state.enabled = enable;
	return pwm_apply_state(pwm, &state);
}

static int imx334_set_xhs_locked(struct imx334 *imx334, bool enable)
{
	int ret;

	ret = imx334_apply_pwm(imx334->xhs_pwm, imx334->xhs_period_ns,
			       imx334->xhs_duty_ns, enable);
	if (!ret)
		imx334->xhs_enabled = enable;
	return ret;
}

static int imx334_set_continuous_xvs_locked(struct imx334 *imx334, bool enable)
{
	return imx334_apply_pwm(imx334->xvs_pwm, imx334->xvs_period_ns,
				(u64)imx334->xvs_pulse_us * 1000ULL, enable);
}

static int imx334_stop_sync_locked(struct imx334 *imx334)
{
	int ret = 0;
	int tmp;

	if (imx334->xvs_pwm) {
		tmp = imx334_set_continuous_xvs_locked(imx334, false);
		if (tmp && !ret)
			ret = tmp;
	}
	if (imx334->xhs_pwm) {
		tmp = imx334_set_xhs_locked(imx334, false);
		if (tmp && !ret)
			ret = tmp;
	}
	return ret;
}

static int imx334_light_source_value_locked(struct imx334 *imx334, bool on)
{
	return on ? imx334->light_source_active_level :
		    !imx334->light_source_active_level;
}

static void imx334_light_source_set_locked(struct imx334 *imx334, bool on)
{
	if (imx334->light_source_gpio)
		gpiod_set_value_cansleep(imx334->light_source_gpio,
			imx334_light_source_value_locked(imx334, on));
}

static int imx334_light_source_init_locked(struct imx334 *imx334)
{
	if (!imx334->light_source_gpio)
		return 0;

	return gpiod_direction_output(imx334->light_source_gpio,
		imx334_light_source_value_locked(imx334, false));
}

static int imx334_trigger_once_locked(struct imx334 *imx334)
{
	u64 frame_ns;
	u64 request_seq;
	u32 frame_wait_us;
	u32 pulse_wait_us;
	u32 light_hold_us = 0;
	u32 i;
	int ret = 0;

	if (!imx334->streaming || imx334->active_mode != IMX334_XVS_XHS_ONE_SHOT)
		return -EBUSY;
	if (!imx334->xvs_pwm || !imx334->xhs_pwm)
		return -ENODEV;
	if (imx334->trigger_busy) {
		imx334->trigger_busy_count++;
		return -EBUSY;
	}

	frame_ns = imx334->xvs_period_ns;
	frame_wait_us = DIV_ROUND_UP_ULL(frame_ns, 1000ULL);
	pulse_wait_us = imx334->xvs_pulse_us + 50;
	imx334->trigger_busy = true;
	request_seq = ++imx334->trigger_request_count;
	imx334->trigger_start_ns = ktime_get_ns();

	if (imx334->light_source_enabled && imx334->light_source_gpio) {
		imx334_light_source_set_locked(imx334, true);
		if (imx334->light_source_advance_us)
			usleep_range(imx334->light_source_advance_us,
				     imx334->light_source_advance_us + 50);
	}

	for (i = 0; i < imx334->trigger_burst_count; i++) {
		u64 pulse_start_ns = ktime_get_ns();
		u64 elapsed_us;
		u32 remaining_us;

		ret = imx334_apply_pwm(imx334->xvs_pwm, frame_ns,
				       (u64)imx334->xvs_pulse_us * 1000ULL, true);
		if (ret)
			break;

		usleep_range(pulse_wait_us, pulse_wait_us + 50);
		ret = imx334_set_continuous_xvs_locked(imx334, false);
		if (ret)
			break;

		dev_dbg(&imx334->client->dev,
			"trigger request=%llu pulse=%u/%u at=%llu\n",
			request_seq, i + 1, imx334->trigger_burst_count,
			pulse_start_ns);

		if (i + 1 == imx334->trigger_burst_count)
			continue;

		elapsed_us = div_u64(ktime_get_ns() - pulse_start_ns, 1000ULL);
		remaining_us = elapsed_us < frame_wait_us ?
			frame_wait_us - elapsed_us : 1;
		usleep_range(remaining_us, remaining_us + 100);
	}

	if (!ret) {
		imx334->last_trigger_ns = ktime_get_ns();
		imx334->trigger_end_ns = imx334->last_trigger_ns;
		imx334->trigger_complete_count++;
		light_hold_us = imx334_lines_to_exposure_us(imx334,
			imx334_exposure_us_to_lines(imx334, imx334->exposure->val));
		dev_info(&imx334->client->dev,
			 "trigger request=%llu complete burst=%u duration_us=%llu\n",
			 request_seq, imx334->trigger_burst_count,
			 div_u64(imx334->trigger_end_ns - imx334->trigger_start_ns,
				 1000ULL));
	} else {
		imx334_set_continuous_xvs_locked(imx334, false);
		imx334->trigger_end_ns = ktime_get_ns();
		imx334->trigger_fail_count++;
		dev_err(&imx334->client->dev,
			"trigger request=%llu failed pulse=%u/%u ret=%d\n",
			request_seq, i + 1, imx334->trigger_burst_count, ret);
	}

	if (imx334->light_source_enabled && imx334->light_source_gpio) {
		if (!ret && light_hold_us)
			usleep_range(light_hold_us, light_hold_us + 100);
		if (imx334->light_source_off_delay_us)
			usleep_range(imx334->light_source_off_delay_us,
				     imx334->light_source_off_delay_us + 50);
		imx334_light_source_set_locked(imx334, false);
	}
	imx334->trigger_busy = false;
	return ret;
}

static ssize_t run_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	ssize_t len;

	mutex_lock(&imx334->mutex);
	len = sysfs_emit(buf, "pending=%s\nactive=%s\nstreaming=%u\navailable=free_run xvs_xhs_one_shot\n",
			 imx334_op_mode_name(imx334->pending_mode),
			 imx334_op_mode_name(imx334->active_mode), imx334->streaming);
	mutex_unlock(&imx334->mutex);
	return len;
}

static ssize_t run_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	enum imx334_op_mode mode;
	int ret;

	ret = imx334_parse_run_mode(buf, &mode);
	if (ret)
		return ret;

	/*
	 * Keep the V4L2 operation-mode control as the canonical value.  Stream-on
	 * reapplies cached controls, so changing only pending_mode here would be
	 * overwritten by the old control value during v4l2_ctrl_handler_setup().
	 */
	if (imx334->op_mode_ctrl)
		ret = v4l2_ctrl_s_ctrl(imx334->op_mode_ctrl, mode);
	else {
		mutex_lock(&imx334->mutex);
		ret = imx334_mode_switch(imx334, mode);
		mutex_unlock(&imx334->mutex);
	}

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(run_mode);

static ssize_t trigger_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	ssize_t len;

	mutex_lock(&imx334->mutex);
	len = sysfs_emit(buf,
		"available=echo 1 > trigger\n"
		"pulse_us=%u\n"
		"xhs_period_ns=%u\n"
		"xvs_period_ns=%u\n"
		"burst_count=%u\n"
		"busy=%u\n"
		"requests=%llu\n"
		"completed=%llu\n"
		"failed=%llu\n"
		"busy_rejected=%llu\n"
		"last_start_ns=%llu\n"
		"last_end_ns=%llu\n"
		"mode=%s\n"
		"streaming=%u\n",
		imx334->xvs_pulse_us, imx334->xhs_period_ns,
		imx334->xvs_period_ns, imx334->trigger_burst_count,
		imx334->trigger_busy, imx334->trigger_request_count,
		imx334->trigger_complete_count, imx334->trigger_fail_count,
		imx334->trigger_busy_count, imx334->trigger_start_ns,
		imx334->trigger_end_ns, imx334_op_mode_name(imx334->active_mode),
		imx334->streaming);
	mutex_unlock(&imx334->mutex);
	return len;
}

static ssize_t trigger_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;
	if (!value)
		return count;
	mutex_lock(&imx334->mutex);
	ret = imx334_trigger_once_locked(imx334);
	mutex_unlock(&imx334->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(trigger);

static ssize_t trigger_pulse_us_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	ssize_t len;

	mutex_lock(&imx334->mutex);
	len = sysfs_emit(buf, "%u\n", imx334->xvs_pulse_us);
	mutex_unlock(&imx334->mutex);
	return len;
}

static ssize_t trigger_pulse_us_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	unsigned int pulse_us;
	int ret;

	ret = kstrtouint(buf, 0, &pulse_us);
	if (ret)
		return ret;
	if (pulse_us < IMX334_XVS_PULSE_US_MIN ||
	    pulse_us > IMX334_XVS_PULSE_US_MAX ||
	    (u64)pulse_us * 1000ULL >= imx334->xvs_period_ns)
		return -ERANGE;
	mutex_lock(&imx334->mutex);
	imx334->xvs_pulse_us = pulse_us;
	if (imx334->streaming && imx334->active_mode == IMX334_FREE_RUN)
		ret = imx334_set_continuous_xvs_locked(imx334, true);
	mutex_unlock(&imx334->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(trigger_pulse_us);

static ssize_t trigger_burst_count_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	ssize_t len;

	mutex_lock(&imx334->mutex);
	len = sysfs_emit(buf, "%u\n", imx334->trigger_burst_count);
	mutex_unlock(&imx334->mutex);
	return len;
}

static ssize_t trigger_burst_count_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct imx334 *imx334 = to_imx334(sd);
	unsigned int burst_count;
	int ret;

	ret = kstrtouint(buf, 0, &burst_count);
	if (ret)
		return ret;
	if (burst_count < 1 || burst_count > IMX334_TRIGGER_BURST_COUNT_MAX)
		return -ERANGE;

	mutex_lock(&imx334->mutex);
	if (imx334->trigger_busy)
		ret = -EBUSY;
	else
		imx334->trigger_burst_count = burst_count;
	mutex_unlock(&imx334->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(trigger_burst_count);

static struct attribute *imx334_attrs[] = {
	&dev_attr_run_mode.attr,
	&dev_attr_trigger.attr,
	&dev_attr_trigger_pulse_us.attr,
	&dev_attr_trigger_burst_count.attr,
	NULL,
};

static const struct attribute_group imx334_attr_group = {
	.attrs = imx334_attrs,
};

static int imx334_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx334 *imx334 = to_imx334(sd);
	const struct imx334_mode *mode;
	s64 h_blank, vblank_def;
	s64 dst_pixel_rate = 0;
	int ret = 0;

	mutex_lock(&imx334->mutex);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx334->mutex);
		return -ENOTTY;
#endif
	} else {
		imx334->cur_mode = mode;
		imx334->cur_vts = imx334->cur_mode->vts_def;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx334->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx334->vblank, vblank_def,
					 IMX334_VTS_MAX - mode->height,
					 1, vblank_def);
		if (imx334->cur_vclk_freq != mode->vclk_freq) {
			if (imx334->xvclk) {
				clk_disable_unprepare(imx334->xvclk);
				ret = clk_set_rate(imx334->xvclk, mode->vclk_freq);
				ret |= clk_prepare_enable(imx334->xvclk);
				if (ret < 0) {
					dev_err(&imx334->client->dev, "Failed to enable xvclk\n");
					mutex_unlock(&imx334->mutex);
					return ret;
				}
			}
			imx334->cur_vclk_freq = mode->vclk_freq;
		}
		if (imx334->cur_mipi_freq_idx != mode->mipi_freq_idx) {
			dst_pixel_rate = ((u32)link_freq_menu_items[mode->mipi_freq_idx]) /
				mode->bpp * 2 * IMX334_LANES;
			__v4l2_ctrl_s_ctrl_int64(imx334->pixel_rate,
						 dst_pixel_rate);
			__v4l2_ctrl_s_ctrl(imx334->link_freq,
					   mode->mipi_freq_idx);
			imx334->cur_mipi_freq_idx = mode->mipi_freq_idx;
		}
	}
	mutex_unlock(&imx334->mutex);
	return 0;
}

static int imx334_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx334 *imx334 = to_imx334(sd);
	const struct imx334_mode *mode = imx334->cur_mode;

	mutex_lock(&imx334->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
#else
		mutex_unlock(&imx334->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
		/* format info: width/height/data type/virctual channel */
		if (fmt->pad < PAD_MAX && mode->hdr_mode != NO_HDR)
			fmt->reserved[0] = mode->vc[fmt->pad];
		else
			fmt->reserved[0] = mode->vc[PAD0];
	}
	mutex_unlock(&imx334->mutex);

	return 0;
}

static int imx334_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx334 *imx334 = to_imx334(sd);

	if (code->index != 0)
		return -EINVAL;
	code->code = imx334->cur_mode->bus_fmt;

	return 0;
}

static int imx334_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != supported_modes[0].bus_fmt)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int imx334_enable_test_pattern(struct imx334 *imx334, u32 pattern)
{
	u32 val;

	if (pattern)
		val = (pattern - 1) | IMX334_TEST_PATTERN_ENABLE;
	else
		val = IMX334_TEST_PATTERN_DISABLE;

	return imx334_write_reg(imx334->client,
				IMX334_REG_TEST_PATTERN,
				IMX334_REG_VALUE_08BIT,
				val);
}

static int imx334_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx334 *imx334 = to_imx334(sd);
	const struct imx334_mode *mode = imx334->cur_mode;

	fi->interval = mode->max_fps;

	return 0;
}

static int imx334_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	config->bus.mipi_csi2.num_data_lanes = IMX334_LANES;
	config->type = V4L2_MBUS_CSI2_DPHY;
	return 0;
}

static void imx334_get_module_inf(struct imx334 *imx334,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, IMX334_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, imx334->module_name,
		sizeof(inf->base.module));
	strscpy(inf->base.lens, imx334->len_name, sizeof(inf->base.lens));
}

static int imx334_quick_stream_locked(struct imx334 *imx334, bool on)
{
	int ret;

	if (on) {
		if (imx334->streaming)
			return 0;
		ret = pm_runtime_resume_and_get(&imx334->client->dev);
		if (ret < 0)
			return ret;
		ret = __imx334_start_stream(imx334, true);
		if (ret) {
			pm_runtime_put(&imx334->client->dev);
			return ret;
		}
		imx334->streaming = true;
		return 0;
	}

	if (!imx334->streaming)
		return 0;
	ret = __imx334_stop_stream(imx334);
	pm_runtime_put(&imx334->client->dev);
	if (!ret)
		imx334->streaming = false;
	return ret;
}

static long imx334_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx334 *imx334 = to_imx334(sd);
	struct rkmodule_hdr_cfg *hdr;
	long ret = 0;
	u32 stream = 0;
	u32 sync_mode;

	switch (cmd) {
	case PREISP_CMD_SET_HDRAE_EXP:
		ret = -EINVAL;
		break;
	case RKMODULE_GET_MODULE_INFO:
		imx334_get_module_inf(imx334, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		hdr->esp.mode = HDR_NORMAL_VC;
		hdr->hdr_mode = imx334->cur_mode->hdr_mode;
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		if (hdr->hdr_mode != NO_HDR)
			ret = -EINVAL;
		break;
	case RKMODULE_GET_SYNC_MODE:
		/*
		 * This ioctl describes Rockchip CIF multi-camera group sync, not the
		 * IMX334 XMASTER electrical role.  This board has one externally timed
		 * sensor, so attaching it as a lone CIF SLAVE leaves the group without
		 * an internal master and causes every SOF to be rejected.
		 */
		*((u32 *)arg) = NO_SYNC_MODE;
		break;
	case RKMODULE_SET_SYNC_MODE:
		sync_mode = *((u32 *)arg);
		if (sync_mode != NO_SYNC_MODE)
			ret = -EINVAL;
		else
			imx334->sync_mode = NO_SYNC_MODE;
		break;
	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);
		mutex_lock(&imx334->mutex);
		ret = imx334_quick_stream_locked(imx334, !!stream);
		mutex_unlock(&imx334->mutex);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long imx334_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_awb_cfg *cfg;
	struct rkmodule_hdr_cfg *hdr;
	struct preisp_hdrae_exp_s *hdrae;
	long ret;
	u32 stream = 0;
	u32 sync_mode = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx334_ioctl(sd, cmd, inf);
		if (!ret)
			ret = copy_to_user(up, inf, sizeof(*inf));
		kfree(inf);
		break;
	case RKMODULE_AWB_CFG:
		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(cfg, up, sizeof(*cfg));
		if (!ret)
			ret = imx334_ioctl(sd, cmd, cfg);
		kfree(cfg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		ret = imx334_ioctl(sd, cmd, hdr);
		if (!ret)
			ret = copy_to_user(up, hdr, sizeof(*hdr));
		kfree(hdr);
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(hdr, up, sizeof(*hdr));
		if (!ret)
			ret = imx334_ioctl(sd, cmd, hdr);
		kfree(hdr);
		break;
	case PREISP_CMD_SET_HDRAE_EXP:
		hdrae = kzalloc(sizeof(*hdrae), GFP_KERNEL);
		if (!hdrae) {
			ret = -ENOMEM;
			return ret;
		}

		ret = copy_from_user(hdrae, up, sizeof(*hdrae));
		if (!ret)
			ret = imx334_ioctl(sd, cmd, hdrae);
		kfree(hdrae);
		break;
	case RKMODULE_GET_SYNC_MODE:
		ret = imx334_ioctl(sd, cmd, &sync_mode);
		if (!ret)
			ret = copy_to_user(up, &sync_mode, sizeof(sync_mode));
		break;
	case RKMODULE_SET_SYNC_MODE:
		ret = copy_from_user(&sync_mode, up, sizeof(sync_mode));
		if (!ret)
			ret = imx334_ioctl(sd, cmd, &sync_mode);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		ret = copy_from_user(&stream, up, sizeof(u32));
		if (!ret)
			ret = imx334_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __imx334_start_stream(struct imx334 *imx334, bool setup_controls)
{
	int ret;

	ret = imx334_write_array(imx334->client, imx334->cur_mode->global_reg_list);
	if (ret)
		return ret;
	ret = imx334_write_array(imx334->client, imx334->cur_mode->reg_list);
	if (ret)
		return ret;

	if (setup_controls) {
		mutex_unlock(&imx334->mutex);
		ret = v4l2_ctrl_handler_setup(&imx334->ctrl_handler);
		mutex_lock(&imx334->mutex);
		if (ret)
			return ret;
	}

	ret = imx334_set_xhs_locked(imx334, true);
	if (ret)
		return ret;

	ret = imx334_write_reg(imx334->client, IMX334_REG_CTRL_MODE,
			       IMX334_REG_VALUE_08BIT, IMX334_MODE_STREAMING);
	if (ret)
		goto err_sync;

	usleep_range(IMX334_STANDBY_EXIT_US, IMX334_STANDBY_EXIT_US + 1000);
	if (imx334->pending_mode == IMX334_FREE_RUN) {
		ret = imx334_set_continuous_xvs_locked(imx334, true);
		if (ret)
			goto err_standby;
	}

	imx334->active_mode = imx334->pending_mode;
	imx334->last_trigger_ns = 0;
	return 0;

err_standby:
	imx334_write_reg(imx334->client, IMX334_REG_CTRL_MODE,
			 IMX334_REG_VALUE_08BIT, IMX334_MODE_SW_STANDBY);
err_sync:
	imx334_stop_sync_locked(imx334);
	return ret;
}

static int __imx334_stop_stream(struct imx334 *imx334)
{
	int ret;
	int sync_ret;

	imx334->trigger_busy = false;
	imx334_light_source_set_locked(imx334, false);
	sync_ret = imx334_stop_sync_locked(imx334);
	ret = imx334_write_reg(imx334->client, IMX334_REG_CTRL_MODE,
			       IMX334_REG_VALUE_08BIT, IMX334_MODE_SW_STANDBY);
	return ret ? ret : sync_ret;
}

static int imx334_mode_switch(struct imx334 *imx334,
			      enum imx334_op_mode new_mode)
{
	int ret;

	if (new_mode > IMX334_XVS_XHS_ONE_SHOT)
		return -EINVAL;
	if (imx334->pending_mode == new_mode &&
	    (!imx334->streaming || imx334->active_mode == new_mode))
		return 0;

	imx334->pending_mode = new_mode;
	if (!imx334->streaming)
		return 0;

	ret = __imx334_stop_stream(imx334);
	if (ret)
		return ret;
	imx334->streaming = false;
	ret = __imx334_start_stream(imx334, false);
	if (!ret)
		imx334->streaming = true;
	return ret;
}

static int imx334_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx334 *imx334 = to_imx334(sd);
	struct i2c_client *client = imx334->client;
	int ret = 0;

	mutex_lock(&imx334->mutex);
	on = !!on;
	if (on == imx334->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __imx334_start_stream(imx334, true);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		ret = __imx334_stop_stream(imx334);
		pm_runtime_put(&client->dev);
		if (ret)
			goto unlock_and_return;
	}

	imx334->streaming = on;

unlock_and_return:
	mutex_unlock(&imx334->mutex);

	return ret;
}

static int imx334_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx334 *imx334 = to_imx334(sd);
	struct i2c_client *client = imx334->client;
	int ret = 0;

	mutex_lock(&imx334->mutex);

	/* If the power state is not modified - no work to do. */
	if (imx334->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		imx334->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx334->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&imx334->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 imx334_cal_delay(u32 cycles, struct imx334 *imx334)
{
	if (imx334->cur_mode->vclk_freq == IMX334_XVCLK_FREQ_37)
		return DIV_ROUND_UP(cycles, IMX334_XVCLK_FREQ_37 / 1000 / 1000);
	else
		return DIV_ROUND_UP(cycles, IMX334_XVCLK_FREQ_74 / 1000 / 1000);
}

static int __imx334_power_on(struct imx334 *imx334)
{
	int ret;
	u32 delay_us;
	s64 vclk_freq;
	struct device *dev = &imx334->client->dev;

	if (!IS_ERR_OR_NULL(imx334->pins_default)) {
		ret = pinctrl_select_state(imx334->pinctrl,
					   imx334->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	if (imx334->cur_mode->vclk_freq == IMX334_XVCLK_FREQ_37)
		vclk_freq = IMX334_XVCLK_FREQ_37;
	else
		vclk_freq = IMX334_XVCLK_FREQ_74;

	if (imx334->xvclk) {
		ret = clk_set_rate(imx334->xvclk, vclk_freq);
		if (ret < 0) {
			dev_err(dev, "Failed to set xvclk rate\n");
			return ret;
		}
		if (clk_get_rate(imx334->xvclk) != vclk_freq)
			dev_warn(dev, "xvclk mismatched, modes are based on 37.125MHz\n");
		ret = clk_prepare_enable(imx334->xvclk);
		if (ret < 0) {
			dev_err(dev, "Failed to enable xvclk\n");
			return ret;
		}
	}

	if (!IS_ERR_OR_NULL(imx334->reset_gpio))
		gpiod_set_value_cansleep(imx334->reset_gpio, 1);

	ret = regulator_bulk_enable(IMX334_NUM_SUPPLIES, imx334->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	usleep_range(500, 1000);
	if (!IS_ERR_OR_NULL(imx334->reset_gpio))
		gpiod_set_value_cansleep(imx334->reset_gpio, 0);

	if (!IS_ERR_OR_NULL(imx334->pwdn_gpio))
		gpiod_set_value_cansleep(imx334->pwdn_gpio, 1);

	/* Allow the on-module oscillator and serial interface to settle. */
	delay_us = max_t(u32, imx334_cal_delay(8192, imx334), 2000);
	usleep_range(delay_us, delay_us * 2);

	return 0;

disable_clk:
	if (imx334->xvclk)
		clk_disable_unprepare(imx334->xvclk);

	return ret;
}

static void __imx334_power_off(struct imx334 *imx334)
{
	imx334_stop_sync_locked(imx334);
	imx334_light_source_set_locked(imx334, false);
	if (!IS_ERR_OR_NULL(imx334->pwdn_gpio))
		gpiod_set_value_cansleep(imx334->pwdn_gpio, 0);
	if (imx334->xvclk)
		clk_disable_unprepare(imx334->xvclk);
	if (!IS_ERR_OR_NULL(imx334->reset_gpio))
		gpiod_set_value_cansleep(imx334->reset_gpio, 1);
	regulator_bulk_disable(IMX334_NUM_SUPPLIES, imx334->supplies);
}

static int imx334_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx334 *imx334 = to_imx334(sd);

	return __imx334_power_on(imx334);
}

static int imx334_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx334 *imx334 = to_imx334(sd);

	__imx334_power_off(imx334);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx334_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx334 *imx334 = to_imx334(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->state, 0);
	const struct imx334_mode *def_mode = &supported_modes[0];

	mutex_lock(&imx334->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx334->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int imx334_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;

	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	fie->reserved[0] = supported_modes[fie->index].hdr_mode;
	return 0;
}

#define CROP_START(SRC, DST) (((SRC) - (DST)) / 2 / 4 * 4)
#define DST_WIDTH 3840
#define DST_HEIGHT 2160

static int imx334_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx334 *imx334 = to_imx334(sd);

	if (sel->target == V4L2_SEL_TGT_CROP_BOUNDS) {
		sel->r.left = CROP_START(imx334->cur_mode->width, DST_WIDTH);
		sel->r.width = DST_WIDTH;
		sel->r.top = CROP_START(imx334->cur_mode->height, DST_HEIGHT);
		sel->r.height = DST_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct dev_pm_ops imx334_pm_ops = {
	SET_RUNTIME_PM_OPS(imx334_runtime_suspend,
			   imx334_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx334_internal_ops = {
	.open = imx334_open,
};
#endif

static const struct v4l2_subdev_core_ops imx334_core_ops = {
	.s_power = imx334_s_power,
	.ioctl = imx334_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx334_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx334_video_ops = {
	.s_stream = imx334_s_stream,
	.g_frame_interval = imx334_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx334_pad_ops = {
	.enum_mbus_code = imx334_enum_mbus_code,
	.enum_frame_size = imx334_enum_frame_sizes,
	.enum_frame_interval = imx334_enum_frame_interval,
	.get_fmt = imx334_get_fmt,
	.set_fmt = imx334_set_fmt,
	.get_selection = imx334_get_selection,
	.get_mbus_config = imx334_g_mbus_config,
};

static const struct v4l2_subdev_ops imx334_subdev_ops = {
	.core	= &imx334_core_ops,
	.video	= &imx334_video_ops,
	.pad	= &imx334_pad_ops,
};

static const struct v4l2_ctrl_ops imx334_ctrl_ops;

static u32 imx334_exposure_us_to_lines(struct imx334 *imx334, u32 exposure_us)
{
	u64 request_ns = (u64)exposure_us * 1000ULL;
	u32 max_lines = imx334->cur_vts - IMX334_EXPOSURE_MIN;

	if (request_ns <= IMX334_EXPOSURE_OFFSET_NS)
		return 1;
	return clamp_t(u32,
		DIV_ROUND_CLOSEST_ULL(request_ns - IMX334_EXPOSURE_OFFSET_NS,
				      imx334->xhs_period_ns),
		1, max_lines);
}

static u32 imx334_lines_to_exposure_us(struct imx334 *imx334, u32 lines)
{
	return DIV_ROUND_CLOSEST_ULL((u64)lines * imx334->xhs_period_ns +
				     IMX334_EXPOSURE_OFFSET_NS, 1000);
}

static const char * const imx334_op_mode_menu[] = {
	"FREE_RUN",
	"XVS_XHS_ONE_SHOT",
};

static const struct v4l2_ctrl_config imx334_op_mode_ctrl_cfg = {
	.ops = &imx334_ctrl_ops,
	.id = V4L2_CID_IMX334_OP_MODE,
	.name = "operation_mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = IMX334_FREE_RUN,
	.max = IMX334_XVS_XHS_ONE_SHOT,
	.def = IMX334_FREE_RUN,
	.qmenu = imx334_op_mode_menu,
};

#define IMX334_BOOL_CTRL(_id, _name) \
	{ .ops = &imx334_ctrl_ops, .id = (_id), .name = (_name), \
	  .type = V4L2_CTRL_TYPE_BOOLEAN, .min = 0, .max = 1, .step = 1 }
#define IMX334_INT_CTRL(_id, _name, _max) \
	{ .ops = &imx334_ctrl_ops, .id = (_id), .name = (_name), \
	  .type = V4L2_CTRL_TYPE_INTEGER, .min = 0, .max = (_max), .step = 1 }

static const struct v4l2_ctrl_config imx334_light_source_enable_cfg =
	IMX334_BOOL_CTRL(V4L2_CID_IMX334_LIGHT_SOURCE_ENABLE,
			 "light_source_enable");
static const struct v4l2_ctrl_config imx334_light_source_active_level_cfg =
	IMX334_BOOL_CTRL(V4L2_CID_IMX334_LIGHT_SOURCE_ACTIVE_LEVEL,
			 "light_source_active_level");
static const struct v4l2_ctrl_config imx334_light_source_advance_us_cfg =
	IMX334_INT_CTRL(V4L2_CID_IMX334_LIGHT_SOURCE_ADVANCE_US,
			"light_source_advance_us", 100000);
static const struct v4l2_ctrl_config imx334_light_source_off_delay_us_cfg =
	IMX334_INT_CTRL(V4L2_CID_IMX334_LIGHT_SOURCE_OFF_DELAY_US,
			"light_source_off_delay_us", 1000000);

static int imx334_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx334 *imx334 = container_of(ctrl->handler,
					     struct imx334, ctrl_handler);
	struct i2c_client *client = imx334->client;
	s64 max;
	int ret = 0;
	u32 exposure_lines;
	u32 shr0 = 0;
	u32 vts = 0;
	u32 flip = 0;

	if (ctrl->id == V4L2_CID_IMX334_OP_MODE)
		return imx334_mode_switch(imx334, ctrl->val);

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		max = imx334_lines_to_exposure_us(imx334,
			imx334->cur_mode->height + ctrl->val - IMX334_EXPOSURE_MIN);
		__v4l2_ctrl_modify_range(imx334->exposure,
					 imx334->exposure->minimum, max,
					 imx334->exposure->step,
					 min_t(s64, imx334->exposure->default_value, max));
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ENABLE:
		imx334->light_source_enabled = !!ctrl->val;
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ACTIVE_LEVEL:
		imx334->light_source_active_level = !!ctrl->val;
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ADVANCE_US:
		imx334->light_source_advance_us = ctrl->val;
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_OFF_DELAY_US:
		imx334->light_source_off_delay_us = ctrl->val;
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		exposure_lines = imx334_exposure_us_to_lines(imx334, ctrl->val);
		shr0 = imx334->cur_vts - exposure_lines;
		ret = imx334_write_reg(imx334->client, IMX334_REG_GROUP_HOLD,
				       IMX334_REG_VALUE_08BIT, IMX334_GROUP_HOLD_START);
		ret |= imx334_write_reg(imx334->client,
					IMX334_LF_EXPO_REG_H,
					IMX334_REG_VALUE_08BIT,
					IMX334_FETCH_EXP_H(shr0));
		ret |= imx334_write_reg(imx334->client,
					IMX334_LF_EXPO_REG_M,
					IMX334_REG_VALUE_08BIT,
					IMX334_FETCH_EXP_M(shr0));
		ret |= imx334_write_reg(imx334->client,
					IMX334_LF_EXPO_REG_L,
					IMX334_REG_VALUE_08BIT,
					IMX334_FETCH_EXP_L(shr0));
		ret |= imx334_write_reg(imx334->client, IMX334_REG_GROUP_HOLD,
					IMX334_REG_VALUE_08BIT, IMX334_GROUP_HOLD_END);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx334_write_reg(imx334->client,
				       IMX334_REG_GAIN,
				       IMX334_REG_VALUE_08BIT, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		vts = ctrl->val + imx334->cur_mode->height;
		/*
		 * vts of hdr mode is double to correct T-line calculation.
		 * Restore before write to reg.
		 */
		if (imx334->cur_mode->hdr_mode == HDR_X2) {
			vts = ((vts + 3) >> 2) * 4;
			imx334->cur_vts = vts;
			vts = vts >> 1;
		} else {
			imx334->cur_vts = vts;
		}
		ret = imx334_write_reg(imx334->client,
				       IMX334_REG_VTS_H,
				       IMX334_REG_VALUE_08BIT,
				       IMX334_FETCH_VTS_H(vts));
		ret |= imx334_write_reg(imx334->client,
					IMX334_REG_VTS_M,
					IMX334_REG_VALUE_08BIT,
					IMX334_FETCH_VTS_M(vts));
		ret |= imx334_write_reg(imx334->client,
					IMX334_REG_VTS_L,
					IMX334_REG_VALUE_08BIT,
					IMX334_FETCH_VTS_L(vts));
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx334_enable_test_pattern(imx334, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = imx334_write_reg(imx334->client, IMX334_HREVERSE_REG,
				       IMX334_REG_VALUE_08BIT, !!ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		flip = ctrl->val;
		if (flip) {
			ret = imx334_write_reg(imx334->client, IMX334_VREVERSE_REG,
				IMX334_REG_VALUE_08BIT, !!flip);
			ret |= imx334_write_reg(imx334->client, 0x3080,
				IMX334_REG_VALUE_08BIT, 0xfe);
			ret |= imx334_write_reg(imx334->client, 0x309b,
				IMX334_REG_VALUE_08BIT, 0xfe);
		} else {
			ret = imx334_write_reg(imx334->client, IMX334_VREVERSE_REG,
				IMX334_REG_VALUE_08BIT, !!flip);
			ret |= imx334_write_reg(imx334->client, 0x3080,
				IMX334_REG_VALUE_08BIT, 0x02);
			ret |= imx334_write_reg(imx334->client, 0x309b,
				IMX334_REG_VALUE_08BIT, 0x02);
		}
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ENABLE:
		if (imx334->streaming && imx334->active_mode == IMX334_FREE_RUN)
			imx334_light_source_set_locked(imx334,
						 imx334->light_source_enabled);
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ACTIVE_LEVEL:
		ret = imx334_light_source_init_locked(imx334);
		if (!ret && imx334->streaming &&
		    imx334->active_mode == IMX334_FREE_RUN)
			imx334_light_source_set_locked(imx334,
						 imx334->light_source_enabled);
		break;
	case V4L2_CID_IMX334_LIGHT_SOURCE_ADVANCE_US:
	case V4L2_CID_IMX334_LIGHT_SOURCE_OFF_DELAY_US:
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx334_ctrl_ops = {
	.s_ctrl = imx334_set_ctrl,
};

static int imx334_initialize_controls(struct imx334 *imx334)
{
	const struct imx334_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;
	s64 dst_pixel_rate = 0;

	handler = &imx334->ctrl_handler;
	mode = imx334->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 14);
	if (ret)
		return ret;
	handler->lock = &imx334->mutex;

	imx334->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
						   V4L2_CID_LINK_FREQ,
						   0, 0, link_freq_menu_items);
	if (imx334->link_freq)
		imx334->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	dst_pixel_rate = ((u32)link_freq_menu_items[mode->mipi_freq_idx]) /
		mode->bpp * 2 * IMX334_LANES;

	imx334->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
					       V4L2_CID_PIXEL_RATE,
					       0, PIXEL_RATE_WITH_891M_10BIT,
					       1, dst_pixel_rate);
	if (imx334->pixel_rate)
		imx334->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_s_ctrl(imx334->link_freq,
			 mode->mipi_freq_idx);
	imx334->cur_mipi_freq_idx = mode->mipi_freq_idx;
	imx334->cur_vclk_freq = mode->vclk_freq;

	h_blank = mode->hts_def - mode->width;
	imx334->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					   h_blank, h_blank, 1, h_blank);
	if (imx334->hblank)
		imx334->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	imx334->vblank = v4l2_ctrl_new_std(handler, &imx334_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_def,
					   IMX334_VTS_MAX - mode->height,
					   1, vblank_def);
	imx334->cur_vts = mode->vts_def;
	exposure_max = imx334_lines_to_exposure_us(imx334,
						 mode->vts_def - IMX334_EXPOSURE_MIN);
	imx334->exposure = v4l2_ctrl_new_std(handler, &imx334_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     1, exposure_max,
					     IMX334_EXPOSURE_STEP,
					     imx334_lines_to_exposure_us(imx334,
								      mode->exp_def));

	imx334->anal_gain = v4l2_ctrl_new_std(handler, &imx334_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      IMX334_GAIN_MIN,
					      IMX334_GAIN_MAX,
					      IMX334_GAIN_STEP,
					      IMX334_GAIN_DEFAULT);

	imx334->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
							    &imx334_ctrl_ops,
				V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx334_test_pattern_menu) - 1,
				0, 0, imx334_test_pattern_menu);

	v4l2_ctrl_new_std(handler, &imx334_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(handler, &imx334_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);

	imx334->op_mode_ctrl = v4l2_ctrl_new_custom(handler,
						    &imx334_op_mode_ctrl_cfg, NULL);
	imx334->light_source_enable_ctrl = v4l2_ctrl_new_custom(
		handler, &imx334_light_source_enable_cfg, NULL);
	imx334->light_source_active_level_ctrl = v4l2_ctrl_new_custom(
		handler, &imx334_light_source_active_level_cfg, NULL);
	imx334->light_source_advance_us_ctrl = v4l2_ctrl_new_custom(
		handler, &imx334_light_source_advance_us_cfg, NULL);
	imx334->light_source_off_delay_us_ctrl = v4l2_ctrl_new_custom(
		handler, &imx334_light_source_off_delay_us_cfg, NULL);

	if (handler->error) {
		ret = handler->error;
		dev_err(&imx334->client->dev,
			"Failed to init controls(  %d  )\n", ret);
		goto err_free_handler;
	}

	if (imx334->op_mode_ctrl) {
		ret = __v4l2_ctrl_s_ctrl(imx334->op_mode_ctrl,
					   imx334->pending_mode);
		if (ret)
			goto err_free_handler;
	}
	if (imx334->light_source_active_level_ctrl)
		__v4l2_ctrl_s_ctrl(imx334->light_source_active_level_ctrl,
				     imx334->light_source_active_level);
	if (imx334->light_source_advance_us_ctrl)
		__v4l2_ctrl_s_ctrl(imx334->light_source_advance_us_ctrl,
				     imx334->light_source_advance_us);
	if (imx334->light_source_off_delay_us_ctrl)
		__v4l2_ctrl_s_ctrl(imx334->light_source_off_delay_us_ctrl,
				     imx334->light_source_off_delay_us);

	imx334->subdev.ctrl_handler = handler;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int imx334_check_sensor_id(struct imx334 *imx334,
				  struct i2c_client *client)
{
	struct device *dev = &imx334->client->dev;
	u32 id = 0;
	int ret, i;

	for (i = 0; i < 10; i++) {
		ret = imx334_read_reg(client, IMX334_REG_CHIP_ID,
				      IMX334_REG_VALUE_08BIT, &id);
		if (id == CHIP_ID)
			break;
	}

	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		usleep_range(2000, 4000);
		return -ENODEV;
	}

	dev_info(dev, "Detected imx334 id:%06x\n", CHIP_ID);

	return 0;
}

static int imx334_configure_regulators(struct imx334 *imx334)
{
	unsigned int i;

	for (i = 0; i < IMX334_NUM_SUPPLIES; i++)
		imx334->supplies[i].supply = imx334_supply_names[i];

	return devm_regulator_bulk_get(&imx334->client->dev,
				       IMX334_NUM_SUPPLIES,
				       imx334->supplies);
}

static int imx334_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx334 *imx334;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;
	u32 i, hdr_mode = 0;
	u32 trigger_mode = IMX334_FREE_RUN;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff);

	imx334 = devm_kzalloc(dev, sizeof(*imx334), GFP_KERNEL);
	if (!imx334)
		return -ENOMEM;

	of_property_read_u32(node, OF_CAMERA_HDR_MODE, &hdr_mode);
	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx334->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx334->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx334->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx334->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	imx334->client = client;
	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		if (hdr_mode == supported_modes[i].hdr_mode) {
			imx334->cur_mode = &supported_modes[i];
			break;
		}
	}
	if (i == ARRAY_SIZE(supported_modes))
		imx334->cur_mode = &supported_modes[0];

	imx334->sync_mode = NO_SYNC_MODE;
	imx334->pending_mode = IMX334_FREE_RUN;
	imx334->active_mode = IMX334_FREE_RUN;
	imx334->xvs_pulse_us = IMX334_XVS_PULSE_US_DEFAULT;
	imx334->xvs_period_ns = IMX334_XVS_PERIOD_NS_DEFAULT;
	imx334->xhs_period_ns = IMX334_XHS_PERIOD_NS_DEFAULT;
	imx334->xhs_duty_ns = IMX334_XHS_DUTY_NS_DEFAULT;
	imx334->trigger_burst_count = IMX334_TRIGGER_BURST_COUNT_DEFAULT;

	if (!of_property_read_u32(node, OF_IMX334_TRIGGER_MODE, &trigger_mode) &&
	    trigger_mode <= IMX334_XVS_XHS_ONE_SHOT)
		imx334->pending_mode = trigger_mode;
	imx334->active_mode = imx334->pending_mode;
	of_property_read_u32(node, OF_IMX334_XVS_PULSE_US,
			     &imx334->xvs_pulse_us);
	of_property_read_u32(node, OF_IMX334_XVS_PERIOD_NS,
			     &imx334->xvs_period_ns);
	of_property_read_u32(node, OF_IMX334_XHS_PERIOD_NS,
			     &imx334->xhs_period_ns);
	of_property_read_u32(node, OF_IMX334_XHS_DUTY_NS,
			     &imx334->xhs_duty_ns);
	if (imx334->xvs_pulse_us < IMX334_XVS_PULSE_US_MIN ||
	    imx334->xvs_pulse_us > IMX334_XVS_PULSE_US_MAX ||
	    (u64)imx334->xvs_pulse_us * 1000ULL >= imx334->xvs_period_ns ||
	    !imx334->xhs_period_ns || !imx334->xhs_duty_ns ||
	    imx334->xhs_duty_ns >= imx334->xhs_period_ns) {
		dev_err(dev, "invalid XVS/XHS timing properties\n");
		return -EINVAL;
	}

	imx334->xvclk = devm_clk_get_optional(dev, "inck");
	if (IS_ERR(imx334->xvclk))
		return dev_err_probe(dev, PTR_ERR(imx334->xvclk),
				     "failed to get inck\n");
	if (!imx334->xvclk) {
		imx334->xvclk = devm_clk_get_optional(dev, "xvclk");
		if (IS_ERR(imx334->xvclk))
			return dev_err_probe(dev, PTR_ERR(imx334->xvclk),
					     "failed to get xvclk\n");
	}
	if (!imx334->xvclk)
		dev_info(dev, "using on-module 37.125 MHz oscillator\n");

	imx334->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx334->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx334->reset_gpio),
				     "failed to get reset-gpios\n");

	imx334->pwdn_gpio = devm_gpiod_get_optional(dev, "pwdn",
						    GPIOD_OUT_LOW);
	if (IS_ERR(imx334->pwdn_gpio))
		return dev_err_probe(dev, PTR_ERR(imx334->pwdn_gpio),
				     "failed to get pwdn-gpios\n");

	imx334->xvs_pwm = imx334_devm_pwm_get_optional(dev, "xvs");
	if (IS_ERR(imx334->xvs_pwm))
		return dev_err_probe(dev, PTR_ERR(imx334->xvs_pwm),
				     "failed to get XVS PWM\n");
	imx334->xhs_pwm = imx334_devm_pwm_get_optional(dev, "xhs");
	if (IS_ERR(imx334->xhs_pwm))
		return dev_err_probe(dev, PTR_ERR(imx334->xhs_pwm),
				     "failed to get XHS PWM\n");
	if (!imx334->xvs_pwm || !imx334->xhs_pwm)
		return dev_err_probe(dev, -ENODEV,
				     "both XVS and XHS PWMs are required\n");

	imx334->light_source_gpio = devm_gpiod_get_optional(dev, "light-source",
							    GPIOD_ASIS);
	if (IS_ERR(imx334->light_source_gpio))
		return dev_err_probe(dev, PTR_ERR(imx334->light_source_gpio),
				     "failed to get light-source-gpios\n");
	{
		const char *active_level;

		if (!of_property_read_string(node, "light-source-active-level",
					     &active_level))
			imx334->light_source_active_level =
				!strcmp(active_level, "high");
	}
	of_property_read_u32(node, "light-source-exposure-advance-us",
			     &imx334->light_source_advance_us);
	of_property_read_u32(node, "light-source-exposure-off-delay-us",
			     &imx334->light_source_off_delay_us);
	imx334->light_source_advance_us =
		min(imx334->light_source_advance_us, 100000U);
	imx334->light_source_off_delay_us =
		min(imx334->light_source_off_delay_us, 1000000U);

	imx334->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx334->pinctrl)) {
		imx334->pins_default =
			pinctrl_lookup_state(imx334->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx334->pins_default))
			dev_info(dev, "could not get default pinstate\n");

		imx334->pins_sleep =
			pinctrl_lookup_state(imx334->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx334->pins_sleep))
			dev_info(dev, "could not get sleep pinstate\n");
	} else {
		dev_info(dev, "no pinctrl\n");
	}

	ret = imx334_configure_regulators(imx334);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	mutex_init(&imx334->mutex);
	ret = imx334_stop_sync_locked(imx334);
	if (ret)
		goto err_destroy_mutex;
	ret = imx334_light_source_init_locked(imx334);
	if (ret)
		goto err_destroy_mutex;

	sd = &imx334->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx334_subdev_ops);

	ret = imx334_initialize_controls(imx334);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx334_power_on(imx334);
	if (ret)
		goto err_free_handler;

	ret = imx334_check_sensor_id(imx334, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &imx334_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	imx334->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx334->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx334->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx334->module_index, facing,
		 IMX334_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	ret = devm_device_add_group(dev, &imx334_attr_group);
	if (ret)
		goto err_unregister_subdev;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_unregister_subdev:
	v4l2_async_unregister_subdev(sd);
err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx334_power_off(imx334);
err_free_handler:
	v4l2_ctrl_handler_free(&imx334->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx334->mutex);

	return ret;
}

static void imx334_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx334 *imx334 = to_imx334(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx334->ctrl_handler);
	mutex_destroy(&imx334->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx334_power_off(imx334);
	pm_runtime_set_suspended(&client->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx334_of_match[] = {
	{ .compatible = "sony,imx334" },
	{},
};
MODULE_DEVICE_TABLE(of, imx334_of_match);
#endif

static const struct i2c_device_id imx334_match_id[] = {
	{ "sony,imx334", 0 },
	{ },
};

static struct i2c_driver imx334_i2c_driver = {
	.driver = {
		.name = IMX334_NAME,
		.pm = &imx334_pm_ops,
		.of_match_table = of_match_ptr(imx334_of_match),
	},
	.probe		= &imx334_probe,
	.remove		= &imx334_remove,
	.id_table	= imx334_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx334_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx334_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony imx334 sensor driver");
MODULE_LICENSE("GPL v2");
