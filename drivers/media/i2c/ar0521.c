// SPDX-License-Identifier: GPL-2.0
/*
 * ON Semiconductor AR0521 MIPI CSI-2 sensor driver
 *
 * Copyright (C) 2021 Sieć Badawcza Łukasiewicz
 * - Przemysłowy Instytut Automatyki i Pomiarów PIAP
 * Written by Krzysztof Hałasa
 *
 * Rockchip / DLCVCAM integration, RAW10 4-lane, AND9573 slave trigger.
 */

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pwm.h>
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
#define AR0521_NAME				"ar0521"

#define AR0521_EXTCLK_MIN			(10 * 1000 * 1000)
#define AR0521_EXTCLK_MAX			(48 * 1000 * 1000)
#define AR0521_EXTCLK_DEFAULT			27000000U
#define AR0521_EXTCLK_EXPECTED			AR0521_EXTCLK_DEFAULT

#define AR0521_PLL_MIN				(320 * 1000 * 1000)
#define AR0521_PLL_MAX				(1280 * 1000 * 1000)

/*
 * AND9573 p18 Table 5, RAW10 Full, 27 MHz EXTCLK (register values as-is):
 *   vt_pix=5, vt_sys=1, pre1=pre2=3, mult1=89, mult2=115,
 *   op_pix=10, op_sys=1.
 *
 * RR 0x0304 / 0x0306 packing (pre2/pre1 and pll_multiplier2/1):
 *   0x0304 = (pre2 << 8) | pre1 = 0x0303
 *   0x0306 = (mult2 << 8) | mult1 = 0x7359
 *
 * Datasheet p10: PLL multipliers must be even; an odd programmed M is
 * used as M-1. Registers still write Table 5's 89/115; software clocks
 * use the effective even values 88/114. Do not mix nominal Table M with
 * derived VCO / VT / link / pixel rates.
 *
 * Effective clocks (27 MHz * Meff / pre):
 *   PLL1 VCO = 27e6 * 88 / 3 = 792 MHz
 *   PLL2 VCO = 27e6 * 114 / 3 = 1026 MHz
 *   VT pix   = PLL2 / vt_pix = 1026 / 5 = 205.2 MHz
 *   Word clk = PLL2 / op_pix = 1026 / 10 = 102.6 MHz
 *   MIPI bitrate/lane = PLL2 VCO = 1026 Mbps
 *   V4L2 link_freq    = bitrate/2 = 513 MHz
 *   4-lane RAW10 pixel_rate = 1026e6 * 4 / 10 = 410.4 MHz = 2 * VT
 *
 * RAW10 0x0112 = 0x0A0A; 4-lane 0x31AE = 0x0204.
 */
#define AR0521_PLL_PRE				3U
#define AR0521_PLL1_MULT_REG			89U
#define AR0521_PLL2_MULT_REG			115U
#define AR0521_PLL1_MULT_EFF			(AR0521_PLL1_MULT_REG & ~1U)
#define AR0521_PLL2_MULT_EFF			(AR0521_PLL2_MULT_REG & ~1U)
#define AR0521_VT_PIX_CLK_DIV			5U
#define AR0521_VT_SYS_CLK_DIV			1U
#define AR0521_OP_PIX_CLK_DIV			10U
#define AR0521_OP_SYS_CLK_DIV			1U
#define AR0521_PLL1_VCO_HZ			792000000ULL
#define AR0521_PLL2_VCO_HZ			1026000000ULL
#define AR0521_VT_PIX_CLK_HZ			205200000ULL
#define AR0521_WORD_CLK_HZ			102600000ULL
#define AR0521_MIPI_BITRATE_PER_LANE_HZ		1026000000ULL
#define AR0521_LINK_FREQ_HZ			513000000LL
#define AR0521_PIXEL_RATE			410400000ULL
#define AR0521_PIXEL_RATE_MIN			(168 * 1000 * 1000ULL)
#define AR0521_PIXEL_RATE_MAX			(420 * 1000 * 1000ULL)
#define AR0521_NUM_DATA_LANES			4U
#define AR0521_BITS_PER_SAMPLE			10U

/*
 * AND9484 default window is 2600x1952 starting at (4,4). Product mode
 * 2592x1944 is a symmetric 8-pixel crop (4 per side). No official
 * 2592x1944 table is published.
 */
#define AR0521_NATIVE_LEFT			4U
#define AR0521_NATIVE_TOP			4U
#define AR0521_NATIVE_WIDTH			2600U
#define AR0521_NATIVE_HEIGHT			1952U
#define AR0521_WIDTH_MIN			8U
#define AR0521_HEIGHT_MIN			8U
#define AR0521_WIDTH_MAX			2592U
#define AR0521_HEIGHT_MAX			1944U
#define AR0521_DEFAULT_LEFT			8U
#define AR0521_DEFAULT_TOP			8U

#define AR0521_WIDTH_BLANKING_MIN		572U
#define AR0521_HEIGHT_BLANKING_MIN		38U /* must be even */
#define AR0521_TOTAL_WIDTH_MIN			2968U
#define AR0521_TOTAL_HEIGHT_MAX			65535U
#define AR0521_CIT_MARGIN			4U
#define AR0521_MIN_FRAME_BLANK_LINES		28U

/*
 * Product default ~30 fps at 2592x1944, LLPCK/total_width = 3164.
 * FLL = round(pixel_rate / (LLPCK * 30)) = round(410.4e6 / (3164 * 30))
 *     = 4324 (even). VBLANK = FLL - 1944 = 2380 (even).
 * Constraints: even VBLANK; FLL >= active + 28 and CIT + 4.
 * Actual fps = 410.4e6 / (3164 * 4324) ≈ 29.998 fps. This is a product
 * default, not a Table 5 30 fps definition.
 */
#define AR0521_DEFAULT_TOTAL_HEIGHT		4324U
#define AR0521_HEIGHT_BLANKING_DEFAULT		(AR0521_DEFAULT_TOTAL_HEIGHT - \
						 AR0521_HEIGHT_MAX)

/*
 * V4L2 analogue gain is Q8 (256 = 1x). Hardware mapping uses R0x305E
 * (AND9573 Table 2 / RR global_gain): 1x = 0x2000.
 *   [15:7] digital gain / 64, 0x40 = 1x
 *   [6:4]  analog_coarse
 *   [3:0]  analog_fine
 * Analog-only coverage is 1x..16x (digital held at 1x). Do not program
 * R0x3EC4 companions from Table 2. R0x3028 is SMIA analogue_gain_code,
 * not the product analog path.
 */
#define AR0521_ANA_GAIN_MIN			256U	/* 1x */
#define AR0521_ANA_GAIN_MAX			4096U	/* 16x */
#define AR0521_ANA_GAIN_STEP			1U
#define AR0521_ANA_GAIN_DEFAULT			256U	/* 1x */
#define AR0521_GLOBAL_GAIN_DG_1X		0x40
#define AR0521_GLOBAL_GAIN_DG_SHIFT		7
#define AR0521_GLOBAL_GAIN_COARSE_SHIFT		4
#define AR0521_GLOBAL_GAIN_FINE_MASK		0x0F
#define AR0521_GLOBAL_GAIN_COARSE_MASK		0x07

#define AR0521_MODEL_ID				0x0457
#define AR0521_REG_MODEL_ID			0x3000
#define AR0521_REG_REVISION			0x0002

#define AR0521_REG_VT_PIX_CLK_DIV		0x0300
#define AR0521_REG_VT_SYS_CLK_DIV		0x0302
#define AR0521_REG_PRE_PLL_CLK_DIV		0x0304
#define AR0521_REG_PLL_MULTIPLIER		0x0306
#define AR0521_REG_OP_PIX_CLK_DIV		0x0308
#define AR0521_REG_OP_SYS_CLK_DIV		0x030A
#define AR0521_REG_FRAME_LENGTH_LINES		0x0340
#define AR0521_REG_LINE_LENGTH_PCK		0x0342
#define AR0521_REG_X_ADDR_START			0x0344
#define AR0521_REG_Y_ADDR_START			0x0346
#define AR0521_REG_X_ADDR_END			0x0348
#define AR0521_REG_Y_ADDR_END			0x034A
#define AR0521_REG_X_OUTPUT_SIZE			0x034C
#define AR0521_REG_Y_OUTPUT_SIZE			0x034E

#define AR0521_REG_COARSE_INTEGRATION_TIME	0x3012
#define AR0521_REG_ROW_SPEED			0x3016
#define AR0521_REG_RESET				0x301A
/* AND9573 RESET_REGISTER: 0x0218 standby, 0x021C streaming (bit2). */
#define   AR0521_REG_RESET_DEFAULTS		  0x0218
#define   AR0521_REG_RESET_GROUP_PARAM_HOLD	  0x8000
#define   AR0521_REG_RESET_GPI			  BIT(8)
#define   AR0521_REG_RESET_STREAM		  BIT(2)
#define   AR0521_REG_RESET_RESTART		  BIT(1)
#define   AR0521_REG_RESET_INIT			  BIT(0)

#define AR0521_REG_GPIO_GPO_1			0x30F8
#define AR0521_REG_GPIO_GPO_2			0x30FA
#define AR0521_REG_GPIO_CTRL2			0x3026
#define AR0521_REG_GROUPED_PARAMETER_HOLD	0x0104
#define AR0521_REG_GLOBAL_GAIN			0x305E
#define AR0521_REG_READ_MODE			0x3040
#define   AR0521_REG_READ_MODE_VERT_FLIP	  BIT(15)
#define   AR0521_REG_READ_MODE_HORIZ_MIRROR	  BIT(14)

#define AR0521_REG_HISPI_TEST_MODE		0x3066
#define AR0521_REG_HISPI_TEST_MODE_LP11		  0x0004
#define AR0521_REG_TEST_PATTERN_MODE		0x3070
#define AR0521_REG_SERIAL_FORMAT			0x31AE
#define AR0521_REG_SERIAL_FORMAT_MIPI		  0x0200
#define AR0521_REG_HISPI_TIMING			0x31BC
#define   AR0521_REG_HISPI_TIMING_CONT_TX_CLK	  BIT(15)
#define AR0521_REG_HISPI_CONTROL_STATUS		0x31C6
#define AR0521_REG_HISPI_CONTROL_STATUS_FRAMER_TEST_MODE_ENABLE 0x80
#define AR0521_REG_VD_TRIG_NEW_FRAME		0x3158
#define AR0521_REG_GLOBAL_SEQ_TRIGGER		0x315E
#define   AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR	  BIT(0)
#define   AR0521_REG_GLOBAL_SEQ_TRIGGER_BIT1	  BIT(1)
#define   AR0521_REG_GLOBAL_SEQ_TRIGGER_SCALE_MASK (BIT(5) | BIT(4))
#define AR0521_REG_GLOBAL_RST_END		0x3160
#define AR0521_REG_GLOBAL_READ_START		0x3166
#define AR0521_REG_GLOBAL_READ_START_HI		0x3168
/*
 * Non-bulb Triggered GRR: N_rst at 0x3160, 24-bit N_rd at 0x3166 +
 * 0x3168[7:0]. Scale code 0 (bits[5:4]=0) is 512 VT clocks/count.
 * Use VT pix 205.2 MHz, not the 410.4 MHz V4L2 pixel_rate.
 */
#define AR0521_GRR_N_RST			0x00EC
#define AR0521_GRR_SCALE			512U
#define AR0521_GRR_N_RD_MAX			0x00FFFFFFU
#define AR0521_TRIGGER_EXPOSURE_US_MAX		2000000U

#define AR0521_SLAVE_GPIO_GPO_1			0x0002
/*
 * AND9484 R0x3026 gpi_status:
 *   [15:13] standby_pin_select, [12:10] unused/RO, [9:7] trigger_pin_select,
 *   [6:4] saddr_pin_select, [3:0] gpi3..gpi0 status (RO).
 * Pin select encodings: 0=GPI0, 1=GPI1, 2=GPI2, 3=GPI3, 7=not controlled
 * (trigger/saddr). Trigger is an active-high VD input.
 *
 * AND9573 Table 7/8 maps Flash=GPI0, Shutter=GPI1, Trigger=GPI2.
 * AR0521/D pin table: FLASH package pin 39, TRIGGER package pin 51.
 *
 * Official recipes:
 *   p24 slave (GPIO0/FLASH VD): 0xFC70 -> trigger_pin_select=0 (GPI0/pin39)
 *   p23 GRR (TRIGGER pin VD):   0xFD70 -> trigger_pin_select=2 (GPI2/pin51)
 *
 * This board PWM (GPIO0_B5 / pwm1_ch1_m0) is wired to package pin 51
 * TRIGGER=GPI2, so use 0xFD70, not the p24 FLASH/GPIO0 value.
 */
