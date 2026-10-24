/*
 * Deterministic host harness for the real AR0521 GRR functions.
 *
 * This is not a model of the kernel scheduler or the camera hardware. It
 * supplies interleaving points for I2C, frame IRQs, completion waits, and
 * workqueue joining so the real state machine and cleanup order can run.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#define UNUSED(x) ((void)(x))

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n) (1UL << (n))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define DIV_ROUND_UP_ULL(a, b) (((u64)(a) + (u64)(b) - 1ULL) / (u64)(b))
#define READ_ONCE(value) (*(volatile __typeof__(value) *)&(value))
#define WRITE_ONCE(value, new_value) \
	do { *(volatile __typeof__(value) *)&(value) = (new_value); } while (0)

struct device {
	int marker;
};

struct i2c_client {
	u16 addr;
};

struct pwm_device {
	int marker;
};

struct v4l2_subdev {
	void *priv;
	void *fwnode;
};

struct notifier_block {
	int (*notifier_call)(struct notifier_block *nb, unsigned long action, void *data);
};

struct rk_camera_frame_event {
	void *terminal;
	u32 mask;
};

struct workqueue_struct {
	int marker;
};

struct work_struct {
	void (*func)(struct work_struct *work);
	bool queued;
	bool cancelled;
	bool completed;
};

struct completion {
	unsigned int done;
};

struct mutex {
	int marker;
};

typedef struct {
	int counter;
} atomic_t;

typedef struct {
	pthread_mutex_t lock;
} spinlock_t;

#define atomic_read(atomic) ((atomic)->counter)
#define atomic_set(atomic, value) ((atomic)->counter = (value))
#define atomic_cmpxchg(atomic, old_value, new_value) \
	__sync_val_compare_and_swap(&(atomic)->counter, (old_value), (new_value))

#define spin_lock_init(spinp) pthread_mutex_init(&((spinp)->lock), NULL)
#define spin_lock_irqsave(spinp, flags) \
	do { (flags) = 0; pthread_mutex_lock(&((spinp)->lock)); } while (0)
#define spin_unlock_irqrestore(spinp, flags) \
	do { UNUSED(flags); pthread_mutex_unlock(&((spinp)->lock)); } while (0)
#define lockdep_assert_held(lock) UNUSED(lock)

#define AR0521_REG_VD_TRIG_NEW_FRAME 0x3158
#define AR0521_REG_GPIO_GPO_2 0x30FA
#define AR0521_REG_GLOBAL_SEQ_TRIGGER 0x315E
#define AR0521_REG_GLOBAL_READ_START 0x3166
#define AR0521_REG_GLOBAL_READ_START_HI 0x3168
#define AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR BIT(0)
#define AR0521_REG_RESET 0x301A
#define AR0521_REG_RESET_GPI BIT(8)
#define AR0521_GRR_SCALE 512U
#define AR0521_VT_PIX_CLK_HZ 205200000ULL
#define AR0521_SLAVE_VD_TRIG 0x8000

#define RK_CAMERA_FRAME_START BIT(0)
#define RK_CAMERA_FRAME_END BIT(1)
#define RK_CAMERA_FRAME_ERROR BIT(2)

enum ar0521_grr_phase {
	AR0521_GRR_IDLE,
	AR0521_GRR_WAIT_START,
	AR0521_GRR_WAIT_END,
	AR0521_GRR_DONE,
	AR0521_GRR_ERROR,
};

enum ar0521_op_mode {
	AR0521_FREE_RUN = 0,
	AR0521_TRIGGER_ONE_SHOT = 1,
};

struct ar0521_test_format {
	u32 height;
};

struct ar0521 {
	struct device *dev;
	struct i2c_client *client;
	struct pwm_device *trigger_pwm;
	struct mutex mutex;

	struct work_struct grr_pwm_work;
	struct completion grr_start;
	struct completion grr_end;
	spinlock_t grr_irq_lock;
	struct notifier_block grr_frame_notifier;
	atomic_t grr_phase;
	atomic_t grr_released;
	atomic_t grr_release_requested;
	bool grr_rearm;
	bool grr_faulted;
	int grr_pwm_ret;
	int grr_last_ret;
	u64 grr_pwm_call_ns;
	u64 grr_start_ns;
	u64 grr_end_ns;
	u64 grr_release_ns;
	u64 grr_release_request_ns;
	u64 grr_reclose_ns;
	u32 grr_gate_calls;
	u32 grr_gate_fail_call;
	u16 grr_gate_target;
	u16 grr_gate_actual;
	int grr_gate_write_ret;
	int grr_gate_read_ret;

	struct v4l2_subdev sd;
	struct ar0521_test_format format;
	bool streaming;
	enum ar0521_op_mode active_mode;
	u32 light_source_advance_us;
	u32 light_source_off_delay_us;
	bool light_source_enabled;
	void *light_source_gpio;
};

enum event_kind {
	EV_REG_WRITE,
	EV_REG_READ,
	EV_IRQ,
	EV_WAIT,
	EV_POLL,
	EV_QUEUE,
	EV_FLUSH,
	EV_CANCEL,
	EV_PWM,
	EV_PWM_IDLE,
	EV_STREAM_OFF,
	EV_LIGHT,
	EV_SLEEP,
};

struct event {
	enum event_kind kind;
	u16 reg;
	int value;
	int ok;
	int phase;
	int phase_after;
	int released;
	int handled;
};

enum wait_action {
	WAIT_NONE,
	WAIT_START,
	WAIT_END,
	WAIT_START_THEN_START,
	WAIT_START_THEN_ERROR,
	WAIT_ERROR,
};

struct mock_state {
	struct device dev;
	struct i2c_client client;
	struct pwm_device pwm;
	struct workqueue_struct wq;
	struct ar0521 sensor;
	int terminal_fwnode;
	int other_fwnode;

	struct event events[256];
	int event_count;

	enum wait_action start_action;
	enum wait_action end_action;

	bool queue_result;
	bool poll_never_ready;
	int poll_override;
	int poll_calls;

	int pwm_ret;
	int pwm_calls;
	int init_trigger_pwm_calls;
	int init_trigger_pwm_ret;
	int stream_on_calls;
	int stream_off_calls;
	int stream_bit_ret;
	int exit_lp11_calls;

	int fail_write_remaining;
	u16 fail_write_reg;
	u16 fail_write_value;
	int fail_read_remaining;
	u16 fail_read_reg;
	u32 fail_read_gate_call;
	int mismatch_vd_read;

	bool inject_start_on_release_write;
	bool inject_start_on_release_read;
	bool inject_start_and_end_on_release_read;
	bool inject_start_on_prepare_read;
	bool inject_end_on_first_vd_read;

	u16 vd_reg;
	u16 seq_trigger;
	int vd_reads;
	int gpio_reads;
	u16 gpio_before_rearm;
	u16 gpio_after_rearm;
	u64 clock_ns;
	u64 clock_step_ns;
	bool initialized;
};

static struct mock_state g;
static struct workqueue_struct *const __attribute__((unused)) system_highpri_wq = &g.wq;
#define to_ar0521(sd) ((struct ar0521 *)(sd)->priv)

static int ar0521_grr_frame_irq(struct v4l2_subdev *sd, u32 status,
				bool *handled);
static int ar0521_grr_frame_notify(struct notifier_block *nb,
					unsigned long action, void *data);
static int ar0521_grr_gate(struct ar0521 *sensor, u16 value);
static int ar0521_grr_trigger_locked(struct ar0521 *sensor);
static void ar0521_grr_pwm_work(struct work_struct *work);


static void record_event(enum event_kind kind, u16 reg, int value, int ok,
			 int phase, int phase_after, int released, int handled)
{
	struct event *event;

	if (g.event_count >= (int)(sizeof(g.events) / sizeof(g.events[0])))
		return;
	event = &g.events[g.event_count++];
	*event = (struct event) {
		.kind = kind,
		.reg = reg,
		.value = value,
		.ok = ok,
		.phase = phase,
		.phase_after = phase_after,
		.released = released,
		.handled = handled,
	};
}

static u64 ktime_get_ns(void)
{
	g.clock_ns += g.clock_step_ns;
	return g.clock_ns;
}

static unsigned long usecs_to_jiffies(u64 usecs)
{
	return (unsigned long)usecs;
}

static u64 ar0521_line_period_ns(const struct ar0521 *sensor)
{
	UNUSED(sensor);
	return 10000ULL;
}

static void init_completion(struct completion *completion)
{
	completion->done = 0;
}

static void reinit_completion(struct completion *completion)
{
	completion->done = 0;
}

static void complete(struct completion *completion)
{
	completion->done++;
}

static void mock_irq(u32 status)
{
	struct ar0521 *sensor = &g.sensor;
	bool handled = false;
	int phase = atomic_read(&sensor->grr_phase);
	int released = atomic_read(&sensor->grr_released);

	ar0521_grr_frame_irq(&sensor->sd, status, &handled);
	record_event(EV_IRQ, 0, status, 1, phase,
		     atomic_read(&sensor->grr_phase), released, handled);
}

static void apply_wait_action(enum wait_action action)
{
	switch (action) {
	case WAIT_NONE:
		break;
	case WAIT_START:
		mock_irq(RK_CAMERA_FRAME_START);
		break;
	case WAIT_END:
		mock_irq(RK_CAMERA_FRAME_END);
		break;
	case WAIT_START_THEN_START:
		mock_irq(RK_CAMERA_FRAME_START);
		mock_irq(RK_CAMERA_FRAME_START);
		break;
	case WAIT_START_THEN_ERROR:
		mock_irq(RK_CAMERA_FRAME_START);
		mock_irq(RK_CAMERA_FRAME_ERROR);
		break;
	case WAIT_ERROR:
		mock_irq(RK_CAMERA_FRAME_ERROR);
		break;
	}
}

static unsigned long wait_for_completion_timeout(struct completion *completion,
						  unsigned long timeout)
{
	struct ar0521 *sensor = &g.sensor;
	enum wait_action action = WAIT_NONE;
	int which;

	UNUSED(timeout);
	if (completion == &sensor->grr_start) {
		which = 1;
		action = g.start_action;
	} else if (completion == &sensor->grr_end) {
		which = 2;
		action = g.end_action;
	} else {
		which = 0;
	}
	apply_wait_action(action);
	record_event(EV_WAIT, (u16)which, completion->done != 0, 1,
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_released), 0);
	if (!completion->done)
		return 0;
	completion->done--;
	return 1;
}

static int ar0521_write_reg(struct ar0521 *sensor, u16 reg, u16 value)
{
	UNUSED(sensor);
	if (g.fail_write_remaining > 0 && reg == g.fail_write_reg &&
	    value == g.fail_write_value) {
		g.fail_write_remaining--;
		record_event(EV_REG_WRITE, reg, value, 0,
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_released), 0);
		return -EIO;
	}

	if (reg == AR0521_REG_VD_TRIG_NEW_FRAME)
		g.vd_reg = value;
	if (reg == AR0521_REG_GLOBAL_SEQ_TRIGGER)
		g.seq_trigger = value;
	record_event(EV_REG_WRITE, reg, value, 1,
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_released), 0);

	if (reg == AR0521_REG_VD_TRIG_NEW_FRAME && value == 0 &&
	    g.inject_start_on_release_write) {
		g.inject_start_on_release_write = false;
		mock_irq(RK_CAMERA_FRAME_START);
	}
	return 0;
}

static int ar0521_read_reg(struct ar0521 *sensor, u16 reg, u16 *value)
{
	u16 actual;

	if (g.fail_read_gate_call &&
	    g.sensor.grr_gate_calls == g.fail_read_gate_call &&
	    reg == AR0521_REG_VD_TRIG_NEW_FRAME) {
		record_event(EV_REG_READ, reg, 0, 0,
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_released), 0);
		return -EIO;
	}
	if (g.fail_read_remaining > 0 && reg == g.fail_read_reg) {
		g.fail_read_remaining--;
		record_event(EV_REG_READ, reg, 0, 0,
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_phase),
			     atomic_read(&sensor->grr_released), 0);
		return -EIO;
	}

	switch (reg) {
	case AR0521_REG_GPIO_GPO_2:
		actual = g.gpio_reads ? g.gpio_after_rearm : g.gpio_before_rearm;
		if (g.sensor.grr_pwm_work.queued && !g.poll_never_ready)
			actual |= BIT(1);
		g.gpio_reads++;
		break;
	case AR0521_REG_GLOBAL_READ_START:
		actual = 0x0100;
		break;
	case AR0521_REG_GLOBAL_READ_START_HI:
		actual = 0;
		break;
	case AR0521_REG_GLOBAL_SEQ_TRIGGER:
		actual = g.seq_trigger;
		break;
	case AR0521_REG_VD_TRIG_NEW_FRAME:
		g.vd_reads++;
		actual = g.vd_reg;
		if (g.vd_reads == g.mismatch_vd_read)
			actual ^= BIT(0);
		break;
	default:
		actual = 0;
		break;
	}
	*value = actual;
	record_event(EV_REG_READ, reg, actual, 1,
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_released), 0);

	if (reg == AR0521_REG_VD_TRIG_NEW_FRAME && g.vd_reg == 0 &&
	    g.inject_start_on_release_read) {
		g.inject_start_on_release_read = false;
		mock_irq(RK_CAMERA_FRAME_START);
		if (g.inject_start_and_end_on_release_read) {
			g.inject_start_and_end_on_release_read = false;
			mock_irq(RK_CAMERA_FRAME_END);
		}
	}
	if (reg == AR0521_REG_GPIO_GPO_2 && g.gpio_reads == 2 &&
	    g.inject_start_on_prepare_read) {
		g.inject_start_on_prepare_read = false;
		mock_irq(RK_CAMERA_FRAME_START);
	}
	if (reg == AR0521_REG_VD_TRIG_NEW_FRAME && g.vd_reads == 1 &&
	    g.inject_end_on_first_vd_read) {
		g.inject_end_on_first_vd_read = false;
		mock_irq(RK_CAMERA_FRAME_END);
	}
	return 0;
}

static int ar0521_update_bits(struct ar0521 *sensor, u16 reg, u16 mask, u16 value)
{
	u16 current;
	int ret;

	ret = ar0521_read_reg(sensor, reg, &current);
	if (ret)
		return ret;
	current = (current & ~mask) | (value & mask);
	return ar0521_write_reg(sensor, reg, current);
}

static int ar0521_init_trigger_pwm(struct ar0521 *sensor)
{
	int ret = g.init_trigger_pwm_ret;

	g.init_trigger_pwm_calls++;
	record_event(EV_PWM_IDLE, 0, ret, ret == 0,
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_phase),
		     atomic_read(&sensor->grr_released), 0);
	return ret;
}
static int ar0521_stream_bit(struct ar0521 *sensor, bool on)
{
	UNUSED(sensor);
	if (on) {
		g.stream_on_calls++;
	} else {
		g.stream_off_calls++;
		record_event(EV_STREAM_OFF, 0, 0, 1,
			     atomic_read(&g.sensor.grr_phase),
			     atomic_read(&g.sensor.grr_phase),
			     atomic_read(&g.sensor.grr_released), 0);
	}
	return g.stream_bit_ret;
}

static void mock_dev_err(const char *format, ...)
{
	UNUSED(format);
}

#define dev_err(dev, format, ...) \
	do { UNUSED(dev); mock_dev_err(format, ##__VA_ARGS__); } while (0)
#define msleep(msecs) record_event(EV_SLEEP, 0, (msecs) * 1000, 1, 0, 0, 0, 0)

/* Retained workqueue/poll mocks trap accidental PWM-path reintroduction. */
static bool __attribute__((unused)) queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	UNUSED(wq);
	/* false models an already-pending work item that could not be newly queued. */
	work->queued = true;
	work->cancelled = false;
	work->completed = false;
	record_event(EV_QUEUE, 0, g.queue_result, g.queue_result,
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_released), 0);
	return g.queue_result;
}

