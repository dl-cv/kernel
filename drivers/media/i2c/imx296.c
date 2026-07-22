// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX296 sensor driver
 *
 * Copyright 2019 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 * Copyright 2026 OpenAI
 */

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/videodev2.h>

#include <linux/rk-camera-module.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define DRIVER_VERSION				KERNEL_VERSION(0, 0x01, 0x00)

#define IMX296_NAME				"imx296"
#define IMX296_PIXEL_ARRAY_WIDTH		1456
#define IMX296_PIXEL_ARRAY_HEIGHT		1088
#define IMX296_PIXEL_RATE			(1188000000ULL / 10)
#define IMX296_NUM_DATA_LANES			1U
#define IMX296_BITS_PER_SAMPLE			10U
#define IMX296_LINK_FREQ			(IMX296_PIXEL_RATE * \
						 IMX296_BITS_PER_SAMPLE / \
						 (2 * IMX296_NUM_DATA_LANES))

#define IMX296_REG_8BIT(n)			((1 << 16) | (n))
#define IMX296_REG_16BIT(n)			((2 << 16) | (n))
#define IMX296_REG_24BIT(n)			((3 << 16) | (n))
#define IMX296_REG_SIZE_SHIFT			16
#define IMX296_REG_ADDR_MASK			0xffff

#define IMX296_CTRL00				IMX296_REG_8BIT(0x3000)
#define IMX296_CTRL00_STANDBY			BIT(0)
#define IMX296_CTRL08				IMX296_REG_8BIT(0x3008)
#define IMX296_CTRL08_REGHOLD			BIT(0)
#define IMX296_CTRL0A				IMX296_REG_8BIT(0x300a)
#define IMX296_CTRL0A_XMSTA			BIT(0)
#define IMX296_CTRL0B				IMX296_REG_8BIT(0x300b)
#define IMX296_CTRL0B_TRIGEN			BIT(0)
#define IMX296_CTRL0D				IMX296_REG_8BIT(0x300d)
#define IMX296_CTRL0D_WINMODE_FD_BINNING	(2 << 0)
#define IMX296_CTRL0D_HADD_ON_BINNING		BIT(5)
#define IMX296_CTRL0E				IMX296_REG_8BIT(0x300e)
#define IMX296_CTRL0E_VREVERSE			BIT(0)
#define IMX296_CTRL0E_HREVERSE			BIT(1)

#define IMX296_VMAX				IMX296_REG_24BIT(0x3010)
#define IMX296_HMAX				IMX296_REG_16BIT(0x3014)
#define IMX296_TMDCTRL				IMX296_REG_8BIT(0x301d)
#define IMX296_TMDCTRL_LATCH			BIT(0)
#define IMX296_TMDOUT				IMX296_REG_16BIT(0x301e)
#define IMX296_TMDOUT_MASK			0x3ff

#define IMX296_BLKLEVELAUTO			IMX296_REG_8BIT(0x3022)
#define IMX296_BLKLEVELAUTO_ON			0x01
#define IMX296_BLKLEVELAUTO_OFF			0xf0
#define IMX296_CTRLTOUT				IMX296_REG_8BIT(0x3026)
#define IMX296_CTRLTOUT_TOUT1SEL_LOW		(0 << 0)
#define IMX296_CTRLTOUT_TOUT2SEL_LOW		(0 << 2)
#define IMX296_CTRLTRIG				IMX296_REG_8BIT(0x3029)
#define IMX296_CTRLTRIG_TOUT1_SEL_LOW		(0 << 0)
#define IMX296_CTRLTRIG_TOUT2_SEL_LOW		(0 << 4)
#define IMX296_SYNCSEL				IMX296_REG_8BIT(0x3036)
#define IMX296_SYNCSEL_NORMAL			0xc0
#define IMX296_SYNCSEL_HIZ			0xf0

#define IMX296_PULSE1				IMX296_REG_8BIT(0x306d)
#define IMX296_PULSE2				IMX296_REG_8BIT(0x3079)

#define IMX296_INCKSEL(n)			IMX296_REG_8BIT(0x3089 + (n))
#define IMX296_SHS1				IMX296_REG_24BIT(0x308d)
#define IMX296_VINT				IMX296_REG_8BIT(0x30aa)
#define IMX296_VINT_EN				BIT(0)
#define IMX296_LOWLAGTRG			IMX296_REG_8BIT(0x30ae)
#define IMX296_LOWLAGTRG_FAST			BIT(0)

#define IMX296_SENSOR_INFO			IMX296_REG_16BIT(0x3148)
#define IMX296_SENSOR_INFO_MONO			BIT(15)
#define IMX296_SENSOR_INFO_IMX296LQ		0x4a00
#define IMX296_SENSOR_INFO_IMX296LL		0xca00

#define IMX296_GTTABLENUM			IMX296_REG_8BIT(0x4114)
#define IMX296_MIPIC_AREA3W			IMX296_REG_16BIT(0x4182)
#define IMX296_CTRL418C				IMX296_REG_8BIT(0x418c)

#define IMX296_GAIN				IMX296_REG_16BIT(0x3204)
#define IMX296_GAINDLY				IMX296_REG_8BIT(0x3212)
#define IMX296_GAINDLY_0FRAME			0x08
#define IMX296_GAINDLY_1FRAME			0x09
#define IMX296_BLKLEVEL				IMX296_REG_16BIT(0x3254)

#define IMX296_PGCTRL				IMX296_REG_8BIT(0x3238)
#define IMX296_PGCTRL_REGEN			BIT(0)
#define IMX296_PGCTRL_CLKEN			BIT(2)
#define IMX296_PGCTRL_MODE(n)			((n) << 3)
#define IMX296_PGHPOS				IMX296_REG_16BIT(0x3239)
#define IMX296_PGVPOS				IMX296_REG_16BIT(0x323c)
#define IMX296_PGHPSTEP				IMX296_REG_8BIT(0x323e)
#define IMX296_PGVPSTEP				IMX296_REG_8BIT(0x323f)
#define IMX296_PGHPNUM				IMX296_REG_8BIT(0x3240)
#define IMX296_PGVPNUM				IMX296_REG_8BIT(0x3241)
#define IMX296_PGDATA1				IMX296_REG_16BIT(0x3244)
#define IMX296_PGDATA2				IMX296_REG_16BIT(0x3246)
#define IMX296_PGHGSTEP				IMX296_REG_8BIT(0x3249)

#define IMX296_FID0_ROI				IMX296_REG_8BIT(0x3300)
#define IMX296_FID0_ROIH1ON			BIT(0)
#define IMX296_FID0_ROIV1ON			BIT(1)
#define IMX296_FID0_ROIPH1			IMX296_REG_16BIT(0x3310)
#define IMX296_FID0_ROIPV1			IMX296_REG_16BIT(0x3312)
#define IMX296_FID0_ROIWH1			IMX296_REG_16BIT(0x3314)
#define IMX296_FID0_ROIWH1_MIN			96
#define IMX296_FID0_ROIWV1			IMX296_REG_16BIT(0x3316)
#define IMX296_FID0_ROIWV1_MIN			88

/*
 * VBLANK 决定帧率：fps = OP_CLK / (HMAX * (height + vblank))
 * OP_CLK=74.25MHz, HMAX=1100, height=1088
 * 默认 vblank=1162 对应 30fps
 * 常用对应关系：60fps->37, 50fps->262, 30fps->1162, 25fps->1612, 15fps->3412
 */
#define IMX296_HMAX_DEFAULT			1100U
#define IMX296_VBLANK_DEFAULT			1162U
/*
 * Fast Trigger does not use the 30 fps free-run blanking budget. Sony's
 * timing requirement is VTR = ROIWV1 + 30 for vertical ROI, otherwise the
 * ROI frame stays open until the next XTRIG and vb2_done is delayed one shot.
 */
#define IMX296_XTRIG_VBLANK			30U
#define IMX296_VBLANK_MAX			(1048575U - IMX296_PIXEL_ARRAY_HEIGHT)
#define IMX296_EXPOSURE_DEFAULT_LINES		1104U
#define IMX296_ANALOG_GAIN_MIN			0U
#define IMX296_ANALOG_GAIN_MAX			240U
#define IMX296_ANALOG_GAIN_STEP			1U
#define IMX296_MIN_MEMORY_WAIT_LINES		4U
#define IMX296_OP_CLK_FREQ_HZ			74250000ULL
#define IMX296_EXPOSURE_OFFSET_NS		14260ULL
#define OF_CAMERA_PINCTRL_STATE_DEFAULT		"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP		"rockchip,camera_sleep"
#define OF_IMX296_TRIGGER_MODE			"trigger-mode"
#define OF_IMX296_TRIGGER_PULSE_US		"rockchip,trigger-pulse-us"
#define IMX296_TRIGGER_PULSE_US_DEFAULT		5U
#define IMX296_TRIGGER_PULSE_US_MIN		1U
#define IMX296_TRIGGER_PULSE_US_MAX		1000000U
#define IMX296_TRIGGER_PERIOD_NS_DEFAULT	10000000ULL
#define IMX296_TRIGGER_PERIOD_MARGIN_NS		10000000ULL

#ifndef V4L2_CID_USER_IMX296_BASE
#define V4L2_CID_USER_IMX296_BASE		(V4L2_CID_USER_BASE + 0x10d0)
#endif
#define V4L2_CID_IMX296_OP_MODE			(V4L2_CID_USER_IMX296_BASE + 0x1)
#define V4L2_CID_IMX296_LIGHT_SOURCE_ENABLE		(V4L2_CID_USER_IMX296_BASE + 0x2)
#define V4L2_CID_IMX296_LIGHT_SOURCE_ACTIVE_LEVEL	(V4L2_CID_USER_IMX296_BASE + 0x3)
#define V4L2_CID_IMX296_LIGHT_SOURCE_ADVANCE_US		(V4L2_CID_USER_IMX296_BASE + 0x4)
#define V4L2_CID_IMX296_LIGHT_SOURCE_OFF_DELAY_US	(V4L2_CID_USER_IMX296_BASE + 0x5)

/* Datasheet: changing ROI geometry produces one invalid output frame. */
#define IMX296_ROI_INVALID_FRAME_WAIT_US	50000U

enum imx296_op_mode {
	IMX296_FREE_RUN = 0,
	IMX296_XTRIG_ONE_SHOT = 1,
};