#define AR0521_GPI_PIN_GPI2			2
#define AR0521_GPI_PIN_NONE			7
#define AR0521_GPI_STANDBY_PIN_SELECT_SHIFT	13
#define AR0521_GPI_UNUSED_SHIFT			10
#define AR0521_GPI_TRIGGER_PIN_SELECT_SHIFT	7
#define AR0521_GPI_SADDR_PIN_SELECT_SHIFT	4
#define AR0521_SLAVE_GPIO_CTRL2			\
	((AR0521_GPI_PIN_NONE << AR0521_GPI_STANDBY_PIN_SELECT_SHIFT) | \
	 (AR0521_GPI_PIN_NONE << AR0521_GPI_UNUSED_SHIFT) | \
	 (AR0521_GPI_PIN_GPI2 << AR0521_GPI_TRIGGER_PIN_SELECT_SHIFT) | \
	 (AR0521_GPI_PIN_NONE << AR0521_GPI_SADDR_PIN_SELECT_SHIFT))
static_assert(AR0521_SLAVE_GPIO_CTRL2 == 0xFD70,
	      "slave gpi_status must be AND9573 p23 TRIGGER/GPI2 recipe");
#define AR0521_SLAVE_VD_TRIG			0x8000

#define OF_CAMERA_PINCTRL_STATE_DEFAULT		"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP		"rockchip,camera_sleep"
#define OF_AR0521_TRIGGER_MODE			"trigger-mode"
#define OF_AR0521_TRIGGER_PULSE_US		"rockchip,trigger-pulse-us"
#define AR0521_TRIGGER_PULSE_US_DEFAULT		800U
#define AR0521_TRIGGER_PULSE_US_MIN		1U
#define AR0521_TRIGGER_PULSE_US_MAX		1000000U
#define AR0521_TRIGGER_PERIOD_NS_DEFAULT	10000000ULL
#define AR0521_TRIGGER_PERIOD_MARGIN_NS		10000000ULL

/*
 * CODA already owns USER_BASE+0x10e0. IMX296 uses +0x10d0. Pick an
 * unused 16-id window after DW100 (+0x1190).
 */
#define V4L2_CID_USER_AR0521_BASE		(V4L2_CID_USER_BASE + 0x11a0)
#define V4L2_CID_AR0521_OP_MODE			(V4L2_CID_USER_AR0521_BASE + 0x1)
#define V4L2_CID_AR0521_LIGHT_SOURCE_ENABLE	(V4L2_CID_USER_AR0521_BASE + 0x2)
#define V4L2_CID_AR0521_LIGHT_SOURCE_ACTIVE_LEVEL (V4L2_CID_USER_AR0521_BASE + 0x3)
#define V4L2_CID_AR0521_LIGHT_SOURCE_ADVANCE_US	(V4L2_CID_USER_AR0521_BASE + 0x4)
#define V4L2_CID_AR0521_LIGHT_SOURCE_OFF_DELAY_US (V4L2_CID_USER_AR0521_BASE + 0x5)

#define be					cpu_to_be16

enum ar0521_op_mode {
	AR0521_FREE_RUN = 0,
	AR0521_TRIGGER_ONE_SHOT = 1,
};

static const char * const ar0521_supply_names[] = {
	"vdd_io",
	"vdd",
	"vaa",
};

struct ar0521 {
	struct device *dev;
	struct i2c_client *client;
	struct clk *extclk;
	u32 extclk_freq;
	struct regulator *supplies[ARRAY_SIZE(ar0521_supply_names)];
	struct gpio_desc *reset_gpio;
	struct pwm_device *trigger_pwm;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_sleep;
	struct pinctrl_state *pins_active_high;
	struct mutex mutex; /* serialize streaming, controls, and trigger */

	bool streaming;
	bool power_on;
	bool sysfs_registered;
	unsigned int lane_count;

	enum rkmodule_sync_mode sync_mode;
	enum ar0521_op_mode active_mode;
	enum ar0521_op_mode pending_mode;
	u32 trigger_pulse_us;

	struct gpio_desc *light_source_gpio;
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

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_rect crop;
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

	u16 saved_gpio_gpo_1;
	u16 saved_gpio_ctrl2;
	u16 saved_vd_trig;
	u16 saved_global_seq;
	u16 saved_reset;
	u16 saved_global_rst_end;
	u16 saved_global_read_start;
	u16 saved_global_read_start_hi;
	bool slave_backup_valid;
};

static inline struct ar0521 *to_ar0521(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ar0521, sd);
}

static const char * const ar0521_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Faded color bars"
};

static const char * const ar0521_op_mode_menu[] = {
	"FREE_RUN",
	"XTRIG_ONE_SHOT",
};

static const s64 ar0521_link_freq_menu[] = {
	AR0521_LINK_FREQ_HZ,
};

static const u32 ar0521_mbus_codes[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
};

static const char *ar0521_op_mode_name(enum ar0521_op_mode mode)
{
	if (mode == AR0521_TRIGGER_ONE_SHOT)
		return "master_fast_trigger";
	return "free_run";
}

static const char *ar0521_sync_mode_name(enum rkmodule_sync_mode mode)
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

static int ar0521_parse_run_mode(const char *buf, enum ar0521_op_mode *mode)
{
	if (sysfs_streq(buf, "free_run") ||
	    sysfs_streq(buf, "normal") ||
	    sysfs_streq(buf, "continuous") ||
	    sysfs_streq(buf, "0")) {
		*mode = AR0521_FREE_RUN;
		return 0;
	}

	if (sysfs_streq(buf, "master_fast_trigger") ||
	    sysfs_streq(buf, "fast_trigger") ||
	    sysfs_streq(buf, "xtrig_one_shot") ||
	    sysfs_streq(buf, "trigger") ||
	    sysfs_streq(buf, "1")) {
		*mode = AR0521_TRIGGER_ONE_SHOT;
		return 0;
	}

	return -EINVAL;
}

static struct ar0521 *ar0521_from_dev(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	if (!sd)
		return NULL;

	return to_ar0521(sd);
}

static struct pwm_device *ar0521_devm_pwm_get_optional(struct device *dev,
						       const char *con_id)
{
	struct pwm_device *pwm;
	int ret;

	pwm = devm_pwm_get(dev, con_id);
	if (IS_ERR(pwm)) {
		ret = PTR_ERR(pwm);
		if (ret == -ENODEV || ret == -ENOENT)
			return NULL;
		return pwm;
	}

	return pwm;
}

static u64 ar0521_pwm_period_ns(struct pwm_device *pwm)
{
	struct pwm_state state;
	struct pwm_args args;

	if (!pwm)
		return AR0521_TRIGGER_PERIOD_NS_DEFAULT;

	pwm_get_state(pwm, &state);
	if (state.period)
		return state.period;

	pwm_get_args(pwm, &args);
	return args.period ? args.period : AR0521_TRIGGER_PERIOD_NS_DEFAULT;
}

static u64 ar0521_trigger_period_ns(u64 pulse_ns, u64 base_period_ns)
{
	u64 min_period_ns = pulse_ns + AR0521_TRIGGER_PERIOD_MARGIN_NS;

	if (base_period_ns < AR0521_TRIGGER_PERIOD_NS_DEFAULT)
		base_period_ns = AR0521_TRIGGER_PERIOD_NS_DEFAULT;

	return max(base_period_ns, min_period_ns);
}

static int ar0521_init_trigger_pwm(struct ar0521 *sensor)
{
	struct pwm_state state = { 0 };
	u64 pulse_ns;

	if (!sensor->trigger_pwm)
		return 0;

	pulse_ns = (u64)sensor->trigger_pulse_us * 1000ULL;
	state.period = ar0521_trigger_period_ns(pulse_ns,
						ar0521_pwm_period_ns(sensor->trigger_pwm));
	state.duty_cycle = 0;
	state.polarity = PWM_POLARITY_NORMAL;
	state.enabled = false;

	return pwm_apply_state(sensor->trigger_pwm, &state);
}

static int ar0521_light_source_value_locked(struct ar0521 *sensor, bool on)
{
	return on ? sensor->light_source_active_level :
		    !sensor->light_source_active_level;
}

static int ar0521_light_source_apply_pinctrl_locked(struct ar0521 *sensor)
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

static void ar0521_light_source_set_locked(struct ar0521 *sensor, bool on)
{
	if (!sensor->light_source_gpio)
		return;

	gpiod_set_value(sensor->light_source_gpio,
			ar0521_light_source_value_locked(sensor, on));
}

static int ar0521_light_source_init_locked(struct ar0521 *sensor)
{
	int idle_value;
	int ret;

	if (!sensor->light_source_gpio)
		return 0;

	ret = ar0521_light_source_apply_pinctrl_locked(sensor);
	if (ret)
		dev_dbg(sensor->dev,
			"failed to select light source pinctrl state (%d)\n", ret);

	idle_value = ar0521_light_source_value_locked(sensor, false);
	ret = gpiod_direction_output(sensor->light_source_gpio, idle_value);
	if (ret) {
		dev_err(sensor->dev,
			"failed to set light source gpio direction (%d)\n", ret);
		return ret;
	}

	ar0521_light_source_set_locked(sensor, false);
	return 0;
}

static void ar0521_sleep_us(u32 us)
{
	if (!us)
		return;
	if (us <= 1000)
		udelay(us);
	else
		usleep_range(us, us + max_t(u32, 20U, us / 10U));
}

static int ar0521_write_regs(struct ar0521 *sensor, const __be16 *data,
			     unsigned int count)
{
	struct i2c_client *client = sensor->client;
	struct i2c_msg msg;
	int ret;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = (u8 *)data;
	msg.len = count * sizeof(*data);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		if (ret >= 0)
			ret = -EIO;
		dev_err(sensor->dev, "I2C write error %d\n", ret);
		return ret;
	}

	return 0;
}

static int ar0521_write_reg(struct ar0521 *sensor, u16 reg, u16 val)
{
	__be16 buf[2] = { be(reg), be(val) };

	return ar0521_write_regs(sensor, buf, 2);
}

static int ar0521_read_reg(struct ar0521 *sensor, u16 reg, u16 *val)
{
	struct i2c_client *client = sensor->client;
	__be16 addr = be(reg);
	__be16 data = 0;
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = client->flags,
			.len = sizeof(addr),
			.buf = (u8 *)&addr,
		},
		{
			.addr = client->addr,
			.flags = client->flags | I2C_M_RD,
			.len = sizeof(data),
			.buf = (u8 *)&data,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret != 2) {
		if (ret >= 0)
			ret = -EIO;
		dev_err(sensor->dev, "I2C read 0x%04x failed: %d\n", reg, ret);
		return ret;
	}

	*val = be16_to_cpu(data);
	return 0;
}

static int ar0521_update_bits(struct ar0521 *sensor, u16 reg, u16 mask, u16 val)
{
	u16 cur;
	int ret;

	ret = ar0521_read_reg(sensor, reg, &cur);
	if (ret)
		return ret;

	cur = (cur & ~mask) | (val & mask);
	return ar0521_write_reg(sensor, reg, cur);
}

static int ar0521_group_hold(struct ar0521 *sensor, bool hold)
{
	/* SMIA grouped_parameter_hold alias; avoids whole-word 0x301A writes. */
	return ar0521_write_reg(sensor, AR0521_REG_GROUPED_PARAMETER_HOLD,
				hold ? 1 : 0);
}

static u32 ar0521_mbus_code(const struct ar0521 *sensor)
{
	unsigned int i = 0;

	if (sensor->vflip && sensor->hflip)
		i = (sensor->vflip->val ? 2 : 0) | (sensor->hflip->val ? 1 : 0);

	return ar0521_mbus_codes[i];
}

static u32 ar0521_total_width_locked(const struct ar0521 *sensor)
{
	u32 width = sensor->format.width;
	u32 hblank = sensor->hblank ? sensor->hblank->val :
				      AR0521_WIDTH_BLANKING_MIN;

	return max(width + hblank, AR0521_TOTAL_WIDTH_MIN);
}

static u32 ar0521_total_height_locked(const struct ar0521 *sensor)
{
	u32 height = sensor->format.height;
	u32 vblank = sensor->vblank ? sensor->vblank->val :
				      AR0521_HEIGHT_BLANKING_MIN;

	return min_t(u32, height + vblank, AR0521_TOTAL_HEIGHT_MAX);
}

static u64 ar0521_line_period_ns(const struct ar0521 *sensor)
{
	u32 total_width = ar0521_total_width_locked(sensor);

	/*
	 * 0x0342 is programmed with total_width (3164 at full FOV). That
	 * value is "twice the pixel-clocks-per-line" in the RR, so line
	 * time is total_width / (2 * vt) = total_width / PIXEL_RATE.
	 */
	return DIV_ROUND_CLOSEST_ULL((u64)total_width * 1000000000ULL,
				     AR0521_PIXEL_RATE);
}