static bool __attribute__((unused)) cancel_work_sync(struct work_struct *work)
{
	bool pending = work->queued && !work->completed;

	record_event(EV_CANCEL, 0, pending, pending,
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_released), 0);
	if (pending) {
		work->queued = false;
		work->cancelled = true;
	}
	return pending;
}

static bool __attribute__((unused)) flush_work(struct work_struct *work)
{
	bool pending = work->queued && !work->completed;

	record_event(EV_FLUSH, 0, pending, pending,
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_released), 0);
	if (pending && !work->cancelled) {
		work->func(work);
		work->queued = false;
		work->completed = true;
	}
	return pending;
}

#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before_read, ...) \
({ \
	int __poll_ret; \
	UNUSED(sleep_us); \
	UNUSED(timeout_us); \
	UNUSED(sleep_before_read); \
	__poll_ret = (op)(__VA_ARGS__); \
	g.poll_calls++; \
	if (g.poll_override) \
		__poll_ret = g.poll_override; \
	(val) = __poll_ret; \
	if (!__poll_ret && !(cond)) \
		__poll_ret = -ETIMEDOUT; \
	record_event(EV_POLL, 0, __poll_ret, __poll_ret == 0, \
		     atomic_read(&g.sensor.grr_phase), \
		     atomic_read(&g.sensor.grr_phase), \
		     atomic_read(&g.sensor.grr_released), 0); \
	__poll_ret; \
})

