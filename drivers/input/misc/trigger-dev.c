// SPDX-License-Identifier: GPL-2.0
/*
 * trigger-dev: Generic GPIO input device that triggers camera single-frame capture
 *
 * Features:
 * - GPIO input (edge interrupt)
 * - Press/release prints: "down"(red) / "up"
 * - One-shot trigger per press (must release before next trigger)
 * - Two trigger modes (lower latency first):
 *   1. trigger-output-gpios: directly pulse GPIO (e.g. camera FSIN) - minimal latency
 *   2. trigger-path: write to sysfs "echo 1 > ..." - fallback for software trigger
 *
 * When trigger-output-gpios is set, the camera node must NOT have fsin-gpios
 * (same GPIO, single owner). Use /delete-property/ fsin-gpios in overlay if needed.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>

#define TRIGGER_DEV_MODNAME "trigger-dev"

#define ANSI_RED   "\033[1;31m"
#define ANSI_RESET "\033[0m"

struct trigger_dev {
	struct device		*dev;
	struct gpio_desc	*input_gpiod;
	int			irq;

	/* Input device */
	struct input_dev	*input;
	u32			key_code;
	bool			pressed;

	/* Debounce (ms). If 0, handle state immediately (no debounce). */
	u32			debounce_ms;
	struct delayed_work	debounce_work;

	/* Trigger action: direct GPIO pulse (Mode A), DT: trigger-output-gpios */
	struct gpio_desc	*output_gpiod;
	/* 单次脉冲宽度(us)，DT: trigger-output-pulse-us，默认 50 */
	u32			output_pulse_us;
	/* 每次外部触发产生的脉冲个数，DT: trigger-output-pulse-count，默认 1 */
	u32			output_pulse_count;
	/* 多脉冲时脉冲间隔(us)，DT: trigger-output-pulse-interval-us，0=无间隔 */
	u32			output_pulse_interval_us;

	/* Trigger action: write payload to sysfs path (Mode B), DT: trigger-path */
	char			*trigger_path;
	/* 写入 sysfs 的内容，DT: trigger-value，默认 "1\n" */
	char			*trigger_payload;
	size_t			trigger_payload_len;

	/* 运行时模式：true=GPIO 直连(Mode A)，false=sysfs(Mode B)；两者都配置时优先 GPIO */
	bool			mode_use_gpio;
	struct mutex		mode_mutex;

	atomic_t		trigger_pending;
	struct work_struct	trigger_work;

	/* Debug stats */
	atomic64_t		irq_count;
	atomic64_t		trigger_ok_count;
	atomic64_t		trigger_fail_count;
	u32			max_pending;
	u64			last_irq_ns;
};

#define TRIGGER_OUTPUT_PULSE_US_DEFAULT	50
#define TRIGGER_OUTPUT_PULSE_COUNT_DEFAULT	1

/* Direct GPIO pulse (FSIN): count and interval configurable per external trigger */
static int trigger_dev_pulse_output(struct trigger_dev *tdev)
{
	u32 pulse_us, count, interval_us;
	int i;

	if (!tdev->output_gpiod)
		return -ENODEV;

	mutex_lock(&tdev->mode_mutex);
	pulse_us = tdev->output_pulse_us ? : TRIGGER_OUTPUT_PULSE_US_DEFAULT;
	count = tdev->output_pulse_count ? : TRIGGER_OUTPUT_PULSE_COUNT_DEFAULT;
	interval_us = tdev->output_pulse_interval_us;
	mutex_unlock(&tdev->mode_mutex);

	/*
	 * For ACTIVE_LOW (normally high, pull low on trigger):
	 * gpiod 1 = physical low, 0 = physical high.
	 * Pulse: 1 -> hold -> 0.
	 */
	for (i = 0; i < count; i++) {
		if (i > 0 && interval_us)
			usleep_range(interval_us, interval_us + 100);

		gpiod_set_value_cansleep(tdev->output_gpiod, 1);
		usleep_range(pulse_us, pulse_us + 20);
		gpiod_set_value_cansleep(tdev->output_gpiod, 0);
	}

	return 0;
}