/*
 * RKISP sits behind the RKCIF bridge on this platform, so its active media
 * entity is the CIF subdevice rather than the IMX296 subdevice.  Keep a small
 * read-only mode bridge for RKISP's fast-trigger early-done decision.  The
 * board has a single IMX296; atomic access also makes the IRQ/start paths
 * independent of the sensor mutex.
 */
static atomic_t imx296_fast_trigger_mode = ATOMIC_INIT(0);

bool imx296_is_fast_trigger_active(void)
{
	return atomic_read(&imx296_fast_trigger_mode) != 0;
}
EXPORT_SYMBOL_GPL(imx296_is_fast_trigger_active);

struct imx296_clk_params {
	unsigned int freq;
	u8 incksel[4];
	u8 ctrl418c;
};

static const struct imx296_clk_params imx296_clk_params[] = {
	{ 37125000, { 0x80, 0x0b, 0x80, 0x08 }, 116 },
	{ 54000000, { 0xb0, 0x0f, 0xb0, 0x0c }, 168 },
	{ 74250000, { 0x80, 0x0f, 0x80, 0x0c }, 232 },
};

static const char * const imx296_supply_names[] = {
	"dvdd",
	"ovdd",
	"avdd",
};

struct imx296 {
	struct device *dev;
	struct i2c_client *client;
	struct clk *clk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx296_supply_names)];
	struct gpio_desc *reset_gpio;
	struct pwm_device *trigger_pwm;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_sleep;
	struct regmap *regmap;
	struct mutex mutex;

	const struct imx296_clk_params *clk_params;
	bool mono;
	bool streaming;
	bool power_on;

	enum rkmodule_sync_mode sync_mode;
	enum imx296_op_mode active_mode;
	enum imx296_op_mode pending_mode;
	u32 trigger_pulse_us;

	struct gpio_desc *light_source_gpio;
	struct pinctrl_state *pins_active_high;
	bool light_source_enabled;
	bool light_source_active_level;
	u32 light_source_advance_us;
	u32 light_source_off_delay_us;

	struct v4l2_ctrl *light_source_enable_ctrl;
	struct v4l2_ctrl *light_source_active_level_ctrl;
	struct v4l2_ctrl *light_source_advance_us_ctrl;
	struct v4l2_ctrl *light_source_off_delay_us_ctrl;

	u32 module_index;
	const char *module_facing;
	const char *module_name;
	const char *len_name;

	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_rect crop;
	/* Set when ACTIVE crop changes; cleared after ROI invalid-frame discard. */
	bool roi_boundary_pending;
	struct v4l2_mbus_framefmt format;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *anal_gain;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *test_pattern;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *op_mode_ctrl;
};

static inline struct imx296 *to_imx296(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx296, subdev);
}

static const char * const imx296_test_pattern_menu[] = {
	"Disabled",
	"Multiple Pixels",
	"Sequence 1",
	"Sequence 2",
	"Gradient",
	"Row",
	"Column",
	"Cross",
	"Stripe",
	"Checks",
};

static const char * const imx296_op_mode_menu[] = {
	"FREE_RUN",
	"XTRIG_ONE_SHOT",
};

static const s64 imx296_link_freq_menu[] = {
	IMX296_LINK_FREQ,
};

static const char *imx296_op_mode_name(enum imx296_op_mode mode)
{
	switch (mode) {
	case IMX296_FREE_RUN:
		return "free_run";
	case IMX296_XTRIG_ONE_SHOT:
		return "master_fast_trigger";
	default:
		return "unknown";
	}
}

static const char *imx296_sync_mode_name(enum rkmodule_sync_mode mode)
{
	switch (mode) {
	case INTERNAL_MASTER_MODE:
		return "INTERNAL_MASTER";
	case EXTERNAL_MASTER_MODE:
		return "EXTERNAL_MASTER";
	case SLAVE_MODE:
		return "SLAVE";
	case NO_SYNC_MODE:
	default:
		return "NO_SYNC";
	}
}

static int imx296_parse_run_mode(const char *buf, enum imx296_op_mode *mode)
{
	if (sysfs_streq(buf, "free_run") ||
	    sysfs_streq(buf, "normal") ||
	    sysfs_streq(buf, "continuous") ||
	    sysfs_streq(buf, "0")) {
		*mode = IMX296_FREE_RUN;
		return 0;
	}

	if (sysfs_streq(buf, "master_fast_trigger") ||
	    sysfs_streq(buf, "fast_trigger") ||
	    sysfs_streq(buf, "xtrig_one_shot") ||
	    sysfs_streq(buf, "trigger") ||
	    sysfs_streq(buf, "1")) {
		*mode = IMX296_XTRIG_ONE_SHOT;
		return 0;
	}

	return -EINVAL;
}

static struct imx296 *imx296_from_dev(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	if (!sd)
		return NULL;

	return to_imx296(sd);
}

static struct pwm_device *imx296_devm_pwm_get_optional(struct device *dev,
						       const char *con_id)
{
	struct pwm_device *pwm;
	int ret;

	pwm = devm_pwm_get(dev, con_id);
	if (IS_ERR(pwm)) {
		ret = PTR_ERR(pwm);
		if (ret == -ENOENT || ret == -ENODEV || ret == -EINVAL)
			return NULL;
		return pwm;
	}

	return pwm;
}

static u64 imx296_pwm_period_ns(struct pwm_device *pwm)
{
	struct pwm_state state;
	struct pwm_args args;

	if (!pwm)
		return 0;

	pwm_get_state(pwm, &state);
	if (state.period)
		return state.period;

	pwm_get_args(pwm, &args);
	return args.period;
}

static u64 imx296_trigger_period_ns(u64 pulse_ns, u64 base_period_ns)
{
	u64 min_period_ns = pulse_ns + IMX296_TRIGGER_PERIOD_MARGIN_NS;

	if (base_period_ns < IMX296_TRIGGER_PERIOD_NS_DEFAULT)
		base_period_ns = IMX296_TRIGGER_PERIOD_NS_DEFAULT;

	return max(base_period_ns, min_period_ns);
}

static int imx296_init_trigger_pwm(struct imx296 *sensor)
{
	struct pwm_state state = { 0 };
	u64 pulse_ns;

	if (!sensor->trigger_pwm)
		return 0;

	pulse_ns = (u64)sensor->trigger_pulse_us * 1000ULL;
	state.period = imx296_trigger_period_ns(
		pulse_ns, imx296_pwm_period_ns(sensor->trigger_pwm));
	state.duty_cycle = 0;
	state.polarity = PWM_POLARITY_INVERSED;
	state.enabled = false;

	return pwm_apply_state(sensor->trigger_pwm, &state);
}

static int imx296_light_source_value_locked(struct imx296 *sensor, bool on)
{
	/*
	 * active_level: false = low active, true = high active.
	 * Low active:  on -> GPIO 0, off -> GPIO 1.
	 * High active: on -> GPIO 1, off -> GPIO 0.
	 */
	return on ? sensor->light_source_active_level : !sensor->light_source_active_level;
}

static int imx296_light_source_apply_pinctrl_locked(struct imx296 *sensor)
{
	struct pinctrl_state *state;

	if (!sensor->pinctrl)
		return 0;

	state = sensor->light_source_active_level ? sensor->pins_active_high
						  : sensor->pins_default;
	if (!state)
		return 0;

	return pinctrl_select_state(sensor->pinctrl, state);
}

static void imx296_light_source_set_locked(struct imx296 *sensor, bool on)
{
	if (!sensor->light_source_gpio)
		return;

	gpiod_set_value(sensor->light_source_gpio,
			imx296_light_source_value_locked(sensor, on));
}

static int imx296_light_source_init_locked(struct imx296 *sensor)
{
	int idle_value;
	int ret;

	if (!sensor->light_source_gpio)
		return 0;

	ret = imx296_light_source_apply_pinctrl_locked(sensor);
	if (ret < 0) {
		dev_warn(sensor->dev,
			 "failed to select light source pinctrl state (%d)\n", ret);
		return ret;
	}

	idle_value = imx296_light_source_value_locked(sensor, false);
	ret = gpiod_direction_output(sensor->light_source_gpio, idle_value);
	if (ret < 0) {
		dev_err(sensor->dev,
			"failed to set light source gpio direction (%d)\n", ret);
		return ret;
	}

	/*
	 * gpiod_direction_output() only guarantees the initial value when the
	 * line changes from input to output. After that, some gpiochip drivers
	 * skip updating the value if the direction is already output, so set the
	 * idle level explicitly here.
	 */
	imx296_light_source_set_locked(sensor, false);

	return 0;
}

static int imx296_trigger_once_locked(struct imx296 *sensor)
{
	struct pwm_state state;
	u64 duty_ns;
	u32 pulse_us;
	u32 advance_us;
	u32 off_delay_us;
	int ret;

	if (!sensor->trigger_pwm)
		return -ENODEV;

	pulse_us = clamp_t(u32, sensor->trigger_pulse_us,
			   IMX296_TRIGGER_PULSE_US_MIN,
			   IMX296_TRIGGER_PULSE_US_MAX);
	duty_ns = (u64)pulse_us * 1000ULL;

	advance_us = sensor->light_source_enabled ?
			 sensor->light_source_advance_us : 0;
	off_delay_us = sensor->light_source_enabled ?
		       sensor->light_source_off_delay_us : 0;

	if (sensor->light_source_enabled && sensor->light_source_gpio) {
		imx296_light_source_set_locked(sensor, true);
		if (advance_us > 0) {
			if (advance_us <= 1000)
				udelay(advance_us);
			else
				usleep_range(advance_us,
					     advance_us + max_t(u32, 20U,
								advance_us / 10U));
		}
	}

	pwm_get_state(sensor->trigger_pwm, &state);
	state.period = imx296_trigger_period_ns(
		duty_ns, imx296_pwm_period_ns(sensor->trigger_pwm));
	state.duty_cycle = duty_ns;
	state.polarity = PWM_POLARITY_INVERSED;
	state.enabled = true;

	ret = pwm_apply_state(sensor->trigger_pwm, &state);
	if (ret)
		goto out_light;

	/*
	 * Keep the PWM enabled long enough for the low-active portion to finish,
	 * then disable before the next period starts so userspace gets exactly
	 * one low pulse per write.
	 */
	if (pulse_us <= 1000)
		udelay(pulse_us);
	else
		usleep_range(pulse_us, pulse_us + max_t(u32, 20U, pulse_us / 10U));

	state.duty_cycle = 0;
	state.enabled = false;
	ret = pwm_apply_state(sensor->trigger_pwm, &state);
	if (ret)
		goto out_light;

	dev_info(sensor->dev,
		 "trigger pulse emitted: width=%u us idle=high active=low mode=%s streaming=%u\n",
		 pulse_us,
		 imx296_op_mode_name(sensor->streaming ?
				     sensor->active_mode :
				     sensor->pending_mode),
		 sensor->streaming);

	if (sensor->streaming && sensor->active_mode != IMX296_XTRIG_ONE_SHOT)
		dev_warn(sensor->dev,
			 "trigger pulse was emitted while active mode is %s; switch run_mode to master_fast_trigger for one-shot capture\n",
			 imx296_op_mode_name(sensor->active_mode));

out_light:
	if (sensor->light_source_enabled && sensor->light_source_gpio) {
		if (off_delay_us > 0) {
			if (off_delay_us <= 1000)
				udelay(off_delay_us);
			else
				usleep_range(off_delay_us,
					     off_delay_us + max_t(u32, 20U,
								  off_delay_us / 10U));
		}
		imx296_light_source_set_locked(sensor, false);
	}

	return ret;
}
static int imx296_set_ctrl(struct v4l2_ctrl *ctrl);
static int imx296_mode_switch(struct imx296 *sensor,
			      enum imx296_op_mode new_mode);