static void ar0521_light_source_set_locked(struct ar0521 *sensor, bool on)
{
	record_event(EV_LIGHT, 0, on, 1, 0, 0, 0, 0);
}

static void ar0521_sleep_us(u32 us)
{
	record_event(EV_SLEEP, 0, us, 1, 0, 0, 0, 0);
}

struct device_attribute { int unused; };
static struct ar0521 *ar0521_from_dev(struct device *dev)
{
	return dev == &g.dev ? &g.sensor : NULL;
}
static int kstrtouint(const char *buf, unsigned int base, unsigned int *value)
{
	char *end;
	unsigned long result = strtoul(buf, &end, base);
	if (end == buf || (*end && *end != '\n') || result > UINT32_MAX)
		return -EINVAL;
	*value = result;
	return 0;
}
#define mutex_lock(lock) UNUSED(lock)
#define mutex_unlock(lock) UNUSED(lock)
static int ar0521_trigger_once_locked(struct ar0521 *sensor);
#define pm_runtime_active(dev) (UNUSED(dev), true)

static int ar0521_exit_lp11_locked(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.exit_lp11_calls++;
	return 0;
}

/* ACTUAL_FUNCTIONS */

static int ar0521_trigger_once_locked(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.pwm_calls++;
	record_event(EV_PWM, 0, g.pwm_ret, g.pwm_ret == 0,
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_phase),
		     atomic_read(&g.sensor.grr_released), 0);
	return g.pwm_ret;
}

static void ar0521_grr_pwm_work(struct work_struct *work)
{
	struct ar0521 *sensor = &g.sensor;

	UNUSED(work);
	WRITE_ONCE(sensor->grr_pwm_call_ns, ktime_get_ns());
	sensor->grr_pwm_ret = ar0521_trigger_once_locked(sensor);
}

static void reset_mock(void)
{
	if (g.initialized)
		pthread_mutex_destroy(&g.sensor.grr_irq_lock.lock);
	memset(&g, 0, sizeof(g));
	g.initialized = true;
	g.sensor.dev = &g.dev;
	g.sensor.client = &g.client;
	g.sensor.trigger_pwm = &g.pwm;
	g.sensor.sd.priv = &g.sensor;
	g.sensor.sd.fwnode = &g.terminal_fwnode;
	g.sensor.streaming = true;
	g.sensor.active_mode = AR0521_TRIGGER_ONE_SHOT;
	g.sensor.format.height = 1944;
	g.sensor.light_source_advance_us = 0;
	spin_lock_init(&g.sensor.grr_irq_lock);
	init_completion(&g.sensor.grr_start);
	init_completion(&g.sensor.grr_end);
	atomic_set(&g.sensor.grr_phase, AR0521_GRR_IDLE);
	atomic_set(&g.sensor.grr_released, 0);
	atomic_set(&g.sensor.grr_release_requested, 0);
	g.sensor.grr_pwm_work.func = ar0521_grr_pwm_work;
	g.queue_result = true;
	g.clock_step_ns = 1000000ULL;
	g.vd_reg = AR0521_SLAVE_VD_TRIG;
	g.seq_trigger = AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR;
	g.gpio_before_rearm = 0x0080;
	g.gpio_after_rearm = 0x0080;
}

static int failures;
static int case_failures;

#define CHECK_TRUE(condition, message) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
			case_failures++; \
		} \
	} while (0)

#define CHECK_EQ(actual, expected, message) \
	do { \
		long __actual = (long)(actual); \
		long __expected = (long)(expected); \
		if (__actual != __expected) { \
			fprintf(stderr, "FAIL: %s: got %ld expected %ld (line %d)\n", \
				message, __actual, __expected, __LINE__); \
			case_failures++; \
		} \
	} while (0)

static void case_begin(void)
{
	reset_mock();
	case_failures = 0;
}

static void case_end(const char *name)
{
	if (case_failures) {
		failures += case_failures;
		printf("FAILED: %s (%d checks)\n", name, case_failures);
	} else {
		printf("PASS: %s\n", name);
	}
}

static int count_events(enum event_kind kind)
{
	int count = 0;
	int i;

	for (i = 0; i < g.event_count; i++)
		if (g.events[i].kind == kind)
			count++;
	return count;
}

static int last_event(enum event_kind kind)
{
	int i;

	for (i = g.event_count - 1; i >= 0; i--)
		if (g.events[i].kind == kind)
			return i;
	return -1;
}

static struct event *event_of_kind(enum event_kind kind, int ordinal)
{
	int i;

	for (i = 0; i < g.event_count; i++) {
		if (g.events[i].kind == kind && ordinal-- == 0)
			return &g.events[i];
	}
	return NULL;
}

