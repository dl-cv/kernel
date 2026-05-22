// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pwm-fan.c - Hwmon driver for fans connected to PWM lines.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 * Author: Kamil Debski <k.debski@samsung.com>
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/timer.h>
#include <soc/rockchip/rockchip_system_monitor.h>

#define MAX_PWM 255
#define PWM_FAN_PROFILE_COUNT 3

enum pwm_fan_mode {
	PWM_FAN_MODE_SILENT = 0,
	PWM_FAN_MODE_NORMAL,
	PWM_FAN_MODE_TURBO,
};

static const char * const pwm_fan_mode_names[PWM_FAN_PROFILE_COUNT] = {
	"silent",
	"normal",
	"turbo",
};

static const char * const pwm_fan_mode_levels_props[PWM_FAN_PROFILE_COUNT] = {
	"rockchip,cooling-levels-silent",
	"rockchip,cooling-levels-normal",
	"rockchip,cooling-levels-turbo",
};

static const char * const pwm_fan_mode_trips_props[PWM_FAN_PROFILE_COUNT] = {
	"rockchip,temp-trips-silent",
	"rockchip,temp-trips-normal",
	"rockchip,temp-trips-turbo",
};

struct thermal_trips {
	int temp;
	int state;
};

struct pwm_fan_profile {
	unsigned int *cooling_levels;
	unsigned int max_state;
	struct thermal_trips *thermal_trips;
};

struct pwm_fan_ctx {
	struct mutex lock;
	struct pwm_device *pwm;
	struct regulator *reg_en;

	int irq;
	atomic_t pulses;
	unsigned int rpm;
	u8 pulses_per_revolution;
	ktime_t sample_start;
	struct timer_list rpm_timer;

	unsigned int pwm_value;
	unsigned int pwm_fan_state;
	unsigned int pwm_fan_max_state;
	unsigned int *pwm_fan_cooling_levels;
	struct pwm_fan_profile profiles[PWM_FAN_PROFILE_COUNT];
	unsigned int mode;
	struct thermal_cooling_device *cdev;
	struct notifier_block thermal_nb;
	struct thermal_trips *thermal_trips;
	int last_temp;
	bool thermal_notifier_is_ok;
	bool last_temp_valid;
	bool mode_control_supported;
	bool manual_mode;	/* when true, thermal notifier does not override pwm1 */
};

/* This handler assumes self resetting edge triggered interrupt. */
static irqreturn_t pulse_handler(int irq, void *dev_id)
{
	struct pwm_fan_ctx *ctx = dev_id;

	atomic_inc(&ctx->pulses);

	return IRQ_HANDLED;
}

static void sample_timer(struct timer_list *t)
{
	struct pwm_fan_ctx *ctx = from_timer(ctx, t, rpm_timer);
	unsigned int delta = ktime_ms_delta(ktime_get(), ctx->sample_start);
	int pulses;

	if (delta) {
		pulses = atomic_read(&ctx->pulses);
		atomic_sub(pulses, &ctx->pulses);
		ctx->rpm = (unsigned int)(pulses * 1000 * 60) /
			(ctx->pulses_per_revolution * delta);

		ctx->sample_start = ktime_get();
	}

	mod_timer(&ctx->rpm_timer, jiffies + HZ);
}

static int  __set_pwm(struct pwm_fan_ctx *ctx, unsigned long pwm)
{
	unsigned long period;
	int ret = 0;
	struct pwm_state state = { };

	mutex_lock(&ctx->lock);
	if (ctx->pwm_value == pwm)
		goto exit_set_pwm_err;

	pwm_init_state(ctx->pwm, &state);
	period = ctx->pwm->args.period;
	state.duty_cycle = DIV_ROUND_UP(pwm * (period - 1), MAX_PWM);
	state.enabled = pwm ? true : false;

	ret = pwm_apply_state(ctx->pwm, &state);
	if (!ret)
		ctx->pwm_value = pwm;
exit_set_pwm_err:
	mutex_unlock(&ctx->lock);
	return ret;
}

static void pwm_fan_update_state(struct pwm_fan_ctx *ctx, unsigned long pwm)
{
	int i;

	if (!ctx->pwm_fan_cooling_levels) {
		ctx->pwm_fan_state = 0;
		return;
	}

	for (i = 0; i < ctx->pwm_fan_max_state; ++i)
		if (pwm < ctx->pwm_fan_cooling_levels[i + 1])
			break;

	ctx->pwm_fan_state = i;
}

