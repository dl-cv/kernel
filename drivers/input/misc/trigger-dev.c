// SPDX-License-Identifier: GPL-2.0
/*
 * trigger-dev: Generic GPIO input device that triggers camera single-frame capture
 *
 * Features:
 * - GPIO input (edge interrupt)
 * - Press/release prints: "down"(red) / "up"
 * - Two input modes:
 *   1. button: debounce + press/release state machine, safe for keys
 *   2. edge: fast active-edge trigger, suitable for clean external pulses
 * - Two trigger modes (lower latency first):
 *   1. trigger-output-gpios: directly pulse GPIO (e.g. camera FSIN) - minimal latency
 *   2. trigger-path: write to sysfs "echo 1 > ..." - fallback for software trigger
 * - Optional result LEDs: ok-led-gpios / ng-led-gpios; sysfs "result" (ok/ng/off),
 *   "result_led_enable"; each external trigger clears LEDs before camera action.
 *
 * When trigger-output-gpios is set, keep a single owner for that output GPIO.
 * If another node already claims the same pin, remove one side in DT.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
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

enum trigger_dev_input_mode {
	TRIGGER_DEV_INPUT_MODE_EDGE = 0,
	TRIGGER_DEV_INPUT_MODE_BUTTON,
};

enum trigger_dev_result_led {
	TRIGGER_DEV_RESULT_NONE = 0,
	TRIGGER_DEV_RESULT_OK,
	TRIGGER_DEV_RESULT_NG,
};

enum trigger_dev_action_mode {
	TRIGGER_DEV_ACTION_SYSFS = 0,
	TRIGGER_DEV_ACTION_GPIO,
	TRIGGER_DEV_ACTION_NOTIFY,
};

struct trigger_dev {
	struct device		*dev;
	struct gpio_desc	*input_gpiod;
	int			irq;
	enum trigger_dev_input_mode input_mode;

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

	/* Runtime action mode; notify keeps IRQ/input events but skips camera action. */
	enum trigger_dev_action_mode action_mode;
	struct mutex		mode_mutex;

	/* 结果指示灯：DT ok-led-gpios / ng-led-gpios；逻辑 0=inactive(灭) 1=active(亮) */
	struct gpio_desc	*ok_led_gpiod;
	struct gpio_desc	*ng_led_gpiod;
	bool			result_led_enable;
	enum trigger_dev_result_led result_led;
	struct mutex		led_mutex;

	atomic_t		trigger_pending;
	atomic_t		trigger_active;
	struct work_struct	trigger_work;

	/* Debug stats */
	atomic64_t		irq_count;
	atomic64_t		trigger_ok_count;
	atomic64_t		trigger_fail_count;
	atomic64_t		trigger_drop_count;
	u32			max_pending;
	u64			last_irq_ns;
};

#define TRIGGER_OUTPUT_PULSE_US_DEFAULT	50
#define TRIGGER_OUTPUT_PULSE_COUNT_DEFAULT	1

static const char *trigger_dev_input_mode_name(enum trigger_dev_input_mode mode)
{
	switch (mode) {
	case TRIGGER_DEV_INPUT_MODE_BUTTON:
		return "button";
	case TRIGGER_DEV_INPUT_MODE_EDGE:
	default:
		return "edge";
	}
}

static int trigger_dev_parse_input_mode(const char *buf,
					enum trigger_dev_input_mode *mode)
{
	if (sysfs_streq(buf, "edge") || sysfs_streq(buf, "pulse")) {
		*mode = TRIGGER_DEV_INPUT_MODE_EDGE;
		return 0;
	}

	if (sysfs_streq(buf, "button")) {
		*mode = TRIGGER_DEV_INPUT_MODE_BUTTON;
		return 0;
	}

	return -EINVAL;
}

static unsigned int
trigger_dev_irq_type_for_mode(struct trigger_dev *tdev,
			      enum trigger_dev_input_mode mode)
{
	bool active_low = gpiod_is_active_low(tdev->input_gpiod);

	if (mode == TRIGGER_DEV_INPUT_MODE_BUTTON)
		return IRQ_TYPE_EDGE_BOTH;

	return active_low ? IRQ_TYPE_EDGE_FALLING : IRQ_TYPE_EDGE_RISING;
}