static int count_reg_write(u16 value, int success_only)
{
	int count = 0;
	int i;

	for (i = 0; i < g.event_count; i++) {
		if (g.events[i].kind == EV_REG_WRITE &&
		    g.events[i].reg == AR0521_REG_VD_TRIG_NEW_FRAME &&
		    g.events[i].value == value &&
		    (!success_only || g.events[i].ok))
			count++;
	}
	return count;
}

static int last_reg_write(u16 value)
{
	int i;

	for (i = g.event_count - 1; i >= 0; i--) {
		if (g.events[i].kind == EV_REG_WRITE &&
		    g.events[i].reg == AR0521_REG_VD_TRIG_NEW_FRAME &&
		    g.events[i].value == value && g.events[i].ok)
			return i;
	}
	return -1;
}

static int count_irq(u32 status)
{
	int count = 0;
	int i;

	for (i = 0; i < g.event_count; i++)
		if (g.events[i].kind == EV_IRQ &&
		    (u32)g.events[i].value == status)
			count++;
	return count;
}

static struct event *irq_event(u32 status, int ordinal)
{
	int i;

	for (i = 0; i < g.event_count; i++) {
		if (g.events[i].kind == EV_IRQ &&
		    (u32)g.events[i].value == status && ordinal-- == 0)
			return &g.events[i];
	}
	return NULL;
}

static void check_failure_cleanup(void)
{
	int gate_last = last_reg_write(AR0521_SLAVE_VD_TRIG);
	int idle = last_event(EV_PWM_IDLE);
	int stop = last_event(EV_STREAM_OFF);

	CHECK_EQ(g.stream_off_calls, 1, "failure must stop streaming once");
	CHECK_EQ(g.init_trigger_pwm_calls, 1,
		 "failure must force PWM idle exactly once");
	CHECK_TRUE(gate_last >= 0, "failure cleanup must reclose the VD gate");
	CHECK_TRUE(idle > gate_last, "PWM idle must follow the final gate 0x8000");
	CHECK_TRUE(stop > idle, "standby must follow PWM idle");
}

static void test_normal_direct_release(void)
{
	int ret;

	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, 0, "normal trigger must succeed");
	CHECK_EQ(g.pwm_calls, 0, "direct release emits no PWM");
	CHECK_EQ(count_events(EV_FLUSH), 0, "direct release has no PWM work to join");
	CHECK_EQ(count_events(EV_CANCEL), 0, "normal trigger must not cancel work");
	CHECK_EQ(g.stream_off_calls, 0, "normal trigger must keep streaming");
	CHECK_EQ(g.init_trigger_pwm_calls, 0,
		 "normal trigger must not force PWM idle");
	CHECK_EQ(count_reg_write(0, 1), 1, "normal trigger must release gate once");
	CHECK_EQ(count_reg_write(AR0521_SLAVE_VD_TRIG, 1), 3,
		 "normal trigger needs setup, reclose, and final cleanup gates");
	CHECK_TRUE(atomic_read(&g.sensor.grr_phase) == AR0521_GRR_IDLE,
		   "normal trigger must end IDLE");
	CHECK_TRUE(!g.sensor.grr_faulted, "normal trigger must not fault");
	CHECK_TRUE(irq_event(RK_CAMERA_FRAME_START, 0) != NULL,
		   "normal trigger must observe START");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_START, 0)->released, 1,
		 "START must only be accepted after gate0 readback");
	CHECK_EQ(count_events(EV_QUEUE), 0, "direct release must not queue work");
	case_end("normal direct release");
}

static void test_reclose_follows_end(void)
{
	int i, close_index = -1, end_index = -1, close_count = 0;

	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0, "END-gated transaction succeeds");
	for (i = 0; i < g.event_count; i++) {
		if (g.events[i].kind == EV_IRQ && g.events[i].value == RK_CAMERA_FRAME_END)
			end_index = i;
		if (g.events[i].kind == EV_REG_WRITE &&
		    g.events[i].reg == AR0521_REG_VD_TRIG_NEW_FRAME &&
		    g.events[i].value == AR0521_SLAVE_VD_TRIG && ++close_count == 2)
			close_index = i;
	}
	CHECK_TRUE(end_index >= 0 && close_index > end_index,
		   "normal reclose must follow current frame END, not START");
	CHECK_TRUE(g.sensor.grr_reclose_ns >= g.sensor.grr_end_ns,
		   "reclose timestamp must not precede END");
	case_end("reclose only after END");
}

static void test_start_during_release_write(void)
{
	struct event *start;
	int ret;

	case_begin();
	g.inject_start_on_release_write = true;
	g.end_action = WAIT_END;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, 0, "START during release write must be buffered");
	CHECK_EQ(g.pwm_calls, 0, "direct release emits no PWM");
	CHECK_EQ(count_events(EV_CANCEL), 0, "buffered START must not fault");
	CHECK_TRUE(!g.sensor.grr_faulted, "buffered START must not fault the sensor");
	start = irq_event(RK_CAMERA_FRAME_START, 0);
	CHECK_TRUE(start != NULL, "release write must deliver START");
	if (start) {
		CHECK_EQ(start->phase, AR0521_GRR_WAIT_START,
			 "release write START must belong to WAIT_START");
		CHECK_EQ(start->phase_after, AR0521_GRR_WAIT_END,
			 "release write START must enter WAIT_END");
		CHECK_EQ(start->released, 0,
			 "release write START must precede released confirmation");
	}
	CHECK_TRUE(g.sensor.grr_release_request_ns > 0,
		   "release request timestamp must be recorded");
	CHECK_TRUE(g.sensor.grr_release_request_ns < g.sensor.grr_start_ns,
		   "software release request must precede the early START");
	CHECK_TRUE(g.sensor.grr_start_ns < g.sensor.grr_release_ns,
		   "release confirmation must follow the early START");
	case_end("START during release write");
}

static void test_start_during_release_read(void)
{
	struct event *start;
	int ret;

	case_begin();
	g.inject_start_on_release_read = true;
	g.end_action = WAIT_END;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, 0, "START during release readback must be buffered");
	CHECK_EQ(g.pwm_calls, 0, "direct release emits no PWM");
	CHECK_EQ(count_events(EV_CANCEL), 0, "readback START must not fault");
	CHECK_TRUE(!g.sensor.grr_faulted, "readback START must not fault the sensor");
	start = irq_event(RK_CAMERA_FRAME_START, 0);
	CHECK_TRUE(start != NULL, "release readback must deliver START");
	if (start) {
		CHECK_EQ(start->phase, AR0521_GRR_WAIT_START,
			 "release readback START must belong to WAIT_START");
		CHECK_EQ(start->phase_after, AR0521_GRR_WAIT_END,
			 "release readback START must enter WAIT_END");
		CHECK_EQ(start->released, 0,
			 "release readback START must precede released confirmation");
	}
	CHECK_TRUE(g.sensor.grr_release_request_ns > 0,
		   "readback case must record the software request");
	CHECK_TRUE(g.sensor.grr_release_request_ns < g.sensor.grr_start_ns,
		   "readback request must precede the early START");
	CHECK_TRUE(g.sensor.grr_start_ns < g.sensor.grr_release_ns,
		   "readback confirmation must follow the early START");
	case_end("START during release readback");
}