static int trigger_dev_write_once(struct trigger_dev *tdev)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	if (!tdev->trigger_path || !tdev->trigger_payload)
		return -EINVAL;

	filp = filp_open(tdev->trigger_path, O_WRONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = kernel_write(filp, tdev->trigger_payload,
			   tdev->trigger_payload_len, &pos);
	filp_close(filp, NULL);

	if (ret < 0)
		return (int)ret;
	if (ret != tdev->trigger_payload_len)
		return -EIO;

	return 0;
}

static void trigger_dev_trigger_work(struct work_struct *work)
{
	struct trigger_dev *tdev =
		container_of(work, struct trigger_dev, trigger_work);
	bool use_gpio;
	int pending_left;
	u64 t0, dt_us;
	s64 seq;
	int ret;

	/* Drain pending triggers (presses) */
	while (atomic_dec_if_positive(&tdev->trigger_pending) >= 0) {
		t0 = ktime_get_ns();
		mutex_lock(&tdev->mode_mutex);
		use_gpio = tdev->mode_use_gpio && tdev->output_gpiod;
		mutex_unlock(&tdev->mode_mutex);

		if (use_gpio)
			ret = trigger_dev_pulse_output(tdev);
		else if (tdev->trigger_path)
			ret = trigger_dev_write_once(tdev);
		else
			ret = -ENODEV;

		if (ret) {
			atomic64_inc(&tdev->trigger_fail_count);
			dev_err(tdev->dev, "trigger failed: %s ret=%d\n",
				use_gpio ? "gpio" : "sysfs", ret);
		} else {
			atomic64_inc(&tdev->trigger_ok_count);
		}

		seq = atomic64_read(&tdev->trigger_ok_count) +
		      atomic64_read(&tdev->trigger_fail_count);
		pending_left = atomic_read(&tdev->trigger_pending);
		dt_us = div_u64(ktime_get_ns() - t0, 1000);
		dev_info(tdev->dev,
			 "触发处理 seq=%lld mode=%s ret=%d cost=%lluus pending=%d irq_total=%lld ok=%lld fail=%lld\n",
			 seq,
			 use_gpio ? "gpio" : "sysfs",
			 ret,
			 dt_us,
			 pending_left,
			 atomic64_read(&tdev->irq_count),
			 atomic64_read(&tdev->trigger_ok_count),
			 atomic64_read(&tdev->trigger_fail_count));
	}
}

static void trigger_dev_handle_state(struct trigger_dev *tdev, bool pressed_now)
{
	if (pressed_now == tdev->pressed)
		return;

	tdev->pressed = pressed_now;

	if (pressed_now) {
		dev_info(tdev->dev, ANSI_RED "down" ANSI_RESET "\n");
		input_report_key(tdev->input, tdev->key_code, 1);
		input_sync(tdev->input);

		/*
		 * Only one trigger per press. Pending counter ensures we don't
		 * lose fast repeated press cycles even if trigger_work is busy.
		 */
		atomic_inc(&tdev->trigger_pending);
		if ((u32)atomic_read(&tdev->trigger_pending) > tdev->max_pending)
			tdev->max_pending = atomic_read(&tdev->trigger_pending);
		schedule_work(&tdev->trigger_work);
	} else {
		dev_info(tdev->dev, "up\n");
		input_report_key(tdev->input, tdev->key_code, 0);
		input_sync(tdev->input);
	}
}

static void trigger_dev_debounce_work(struct work_struct *work)
{
	struct trigger_dev *tdev =
		container_of(to_delayed_work(work), struct trigger_dev, debounce_work);
	int val;

	val = gpiod_get_value_cansleep(tdev->input_gpiod);
	if (val < 0) {
		dev_err(tdev->dev, "failed to read input gpio: %d\n", val);
		return;
	}

	trigger_dev_handle_state(tdev, !!val);
}