static u32 ar0521_lines_to_exposure_us(const struct ar0521 *sensor, u32 lines)
{
	u64 exposure_ns = (u64)lines * ar0521_line_period_ns(sensor);

	return max_t(u32, 1, DIV_ROUND_CLOSEST_ULL(exposure_ns, 1000));
}

static u32 ar0521_exposure_us_to_lines_locked(struct ar0521 *sensor,
					      u32 exposure_us, u32 frame_lines)
{
	u64 request_ns = (u64)exposure_us * 1000ULL;
	u64 line_ns = ar0521_line_period_ns(sensor);
	u32 lines;
	u32 max_lines;

	max_lines = frame_lines > AR0521_CIT_MARGIN ?
		    frame_lines - AR0521_CIT_MARGIN : 1;
	lines = max_t(u32, 1, DIV_ROUND_CLOSEST_ULL(request_ns, line_ns));
	return clamp_t(u32, lines, 1, max_lines);
}

static enum ar0521_op_mode ar0521_ctrl_mode_locked(const struct ar0521 *sensor)
{
	return sensor->streaming ? sensor->active_mode : sensor->pending_mode;
}

static u32 ar0521_grr_min_exposure_us(void)
{
	/* Smallest integer us whose rounded GRR delta is at least 1. */
	return max_t(u32, 1,
		     DIV_ROUND_UP_ULL((u64)AR0521_GRR_SCALE * 1000000ULL,
				      2ULL * AR0521_VT_PIX_CLK_HZ));
}

static u32 ar0521_grr_exposure_us_to_n_rd(u32 exposure_us)
{
	u64 delta;
	u32 span = AR0521_GRR_N_RD_MAX - AR0521_GRR_N_RST;

	delta = DIV_ROUND_CLOSEST_ULL((u64)exposure_us * AR0521_VT_PIX_CLK_HZ,
				      (u64)AR0521_GRR_SCALE * 1000000ULL);
	if (delta < 1)
		delta = 1;
	if (delta > span)
		return AR0521_GRR_N_RD_MAX;
	return AR0521_GRR_N_RST + (u32)delta;
}

static int ar0521_write_grr_n_rd_locked(struct ar0521 *sensor, u32 n_rd)
{
	__be16 regs[] = {
		be(AR0521_REG_GLOBAL_READ_START),
		be(n_rd & 0xFFFF),
		be((n_rd >> 16) & 0xFF)
	};

	return ar0521_write_regs(sensor, regs, ARRAY_SIZE(regs));
}

static int ar0521_apply_grr_timing_locked(struct ar0521 *sensor, u32 exposure_us)
{
	int ret;

	ret = ar0521_write_reg(sensor, AR0521_REG_GLOBAL_RST_END,
			       AR0521_GRR_N_RST);
	if (ret)
		return ret;
	return ar0521_write_grr_n_rd_locked(sensor,
					    ar0521_grr_exposure_us_to_n_rd(exposure_us));
}

static int ar0521_config_grr_seq_trigger_locked(struct ar0521 *sensor)
{
	u16 mask = AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR |
		   AR0521_REG_GLOBAL_SEQ_TRIGGER_BIT1 |
		   AR0521_REG_GLOBAL_SEQ_TRIGGER_SCALE_MASK;
	u16 val = AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR;

	return ar0521_update_bits(sensor, AR0521_REG_GLOBAL_SEQ_TRIGGER,
				  mask, val);
}

static void ar0521_update_exposure_range_locked(struct ar0521 *sensor,
						enum ar0521_op_mode mode,
						u32 vblank)
{
	u32 min_us;
	u32 max_us;
	u32 def_us;

	if (!sensor->exposure)
		return;

	if (mode == AR0521_TRIGGER_ONE_SHOT) {
		min_us = ar0521_grr_min_exposure_us();
		max_us = AR0521_TRIGGER_EXPOSURE_US_MAX;
	} else {
		u32 frame_lines = sensor->format.height + vblank;
		u32 max_lines = frame_lines > AR0521_CIT_MARGIN ?
				frame_lines - AR0521_CIT_MARGIN : 1;

		min_us = ar0521_lines_to_exposure_us(sensor, 1);
		max_us = ar0521_lines_to_exposure_us(sensor, max_lines);
	}

	def_us = clamp_val(ar0521_lines_to_exposure_us(sensor, 360),
			   min_us, max_us);
	__v4l2_ctrl_modify_range(sensor->exposure, min_us, max_us, 1, def_us);
}

static void ar0521_setup_hblank(struct ar0521 *sensor, unsigned int width)
{
	unsigned int min_total = max(width + AR0521_WIDTH_BLANKING_MIN,
				     AR0521_TOTAL_WIDTH_MIN);
	unsigned int hblank = min_total - width;

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

static void ar0521_update_ctrl_visibility_locked(struct ar0521 *sensor,
						 enum ar0521_op_mode mode)
{
	/* Exposure/VBLANK apply in both FREE_RUN and TRIGGER. */
	if (sensor->exposure)
		v4l2_ctrl_activate(sensor->exposure, true);
	if (sensor->vblank)
		v4l2_ctrl_activate(sensor->vblank, true);
}

static struct v4l2_rect *
ar0521_get_pad_crop(struct ar0521 *sensor, struct v4l2_subdev_state *state,
		    unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&sensor->sd, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->crop;
	}

	return NULL;
}

static struct v4l2_mbus_framefmt *
ar0521_get_pad_format(struct ar0521 *sensor, struct v4l2_subdev_state *state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&sensor->sd, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->format;
	}

	return NULL;
}

static void ar0521_get_module_inf(struct ar0521 *sensor, struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, AR0521_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, sensor->module_name, sizeof(inf->base.module));
	strscpy(inf->base.lens, sensor->len_name, sizeof(inf->base.lens));
}

static int ar0521_set_geometry(struct ar0521 *sensor)
{
	u16 x = sensor->crop.left;
	u16 y = sensor->crop.top;
	u16 width = sensor->format.width;
	u16 height = sensor->format.height;
	u16 total_width = ar0521_total_width_locked(sensor);
	u16 total_height = height + (sensor->vblank ? sensor->vblank->val :
						      AR0521_HEIGHT_BLANKING_MIN);
	__be16 regs[] = {
		be(AR0521_REG_FRAME_LENGTH_LINES),
		be(total_height),
		be(total_width),
		be(x),
		be(y),
		be(x + width - 1),
		be(y + height - 1),
		be(width),
		be(height)
	};

	return ar0521_write_regs(sensor, regs, ARRAY_SIZE(regs));
}

static int ar0521_write_pll(struct ar0521 *sensor)
{
	/* Table 5 RAW10 Full: 0x0304=0x0303, 0x0306=0x7359 (odd M as-is). */
	__be16 pll_regs[] = {
		be(AR0521_REG_VT_PIX_CLK_DIV),
		be(AR0521_VT_PIX_CLK_DIV),
		be(AR0521_VT_SYS_CLK_DIV),
		be((AR0521_PLL_PRE << 8) | AR0521_PLL_PRE),
		be((AR0521_PLL2_MULT_REG << 8) | AR0521_PLL1_MULT_REG),
		be(AR0521_OP_PIX_CLK_DIV),
		be(AR0521_OP_SYS_CLK_DIV)
	};

	return ar0521_write_regs(sensor, pll_regs, ARRAY_SIZE(pll_regs));
}

static u16 ar0521_q8_to_global_gain(u32 gain_q8)
{
	u32 coarse;
	u32 remainder_q8;
	u32 fine;

	gain_q8 = clamp_t(u32, gain_q8, AR0521_ANA_GAIN_MIN, AR0521_ANA_GAIN_MAX);

	/*
	 * Analog coarse is a power-of-two stage (1/2/4/8/16). Fine is a
	 * 4-bit linear fraction of that stage. Digital stays at 1x (0x40).
	 * Integer stages match AND9573 Table 2 with digital = 1x:
	 * 1x=0x2000, 2x=0x2010, 4x=0x2020, 8x=0x2030, 16x=0x2040.
	 */
	if (gain_q8 >= 4096)
		coarse = 4;
	else if (gain_q8 >= 2048)
		coarse = 3;
	else if (gain_q8 >= 1024)
		coarse = 2;
	else if (gain_q8 >= 512)
		coarse = 1;
	else
		coarse = 0;

	remainder_q8 = gain_q8 - (256U << coarse);
	fine = DIV_ROUND_CLOSEST(remainder_q8 << 4, 256U << coarse);
	if (fine > AR0521_GLOBAL_GAIN_FINE_MASK) {
		if (coarse < AR0521_GLOBAL_GAIN_COARSE_MASK) {
			coarse++;
			fine = 0;
		} else {
			fine = AR0521_GLOBAL_GAIN_FINE_MASK;
		}
	}

	return (AR0521_GLOBAL_GAIN_DG_1X << AR0521_GLOBAL_GAIN_DG_SHIFT) |
	       ((coarse & AR0521_GLOBAL_GAIN_COARSE_MASK) <<
		AR0521_GLOBAL_GAIN_COARSE_SHIFT) |
	       (fine & AR0521_GLOBAL_GAIN_FINE_MASK);
}

static int ar0521_apply_analog_gain_locked(struct ar0521 *sensor, u32 value)
{
	return ar0521_write_reg(sensor, AR0521_REG_GLOBAL_GAIN,
				ar0521_q8_to_global_gain(value));
}

static int ar0521_apply_flip_locked(struct ar0521 *sensor)
{
	u16 val = 0;

	if (sensor->vflip && sensor->vflip->val)
		val |= AR0521_REG_READ_MODE_VERT_FLIP;
	if (sensor->hflip && sensor->hflip->val)
		val |= AR0521_REG_READ_MODE_HORIZ_MIRROR;

	return ar0521_update_bits(sensor, AR0521_REG_READ_MODE,
				  AR0521_REG_READ_MODE_VERT_FLIP |
				  AR0521_REG_READ_MODE_HORIZ_MIRROR, val);
}

static int ar0521_apply_test_pattern_locked(struct ar0521 *sensor, u32 value)
{
	return ar0521_write_reg(sensor, AR0521_REG_TEST_PATTERN_MODE, value);
}

static int ar0521_apply_vblank_locked(struct ar0521 *sensor, u32 vblank)
{
	u16 total_height = sensor->format.height + vblank;

	return ar0521_write_reg(sensor, AR0521_REG_FRAME_LENGTH_LINES,
				total_height);
}

static int ar0521_apply_exposure_locked(struct ar0521 *sensor,
					enum ar0521_op_mode mode,
					u32 exposure_us)
{
	u32 frame_lines = sensor->format.height +
			  (sensor->vblank ? sensor->vblank->val :
					    AR0521_HEIGHT_BLANKING_MIN);
	u32 lines = ar0521_exposure_us_to_lines_locked(sensor, exposure_us,
						       frame_lines);
	u32 min_fll = lines + AR0521_CIT_MARGIN;
	int ret;

	/* TRIGGER: keep 0x3012 inside current FLL-4; do not grow FLL. */
	if (mode != AR0521_TRIGGER_ONE_SHOT && frame_lines < min_fll) {
		frame_lines = min_fll;
		ret = ar0521_write_reg(sensor, AR0521_REG_FRAME_LENGTH_LINES,
				       frame_lines);
		if (ret)
			return ret;
	}

	return ar0521_write_reg(sensor, AR0521_REG_COARSE_INTEGRATION_TIME,
				lines);
}

static int ar0521_stream_bit(struct ar0521 *sensor, bool on)
{
	return ar0521_update_bits(sensor, AR0521_REG_RESET,
				  AR0521_REG_RESET_STREAM,
				  on ? AR0521_REG_RESET_STREAM : 0);
}

static int ar0521_restore_slave_backup_locked(struct ar0521 *sensor)
{
	int ret;

	if (!sensor->slave_backup_valid)
		return 0;

	ret = ar0521_write_reg(sensor, AR0521_REG_GPIO_GPO_1,
			       sensor->saved_gpio_gpo_1);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_GPIO_CTRL2,
			       sensor->saved_gpio_ctrl2);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_VD_TRIG_NEW_FRAME,
			       sensor->saved_vd_trig);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_GLOBAL_SEQ_TRIGGER,
			       sensor->saved_global_seq);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_GLOBAL_RST_END,
			       sensor->saved_global_rst_end);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_GLOBAL_READ_START,
			       sensor->saved_global_read_start);
	if (ret)
		return ret;
	ret = ar0521_write_reg(sensor, AR0521_REG_GLOBAL_READ_START_HI,
			       sensor->saved_global_read_start_hi);
	if (ret)
		return ret;

	return ar0521_update_bits(sensor, AR0521_REG_RESET,
				  AR0521_REG_RESET_GPI,
				  sensor->saved_reset & AR0521_REG_RESET_GPI);
}