static void test_start_and_end_before_wait(void)
{
	struct event *start, *end;
	int ret;

	case_begin();
	g.inject_start_on_release_read = true;
	g.inject_start_and_end_on_release_read = true;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, 0, "START+END completed before wait_start must succeed");
	CHECK_EQ(g.pwm_calls, 0, "direct release emits no PWM");
	CHECK_EQ(count_events(EV_CANCEL), 0, "early complete frame must not fault");
	CHECK_TRUE(!g.sensor.grr_faulted, "early complete frame must stay healthy");
	start = irq_event(RK_CAMERA_FRAME_START, 0);
	end = irq_event(RK_CAMERA_FRAME_END, 0);
	CHECK_TRUE(start != NULL && end != NULL,
		   "early complete frame must deliver START and END");
	if (start && end) {
		CHECK_EQ(start->released, 0,
			 "early START must precede released confirmation");
		CHECK_EQ(end->released, 0,
			 "early END must precede released confirmation");
	}
	CHECK_TRUE(g.sensor.grr_release_request_ns < g.sensor.grr_start_ns,
		   "early frame request must precede START");
	CHECK_TRUE(g.sensor.grr_start_ns < g.sensor.grr_end_ns,
		   "early frame START must precede END");
	CHECK_TRUE(g.sensor.grr_end_ns < g.sensor.grr_release_ns,
		   "early frame END must precede release confirmation");
	if (event_of_kind(EV_WAIT, 0) && event_of_kind(EV_WAIT, 1)) {
		CHECK_EQ(event_of_kind(EV_WAIT, 0)->ok, 1,
			 "wait_start must consume the already-completed START");
		CHECK_EQ(event_of_kind(EV_WAIT, 1)->ok, 1,
			 "wait_end must consume the already-completed END");
	} else {
		CHECK_TRUE(false, "early complete frame must reach both completion waits");
	}
	case_end("START+END before caller wait");
}

static void test_start_before_release_request(void)
{
	struct event *start;
	int ret;

	case_begin();
	g.inject_start_on_prepare_read = true;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EPROTO, "START before the release request must fail closed");
	CHECK_EQ(g.pwm_calls, 0, "pre-request START must not emit PWM");
	CHECK_EQ(count_events(EV_QUEUE), 0, "pre-request START must stop before queueing");
	CHECK_EQ(atomic_read(&g.sensor.grr_release_requested), 0,
		 "pre-request START must not fabricate a request");
	CHECK_EQ(g.sensor.grr_release_request_ns, 0,
		 "pre-request START must not record a request time");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "pre-request START must leave released clear");
	start = irq_event(RK_CAMERA_FRAME_START, 0);
	CHECK_TRUE(start != NULL && start->phase_after == AR0521_GRR_ERROR,
		   "pre-request START must enter ERROR");
	CHECK_TRUE(g.sensor.grr_faulted, "pre-request START must fault");
	check_failure_cleanup();
	case_end("START before release request");
}

static void test_early_end(void)
{
	int ret;

	case_begin();
	g.inject_end_on_first_vd_read = true;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EPROTO, "END before release must fail");
	CHECK_EQ(count_irq(RK_CAMERA_FRAME_END), 1, "early END must reach IRQ once");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_END, 0)->released, 0,
		 "early END must see released=0");
	CHECK_EQ(count_events(EV_QUEUE), 0, "early END must not queue work");
	CHECK_TRUE(g.sensor.grr_faulted, "early END must fault");
	check_failure_cleanup();
	case_end("early END");
}

static void test_repeated_start(void)
{
	int ret;

	case_begin();
	g.start_action = WAIT_START_THEN_START;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EPROTO, "repeated START must fail");
	CHECK_EQ(count_irq(RK_CAMERA_FRAME_START), 2,
		 "repeated START must exercise two IRQ events");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_START, 0)->phase_after,
		 AR0521_GRR_WAIT_END, "first START must enter WAIT_END");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_START, 1)->phase,
		 AR0521_GRR_WAIT_END, "second START must be unexpected");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_START, 1)->phase_after,
		 AR0521_GRR_ERROR, "second START must enter ERROR");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_TRUE(g.sensor.grr_faulted, "repeated START must fault");
	check_failure_cleanup();
	case_end("repeated START");
}

static void test_frame_error(void)
{
	int ret;

	case_begin();
	g.start_action = WAIT_START_THEN_ERROR;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EPROTO, "frame ERROR must fail");
	CHECK_EQ(count_irq(RK_CAMERA_FRAME_ERROR), 1, "ERROR event must reach IRQ");
	CHECK_EQ(irq_event(RK_CAMERA_FRAME_ERROR, 0)->phase_after,
		 AR0521_GRR_ERROR, "ERROR must enter ERROR phase");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_TRUE(g.sensor.grr_faulted, "ERROR must fault");
	check_failure_cleanup();
	case_end("frame ERROR");
}

static void test_start_timeout(void)
{
	int ret;

	case_begin();
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -ETIMEDOUT, "missing START must time out");
	CHECK_EQ(count_events(EV_WAIT), 1, "start timeout must not wait for END");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_EQ(g.pwm_calls, 0, "pending work must not run after cancellation");
	CHECK_TRUE(g.sensor.grr_faulted, "start timeout must fault");
	check_failure_cleanup();
	case_end("START timeout");
}

static void test_end_timeout(void)
{
	int ret;

	case_begin();
	g.start_action = WAIT_START;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -ETIMEDOUT, "missing END must time out");
	CHECK_EQ(count_events(EV_WAIT), 2, "end timeout must wait for START and END");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_TRUE(g.sensor.grr_faulted, "end timeout must fault");
	check_failure_cleanup();
	case_end("END timeout");
}

static void test_gate_write_failure(void)
{
	int ret;

	case_begin();
	g.fail_write_remaining = 1;
	g.fail_write_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	g.fail_write_value = AR0521_SLAVE_VD_TRIG;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "gate write failure must propagate");
	CHECK_EQ(count_reg_write(AR0521_SLAVE_VD_TRIG, 0), 2,
		 "gate write failure and cleanup write must both be observable");
	CHECK_EQ(count_reg_write(AR0521_SLAVE_VD_TRIG, 1), 1,
		 "cleanup gate write must recover after the injected failure");
	CHECK_EQ(count_events(EV_QUEUE), 0, "gate write failure must not queue work");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "gate write failure must not release START");
	CHECK_TRUE(g.sensor.grr_faulted, "gate write failure must fault");
	check_failure_cleanup();
	case_end("I2C gate write failure");
}

static void test_gate_readback_failure(void)
{
	int ret;

	case_begin();
	g.fail_read_remaining = 1;
	g.fail_read_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "gate readback failure must propagate");
	CHECK_EQ(g.vd_reads, 1, "cleanup gate read must succeed after failed readback");
	CHECK_EQ(count_events(EV_QUEUE), 0,
		 "gate readback failure must not queue work");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "gate readback failure must not release START");
	CHECK_TRUE(g.sensor.grr_faulted, "gate readback failure must fault");
	check_failure_cleanup();
	case_end("I2C gate readback failure");
}

static void test_release_write_failure(void)
{
	int ret;

	case_begin();
	g.fail_write_remaining = 1;
	g.fail_write_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	g.fail_write_value = 0;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "release write failure must propagate");
	CHECK_EQ(count_reg_write(0, 0), 1,
		 "release write failure must remain observable");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_TRUE(g.sensor.grr_release_request_ns > 0,
		   "release write failure must preserve the software request time");
	CHECK_EQ(g.sensor.grr_release_ns, 0,
		 "failed release write must not confirm release");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "failed release write must leave released clear");
	CHECK_EQ(atomic_read(&g.sensor.grr_release_requested), 0,
		 "failed release write must clear the request state");
	CHECK_TRUE(g.sensor.grr_faulted, "release write failure must fault");
	check_failure_cleanup();
	case_end("release gate write failure");
}