static u32 imx296_mbus_code(const struct imx296 *sensor);

static const struct v4l2_ctrl_ops imx296_ctrl_ops = {
	.s_ctrl = imx296_set_ctrl,
};

static const struct v4l2_ctrl_config imx296_op_mode_ctrl_cfg = {
	.ops = &imx296_ctrl_ops,
	.id = V4L2_CID_IMX296_OP_MODE,
	.type = V4L2_CTRL_TYPE_MENU,
	.name = "imx296_mode",
	.min = IMX296_FREE_RUN,
	.max = IMX296_XTRIG_ONE_SHOT,
	.def = IMX296_FREE_RUN,
	.qmenu = imx296_op_mode_menu,
};

static const struct v4l2_ctrl_config imx296_light_source_enable_cfg = {
	.ops = &imx296_ctrl_ops,
	.id = V4L2_CID_IMX296_LIGHT_SOURCE_ENABLE,
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.name = "light_source_enable",
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config imx296_light_source_active_level_cfg = {
	.ops = &imx296_ctrl_ops,
	.id = V4L2_CID_IMX296_LIGHT_SOURCE_ACTIVE_LEVEL,
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.name = "light_source_active_level",
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config imx296_light_source_advance_us_cfg = {
	.ops = &imx296_ctrl_ops,
	.id = V4L2_CID_IMX296_LIGHT_SOURCE_ADVANCE_US,
	.type = V4L2_CTRL_TYPE_INTEGER,
	.name = "light_source_advance_us",
	.min = 0,
	.max = 100000,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config imx296_light_source_off_delay_us_cfg = {
	.ops = &imx296_ctrl_ops,
	.id = V4L2_CID_IMX296_LIGHT_SOURCE_OFF_DELAY_US,
	.type = V4L2_CTRL_TYPE_INTEGER,
	.name = "light_source_off_delay_us",
	.min = 0,
	.max = 1000000,
	.step = 1,
	.def = 0,
};

static ssize_t run_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	ssize_t len;

	if (!sensor)
		return -ENODEV;

	mutex_lock(&sensor->mutex);
	len = sysfs_emit(buf,
			 "pending=%s\nactive=%s\nstreaming=%u\navailable=free_run master_fast_trigger\n",
			 imx296_op_mode_name(sensor->pending_mode),
			 imx296_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return len;
}

static ssize_t run_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	enum imx296_op_mode mode;
	int ret;

	if (!sensor || !sensor->op_mode_ctrl)
		return -ENODEV;

	ret = imx296_parse_run_mode(buf, &mode);
	if (ret)
		return ret;

	ret = v4l2_ctrl_s_ctrl(sensor->op_mode_ctrl, mode);
	if (ret)
		return ret;

	mutex_lock(&sensor->mutex);
	if (sensor->pending_mode != mode)
		dev_warn(dev,
			 "run mode request=%s but state is pending=%s active=%s streaming=%u\n",
			 imx296_op_mode_name(mode),
			 imx296_op_mode_name(sensor->pending_mode),
			 imx296_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	else
		dev_info(dev,
			 "run mode request=%s pending=%s active=%s streaming=%u\n",
			 imx296_op_mode_name(mode),
			 imx296_op_mode_name(sensor->pending_mode),
			 imx296_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return count;
}

static DEVICE_ATTR_RW(run_mode);

static ssize_t trigger_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	ssize_t len;

	if (!sensor)
		return -ENODEV;

	mutex_lock(&sensor->mutex);
	len = sysfs_emit(buf,
			 "available=echo 1 > trigger\npulse_us=%u\nidle=high\nactive=low\npwm_present=%u\npending=%s\nactive_mode=%s\nstreaming=%u\n",
			 sensor->trigger_pulse_us,
			 !!sensor->trigger_pwm,
			 imx296_op_mode_name(sensor->pending_mode),
			 imx296_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return len;
}

static ssize_t trigger_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	unsigned int val;
	int ret;

	if (!sensor)
		return -ENODEV;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (!val)
		return count;
	if (val != 1)
		return -EINVAL;

	mutex_lock(&sensor->mutex);
	ret = imx296_trigger_once_locked(sensor);
	mutex_unlock(&sensor->mutex);

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(trigger);

static ssize_t trigger_pulse_us_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	ssize_t len;

	if (!sensor)
		return -ENODEV;

	mutex_lock(&sensor->mutex);
	len = sysfs_emit(buf, "%u\n", sensor->trigger_pulse_us);
	mutex_unlock(&sensor->mutex);

	return len;
}

static ssize_t trigger_pulse_us_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct imx296 *sensor = imx296_from_dev(dev);
	unsigned int pulse_us;

	if (!sensor)
		return -ENODEV;

	if (kstrtouint(buf, 0, &pulse_us))
		return -EINVAL;
	if (pulse_us < IMX296_TRIGGER_PULSE_US_MIN ||
	    pulse_us > IMX296_TRIGGER_PULSE_US_MAX)
		return -ERANGE;

	mutex_lock(&sensor->mutex);
	if (sensor->trigger_pulse_us != pulse_us) {
		u32 old_pulse = sensor->trigger_pulse_us;

		sensor->trigger_pulse_us = pulse_us;
		dev_info(dev,
			 "trigger pulse width set to %u us (was %u)\n",
			 pulse_us, old_pulse);
	}
	mutex_unlock(&sensor->mutex);

	return count;
}

static DEVICE_ATTR_RW(trigger_pulse_us);

static struct attribute *imx296_attrs[] = {
	&dev_attr_run_mode.attr,
	&dev_attr_trigger.attr,
	&dev_attr_trigger_pulse_us.attr,
	NULL
};

static const struct attribute_group imx296_attr_group = {
	.attrs = imx296_attrs,
};

static int imx296_read(struct imx296 *sensor, u32 addr)
{
	u8 data[3] = { 0, 0, 0 };
	int ret;

	ret = regmap_raw_read(sensor->regmap, addr & IMX296_REG_ADDR_MASK, data,
			      (addr >> IMX296_REG_SIZE_SHIFT) & 3);
	if (ret < 0)
		return ret;

	return (data[2] << 16) | (data[1] << 8) | data[0];
}

static void imx296_log_stream_state_locked(struct imx296 *sensor, const char *tag)
{
	u32 ctrl00, ctrl08, ctrl0a, ctrl0b, syncsel, lowlagtrg, pgctrl;
	u32 reg = 0;
	const char *name = NULL;
	int ret;

#define IMX296_READBACK(_reg, _dst, _name) \
	do { \
		reg = (_reg); \
		name = (_name); \
		ret = imx296_read(sensor, reg); \
		if (ret < 0) \
			goto read_fail; \
		(_dst) = ret; \
	} while (0)

	IMX296_READBACK(IMX296_CTRL00, ctrl00, "CTRL00");
	IMX296_READBACK(IMX296_CTRL08, ctrl08, "CTRL08");
	IMX296_READBACK(IMX296_CTRL0A, ctrl0a, "CTRL0A");
	IMX296_READBACK(IMX296_CTRL0B, ctrl0b, "CTRL0B");
	IMX296_READBACK(IMX296_SYNCSEL, syncsel, "SYNCSEL");
	IMX296_READBACK(IMX296_LOWLAGTRG, lowlagtrg, "LOWLAGTRG");
	IMX296_READBACK(IMX296_PGCTRL, pgctrl, "PGCTRL");

#undef IMX296_READBACK

	dev_info(sensor->dev,
		 "stream-state[%s]: pending=%s active=%s test_pattern=%u ctrl00=0x%02x standby=%u ctrl08=0x%02x reghold=%u ctrl0a=0x%02x xmsta=%u ctrl0b=0x%02x trigen=%u syncsel=0x%02x lowlag=0x%02x pgctrl=0x%02x regen=%u clken=%u pg_mode=%u\n",
		 tag,
		 imx296_op_mode_name(sensor->pending_mode),
		 imx296_op_mode_name(sensor->active_mode),
		 sensor->test_pattern ? sensor->test_pattern->val : 0,
		 ctrl00, !!(ctrl00 & IMX296_CTRL00_STANDBY),
		 ctrl08, !!(ctrl08 & IMX296_CTRL08_REGHOLD),
		 ctrl0a, !!(ctrl0a & IMX296_CTRL0A_XMSTA),
		 ctrl0b, !!(ctrl0b & IMX296_CTRL0B_TRIGEN),
		 syncsel, lowlagtrg, pgctrl,
		 !!(pgctrl & IMX296_PGCTRL_REGEN),
		 !!(pgctrl & IMX296_PGCTRL_CLKEN),
		 (pgctrl >> 3) & 0x1f);
	return;

read_fail:
	dev_warn(sensor->dev,
		 "stream-state[%s]: failed to read %s (0x%04x): %d\n",
		 tag, name, reg & IMX296_REG_ADDR_MASK, ret);
}

static int imx296_write(struct imx296 *sensor, u32 addr, u32 value, int *err)
{
	u8 data[3] = { value & 0xff, (value >> 8) & 0xff, value >> 16 };
	int ret;

	if (err && *err)
		return *err;

	ret = regmap_raw_write(sensor->regmap, addr & IMX296_REG_ADDR_MASK,
			       data, (addr >> IMX296_REG_SIZE_SHIFT) & 3);
	if (ret < 0) {
		dev_err(sensor->dev, "%u-bit write to 0x%04x failed: %d\n",
			((addr >> IMX296_REG_SIZE_SHIFT) & 3) * 8,
			addr & IMX296_REG_ADDR_MASK, ret);
		if (err)
			*err = ret;
	}

	return ret;
}

static u64 imx296_line_period_ns(void)
{
	return DIV_ROUND_CLOSEST_ULL((u64)IMX296_HMAX_DEFAULT * 1000000000ULL,
				     IMX296_OP_CLK_FREQ_HZ);
}

static u32 imx296_lines_to_exposure_us(u32 lines)
{
	u64 exposure_ns = (u64)lines * imx296_line_period_ns() +
			  IMX296_EXPOSURE_OFFSET_NS;

	return DIV_ROUND_CLOSEST_ULL(exposure_ns, 1000);
}

static struct v4l2_rect *
imx296_get_pad_crop(struct imx296 *sensor, struct v4l2_subdev_state *state,
		    unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&sensor->subdev, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->crop;
	}

	return NULL;
}

static struct v4l2_mbus_framefmt *
imx296_get_pad_format(struct imx296 *sensor, struct v4l2_subdev_state *state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&sensor->subdev, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->format;
	}

	return NULL;
}