static int trigger_dev_refresh_pressed_state(struct trigger_dev *tdev)
{
	int val;

	val = gpiod_get_value_cansleep(tdev->input_gpiod);
	if (val < 0)
		return val;

	tdev->pressed = !!val;
	return 0;
}

static int trigger_dev_set_input_mode(struct trigger_dev *tdev,
				      enum trigger_dev_input_mode mode)
{
	unsigned int irq_type = trigger_dev_irq_type_for_mode(tdev, mode);
	int ret;

	disable_irq(tdev->irq);
	cancel_delayed_work_sync(&tdev->debounce_work);

	ret = irq_set_irq_type(tdev->irq, irq_type);
	if (!ret) {
		ret = trigger_dev_refresh_pressed_state(tdev);
		if (!ret)
			WRITE_ONCE(tdev->input_mode, mode);
	}

	enable_irq(tdev->irq);
	return ret;
}

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

static bool trigger_dev_has_result_leds(struct trigger_dev *tdev)
{
	return tdev->ok_led_gpiod || tdev->ng_led_gpiod;
}

/* led_mutex held */
static void trigger_dev_leds_off_locked(struct trigger_dev *tdev)
{
	if (tdev->ok_led_gpiod)
		gpiod_set_value_cansleep(tdev->ok_led_gpiod, 0);
	if (tdev->ng_led_gpiod)
		gpiod_set_value_cansleep(tdev->ng_led_gpiod, 0);
	tdev->result_led = TRIGGER_DEV_RESULT_NONE;
}

/* led_mutex held */
static void trigger_dev_leds_set_ok_locked(struct trigger_dev *tdev)
{
	if (tdev->ng_led_gpiod)
		gpiod_set_value_cansleep(tdev->ng_led_gpiod, 0);
	if (tdev->ok_led_gpiod)
		gpiod_set_value_cansleep(tdev->ok_led_gpiod, 1);
	tdev->result_led = TRIGGER_DEV_RESULT_OK;
}

/* led_mutex held */
static void trigger_dev_leds_set_ng_locked(struct trigger_dev *tdev)
{
	if (tdev->ok_led_gpiod)
		gpiod_set_value_cansleep(tdev->ok_led_gpiod, 0);
	if (tdev->ng_led_gpiod)
		gpiod_set_value_cansleep(tdev->ng_led_gpiod, 1);
	tdev->result_led = TRIGGER_DEV_RESULT_NG;
}

static const char *trigger_dev_action_mode_name(enum trigger_dev_action_mode mode)
{
	switch (mode) {
	case TRIGGER_DEV_ACTION_GPIO:
		return "gpio";
	case TRIGGER_DEV_ACTION_NOTIFY:
		return "notify";
	case TRIGGER_DEV_ACTION_SYSFS:
	default:
		return "sysfs";
	}
}

static void trigger_dev_trigger_work(struct work_struct *work)
{
	struct trigger_dev *tdev =
		container_of(work, struct trigger_dev, trigger_work);
	enum trigger_dev_action_mode action_mode;
	int pending_left;
	u64 t0, dt_us;
	s64 seq;
	int ret;

	/* Drain the single bounded pending request. */
	while (atomic_dec_if_positive(&tdev->trigger_pending) >= 0) {
		atomic_set(&tdev->trigger_active, 1);
		mutex_lock(&tdev->led_mutex);
		if (tdev->result_led_enable)
			trigger_dev_leds_off_locked(tdev);
		mutex_unlock(&tdev->led_mutex);

		t0 = ktime_get_ns();
		mutex_lock(&tdev->mode_mutex);
		action_mode = tdev->action_mode;
		mutex_unlock(&tdev->mode_mutex);

		if (action_mode == TRIGGER_DEV_ACTION_NOTIFY)
			ret = 0;
		else if (action_mode == TRIGGER_DEV_ACTION_GPIO && tdev->output_gpiod)
			ret = trigger_dev_pulse_output(tdev);
		else if (tdev->trigger_path)
			ret = trigger_dev_write_once(tdev);
		else
			ret = -ENODEV;

		if (ret) {
			atomic64_inc(&tdev->trigger_fail_count);
			dev_err(tdev->dev, "trigger failed: %s ret=%d\n",
				trigger_dev_action_mode_name(action_mode), ret);
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
			 trigger_dev_action_mode_name(action_mode),
			 ret,
			 dt_us,
			 pending_left,
			 atomic64_read(&tdev->irq_count),
			 atomic64_read(&tdev->trigger_ok_count),
			 atomic64_read(&tdev->trigger_fail_count));
	}
	atomic_set(&tdev->trigger_active, 0);
}