static irqreturn_t trigger_dev_irq(int irq, void *dev_id)
{
	struct trigger_dev *tdev = dev_id;
	int pending;

	tdev->last_irq_ns = ktime_get_ns();
	atomic64_inc(&tdev->irq_count);

	/*
	 * Fast edge mode:
	 * debounce=0 means external trigger may be a narrow pulse. Don't sample
	 * GPIO level in deferred work (may already bounce back), queue one trigger
	 * per IRQ edge directly.
	 */
	if (!tdev->debounce_ms) {
		pending = atomic_inc_return(&tdev->trigger_pending);
		if ((u32)pending > tdev->max_pending)
			tdev->max_pending = pending;
		schedule_work(&tdev->trigger_work);
		if (pending > 1)
			dev_warn(tdev->dev, "触发排队中 pending=%d\n", pending);
		return IRQ_HANDLED;
	}

	mod_delayed_work(system_wq, &tdev->debounce_work,
			 msecs_to_jiffies(tdev->debounce_ms));
	return IRQ_HANDLED;
}

static int trigger_dev_parse_dt(struct device *dev, struct trigger_dev *tdev)
{
	const char *path;
	const char *value;
	const char *attr;
	u32 bus, addr;
	int ret;

	/* Optional: input key code (defaults to KEY_CAMERA) */
	tdev->key_code = KEY_CAMERA;
	device_property_read_u32(dev, "linux,code", &tdev->key_code);

	/* Debounce (ms). Default 0 = no debounce. */
	tdev->debounce_ms = 0;
	device_property_read_u32(dev, "debounce-ms", &tdev->debounce_ms);

	/*
	 * trigger-output-gpios: 直连 GPIO 脉冲(Mode A)，如相机 FSIN。
	 * 使用此模式时，相机节点不得配置 fsin-gpios（同一 GPIO 只能有一个所有者）。
	 */
	tdev->output_gpiod = devm_gpiod_get_optional(dev, "output", GPIOD_OUT_LOW);
	if (IS_ERR(tdev->output_gpiod))
		return PTR_ERR(tdev->output_gpiod);

	/* trigger-output-pulse-us: 脉冲宽度(us)，默认 50 */
	device_property_read_u32(dev, "trigger-output-pulse-us",
				 &tdev->output_pulse_us);
	/* trigger-output-pulse-count: 每次触发的脉冲个数，默认 1 */
	device_property_read_u32(dev, "trigger-output-pulse-count",
				&tdev->output_pulse_count);
	/* trigger-output-pulse-interval-us: 多脉冲间隔(us)，0=无间隔 */
	device_property_read_u32(dev, "trigger-output-pulse-interval-us",
				&tdev->output_pulse_interval_us);

	/*
	 * trigger-path: sysfs write (Mode B). Parse when provided.
	 * - preferred: trigger-path = "/sys/..."
	 * - fallback: trigger-i2c-bus + trigger-i2c-addr (+ optional trigger-attr)
	 */
	ret = device_property_read_string(dev, "trigger-path", &path);
	if (!ret) {
		tdev->trigger_path = devm_kstrdup(dev, path, GFP_KERNEL);
		if (!tdev->trigger_path)
			return -ENOMEM;
	} else if (!tdev->output_gpiod) {
		if (device_property_read_u32(dev, "trigger-i2c-bus", &bus) ||
		    device_property_read_u32(dev, "trigger-i2c-addr", &addr))
			return -EINVAL;

		attr = "trigger";
		device_property_read_string(dev, "trigger-attr", &attr);

		tdev->trigger_path = devm_kasprintf(dev, GFP_KERNEL,
						    "/sys/bus/i2c/devices/%u-%04x/%s",
						    bus, addr, attr);
		if (!tdev->trigger_path)
			return -ENOMEM;
	}

	if (tdev->trigger_path) {
		value = "1";
		device_property_read_string(dev, "trigger-value", &value);
		tdev->trigger_payload = devm_kasprintf(dev, GFP_KERNEL, "%s\n", value);
		if (!tdev->trigger_payload)
			return -ENOMEM;
		tdev->trigger_payload_len = strlen(tdev->trigger_payload);
	}

	/* Need at least one trigger action */
	if (!tdev->output_gpiod && !tdev->trigger_path)
		return -EINVAL;

	/* Default mode: prefer gpio when both configured */
	tdev->mode_use_gpio = tdev->output_gpiod;

	/* Try hardware debounce if available, else keep software debounce. */
	if (tdev->debounce_ms) {
		ret = gpiod_set_debounce(tdev->input_gpiod, tdev->debounce_ms * 1000);
		if (!ret)
			tdev->debounce_ms = 0;
	}

	return 0;
}