static u32 imx296_frame_lines_locked(struct imx296 *sensor,
				     enum imx296_op_mode mode)
{
	const struct v4l2_mbus_framefmt *format;
	u32 vblank = IMX296_VBLANK_DEFAULT;

	format = &sensor->format;
	if (mode == IMX296_FREE_RUN && sensor->vblank)
		vblank = sensor->vblank->val;

	return format->height + vblank;
}

static u32 imx296_exposure_us_to_lines_locked(struct imx296 *sensor,
					      u32 exposure_us,
					      u32 frame_lines)
{
	u64 request_ns = (u64)exposure_us * 1000ULL;
	u64 line_ns = imx296_line_period_ns();
	u32 lines;
	u32 max_lines;

	max_lines = frame_lines > IMX296_MIN_MEMORY_WAIT_LINES ?
		    frame_lines - IMX296_MIN_MEMORY_WAIT_LINES : 1;

	if (request_ns <= IMX296_EXPOSURE_OFFSET_NS)
		return 1;

	lines = DIV_ROUND_CLOSEST_ULL(request_ns - IMX296_EXPOSURE_OFFSET_NS,
				      line_ns);

	return clamp_t(u32, lines, 1, max_lines);
}

static void imx296_get_module_inf(struct imx296 *sensor,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, IMX296_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, sensor->module_name, sizeof(inf->base.module));
	strlcpy(inf->base.lens, sensor->len_name, sizeof(inf->base.lens));
}

static int imx296_power_on(struct imx296 *sensor)
{
	int ret;

	if (!IS_ERR_OR_NULL(sensor->pinctrl)) {
		ret = imx296_light_source_apply_pinctrl_locked(sensor);
		if (ret < 0)
			dev_dbg(sensor->dev, "could not set light source pin state\n");
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies),
				    sensor->supplies);
	if (ret < 0)
		return ret;

	udelay(1);

	if (sensor->reset_gpio) {
		ret = gpiod_direction_output(sensor->reset_gpio, 0);
		if (ret < 0)
			goto err_supply;
	}

	udelay(1);

	if (sensor->clk) {
		ret = clk_prepare_enable(sensor->clk);
		if (ret < 0)
			goto err_reset;
	}

	usleep_range(1000, 2000);

	return 0;

err_reset:
	if (sensor->reset_gpio)
		gpiod_direction_output(sensor->reset_gpio, 1);
err_supply:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	return ret;
}

static void imx296_power_off(struct imx296 *sensor)
{
	if (sensor->clk)
		clk_disable_unprepare(sensor->clk);
	if (sensor->reset_gpio)
		gpiod_direction_output(sensor->reset_gpio, 1);

	if (!IS_ERR_OR_NULL(sensor->pins_sleep)) {
		int ret;

		ret = pinctrl_select_state(sensor->pinctrl, sensor->pins_sleep);
		if (ret < 0)
			dev_dbg(sensor->dev, "could not set sleep pin state\n");
	}

	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
}

static int imx296_apply_flip_locked(struct imx296 *sensor)
{
	u32 val = sensor->vflip->val | (sensor->hflip->val << 1);

	return imx296_write(sensor, IMX296_CTRL0E, val, NULL);
}

static int imx296_apply_test_pattern_locked(struct imx296 *sensor, u32 value)
{
	int ret = 0;

	if (value) {
		imx296_write(sensor, IMX296_PGHPOS, 8, &ret);
		imx296_write(sensor, IMX296_PGVPOS, 8, &ret);
		imx296_write(sensor, IMX296_PGHPSTEP, 8, &ret);
		imx296_write(sensor, IMX296_PGVPSTEP, 8, &ret);
		imx296_write(sensor, IMX296_PGHPNUM, 100, &ret);
		imx296_write(sensor, IMX296_PGVPNUM, 100, &ret);
		imx296_write(sensor, IMX296_PGDATA1, 0x300, &ret);
		imx296_write(sensor, IMX296_PGDATA2, 0x100, &ret);
		imx296_write(sensor, IMX296_PGHGSTEP, 0, &ret);
		imx296_write(sensor, IMX296_BLKLEVEL, 0, &ret);
		imx296_write(sensor, IMX296_BLKLEVELAUTO,
			     IMX296_BLKLEVELAUTO_OFF, &ret);
		imx296_write(sensor, IMX296_PGCTRL,
			     IMX296_PGCTRL_REGEN |
			     IMX296_PGCTRL_CLKEN |
			     IMX296_PGCTRL_MODE(value - 1), &ret);
	} else {
		imx296_write(sensor, IMX296_PGCTRL, IMX296_PGCTRL_CLKEN, &ret);
		imx296_write(sensor, IMX296_BLKLEVEL, 0x3c, &ret);
		imx296_write(sensor, IMX296_BLKLEVELAUTO,
			     IMX296_BLKLEVELAUTO_ON, &ret);
	}

	return ret;
}

static int imx296_apply_analog_gain_locked(struct imx296 *sensor, u32 value)
{
	value = clamp_t(u32, value, IMX296_ANALOG_GAIN_MIN, IMX296_ANALOG_GAIN_MAX);
	return imx296_write(sensor, IMX296_GAIN, value, NULL);
}

static int imx296_apply_vblank_locked(struct imx296 *sensor, u32 vblank)
{
	const struct v4l2_mbus_framefmt *format;

	format = &sensor->format;
	return imx296_write(sensor, IMX296_VMAX, format->height + vblank, NULL);
}

static int imx296_apply_exposure_locked(struct imx296 *sensor, u32 exposure_us)
{
	u32 frame_lines;
	u32 exposure_lines;

	frame_lines = imx296_frame_lines_locked(sensor, IMX296_FREE_RUN);
	exposure_lines = imx296_exposure_us_to_lines_locked(sensor, exposure_us,
							    frame_lines);

	return imx296_write(sensor, IMX296_SHS1,
			    frame_lines - exposure_lines, NULL);
}

static void imx296_disable_trigger_outputs_locked(struct imx296 *sensor, int *err)
{
	imx296_write(sensor, IMX296_CTRLTOUT,
		     IMX296_CTRLTOUT_TOUT1SEL_LOW |
		     IMX296_CTRLTOUT_TOUT2SEL_LOW, err);
	imx296_write(sensor, IMX296_CTRLTRIG,
		     IMX296_CTRLTRIG_TOUT1_SEL_LOW |
		     IMX296_CTRLTRIG_TOUT2_SEL_LOW, err);
	imx296_write(sensor, IMX296_PULSE1, 0, err);
	imx296_write(sensor, IMX296_PULSE2, 0, err);
}

static void imx296_update_ctrl_visibility_locked(struct imx296 *sensor,
						 enum imx296_op_mode mode)
{
	if (sensor->exposure)
		v4l2_ctrl_activate(sensor->exposure,
				   mode == IMX296_FREE_RUN);
	if (sensor->vblank)
		v4l2_ctrl_activate(sensor->vblank,
				   mode == IMX296_FREE_RUN);
}

static void imx296_update_free_run_exposure_range_locked(struct imx296 *sensor,
							 u32 vblank)
{
	u32 frame_lines = sensor->format.height + vblank;
	u32 max_lines = frame_lines > IMX296_MIN_MEMORY_WAIT_LINES ?
			frame_lines - IMX296_MIN_MEMORY_WAIT_LINES : 1;
	u32 min_us;
	u32 max_us;
	u32 def_us;

	if (!sensor->exposure)
		return;

	min_us = imx296_lines_to_exposure_us(1);
	max_us = imx296_lines_to_exposure_us(max_lines);
	def_us = clamp_val(imx296_lines_to_exposure_us(IMX296_EXPOSURE_DEFAULT_LINES),
			   min_us, max_us);

	__v4l2_ctrl_modify_range(sensor->exposure, min_us, max_us, 1, def_us);
}

static void imx296_setup_hblank(struct imx296 *sensor, unsigned int width)
{
	unsigned int hblank;

	hblank = IMX296_HMAX_DEFAULT * IMX296_PIXEL_RATE / IMX296_OP_CLK_FREQ_HZ
	       - width;

	if (!sensor->hblank) {
		sensor->hblank = v4l2_ctrl_new_std(&sensor->ctrls, NULL,
						   V4L2_CID_HBLANK, hblank,
						   hblank, 1, hblank);
		if (sensor->hblank)
			sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	} else {
		__v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank, 1,
					 hblank);
	}
}