static const char *pwm_fan_mode_name(unsigned int mode)
{
	if (mode >= PWM_FAN_PROFILE_COUNT)
		return "unknown";

	return pwm_fan_mode_names[mode];
}

static int pwm_fan_apply_temp(struct pwm_fan_ctx *ctx, int temp);
static int pwm_fan_switch_mode(struct pwm_fan_ctx *ctx, unsigned int mode,
			       bool apply_immediately);

static ssize_t pwm_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long pwm;
	int ret;

	if (kstrtoul(buf, 10, &pwm) || pwm > MAX_PWM)
		return -EINVAL;

	ret = __set_pwm(ctx, pwm);
	if (ret)
		return ret;

	pwm_fan_update_state(ctx, pwm);
	return count;
}

static ssize_t pwm_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->pwm_value);
}

static ssize_t rpm_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->rpm);
}

static ssize_t manual_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->manual_mode ? 1 : 0);
}

static ssize_t manual_mode_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long val;
	int ret = 0;
	bool old_manual_mode;

	if (kstrtoul(buf, 10, &val) || val > 1)
		return -EINVAL;

	old_manual_mode = ctx->manual_mode;
	ctx->manual_mode = !!val;

	/*
	 * Restore automatic thermal control immediately when leaving manual mode
	 * so userspace does not need to wait for the next temp notification.
	 */
	if (old_manual_mode && !ctx->manual_mode &&
	    ctx->thermal_notifier_is_ok && ctx->last_temp_valid) {
		ret = pwm_fan_apply_temp(ctx, ctx->last_temp);
		if (ret)
			return ret;
	}

	return count;
}

static DEVICE_ATTR_RW(manual_mode);

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->mode);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long mode;
	int ret;

	if (!ctx->mode_control_supported)
		return -EOPNOTSUPP;
	if (kstrtoul(buf, 10, &mode) || mode >= PWM_FAN_PROFILE_COUNT)
		return -EINVAL;

	ret = pwm_fan_switch_mode(ctx, mode, true);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(fan_mode);

static ssize_t fan_mode_name_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", pwm_fan_mode_name(ctx->mode));
}

static ssize_t fan_mode_name_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned int mode;
	int ret;

	if (!ctx->mode_control_supported)
		return -EOPNOTSUPP;

	for (mode = 0; mode < PWM_FAN_PROFILE_COUNT; mode++) {
		if (!sysfs_streq(buf, pwm_fan_mode_names[mode]))
			continue;

		ret = pwm_fan_switch_mode(ctx, mode, true);
		if (ret)
			return ret;

		return count;
	}

	return -EINVAL;
}

static DEVICE_ATTR_RW(fan_mode_name);

static ssize_t fan_mode_names_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s %s %s\n",
		       pwm_fan_mode_names[PWM_FAN_MODE_SILENT],
		       pwm_fan_mode_names[PWM_FAN_MODE_NORMAL],
		       pwm_fan_mode_names[PWM_FAN_MODE_TURBO]);
}

static DEVICE_ATTR_RO(fan_mode_names);

static SENSOR_DEVICE_ATTR_RW(pwm1, pwm, 0);
static SENSOR_DEVICE_ATTR_RO(fan1_input, rpm, 0);

static struct attribute *pwm_fan_attrs[] = {
	&sensor_dev_attr_pwm1.dev_attr.attr,
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&dev_attr_manual_mode.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_fan_mode_name.attr,
	&dev_attr_fan_mode_names.attr,
	NULL,
};

static umode_t pwm_fan_attrs_visible(struct kobject *kobj, struct attribute *a,
				     int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	(void)n;

	/* Hide fan1_input in case no interrupt is available */
	if (a == &sensor_dev_attr_fan1_input.dev_attr.attr && ctx->irq <= 0)
		return 0;

	if ((a == &dev_attr_fan_mode.attr ||
	     a == &dev_attr_fan_mode_name.attr ||
	     a == &dev_attr_fan_mode_names.attr) &&
	    !ctx->mode_control_supported)
		return 0;

	return a->mode;
}

static const struct attribute_group pwm_fan_group = {
	.attrs = pwm_fan_attrs,
	.is_visible = pwm_fan_attrs_visible,
};