static int ar0521_enter_slave_locked(struct ar0521 *sensor)
{
	int ret;
	int rb;

	if (!sensor->slave_backup_valid) {
		ret = ar0521_read_reg(sensor, AR0521_REG_GPIO_GPO_1,
				      &sensor->saved_gpio_gpo_1);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_GPIO_CTRL2,
				      &sensor->saved_gpio_ctrl2);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_VD_TRIG_NEW_FRAME,
				      &sensor->saved_vd_trig);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_GLOBAL_SEQ_TRIGGER,
				      &sensor->saved_global_seq);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_RESET,
				      &sensor->saved_reset);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_GLOBAL_RST_END,
				      &sensor->saved_global_rst_end);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_GLOBAL_READ_START,
				      &sensor->saved_global_read_start);
		if (ret)
			return ret;
		ret = ar0521_read_reg(sensor, AR0521_REG_GLOBAL_READ_START_HI,
				      &sensor->saved_global_read_start_hi);
		if (ret)
			return ret;
		sensor->slave_backup_valid = true;
	}

	/* AND9573 p23/p24: stop stream, then GPI/VD/GRR, then stream.
	 * 0x3026 uses TRIGGER/GPI2 (0xFD70), not FLASH/GPI0 (0xFC70).
	 */
	ret = ar0521_stream_bit(sensor, false);
	if (ret)
		goto rollback;
	ret = ar0521_write_reg(sensor, AR0521_REG_GPIO_GPO_1,
			       AR0521_SLAVE_GPIO_GPO_1);
	if (ret)
		goto rollback;
	ret = ar0521_update_bits(sensor, AR0521_REG_RESET,
				 AR0521_REG_RESET_GPI, AR0521_REG_RESET_GPI);
	if (ret)
		goto rollback;
	ret = ar0521_write_reg(sensor, AR0521_REG_GPIO_CTRL2,
			       AR0521_SLAVE_GPIO_CTRL2);
	if (ret)
		goto rollback;
	ret = ar0521_write_reg(sensor, AR0521_REG_VD_TRIG_NEW_FRAME,
			       AR0521_SLAVE_VD_TRIG);
	if (ret)
		goto rollback;
	ret = ar0521_config_grr_seq_trigger_locked(sensor);
	if (ret)
		goto rollback;
	ret = ar0521_apply_grr_timing_locked(sensor, sensor->exposure->val);
	if (ret)
		goto rollback;

	ret = ar0521_stream_bit(sensor, true);
	if (ret)
		goto rollback;
	return 0;

rollback:
	rb = ar0521_restore_slave_backup_locked(sensor);
	if (rb) {
		dev_err(sensor->dev,
			"enter_slave rollback restore failed (%d)\n", rb);
	} else {
		sensor->slave_backup_valid = false;
	}
	return ret;
}

static int ar0521_exit_slave_locked(struct ar0521 *sensor)
{
	int ret;

	if (!sensor->slave_backup_valid)
		return 0;

	ret = ar0521_stream_bit(sensor, false);
	if (ret)
		return ret;

	ret = ar0521_restore_slave_backup_locked(sensor);
	if (ret)
		return ret;

	sensor->slave_backup_valid = false;
	return 0;
}

static int ar0521_apply_mode_regs_locked(struct ar0521 *sensor,
					 enum ar0521_op_mode mode)
{
	if (mode == AR0521_TRIGGER_ONE_SHOT)
		return ar0521_enter_slave_locked(sensor);

	return ar0521_exit_slave_locked(sensor);
}

static int ar0521_restore_ctrls_for_mode_locked(struct ar0521 *sensor,
						enum ar0521_op_mode mode)
{
	int ret;

	ret = ar0521_group_hold(sensor, true);
	if (ret)
		return ret;

	ret = ar0521_apply_analog_gain_locked(sensor, sensor->anal_gain->val);
	if (ret)
		goto out;
	ret = ar0521_apply_flip_locked(sensor);
	if (ret)
		goto out;
	ret = ar0521_apply_test_pattern_locked(sensor,
					       sensor->test_pattern->val);
	if (ret)
		goto out;
	ret = ar0521_set_geometry(sensor);
	if (ret)
		goto out;
	ar0521_update_exposure_range_locked(sensor, mode,
					    sensor->vblank->val);
	ret = ar0521_apply_vblank_locked(sensor, sensor->vblank->val);
	if (ret)
		goto out;
	ret = ar0521_apply_exposure_locked(sensor, mode,
					   sensor->exposure->val);

out:
	{
		int hold_ret = ar0521_group_hold(sensor, false);

		if (!ret)
			ret = hold_ret;
	}
	return ret;
}

static int ar0521_exit_lp11_locked(struct ar0521 *sensor)
{
	int ret;

	/*
	 * power_on programs HISPI_TEST_MODE LP11 for PHY bring-up
	 * (4-lane readback 0x03C4). Clear the test-mode word before
	 * FS/FE so lanes leave LP11 test. RR: 0x3066 is lane/test.
	 */
	ret = ar0521_write_reg(sensor, AR0521_REG_HISPI_TEST_MODE, 0x0000);
	if (ret)
		return ret;

	/*
	 * RK DTS has no clock-noncontinuous; CSI expects a continuous
	 * clock. Keep other 0x31BC timing bits; set cont_tx_clk only
	 * (0x068C -> 0x868C).
	 */
	ret = ar0521_update_bits(sensor, AR0521_REG_HISPI_TIMING,
				 AR0521_REG_HISPI_TIMING_CONT_TX_CLK,
				 AR0521_REG_HISPI_TIMING_CONT_TX_CLK);
	if (ret)
		return ret;

	return ar0521_write_reg(sensor, AR0521_REG_HISPI_CONTROL_STATUS, 0);
}

static int ar0521_stream_on_bits(struct ar0521 *sensor)
{
	int ret;

	ret = ar0521_exit_lp11_locked(sensor);
	if (ret)
		return ret;

	/*
	 * Mainline AR0521 writes RESET_DEFAULTS|STREAM as a whole word
	 * rather than OR-ing STREAM onto whatever RESET currently holds.
	 * Leaving leftover RESET bits (INIT/RESTART/GROUP_HOLD) can keep
	 * MIPI in LP-11 / no FS-FE.
	 */
	ret = ar0521_write_reg(sensor, AR0521_REG_RESET,
			       AR0521_REG_RESET_DEFAULTS |
			       AR0521_REG_RESET_STREAM);
	if (ret)
		return ret;

	__v4l2_ctrl_grab(sensor->vflip, 1);
	__v4l2_ctrl_grab(sensor->hflip, 1);
	return 0;
}

static int ar0521_stream_off_bits(struct ar0521 *sensor)
{
	int ret;

	ret = ar0521_stream_bit(sensor, false);
	if (ret)
		return ret;

	__v4l2_ctrl_grab(sensor->vflip, 0);
	__v4l2_ctrl_grab(sensor->hflip, 0);
	return 0;
}

static int ar0521_quick_stream(struct ar0521 *sensor, bool on)
{
	int ret;

	if (!sensor->streaming || !pm_runtime_active(sensor->dev))
		return -EINVAL;

	if (on) {
		ret = ar0521_exit_lp11_locked(sensor);
		if (ret)
			return ret;
		if (sensor->active_mode == AR0521_TRIGGER_ONE_SHOT) {
			ret = ar0521_update_bits(sensor, AR0521_REG_RESET,
						 AR0521_REG_RESET_GPI,
						 AR0521_REG_RESET_GPI);
			if (ret)
				return ret;
		}
		ret = ar0521_stream_bit(sensor, true);
		if (ret)
			return ret;
		if (sensor->active_mode == AR0521_FREE_RUN &&
		    sensor->light_source_enabled)
			ar0521_light_source_set_locked(sensor, true);
		return 0;
	}

	ar0521_light_source_set_locked(sensor, false);
	return ar0521_stream_bit(sensor, false);
}

static int ar0521_trigger_once_locked(struct ar0521 *sensor)
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
			   AR0521_TRIGGER_PULSE_US_MIN,
			   AR0521_TRIGGER_PULSE_US_MAX);
	duty_ns = (u64)pulse_us * 1000ULL;
	advance_us = sensor->light_source_enabled ?
		     sensor->light_source_advance_us : 0;
	off_delay_us = sensor->light_source_enabled ?
		       sensor->light_source_off_delay_us : 0;

	if (sensor->light_source_enabled && sensor->light_source_gpio) {
		ar0521_light_source_set_locked(sensor, true);
		ar0521_sleep_us(advance_us);
	}

	pwm_get_state(sensor->trigger_pwm, &state);
	state.period = ar0521_trigger_period_ns(duty_ns,
						ar0521_pwm_period_ns(sensor->trigger_pwm));
	state.duty_cycle = duty_ns;
	state.polarity = PWM_POLARITY_NORMAL;
	state.enabled = true;

	ret = pwm_apply_state(sensor->trigger_pwm, &state);
	if (ret)
		goto out_light;

	ar0521_sleep_us(pulse_us);

	state.duty_cycle = 0;
	state.enabled = false;
	ret = pwm_apply_state(sensor->trigger_pwm, &state);
	if (ret)
		goto out_light;

	dev_info(sensor->dev,
		 "trigger pulse emitted: width=%u us idle=low active=high mode=%s streaming=%u\n",
		 pulse_us,
		 ar0521_op_mode_name(sensor->streaming ?
				     sensor->active_mode :
				     sensor->pending_mode),
		 sensor->streaming);

	if (sensor->streaming && sensor->active_mode != AR0521_TRIGGER_ONE_SHOT)
		dev_warn(sensor->dev,
			 "trigger pulse was emitted while active mode is %s; switch run_mode to master_fast_trigger for one-shot capture\n",
			 ar0521_op_mode_name(sensor->active_mode));

out_light:
	if (sensor->light_source_enabled && sensor->light_source_gpio) {
		ar0521_sleep_us(off_delay_us);
		ar0521_light_source_set_locked(sensor, false);
	}

	return ret;
}

static int ar0521_set_ctrl(struct v4l2_ctrl *ctrl);
static int ar0521_mode_switch(struct ar0521 *sensor, enum ar0521_op_mode new_mode);

static const struct v4l2_ctrl_ops ar0521_ctrl_ops = {
	.s_ctrl = ar0521_set_ctrl,
};

static const struct v4l2_ctrl_config ar0521_op_mode_ctrl_cfg = {
	.ops = &ar0521_ctrl_ops,
	.id = V4L2_CID_AR0521_OP_MODE,
	.type = V4L2_CTRL_TYPE_MENU,
	.name = "ar0521_mode",
	.min = AR0521_FREE_RUN,
	.max = AR0521_TRIGGER_ONE_SHOT,
	.def = AR0521_FREE_RUN,
	.qmenu = ar0521_op_mode_menu,
};