static bool trigger_dev_queue_request(struct trigger_dev *tdev)
{
	if (atomic_read(&tdev->trigger_active) ||
	    atomic_cmpxchg(&tdev->trigger_pending, 0, 1) != 0) {
		atomic64_inc(&tdev->trigger_drop_count);
		return false;
	}

	if (tdev->max_pending < 1)
		tdev->max_pending = 1;
	schedule_work(&tdev->trigger_work);
	return true;
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

		/* Keep at most one request pending; never replay stale presses later. */
		if (!trigger_dev_queue_request(tdev))
			dev_warn(tdev->dev, "触发忙，丢弃本次按下\n");
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

static void trigger_dev_report_pulse(struct trigger_dev *tdev)
{
	input_event(tdev->input, EV_KEY, tdev->key_code, 1);
	input_sync(tdev->input);
	input_event(tdev->input, EV_KEY, tdev->key_code, 0);
	input_sync(tdev->input);
}

static irqreturn_t trigger_dev_irq(int irq, void *dev_id)
{
	struct trigger_dev *tdev = dev_id;
	enum trigger_dev_input_mode input_mode = READ_ONCE(tdev->input_mode);

	tdev->last_irq_ns = ktime_get_ns();
	atomic64_inc(&tdev->irq_count);

	/*
	 * Fast edge mode:
	 * Queue one trigger for each active edge. This is intended for clean
	 * external pulses where the rising edge should be ignored and the input
	 * may already bounce back before deferred work runs.
	 */
	if (input_mode == TRIGGER_DEV_INPUT_MODE_EDGE) {
		trigger_dev_report_pulse(tdev);
		if (!trigger_dev_queue_request(tdev))
			dev_warn(tdev->dev, "触发忙，丢弃本次边沿\n");
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
	const char *input_mode;
	u32 bus, addr;
	int ret;

	/* Optional: input key code (defaults to KEY_CAMERA) */
	tdev->key_code = KEY_CAMERA;
	device_property_read_u32(dev, "linux,code", &tdev->key_code);

	/* Debounce (ms). Default 0 = no debounce. */
	tdev->debounce_ms = 0;
	device_property_read_u32(dev, "debounce-ms", &tdev->debounce_ms);

	tdev->input_mode = TRIGGER_DEV_INPUT_MODE_EDGE;
	input_mode = "edge";
	ret = device_property_read_string(dev, "trigger-input-mode", &input_mode);
	if (!ret) {
		ret = trigger_dev_parse_input_mode(input_mode, &tdev->input_mode);
		if (ret)
			return ret;
	}

	/*
	 * trigger-output-gpios: 直连 GPIO 脉冲(Mode A)，如相机触发脚。
	 * 使用此模式时，确保该输出 GPIO 没有被其它节点占用。
	 */
	tdev->output_gpiod = devm_gpiod_get_optional(dev, "output", GPIOD_OUT_LOW);
	if (IS_ERR(tdev->output_gpiod))
		return PTR_ERR(tdev->output_gpiod);

	tdev->ok_led_gpiod = devm_gpiod_get_optional(dev, "ok-led", GPIOD_OUT_LOW);
	if (IS_ERR(tdev->ok_led_gpiod))
		return PTR_ERR(tdev->ok_led_gpiod);
	tdev->ng_led_gpiod = devm_gpiod_get_optional(dev, "ng-led", GPIOD_OUT_LOW);
	if (IS_ERR(tdev->ng_led_gpiod))
		return PTR_ERR(tdev->ng_led_gpiod);

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
	tdev->action_mode = tdev->output_gpiod ?
		TRIGGER_DEV_ACTION_GPIO : TRIGGER_DEV_ACTION_SYSFS;

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
	int gpio;
	bool active_low;
	int gpiod_err;

	tdev = devm_kzalloc(dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->dev = dev;
	platform_set_drvdata(pdev, tdev);
	mutex_init(&tdev->mode_mutex);
	mutex_init(&tdev->led_mutex);

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
	atomic_set(&tdev->trigger_active, 0);
	atomic64_set(&tdev->irq_count, 0);
	atomic64_set(&tdev->trigger_ok_count, 0);
	atomic64_set(&tdev->trigger_fail_count, 0);
	atomic64_set(&tdev->trigger_drop_count, 0);
	tdev->max_pending = 0;
	tdev->last_irq_ns = 0;

	ret = trigger_dev_refresh_pressed_state(tdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read initial gpio state\n");

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

	ret = irq_set_irq_type(tdev->irq,
			       trigger_dev_irq_type_for_mode(tdev,
							      tdev->input_mode));
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure irq type\n");

	ret = devm_request_any_context_irq(dev, tdev->irq, trigger_dev_irq,
					   0, TRIGGER_DEV_MODNAME, tdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	gpio = desc_to_gpio(tdev->input_gpiod);
	active_low = gpiod_is_active_low(tdev->input_gpiod);
	dev_info(dev, "ready: gpio=%d irq=%d active_low=%d input_mode=%s debounce_ms=%u key_code=%u mode=%s\n",
		 gpio,
		 tdev->irq,
		 active_low,
		 trigger_dev_input_mode_name(tdev->input_mode),
		 tdev->debounce_ms,
		 tdev->key_code,
		 trigger_dev_action_mode_name(tdev->action_mode));
	if (tdev->input_mode == TRIGGER_DEV_INPUT_MODE_BUTTON &&
	    !tdev->debounce_ms)
		dev_warn(dev, "button input mode is selected with debounce-ms=0; mechanical keys may still bounce\n");
	if (tdev->output_gpiod)
		dev_info(dev, "  output_gpio=%d pulse_us=%u\n",
			 desc_to_gpio(tdev->output_gpiod),
			 tdev->output_pulse_us ? : TRIGGER_OUTPUT_PULSE_US_DEFAULT);
	else
		dev_info(dev, "  trigger_path=%s payload=%s\n",
			 tdev->trigger_path, tdev->trigger_payload);

	mutex_lock(&tdev->led_mutex);
	tdev->result_led_enable = true;
	tdev->result_led = TRIGGER_DEV_RESULT_NONE;
	trigger_dev_leds_off_locked(tdev);
	mutex_unlock(&tdev->led_mutex);

	if (trigger_dev_has_result_leds(tdev)) {
		if (tdev->ok_led_gpiod)
			dev_info(dev, "  ok_led_gpio=%d\n",
				 desc_to_gpio(tdev->ok_led_gpiod));
		if (tdev->ng_led_gpiod)
			dev_info(dev, "  ng_led_gpio=%d\n",
				 desc_to_gpio(tdev->ng_led_gpiod));
	}

	return 0;
}

/* Sysfs: input_mode (button|edge) */
static ssize_t input_mode_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  trigger_dev_input_mode_name(READ_ONCE(tdev->input_mode)));
}

static ssize_t input_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	enum trigger_dev_input_mode mode;
	int ret;

	ret = trigger_dev_parse_input_mode(buf, &mode);
	if (ret)
		return ret;

	if (mode == READ_ONCE(tdev->input_mode))
		return count;

	ret = trigger_dev_set_input_mode(tdev, mode);
	if (ret)
		return ret;

	dev_info(dev, "input mode switched to %s\n",
		 trigger_dev_input_mode_name(mode));
	if (mode == TRIGGER_DEV_INPUT_MODE_BUTTON && !tdev->debounce_ms)
		dev_warn(dev, "button input mode is selected with debounce-ms=0; mechanical keys may still bounce\n");

	return count;
}
static DEVICE_ATTR_RW(input_mode);

/* Sysfs: mode (gpio|sysfs|notify) */
static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	enum trigger_dev_action_mode mode;

	mutex_lock(&tdev->mode_mutex);
	mode = tdev->action_mode;
	mutex_unlock(&tdev->mode_mutex);
	return sysfs_emit(buf, "%s\n", trigger_dev_action_mode_name(mode));
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	enum trigger_dev_action_mode mode;

	if (sysfs_streq(buf, "notify") || sysfs_streq(buf, "listen-only"))
		mode = TRIGGER_DEV_ACTION_NOTIFY;
	else if (sysfs_streq(buf, "gpio")) {
		if (!tdev->output_gpiod)
			return -ENODEV;
		mode = TRIGGER_DEV_ACTION_GPIO;
	} else if (sysfs_streq(buf, "sysfs")) {
		if (!tdev->trigger_path)
			return -ENODEV;
		mode = TRIGGER_DEV_ACTION_SYSFS;
	} else {
		return -EINVAL;
	}

	cancel_work_sync(&tdev->trigger_work);
	atomic_set(&tdev->trigger_pending, 0);
	atomic_set(&tdev->trigger_active, 0);
	mutex_lock(&tdev->mode_mutex);
	tdev->action_mode = mode;
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

static const char *trigger_dev_result_name(enum trigger_dev_result_led r)
{
	switch (r) {
	case TRIGGER_DEV_RESULT_OK:
		return "ok";
	case TRIGGER_DEV_RESULT_NG:
		return "ng";
	default:
		return "none";
	}
}

/* Sysfs: result (ok|ng|off), read: ok|ng|none */
static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	const char *name;

	if (!trigger_dev_has_result_leds(tdev))
		return -ENODEV;

	mutex_lock(&tdev->led_mutex);
	name = trigger_dev_result_name(tdev->result_led);
	mutex_unlock(&tdev->led_mutex);

	return sysfs_emit(buf, "%s\n", name);
}

static ssize_t result_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);

	if (!trigger_dev_has_result_leds(tdev))
		return -ENODEV;

	mutex_lock(&tdev->led_mutex);
	if (!tdev->result_led_enable) {
		mutex_unlock(&tdev->led_mutex);
		return count;
	}

	if (sysfs_streq(buf, "ok")) {
		trigger_dev_leds_set_ok_locked(tdev);
	} else if (sysfs_streq(buf, "ng")) {
		trigger_dev_leds_set_ng_locked(tdev);
	} else if (sysfs_streq(buf, "off")) {
		trigger_dev_leds_off_locked(tdev);
	} else {
		mutex_unlock(&tdev->led_mutex);
		return -EINVAL;
	}
	mutex_unlock(&tdev->led_mutex);

	return count;
}
static DEVICE_ATTR_RW(result);