static const struct attribute_group *pwm_fan_groups[] = {
	&pwm_fan_group,
	NULL,
};

/* thermal cooling device callbacks */
static int pwm_fan_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->pwm_fan_max_state;

	return 0;
}

static int pwm_fan_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->pwm_fan_state;

	return 0;
}

static int
pwm_fan_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;
	int ret;

	if (!ctx || (state > ctx->pwm_fan_max_state))
		return -EINVAL;

	if (state == ctx->pwm_fan_state)
		return 0;

	ret = __set_pwm(ctx, ctx->pwm_fan_cooling_levels[state]);
	if (ret) {
		dev_err(&cdev->device, "Cannot set pwm!\n");
		return ret;
	}

	ctx->pwm_fan_state = state;

	return ret;
}

static const struct thermal_cooling_device_ops pwm_fan_cooling_ops = {
	.get_max_state = pwm_fan_get_max_state,
	.get_cur_state = pwm_fan_get_cur_state,
	.set_cur_state = pwm_fan_set_cur_state,
};

static int pwm_fan_get_cooling_levels(struct device *dev, const char *prop_name,
				      unsigned int **cooling_levels,
				      unsigned int *max_state)
{
	struct device_node *np = dev->of_node;
	unsigned int *levels;
	int num, i, ret;

	if (!of_find_property(np, prop_name, NULL))
		return -ENOENT;

	ret = of_property_count_u32_elems(np, prop_name);
	if (ret <= 0) {
		dev_err(dev, "Invalid property '%s'\n", prop_name);
		return ret ? : -EINVAL;
	}

	num = ret;
	levels = devm_kcalloc(dev, num, sizeof(u32), GFP_KERNEL);
	if (!levels)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, prop_name, levels, num);
	if (ret) {
		dev_err(dev, "Property '%s' cannot be read!\n", prop_name);
		return ret;
	}

	for (i = 0; i < num; i++) {
		if (levels[i] > MAX_PWM) {
			dev_err(dev, "Property '%s' state[%d]:%d > %d\n",
				prop_name, i, levels[i], MAX_PWM);
			return -EINVAL;
		}
	}

	*cooling_levels = levels;
	*max_state = num - 1;

	return 0;
}

static int pwm_fan_of_get_cooling_data(struct device *dev,
				       struct pwm_fan_ctx *ctx)
{
	int ret;

	ret = pwm_fan_get_cooling_levels(dev, "cooling-levels",
					 &ctx->pwm_fan_cooling_levels,
					 &ctx->pwm_fan_max_state);
	if (ret == -ENOENT)
		return 0;

	return ret;
}

static void pwm_fan_regulator_disable(void *data)
{
	regulator_disable(data);
}

static void pwm_fan_pwm_disable(void *__ctx)
{
	struct pwm_fan_ctx *ctx = __ctx;
	pwm_disable(ctx->pwm);
	del_timer_sync(&ctx->rpm_timer);
}

static int pwm_fan_get_thermal_trips(struct device *dev, const char *prop_name,
				     struct thermal_trips **trips)
{
	struct device_node *np = dev->of_node;
	struct thermal_trips *thermal_trips;
	const struct property *prop;
	int count, i;

	prop = of_find_property(np, prop_name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	count = of_property_count_u32_elems(np, prop_name);
	if (count < 0)
		return -EINVAL;
	if (count % 2)
		return -EINVAL;
	thermal_trips = devm_kzalloc(dev,
				     sizeof(*thermal_trips) * (count / 2 + 1),
				     GFP_KERNEL);
	if (!thermal_trips)
		return -ENOMEM;

	for (i = 0; i < count / 2; i++) {
		of_property_read_u32_index(np, prop_name, 2 * i,
					   &thermal_trips[i].temp);
		of_property_read_u32_index(np, prop_name, 2 * i + 1,
					   &thermal_trips[i].state);
	}
	thermal_trips[i].temp = 0;
	thermal_trips[i].state = INT_MAX;

	*trips = thermal_trips;

	return 0;
}

static int pwm_fan_validate_thermal_trips(struct device *dev,
					  const char *prop_name,
					  struct thermal_trips *trips,
					  unsigned int max_state)
{
	int i;

	for (i = 0; trips[i].state != INT_MAX; i++) {
		if (trips[i].state < 0 || trips[i].state > max_state) {
			dev_err(dev,
				"Property '%s' state[%d]:%d > max_state(%u)\n",
				prop_name, i, trips[i].state, max_state);
			return -EINVAL;
		}
	}

	return 0;
}

static void pwm_fan_use_legacy_profiles(struct pwm_fan_ctx *ctx,
					struct thermal_trips *legacy_trips)
{
	int i;