static const struct v4l2_ctrl_config ar0521_light_source_enable_cfg = {
	.ops = &ar0521_ctrl_ops,
	.id = V4L2_CID_AR0521_LIGHT_SOURCE_ENABLE,
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.name = "light_source_enable",
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config ar0521_light_source_active_level_cfg = {
	.ops = &ar0521_ctrl_ops,
	.id = V4L2_CID_AR0521_LIGHT_SOURCE_ACTIVE_LEVEL,
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.name = "light_source_active_level",
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config ar0521_light_source_advance_us_cfg = {
	.ops = &ar0521_ctrl_ops,
	.id = V4L2_CID_AR0521_LIGHT_SOURCE_ADVANCE_US,
	.type = V4L2_CTRL_TYPE_INTEGER,
	.name = "light_source_advance_us",
	.min = 0,
	.max = 100000,
	.step = 1,
	.def = 0,
};

static const struct v4l2_ctrl_config ar0521_light_source_off_delay_us_cfg = {
	.ops = &ar0521_ctrl_ops,
	.id = V4L2_CID_AR0521_LIGHT_SOURCE_OFF_DELAY_US,
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
	struct ar0521 *sensor = ar0521_from_dev(dev);
	ssize_t len;

	if (!sensor)
		return -ENODEV;

	mutex_lock(&sensor->mutex);
	len = sysfs_emit(buf,
			 "pending=%s\nactive=%s\nstreaming=%u\navailable=free_run master_fast_trigger\n",
			 ar0521_op_mode_name(sensor->pending_mode),
			 ar0521_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return len;
}

static ssize_t run_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ar0521 *sensor = ar0521_from_dev(dev);
	enum ar0521_op_mode mode;
	int ret;

	if (!sensor || !sensor->op_mode_ctrl)
		return -ENODEV;

	ret = ar0521_parse_run_mode(buf, &mode);
	if (ret)
		return ret;

	ret = v4l2_ctrl_s_ctrl(sensor->op_mode_ctrl, mode);
	if (ret)
		return ret;

	mutex_lock(&sensor->mutex);
	if (sensor->pending_mode != mode)
		dev_warn(dev,
			 "run mode request=%s but state is pending=%s active=%s streaming=%u\n",
			 ar0521_op_mode_name(mode),
			 ar0521_op_mode_name(sensor->pending_mode),
			 ar0521_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	else
		dev_info(dev,
			 "run mode request=%s pending=%s active=%s streaming=%u\n",
			 ar0521_op_mode_name(mode),
			 ar0521_op_mode_name(sensor->pending_mode),
			 ar0521_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return count;
}

static DEVICE_ATTR_RW(run_mode);

static ssize_t trigger_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct ar0521 *sensor = ar0521_from_dev(dev);
	ssize_t len;

	if (!sensor)
		return -ENODEV;

	mutex_lock(&sensor->mutex);
	len = sysfs_emit(buf,
			 "available=echo 1 > trigger\npulse_us=%u\nidle=low\nactive=high\npwm_present=%u\npending=%s\nactive_mode=%s\nstreaming=%u\n",
			 sensor->trigger_pulse_us,
			 !!sensor->trigger_pwm,
			 ar0521_op_mode_name(sensor->pending_mode),
			 ar0521_op_mode_name(sensor->active_mode),
			 sensor->streaming);
	mutex_unlock(&sensor->mutex);

	return len;
}

static ssize_t trigger_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ar0521 *sensor = ar0521_from_dev(dev);
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
	ret = ar0521_trigger_once_locked(sensor);
	mutex_unlock(&sensor->mutex);

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(trigger);

static ssize_t trigger_pulse_us_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ar0521 *sensor = ar0521_from_dev(dev);
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
	struct ar0521 *sensor = ar0521_from_dev(dev);
	unsigned int pulse_us;

	if (!sensor)
		return -ENODEV;

	if (kstrtouint(buf, 0, &pulse_us))
		return -EINVAL;
	if (pulse_us < AR0521_TRIGGER_PULSE_US_MIN ||
	    pulse_us > AR0521_TRIGGER_PULSE_US_MAX)
		return -ERANGE;

	mutex_lock(&sensor->mutex);
	sensor->trigger_pulse_us = pulse_us;
	mutex_unlock(&sensor->mutex);

	dev_info(dev, "trigger pulse width set to %u us\n", pulse_us);
	return count;
}

static DEVICE_ATTR_RW(trigger_pulse_us);

static struct attribute *ar0521_attrs[] = {
	&dev_attr_run_mode.attr,
	&dev_attr_trigger.attr,
	&dev_attr_trigger_pulse_us.attr,
	NULL
};

static const struct attribute_group ar0521_attr_group = {
	.attrs = ar0521_attrs,
};

static int ar0521_ctrls_init(struct ar0521 *sensor)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *handler = &sensor->ctrls;
	u32 def_exposure_us;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret < 0)
		return ret;

	ret = v4l2_ctrl_handler_init(handler, 20);
	if (ret)
		return ret;

	handler->lock = &sensor->mutex;

	def_exposure_us = ar0521_lines_to_exposure_us(sensor, 360);
	sensor->exposure = v4l2_ctrl_new_std(handler, &ar0521_ctrl_ops,
					     V4L2_CID_EXPOSURE, 1,
					     AR0521_TRIGGER_EXPOSURE_US_MAX, 1,
					     def_exposure_us);
	sensor->anal_gain = v4l2_ctrl_new_std(handler, &ar0521_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      AR0521_ANA_GAIN_MIN,
					      AR0521_ANA_GAIN_MAX,
					      AR0521_ANA_GAIN_STEP,
					      AR0521_ANA_GAIN_DEFAULT);

	sensor->hflip = v4l2_ctrl_new_std(handler, &ar0521_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (sensor->hflip)
		sensor->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	sensor->vflip = v4l2_ctrl_new_std(handler, &ar0521_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (sensor->vflip)
		sensor->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ar0521_setup_hblank(sensor, AR0521_WIDTH_MAX);

	sensor->vblank = v4l2_ctrl_new_std(handler, &ar0521_ctrl_ops,
					   V4L2_CID_VBLANK,
					   AR0521_HEIGHT_BLANKING_MIN,
					   AR0521_TOTAL_HEIGHT_MAX -
					   AR0521_HEIGHT_MAX, 2,
					   AR0521_HEIGHT_BLANKING_DEFAULT);

	sensor->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(ar0521_link_freq_menu) - 1,
						   0, ar0521_link_freq_menu);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
					       V4L2_CID_PIXEL_RATE,
					       AR0521_PIXEL_RATE_MIN,
					       AR0521_PIXEL_RATE_MAX, 1,
					       AR0521_PIXEL_RATE);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->test_pattern =
		v4l2_ctrl_new_std_menu_items(handler, &ar0521_ctrl_ops,
					     V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(ar0521_test_pattern_menu) - 1,
					     0, 0, ar0521_test_pattern_menu);
	sensor->op_mode_ctrl = v4l2_ctrl_new_custom(handler,
						    &ar0521_op_mode_ctrl_cfg,
						    NULL);
	sensor->light_source_enable_ctrl =
		v4l2_ctrl_new_custom(handler, &ar0521_light_source_enable_cfg,
				     NULL);
	sensor->light_source_active_level_ctrl =
		v4l2_ctrl_new_custom(handler,
				     &ar0521_light_source_active_level_cfg, NULL);
	sensor->light_source_advance_us_ctrl =
		v4l2_ctrl_new_custom(handler, &ar0521_light_source_advance_us_cfg,
				     NULL);
	sensor->light_source_off_delay_us_ctrl =
		v4l2_ctrl_new_custom(handler,
				     &ar0521_light_source_off_delay_us_cfg, NULL);

	v4l2_ctrl_new_fwnode_properties(handler, &ar0521_ctrl_ops, &props);

	if (handler->error) {
		ret = handler->error;
		dev_err(sensor->dev, "failed to add controls (%d)\n", ret);
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	if (sensor->op_mode_ctrl) {
		mutex_lock(&sensor->mutex);
		ret = __v4l2_ctrl_s_ctrl(sensor->op_mode_ctrl,
					 sensor->pending_mode);
		mutex_unlock(&sensor->mutex);
		if (ret < 0) {
			dev_err(sensor->dev,
				"failed to sync default run mode control (%d)\n",
				ret);
			v4l2_ctrl_handler_free(handler);
			return ret;
		}
	}

	sensor->sd.ctrl_handler = handler;
	ar0521_update_exposure_range_locked(sensor, sensor->pending_mode,
					    sensor->vblank->val);
	ar0521_update_ctrl_visibility_locked(sensor, sensor->pending_mode);
	return 0;
}

#define REGS_ENTRY(a)	{(a), ARRAY_SIZE(a)}
#define REGS(...)	REGS_ENTRY(((const __be16[]){__VA_ARGS__}))

static const struct initial_reg {
	const __be16 *data; /* data[0] is register address */
	unsigned int count;
} initial_regs[] = {
	REGS(be(0x0112), be(0x0A0A)), /* RAW10/RAW10; ADC 10-bit via 0x3F3E */

	/* PEDESTAL+2 :+2 is a workaround for 10bit mode +0.5 rounding */
	REGS(be(0x301E), be(0x00AA)),

	/* corrections_recommended_bayer */
	REGS(be(0x3042),
	     be(0x0004),  /* 3042: RNC: enable b/w rnc mode */
	     be(0x4580)), /* 3044: RNC: enable row noise correction */

	REGS(be(0x30D2),
	     be(0x0000),  /* 30D2: CRM/CC: enable crm on Visible and CC rows */
	     be(0x0000),  /* 30D4: CC: CC enabled with 16 samples per column */
	     /* 30D6: CC: bw mode enabled/12 bit data resolution/bw mode */
	     be(0x2FFF)),

	REGS(be(0x30DA),
	     be(0x0FFF),  /* 30DA: CC: column correction clip level 2 is 0 */
	     be(0x0FFF),  /* 30DC: CC: column correction clip level 3 is 0 */
	     be(0x0000)), /* 30DE: CC: Group FPN correction */

	/* RNC: rnc scaling factor = * 54 / 64 (32 / 38 * 64 = 53.9) */
	REGS(be(0x30EE), be(0x1136)),
	REGS(be(0x30FA), be(0xFD00)), /* GPIO0 = flash, GPIO1 = shutter */
	REGS(be(0x3120), be(0x0005)), /* p1 dither enabled for 10bit mode */
	REGS(be(0x3172), be(0x0206)), /* txlo clk divider options */
	/* FDOC:fdoc settings with fdoc every frame turned of */
	REGS(be(0x3180), be(0x9434)),

	REGS(be(0x31B0),
	     be(0x008B),  /* 31B0: frame_preamble - FIXME check WRT lanes# */
	     be(0x0050)), /* 31B2: line_preamble - FIXME check WRT lanes# */

	/* don't use continuous clock mode while shut down */
	REGS(be(0x31BC), be(0x068C)),
	REGS(be(0x31E0), be(0x0781)), /* Fuse/2DDC: enable 2ddc */

	/* analog_setup_recommended_10bit */
	REGS(be(0x341A), be(0x4735)), /* Samp&Hold pulse in ADC */
	REGS(be(0x3420), be(0x4735)), /* Samp&Hold pulse in ADC */
	REGS(be(0x3426), be(0x8A1A)), /* ADC offset distribution pulse */
	REGS(be(0x342A), be(0x0018)), /* pulse_config */

	/* pixel_timing_recommended */
	REGS(be(0x3D00),
	     /* 3D00 */ be(0x043E), be(0x4760), be(0xFFFF), be(0xFFFF),
	     /* 3D08 */ be(0x8000), be(0x0510), be(0xAF08), be(0x0252),
	     /* 3D10 */ be(0x486F), be(0x5D5D), be(0x8056), be(0x8313),
	     /* 3D18 */ be(0x0087), be(0x6A48), be(0x6982), be(0x0280),
	     /* 3D20 */ be(0x8359), be(0x8D02), be(0x8020), be(0x4882),
	     /* 3D28 */ be(0x4269), be(0x6A95), be(0x5988), be(0x5A83),
	     /* 3D30 */ be(0x5885), be(0x6280), be(0x6289), be(0x6097),
	     /* 3D38 */ be(0x5782), be(0x605C), be(0xBF18), be(0x0961),
	     /* 3D40 */ be(0x5080), be(0x2090), be(0x4390), be(0x4382),
	     /* 3D48 */ be(0x5F8A), be(0x5D5D), be(0x9C63), be(0x8063),
	     /* 3D50 */ be(0xA960), be(0x9757), be(0x8260), be(0x5CFF),
	     /* 3D58 */ be(0xBF10), be(0x1681), be(0x0802), be(0x8000),
	     /* 3D60 */ be(0x141C), be(0x6000), be(0x6022), be(0x4D80),
	     /* 3D68 */ be(0x5C97), be(0x6A69), be(0xAC6F), be(0x4645),
	     /* 3D70 */ be(0x4400), be(0x0513), be(0x8069), be(0x6AC6),
	     /* 3D78 */ be(0x5F95), be(0x5F70), be(0x8040), be(0x4A81),
	     /* 3D80 */ be(0x0300), be(0xE703), be(0x0088), be(0x4A83),
	     /* 3D88 */ be(0x40FF), be(0xFFFF), be(0xFD70), be(0x8040),
	     /* 3D90 */ be(0x4A85), be(0x4FA8), be(0x4F8C), be(0x0070),
	     /* 3D98 */ be(0xBE47), be(0x8847), be(0xBC78), be(0x6B89),
	     /* 3DA0 */ be(0x6A80), be(0x6986), be(0x6B8E), be(0x6B80),
	     /* 3DA8 */ be(0x6980), be(0x6A88), be(0x7C9F), be(0x866B),
	     /* 3DB0 */ be(0x8765), be(0x46FF), be(0xE365), be(0xA679),
	     /* 3DB8 */ be(0x4A40), be(0x4580), be(0x44BC), be(0x7000),
	     /* 3DC0 */ be(0x8040), be(0x0802), be(0x10EF), be(0x0104),
	     /* 3DC8 */ be(0x3860), be(0x5D5D), be(0x5682), be(0x1300),
	     /* 3DD0 */ be(0x8648), be(0x8202), be(0x8082), be(0x598A),
	     /* 3DD8 */ be(0x0280), be(0x2048), be(0x3060), be(0x8042),
	     /* 3DE0 */ be(0x9259), be(0x865A), be(0x8258), be(0x8562),
	     /* 3DE8 */ be(0x8062), be(0x8560), be(0x9257), be(0x8221),
	     /* 3DF0 */ be(0x10FF), be(0xB757), be(0x9361), be(0x1019),
	     /* 3DF8 */ be(0x8020), be(0x9043), be(0x8E43), be(0x845F),
	     /* 3E00 */ be(0x835D), be(0x805D), be(0x8163), be(0x8063),
	     /* 3E08 */ be(0xA060), be(0x9157), be(0x8260), be(0x5CFF),
	     /* 3E10 */ be(0xFFFF), be(0xFFE5), be(0x1016), be(0x2048),
	     /* 3E18 */ be(0x0802), be(0x1C60), be(0x0014), be(0x0060),
	     /* 3E20 */ be(0x2205), be(0x8120), be(0x908F), be(0x6A80),
	     /* 3E28 */ be(0x6982), be(0x5F9F), be(0x6F46), be(0x4544),
	     /* 3E30 */ be(0x0005), be(0x8013), be(0x8069), be(0x6A80),
	     /* 3E38 */ be(0x7000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E40 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E48 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E50 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E58 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E60 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E68 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E70 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E78 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E80 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E88 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E90 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3E98 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3EA0 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3EA8 */ be(0x0000), be(0x0000), be(0x0000), be(0x0000),
	     /* 3EB0 */ be(0x0000), be(0x0000), be(0x0000)),

	REGS(be(0x3EB6), be(0x004C)), /* ECL */

	REGS(be(0x3EBA),
	     be(0xAAAD),  /* 3EBA */
	     be(0x0086)), /* 3EBC: Bias currents for FSC/ECL */

	REGS(be(0x3EC0),
	     be(0x1E00),  /* 3EC0: SFbin/SH mode settings */
	     be(0x100A),  /* 3EC2: CLK divider for ramp for 10 bit 400MH */
	     /* 3EC4: FSC clamps for HDR mode and adc comp power down co */
	     be(0x3300),
	     be(0xEA44),  /* 3EC6: VLN and clk gating controls */
	     be(0x6F6F),  /* 3EC8: Txl0 and Txlo1 settings for normal mode */
	     be(0x2F4A),  /* 3ECA: CDAC/Txlo2/RSTGHI/RSTGLO settings */
	     be(0x0506),  /* 3ECC: RSTDHI/RSTDLO/CDAC/TXHI settings */
	     /* 3ECE: Ramp buffer settings and Booster enable (bits 0-5) */
	     be(0x203B),
	     be(0x13F0),  /* 3ED0: TXLO from atest/sf bin settings */
	     be(0xA53D),  /* 3ED2: Ramp offset */
	     be(0x862F),  /* 3ED4: TXLO open loop/row driver settings */
	     be(0x4081),  /* 3ED6: Txlatch fr cfpn rows/vln bias */
	     be(0x8003),  /* 3ED8: Ramp step setting for 10 bit 400 Mhz */
	     be(0xA580),  /* 3EDA: Ramp Offset */
	     be(0xC000),  /* 3EDC: over range for rst and under range for sig */
	     be(0xC103)), /* 3EDE: over range for sig and col dec clk settings */

	/* corrections_recommended_bayer */
	REGS(be(0x3F00),
	     be(0x0017),  /* 3F00: BM_T0 */
	     be(0x02DD),  /* 3F02: BM_T1 */
	     /* 3F04: if Ana_gain less than 2, use noise_floor0, multipl */
	     be(0x0020),
	     /* 3F06: if Ana_gain between 4 and 7, use noise_floor2 and */
	     be(0x0040),
	     /* 3F08: if Ana_gain between 4 and 7, use noise_floor2 and */
	     be(0x0070),
	     /* 3F0A: Define noise_floor0(low address) and noise_floor1 */
	     be(0x0101),
	     be(0x0302)), /* 3F0C: Define noise_floor2 and noise_floor3 */

	REGS(be(0x3F10),
	     be(0x0505),  /* 3F10: single k factor 0 */
	     be(0x0505),  /* 3F12: single k factor 1 */
	     be(0x0505),  /* 3F14: single k factor 2 */
	     be(0x01FF),  /* 3F16: cross factor 0 */
	     be(0x01FF),  /* 3F18: cross factor 1 */
	     be(0x01FF),  /* 3F1A: cross factor 2 */
	     be(0x0022)), /* 3F1E */

	/* GTH_THRES_RTN: 4max,4min filtered out of every 46 samples and */
	REGS(be(0x3F2C), be(0x442E)),

	REGS(be(0x3F3E),
	     be(0x0000),  /* 3F3E: Switch ADC from 12 bit to 10 bit mode */
	     be(0x1511),  /* 3F40: couple k factor 0 */
	     be(0x1511),  /* 3F42: couple k factor 1 */
	     be(0x0707)), /* 3F44: couple k factor 2 */
};

static void ar0521_power_off_partial(struct ar0521 *sensor, int n_supplies,
				     bool clk_on)
{
	int i;

	/* POR discards on-sensor GRR/slave state; drop stale software backup. */
	sensor->slave_backup_valid = false;

	if (clk_on && sensor->extclk)
		clk_disable_unprepare(sensor->extclk);

	if (sensor->reset_gpio)
		gpiod_set_value(sensor->reset_gpio, 1);

	for (i = n_supplies - 1; i >= 0; i--) {
		if (sensor->supplies[i])
			regulator_disable(sensor->supplies[i]);
	}

	if (!IS_ERR_OR_NULL(sensor->pinctrl) && sensor->pins_sleep)
		pinctrl_select_state(sensor->pinctrl, sensor->pins_sleep);
}

static int ar0521_power_off(struct ar0521 *sensor)
{
	ar0521_power_off_partial(sensor, ARRAY_SIZE(ar0521_supply_names),
				 true);
	return 0;
}

static int ar0521_power_on(struct ar0521 *sensor)
{
	unsigned int cnt;
	unsigned int enabled = 0;
	bool clk_on = false;
	int ret;

	if (!IS_ERR_OR_NULL(sensor->pinctrl)) {
		ret = ar0521_light_source_apply_pinctrl_locked(sensor);
		if (ret < 0)
			dev_dbg(sensor->dev, "could not set light source pin state\n");
	}

	for (cnt = 0; cnt < ARRAY_SIZE(ar0521_supply_names); cnt++) {
		if (!sensor->supplies[cnt])
			continue;
		ret = regulator_enable(sensor->supplies[cnt]);
		if (ret < 0)
			goto off;
		enabled = cnt + 1;
		usleep_range(1000, 1500);
	}

	if (sensor->extclk) {
		ret = clk_prepare_enable(sensor->extclk);
		if (ret < 0) {
			dev_err(sensor->dev, "error enabling sensor clock\n");
			goto off;
		}
		clk_on = true;
	}
	usleep_range(1000, 1500);

	if (sensor->reset_gpio)
		gpiod_set_value(sensor->reset_gpio, 0);
	/* Datasheet: >= 45000 extclk cycles after RESET_N deassert. */
	usleep_range(2000, 2500);

	for (cnt = 0; cnt < ARRAY_SIZE(initial_regs); cnt++) {
		ret = ar0521_write_regs(sensor, initial_regs[cnt].data,
					initial_regs[cnt].count);
		if (ret)
			goto off;
	}

	ret = ar0521_write_reg(sensor, AR0521_REG_SERIAL_FORMAT,
			       AR0521_REG_SERIAL_FORMAT_MIPI |
			       sensor->lane_count);
	if (ret)
		goto off;

	ret = ar0521_write_reg(sensor, AR0521_REG_HISPI_TEST_MODE,
			       ((0x40 << sensor->lane_count) - 0x40) |
			       AR0521_REG_HISPI_TEST_MODE_LP11);
	if (ret)
		goto off;

	ret = ar0521_write_reg(sensor, AR0521_REG_ROW_SPEED,
			       0x110 | (4 / sensor->lane_count));
	if (ret)
		goto off;

	return 0;
off:
	ar0521_power_off_partial(sensor, enabled, clk_on);
	return ret;
}

static int ar0521_identify(struct ar0521 *sensor)
{
	u16 model;
	u16 rev;
	int ret;

	ret = ar0521_read_reg(sensor, AR0521_REG_MODEL_ID, &model);
	if (ret)
		return ret;

	dev_info(sensor->dev, "model_id 0x%04x at 0x3000\n", model);
	if (model != AR0521_MODEL_ID) {
		dev_err(sensor->dev, "unexpected model_id 0x%04x, want 0x%04x\n",
			model, AR0521_MODEL_ID);
		return -ENODEV;
	}

	ret = ar0521_read_reg(sensor, AR0521_REG_REVISION, &rev);
	if (ret)
		return ret;

	dev_info(sensor->dev, "identified AR0521 model_id=0x%04x revision=0x%04x\n",
		 model, rev);
	return 0;
}

static int ar0521_setup(struct ar0521 *sensor)
{
	int ret;

	ret = ar0521_write_pll(sensor);
	if (ret)
		return ret;

	ret = ar0521_set_geometry(sensor);
	if (ret)
		return ret;

	return 0;
}

static int ar0521_mode_switch(struct ar0521 *sensor, enum ar0521_op_mode new_mode)
{
	enum ar0521_op_mode old_pending = sensor->pending_mode;
	enum ar0521_op_mode old_active = sensor->active_mode;
	bool old_streaming = sensor->streaming;
	int ret = 0;
	int rec_ret;

	if (new_mode > AR0521_TRIGGER_ONE_SHOT)
		return -EINVAL;

	if (!old_streaming) {
		sensor->pending_mode = new_mode;
		ar0521_update_ctrl_visibility_locked(sensor, new_mode);
		ar0521_update_exposure_range_locked(sensor, new_mode,
						    sensor->vblank->val);
		return 0;
	}

	if (old_active == new_mode) {
		sensor->pending_mode = new_mode;
		ar0521_update_ctrl_visibility_locked(sensor, new_mode);
		ar0521_update_exposure_range_locked(sensor, new_mode,
						    sensor->vblank->val);
		return 0;
	}

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	sensor->pending_mode = new_mode;
	ar0521_update_ctrl_visibility_locked(sensor, new_mode);

	ret = ar0521_stream_off_bits(sensor);
	if (ret)
		goto restore;

	if (old_active == AR0521_TRIGGER_ONE_SHOT) {
		ret = ar0521_exit_slave_locked(sensor);
		if (ret)
			goto restore;
	}

	ret = ar0521_apply_mode_regs_locked(sensor, new_mode);
	if (ret)
		goto restore;

	ret = ar0521_restore_ctrls_for_mode_locked(sensor, new_mode);
	if (ret)
		goto restore;

	if (new_mode == AR0521_FREE_RUN) {
		ret = ar0521_stream_on_bits(sensor);
		if (ret)
			goto restore;
	} else {
		ret = ar0521_exit_lp11_locked(sensor);
		if (ret)
			goto restore;
		__v4l2_ctrl_grab(sensor->vflip, 1);
		__v4l2_ctrl_grab(sensor->hflip, 1);
	}

	sensor->active_mode = new_mode;
	sensor->streaming = true;
	pm_runtime_put(sensor->dev);
	return 0;

restore:
	sensor->pending_mode = old_pending;
	ar0521_update_ctrl_visibility_locked(sensor, old_pending);

	rec_ret = ar0521_apply_mode_regs_locked(sensor, old_active);
	if (!rec_ret)
		rec_ret = ar0521_restore_ctrls_for_mode_locked(sensor, old_active);
	if (!rec_ret) {
		if (old_active == AR0521_FREE_RUN)
			rec_ret = ar0521_stream_on_bits(sensor);
		else
			rec_ret = ar0521_exit_lp11_locked(sensor);
	}

	if (!rec_ret && old_streaming) {
		sensor->active_mode = old_active;
		sensor->streaming = true;
		if (old_active == AR0521_TRIGGER_ONE_SHOT) {
			__v4l2_ctrl_grab(sensor->vflip, 1);
			__v4l2_ctrl_grab(sensor->hflip, 1);
		}
		pm_runtime_put(sensor->dev);
		return ret;
	}

	/* Recovery failed: stay stopped and drop the original stream PM ref. */
	sensor->streaming = false;
	sensor->active_mode = old_pending;
	ar0521_light_source_set_locked(sensor, false);
	__v4l2_ctrl_grab(sensor->vflip, 0);
	__v4l2_ctrl_grab(sensor->hflip, 0);
	pm_runtime_put(sensor->dev);
	pm_runtime_mark_last_busy(sensor->dev);
	pm_runtime_put_autosuspend(sensor->dev);
	return rec_ret ? rec_ret : ret;
}

static int ar0521_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ar0521 *sensor = container_of(ctrl->handler, struct ar0521, ctrls);
	enum ar0521_op_mode mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_AR0521_OP_MODE)
		return ar0521_mode_switch(sensor, ctrl->val);

	mode = ar0521_ctrl_mode_locked(sensor);

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ar0521_update_exposure_range_locked(sensor, mode, ctrl->val);
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_ENABLE:
		sensor->light_source_enabled = !!ctrl->val;
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_ACTIVE_LEVEL:
		sensor->light_source_active_level = !!ctrl->val;
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_ADVANCE_US:
		sensor->light_source_advance_us = ctrl->val;
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_OFF_DELAY_US:
		sensor->light_source_off_delay_us = ctrl->val;
		break;
	default:
		break;
	}

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	ret = ar0521_group_hold(sensor, true);
	if (ret)
		goto out_pm;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ar0521_apply_exposure_locked(sensor, mode, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ar0521_apply_analog_gain_locked(sensor, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = ar0521_apply_vblank_locked(sensor, ctrl->val);
		if (ret)
			break;
		ret = ar0521_apply_exposure_locked(sensor, mode, sensor->exposure->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		sensor->format.code = ar0521_mbus_code(sensor);
		ret = ar0521_apply_flip_locked(sensor);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ar0521_apply_test_pattern_locked(sensor, ctrl->val);
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_ENABLE:
		if (sensor->streaming && sensor->active_mode == AR0521_FREE_RUN)
			ar0521_light_source_set_locked(sensor,
						       sensor->light_source_enabled);
		break;
	case V4L2_CID_AR0521_LIGHT_SOURCE_ACTIVE_LEVEL:
		ret = ar0521_light_source_init_locked(sensor);
		if (ret < 0)
			break;
		if (sensor->streaming && sensor->active_mode == AR0521_FREE_RUN)
			ar0521_light_source_set_locked(sensor,
						       sensor->light_source_enabled);
		break;
	case V4L2_CID_HBLANK:
		ret = ar0521_set_geometry(sensor);
		break;
	default:
		break;
	}

	{
		int hold_ret = ar0521_group_hold(sensor, false);

		if (!ret)
			ret = hold_ret;
	}
	if (ctrl->id == V4L2_CID_EXPOSURE &&
	    mode == AR0521_TRIGGER_ONE_SHOT &&
	    sensor->slave_backup_valid) {
		int grr_ret = ar0521_apply_grr_timing_locked(sensor, ctrl->val);

		if (!ret)
			ret = grr_ret;
	}

out_pm:
	pm_runtime_put(sensor->dev);
	return ret;
}

static int ar0521_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ar0521 *sensor = to_ar0521(sd);
	int ret = 0;
	int rb;

	mutex_lock(&sensor->mutex);

	enable = !!enable;
	if (enable == sensor->streaming)
		goto unlock;

	if (!enable) {
		if (sensor->active_mode == AR0521_TRIGGER_ONE_SHOT)
			ret = ar0521_exit_slave_locked(sensor);
		if (!ret)
			ret = ar0521_stream_off_bits(sensor);
		ar0521_light_source_set_locked(sensor, false);
		if (!ret) {
			sensor->streaming = false;
			pm_runtime_mark_last_busy(sensor->dev);
			pm_runtime_put_autosuspend(sensor->dev);
		}
		goto unlock;
	}

	ar0521_update_ctrl_visibility_locked(sensor, sensor->pending_mode);

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		goto unlock;

	ret = ar0521_setup(sensor);
	if (ret)
		goto err_pm;

	ret = ar0521_restore_ctrls_for_mode_locked(sensor, sensor->pending_mode);
	if (ret)
		goto err_pm;

	if (sensor->pending_mode == AR0521_TRIGGER_ONE_SHOT) {
		ret = ar0521_apply_mode_regs_locked(sensor,
						    AR0521_TRIGGER_ONE_SHOT);
		if (ret)
			goto err_pm;
		ret = ar0521_exit_lp11_locked(sensor);
		if (ret)
			goto err_pm;
		__v4l2_ctrl_grab(sensor->vflip, 1);
		__v4l2_ctrl_grab(sensor->hflip, 1);
	} else {
		ret = ar0521_apply_mode_regs_locked(sensor, AR0521_FREE_RUN);
		if (ret)
			goto err_pm;
		ret = ar0521_stream_on_bits(sensor);
		if (ret)
			goto err_pm;
	}

	ret = ar0521_light_source_init_locked(sensor);
	if (ret)
		goto err_pm;

	sensor->active_mode = sensor->pending_mode;
	sensor->streaming = true;

	if (sensor->active_mode == AR0521_FREE_RUN && sensor->light_source_enabled)
		ar0521_light_source_set_locked(sensor, true);

	goto unlock;

err_pm:
	ar0521_light_source_set_locked(sensor, false);
	if (sensor->slave_backup_valid) {
		rb = ar0521_exit_slave_locked(sensor);
		if (rb) {
			dev_err(sensor->dev,
				"s_stream rollback exit_slave failed (%d)\n",
				rb);
			if (!ret)
				ret = rb;
		}
	}
	rb = ar0521_stream_off_bits(sensor);
	if (rb) {
		dev_err(sensor->dev,
			"s_stream rollback stream off failed (%d)\n",
			rb);
		if (!ret)
			ret = rb;
	}
	sensor->streaming = false;
	pm_runtime_put_sync(sensor->dev);
unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int ar0521_s_power(struct v4l2_subdev *sd, int on)
{
	struct ar0521 *sensor = to_ar0521(sd);
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

static int ar0521_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ar0521 *sensor = to_ar0521(sd);
	u32 total_height;
	u32 total_width;

	mutex_lock(&sensor->mutex);
	total_width = ar0521_total_width_locked(sensor);
	total_height = ar0521_total_height_locked(sensor);
	fi->interval.numerator = total_height * total_width;
	fi->interval.denominator = AR0521_PIXEL_RATE;
	mutex_unlock(&sensor->mutex);

	return 0;
}

static int ar0521_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	struct ar0521 *sensor = to_ar0521(sd);

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = sensor->lane_count;
	return 0;
}