static void test_release_readback_failure(void)
{
	int ret;

	case_begin();
	g.fail_read_gate_call = 2;
	g.fail_read_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "release readback failure must propagate");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_TRUE(g.sensor.grr_release_request_ns > 0,
		   "release readback failure must preserve the software request time");
	CHECK_EQ(g.sensor.grr_release_ns, 0,
		 "failed release readback must not confirm release");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "failed release readback must leave released clear");
	CHECK_EQ(atomic_read(&g.sensor.grr_release_requested), 0,
		 "failed release readback must clear the request state");
	CHECK_TRUE(g.sensor.grr_faulted, "release readback failure must fault");
	check_failure_cleanup();
	case_end("release gate readback failure");
}





static void test_readback_mismatch(void)
{
	int ret;

	case_begin();
	/* Gate0 readback is the second VD read and must fail closed. */
	g.mismatch_vd_read = 2;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "readback mismatch must return EIO");
	CHECK_EQ(atomic_read(&g.sensor.grr_released), 0,
		 "readback mismatch must not set released");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_EQ(g.pwm_calls, 0, "readback mismatch must not leave runnable work");
	CHECK_TRUE(g.sensor.grr_faulted, "readback mismatch must fault");
	check_failure_cleanup();
	case_end("gate readback mismatch");
}

static void test_gate_diagnostic_write_failure(void)
{
	case_begin();
	g.fail_write_remaining = 1;
	g.fail_write_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	g.fail_write_value = AR0521_SLAVE_VD_TRIG;
	CHECK_EQ(ar0521_grr_gate(&g.sensor, AR0521_SLAVE_VD_TRIG), -EIO,
		 "gate write failure must propagate");
	CHECK_EQ(g.sensor.grr_gate_fail_call, 1, "first failed gate recorded");
	CHECK_EQ(g.sensor.grr_gate_target, AR0521_SLAVE_VD_TRIG, "target recorded");
	CHECK_EQ(g.sensor.grr_gate_write_ret, -EIO, "write errno recorded");
	CHECK_EQ(g.sensor.grr_gate_read_ret, -EINPROGRESS, "unattempted read explicit");
	CHECK_EQ(ar0521_grr_gate(&g.sensor, AR0521_SLAVE_VD_TRIG), 0, "cleanup succeeds");
	CHECK_EQ(g.sensor.grr_gate_fail_call, 1, "cleanup preserves first failure");
	CHECK_EQ(g.sensor.grr_gate_write_ret, -EIO, "cleanup preserves errno");
	case_end("gate diagnostics write failure survives cleanup");
}

static void test_gate_diagnostic_read_failure(void)
{
	case_begin();
	g.fail_read_remaining = 1;
	g.fail_read_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
	CHECK_EQ(ar0521_grr_gate(&g.sensor, 0), -EIO, "gate read failure propagates");
	CHECK_EQ(g.sensor.grr_gate_fail_call, 1, "read failure gate recorded");
	CHECK_EQ(g.sensor.grr_gate_write_ret, 0, "successful write recorded");
	CHECK_EQ(g.sensor.grr_gate_read_ret, -EIO, "read errno recorded");
	case_end("gate diagnostics read failure");
}

static void test_gate_diagnostic_reclose_mismatch(void)
{
	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	g.mismatch_vd_read = 3;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -EIO, "reclose mismatch fails");
	CHECK_EQ(g.sensor.grr_gate_fail_call, 3, "post END reclose is gate 3");
	CHECK_EQ(g.sensor.grr_gate_target, AR0521_SLAVE_VD_TRIG, "reclose target");
	CHECK_EQ(g.sensor.grr_gate_actual, AR0521_SLAVE_VD_TRIG | BIT(0), "actual retained");
	CHECK_EQ(g.sensor.grr_gate_write_ret, 0, "mismatch is not write errno");
	CHECK_EQ(g.sensor.grr_gate_read_ret, 0, "mismatch is not read errno");
	CHECK_EQ(g.sensor.grr_gate_calls, 4, "cleanup gate still executed");
	CHECK_EQ(count_events(EV_WAIT), 2, "reclose failure follows END wait");
	case_end("gate diagnostics reclose mismatch survives cleanup");
}

static void test_gate_diagnostic_transaction_reset(void)
{
	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	g.sensor.grr_gate_calls = 9;
	g.sensor.grr_gate_fail_call = 7;
	g.sensor.grr_gate_target = 123;
	g.sensor.grr_gate_actual = 124;
	g.sensor.grr_gate_write_ret = -EIO;
	g.sensor.grr_gate_read_ret = -EIO;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0, "new transaction succeeds");
	CHECK_EQ(g.sensor.grr_gate_calls, 4, "new transaction gate count reset");
	CHECK_EQ(g.sensor.grr_gate_fail_call, 0, "old failure cleared");
	CHECK_EQ(g.sensor.grr_gate_target, 0, "old target cleared");
	CHECK_EQ(g.sensor.grr_gate_actual, 0, "old actual cleared");
	CHECK_EQ(g.sensor.grr_gate_write_ret, 0, "old write error cleared");
	CHECK_EQ(g.sensor.grr_gate_read_ret, 0, "old read error cleared");
	case_end("gate diagnostics reset at new transaction");
}

static void test_reclose_timeout(void)
{
	int ret;

	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	g.clock_step_ns = 9000000ULL;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -ETIME, "reclose later than 8ms must return ETIME");
	CHECK_EQ(count_events(EV_WAIT), 2,
		 "reclose budget is measured after END");
	CHECK_EQ(count_events(EV_CANCEL), 0, "direct release has no PWM work to join");
	CHECK_EQ(g.pwm_calls, 0, "reclose timeout must not leave runnable work");
	CHECK_TRUE(g.sensor.grr_faulted, "reclose timeout must fault");
	check_failure_cleanup();
	case_end("reclose over 8ms");
}

static void test_pwm_idle_failure_preserves_error(void)
{
	int ret;

	case_begin();
	g.init_trigger_pwm_ret = -EIO;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -ETIMEDOUT,
		 "PWM-idle failure must not replace the original START timeout");
	CHECK_EQ(g.init_trigger_pwm_calls, 1,
		 "failure cleanup must attempt PWM idle once");
	CHECK_EQ(g.events[last_event(EV_PWM_IDLE)].ok, 0,
		 "forced PWM idle failure must remain observable");
	CHECK_TRUE(g.sensor.grr_faulted,
		   "PWM-idle failure must keep the sensor faulted");
	check_failure_cleanup();
	case_end("PWM idle failure preserves original error");
}
static void test_notify_other_terminal_no_state_change(void)
{
	struct rk_camera_frame_event event = {
		.terminal = &g.other_fwnode,
		.mask = RK_CAMERA_FRAME_START,
	};
	int ret;

	case_begin();
	atomic_set(&g.sensor.grr_phase, AR0521_GRR_WAIT_START);
	atomic_set(&g.sensor.grr_released, 1);
	ret = ar0521_grr_frame_notify(&g.sensor.grr_frame_notifier, 0, &event);
	CHECK_EQ(ret, NOTIFY_DONE, "foreign terminal must return NOTIFY_DONE");
	CHECK_EQ(atomic_read(&g.sensor.grr_phase), AR0521_GRR_WAIT_START,
		 "foreign terminal must not change phase");
	CHECK_EQ(g.sensor.grr_start.done, 0,
		 "foreign terminal must not complete START");
	case_end("notifier foreign terminal is inert");
}