static int trigger_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct trigger_dev *tdev;
	int irq;
	int ret;
	int val;
	int gpio;
	bool active_low;
	int gpiod_err;

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->dev = dev;
	platform_set_drvdata(pdev, tdev);
	mutex_init(&tdev->mode_mutex);

	/*
	 * Primary DT property: input-gpios (con_id = "input")
	 * Compatibility with common tutorials: button-gpios (con_id = "button")
	 */
	tdev->input_gpiod = devm_gpiod_get(dev, "input", GPIOD_IN);
	if (IS_ERR(tdev->input_gpiod)) {
		gpiod_err = PTR_ERR(tdev->input_gpiod);
		if (gpiod_err == -ENOENT)
			tdev->input_gpiod = devm_gpiod_get(dev, "button", GPIOD_IN);
	}
	if (IS_ERR(tdev->input_gpiod)) {
		gpiod_err = PTR_ERR(tdev->input_gpiod);
		if (gpiod_err == -EBUSY)
			dev_err(dev, "input gpio is busy (already in use). Please choose a free GPIO in DT.\n");
		return dev_err_probe(dev, gpiod_err, "failed to get input gpio (input-gpios/button-gpios)\n");
	}

	ret = trigger_dev_parse_dt(dev, tdev);
	if (ret)
		return dev_err_probe(dev, ret, "invalid DT properties\n");

	tdev->input = devm_input_allocate_device(dev);
	if (!tdev->input)
		return -ENOMEM;

	tdev->input->name = TRIGGER_DEV_MODNAME;
	tdev->input->id.bustype = BUS_HOST;

	input_set_capability(tdev->input, EV_KEY, tdev->key_code);
	input_set_drvdata(tdev->input, tdev);

	INIT_DELAYED_WORK(&tdev->debounce_work, trigger_dev_debounce_work);
	INIT_WORK(&tdev->trigger_work, trigger_dev_trigger_work);
	atomic_set(&tdev->trigger_pending, 0);
	atomic64_set(&tdev->irq_count, 0);
	atomic64_set(&tdev->trigger_ok_count, 0);
	atomic64_set(&tdev->trigger_fail_count, 0);
	tdev->max_pending = 0;
	tdev->last_irq_ns = 0;

	/* Initialize pressed state without triggering. */
	val = gpiod_get_value_cansleep(tdev->input_gpiod);
	if (val < 0)
		return dev_err_probe(dev, val, "failed to read initial gpio state\n");
	tdev->pressed = !!val;

	ret = input_register_device(tdev->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input device\n");

	/*
	 * If DT provides interrupts (platform IRQ), prefer it. Otherwise fall
	 * back to GPIO-to-IRQ mapping.
	 */
	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0)
		irq = gpiod_to_irq(tdev->input_gpiod);
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get irq for input gpio\n");
	tdev->irq = irq;

	/*
	 * IRQ flags: use IRQF_TRIGGER_FALLING for "active low, normally high"
	 * (falling edge = trigger). If DT specifies interrupts property, the
	 * flags may be overridden by irq_create_of_mapping; request_irq uses
	 * the combined result. For gpiod_to_irq path, we pass the desired flags.
	 */
	ret = devm_request_any_context_irq(dev, tdev->irq, trigger_dev_irq,
					   IRQF_TRIGGER_FALLING,
					   TRIGGER_DEV_MODNAME, tdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	gpio = desc_to_gpio(tdev->input_gpiod);
	active_low = gpiod_is_active_low(tdev->input_gpiod);
	dev_info(dev, "ready: gpio=%d irq=%d active_low=%d debounce_ms=%u key_code=%u mode=%s\n",
		 gpio,
		 tdev->irq,
		 active_low,
		 tdev->debounce_ms,
		 tdev->key_code,
		 tdev->output_gpiod ?
		 "direct-gpio-pulse" : "sysfs-write");
	if (tdev->output_gpiod)
		dev_info(dev, "  output_gpio=%d pulse_us=%u\n",
			 desc_to_gpio(tdev->output_gpiod),
			 tdev->output_pulse_us ? : TRIGGER_OUTPUT_PULSE_US_DEFAULT);
	else
		dev_info(dev, "  trigger_path=%s payload=%s\n",
			 tdev->trigger_path, tdev->trigger_payload);

	return 0;
}