	for (i = 0; i < PWM_FAN_PROFILE_COUNT; i++) {
		ctx->profiles[i].cooling_levels = ctx->pwm_fan_cooling_levels;
		ctx->profiles[i].max_state = ctx->pwm_fan_max_state;
		ctx->profiles[i].thermal_trips = legacy_trips;
	}
}

static int pwm_fan_init_profiles(struct device *dev, struct pwm_fan_ctx *ctx)
{
	struct device_node *np = dev->of_node;
	struct thermal_trips *legacy_trips;
	bool has_profile_props = false;
	bool has_all_profile_props = true;
	int i, ret;

	ret = pwm_fan_get_thermal_trips(dev, "rockchip,temp-trips",
					&legacy_trips);
	if (ret)
		return ret;

	ret = pwm_fan_validate_thermal_trips(dev, "rockchip,temp-trips",
					     legacy_trips,
					     ctx->pwm_fan_max_state);
	if (ret)
		return ret;

	for (i = 0; i < PWM_FAN_PROFILE_COUNT; i++) {
		bool has_levels;
		bool has_trips;

		has_levels = !!of_find_property(np, pwm_fan_mode_levels_props[i],
						NULL);
		has_trips = !!of_find_property(np, pwm_fan_mode_trips_props[i],
					       NULL);
		if (has_levels || has_trips)
			has_profile_props = true;
		if (!has_levels || !has_trips)
			has_all_profile_props = false;
	}

	if (!has_profile_props || !has_all_profile_props) {
		if (has_profile_props && !has_all_profile_props)
			dev_warn(dev,
				 "Incomplete profile properties, using legacy curve\n");
		pwm_fan_use_legacy_profiles(ctx, legacy_trips);
		return 0;
	}

	for (i = 0; i < PWM_FAN_PROFILE_COUNT; i++) {
		ret = pwm_fan_get_cooling_levels(dev,
						 pwm_fan_mode_levels_props[i],
						 &ctx->profiles[i].cooling_levels,
						 &ctx->profiles[i].max_state);
		if (ret)
			return ret;

		ret = pwm_fan_get_thermal_trips(dev,
						pwm_fan_mode_trips_props[i],
						&ctx->profiles[i].thermal_trips);
		if (ret)
			return ret;

		ret = pwm_fan_validate_thermal_trips(dev,
						     pwm_fan_mode_trips_props[i],
						     ctx->profiles[i].thermal_trips,
						     ctx->profiles[i].max_state);
		if (ret)
			return ret;
	}

	return 0;
}

static int pwm_fan_temp_to_state(struct pwm_fan_ctx *ctx, int temp)
{
	struct thermal_trips *trips = ctx->thermal_trips;
	int i, state = 0;

	if (!trips)
		return state;

	for (i = 0; trips[i].state != INT_MAX; i++) {
		if (temp >= trips[i].temp)
			state = trips[i].state;
	}

	return state;
}

static int pwm_fan_apply_temp(struct pwm_fan_ctx *ctx, int temp)
{
	int state, ret;

	state = pwm_fan_temp_to_state(ctx, temp);
	if (state > ctx->pwm_fan_max_state)
		state = ctx->pwm_fan_max_state;
	if (state == ctx->pwm_fan_state)
		return 0;

	ret = __set_pwm(ctx, ctx->pwm_fan_cooling_levels[state]);
	if (ret)
		return ret;

	ctx->pwm_fan_state = state;

	return 0;
}

static int pwm_fan_switch_mode(struct pwm_fan_ctx *ctx, unsigned int mode,
			       bool apply_immediately)
{
	struct pwm_fan_profile *profile;

	if (mode >= PWM_FAN_PROFILE_COUNT)
		return -EINVAL;

	profile = &ctx->profiles[mode];
	if (!profile->cooling_levels || !profile->thermal_trips)
		return -EINVAL;

	ctx->mode = mode;
	ctx->pwm_fan_cooling_levels = profile->cooling_levels;
	ctx->pwm_fan_max_state = profile->max_state;
	ctx->thermal_trips = profile->thermal_trips;
	pwm_fan_update_state(ctx, ctx->pwm_value);

	if (!apply_immediately || !ctx->thermal_notifier_is_ok ||
	    ctx->manual_mode || !ctx->last_temp_valid)
		return 0;

	return pwm_fan_apply_temp(ctx, ctx->last_temp);
}

static int pwm_fan_thermal_notifier_call(struct notifier_block *nb,
					 unsigned long event, void *data)
{
	struct pwm_fan_ctx *ctx = container_of(nb, struct pwm_fan_ctx, thermal_nb);
	struct system_monitor_event_data *event_data = data;
	int ret;

	if (event != SYSTEM_MONITOR_CHANGE_TEMP)
		return NOTIFY_OK;

	ctx->last_temp = event_data->temp;
	ctx->last_temp_valid = true;
	if (ctx->manual_mode)
		return NOTIFY_OK;

	ret = pwm_fan_apply_temp(ctx, event_data->temp);
	if (ret)
		return NOTIFY_BAD;

	return NOTIFY_OK;
}

static int pwm_fan_register_thermal_notifier(struct pwm_fan_ctx *ctx)
{
	ctx->thermal_nb.notifier_call = pwm_fan_thermal_notifier_call;

	return rockchip_system_monitor_register_notifier(&ctx->thermal_nb);
}

static int pwm_fan_probe(struct platform_device *pdev)
{
	struct thermal_cooling_device *cdev;
	struct device *dev = &pdev->dev;
	struct pwm_fan_ctx *ctx;
	struct device *hwmon;
	int ret;
	struct pwm_state state = { };
	u32 ppr = 2;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);