/* Sysfs: result_led_enable (0=驱动不操作结果灯并灭灯, 1=正常) */
static ssize_t result_led_enable_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	int en;

	if (!trigger_dev_has_result_leds(tdev))
		return -ENODEV;

	mutex_lock(&tdev->led_mutex);
	en = tdev->result_led_enable;
	mutex_unlock(&tdev->led_mutex);

	return sysfs_emit(buf, "%d\n", en);
}

static ssize_t result_led_enable_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	bool v;
	int ret;

	if (!trigger_dev_has_result_leds(tdev))
		return -ENODEV;

	ret = kstrtobool(buf, &v);
	if (ret)
		return ret;

	mutex_lock(&tdev->led_mutex);
	if (!v) {
		tdev->result_led_enable = false;
		trigger_dev_leds_off_locked(tdev);
	} else {
		tdev->result_led_enable = true;
	}
	mutex_unlock(&tdev->led_mutex);

	return count;
}
static DEVICE_ATTR_RW(result_led_enable);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct trigger_dev *tdev = dev_get_drvdata(dev);
	u64 now = ktime_get_ns();
	u64 ago_us = tdev->last_irq_ns ? div_u64(now - tdev->last_irq_ns, 1000) : 0;

	return sysfs_emit(buf,
			  "irq=%lld ok=%lld fail=%lld dropped=%lld active=%d pending=%d max_pending=%u last_irq_ago_us=%llu input_mode=%s debounce_ms=%u\n",
			  atomic64_read(&tdev->irq_count),
			  atomic64_read(&tdev->trigger_ok_count),
			  atomic64_read(&tdev->trigger_fail_count),
			  atomic64_read(&tdev->trigger_drop_count),
			  atomic_read(&tdev->trigger_active),
			  atomic_read(&tdev->trigger_pending),
			  tdev->max_pending,
			  ago_us,
			  trigger_dev_input_mode_name(READ_ONCE(tdev->input_mode)),
			  tdev->debounce_ms);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *trigger_dev_attrs[] = {
	&dev_attr_input_mode.attr,
	&dev_attr_mode.attr,
	&dev_attr_pulse_count.attr,
	&dev_attr_pulse_interval_us.attr,
	&dev_attr_result.attr,
	&dev_attr_result_led_enable.attr,
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

MODULE_DESCRIPTION("Generic GPIO input trigger device (camera trigger + optional result LEDs)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);