static long ar0521_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct ar0521 *sensor = to_ar0521(sd);
	u32 stream = 0;
	u32 sync_mode = 0;
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		ar0521_get_module_inf(sensor, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);
		mutex_lock(&sensor->mutex);
		ret = ar0521_quick_stream(sensor, !!stream);
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
static long ar0521_compat_ioctl32(struct v4l2_subdev *sd,
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
		ret = ar0521_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(stream)))
			return -EFAULT;
		ret = ar0521_ioctl(sd, cmd, &stream);
		break;
	case RKMODULE_GET_SYNC_MODE:
		ret = ar0521_ioctl(sd, cmd, &sync_mode);
		if (!ret && copy_to_user(up, &sync_mode, sizeof(sync_mode)))
			ret = -EFAULT;
		break;
	case RKMODULE_SET_SYNC_MODE:
		if (copy_from_user(&sync_mode, up, sizeof(sync_mode)))
			return -EFAULT;
		ret = ar0521_ioctl(sd, cmd, &sync_mode);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif
static int ar0521_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ar0521 *sensor = to_ar0521(sd);

	if (code->index != 0)
		return -EINVAL;

	code->code = ar0521_mbus_code(sensor);
	return 0;
}

static int ar0521_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ar0521 *sensor = to_ar0521(sd);

	if (fse->index >= 1 || fse->code != ar0521_mbus_code(sensor))
		return -EINVAL;

	fse->min_width = AR0521_WIDTH_MIN;
	fse->max_width = AR0521_WIDTH_MAX;
	fse->min_height = AR0521_HEIGHT_MIN;
	fse->max_height = AR0521_HEIGHT_MAX;
	return 0;
}