	ctx->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(ctx->pwm))
		return dev_err_probe(dev, PTR_ERR(ctx->pwm), "Could not get PWM\n");

	platform_set_drvdata(pdev, ctx);

	ctx->irq = platform_get_irq_optional(pdev, 0);
	if (ctx->irq == -EPROBE_DEFER)
		return ctx->irq;

	ctx->reg_en = devm_regulator_get_optional(dev, "fan");
	if (IS_ERR(ctx->reg_en)) {
		if (PTR_ERR(ctx->reg_en) != -ENODEV)
			return PTR_ERR(ctx->reg_en);

		ctx->reg_en = NULL;
	} else {
		ret = regulator_enable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to enable fan supply: %d\n", ret);
			return ret;
		}
		ret = devm_add_action_or_reset(dev, pwm_fan_regulator_disable,
					       ctx->reg_en);
		if (ret)
			return ret;
	}

	/* Default PWM: from DT "default-pwm" (0-255), or 128 (medium speed) */
	ctx->pwm_value = 128;
	of_property_read_u32(dev->of_node, "default-pwm", &ctx->pwm_value);
	if (ctx->pwm_value > MAX_PWM)
		ctx->pwm_value = MAX_PWM;

	pwm_init_state(ctx->pwm, &state);
	/*
	 * __set_pwm assumes that MAX_PWM * (period - 1) fits into an unsigned
	 * long. Check this here to prevent the fan running at a too low
	 * frequency.
	 */
	if (state.period > ULONG_MAX / MAX_PWM + 1) {
		dev_err(dev, "Configured period too big\n");
		return -EINVAL;
	}

	/* Set duty cycle from default-pwm and enable PWM output */
	state.duty_cycle = DIV_ROUND_UP(ctx->pwm_value * (state.period - 1), MAX_PWM);
	state.enabled = (ctx->pwm_value > 0);

	ret = pwm_apply_state(ctx->pwm, &state);
	if (ret) {
		dev_err(dev, "Failed to configure PWM: %d\n", ret);
		return ret;
	}
	timer_setup(&ctx->rpm_timer, sample_timer, 0);
	ret = devm_add_action_or_reset(dev, pwm_fan_pwm_disable, ctx);
	if (ret)
		return ret;

	of_property_read_u32(dev->of_node, "pulses-per-revolution", &ppr);
	ctx->pulses_per_revolution = ppr;
	if (!ctx->pulses_per_revolution) {
		dev_err(dev, "pulses-per-revolution can't be zero.\n");
		return -EINVAL;
	}

	if (ctx->irq > 0) {
		ret = devm_request_irq(dev, ctx->irq, pulse_handler, 0,
				       pdev->name, ctx);
		if (ret) {
			dev_err(dev, "Failed to request interrupt: %d\n", ret);
			return ret;
		}
		ctx->sample_start = ktime_get();
		mod_timer(&ctx->rpm_timer, jiffies + HZ);
	}

	ret = pwm_fan_of_get_cooling_data(dev, ctx);
	if (ret)
		return ret;

	pwm_fan_update_state(ctx, ctx->pwm_value);
	if (IS_REACHABLE(CONFIG_ROCKCHIP_SYSTEM_MONITOR) &&
	    of_find_property(dev->of_node, "rockchip,temp-trips", NULL)) {
		ret = pwm_fan_init_profiles(dev, ctx);
		if (ret)
			return ret;

		ret = pwm_fan_switch_mode(ctx, PWM_FAN_MODE_NORMAL, false);
		if (ret)
			return ret;

		ret = pwm_fan_register_thermal_notifier(ctx);
		if (ret)
			dev_err(dev, "Failed to register thermal notifier: %d\n", ret);
		else {
			ctx->thermal_notifier_is_ok = true;
			ctx->mode_control_supported = true;
		}
	} else if (IS_ENABLED(CONFIG_THERMAL)) {
		cdev = devm_thermal_of_cooling_device_register(dev,
			dev->of_node, "pwm-fan", ctx, &pwm_fan_cooling_ops);
		if (IS_ERR(cdev)) {
			ret = PTR_ERR(cdev);
			dev_err(dev,
				"Failed to register pwm-fan as cooling device: %d\n",
				ret);
			return ret;
		}
		ctx->cdev = cdev;
		/* thermal_cdev_update(cdev); */
	}

	hwmon = devm_hwmon_device_register_with_groups(dev, "pwmfan",
						       ctx, pwm_fan_groups);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Failed to register hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