static void test_notify_same_terminal_enters_real_irq(void)
{
	struct rk_camera_frame_event event = {
		.terminal = g.sensor.sd.fwnode,
		.mask = RK_CAMERA_FRAME_START,
	};
	int ret;

	case_begin();
	atomic_set(&g.sensor.grr_phase, AR0521_GRR_WAIT_START);
	atomic_set(&g.sensor.grr_released, 1);
	ret = ar0521_grr_frame_notify(&g.sensor.grr_frame_notifier, 0, &event);
	CHECK_EQ(ret, NOTIFY_OK, "handled terminal event must return NOTIFY_OK");
	CHECK_EQ(atomic_read(&g.sensor.grr_phase), AR0521_GRR_WAIT_END,
		 "terminal START must reach the original IRQ core");
	CHECK_EQ(g.sensor.grr_start.done, 1,
		 "terminal START must complete the original START completion");
	case_end("notifier same terminal calls original IRQ");
}

static void test_notify_idle_returns_done(void)
{
	struct rk_camera_frame_event event = {
		.terminal = g.sensor.sd.fwnode,
		.mask = RK_CAMERA_FRAME_START,
	};
	int ret;

	case_begin();
	atomic_set(&g.sensor.grr_phase, AR0521_GRR_IDLE);
	atomic_set(&g.sensor.grr_released, 1);
	ret = ar0521_grr_frame_notify(&g.sensor.grr_frame_notifier, 0, &event);
	CHECK_EQ(ret, NOTIFY_DONE, "IDLE terminal event must return NOTIFY_DONE");
	CHECK_EQ(atomic_read(&g.sensor.grr_phase), AR0521_GRR_IDLE,
		 "IDLE terminal event must stay IDLE");
	CHECK_EQ(g.sensor.grr_start.done, 0,
		 "IDLE terminal event must not complete START");
	case_end("notifier IDLE returns NOTIFY_DONE");
}

static void test_quick_stream_fault_isolation(void)
{
	int events_before, stream_off_before;

	case_begin();
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -ETIMEDOUT,
		 "real GRR START timeout establishes fault");
	CHECK_TRUE(g.sensor.grr_faulted, "GRR timeout must set faulted");
	events_before = g.event_count;
	stream_off_before = g.stream_off_calls;
	CHECK_EQ(ar0521_quick_stream(&g.sensor, true), -EIO,
		 "faulted quick-on must return EIO");
	CHECK_EQ(g.event_count, events_before,
		 "faulted quick-on must not reach hardware operations");
	CHECK_EQ(g.stream_off_calls, stream_off_before,
		 "faulted quick-on must not stop stream");
	CHECK_EQ(g.exit_lp11_calls, 0, "faulted quick-on must not exit LP11");
	stream_off_before = g.stream_off_calls;
	CHECK_EQ(ar0521_quick_stream(&g.sensor, false), 0,
		 "quick-off must remain allowed while faulted");
	CHECK_EQ(g.stream_off_calls, stream_off_before + 1,
		 "quick-off must execute the stream-off path");
	CHECK_TRUE(g.sensor.grr_faulted, "quick-off must preserve fault state");
	CHECK_EQ(g.exit_lp11_calls, 0, "quick-off must not exit LP11");
	case_end("quick_stream fault isolation");
}

static void test_quick_stream_nonfault_on(void)
{
	case_begin();
	CHECK_EQ(ar0521_quick_stream(&g.sensor, true), 0,
		 "non-fault quick-on must still succeed");
	CHECK_EQ(g.exit_lp11_calls, 1,
		 "non-fault quick-on must enter the normal LP11 path");
	CHECK_EQ(g.stream_on_calls, 1,
		 "non-fault quick-on must execute stream-on");
	CHECK_TRUE(count_events(EV_REG_WRITE) > 0,
		 "non-fault quick-on must execute normal register setup");
	CHECK_TRUE(!g.sensor.grr_faulted,
		 "non-fault quick-on must not create a fault");
	case_end("quick_stream nonfault on preserved");
}

static void test_grr_trigger_fault_gate(void)
{
	int ret;

	case_begin();
	g.sensor.grr_faulted = true;
	ret = ar0521_grr_trigger_locked(&g.sensor);
	CHECK_EQ(ret, -EIO, "faulted sensor must reject at grr_trigger entry");
	CHECK_EQ(g.event_count, 0, "grr_trigger fault gate must precede I2C and work");
	CHECK_EQ(g.stream_off_calls, 0, "fault gate must not reopen cleanup path");
	case_end("grr_trigger fault gate");
}

static void test_direct_release_no_long_rearm_sleeps(void)
{
	int i, clear = -1, set = -1, release = -1, long_sleeps = 0;

	case_begin();
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0,
		 "direct release with no long rearm sleeps succeeds");
	for (i = 0; i < g.event_count; i++) {
		if (g.events[i].kind == EV_SLEEP &&
		    (g.events[i].value == 100000 || g.events[i].value == 200000))
			long_sleeps++;
		if (g.events[i].kind == EV_REG_WRITE &&
		    g.events[i].reg == AR0521_REG_GLOBAL_SEQ_TRIGGER) {
			if (!(g.events[i].value & AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR) &&
			    clear < 0)
				clear = i;
			if ((g.events[i].value & AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR) &&
			    clear >= 0 && set < 0)
				set = i;
		}
		if (g.events[i].kind == EV_REG_WRITE &&
		    g.events[i].reg == AR0521_REG_VD_TRIG_NEW_FRAME &&
		    g.events[i].value == 0)
			release = i;
	}
	CHECK_EQ(long_sleeps, 0, "actual trigger path must not sleep 100ms or 200ms");
	CHECK_TRUE(clear >= 0, "actual trigger path must clear 315E bit0");
	CHECK_TRUE(set > clear, "actual trigger path must set 315E bit0 after clear");
	CHECK_TRUE(release > set, "3158 release must follow 315E clear then set");
	CHECK_EQ(count_reg_write(0, 1), 1, "direct release must write 3158=0 once");
	case_end("direct release no long rearm sleeps");
}

static void test_direct_release_no_pwm_or_poll(void)
{
	case_begin();
	g.poll_never_ready = true;
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0, "release must not require SHUTTER high");
	CHECK_EQ(g.poll_calls, 0, "direct release must not poll SHUTTER");
	CHECK_EQ(count_events(EV_QUEUE), 0, "direct release must not enqueue PWM");
	CHECK_EQ(g.pwm_calls, 0, "direct release must emit zero PWM");
	CHECK_EQ(g.sensor.grr_pwm_call_ns, 0, "PWM diagnostic remains zero");
	case_end("direct release zero PWM and no SHUTTER poll");
}

static void test_direct_early_frame_no_pwm(void)
{
	case_begin();
	g.inject_start_on_release_read = true;
	g.inject_start_and_end_on_release_read = true;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0, "complete frame before readback return is legal");
	CHECK_EQ(g.pwm_calls, 0, "early frame must not emit PWM");
	CHECK_EQ(g.sensor.grr_pwm_call_ns, 0, "early frame PWM timestamp is zero");
	CHECK_TRUE(g.sensor.grr_start_ns < g.sensor.grr_release_ns, "START precedes release readback return");
	CHECK_TRUE(g.sensor.grr_end_ns < g.sensor.grr_release_ns, "END precedes release readback return");
	case_end("direct short frame before gate readback");
}