static int imx296_ctrls_init(struct imx296 *sensor)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *handler = &sensor->ctrls;
	u32 def_exposure_us;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret < 0)
		return ret;

	ret = v4l2_ctrl_handler_init(handler, 15);
	if (ret)
		return ret;

	handler->lock = &sensor->mutex;

	def_exposure_us = imx296_lines_to_exposure_us(IMX296_EXPOSURE_DEFAULT_LINES);
	sensor->exposure = v4l2_ctrl_new_std(handler, &imx296_ctrl_ops,
					     V4L2_CID_EXPOSURE, 1, 2000000, 1,
					     def_exposure_us);
	sensor->anal_gain = v4l2_ctrl_new_std(handler, &imx296_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      IMX296_ANALOG_GAIN_MIN,
					      IMX296_ANALOG_GAIN_MAX,
					      IMX296_ANALOG_GAIN_STEP,
					      IMX296_ANALOG_GAIN_MIN);

	sensor->hflip = v4l2_ctrl_new_std(handler, &imx296_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (sensor->hflip && !sensor->mono)
		sensor->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	sensor->vflip = v4l2_ctrl_new_std(handler, &imx296_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (sensor->vflip && !sensor->mono)
		sensor->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx296_setup_hblank(sensor, IMX296_PIXEL_ARRAY_WIDTH);

	sensor->vblank = v4l2_ctrl_new_std(handler, &imx296_ctrl_ops,
					   V4L2_CID_VBLANK,
					   IMX296_VBLANK_DEFAULT,
					   IMX296_VBLANK_MAX, 1,
					   IMX296_VBLANK_DEFAULT);

	sensor->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(imx296_link_freq_menu) - 1,
						   0, imx296_link_freq_menu);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
					       V4L2_CID_PIXEL_RATE,
					       1122000000 / 10,
					       1198000000 / 10, 1,
					       IMX296_PIXEL_RATE);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->test_pattern = v4l2_ctrl_new_std_menu_items(
		handler, &imx296_ctrl_ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(imx296_test_pattern_menu) - 1, 0, 0,
		imx296_test_pattern_menu);
	sensor->op_mode_ctrl = v4l2_ctrl_new_custom(handler,
						    &imx296_op_mode_ctrl_cfg,
						    NULL);

	sensor->light_source_enable_ctrl = v4l2_ctrl_new_custom(handler,
							       &imx296_light_source_enable_cfg,
							       NULL);
	sensor->light_source_active_level_ctrl = v4l2_ctrl_new_custom(handler,
								      &imx296_light_source_active_level_cfg,
								      NULL);
	sensor->light_source_advance_us_ctrl = v4l2_ctrl_new_custom(handler,
								  &imx296_light_source_advance_us_cfg,
								  NULL);
	sensor->light_source_off_delay_us_ctrl = v4l2_ctrl_new_custom(handler,
								    &imx296_light_source_off_delay_us_cfg,
								    NULL);

	v4l2_ctrl_new_fwnode_properties(handler, &imx296_ctrl_ops, &props);

	if (handler->error) {
		ret = handler->error;
		dev_err(sensor->dev, "failed to add controls (%d)\n", ret);
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	if (sensor->op_mode_ctrl) {
		/*
		 * Keep the control cache aligned with the DTS-selected default
		 * mode so the first sysfs/V4L2 write is not swallowed as a
		 * no-op when the board boots in master_fast_trigger.
		 */
		ret = __v4l2_ctrl_s_ctrl(sensor->op_mode_ctrl, sensor->pending_mode);
		if (ret < 0) {
			dev_err(sensor->dev,
				"failed to sync default run mode control (%d)\n", ret);
			v4l2_ctrl_handler_free(handler);
			return ret;
		}
	}

	sensor->subdev.ctrl_handler = handler;
	imx296_update_free_run_exposure_range_locked(sensor,
						    sensor->vblank->val);
	imx296_update_ctrl_visibility_locked(sensor, sensor->pending_mode);

	return 0;
}

static const struct {
	unsigned int reg;
	unsigned int value;
} imx296_init_table[] = {
	{ IMX296_REG_8BIT(0x3005), 0xf0 },
	{ IMX296_REG_8BIT(0x309e), 0x04 },
	{ IMX296_REG_8BIT(0x30a0), 0x04 },
	{ IMX296_REG_8BIT(0x30a1), 0x3c },
	{ IMX296_REG_8BIT(0x30a4), 0x5f },
	{ IMX296_REG_8BIT(0x30a8), 0x91 },
	{ IMX296_REG_8BIT(0x30ac), 0x28 },
	{ IMX296_REG_8BIT(0x30af), 0x0b },
	{ IMX296_REG_8BIT(0x30df), 0x00 },
	{ IMX296_REG_8BIT(0x3165), 0x00 },
	{ IMX296_REG_8BIT(0x3169), 0x10 },
	{ IMX296_REG_8BIT(0x316a), 0x02 },
	{ IMX296_REG_8BIT(0x31c8), 0xf3 },
	{ IMX296_REG_8BIT(0x31d0), 0xf4 },
	{ IMX296_REG_8BIT(0x321a), 0x00 },
	{ IMX296_REG_8BIT(0x3226), 0x02 },
	{ IMX296_REG_8BIT(0x3256), 0x01 },
	{ IMX296_REG_8BIT(0x3541), 0x72 },
	{ IMX296_REG_8BIT(0x3516), 0x77 },
	{ IMX296_REG_8BIT(0x350b), 0x7f },
	{ IMX296_REG_8BIT(0x3758), 0xa3 },
	{ IMX296_REG_8BIT(0x3759), 0x00 },
	{ IMX296_REG_8BIT(0x375a), 0x85 },
	{ IMX296_REG_8BIT(0x375b), 0x00 },
	{ IMX296_REG_8BIT(0x3832), 0xf5 },
	{ IMX296_REG_8BIT(0x3833), 0x00 },
	{ IMX296_REG_8BIT(0x38a2), 0xf6 },
	{ IMX296_REG_8BIT(0x38a3), 0x00 },
	{ IMX296_REG_8BIT(0x3a00), 0x80 },
	{ IMX296_REG_8BIT(0x3d48), 0xa3 },
	{ IMX296_REG_8BIT(0x3d49), 0x00 },
	{ IMX296_REG_8BIT(0x3d4a), 0x85 },
	{ IMX296_REG_8BIT(0x3d4b), 0x00 },
	{ IMX296_REG_8BIT(0x400e), 0x58 },
	{ IMX296_REG_8BIT(0x4014), 0x1c },
	{ IMX296_REG_8BIT(0x4041), 0x2a },
	{ IMX296_REG_8BIT(0x40a2), 0x06 },
	{ IMX296_REG_8BIT(0x40c1), 0xf6 },
	{ IMX296_REG_8BIT(0x40c7), 0x0f },
	{ IMX296_REG_8BIT(0x40c8), 0x00 },
	{ IMX296_REG_8BIT(0x4174), 0x00 },
};

static int imx296_setup(struct imx296 *sensor)
{
	const struct v4l2_mbus_framefmt *format = &sensor->format;
	const struct v4l2_rect *crop = &sensor->crop;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(imx296_init_table); ++i)
		imx296_write(sensor, imx296_init_table[i].reg,
			     imx296_init_table[i].value, &ret);

	if (crop->width != IMX296_PIXEL_ARRAY_WIDTH ||
	    crop->height != IMX296_PIXEL_ARRAY_HEIGHT) {
		imx296_write(sensor, IMX296_FID0_ROI,
			     IMX296_FID0_ROIH1ON | IMX296_FID0_ROIV1ON, &ret);
		imx296_write(sensor, IMX296_FID0_ROIPH1, crop->left, &ret);
		imx296_write(sensor, IMX296_FID0_ROIPV1, crop->top, &ret);
		imx296_write(sensor, IMX296_FID0_ROIWH1, crop->width, &ret);
		imx296_write(sensor, IMX296_FID0_ROIWV1, crop->height, &ret);
		imx296_write(sensor, IMX296_MIPIC_AREA3W, crop->height, &ret);
	} else {
		/*
		 * Restore the ROI geometry registers as well as disabling ROI.
		 * Leaving the previous window programmed is observable after an
		 * ROI-to-full-frame restart in fast-trigger mode: the frame identity
		 * is current, but the last output lines can still contain data from
		 * the preceding buffer.  The power-on full-frame state has all four
		 * geometry registers cleared, so reproduce that state explicitly.
		 */
		imx296_write(sensor, IMX296_FID0_ROI, 0, &ret);
		imx296_write(sensor, IMX296_FID0_ROIPH1, 0, &ret);
		imx296_write(sensor, IMX296_FID0_ROIPV1, 0, &ret);
		imx296_write(sensor, IMX296_FID0_ROIWH1, 0, &ret);
		imx296_write(sensor, IMX296_FID0_ROIWV1, 0, &ret);
		imx296_write(sensor, IMX296_MIPIC_AREA3W,
			     IMX296_PIXEL_ARRAY_HEIGHT, &ret);
	}

	imx296_write(sensor, IMX296_CTRL0D,
		     (crop->width != format->width ?
		      IMX296_CTRL0D_HADD_ON_BINNING : 0) |
		     (crop->height != format->height ?
		      IMX296_CTRL0D_WINMODE_FD_BINNING : 0),
		     &ret);

	imx296_write(sensor, IMX296_HMAX, IMX296_HMAX_DEFAULT, &ret);
	imx296_write(sensor, IMX296_VMAX,
		     format->height + sensor->vblank->val, &ret);

	for (i = 0; i < ARRAY_SIZE(sensor->clk_params->incksel); ++i)
		imx296_write(sensor, IMX296_INCKSEL(i),
			     sensor->clk_params->incksel[i], &ret);

	imx296_write(sensor, IMX296_GTTABLENUM, 0xc5, &ret);
	imx296_write(sensor, IMX296_CTRL418C, sensor->clk_params->ctrl418c,
		     &ret);
	imx296_write(sensor, IMX296_GAINDLY,
		     sensor->pending_mode == IMX296_XTRIG_ONE_SHOT ?
		     IMX296_GAINDLY_0FRAME : IMX296_GAINDLY_1FRAME,
		     &ret);
	imx296_write(sensor, IMX296_BLKLEVEL, 0x03c, &ret);
	imx296_write(sensor, IMX296_VINT, IMX296_VINT_EN, &ret);

	return ret;
}

static int imx296_apply_mode_regs_locked(struct imx296 *sensor,
					 enum imx296_op_mode mode)
{
	int ret = 0;

	imx296_write(sensor, IMX296_CTRL08, IMX296_CTRL08_REGHOLD, &ret);

	switch (mode) {
	case IMX296_FREE_RUN:
		imx296_write(sensor, IMX296_CTRL0B, 0, &ret);
		imx296_write(sensor, IMX296_LOWLAGTRG, 0, &ret);
		imx296_write(sensor, IMX296_SYNCSEL,
			     IMX296_SYNCSEL_NORMAL, &ret);
		imx296_disable_trigger_outputs_locked(sensor, &ret);
		break;

	case IMX296_XTRIG_ONE_SHOT:
		/*
		 * Single-camera external trigger is implemented as the IMX296
		 * fast trigger path. Per the Sony datasheet, fast trigger is
		 * only supported while the sensor itself runs in master mode, so
		 * XMASTER must be strapped for master mode on the board.
		 */
		imx296_write(sensor, IMX296_CTRL0B,
			     IMX296_CTRL0B_TRIGEN, &ret);
		imx296_write(sensor, IMX296_LOWLAGTRG,
			     IMX296_LOWLAGTRG_FAST, &ret);
		imx296_write(sensor, IMX296_SYNCSEL, IMX296_SYNCSEL_HIZ, &ret);
		imx296_disable_trigger_outputs_locked(sensor, &ret);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	imx296_write(sensor, IMX296_CTRL08, 0, &ret);

	return ret;
}

static int imx296_restore_ctrls_for_mode_locked(struct imx296 *sensor,
						enum imx296_op_mode mode)
{
	int ret;