static int pwm_fan_disable(struct device *dev)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	struct pwm_args args;
	int ret;

	pwm_get_args(ctx->pwm, &args);

	if (ctx->pwm_value || ctx->thermal_notifier_is_ok) {
		ret = pwm_config(ctx->pwm, 0, args.period);
		if (ret < 0)
			return ret;

		pwm_disable(ctx->pwm);
	}

	if (ctx->reg_en) {
		ret = regulator_disable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to disable fan supply: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void pwm_fan_shutdown(struct platform_device *pdev)
{
	pwm_fan_disable(&pdev->dev);
}

#ifdef CONFIG_PM_SLEEP
static int pwm_fan_suspend(struct device *dev)
{
	return pwm_fan_disable(dev);
}

static int pwm_fan_resume(struct device *dev)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	struct pwm_args pargs;
	unsigned long duty;
	int ret;

	if (ctx->reg_en) {
		ret = regulator_enable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to enable fan supply: %d\n", ret);
			return ret;
		}
	}

	if (ctx->pwm_value == 0 && !ctx->thermal_notifier_is_ok)
		return 0;

	pwm_get_args(ctx->pwm, &pargs);
	duty = DIV_ROUND_UP_ULL(ctx->pwm_value * (pargs.period - 1), MAX_PWM);
	ret = pwm_config(ctx->pwm, duty, pargs.period);
	if (ret)
		return ret;
	return pwm_enable(ctx->pwm);
}
#endif

static DEFINE_SIMPLE_DEV_PM_OPS(pwm_fan_pm, pwm_fan_suspend, pwm_fan_resume);

static const struct of_device_id of_pwm_fan_match[] = {
	{ .compatible = "pwm-fan", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_fan_match);

static struct platform_driver pwm_fan_driver = {
	.probe		= pwm_fan_probe,
	.shutdown	= pwm_fan_shutdown,
	.driver	= {
		.name		= "pwm-fan",
		.pm		= &pwm_fan_pm,
		.of_match_table	= of_pwm_fan_match,
	},
};

module_platform_driver(pwm_fan_driver);

MODULE_AUTHOR("Kamil Debski <k.debski@samsung.com>");
MODULE_ALIAS("platform:pwm-fan");
MODULE_DESCRIPTION("PWM FAN driver");
MODULE_LICENSE("GPL");