static void test_direct_light_exits(void)
{
	int mode, i, on, advance, release, off_delay, off;
	for (mode = 0; mode < 4; mode++) {
		case_begin();
		g.sensor.light_source_enabled = true;
		g.sensor.light_source_gpio = &g.dev;
		g.sensor.light_source_advance_us = 321;
		g.sensor.light_source_off_delay_us = 654;
		if (mode != 1) { g.start_action = WAIT_START; g.end_action = WAIT_END; }
		if (mode >= 2) {
			g.fail_write_remaining = 1;
			g.fail_write_reg = AR0521_REG_VD_TRIG_NEW_FRAME;
			g.fail_write_value = mode == 2 ? 0 : AR0521_SLAVE_VD_TRIG;
		}
		CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), mode == 0 ? 0 : (mode == 1 ? -ETIMEDOUT : -EIO), "light path result");
		on = advance = release = off_delay = off = -1;
		for (i = 0; i < g.event_count; i++) {
			if (g.events[i].kind == EV_LIGHT && g.events[i].value) on = i;
			if (g.events[i].kind == EV_LIGHT && !g.events[i].value) off = i;
			if (g.events[i].kind == EV_SLEEP && g.events[i].value == 321) advance = i;
			if (g.events[i].kind == EV_SLEEP && g.events[i].value == 654) off_delay = i;
			if (g.events[i].kind == EV_REG_WRITE && g.events[i].reg == AR0521_REG_VD_TRIG_NEW_FRAME && !g.events[i].value) release = i;
		}
		if (mode < 3) CHECK_TRUE(on >= 0 && advance > on && release > advance, "light on then advance before release");
		CHECK_TRUE(off_delay > last_reg_write(AR0521_SLAVE_VD_TRIG) && off > off_delay, "off-delay and light-off follow cleanup on every exit");
		CHECK_EQ(g.pwm_calls, 0, "lighting must not emit PWM");
		case_end(mode == 0 ? "light normal exit" : mode == 1 ? "light timeout exit" : mode == 2 ? "light release-error exit" : "light setup-error exit");
	}
}

static void test_trigger_store_routes(void)
{
	int rearm;
	for (rearm = 0; rearm <= 1; rearm++) {
		case_begin();
		g.sensor.grr_rearm = rearm;
		g.start_action = WAIT_START;
		g.end_action = WAIT_END;
		CHECK_EQ(trigger_store(&g.dev, NULL, "1", 1), 1, "actual trigger_store succeeds");
		CHECK_EQ(g.pwm_calls, rearm ? 0 : 1, "legacy PWM vs direct release routing");
		CHECK_EQ(count_reg_write(0, 1), rearm ? 1 : 0, "only GRR releases the gate");
		g.sensor.grr_faulted = true;
		CHECK_EQ(trigger_store(&g.dev, NULL, "1", 1), -EIO, "fault blocks both routes");
		CHECK_EQ(g.pwm_calls, rearm ? 0 : 1, "fault does not emit PWM");
		case_end(rearm ? "store direct route" : "store legacy PWM route");
	}
}

static void test_direct_without_pwm_or_light(void)
{
	case_begin();
	g.sensor.trigger_pwm = NULL;
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0, "direct path needs no PWM device");
	CHECK_EQ(count_events(EV_LIGHT), 0, "disabled lighting stays untouched");
	CHECK_EQ(g.pwm_calls, 0, "no PWM emitted");
	case_end("direct without PWM or light");
}

static void test_direct_formal_gpio_profile(void)
{
	case_begin();
	g.gpio_before_rearm = g.gpio_after_rearm = 0xFD0C;
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), 0,
		 "formal constant-low GPIO1 profile must allow direct release");
	CHECK_EQ(count_reg_write(0, 1), 1, "formal profile releases gate once");
	CHECK_EQ(count_irq(RK_CAMERA_FRAME_START), 1, "formal profile requires START");
	CHECK_EQ(count_irq(RK_CAMERA_FRAME_END), 1, "formal profile requires END");
	CHECK_EQ(g.pwm_calls, 0, "formal profile must not emit PWM");
	CHECK_TRUE(!g.sensor.grr_faulted, "formal profile success must not fault");
	case_end("direct formal constant-low GPIO profile");

	case_begin();
	g.gpio_before_rearm = g.gpio_after_rearm = 0xFD0C;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -ETIMEDOUT,
		 "constant-low output is not evidence of a completed frame");
	check_failure_cleanup();
	case_end("formal GPIO low still requires frame IRQ");
}

static void test_direct_gpio_guard(void)
{
	int selector;

	for (selector = 0; selector < 8; selector++) {
		if (selector == 1 || selector == 2)
			continue;
		case_begin();
		g.gpio_before_rearm = selector << 7;
		CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -EBUSY,
			 "unqualified GPIO profile must fail before writes");
		CHECK_EQ(count_events(EV_REG_WRITE), 0, "rejected profile must not write");
		case_end("unqualified GPIO profile");
	}
	for (selector = 1; selector <= 2; selector++) {
		case_begin();
		g.gpio_before_rearm = (selector << 7) | BIT(1);
		CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -EBUSY,
			 "high GPIO output must fail before writes");
		CHECK_EQ(count_events(EV_REG_WRITE), 0, "high output must not write");
		case_end("high GPIO output rejected");
	}
	case_begin();
	g.gpio_after_rearm = 0x0100;
	g.start_action = WAIT_START;
	g.end_action = WAIT_END;
	CHECK_EQ(ar0521_grr_trigger_locked(&g.sensor), -EPROTO,
		 "GPIO mux must not change during prepare");
	CHECK_EQ(count_reg_write(0, 1), 0, "changed mux must not release gate");
	check_failure_cleanup();
	case_end("GPIO mux changed during prepare");
}

int main(void)
{
	test_direct_formal_gpio_profile();
	test_direct_gpio_guard();
	test_trigger_store_routes();
	test_direct_without_pwm_or_light();
	test_direct_release_no_long_rearm_sleeps();
	test_direct_release_no_pwm_or_poll();
	test_direct_early_frame_no_pwm();
	test_direct_light_exits();
	test_normal_direct_release();
	test_reclose_follows_end();
	test_start_during_release_write();
	test_start_during_release_read();
	test_start_and_end_before_wait();
	test_start_before_release_request();
	test_early_end();
	test_repeated_start();
	test_frame_error();
	test_start_timeout();
	test_end_timeout();
	test_gate_write_failure();
	test_gate_readback_failure();
	test_release_write_failure();
	test_release_readback_failure();
	test_readback_mismatch();
	test_gate_diagnostic_write_failure();
	test_gate_diagnostic_read_failure();
	test_gate_diagnostic_reclose_mismatch();
	test_gate_diagnostic_transaction_reset();
	test_reclose_timeout();
	test_pwm_idle_failure_preserves_error();
	test_notify_other_terminal_no_state_change();
	test_notify_same_terminal_enters_real_irq();
	test_notify_idle_returns_done();
	test_grr_trigger_fault_gate();
	test_quick_stream_fault_isolation();
	test_quick_stream_nonfault_on();

	if (failures) {
		printf("RESULT: FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("RESULT: PASS (49 state-machine cases)\n");
	return 0;
}