/* Sysfs: mode (gpio|sysfs) */
static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", tdev->mode_use_gpio ? "gpio" : "sysfs");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	bool use_gpio;
	bool can_switch = tdev->output_gpiod && tdev->trigger_path;

	if (!can_switch)
		return -EOPNOTSUPP;

	if (sysfs_streq(buf, "gpio"))
		use_gpio = true;
	else if (sysfs_streq(buf, "sysfs"))
		use_gpio = false;
	else
		return -EINVAL;

	mutex_lock(&tdev->mode_mutex);
	tdev->mode_use_gpio = use_gpio;
	mutex_unlock(&tdev->mode_mutex);

	return count;
}
static DEVICE_ATTR_RW(mode);

/* Sysfs: pulse_count (Mode A only) */
static ssize_t pulse_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	u32 v;

	mutex_lock(&tdev->mode_mutex);
	v = tdev->output_pulse_count ? : TRIGGER_OUTPUT_PULSE_COUNT_DEFAULT;
	mutex_unlock(&tdev->mode_mutex);

	return sysfs_emit(buf, "%u\n", v);
}

static ssize_t pulse_count_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	u32 v;
	int ret;

	if (!tdev->output_gpiod)
		return -ENODEV;

	ret = kstrtou32(buf, 0, &v);
	if (ret || v == 0 || v > 255)
		return -EINVAL;

	mutex_lock(&tdev->mode_mutex);
	tdev->output_pulse_count = v;
	mutex_unlock(&tdev->mode_mutex);

	return count;
}
static DEVICE_ATTR_RW(pulse_count);

/* Sysfs: pulse_interval_us (Mode A only) */
static ssize_t pulse_interval_us_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", tdev->output_pulse_interval_us);
}

static ssize_t pulse_interval_us_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	u32 v;
	int ret;

	if (!tdev->output_gpiod)
		return -ENODEV;

	ret = kstrtou32(buf, 0, &v);
	if (ret)
		return -EINVAL;

	mutex_lock(&tdev->mode_mutex);
	tdev->output_pulse_interval_us = v;
	mutex_unlock(&tdev->mode_mutex);

	return count;
}
static DEVICE_ATTR_RW(pulse_interval_us);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	u64 now = ktime_get_ns();
	u64 ago_us = tdev->last_irq_ns ? div_u64(now - tdev->last_irq_ns, 1000) : 0;

	return sysfs_emit(buf,
			  "irq=%lld ok=%lld fail=%lld pending=%d max_pending=%u last_irq_ago_us=%llu\n",
			  atomic64_read(&tdev->irq_count),
			  atomic64_read(&tdev->trigger_ok_count),
			  atomic64_read(&tdev->trigger_fail_count),
			  atomic_read(&tdev->trigger_pending),
			  tdev->max_pending,
			  ago_us);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *trigger_dev_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_pulse_count.attr,
	&dev_attr_pulse_interval_us.attr,
	&dev_attr_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(trigger_dev);

static int trigger_dev_remove(struct platform_device *pdev)
{
	struct trigger_dev *tdev = platform_get_drvdata(pdev);

	if (tdev) {
		cancel_delayed_work_sync(&tdev->debounce_work);
		cancel_work_sync(&tdev->trigger_work);
	}

	return 0;
}

static const struct of_device_id trigger_dev_of_match[] = {
	{ .compatible = "embedfire,trigger-dev" },
	{ }
};
MODULE_DEVICE_TABLE(of, trigger_dev_of_match);

static struct platform_driver trigger_dev_driver = {
	.probe  = trigger_dev_probe,
	.remove = trigger_dev_remove,
	.driver = {
		.name           = TRIGGER_DEV_MODNAME,
		.of_match_table = of_match_ptr(trigger_dev_of_match),
		.dev_groups     = trigger_dev_groups,
	},
};
module_platform_driver(trigger_dev_driver);

MODULE_DESCRIPTION("Generic GPIO input trigger device (sysfs write on press)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);