	ret = imx296_apply_analog_gain_locked(sensor, sensor->anal_gain->val);
	if (ret)
		return ret;

	ret = imx296_apply_flip_locked(sensor);
	if (ret)
		return ret;

	ret = imx296_apply_test_pattern_locked(sensor, sensor->test_pattern->val);
	if (ret)
		return ret;

	if (mode == IMX296_FREE_RUN) {
		imx296_update_free_run_exposure_range_locked(sensor,
							    sensor->vblank->val);

		ret = imx296_apply_vblank_locked(sensor, sensor->vblank->val);
		if (ret)
			return ret;

		return imx296_apply_exposure_locked(sensor, sensor->exposure->val);
	}

	/* XTRIG exposure is set by pulse width; VMAX must use the VTR baseline. */
	return imx296_apply_vblank_locked(sensor, IMX296_XTRIG_VBLANK);
}

static int imx296_stream_on(struct imx296 *sensor)
{
	int ret = 0;

	imx296_log_stream_state_locked(sensor, "before-on");

	imx296_write(sensor, IMX296_CTRL00, 0, &ret);
	usleep_range(30000, 35000);
	imx296_write(sensor, IMX296_CTRL0A, 0, &ret);

	imx296_log_stream_state_locked(sensor, ret ? "after-on-error" : "after-on");

	__v4l2_ctrl_grab(sensor->vflip, 1);
	__v4l2_ctrl_grab(sensor->hflip, 1);

	return ret;
}

static int imx296_stream_off(struct imx296 *sensor)
{
	int ret = 0;

	imx296_write(sensor, IMX296_CTRL0A, IMX296_CTRL0A_XMSTA, &ret);
	imx296_write(sensor, IMX296_CTRL00, IMX296_CTRL00_STANDBY, &ret);

	__v4l2_ctrl_grab(sensor->vflip, 0);
	__v4l2_ctrl_grab(sensor->hflip, 0);

	return ret;
}

static int imx296_quick_stream(struct imx296 *sensor, bool on)
{
	int ret = 0;

	if (on) {
		imx296_write(sensor, IMX296_CTRL00, 0, &ret);
		usleep_range(2000, 5000);
		imx296_write(sensor, IMX296_CTRL0A, 0, &ret);
	} else {
		imx296_write(sensor, IMX296_CTRL0A, IMX296_CTRL0A_XMSTA, &ret);
		imx296_write(sensor, IMX296_CTRL00,
			     IMX296_CTRL00_STANDBY, &ret);
	}

	return ret;
}

static int imx296_mode_switch(struct imx296 *sensor,
			      enum imx296_op_mode new_mode)
{
	int ret = 0;

	if (new_mode > IMX296_XTRIG_ONE_SHOT)
		return -EINVAL;

	atomic_set(&imx296_fast_trigger_mode,
		   new_mode == IMX296_XTRIG_ONE_SHOT);
	sensor->pending_mode = new_mode;
	imx296_update_ctrl_visibility_locked(sensor, new_mode);

	if (!sensor->streaming)
		return 0;

	if (sensor->active_mode == new_mode)
		return 0;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	ret = imx296_stream_off(sensor);
	if (ret)
		goto out_pm;

	sensor->streaming = false;

	ret = imx296_apply_mode_regs_locked(sensor, new_mode);
	if (ret)
		goto out_pm;

	ret = imx296_restore_ctrls_for_mode_locked(sensor, new_mode);
	if (ret)
		goto out_pm;

	ret = imx296_stream_on(sensor);
	if (ret)
		goto out_pm;

	sensor->active_mode = new_mode;
	sensor->streaming = true;

out_pm:
	if (ret && !sensor->streaming) {
		pm_runtime_mark_last_busy(sensor->dev);
		pm_runtime_put_autosuspend(sensor->dev);
	} else {
		pm_runtime_put(sensor->dev);
	}
	return ret;
}

static int imx296_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx296 *sensor = container_of(ctrl->handler, struct imx296, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_IMX296_OP_MODE)
		return imx296_mode_switch(sensor, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx296_update_free_run_exposure_range_locked(sensor, ctrl->val);
		break;
	case V4L2_CID_IMX296_LIGHT_SOURCE_ENABLE:
		sensor->light_source_enabled = !!ctrl->val;
		break;
	case V4L2_CID_IMX296_LIGHT_SOURCE_ACTIVE_LEVEL:
		sensor->light_source_active_level = !!ctrl->val;
		break;
	case V4L2_CID_IMX296_LIGHT_SOURCE_ADVANCE_US:
		sensor->light_source_advance_us = ctrl->val;
		break;
	case V4L2_CID_IMX296_LIGHT_SOURCE_OFF_DELAY_US:
		sensor->light_source_off_delay_us = ctrl->val;
		break;
	default:
		break;
	}

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		if (sensor->active_mode == IMX296_FREE_RUN)
			ret = imx296_apply_exposure_locked(sensor, ctrl->val);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx296_apply_analog_gain_locked(sensor, ctrl->val);
		break;

	case V4L2_CID_VBLANK:
		if (sensor->active_mode == IMX296_FREE_RUN)
			ret = imx296_apply_vblank_locked(sensor, ctrl->val);
		break;

	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		sensor->format.code = imx296_mbus_code(sensor);
		ret = imx296_apply_flip_locked(sensor);
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = imx296_apply_test_pattern_locked(sensor, ctrl->val);
		break;

	case V4L2_CID_IMX296_LIGHT_SOURCE_ENABLE:
		if (sensor->streaming && sensor->active_mode == IMX296_FREE_RUN)
			imx296_light_source_set_locked(sensor,
						       sensor->light_source_enabled);
		break;

	case V4L2_CID_IMX296_LIGHT_SOURCE_ACTIVE_LEVEL:
		ret = imx296_light_source_init_locked(sensor);
		if (ret < 0)
			break;
		if (sensor->streaming && sensor->active_mode == IMX296_FREE_RUN)
			imx296_light_source_set_locked(sensor,
						       sensor->light_source_enabled);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(sensor->dev);
	return ret;
}

static u32 imx296_mbus_code(const struct imx296 *sensor)
{
	static const u32 mbus_codes[] = {
		MEDIA_BUS_FMT_SRGGB10_1X10,
		MEDIA_BUS_FMT_SGRBG10_1X10,
		MEDIA_BUS_FMT_SGBRG10_1X10,
		MEDIA_BUS_FMT_SBGGR10_1X10,
	};
	unsigned int i = 0;

	if (sensor->mono)
		return MEDIA_BUS_FMT_Y10_1X10;

	if (sensor->vflip && sensor->hflip)
		i = (sensor->vflip->val ? 2 : 0) | (sensor->hflip->val ? 1 : 0);

	return mbus_codes[i];
}

static int imx296_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx296 *sensor = to_imx296(sd);
	int ret = 0;

	mutex_lock(&sensor->mutex);

	enable = !!enable;
	if (enable == sensor->streaming)
		goto unlock;

	if (!enable) {
		ret = imx296_stream_off(sensor);
		imx296_light_source_set_locked(sensor, false);
		if (!ret) {
			sensor->streaming = false;
			pm_runtime_mark_last_busy(sensor->dev);
			pm_runtime_put_autosuspend(sensor->dev);
		}
		goto unlock;
	}

	imx296_update_ctrl_visibility_locked(sensor, sensor->pending_mode);

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		goto unlock;

	ret = imx296_setup(sensor);
	if (ret)
		goto err_pm;

	ret = imx296_apply_mode_regs_locked(sensor, sensor->pending_mode);
	if (ret)
		goto err_pm;

	ret = imx296_restore_ctrls_for_mode_locked(sensor, sensor->pending_mode);
	if (ret)
		goto err_pm;

	ret = imx296_stream_on(sensor);
	if (ret)
		goto err_pm;

	ret = imx296_light_source_init_locked(sensor);
	if (ret)
		goto err_pm;

	sensor->active_mode = sensor->pending_mode;
	sensor->streaming = true;

	/*
	 * The datasheet specifies one invalid frame after ROI geometry changes.
	 * CamOS restarts the sensor in free-run before switching to fast trigger,
	 * so let that invalid frame drain before userspace can change run mode.
	 */
	if (sensor->roi_boundary_pending &&
	    sensor->active_mode == IMX296_FREE_RUN) {
		usleep_range(IMX296_ROI_INVALID_FRAME_WAIT_US,
			     IMX296_ROI_INVALID_FRAME_WAIT_US + 5000U);
		sensor->roi_boundary_pending = false;
	}

	if (sensor->active_mode == IMX296_FREE_RUN && sensor->light_source_enabled)
		imx296_light_source_set_locked(sensor, true);

	goto unlock;

err_pm:
	pm_runtime_put_sync(sensor->dev);
unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int imx296_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx296 *sensor = to_imx296(sd);
	int ret = 0;

	mutex_lock(&sensor->mutex);

	if (sensor->power_on == !!on)
		goto unlock;

	if (on) {
		ret = pm_runtime_get_sync(sensor->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(sensor->dev);
			goto unlock;
		}
		sensor->power_on = true;
	} else {
		pm_runtime_put(sensor->dev);
		sensor->power_on = false;
	}

unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int imx296_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx296 *sensor = to_imx296(sd);
	u32 frame_lines;

	mutex_lock(&sensor->mutex);
	frame_lines = imx296_frame_lines_locked(sensor,
						sensor->streaming ?
						sensor->active_mode :
						sensor->pending_mode);
	fi->interval.numerator = frame_lines * IMX296_HMAX_DEFAULT;
	fi->interval.denominator = IMX296_OP_CLK_FREQ_HZ;
	mutex_unlock(&sensor->mutex);

	return 0;
}

static int imx296_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = IMX296_NUM_DATA_LANES;

	return 0;
}