static void ar0521_clamp_crop_rect(struct v4l2_rect *rect)
{
	s32 max_left;
	s32 max_top;
	s32 left;
	s32 top;

	rect->width = clamp_t(unsigned int, ALIGN(rect->width, 4),
			      AR0521_WIDTH_MIN, AR0521_WIDTH_MAX);
	rect->height = clamp_t(unsigned int, ALIGN(rect->height, 4),
			       AR0521_HEIGHT_MIN, AR0521_HEIGHT_MAX);
	max_left = AR0521_NATIVE_LEFT + AR0521_NATIVE_WIDTH - rect->width;
	max_top = AR0521_NATIVE_TOP + AR0521_NATIVE_HEIGHT - rect->height;
	left = clamp_t(s32, rect->left, AR0521_NATIVE_LEFT, max_left);
	top = clamp_t(s32, rect->top, AR0521_NATIVE_TOP, max_top);
	rect->left = clamp_t(s32, ALIGN(left, 2), AR0521_NATIVE_LEFT, max_left);
	rect->top = clamp_t(s32, ALIGN(top, 2), AR0521_NATIVE_TOP, max_top);
}

/*
 * rkcif_create_dummy_buf() sizes VICAP dummy DMA from this pad op.
 * Without it, max_size stays 0 and STREAMON fails with -ENOMEM.
 */
static int ar0521_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct ar0521 *sensor = to_ar0521(sd);
	u32 total_width;
	u32 total_height;

	if (fie->index != 0)
		return -EINVAL;
	if (fie->code && fie->code != ar0521_mbus_code(sensor))
		return -EINVAL;

	mutex_lock(&sensor->mutex);
	total_width = ar0521_total_width_locked(sensor);
	total_height = ar0521_total_height_locked(sensor);
	fie->code = ar0521_mbus_code(sensor);
	fie->width = sensor->crop.width;
	fie->height = sensor->crop.height;
	fie->interval.numerator = total_height * total_width;
	fie->interval.denominator = AR0521_PIXEL_RATE;
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int ar0521_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct ar0521 *sensor = to_ar0521(sd);
	struct v4l2_mbus_framefmt *format;

	mutex_lock(&sensor->mutex);
	format = ar0521_get_pad_format(sensor, state, fmt->pad, fmt->which);
	format->code = ar0521_mbus_code(sensor);
	fmt->format = *format;
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int ar0521_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct ar0521 *sensor = to_ar0521(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	mutex_lock(&sensor->mutex);
	if (sensor->streaming && fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		mutex_unlock(&sensor->mutex);
		return -EBUSY;
	}

	crop = ar0521_get_pad_crop(sensor, state, fmt->pad, fmt->which);
	format = ar0521_get_pad_format(sensor, state, fmt->pad, fmt->which);

	/*
	 * media-ctl/set_fmt cannot grow output past the current crop, so a
	 * leftover 1456x1088 ROI (camos) made 2592x1944 look stuck. Expand
	 * the crop toward the default origin when a larger size is asked.
	 * Smaller sizes keep the current origin (ROI).
	 */
	if (fmt->format.width > crop->width ||
	    fmt->format.height > crop->height) {
		struct v4l2_rect rect = *crop;

		rect.width = max_t(u32, fmt->format.width, crop->width);
		rect.height = max_t(u32, fmt->format.height, crop->height);
		if (rect.width >= AR0521_WIDTH_MAX &&
		    rect.height >= AR0521_HEIGHT_MAX) {
			rect.left = AR0521_DEFAULT_LEFT;
			rect.top = AR0521_DEFAULT_TOP;
		}
		ar0521_clamp_crop_rect(&rect);
		*crop = rect;
	}

	format->width = crop->width;
	format->height = crop->height;
	format->code = ar0521_mbus_code(sensor);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ar0521_setup_hblank(sensor, format->width);
		ar0521_update_exposure_range_locked(sensor,
						    ar0521_ctrl_mode_locked(sensor),
						    sensor->vblank->val);
	}

	fmt->format = *format;
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int ar0521_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct ar0521 *sensor = to_ar0521(sd);

	mutex_lock(&sensor->mutex);
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *ar0521_get_pad_crop(sensor, state, sel->pad, sel->which);
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = AR0521_DEFAULT_LEFT;
		sel->r.top = AR0521_DEFAULT_TOP;
		sel->r.width = AR0521_WIDTH_MAX;
		sel->r.height = AR0521_HEIGHT_MAX;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = AR0521_NATIVE_LEFT;
		sel->r.top = AR0521_NATIVE_TOP;
		sel->r.width = AR0521_NATIVE_WIDTH;
		sel->r.height = AR0521_NATIVE_HEIGHT;
		break;
	default:
		mutex_unlock(&sensor->mutex);
		return -EINVAL;
	}
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int ar0521_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct ar0521 *sensor = to_ar0521(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	struct v4l2_rect rect;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	rect = sel->r;
	ar0521_clamp_crop_rect(&rect);

	mutex_lock(&sensor->mutex);
	if (sensor->streaming && sel->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		mutex_unlock(&sensor->mutex);
		return -EBUSY;
	}

	crop = ar0521_get_pad_crop(sensor, state, sel->pad, sel->which);
	format = ar0521_get_pad_format(sensor, state, sel->pad, sel->which);
	*crop = rect;
	format->width = rect.width;
	format->height = rect.height;
	sel->r = rect;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ar0521_setup_hblank(sensor, format->width);
		ar0521_update_exposure_range_locked(sensor,
						    ar0521_ctrl_mode_locked(sensor),
						    sensor->vblank->val);
	}

	mutex_unlock(&sensor->mutex);
	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int ar0521_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ar0521 *sensor = to_ar0521(sd);
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

