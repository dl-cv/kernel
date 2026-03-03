// SPDX-License-Identifier: GPL-2.0
/*
 * os08a20 driver
 *
 * Copyright (C) 2021 Rockchip Electronics Co., Ltd.
 *
 * V0.0X01.0X00 init version.
 */

 #include <linux/clk.h>
 #include <linux/device.h>
 #include <linux/delay.h>
 #include <linux/gpio/consumer.h>
 #include <linux/i2c.h>
 #include <linux/module.h>
 #include <linux/pm_runtime.h>
 #include <linux/of.h>
 #include <linux/of_graph.h>
 #include <linux/regulator/consumer.h>
 #include <linux/sysfs.h>
 #include <linux/ktime.h>
 #include <linux/pinctrl/consumer.h>
 #include <linux/version.h>
 #include <media/v4l2-async.h>
 #include <media/media-entity.h>
 #include <media/v4l2-common.h>
 #include <media/v4l2-ctrls.h>
 #include <media/v4l2-device.h>
 #include <media/v4l2-event.h>
 #include <media/v4l2-fwnode.h>
 #include <media/v4l2-image-sizes.h>
 #include <media/v4l2-mediabus.h>
 #include <media/v4l2-subdev.h>
 
 #include <linux/rk-camera-module.h>
 
 /* verify default register values */
 //#define CHECK_REG_VALUE
 
 #define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x09)
 
 #ifndef V4L2_CID_DIGITAL_GAIN
 #define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
 #endif
 
 /* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
 #define MIPI_FREQ	480000000U
 #define OS08A20_PIXEL_RATE		(MIPI_FREQ * 2LL * 4LL / 10)
 #define OS08A20_XVCLK_FREQ		24000000
 
 #define CHIP_ID				0x530841
 #define OS08A20_REG_CHIP_ID		0x300a
 
 #define OS08A20_REG_CTRL_MODE		0x0100
 #define OS08A20_MODE_SW_STANDBY		0x00
 #define OS08A20_MODE_STREAMING		0x01
 
 #define OS08A20_REG_EXPOSURE		0x3501
 #define	OS08A20_EXPOSURE_MIN		4
 #define	OS08A20_EXPOSURE_STEP		1
 #define OS08A20_VTS_MAX			0x7fff
 
 #define OS08A20_REG_GAIN_H		0x3508
 #define OS08A20_REG_GAIN_L		0x3509
 #define OS08A20_GAIN_L_MASK		0xff
 #define OS08A20_GAIN_H_MASK		0x3f
 #define OS08A20_GAIN_H_SHIFT	8
 #define	ANALOG_GAIN_MIN			0x80
 #define	ANALOG_GAIN_MAX			0x7C0
 #define	ANALOG_GAIN_STEP		1
 #define	ANALOG_GAIN_DEFAULT		1024
 
 /* Manual white balance gains (common OmniVision layout, 16-bit each) */
 #define OS08A20_REG_AWB_R_GAIN		0x3400
 #define OS08A20_REG_AWB_G_GAIN		0x3402
 #define OS08A20_REG_AWB_B_GAIN		0x3404
 #define OS08A20_REG_AWB_MANUAL_CTRL	0x3406
 #define OS08A20_WB_GAIN_MIN		0
 #define OS08A20_WB_GAIN_MAX		4095
 #define OS08A20_WB_GAIN_STEP		1
 #define OS08A20_WB_GAIN_DEFAULT		1024 /* 0x400 = 1.0x */
 
 #define OS08A20_REG_GROUP	0x3208
 #define OS08A20_REG_FLIP	0x3820
 #define OS08A20_REG_MIRROR	0x3821
 #define MIRROR_BIT_MASK			BIT(2)
 #define FLIP_BIT_MASK			BIT(2)
 
 #define OS08A20_REG_TEST_PATTERN		0x5081
 #define	OS08A20_TEST_PATTERN_ENABLE	0x08
 #define	OS08A20_TEST_PATTERN_DISABLE	0x0
 
 #define OS08A20_REG_VTS			0x380e
 
 #define REG_NULL			0xFFFF
 #define DELAY_MS			0xEEEE	/* Array delay token */
 
 #define OS08A20_REG_VALUE_08BIT		1
 #define OS08A20_REG_VALUE_16BIT		2
 #define OS08A20_REG_VALUE_24BIT		3
 
 #define OS08A20_LANES			4
 #define OS08A20_BITS_PER_SAMPLE		10
 
 #define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
 #define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"
 
 #define OS08A20_NAME			"os08a20"
 #define OS08A20_MEDIA_BUS_FMT		MEDIA_BUS_FMT_SBGGR10_1X10
 
 /* External FSIN trigger (single-frame) */
 #define OF_FSIN_PULSE_US		"rockchip,fsin-pulse-us"
 #define OS08A20_FSIN_PULSE_US_DEFAULT	50u
 /* 可选默认值，用于 V4L2 曝光/增益控件初始值，重启后通过 DT/overlay 持久化 */
 #define OF_DEFAULT_EXPOSURE		"rockchip,default-exposure"   /* 曝光行数 */
 #define OF_DEFAULT_ANALOG_GAIN		"rockchip,default-analogue-gain"  /* 1024=1.0x */
 
 /* V4L2 custom control for single-frame trigger */
 /*
  * Use a valid "user control" ID (with V4L2_CTRL_CLASS_USER bits). Using the
  * legacy V4L2_CID_PRIVATE_BASE may cause v4l2_ctrl_new_custom() to fail with
  * -ERANGE and prevent probe.
  */
 #ifndef V4L2_CID_USER_OV_BASE
 #define V4L2_CID_USER_OV_BASE		(V4L2_CID_USER_BASE + 0x10c0)
 #endif
 #define V4L2_CID_OS08A20_USER_TRIGGER	(V4L2_CID_USER_OV_BASE + 0x1)
 
 struct os08a20_otp_info {
	 int flag; // bit[7]: info, bit[6]:wb
	 int module_id;
	 int lens_id;
	 int year;
	 int month;
	 int day;
	 int rg_ratio;
	 int bg_ratio;
 };
 
 static const char * const os08a20_supply_names[] = {
	 "avdd",		/* Analog power */
	 "dovdd",	/* Digital I/O power */
	 "dvdd",		/* Digital core power */
 };
 
 #define OS08A20_NUM_SUPPLIES ARRAY_SIZE(os08a20_supply_names)
 
 struct regval {
	 u16 addr;
	 u8 val;
 };
 
 struct os08a20_mode {
	 u32 width;
	 u32 height;
	 struct v4l2_fract max_fps;
	 u32 hts_def;
	 u32 vts_def;
	 u32 exp_def;
	 const struct regval *reg_list;
	 u8 hdr_mode;
 };
 
 struct os08a20 {
	 struct i2c_client	*client;
	 struct clk		*xvclk;
	 struct gpio_desc	*power_gpio;
	 struct gpio_desc	*reset_gpio;
	 struct gpio_desc	*pwdn_gpio;
	 struct gpio_desc	*fsin_gpio;
	 struct regulator_bulk_data supplies[OS08A20_NUM_SUPPLIES];
 
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
	 struct v4l2_ctrl	*auto_wb;
	 struct v4l2_ctrl	*red_balance;
	 struct v4l2_ctrl	*blue_balance;
	 struct v4l2_ctrl	*user_trigger;
	 struct mutex		mutex;
	 bool			streaming;
	 bool			power_on;
	 bool			trigger_armed;
	 u32			trigger_count;
	 const struct os08a20_mode *cur_mode;
	 unsigned int lane_num;
	 unsigned int cfg_num;
	 unsigned int pixel_rate;
	 u32			module_index;
	 struct os08a20_otp_info *otp;
	 const char		*module_facing;
	 const char		*module_name;
	 const char		*len_name;
	 struct rkmodule_awb_cfg	awb_cfg;
	 enum rkmodule_sync_mode	sync_mode;
	 enum rkmodule_sync_mode	dt_sync_mode;
	 u32			fsin_pulse_us;
	 /* DT: rockchip,default-exposure，V4L2 曝光控件初始值(行) */
	 u32			default_exposure;
	 /* DT: rockchip,default-analogue-gain，V4L2 增益控件初始值，1024=1.0x */
	 u32			default_anal_gain;

	 /* 快速触发：缓存上次应用的 3A 参数，未变化时跳过 I2C 写入 */
	 u32			trigger_last_exposure;
	 u32			trigger_last_anal_gain;
	 bool			trigger_last_auto_wb;
	 u32			trigger_last_red;
	 u32			trigger_last_blue;
	 /* 3A 脏标志：控件变更时置位，应用后清除，用于 fast_path 判断 */
	 bool			trigger_3a_dirty;
 };
 
 #define to_os08a20(sd) container_of(sd, struct os08a20, subdev)
 
 /* Forward declarations (needed because trigger helpers are above write_reg()) */
 static int os08a20_write_reg(struct i2c_client *client, u16 reg, u32 len, u32 val);
 static int os08a20_read_reg(struct i2c_client *client, u16 reg,
				 unsigned int len, u32 *val);
 /* 批量 I2C 写入用：reg/len/val 三元组，用于 os08a20_write_regs_batch */
 struct os08a20_reg_val { u16 reg; u32 len; u32 val; };
 static int os08a20_write_regs_batch(struct i2c_client *client,
				     const struct os08a20_reg_val *regs, int num);
 
 static inline bool os08a20_use_fsin_trigger(const struct os08a20 *os08a20)
 {
	 /*
	  * Treat "SLAVE_MODE + fsin gpio bound" as the explicit single-frame trigger
	  * configuration (board uses GPIO1_D6 as FSIN).
	  */
	 return os08a20->dt_sync_mode == SLAVE_MODE && os08a20->fsin_gpio;
 }
 
 static int os08a20_pulse_fsin_gpio(struct os08a20 *os08a20)
 {
	 u32 pulse_us;
 
	 if (!os08a20->fsin_gpio)
		 return -ENODEV;
 
	 pulse_us = os08a20->fsin_pulse_us;
	 if (!pulse_us)
		 pulse_us = OS08A20_FSIN_PULSE_US_DEFAULT;
 
	 /* Logical 1 = "active" (honors GPIO_ACTIVE_LOW in DT). */
	 gpiod_set_value_cansleep(os08a20->fsin_gpio, 1);
	 usleep_range(pulse_us, pulse_us + 20);
	 gpiod_set_value_cansleep(os08a20->fsin_gpio, 0);
 
	 return 0;
 }
 
 static int os08a20_apply_exposure_gain(struct os08a20 *os08a20)
 {
	 u32 exposure = os08a20->cur_mode->exp_def;
	 u32 anal_gain = ANALOG_GAIN_DEFAULT;
	 int ret;

	 if (os08a20->exposure)
		 exposure = os08a20->exposure->val;
	 if (os08a20->anal_gain)
		 anal_gain = os08a20->anal_gain->val;

	 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_EXPOSURE,
				 OS08A20_REG_VALUE_16BIT, exposure);
	 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_GAIN_L,
				  OS08A20_REG_VALUE_08BIT,
				  anal_gain & OS08A20_GAIN_L_MASK);
	 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_GAIN_H,
				  OS08A20_REG_VALUE_08BIT,
				  (anal_gain >> OS08A20_GAIN_H_SHIFT) &
				  OS08A20_GAIN_H_MASK);

	 return ret;
 }

 /* Batch write: STREAMING + exposure + gain in one I2C transaction */
 static int os08a20_trigger_apply_streaming_exposure_gain(struct os08a20 *os08a20)
 {
	 u32 exposure = os08a20->cur_mode->exp_def;
	 u32 anal_gain = ANALOG_GAIN_DEFAULT;
	 struct os08a20_reg_val regs[4];
	 int n = 0;

	 if (os08a20->exposure)
		 exposure = os08a20->exposure->val;
	 if (os08a20->anal_gain)
		 anal_gain = os08a20->anal_gain->val;

	 regs[n].reg = OS08A20_REG_CTRL_MODE;
	 regs[n].len = OS08A20_REG_VALUE_08BIT;
	 regs[n].val = OS08A20_MODE_STREAMING;
	 n++;

	 regs[n].reg = OS08A20_REG_EXPOSURE;
	 regs[n].len = OS08A20_REG_VALUE_16BIT;
	 regs[n].val = exposure;
	 n++;

	 regs[n].reg = OS08A20_REG_GAIN_L;
	 regs[n].len = OS08A20_REG_VALUE_08BIT;
	 regs[n].val = anal_gain & OS08A20_GAIN_L_MASK;
	 n++;

	 regs[n].reg = OS08A20_REG_GAIN_H;
	 regs[n].len = OS08A20_REG_VALUE_08BIT;
	 regs[n].val = (anal_gain >> OS08A20_GAIN_H_SHIFT) & OS08A20_GAIN_H_MASK;
	 n++;

	 return os08a20_write_regs_batch(os08a20->client, regs, n);
 }
 
 static int os08a20_apply_white_balance(struct os08a20 *os08a20)
 {
	 struct device *dev = &os08a20->client->dev;
	 bool auto_wb = true;
	 u32 red = OS08A20_WB_GAIN_DEFAULT;
	 u32 blue = OS08A20_WB_GAIN_DEFAULT;
	 u32 val = 0;
	 int ret;
 
	 /*
	  * Best-effort: not all pipelines expose/use sensor-side WB gains, and
	  * some variants may not implement these registers. Never fail streaming
	  * because of WB control writes.
	  */
	 if (!os08a20->auto_wb || !os08a20->red_balance || !os08a20->blue_balance)
		 return 0;
 
	 auto_wb = os08a20->auto_wb->val;
	 red = os08a20->red_balance->val;
	 blue = os08a20->blue_balance->val;
 
	 ret = os08a20_read_reg(os08a20->client, OS08A20_REG_AWB_MANUAL_CTRL,
					OS08A20_REG_VALUE_08BIT, &val);
	 if (!ret) {
		 if (auto_wb)
			 val &= ~BIT(0);
		 else
			 val |= BIT(0);
 
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_AWB_MANUAL_CTRL,
					 OS08A20_REG_VALUE_08BIT, val);
		 if (ret)
			 dev_warn(dev, "failed to write AWB manual ctrl: %d\n", ret);
	 } else {
		 dev_warn(dev, "failed to read AWB manual ctrl: %d\n", ret);
	 }
 
	 if (!auto_wb) {
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_AWB_R_GAIN,
					 OS08A20_REG_VALUE_16BIT, red);
		 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_AWB_G_GAIN,
					  OS08A20_REG_VALUE_16BIT, OS08A20_WB_GAIN_DEFAULT);
		 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_AWB_B_GAIN,
					  OS08A20_REG_VALUE_16BIT, blue);
		 if (ret)
			 dev_warn(dev, "failed to write AWB gains (r=%u b=%u): %d\n",
				  red, blue, ret);
	 }
 
	 return 0;
 }
 
 static int os08a20_trigger_one_frame_locked(struct os08a20 *os08a20)
 {
	 u32 vblank_val = 0;
	 u32 vts_now = 0;
	 struct i2c_client *client = os08a20->client;
	 u32 exposure, anal_gain;
	 bool auto_wb;
	 u32 red = OS08A20_WB_GAIN_DEFAULT, blue = OS08A20_WB_GAIN_DEFAULT;
	 bool fast_path;
	 int ret;

	 if (!os08a20_use_fsin_trigger(os08a20))
		 return -EINVAL;
	 if (!os08a20->streaming)
		 return -EBUSY;

	 if (!pm_runtime_get_if_in_use(&client->dev))
		 return -EIO;

	 exposure = os08a20->exposure ? os08a20->exposure->val : os08a20->cur_mode->exp_def;
	 anal_gain = os08a20->anal_gain ? os08a20->anal_gain->val : ANALOG_GAIN_DEFAULT;
	 auto_wb = os08a20->auto_wb ? os08a20->auto_wb->val : true;
	 if (os08a20->red_balance)
		 red = os08a20->red_balance->val;
	 if (os08a20->blue_balance)
		 blue = os08a20->blue_balance->val;

	 /*
	  * Fast path: skip 3A when unchanged since last trigger. Assumes sensor
	  * retains exposure/gain in standby, or we've just applied them.
	  */
	 fast_path = !os08a20->trigger_3a_dirty &&
		 exposure == os08a20->trigger_last_exposure &&
		 anal_gain == os08a20->trigger_last_anal_gain &&
		 auto_wb == os08a20->trigger_last_auto_wb &&
		 (auto_wb || (red == os08a20->trigger_last_red &&
			      blue == os08a20->trigger_last_blue));

	 if (fast_path) {
		 /* STREAMING only, then pulse, then STANDBY */
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
					 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_STREAMING);
	 } else {
		 /* Batch: STREAMING + exposure + gain in one I2C transaction */
		 ret = os08a20_trigger_apply_streaming_exposure_gain(os08a20);
		 if (!ret) {
			 os08a20->trigger_last_exposure = exposure;
			 os08a20->trigger_last_anal_gain = anal_gain;
			 os08a20->trigger_last_auto_wb = auto_wb;
			 os08a20->trigger_last_red = red;
			 os08a20->trigger_last_blue = blue;
			 os08a20->trigger_3a_dirty = false;
		 }
		 if (!ret)
			 os08a20_apply_white_balance(os08a20);
	 }
	 if (ret)
		 goto out_pm_put;

	 ret = os08a20_pulse_fsin_gpio(os08a20);

	 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
				  OS08A20_REG_VALUE_08BIT, OS08A20_MODE_SW_STANDBY);

	 if (!ret) {
		 os08a20->trigger_count++;

		if (os08a20->vblank) {
			vblank_val = os08a20->vblank->val;
			vts_now = os08a20->cur_mode->height + vblank_val;
		}

		dev_info(&client->dev,
			 "trigger capture: OK (count=%u vblank=%u vts=%u)\n",
			 os08a20->trigger_count,
			 vblank_val,
			 vts_now);
	} else {
		dev_warn(&client->dev, "trigger capture: FAIL (ret=%d)\n", ret);
	 }

 out_pm_put:
	 pm_runtime_put(&client->dev);
	 return ret;
 }
 
 static ssize_t trigger_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
 {
	 struct i2c_client *client = to_i2c_client(dev);
	 struct v4l2_subdev *sd = i2c_get_clientdata(client);
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 unsigned int val;
	 int ret;
 
	 if (kstrtouint(buf, 0, &val))
		 return -EINVAL;
	 if (!val)
		 return count;
 
	 mutex_lock(&os08a20->mutex);
	 ret = os08a20_trigger_one_frame_locked(os08a20);
	 mutex_unlock(&os08a20->mutex);
 
	 return ret ? ret : count;
 }
 
 static DEVICE_ATTR_WO(trigger);
 
 static struct attribute *os08a20_attrs[] = {
	 &dev_attr_trigger.attr,
	 NULL
 };
 
 static const struct attribute_group os08a20_attr_group = {
	 .attrs = os08a20_attrs,
 };
 
 struct os08a20_id_name {
	 int id;
	 char name[RKMODULE_NAME_LEN];
 };
 
 /*
  * Xclk 24Mhz
  * grabwindow_width 3840
  * grabwindow_height 2160
  * max_framerate 30fps
  * mipi_datarate per lane 960Mbps
  */
 static const struct regval os08a20_global_regs[] = {
	 {0x0100, 0x00},
	 {0x0103, 0x01},
	 {0x0303, 0x01},
	 {0x0305, 0x5a},
	 {0x0306, 0x00},
	 {0x0308, 0x03},
	 {0x0309, 0x04},
	 {0x032a, 0x00},
	 {0x300f, 0x11},
	 {0x3010, 0x01},
	 {0x3011, 0x04},
	 {0x3012, 0x41},
	 {0x3016, 0xf0},
	 {0x301e, 0x98},
	 {0x3031, 0xa9},
	 {0x3103, 0x92},
	 {0x3104, 0x01},
	 {0x3106, 0x10},
	 {0x340c, 0xff},
	 {0x340d, 0xff},
	 {0x031e, 0x09},
	 {0x3505, 0x83},
	 {0x3508, 0x00},
	 {0x3509, 0x80},
	 {0x350a, 0x04},
	 {0x350b, 0x00},
	 {0x350c, 0x00},
	 {0x350d, 0x80},
	 {0x350e, 0x04},
	 {0x350f, 0x00},
	 {0x3600, 0x00},
	 {0x3603, 0x2c},
	 {0x3605, 0x50},
	 {0x3609, 0xb5},
	 {0x3610, 0x39},
	 {0x3762, 0x11},
	 {0x360c, 0x01},
	 {0x3628, 0xa4},
	 {0x362d, 0x10},
	 {0x3660, 0x43},
	 {0x3661, 0x06},
	 {0x3662, 0x00},
	 {0x3663, 0x28},
	 {0x3664, 0x0d},
	 {0x366a, 0x38},
	 {0x366b, 0xa0},
	 {0x366d, 0x00},
	 {0x366e, 0x00},
	 {0x3680, 0x00},
	 {0x36c0, 0x00},
	 {0x3701, 0x02},
	 {0x373b, 0x02},
	 {0x373c, 0x02},
	 {0x3736, 0x02},
	 {0x3737, 0x02},
	 {0x3705, 0x00},
	 {0x3706, 0x39},
	 {0x370a, 0x00},
	 {0x370b, 0x98},
	 {0x3709, 0x49},
	 {0x3714, 0x21},
	 {0x371c, 0x00},
	 {0x371d, 0x08},
	 {0x3740, 0x1b},
	 {0x3741, 0x04},
	 {0x375e, 0x0b},
	 {0x3760, 0x10},
	 {0x3776, 0x10},
	 {0x3781, 0x02},
	 {0x3782, 0x04},
	 {0x3783, 0x02},
	 {0x3784, 0x08},
	 {0x3785, 0x08},
	 {0x3788, 0x01},
	 {0x3789, 0x01},
	 {0x3797, 0x04},
	 {0x3800, 0x00},
	 {0x3801, 0x00},
	 {0x3802, 0x00},
	 {0x3803, 0x0c},
	 {0x3804, 0x0e},
	 {0x3805, 0xff},
	 {0x3806, 0x08},
	 {0x3807, 0x6f},
	 {0x3808, 0x0f},
	 {0x3809, 0x00},
	 {0x380a, 0x08},
	 {0x380b, 0x70},
	 {0x380c, 0x04},
	 {0x380d, 0x0c},
	 {0x380e, 0x09},
	 {0x380f, 0x0a},
	 {0x3813, 0x10},
	 {0x3814, 0x01},
	 {0x3815, 0x01},
	 {0x3816, 0x01},
	 {0x3817, 0x01},
	 {0x381c, 0x00},
	 {0x3820, 0x00},
	 {0x3821, 0x04},
	 {0x3823, 0x08},
	 {0x3826, 0x00},
	 {0x3827, 0x08},
	 {0x382d, 0x08},
	 {0x3832, 0x02},
	 {0x3833, 0x00},
	 {0x383c, 0x48},
	 {0x383d, 0xff},
	 {0x3d85, 0x0b},
	 {0x3d84, 0x40},
	 {0x3d8c, 0x63},
	 {0x3d8d, 0xd7},
	 {0x4000, 0xf8},
	 {0x4001, 0x2b},
	 {0x4004, 0x00},
	 {0x4005, 0x40},
	 {0x400a, 0x01},
	 {0x400f, 0xa0},
	 {0x4010, 0x12},
	 {0x4018, 0x00},
	 {0x4008, 0x02},
	 {0x4009, 0x0d},
	 {0x401a, 0x58},
	 {0x4050, 0x00},
	 {0x4051, 0x01},
	 {0x4028, 0x2f},
	 {0x4052, 0x00},
	 {0x4053, 0x80},
	 {0x4054, 0x00},
	 {0x4055, 0x80},
	 {0x4056, 0x00},
	 {0x4057, 0x80},
	 {0x4058, 0x00},
	 {0x4059, 0x80},
	 {0x430b, 0xff},
	 {0x430c, 0xff},
	 {0x430d, 0x00},
	 {0x430e, 0x00},
	 {0x4501, 0x18},
	 {0x4502, 0x00},
	 {0x4643, 0x00},
	 {0x4640, 0x01},
	 {0x4641, 0x04},
	 {0x4800, 0x64},
	 {0x4809, 0x2b},
	 {0x4813, 0x90},
	 {0x4817, 0x04},
	 {0x4833, 0x18},
	 {0x483b, 0x00},
	 {0x484b, 0x03},
	 {0x4850, 0x7c},
	 {0x4852, 0x06},
	 {0x4856, 0x58},
	 {0x4857, 0xaa},
	 {0x4862, 0x0a},
	 {0x4869, 0x18},
	 {0x486a, 0xaa},
	 {0x486e, 0x03},
	 {0x486f, 0x55},
	 {0x4875, 0xf0},
	 {0x5000, 0x89},
	 {0x5001, 0x42},
	 {0x5004, 0x40},
	 {0x5005, 0x00},
	 {0x5180, 0x00},
	 {0x5181, 0x10},
	 {0x580b, 0x03},
	 {0x4d00, 0x03},
	 {0x4d01, 0xc9},
	 {0x4d02, 0xbc},
	 {0x4d03, 0xc6},
	 {0x4d04, 0x4a},
	 {0x4d05, 0x25},
	 {REG_NULL, 0x00},
 };
 
 /*
  * Xclk 24Mhz
  * Pclk 210Mhz
  * linelength 2200(0x898)
  * framelength 2250(0x7f6)
  * grabwindow_width 3840
  * grabwindow_height 2160
  * max_framerate 30fps
  * mipi_datarate per lane 960Mbps
  */
 static const struct regval os08a20_3840x2160_regs_4lane[] = {
	 // Sysclk 148Mhz, MIPI4_960Mbps/Lane, 30Fps.
	 //Line_length =2200, Frame_length =2250
	 {0x4700, 0x2b},
	 {0x4e00, 0x2b},
	 {0x0305, 0x3c},
	 {0x0323, 0x07},
	 {0x0324, 0x01},
	 {0x0325, 0x29},
	 {0x380c, 0x08},
	 {0x380d, 0x98},
	 {0x380e, 0x08},
	 {0x380f, 0xca},
	 {0x3501, 0x06},
	 {0x3502, 0xca},
	 {0x4837, 0x10},
	 {REG_NULL, 0x00},
 };
 
 
 static const struct os08a20_mode supported_modes_4lane[] = {
	 {
		 .width = 3840,
		 .height = 2160,
		 .max_fps = {
			 .numerator = 10000,
			 .denominator = 300000,
		 },
		 .exp_def = 0x08b0,
		 .hts_def = 0x898 * 2,
		 .vts_def = 0x08c6,
		 .reg_list = os08a20_3840x2160_regs_4lane,
		 .hdr_mode = NO_HDR,
	 },
 };
 
 static const struct os08a20_mode *supported_modes;
 
 static const s64 link_freq_menu_items[] = {
	 MIPI_FREQ
 };
 
 static const char * const os08a20_test_pattern_menu[] = {
	 "Disabled",
	 "Vertical Color Bar Type 1",
	 "Vertical Color Bar Type 2",
	 "Vertical Color Bar Type 3",
	 "Vertical Color Bar Type 4",
	 "Square_BW Color Bar Type 1",
	 "Square_BW Color Bar Type 2",
	 "Square_BW Color Bar Type 3",
	 "Square_BW Color Bar Type 4",
	 "Transparent Color Bar Type 1",
	 "Transparent Color Bar Type 2",
	 "Transparent Color Bar Type 3",
	 "Transparent Color Bar Type 4",
	 "Rolling Color Bar Type 1",
	 "Rolling Color Bar Type 2",
	 "Rolling Color Bar Type 3",
	 "Rolling Color Bar Type 4",
 };
 
 /* Write registers up to 4 at a time */
 static int os08a20_write_reg(struct i2c_client *client, u16 reg,
				 u32 len, u32 val)
 {
	 u32 buf_i, val_i;
	 u8 buf[6];
	 u8 *val_p;
	 __be32 val_be;
 
	 dev_dbg(&client->dev, "%s(%d) enter!\n", __func__, __LINE__);
	 dev_dbg(&client->dev, "write reg(0x%x val:0x%x)!\n", reg, val);
 
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
 
	 if (i2c_master_send(client, buf, len + 2) != len + 2) {
		 dev_err(&client->dev,
				"write reg(0x%x val:0x%x)failed !\n", reg, val);
		 return -EIO;
	 }
	 return 0;
 }

 /* Batch I2C: multiple reg writes in one transaction to reduce latency */
 static int os08a20_write_regs_batch(struct i2c_client *client,
				     const struct os08a20_reg_val *regs, int num)
 {
	 u8 buf[64];
	 int i, j, pos = 0;
	 u8 *val_p;
	 __be32 val_be;

	 if (num <= 0 || num > 16)
		 return -EINVAL;

	 for (i = 0; i < num && pos < sizeof(buf) - 6; i++) {
		 if (regs[i].len > 4)
			 return -EINVAL;
		 buf[pos++] = regs[i].reg >> 8;
		 buf[pos++] = regs[i].reg & 0xff;
		 val_be = cpu_to_be32(regs[i].val);
		 val_p = (u8 *)&val_be;
		 for (j = 4 - regs[i].len; j < 4; j++)
			 buf[pos++] = val_p[j];
	 }
	 if (i2c_master_send(client, buf, pos) != pos)
		 return -EIO;
	 return 0;
 }

 static int os08a20_write_array(struct i2c_client *client,
				   const struct regval *regs)
 {
	 int i, delay_ms, ret = 0;
 
	 for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		 if (regs[i].addr == DELAY_MS) {
			 delay_ms = regs[i].val;
			 dev_info(&client->dev, "delay(%d) ms !\n", delay_ms);
			 usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
			 continue;
		 }
		 ret = os08a20_write_reg(client, regs[i].addr,
						OS08A20_REG_VALUE_08BIT, regs[i].val);
		 if (ret)
			 dev_err(&client->dev, "%s failed !\n", __func__);
	 }
	 return ret;
 }
 
 /* Read registers up to 4 at a time */
 static int os08a20_read_reg(struct i2c_client *client, u16 reg,
					 unsigned int len, u32 *val)
 {
	 struct i2c_msg msgs[2];
	 u8 *data_be_p;
	 __be32 data_be = 0;
	 __be16 reg_addr_be = cpu_to_be16(reg);
	 int ret;
 
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
 
	 ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	 if (ret != ARRAY_SIZE(msgs))
		 return -EIO;
 
	 *val = be32_to_cpu(data_be);
 
	 return 0;
 }
 
 /* Check Register value */
 #ifdef CHECK_REG_VALUE
 static int os08a20_reg_verify(struct i2c_client *client,
				 const struct regval *regs)
 {
	 u32 i;
	 int ret = 0;
	 u32 value;
 
	 for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		 ret = os08a20_read_reg(client, regs[i].addr,
			   OS08A20_REG_VALUE_08BIT, &value);
		 if (value != regs[i].val) {
			 dev_info(&client->dev, "%s: 0x%04x is 0x%x instead of 0x%x\n",
				   __func__, regs[i].addr, value, regs[i].val);
		 }
	 }
	 return ret;
 }
 #endif
 
 static int os08a20_get_reso_dist(const struct os08a20_mode *mode,
				 struct v4l2_mbus_framefmt *framefmt)
 {
	 return abs(mode->width - framefmt->width) +
			abs(mode->height - framefmt->height);
 }
 
 static const struct os08a20_mode *
 os08a20_find_best_fit(struct os08a20 *os08a20,
			 struct v4l2_subdev_format *fmt)
 {
	 struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	 int dist;
	 int cur_best_fit = 0;
	 int cur_best_fit_dist = -1;
	 unsigned int i;
 
	 for (i = 0; i < os08a20->cfg_num; i++) {
		 dist = os08a20_get_reso_dist(&supported_modes[i], framefmt);
		 if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			 cur_best_fit_dist = dist;
			 cur_best_fit = i;
		 }
	 }
 
	 return &supported_modes[cur_best_fit];
 }
 
 static int os08a20_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 const struct os08a20_mode *mode;
	 s64 h_blank, vblank_def;
 
	 mutex_lock(&os08a20->mutex);
 
	 mode = os08a20_find_best_fit(os08a20, fmt);
	 fmt->format.code = OS08A20_MEDIA_BUS_FMT;
	 fmt->format.width = mode->width;
	 fmt->format.height = mode->height;
	 fmt->format.field = V4L2_FIELD_NONE;
	 if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
 #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		 *v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
 #else
		 mutex_unlock(&os08a20->mutex);
		 return -ENOTTY;
 #endif
	 } else {
		 os08a20->cur_mode = mode;
		 h_blank = mode->hts_def - mode->width;
		 __v4l2_ctrl_modify_range(os08a20->hblank, h_blank,
					  h_blank, 1, h_blank);
		 vblank_def = mode->vts_def - mode->height;
		 __v4l2_ctrl_modify_range(os08a20->vblank, vblank_def,
					  OS08A20_VTS_MAX - mode->height,
					  1, vblank_def);
	 }
 
	 mutex_unlock(&os08a20->mutex);
 
	 return 0;
 }
 
 static int os08a20_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 const struct os08a20_mode *mode = os08a20->cur_mode;
 
	 mutex_lock(&os08a20->mutex);
	 if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
 #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		 fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
 #else
		 mutex_unlock(&os08a20->mutex);
		 return -ENOTTY;
 #endif
	 } else {
		 fmt->format.width = mode->width;
		 fmt->format.height = mode->height;
		 fmt->format.code = OS08A20_MEDIA_BUS_FMT;
		 fmt->format.field = V4L2_FIELD_NONE;
	 }
	 mutex_unlock(&os08a20->mutex);
 
	 return 0;
 }
 
 static int os08a20_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
 {
	 if (code->index != 0)
		 return -EINVAL;
	 code->code = OS08A20_MEDIA_BUS_FMT;
 
	 return 0;
 }
 
 static int os08a20_enum_frame_sizes(struct v4l2_subdev *sd,
					struct v4l2_subdev_pad_config *cfg,
					struct v4l2_subdev_frame_size_enum *fse)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
 
	 if (fse->index >= os08a20->cfg_num)
		 return -EINVAL;
 
	 if (fse->code != OS08A20_MEDIA_BUS_FMT)
		 return -EINVAL;
 
	 fse->min_width  = supported_modes[fse->index].width;
	 fse->max_width  = supported_modes[fse->index].width;
	 fse->max_height = supported_modes[fse->index].height;
	 fse->min_height = supported_modes[fse->index].height;
 
	 return 0;
 }
 
 static int os08a20_enable_test_pattern(struct os08a20 *os08a20, u32 pattern)
 {
	 u32 val;
 
	 if (pattern)
		 val = (pattern - 1) | OS08A20_TEST_PATTERN_ENABLE;
	 else
		 val = OS08A20_TEST_PATTERN_DISABLE;
 
	 /* test pattern select*/
	 return os08a20_write_reg(os08a20->client, OS08A20_REG_TEST_PATTERN,
				 OS08A20_REG_VALUE_08BIT, val);
 }
 
 static int os08a20_g_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_frame_interval *fi)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 const struct os08a20_mode *mode = os08a20->cur_mode;
 
	 fi->interval = mode->max_fps;
 
	 return 0;
 }
 
 static void os08a20_get_module_inf(struct os08a20 *os08a20,
				   struct rkmodule_inf *inf)
 {
	 memset(inf, 0, sizeof(*inf));
	 strscpy(inf->base.sensor, OS08A20_NAME, sizeof(inf->base.sensor));
	 strscpy(inf->base.module, os08a20->module_name,
		 sizeof(inf->base.module));
	 strscpy(inf->base.lens, os08a20->len_name, sizeof(inf->base.lens));
 }
 
 static void os08a20_set_awb_cfg(struct os08a20 *os08a20,
				  struct rkmodule_awb_cfg *cfg)
 {
	 mutex_lock(&os08a20->mutex);
	 memcpy(&os08a20->awb_cfg, cfg, sizeof(*cfg));
	 mutex_unlock(&os08a20->mutex);
 }
 
 static long os08a20_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 struct rkmodule_hdr_cfg *hdr;
	 long ret = 0;
	 u32 stream = 0;
	 u32 sync_mode = 0;
 
	 switch (cmd) {
	 case RKMODULE_GET_MODULE_INFO:
		 os08a20_get_module_inf(os08a20, (struct rkmodule_inf *)arg);
		 break;
	 case RKMODULE_GET_HDR_CFG:
		 hdr = (struct rkmodule_hdr_cfg *)arg;
		 hdr->esp.mode = HDR_NORMAL_VC;
		 hdr->hdr_mode = os08a20->cur_mode->hdr_mode;
		 break;
	 case RKMODULE_SET_HDR_CFG:
		 hdr = (struct rkmodule_hdr_cfg *)arg;
		 if (hdr->hdr_mode != 0)
			 ret = -1;
		 break;
	 case RKMODULE_AWB_CFG:
		 os08a20_set_awb_cfg(os08a20, (struct rkmodule_awb_cfg *)arg);
		 break;
	 case RKMODULE_SET_QUICK_STREAM:
 
		 stream = *((u32 *)arg);
 
		 /*
		  * Some Rockchip pipelines use QUICK_STREAM to control the sensor
		  * stream state directly. In FSIN trigger mode we must not enter
		  * free-running streaming here, otherwise preview becomes continuous.
		  */
		 if (os08a20_use_fsin_trigger(os08a20)) {
			 if (stream) {
				 os08a20->trigger_armed = true;
				 os08a20->trigger_3a_dirty = true; /* first trigger does full apply */
				 if (os08a20->fsin_gpio)
					 gpiod_set_value_cansleep(os08a20->fsin_gpio, 0);
			 } else {
				 os08a20->trigger_armed = false;
			 }
			 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
						 OS08A20_REG_VALUE_08BIT,
						 OS08A20_MODE_SW_STANDBY);
		 } else {
			 if (stream)
				 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
					 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_STREAMING);
			 else
				 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
					 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_SW_STANDBY);
		 }
		 break;
	 case RKMODULE_GET_SYNC_MODE:
		 *((u32 *)arg) = os08a20_use_fsin_trigger(os08a20) ?
			 SLAVE_MODE : os08a20->sync_mode;
		 break;
	 case RKMODULE_SET_SYNC_MODE:
		 /* In FSIN trigger mode, sync_mode is fixed by DT (slave). */
		 if (!os08a20_use_fsin_trigger(os08a20)) {
			 sync_mode = *((u32 *)arg);
			 os08a20->sync_mode = sync_mode;
		 }
		 break;
	 default:
		 ret = -ENOIOCTLCMD;
		 break;
	 }
 
	 return ret;
 }
 
 #ifdef CONFIG_COMPAT
 static long os08a20_compat_ioctl32(struct v4l2_subdev *sd,
				   unsigned int cmd, unsigned long arg)
 {
	 void __user *up = compat_ptr(arg);
	 struct rkmodule_inf *inf;
	 struct rkmodule_awb_cfg *awb_cfg;
	 struct rkmodule_hdr_cfg *hdr;
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
 
		 ret = os08a20_ioctl(sd, cmd, inf);
		 if (!ret) {
			 ret = copy_to_user(up, inf, sizeof(*inf));
			 if (ret)
				 ret = -EFAULT;
		 }
		 kfree(inf);
		 break;
	 case RKMODULE_GET_HDR_CFG:
		 hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		 if (!hdr) {
			 ret = -ENOMEM;
			 return ret;
		 }
 
		 ret = os08a20_ioctl(sd, cmd, hdr);
		 if (!ret) {
			 ret = copy_to_user(up, hdr, sizeof(*hdr));
			 if (ret)
				 ret = -EFAULT;
		 }
		 kfree(hdr);
		 break;
	 case RKMODULE_SET_HDR_CFG:
		 hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		 if (!hdr) {
			 ret = -ENOMEM;
			 return ret;
		 }
 
		 if (copy_from_user(hdr, up, sizeof(*hdr))) {
			 kfree(hdr);
			 return -EFAULT;
		 }
 
		 ret = os08a20_ioctl(sd, cmd, hdr);
		 kfree(hdr);
		 break;
	 case RKMODULE_AWB_CFG:
		 awb_cfg = kzalloc(sizeof(*awb_cfg), GFP_KERNEL);
		 if (!awb_cfg) {
			 ret = -ENOMEM;
			 return ret;
		 }
 
		 if (copy_from_user(awb_cfg, up, sizeof(*awb_cfg))) {
			 kfree(awb_cfg);
			 return -EFAULT;
		 }
 
		 ret = os08a20_ioctl(sd, cmd, awb_cfg);
		 kfree(awb_cfg);
		 break;
	 case RKMODULE_SET_QUICK_STREAM:
		 if (copy_from_user(&stream, up, sizeof(u32)))
			 return -EFAULT;
 
		 ret = os08a20_ioctl(sd, cmd, &stream);
		 break;
	 case RKMODULE_GET_SYNC_MODE:
		 ret = os08a20_ioctl(sd, cmd, &sync_mode);
		 if (!ret) {
			 if (copy_to_user(up, &sync_mode, sizeof(u32)))
				 return -EFAULT;
		 }
		 break;
	 case RKMODULE_SET_SYNC_MODE:
		 ret = copy_from_user(&sync_mode, up, sizeof(u32));
		 if (!ret)
			 ret = os08a20_ioctl(sd, cmd, &sync_mode);
		 else
			 ret = -EFAULT;
		 break;
	 default:
		 ret = -ENOIOCTLCMD;
		 break;
	 }
 
	 return ret;
 }
 #endif
 
 static int __os08a20_start_stream(struct os08a20 *os08a20)
 {
	 int ret;
 
	 ret = os08a20_write_array(os08a20->client, os08a20->cur_mode->reg_list);
	 if (ret)
		 return ret;
 
 #ifdef CHECK_REG_VALUE
	 usleep_range(10000, 20000);
	 /*  verify default values to make sure everything has */
	 /*  been written correctly as expected */
	 dev_info(&os08a20->client->dev, "%s:Check register value!\n",
				 __func__);
	 ret = os08a20_reg_verify(os08a20->client, os08a20_global_regs);
	 if (ret)
		 return ret;
 
	 ret = os08a20_reg_verify(os08a20->client, os08a20->cur_mode->reg_list);
	 if (ret)
		 return ret;
 #endif
 
	 /* In case these controls are set before streaming */
	 mutex_unlock(&os08a20->mutex);
	 ret = v4l2_ctrl_handler_setup(&os08a20->ctrl_handler);
	 mutex_lock(&os08a20->mutex);
	 if (ret)
		 return ret;
 
	 os08a20->trigger_armed = os08a20_use_fsin_trigger(os08a20);
	 if (os08a20->trigger_armed) {
		 os08a20->trigger_3a_dirty = true; /* first trigger does full apply */
		 /* keep FSIN inactive while armed */
		 gpiod_set_value_cansleep(os08a20->fsin_gpio, 0);
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
					 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_SW_STANDBY);
	 } else {
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
					 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_STREAMING);
	 }
	 return ret;
 }
 
 static int __os08a20_stop_stream(struct os08a20 *os08a20)
 {
	 os08a20->trigger_armed = false;
	 return os08a20_write_reg(os08a20->client, OS08A20_REG_CTRL_MODE,
				 OS08A20_REG_VALUE_08BIT, OS08A20_MODE_SW_STANDBY);
 }
 
 static int os08a20_s_stream(struct v4l2_subdev *sd, int on)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 struct i2c_client *client = os08a20->client;
	 int ret = 0;
 
	 dev_info(&client->dev, "%s: on: %d, %dx%d@%d\n", __func__, on,
				 os08a20->cur_mode->width,
				 os08a20->cur_mode->height,
		 DIV_ROUND_CLOSEST(os08a20->cur_mode->max_fps.denominator,
		 os08a20->cur_mode->max_fps.numerator));
 
	 mutex_lock(&os08a20->mutex);
	 on = !!on;
	 if (on == os08a20->streaming)
		 goto unlock_and_return;
 
	 if (on) {
		 dev_info(&client->dev, "stream on!!!\n");
		 ret = pm_runtime_get_sync(&client->dev);
		 if (ret < 0) {
			 pm_runtime_put_noidle(&client->dev);
			 goto unlock_and_return;
		 }
 
		 ret = __os08a20_start_stream(os08a20);
		 if (ret) {
			 v4l2_err(sd, "start stream failed while write regs\n");
			 pm_runtime_put(&client->dev);
			 goto unlock_and_return;
		 }
	 } else {
		 dev_info(&client->dev, "stream off!!!\n");
		 __os08a20_stop_stream(os08a20);
		 pm_runtime_put(&client->dev);
	 }
 
	 os08a20->streaming = on;
 
 unlock_and_return:
	 mutex_unlock(&os08a20->mutex);
 
	 return ret;
 }
 
 static int os08a20_s_power(struct v4l2_subdev *sd, int on)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 struct i2c_client *client = os08a20->client;
	 int ret = 0;
 
	 dev_dbg(&client->dev, "%s(%d) on(%d)\n", __func__, __LINE__, on);
 
	 mutex_lock(&os08a20->mutex);
 
	 /* If the power state is not modified - no work to do. */
	 if (os08a20->power_on == !!on)
		 goto unlock_and_return;
 
	 if (on) {
		 ret = pm_runtime_get_sync(&client->dev);
		 if (ret < 0) {
			 pm_runtime_put_noidle(&client->dev);
			 goto unlock_and_return;
		 }
 
		 ret = os08a20_write_array(os08a20->client, os08a20_global_regs);
		 if (ret) {
			 v4l2_err(sd, "could not set init registers\n");
			 pm_runtime_put_noidle(&client->dev);
			 goto unlock_and_return;
		 }
 
		 os08a20->power_on = true;
		 /* export gpio */
		 if (!IS_ERR(os08a20->reset_gpio))
			 gpiod_export(os08a20->reset_gpio, false);
		 if (!IS_ERR(os08a20->pwdn_gpio))
			 gpiod_export(os08a20->pwdn_gpio, false);
	 } else {
		 pm_runtime_put(&client->dev);
		 os08a20->power_on = false;
	 }
 
 unlock_and_return:
	 mutex_unlock(&os08a20->mutex);
 
	 return ret;
 }
 
 /* Calculate the delay in us by clock rate and clock cycles */
 static inline u32 os08a20_cal_delay(u32 cycles)
 {
	 return DIV_ROUND_UP(cycles, OS08A20_XVCLK_FREQ / 1000 / 1000);
 }
 
 static int __os08a20_power_on(struct os08a20 *os08a20)
 {
	 int ret;
	 u32 delay_us;
	 struct device *dev = &os08a20->client->dev;
 
	 if (!IS_ERR(os08a20->power_gpio))
		 gpiod_set_value_cansleep(os08a20->power_gpio, 1);
 
	 usleep_range(1000, 2000);
 
	 if (!IS_ERR_OR_NULL(os08a20->pins_default)) {
		 ret = pinctrl_select_state(os08a20->pinctrl,
						os08a20->pins_default);
		 if (ret < 0)
			 dev_err(dev, "could not set pins\n");
	 }
	 ret = clk_set_rate(os08a20->xvclk, OS08A20_XVCLK_FREQ);
	 if (ret < 0)
		 dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");
	 if (clk_get_rate(os08a20->xvclk) != OS08A20_XVCLK_FREQ)
		 dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");
	 ret = clk_prepare_enable(os08a20->xvclk);
	 if (ret < 0) {
		 dev_err(dev, "Failed to enable xvclk\n");
		 return ret;
	 }
 
	 ret = regulator_bulk_enable(OS08A20_NUM_SUPPLIES, os08a20->supplies);
	 if (ret < 0) {
		 dev_err(dev, "Failed to enable regulators\n");
		 goto disable_clk;
	 }
 
	 if (!IS_ERR(os08a20->reset_gpio))
		 gpiod_set_value_cansleep(os08a20->reset_gpio, 1);
 
	 if (!IS_ERR(os08a20->pwdn_gpio))
		 gpiod_set_value_cansleep(os08a20->pwdn_gpio, 1);
 
	 /* export gpio */
	 if (!IS_ERR(os08a20->reset_gpio))
		 gpiod_export(os08a20->reset_gpio, false);
	 if (!IS_ERR(os08a20->pwdn_gpio))
		 gpiod_export(os08a20->pwdn_gpio, false);
 
	 /* 8192 cycles prior to first SCCB transaction */
	 delay_us = os08a20_cal_delay(8192);
	 usleep_range(delay_us, delay_us * 2);
	 usleep_range(10000, 20000);
	 return 0;
 
 disable_clk:
	 clk_disable_unprepare(os08a20->xvclk);
 
	 return ret;
 }
 
 static void __os08a20_power_off(struct os08a20 *os08a20)
 {
	 int ret;
	 struct device *dev = &os08a20->client->dev;
 
	 if (!IS_ERR(os08a20->pwdn_gpio))
		 gpiod_set_value_cansleep(os08a20->pwdn_gpio, 0);
	 clk_disable_unprepare(os08a20->xvclk);
	 if (!IS_ERR(os08a20->reset_gpio))
		 gpiod_set_value_cansleep(os08a20->reset_gpio, 0);
	 if (!IS_ERR_OR_NULL(os08a20->pins_sleep)) {
		 ret = pinctrl_select_state(os08a20->pinctrl,
						os08a20->pins_sleep);
		 if (ret < 0)
			 dev_dbg(dev, "could not set pins\n");
	 }
	 if (!IS_ERR(os08a20->power_gpio))
		 gpiod_set_value_cansleep(os08a20->power_gpio, 0);
 
	 regulator_bulk_disable(OS08A20_NUM_SUPPLIES, os08a20->supplies);
 }
 
 static int os08a20_runtime_resume(struct device *dev)
 {
	 struct i2c_client *client = to_i2c_client(dev);
	 struct v4l2_subdev *sd = i2c_get_clientdata(client);
	 struct os08a20 *os08a20 = to_os08a20(sd);
 
	 return __os08a20_power_on(os08a20);
 }
 
 static int os08a20_runtime_suspend(struct device *dev)
 {
	 struct i2c_client *client = to_i2c_client(dev);
	 struct v4l2_subdev *sd = i2c_get_clientdata(client);
	 struct os08a20 *os08a20 = to_os08a20(sd);
 
	 __os08a20_power_off(os08a20);
 
	 return 0;
 }
 
 #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
 static int os08a20_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
	 struct v4l2_mbus_framefmt *try_fmt =
				 v4l2_subdev_get_try_format(sd, fh->pad, 0);
	 const struct os08a20_mode *def_mode = &supported_modes[0];
 
	 mutex_lock(&os08a20->mutex);
	 /* Initialize try_fmt */
	 try_fmt->width = def_mode->width;
	 try_fmt->height = def_mode->height;
	 try_fmt->code = OS08A20_MEDIA_BUS_FMT;
	 try_fmt->field = V4L2_FIELD_NONE;
 
	 mutex_unlock(&os08a20->mutex);
	 /* No crop or compose */
 
	 return 0;
 }
 #endif
 
 static int os08a20_enum_frame_interval(struct v4l2_subdev *sd,
						struct v4l2_subdev_pad_config *cfg,
						struct v4l2_subdev_frame_interval_enum *fie)
 {
	 struct os08a20 *os08a20 = to_os08a20(sd);
 
	 if (fie->index >= os08a20->cfg_num)
		 return -EINVAL;
 
	 fie->code = OS08A20_MEDIA_BUS_FMT;
	 fie->width = supported_modes[fie->index].width;
	 fie->height = supported_modes[fie->index].height;
	 fie->interval = supported_modes[fie->index].max_fps;
	 fie->reserved[0] = supported_modes[fie->index].hdr_mode;
	 return 0;
 }
 
 static int os08a20_g_mbus_config(struct v4l2_subdev *sd,
				 unsigned int pad_id,
				 struct v4l2_mbus_config *config)
 {
	 u32 val = 0;
 
	 val = 1 << (OS08A20_LANES - 1) |
		   V4L2_MBUS_CSI2_CHANNEL_0 |
		   V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	 config->type = V4L2_MBUS_CSI2_DPHY;
	 config->flags = val;
 
	 return 0;
 }
 
 static const struct dev_pm_ops os08a20_pm_ops = {
	 SET_RUNTIME_PM_OPS(os08a20_runtime_suspend,
				os08a20_runtime_resume, NULL)
 };
 
 #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
 static const struct v4l2_subdev_internal_ops os08a20_internal_ops = {
	 .open = os08a20_open,
 };
 #endif
 
 static const struct v4l2_subdev_core_ops os08a20_core_ops = {
	 .s_power = os08a20_s_power,
	 .ioctl = os08a20_ioctl,
 #ifdef CONFIG_COMPAT
	 .compat_ioctl32 = os08a20_compat_ioctl32,
 #endif
 };
 
 static const struct v4l2_subdev_video_ops os08a20_video_ops = {
	 .s_stream = os08a20_s_stream,
	 .g_frame_interval = os08a20_g_frame_interval,
 };
 
 static const struct v4l2_subdev_pad_ops os08a20_pad_ops = {
	 .enum_mbus_code = os08a20_enum_mbus_code,
	 .enum_frame_size = os08a20_enum_frame_sizes,
	 .enum_frame_interval = os08a20_enum_frame_interval,
	 .get_fmt = os08a20_get_fmt,
	 .set_fmt = os08a20_set_fmt,
	 .get_mbus_config = os08a20_g_mbus_config,
 };
 
 static const struct v4l2_subdev_ops os08a20_subdev_ops = {
	 .core	= &os08a20_core_ops,
	 .video	= &os08a20_video_ops,
	 .pad	= &os08a20_pad_ops,
 };
 
 static int os08a20_set_ctrl(struct v4l2_ctrl *ctrl)
 {
	 struct os08a20 *os08a20 = container_of(ctrl->handler,
							struct os08a20, ctrl_handler);
	 struct i2c_client *client = os08a20->client;
	 s64 max;
	 u32 val = 0;
	 int ret = 0;
 
	 if (ctrl->id == V4L2_CID_OS08A20_USER_TRIGGER)
		 return os08a20_trigger_one_frame_locked(os08a20);
 
	 /* Propagate change of current control to all related controls */
	 switch (ctrl->id) {
	 case V4L2_CID_VBLANK:
		 /* Update max exposure while meeting expected vblanking */
		 max = os08a20->cur_mode->height + ctrl->val - 4;
		 __v4l2_ctrl_modify_range(os08a20->exposure,
					  os08a20->exposure->minimum, max,
					  os08a20->exposure->step,
					  os08a20->exposure->default_value);
		 break;
	 }
 
	 if (!pm_runtime_get_if_in_use(&client->dev))
		 return 0;
 
	 switch (ctrl->id) {
	 case V4L2_CID_EXPOSURE:
		 os08a20->trigger_3a_dirty = true;
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_EXPOSURE,
					 OS08A20_REG_VALUE_16BIT, ctrl->val);
		 break;
	 case V4L2_CID_ANALOGUE_GAIN:
		 os08a20->trigger_3a_dirty = true;
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_GAIN_L,
					 OS08A20_REG_VALUE_08BIT,
					 ctrl->val & OS08A20_GAIN_L_MASK);
		 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_GAIN_H,
					  OS08A20_REG_VALUE_08BIT,
					  (ctrl->val >> OS08A20_GAIN_H_SHIFT) &
					  OS08A20_GAIN_H_MASK);
		 break;
	 case V4L2_CID_AUTO_WHITE_BALANCE:
		 os08a20->trigger_3a_dirty = true;
		 ret = os08a20_apply_white_balance(os08a20);
		 break;
	 case V4L2_CID_RED_BALANCE:
	 case V4L2_CID_BLUE_BALANCE:
		 os08a20->trigger_3a_dirty = true;
		 if (os08a20->auto_wb && os08a20->auto_wb->val == 0)
			 ret = os08a20_apply_white_balance(os08a20);
		 break;
	 case V4L2_CID_VBLANK:
		 ret = os08a20_write_reg(os08a20->client, OS08A20_REG_VTS,
					 OS08A20_REG_VALUE_16BIT,
					 ctrl->val + os08a20->cur_mode->height);
		 break;
	 case V4L2_CID_TEST_PATTERN:
		 ret = os08a20_enable_test_pattern(os08a20, ctrl->val);
		 break;
	 case V4L2_CID_HFLIP:
		 ret = os08a20_read_reg(os08a20->client, OS08A20_REG_MIRROR,
						OS08A20_REG_VALUE_08BIT,
						&val);
		 if (ctrl->val)
			 val |= MIRROR_BIT_MASK;
		 else
			 val &= ~MIRROR_BIT_MASK;
		 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_MIRROR,
					 OS08A20_REG_VALUE_08BIT,
					 val);
		 break;
	 case V4L2_CID_VFLIP:
		 ret = os08a20_read_reg(os08a20->client, OS08A20_REG_FLIP,
						OS08A20_REG_VALUE_08BIT,
						&val);
		 if (ctrl->val)
			 val |= FLIP_BIT_MASK;
		 else
			 val &= ~FLIP_BIT_MASK;
		 ret |= os08a20_write_reg(os08a20->client, OS08A20_REG_FLIP,
					 OS08A20_REG_VALUE_08BIT,
					 val);
		 break;
	 default:
		 dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			  __func__, ctrl->id, ctrl->val);
		 break;
	 }
 
	 pm_runtime_put(&client->dev);
 
	 return ret;
 }
 
 static const struct v4l2_ctrl_ops os08a20_ctrl_ops = {
	 .s_ctrl = os08a20_set_ctrl,
 };
 
 static const struct v4l2_ctrl_config os08a20_user_trigger_ctrl = {
	 .ops	= &os08a20_ctrl_ops,
	 .id	= V4L2_CID_OS08A20_USER_TRIGGER,
	 .type	= V4L2_CTRL_TYPE_BUTTON,
	 .name	= "user_trigger",
	 .min	= 0,
	 .max	= 0,
	 .step	= 0,
	 .def	= 0,
	 .flags	= V4L2_CTRL_FLAG_WRITE_ONLY,
 };
 
 static int os08a20_initialize_controls(struct os08a20 *os08a20)
 {
	 const struct os08a20_mode *mode;
	 struct v4l2_ctrl_handler *handler;
	 struct v4l2_ctrl *ctrl;
	 s64 exposure_max, vblank_def;
	 u32 h_blank;
	 u32 exposure_def, anal_gain_def;
	 int ret;
 
	 handler = &os08a20->ctrl_handler;
	 mode = os08a20->cur_mode;
	 ret = v4l2_ctrl_handler_init(handler, 14);
	 if (ret)
		 return ret;
	 handler->lock = &os08a20->mutex;
 
	 ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
					   0, 0, link_freq_menu_items);
	 if (ctrl)
		 ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
 
	 v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			   0, os08a20->pixel_rate, 1, os08a20->pixel_rate);
 
	 h_blank = mode->hts_def - mode->width;
	 os08a20->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
						 h_blank, h_blank, 1, h_blank);
	 if (os08a20->hblank)
		 os08a20->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
 
	 vblank_def = mode->vts_def - mode->height;
	 os08a20->vblank = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_VBLANK, vblank_def,
				 OS08A20_VTS_MAX - mode->height,
				 1, vblank_def);
 
	 exposure_max = mode->vts_def - 4;
	 exposure_def = clamp_val(mode->exp_def, OS08A20_EXPOSURE_MIN, (u32)exposure_max);
	 if (os08a20->default_exposure)
		 exposure_def = clamp_val(os08a20->default_exposure,
					  OS08A20_EXPOSURE_MIN, (u32)exposure_max);
	 os08a20->exposure = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_EXPOSURE, OS08A20_EXPOSURE_MIN,
				 exposure_max, OS08A20_EXPOSURE_STEP,
				 exposure_def);
 
	 anal_gain_def = clamp_val((u32)ANALOG_GAIN_DEFAULT, ANALOG_GAIN_MIN, ANALOG_GAIN_MAX);
	 if (os08a20->default_anal_gain)
		 anal_gain_def = clamp_val(os08a20->default_anal_gain,
					   ANALOG_GAIN_MIN, ANALOG_GAIN_MAX);
	 os08a20->anal_gain = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_ANALOGUE_GAIN, ANALOG_GAIN_MIN,
				 ANALOG_GAIN_MAX, ANALOG_GAIN_STEP,
				 anal_gain_def);
 
	 os08a20->auto_wb = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	 os08a20->red_balance = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_RED_BALANCE, OS08A20_WB_GAIN_MIN,
				 OS08A20_WB_GAIN_MAX, OS08A20_WB_GAIN_STEP,
				 OS08A20_WB_GAIN_DEFAULT);
	 os08a20->blue_balance = v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_BLUE_BALANCE, OS08A20_WB_GAIN_MIN,
				 OS08A20_WB_GAIN_MAX, OS08A20_WB_GAIN_STEP,
				 OS08A20_WB_GAIN_DEFAULT);
	 v4l2_ctrl_auto_cluster(3, &os08a20->auto_wb, 0, true);
 
	 os08a20->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				 &os08a20_ctrl_ops, V4L2_CID_TEST_PATTERN,
				 ARRAY_SIZE(os08a20_test_pattern_menu) - 1,
				 0, 0, os08a20_test_pattern_menu);
 
	 v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_HFLIP, 0, 1, 1, 0);
 
	 v4l2_ctrl_new_std(handler, &os08a20_ctrl_ops,
				 V4L2_CID_VFLIP, 0, 1, 1, 0);
 
	 os08a20->user_trigger = v4l2_ctrl_new_custom(handler,
							  &os08a20_user_trigger_ctrl,
							  NULL);
 
	 if (handler->error) {
		 ret = handler->error;
		 dev_err(&os08a20->client->dev,
			 "Failed to init controls(%d)\n", ret);
		 goto err_free_handler;
	 }
 
	 os08a20->subdev.ctrl_handler = handler;
 
	 return 0;
 
 err_free_handler:
	 v4l2_ctrl_handler_free(handler);
 
	 return ret;
 }
 
 static int os08a20_check_sensor_id(struct os08a20 *os08a20,
				   struct i2c_client *client)
 {
	 struct device *dev = &os08a20->client->dev;
	 u32 id = 0;
	 int ret;
 
	 ret = os08a20_read_reg(client, OS08A20_REG_CHIP_ID,
					OS08A20_REG_VALUE_24BIT, &id);
	 if (id != CHIP_ID) {
		 dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		 return -ENODEV;
	 }
 
	 dev_info(dev, "Detected OV%06x sensor\n", CHIP_ID);
 
	 return 0;
 }
 
 static int os08a20_configure_regulators(struct os08a20 *os08a20)
 {
	 unsigned int i;
 
	 for (i = 0; i < OS08A20_NUM_SUPPLIES; i++)
		 os08a20->supplies[i].supply = os08a20_supply_names[i];
 
	 return devm_regulator_bulk_get(&os08a20->client->dev,
						OS08A20_NUM_SUPPLIES,
						os08a20->supplies);
 }
 
 static int os08a20_parse_of(struct os08a20 *os08a20)
 {
	 struct device *dev = &os08a20->client->dev;
	 struct device_node *endpoint;
	 struct fwnode_handle *fwnode;
	 int rval;
 
	 endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	 if (!endpoint) {
		 dev_err(dev, "Failed to get endpoint\n");
		 return -EINVAL;
	 }
	 fwnode = of_fwnode_handle(endpoint);
	 rval = fwnode_property_read_u32_array(fwnode, "data-lanes", NULL, 0);
	 if (rval <= 0) {
		 dev_warn(dev, " Get mipi lane num failed!\n");
		 return -1;
	 }
 
	 os08a20->lane_num = rval;
	 if (os08a20->lane_num == 4) {
		 os08a20->cur_mode = &supported_modes_4lane[0];
		 supported_modes = supported_modes_4lane;
		 os08a20->cfg_num = ARRAY_SIZE(supported_modes_4lane);
 
		 /* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
		 os08a20->pixel_rate = MIPI_FREQ * 2U * os08a20->lane_num / 8U;
		 dev_info(dev, "lane_num(%d)  pixel_rate(%u)\n",
			  os08a20->lane_num, os08a20->pixel_rate);
	 } else {
		 dev_err(dev, "unsupported lane_num(%d)\n", os08a20->lane_num);
		 return -1;
	 }
 
	 return 0;
 }
 
 static int os08a20_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
 {
	 struct device *dev = &client->dev;
	 struct device_node *node = dev->of_node;
	 struct os08a20 *os08a20;
	 struct v4l2_subdev *sd;
	 char facing[2] = "b";
	 int ret;
	 const char *sync_mode_name = NULL;
 
	 dev_info(dev, "driver version: %02x.%02x.%02x",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff);
 
	 os08a20 = devm_kzalloc(dev, sizeof(*os08a20), GFP_KERNEL);
	 if (!os08a20)
		 return -ENOMEM;
 
	 ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
					&os08a20->module_index);
	 if (ret) {
		 dev_warn(dev, "could not get module index!\n");
		 os08a20->module_index = 0;
	 }
	 ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
						&os08a20->module_facing);
	 ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
						&os08a20->module_name);
	 ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
						&os08a20->len_name);
	 if (ret) {
		 dev_err(dev, "could not get module information!\n");
		 return -EINVAL;
	 }
 
	 os08a20->sync_mode = NO_SYNC_MODE;
	 os08a20->dt_sync_mode = NO_SYNC_MODE;
	 ret = of_property_read_string(node, RKMODULE_CAMERA_SYNC_MODE,
					   &sync_mode_name);
	 if (!ret) {
		 if (strcmp(sync_mode_name, RKMODULE_EXTERNAL_MASTER_MODE) == 0)
			 os08a20->sync_mode = EXTERNAL_MASTER_MODE;
		 else if (strcmp(sync_mode_name, RKMODULE_INTERNAL_MASTER_MODE) == 0)
			 os08a20->sync_mode = INTERNAL_MASTER_MODE;
		 else if (strcmp(sync_mode_name, RKMODULE_SLAVE_MODE) == 0)
			 os08a20->sync_mode = SLAVE_MODE;
	 }
	 os08a20->dt_sync_mode = os08a20->sync_mode;
 
	 os08a20->client = client;
 
	 os08a20->fsin_pulse_us = OS08A20_FSIN_PULSE_US_DEFAULT;
	 of_property_read_u32(node, OF_FSIN_PULSE_US, &os08a20->fsin_pulse_us);
 
	 os08a20->xvclk = devm_clk_get(dev, "xvclk");
	 if (IS_ERR(os08a20->xvclk)) {
		 dev_err(dev, "Failed to get xvclk\n");
		 return -EINVAL;
	 }
 
	 os08a20->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	 if (IS_ERR(os08a20->power_gpio))
		 dev_warn(dev, "Failed to get power-gpios, maybe no use\n");
 
	 os08a20->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	 if (IS_ERR(os08a20->reset_gpio))
		 dev_warn(dev, "Failed to get reset-gpios, maybe no use\n");
 
	 os08a20->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	 if (IS_ERR(os08a20->pwdn_gpio))
		 dev_warn(dev, "Failed to get pwdn-gpios\n");
 
	 os08a20->fsin_gpio = devm_gpiod_get_optional(dev, "fsin", GPIOD_OUT_LOW);
	 if (IS_ERR(os08a20->fsin_gpio))
		 return dev_err_probe(dev, PTR_ERR(os08a20->fsin_gpio),
					  "Failed to get fsin-gpios\n");
 
	dev_info(dev,
		 "sync_mode(dt)=%u, fsin_gpio=%s, fsin_pulse_us=%u, fsin_trigger=%d\n",
		  os08a20->dt_sync_mode,
		  os08a20->fsin_gpio ? "yes" : "no",
		  os08a20->fsin_pulse_us,
		  os08a20_use_fsin_trigger(os08a20));
 
	 ret = os08a20_configure_regulators(os08a20);
	 if (ret) {
		 dev_err(dev, "Failed to get power regulators\n");
		 return ret;
	 }
	 ret = os08a20_parse_of(os08a20);
	 if (ret != 0)
		 return -EINVAL;
 
	 /* rockchip,default-exposure / rockchip,default-analogue-gain: 触发模式下 V4L2 控件初始值 */
	 os08a20->default_exposure = 0;
	 os08a20->default_anal_gain = 0;
	 if (!of_property_read_u32(node, OF_DEFAULT_EXPOSURE, &os08a20->default_exposure))
		 dev_info(dev, "DT default exposure=%u\n", os08a20->default_exposure);
	 if (!of_property_read_u32(node, OF_DEFAULT_ANALOG_GAIN, &os08a20->default_anal_gain))
		 dev_info(dev, "DT default analogue gain=%u\n", os08a20->default_anal_gain);
 
	 os08a20->pinctrl = devm_pinctrl_get(dev);
	 if (!IS_ERR(os08a20->pinctrl)) {
		 os08a20->pins_default =
			 pinctrl_lookup_state(os08a20->pinctrl,
						  OF_CAMERA_PINCTRL_STATE_DEFAULT);
		 if (IS_ERR(os08a20->pins_default))
			 dev_err(dev, "could not get default pinstate\n");
 
		 os08a20->pins_sleep =
			 pinctrl_lookup_state(os08a20->pinctrl,
						  OF_CAMERA_PINCTRL_STATE_SLEEP);
		 if (IS_ERR(os08a20->pins_sleep))
			 dev_err(dev, "could not get sleep pinstate\n");
	 }
 
	 mutex_init(&os08a20->mutex);
 
	 sd = &os08a20->subdev;
	 v4l2_i2c_subdev_init(sd, client, &os08a20_subdev_ops);
	 ret = devm_device_add_group(dev, &os08a20_attr_group);
	 if (ret) {
		 dev_err(dev, "failed to register sysfs. err: %d\n", ret);
		 goto err_destroy_mutex;
	 }
	 ret = os08a20_initialize_controls(os08a20);
	 if (ret)
		 goto err_destroy_mutex;
 
	 ret = __os08a20_power_on(os08a20);
	 if (ret)
		 goto err_free_handler;
 
	 ret = os08a20_check_sensor_id(os08a20, client);
	 if (ret < 0) {
		 dev_err(&client->dev, "%s(%d) Check id  failed,\n"
			   "check following information:\n"
			   "Power/PowerDown/Reset/Mclk/I2cBus !!\n",
			   __func__, __LINE__);
		 goto err_power_off;
	 }
 
 #ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	 sd->internal_ops = &os08a20_internal_ops;
	 sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			  V4L2_SUBDEV_FL_HAS_EVENTS;
 #endif
 #if defined(CONFIG_MEDIA_CONTROLLER)
	 os08a20->pad.flags = MEDIA_PAD_FL_SOURCE;
	 sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	 ret = media_entity_pads_init(&sd->entity, 1, &os08a20->pad);
	 if (ret < 0)
		 goto err_power_off;
 #endif
 
	 memset(facing, 0, sizeof(facing));
	 if (strcmp(os08a20->module_facing, "back") == 0)
		 facing[0] = 'b';
	 else
		 facing[0] = 'f';
 
	 snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		  os08a20->module_index, facing,
		  OS08A20_NAME, dev_name(sd->dev));
 
	 ret = v4l2_async_register_subdev_sensor_common(sd);
	 if (ret) {
		 dev_err(dev, "v4l2 async register subdev failed\n");
		 goto err_clean_entity;
	 }
 
	 pm_runtime_set_active(dev);
	 pm_runtime_enable(dev);
	 pm_runtime_idle(dev);
 
	 return 0;
 
 err_clean_entity:
 #if defined(CONFIG_MEDIA_CONTROLLER)
	 media_entity_cleanup(&sd->entity);
 #endif
 err_power_off:
	 __os08a20_power_off(os08a20);
 err_free_handler:
	 v4l2_ctrl_handler_free(&os08a20->ctrl_handler);
 err_destroy_mutex:
	 mutex_destroy(&os08a20->mutex);
 
	 return ret;
 }
 
 static int os08a20_remove(struct i2c_client *client)
 {
	 struct v4l2_subdev *sd = i2c_get_clientdata(client);
	 struct os08a20 *os08a20 = to_os08a20(sd);
 
	 v4l2_async_unregister_subdev(sd);
 #if defined(CONFIG_MEDIA_CONTROLLER)
	 media_entity_cleanup(&sd->entity);
 #endif
	 v4l2_ctrl_handler_free(&os08a20->ctrl_handler);
	 mutex_destroy(&os08a20->mutex);
 
	 pm_runtime_disable(&client->dev);
	 if (!pm_runtime_status_suspended(&client->dev))
		 __os08a20_power_off(os08a20);
	 pm_runtime_set_suspended(&client->dev);
 
	 return 0;
 }
 
 #if IS_ENABLED(CONFIG_OF)
 static const struct of_device_id os08a20_of_match[] = {
	 { .compatible = "ovti,os08a20" },
	 {},
 };
 MODULE_DEVICE_TABLE(of, os08a20_of_match);
 #endif
 
 static const struct i2c_device_id os08a20_match_id[] = {
	 { "ovti,os08a20", 0 },
	 { },
 };
 
 static struct i2c_driver os08a20_i2c_driver = {
	 .driver = {
		 .name = OS08A20_NAME,
		 .pm = &os08a20_pm_ops,
		 .of_match_table = of_match_ptr(os08a20_of_match),
	 },
	 .probe		= &os08a20_probe,
	 .remove		= &os08a20_remove,
	 .id_table	= os08a20_match_id,
 };
 
 static int __init sensor_mod_init(void)
 {
	 return i2c_add_driver(&os08a20_i2c_driver);
 }
 
 static void __exit sensor_mod_exit(void)
 {
	 i2c_del_driver(&os08a20_i2c_driver);
 }
 
 device_initcall_sync(sensor_mod_init);
 module_exit(sensor_mod_exit);
 
 MODULE_DESCRIPTION("OmniVision os08a20 sensor driver");
 MODULE_LICENSE("GPL");
 