static long imx296_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx296 *sensor = to_imx296(sd);
	u32 stream = 0;
	u32 sync_mode = 0;
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx296_get_module_inf(sensor, (struct rkmodule_inf *)arg);
		break;

	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);
		mutex_lock(&sensor->mutex);
		ret = imx296_quick_stream(sensor, !!stream);
		mutex_unlock(&sensor->mutex);
		break;

	case RKMODULE_GET_SYNC_MODE:
		*((u32 *)arg) = sensor->sync_mode;
		break;

	case RKMODULE_SET_SYNC_MODE:
		sync_mode = *((u32 *)arg);
		if (sync_mode != NO_SYNC_MODE &&
		    sync_mode != INTERNAL_MASTER_MODE)
			ret = -EINVAL;
		else
			sensor->sync_mode = INTERNAL_MASTER_MODE;
		break;

	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long imx296_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	long ret;
	u32 stream = 0;
	u32 sync_mode = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;

		ret = imx296_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		break;

	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(stream)))
			return -EFAULT;
		ret = imx296_ioctl(sd, cmd, &stream);
		break;

	case RKMODULE_GET_SYNC_MODE:
		ret = imx296_ioctl(sd, cmd, &sync_mode);
		if (!ret && copy_to_user(up, &sync_mode, sizeof(sync_mode)))
			ret = -EFAULT;
		break;

	case RKMODULE_SET_SYNC_MODE:
		if (copy_from_user(&sync_mode, up, sizeof(sync_mode)))
			return -EFAULT;
		ret = imx296_ioctl(sd, cmd, &sync_mode);
		break;

	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int imx296_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx296 *sensor = to_imx296(sd);

	if (code->index != 0)
		return -EINVAL;

	code->code = imx296_mbus_code(sensor);
	return 0;
}

static int imx296_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx296 *sensor = to_imx296(sd);

	if (fse->index >= 1 || fse->code != imx296_mbus_code(sensor))
		return -EINVAL;

	fse->min_width = IMX296_PIXEL_ARRAY_WIDTH;
	fse->max_width = IMX296_PIXEL_ARRAY_WIDTH;
	fse->min_height = IMX296_PIXEL_ARRAY_HEIGHT;
	fse->max_height = IMX296_PIXEL_ARRAY_HEIGHT;

	return 0;
}

static int imx296_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct imx296 *sensor = to_imx296(sd);
	struct v4l2_mbus_framefmt *format;

	mutex_lock(&sensor->mutex);
	format = imx296_get_pad_format(sensor, state, fmt->pad, fmt->which);
	format->code = imx296_mbus_code(sensor);
	/*
	 * Report actual crop size so downstream subdevs get the real
	 * output dimensions. ROI selection already updates format width
	 * and height in imx296_set_selection().
	 */
	fmt->format = *format;
	mutex_unlock(&sensor->mutex);

	return 0;
}

static int imx296_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct imx296 *sensor = to_imx296(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	mutex_lock(&sensor->mutex);
	crop = imx296_get_pad_crop(sensor, state, fmt->pad, fmt->which);
	format = imx296_get_pad_format(sensor, state, fmt->pad, fmt->which);

	format->width = crop->width;
	format->height = crop->height;
	format->code = imx296_mbus_code(sensor);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		imx296_setup_hblank(sensor, format->width);
		imx296_update_free_run_exposure_range_locked(sensor,
							    sensor->vblank->val);
	}

	fmt->format = *format;
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int imx296_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct imx296 *sensor = to_imx296(sd);

	mutex_lock(&sensor->mutex);
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *imx296_get_pad_crop(sensor, state, sel->pad, sel->which);
		break;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX296_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX296_PIXEL_ARRAY_HEIGHT;
		break;

	default:
		mutex_unlock(&sensor->mutex);
		return -EINVAL;
	}

	mutex_unlock(&sensor->mutex);
	return 0;
}

static int imx296_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct imx296 *sensor = to_imx296(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	struct v4l2_rect rect;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	rect.left = clamp(ALIGN(sel->r.left, 4), 0,
			  IMX296_PIXEL_ARRAY_WIDTH - IMX296_FID0_ROIWH1_MIN);
	rect.top = clamp(ALIGN(sel->r.top, 4), 0,
			 IMX296_PIXEL_ARRAY_HEIGHT - IMX296_FID0_ROIWV1_MIN);
	rect.width = clamp_t(unsigned int, ALIGN(sel->r.width, 4),
			     IMX296_FID0_ROIWH1_MIN, IMX296_PIXEL_ARRAY_WIDTH);
	rect.height = clamp_t(unsigned int, ALIGN(sel->r.height, 4),
			      IMX296_FID0_ROIWV1_MIN, IMX296_PIXEL_ARRAY_HEIGHT);

	rect.width = min_t(unsigned int, rect.width,
			   IMX296_PIXEL_ARRAY_WIDTH - rect.left);
	rect.height = min_t(unsigned int, rect.height,
			    IMX296_PIXEL_ARRAY_HEIGHT - rect.top);

	mutex_lock(&sensor->mutex);
	crop = imx296_get_pad_crop(sensor, state, sel->pad, sel->which);
	format = imx296_get_pad_format(sensor, state, sel->pad, sel->which);

	if (rect.width != crop->width || rect.height != crop->height) {
		format->width = rect.width;
		format->height = rect.height;
	}

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    (rect.left != crop->left || rect.top != crop->top ||
	     rect.width != crop->width || rect.height != crop->height)) {
		sensor->roi_boundary_pending = true;
	}

	*crop = rect;
	sel->r = rect;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		imx296_setup_hblank(sensor, format->width);
		imx296_update_free_run_exposure_range_locked(sensor,
							    sensor->vblank->val);
	}

	mutex_unlock(&sensor->mutex);
	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx296_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx296 *sensor = to_imx296(sd);
	struct v4l2_mbus_framefmt *try_fmt;
	struct v4l2_rect *try_crop;

	mutex_lock(&sensor->mutex);
	try_fmt = v4l2_subdev_get_try_format(sd, fh->state, 0);
	*try_fmt = sensor->format;
	try_crop = v4l2_subdev_get_try_crop(sd, fh->state, 0);
	*try_crop = sensor->crop;
	mutex_unlock(&sensor->mutex);
	return 0;
}
#endif

static const struct v4l2_subdev_core_ops imx296_subdev_core_ops = {
	.s_power = imx296_s_power,
	.ioctl = imx296_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx296_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx296_subdev_video_ops = {
	.s_stream = imx296_s_stream,
	.g_frame_interval = imx296_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx296_subdev_pad_ops = {
	.enum_mbus_code = imx296_enum_mbus_code,
	.enum_frame_size = imx296_enum_frame_size,
	.get_fmt = imx296_get_format,
	.set_fmt = imx296_set_format,
	.get_selection = imx296_get_selection,
	.set_selection = imx296_set_selection,
	.get_mbus_config = imx296_g_mbus_config,
};

static const struct v4l2_subdev_ops imx296_subdev_ops = {
	.core = &imx296_subdev_core_ops,
	.video = &imx296_subdev_video_ops,
	.pad = &imx296_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx296_internal_ops = {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	.open = imx296_open,
#endif
};

static int imx296_subdev_init(struct imx296 *sensor)
{
	int ret;

	v4l2_i2c_subdev_init(&sensor->subdev, sensor->client, &imx296_subdev_ops);
	sensor->subdev.internal_ops = &imx296_internal_ops;

	ret = imx296_ctrls_init(sensor);
	if (ret < 0)
		return ret;

	sensor->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				 V4L2_SUBDEV_FL_HAS_EVENTS;

#if defined(CONFIG_MEDIA_CONTROLLER)
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->subdev.entity, 1, &sensor->pad);
	if (ret < 0) {
		v4l2_ctrl_handler_free(&sensor->ctrls);
		return ret;
	}
#endif

	return 0;
}

static void imx296_subdev_cleanup(struct imx296 *sensor)
{
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sensor->subdev.entity);
#endif
	v4l2_ctrl_handler_free(&sensor->ctrls);
}

static int __maybe_unused imx296_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct imx296 *sensor = to_imx296(subdev);

	return imx296_power_on(sensor);
}

static int __maybe_unused imx296_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct imx296 *sensor = to_imx296(subdev);

	imx296_power_off(sensor);
	return 0;
}

static const struct dev_pm_ops imx296_pm_ops = {
	SET_RUNTIME_PM_OPS(imx296_runtime_suspend, imx296_runtime_resume, NULL)
};

static int imx296_read_temperature(struct imx296 *sensor, int *temp)
{
	int tmdout;
	int ret;

	ret = imx296_write(sensor, IMX296_TMDCTRL, IMX296_TMDCTRL_LATCH, NULL);
	if (ret < 0)
		return ret;

	tmdout = imx296_read(sensor, IMX296_TMDOUT);
	if (tmdout < 0)
		return tmdout;

	tmdout &= IMX296_TMDOUT_MASK;
	*temp = 246312 - 304 * tmdout;

	return imx296_write(sensor, IMX296_TMDCTRL, 0, NULL);
}

static int imx296_identify_model(struct imx296 *sensor)
{
	unsigned int model;
	int temp = 0;
	int ret;

	model = (uintptr_t)of_device_get_match_data(sensor->dev);
	if (model) {
		sensor->mono = model & IMX296_SENSOR_INFO_MONO;
		return 0;
	}

	ret = imx296_write(sensor, IMX296_CTRL00, 0, NULL);
	if (ret < 0) {
		dev_err(sensor->dev,
			"failed to get sensor out of standby (%d)\n", ret);
		return ret;
	}

	usleep_range(30000, 35000);

	ret = imx296_read(sensor, IMX296_SENSOR_INFO);
	if (ret < 0) {
		dev_err(sensor->dev, "failed to read sensor information (%d)\n",
			ret);
		goto done;
	}

	model = (ret >> 6) & 0x1ff;
	switch (model) {
	case 296:
		sensor->mono = ret & IMX296_SENSOR_INFO_MONO;
		break;
	default:
		dev_err(sensor->dev, "invalid device model 0x%04x\n", ret);
		ret = -ENODEV;
		goto done;
	}

	ret = imx296_read_temperature(sensor, &temp);
	if (ret < 0)
		goto done;

	dev_info(sensor->dev, "found IMX%u%s (%u.%uC)\n", model,
		 sensor->mono ? "LL" : "LQ", temp / 1000, (temp / 100) % 10);

done:
	imx296_write(sensor, IMX296_CTRL00, IMX296_CTRL00_STANDBY, NULL);
	return ret;
}

static const struct regmap_range imx296_nonwriteable_ranges[] = {
	{
		.range_min = IMX296_SENSOR_INFO & IMX296_REG_ADDR_MASK,
		.range_max = (IMX296_SENSOR_INFO & IMX296_REG_ADDR_MASK) + 1,
	},
};