static const struct v4l2_subdev_core_ops ar0521_subdev_core_ops = {
	.s_power = ar0521_s_power,
	.ioctl = ar0521_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = ar0521_compat_ioctl32,
#endif
	.log_status = v4l2_ctrl_subdev_log_status,
};

static const struct v4l2_subdev_video_ops ar0521_subdev_video_ops = {
	.s_stream = ar0521_s_stream,
	.g_frame_interval = ar0521_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops ar0521_subdev_pad_ops = {
	.enum_mbus_code = ar0521_enum_mbus_code,
	.enum_frame_size = ar0521_enum_frame_size,
	.enum_frame_interval = ar0521_enum_frame_interval,
	.get_fmt = ar0521_get_format,
	.set_fmt = ar0521_set_format,
	.get_selection = ar0521_get_selection,
	.set_selection = ar0521_set_selection,
	.get_mbus_config = ar0521_g_mbus_config,
};

static const struct v4l2_subdev_ops ar0521_subdev_ops = {
	.core = &ar0521_subdev_core_ops,
	.video = &ar0521_subdev_video_ops,
	.pad = &ar0521_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops ar0521_internal_ops = {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	.open = ar0521_open,
#endif
};

static int ar0521_subdev_init(struct ar0521 *sensor)
{
	int ret;

	v4l2_i2c_subdev_init(&sensor->sd, sensor->client, &ar0521_subdev_ops);
	sensor->sd.internal_ops = &ar0521_internal_ops;

	ret = ar0521_ctrls_init(sensor);
	if (ret < 0)
		return ret;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

#if defined(CONFIG_MEDIA_CONTROLLER)
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret < 0) {
		v4l2_ctrl_handler_free(&sensor->ctrls);
		return ret;
	}
#endif
	return 0;
}

static void ar0521_subdev_cleanup(struct ar0521 *sensor)
{
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sensor->sd.entity);
#endif
	v4l2_ctrl_handler_free(&sensor->ctrls);
}

static int __maybe_unused ar0521_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0521 *sensor = to_ar0521(sd);

	return ar0521_power_on(sensor);
}

static int __maybe_unused ar0521_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0521 *sensor = to_ar0521(sd);

	ar0521_power_off(sensor);
	return 0;
}

static const struct dev_pm_ops ar0521_pm_ops = {
	SET_RUNTIME_PM_OPS(ar0521_runtime_suspend, ar0521_runtime_resume, NULL)
};

static int ar0521_get_optional_supply(struct device *dev, const char *name,
				      struct regulator **out)
{
	struct regulator *reg;

	reg = devm_regulator_get_optional(dev, name);
	if (IS_ERR(reg)) {
		if (PTR_ERR(reg) == -ENODEV) {
			*out = NULL;
			return 0;
		}
		return PTR_ERR(reg);
	}

	*out = reg;
	return 0;
}

static int ar0521_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *endpoint;
	const char *sync_mode_name = NULL;
	struct v4l2_subdev *sd;
	struct ar0521 *sensor;
	char facing[2];
	u32 trigger_mode = AR0521_FREE_RUN;
	unsigned int i;
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
	sensor->lane_count = AR0521_NUM_DATA_LANES;

	ret = of_property_read_string(node, RKMODULE_CAMERA_SYNC_MODE,
				      &sync_mode_name);
	if (!ret) {
		if (strcmp(sync_mode_name, RKMODULE_INTERNAL_MASTER_MODE) != 0)
			dev_warn(dev,
				 "sync-mode '%s' unsupported, forcing internal_master\n",
				 sync_mode_name);
	} else if (ret != -EINVAL) {
		dev_warn(dev, "failed to read sync-mode (%d)\n", ret);
	}

	ret = of_property_read_u32(node, OF_AR0521_TRIGGER_MODE, &trigger_mode);
	if (ret || trigger_mode > AR0521_TRIGGER_ONE_SHOT)
		trigger_mode = AR0521_FREE_RUN;
	sensor->pending_mode = trigger_mode;
	sensor->active_mode = trigger_mode;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "could not parse endpoint\n");
		return ret;
	}

	if (ep.bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(dev, "invalid bus type, must be MIPI CSI2\n");
		return -EINVAL;
	}

	sensor->lane_count = ep.bus.mipi_csi2.num_data_lanes;
	switch (sensor->lane_count) {
	case 1:
	case 2:
	case 4:
		break;
	default:
		dev_err(dev, "invalid number of MIPI data lanes\n");
		return -EINVAL;
	}
	sensor->extclk = devm_clk_get_optional(dev, "extclk");
	if (IS_ERR(sensor->extclk))
		return dev_err_probe(dev, PTR_ERR(sensor->extclk),
				     "failed to get extclk\n");

	if (sensor->extclk) {
		sensor->extclk_freq = clk_get_rate(sensor->extclk);
		if (!sensor->extclk_freq)
			sensor->extclk_freq = AR0521_EXTCLK_DEFAULT;
	} else {
		sensor->extclk_freq = AR0521_EXTCLK_DEFAULT;
		dev_info(dev, "no extclk, using on-module crystal default %u Hz\n",
			 sensor->extclk_freq);
	}

	if (sensor->extclk_freq < AR0521_EXTCLK_MIN ||
	    sensor->extclk_freq > AR0521_EXTCLK_MAX) {
		dev_err(dev, "extclk frequency out of range: %u Hz\n",
			sensor->extclk_freq);
		return -EINVAL;
	}

	if (sensor->extclk_freq != AR0521_EXTCLK_EXPECTED)
		dev_warn(dev,
			 "extclk %u Hz != expected %u Hz (Table5 27 MHz)\n",
			 sensor->extclk_freq, AR0521_EXTCLK_EXPECTED);

	for (i = 0; i < ARRAY_SIZE(ar0521_supply_names); i++) {
		ret = ar0521_get_optional_supply(dev, ar0521_supply_names[i],
						 &sensor->supplies[i]);
		if (ret)
			return dev_err_probe(dev, ret, "failed to get %s\n",
					     ar0521_supply_names[i]);
	}

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset-gpios\n");

	sensor->trigger_pulse_us = AR0521_TRIGGER_PULSE_US_DEFAULT;
	ret = of_property_read_u32(node, OF_AR0521_TRIGGER_PULSE_US,
				   &trigger_mode);
	if (!ret) {
		if (trigger_mode < AR0521_TRIGGER_PULSE_US_MIN ||
		    trigger_mode > AR0521_TRIGGER_PULSE_US_MAX)
			dev_warn(dev,
				 "trigger pulse width %u us out of range, default %u us\n",
				 trigger_mode, AR0521_TRIGGER_PULSE_US_DEFAULT);
		else
			sensor->trigger_pulse_us = trigger_mode;
	} else if (ret != -EINVAL) {
		return dev_err_probe(dev, ret,
				     "failed to read trigger pulse width\n");
	}

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
				 "failed to read light-source-active-level (%d)\n",
				 ret);
		}
	}

	of_property_read_u32(node, "light-source-exposure-advance-us",
			     &sensor->light_source_advance_us);
	of_property_read_u32(node, "light-source-exposure-off-delay-us",
			     &sensor->light_source_off_delay_us);
	if (sensor->light_source_advance_us > 100000)
		sensor->light_source_advance_us = 100000;
	if (sensor->light_source_off_delay_us > 1000000)
		sensor->light_source_off_delay_us = 1000000;

	sensor->trigger_pwm = ar0521_devm_pwm_get_optional(dev, "trigger");
	if (IS_ERR(sensor->trigger_pwm))
		return dev_err_probe(dev, PTR_ERR(sensor->trigger_pwm),
				     "failed to get trigger pwm\n");

	sensor->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(sensor->pinctrl)) {
		sensor->pins_default =
			pinctrl_lookup_state(sensor->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(sensor->pins_default))
			sensor->pins_default =
				pinctrl_lookup_state(sensor->pinctrl, "default");
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

	mutex_init(&sensor->mutex);

	ret = ar0521_init_trigger_pwm(sensor);
	if (ret)
		goto err_destroy_mutex;

	ret = ar0521_power_on(sensor);
	if (ret)
		goto err_destroy_mutex;

	ret = ar0521_light_source_init_locked(sensor);
	if (ret)
		dev_warn(dev, "failed to initialize light source gpio (%d)\n",
			 ret);

	ret = ar0521_identify(sensor);
	if (ret)
		goto err_power;

	sensor->crop.left = AR0521_DEFAULT_LEFT;
	sensor->crop.top = AR0521_DEFAULT_TOP;
	sensor->crop.width = AR0521_WIDTH_MAX;
	sensor->crop.height = AR0521_HEIGHT_MAX;
	sensor->format.width = AR0521_WIDTH_MAX;
	sensor->format.height = AR0521_HEIGHT_MAX;
	sensor->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
	sensor->format.field = V4L2_FIELD_NONE;
	sensor->format.colorspace = V4L2_COLORSPACE_RAW;
	sensor->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	sensor->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	sensor->format.xfer_func = V4L2_XFER_FUNC_NONE;

	ret = ar0521_subdev_init(sensor);
	if (ret)
		goto err_power;

	sd = &sensor->sd;
	memset(facing, 0, sizeof(facing));
	facing[0] = strcmp(sensor->module_facing, "back") == 0 ? 'b' : 'f';
	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 sensor->module_index, facing, AR0521_NAME, dev_name(dev));

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret)
		goto err_pm;

	ret = device_add_group(dev, &ar0521_attr_group);
	if (ret)
		goto err_subdev;
	sensor->sysfs_registered = true;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "driver version: %02x.%02x.%02x, default mode: %s\n",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff,
		 ar0521_op_mode_name(sensor->pending_mode));
	dev_info(dev,
		 "PLL regs: extclk=%u pre=%u mult1=%u mult2=%u (odd M as written)\n",
		 sensor->extclk_freq, AR0521_PLL_PRE,
		 AR0521_PLL1_MULT_REG, AR0521_PLL2_MULT_REG);
	dev_info(dev,
		 "PLL eff: mult1=%u mult2=%u VCO1=%llu VCO2=%llu vt=%llu word=%llu\n",
		 AR0521_PLL1_MULT_EFF, AR0521_PLL2_MULT_EFF,
		 AR0521_PLL1_VCO_HZ, AR0521_PLL2_VCO_HZ,
		 AR0521_VT_PIX_CLK_HZ, AR0521_WORD_CLK_HZ);
	dev_info(dev,
		 "MIPI: bitrate/lane=%llu link_freq=%lld pixel_rate=%llu lanes=%u RAW10\n",
		 AR0521_MIPI_BITRATE_PER_LANE_HZ, AR0521_LINK_FREQ_HZ,
		 AR0521_PIXEL_RATE, sensor->lane_count);
	dev_info(dev, "sync mode: %s\n",
		 ar0521_sync_mode_name(sensor->sync_mode));
	if (sensor->trigger_pwm)
		dev_info(dev,
			 "trigger pwm ready: default high pulse=%u us idle-low\n",
			 sensor->trigger_pulse_us);

	return 0;

err_subdev:
	if (sensor->sysfs_registered)
		device_remove_group(dev, &ar0521_attr_group);
	v4l2_async_unregister_subdev(sd);
err_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	ar0521_subdev_cleanup(sensor);
err_power:
	ar0521_power_off(sensor);
err_destroy_mutex:
	mutex_destroy(&sensor->mutex);
	return ret;
}

static void ar0521_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0521 *sensor = to_ar0521(sd);

	if (sensor->sysfs_registered)
		device_remove_group(&client->dev, &ar0521_attr_group);
	v4l2_async_unregister_subdev(sd);
	ar0521_subdev_cleanup(sensor);
	mutex_destroy(&sensor->mutex);

	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		ar0521_power_off(sensor);
	pm_runtime_set_suspended(sensor->dev);
}

static const struct of_device_id ar0521_dt_ids[] = {
	{ .compatible = "onnn,ar0521" },
	{ }
};
MODULE_DEVICE_TABLE(of, ar0521_dt_ids);

static struct i2c_driver ar0521_i2c_driver = {
	.driver = {
		.name = AR0521_NAME,
		.pm = &ar0521_pm_ops,
		.of_match_table = ar0521_dt_ids,
	},
	.probe_new = ar0521_probe,
	.remove = ar0521_remove,
};

module_i2c_driver(ar0521_i2c_driver);

MODULE_DESCRIPTION("AR0521 MIPI Camera subdev driver");
MODULE_AUTHOR("Krzysztof Hałasa <khalasa@piap.pl>");
MODULE_LICENSE("GPL");