static const struct regmap_access_table imx296_writeable_table = {
	.no_ranges = imx296_nonwriteable_ranges,
	.n_no_ranges = ARRAY_SIZE(imx296_nonwriteable_ranges),
};

static const struct regmap_config imx296_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.wr_table = &imx296_writeable_table,
};

static int imx296_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	const char *sync_mode_name = NULL;
	struct v4l2_subdev *sd;
	struct imx296 *sensor;
	unsigned long clk_rate;
	unsigned int i;
	char facing[2];
	u32 trigger_mode = IMX296_FREE_RUN;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EIO;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &sensor->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &sensor->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &sensor->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &sensor->len_name);
	if (ret) {
		dev_err(dev, "could not get module information\n");
		return -EINVAL;
	}

	sensor->dev = dev;
	sensor->client = client;
	sensor->sync_mode = INTERNAL_MASTER_MODE;

	ret = of_property_read_string(node, RKMODULE_CAMERA_SYNC_MODE,
				      &sync_mode_name);
	if (!ret) {
		if (strcmp(sync_mode_name, RKMODULE_INTERNAL_MASTER_MODE) != 0)
			dev_warn(dev,
				 "sync-mode '%s' is unsupported for IMX296 fast trigger, forcing internal_master\n",
				 sync_mode_name);
	} else if (ret != -EINVAL) {
		dev_warn(dev,
			 "failed to read sync-mode (%d), defaulting to internal_master\n",
			 ret);
	}

	ret = of_property_read_u32(node, OF_IMX296_TRIGGER_MODE, &trigger_mode);
	if (ret || trigger_mode > IMX296_XTRIG_ONE_SHOT)
		trigger_mode = IMX296_FREE_RUN;

	sensor->pending_mode = trigger_mode;
	sensor->active_mode = trigger_mode;

	for (i = 0; i < ARRAY_SIZE(sensor->supplies); ++i)
		sensor->supplies[i].supply = imx296_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset-gpios\n");
	if (!sensor->reset_gpio) {
		sensor->reset_gpio = devm_gpiod_get_optional(dev, "pwdn",
							     GPIOD_OUT_HIGH);
		if (IS_ERR(sensor->reset_gpio))
			return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
					     "failed to get pwdn-gpios\n");
	}

	sensor->clk = devm_clk_get(dev, "inck");
	if (IS_ERR(sensor->clk))
		sensor->clk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(sensor->clk)) {
		dev_info(dev, "no external input clock, assuming on-board oscillator\n");
		sensor->clk = NULL;
	}

	sensor->trigger_pulse_us = IMX296_TRIGGER_PULSE_US_DEFAULT;
	ret = of_property_read_u32(node, OF_IMX296_TRIGGER_PULSE_US,
				   &trigger_mode);
	if (!ret) {
		if (trigger_mode < IMX296_TRIGGER_PULSE_US_MIN ||
		    trigger_mode > IMX296_TRIGGER_PULSE_US_MAX) {
			dev_warn(dev,
				 "trigger pulse width %u us is out of range, defaulting to %u us\n",
				 trigger_mode, IMX296_TRIGGER_PULSE_US_DEFAULT);
		} else {
			sensor->trigger_pulse_us = trigger_mode;
		}
	} else if (ret != -EINVAL) {
		return dev_err_probe(dev, ret,
				     "failed to read trigger pulse width\n");
	}

	sensor->light_source_enabled = false;
	sensor->light_source_active_level = false;
	sensor->light_source_advance_us = 0;
	sensor->light_source_off_delay_us = 0;

	sensor->light_source_gpio = devm_gpiod_get_optional(dev, "light-source",
							    GPIOD_ASIS);
	if (IS_ERR(sensor->light_source_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->light_source_gpio),
				     "failed to get light-source-gpios\n");

	{
		const char *active_level_name = NULL;

		ret = of_property_read_string(node, "light-source-active-level",
					      &active_level_name);
		if (!ret && active_level_name) {
			if (strcmp(active_level_name, "high") == 0)
				sensor->light_source_active_level = true;
			else if (strcmp(active_level_name, "low") != 0)
				dev_warn(dev,
					 "invalid light-source-active-level '%s', defaulting to low\n",
					 active_level_name);
		} else if (ret != -EINVAL) {
			dev_warn(dev,
				 "failed to read light-source-active-level (%d), defaulting to low\n",
				 ret);
		}
	}

	of_property_read_u32(node, "light-source-exposure-advance-us",
			     &sensor->light_source_advance_us);
	of_property_read_u32(node, "light-source-exposure-off-delay-us",
			     &sensor->light_source_off_delay_us);
	if (sensor->light_source_advance_us > 100000) {
		dev_warn(dev,
			 "light-source-exposure-advance-us %u out of range, clamp to 100000\n",
			 sensor->light_source_advance_us);
		sensor->light_source_advance_us = 100000;
	}
	if (sensor->light_source_off_delay_us > 1000000) {
		dev_warn(dev,
			 "light-source-exposure-off-delay-us %u out of range, clamp to 1000000\n",
			 sensor->light_source_off_delay_us);
		sensor->light_source_off_delay_us = 1000000;
	}

	sensor->trigger_pwm = imx296_devm_pwm_get_optional(dev, "trigger");
	if (IS_ERR(sensor->trigger_pwm))
		return dev_err_probe(dev, PTR_ERR(sensor->trigger_pwm),
				     "failed to get trigger pwm\n");

	sensor->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(sensor->pinctrl)) {
		sensor->pins_default =
			pinctrl_lookup_state(sensor->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(sensor->pins_default))
			sensor->pins_default = NULL;

		sensor->pins_sleep =
			pinctrl_lookup_state(sensor->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(sensor->pins_sleep))
			sensor->pins_sleep = NULL;

		sensor->pins_active_high =
			pinctrl_lookup_state(sensor->pinctrl, "active-high");
		if (IS_ERR(sensor->pins_active_high))
			sensor->pins_active_high = NULL;
	} else {
		sensor->pinctrl = NULL;
	}

	if (sensor->clk) {
		clk_rate = clk_get_rate(sensor->clk);
		for (i = 0; i < ARRAY_SIZE(imx296_clk_params); ++i) {
			if (clk_rate == imx296_clk_params[i].freq) {
				sensor->clk_params = &imx296_clk_params[i];
				break;
			}
		}

		if (!sensor->clk_params) {
			dev_err(dev, "unsupported clock rate %lu\n", clk_rate);
			return -EINVAL;
		}
	} else {
		/* Default to 37.125 MHz on-board oscillator parameters */
		sensor->clk_params = &imx296_clk_params[0];
		dev_info(dev, "using on-board oscillator clk params (%u Hz)\n",
			 imx296_clk_params[0].freq);
	}

	sensor->regmap = devm_regmap_init_i2c(client, &imx296_regmap_config);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);

	mutex_init(&sensor->mutex);

	ret = imx296_init_trigger_pwm(sensor);
	if (ret)
		goto err_destroy_mutex;

	ret = imx296_power_on(sensor);
	if (ret)
		goto err_destroy_mutex;

	ret = imx296_light_source_init_locked(sensor);
	if (ret)
		dev_warn(dev,
			 "failed to initialize light source gpio (%d)\n", ret);

	ret = imx296_identify_model(sensor);
	if (ret)
		goto err_power;

	sensor->crop.left = 0;
	sensor->crop.top = 0;
	sensor->crop.width = IMX296_PIXEL_ARRAY_WIDTH;
	sensor->crop.height = IMX296_PIXEL_ARRAY_HEIGHT;
	sensor->format.width = IMX296_PIXEL_ARRAY_WIDTH;
	sensor->format.height = IMX296_PIXEL_ARRAY_HEIGHT;
	sensor->format.code = imx296_mbus_code(sensor);
	sensor->format.field = V4L2_FIELD_NONE;
	sensor->format.colorspace = V4L2_COLORSPACE_RAW;
	sensor->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	sensor->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	sensor->format.xfer_func = V4L2_XFER_FUNC_NONE;

	ret = imx296_subdev_init(sensor);
	if (ret)
		goto err_power;

	sd = &sensor->subdev;
	memset(facing, 0, sizeof(facing));
	facing[0] = strcmp(sensor->module_facing, "back") == 0 ? 'b' : 'f';
	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 sensor->module_index, facing, IMX296_NAME, dev_name(dev));

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret)
		goto err_pm;

	ret = devm_device_add_group(dev, &imx296_attr_group);
	if (ret)
		goto err_subdev;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "driver version: %02x.%02x.%02x, default mode: %s\n",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff,
		 imx296_op_mode_name(sensor->pending_mode));
	dev_info(dev,
		 "sync mode: %s (single-camera XTRIG uses fast trigger in sensor master mode)\n",
		 imx296_sync_mode_name(sensor->sync_mode));
	if (sensor->trigger_pwm)
		dev_info(dev,
			 "trigger pwm ready: default low pulse=%u us\n",
			 sensor->trigger_pulse_us);

	return 0;

err_subdev:
	v4l2_async_unregister_subdev(sd);
err_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	imx296_subdev_cleanup(sensor);
err_power:
	imx296_power_off(sensor);
err_destroy_mutex:
	mutex_destroy(&sensor->mutex);
	return ret;
}

static void imx296_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx296 *sensor = to_imx296(sd);

	v4l2_async_unregister_subdev(sd);
	imx296_subdev_cleanup(sensor);
	mutex_destroy(&sensor->mutex);

	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		imx296_power_off(sensor);
	pm_runtime_set_suspended(sensor->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx296_of_match[] = {
	{ .compatible = "sony,imx296", .data = NULL },
	{ .compatible = "sony,imx296ll",
	  .data = (void *)IMX296_SENSOR_INFO_IMX296LL },
	{ .compatible = "sony,imx296lq",
	  .data = (void *)IMX296_SENSOR_INFO_IMX296LQ },
	{ },
};
MODULE_DEVICE_TABLE(of, imx296_of_match);
#endif

static const struct i2c_device_id imx296_match_id[] = {
	{ "sony,imx296", 0 },
	{ },
};

static struct i2c_driver imx296_i2c_driver = {
	.driver = {
		.name = IMX296_NAME,
		.pm = &imx296_pm_ops,
		.of_match_table = of_match_ptr(imx296_of_match),
	},
	.probe = imx296_probe,
	.remove = imx296_remove,
	.id_table = imx296_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx296_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx296_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony IMX296 Camera driver");
MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
