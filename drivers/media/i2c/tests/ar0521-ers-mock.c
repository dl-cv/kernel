/* Host harness for the real AR0521 ERS single-shot state machine. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n) (1UL << (n))
#define UNUSED(x) ((void)(x))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define READ_ONCE(value) (*(volatile __typeof__(value) *)&(value))
#define WRITE_ONCE(value, new_value) \
	do { *(volatile __typeof__(value) *)&(value) = (new_value); } while (0)
#define DIV_ROUND_UP_ULL(a, b) (((u64)(a) + (u64)(b) - 1ULL) / (u64)(b))
#define DIV_ROUND_CLOSEST_ULL(a, b) (((u64)(a) + (u64)(b) / 2) / (u64)(b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define clamp_t(type, value, low, high) \
	((type)(value) < (type)(low) ? (type)(low) : \
	 ((type)(value) > (type)(high) ? (type)(high) : (type)(value)))
#define clamp_val(value, low, high) clamp_t(int, value, low, high)

#define AR0521_PIXEL_RATE 410400000ULL
#define AR0521_CIT_MARGIN 4U
#define AR0521_HEIGHT_BLANKING_MIN 38
#define AR0521_TRIGGER_EXPOSURE_US_MAX 2000000U
#define AR0521_TOTAL_HEIGHT_MAX 65535U
#define AR0521_REG_RESET 0x301A
#define AR0521_REG_RESET_STREAM BIT(2)
#define AR0521_REG_FRAME_STATUS 0x303C
#define AR0521_REG_FRAME_STATUS_STANDBY BIT(1)
#define AR0521_REG_FRAME_LENGTH_LINES 0x0340
#define AR0521_REG_LINE_LENGTH_PCK 0x0342
#define AR0521_REG_COARSE_INTEGRATION_TIME 0x3012
#define AR0521_REG_VD_TRIG_NEW_FRAME 0x3158
#define AR0521_REG_GLOBAL_SEQ_TRIGGER 0x315E
#define AR0521_REG_GLOBAL_RST_END 0x3160
#define AR0521_REG_GLOBAL_READ_START 0x3166
#define RK_CAMERA_FRAME_START BIT(0)
#define RK_CAMERA_FRAME_END BIT(1)
#define RK_CAMERA_FRAME_ERROR BIT(2)

enum ar0521_op_mode {
	AR0521_FREE_RUN,
	AR0521_TRIGGER_ONE_SHOT,
};

enum ar0521_ers_phase {
	AR0521_ERS_IDLE,
	AR0521_ERS_WAIT_FS,
	AR0521_ERS_WAIT_FE,
	AR0521_ERS_WAIT_STANDBY,
	AR0521_ERS_CLOSING,
};

struct device { int marker; };
struct i2c_client { int marker; };
struct v4l2_ctrl { int val; };
struct v4l2_subdev { void *priv; };
struct workqueue_struct { int marker; };
struct work_struct {
	void (*func)(struct work_struct *work);
	bool queued;
	bool completed;
};
struct completion { unsigned int done; };
struct mutex { int marker; };
typedef struct { int counter; } atomic_t;
typedef struct { pthread_mutex_t lock; } spinlock_t;

#define atomic_read(a) ((a)->counter)
#define atomic_set(a, v) ((a)->counter = (v))
#define spin_lock_init(s) pthread_mutex_init(&(s)->lock, NULL)
#define spin_lock_irqsave(s, flags) \
	do { (flags) = 0; pthread_mutex_lock(&(s)->lock); } while (0)
#define spin_unlock_irqrestore(s, flags) \
	do { UNUSED(flags); pthread_mutex_unlock(&(s)->lock); } while (0)
#define lockdep_assert_held(lock) UNUSED(lock)
#define mutex_lock(m) UNUSED(m)
#define mutex_unlock(m) UNUSED(m)
#define __v4l2_ctrl_grab(c, v) do { UNUSED(c); UNUSED(v); } while (0)

struct ar0521_test_format {
	u32 height;
};

struct ar0521 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct mutex mutex;
	struct workqueue_struct *grr_reclose_wq;
	struct work_struct ers_stream_off_work;
	spinlock_t ers_irq_lock;
	atomic_t ers_phase;
	struct completion ers_fs;
	struct completion ers_fe;
	struct completion ers_off;
	bool ers_protocol_error;
	int ers_off_ret;
	int ers_cleanup_ret;
	int ers_last_ret;
	u32 ers_fs_count;
	u32 ers_fe_count;
	u64 ers_stream_on_ns;
	u64 ers_first_fs_ns;
	u64 ers_off_request_ns;
	u64 ers_first_fe_ns;
	u64 ers_standby_ns;
	struct ar0521_test_format format;
	struct v4l2_subdev sd;
	bool streaming;
	bool grr_rearm;
	bool grr_faulted;
	bool slave_backup_valid;
	bool light_source_enabled;
	void *light_source_gpio;
	u32 light_source_advance_us;
	u32 light_source_off_delay_us;
	enum ar0521_op_mode active_mode;
};

#define to_ar0521(sd) ((struct ar0521 *)(sd)->priv)

struct mock_state {
	struct ar0521 sensor;
	struct workqueue_struct wq;
	struct completion *active_wait;
	u16 regs[65536];
	bool standby;
	bool stream_on;
	bool standby_on_off;
	bool fail_first_off;
	bool fail_all_off;
	bool inject_fs_on_empty_flush;
	bool queue_without_lock;
	int fs_inject_count;
	int fe_inject_count;
	int fe_injected;
	int stream_on_calls;
	int stream_off_calls;
	int enter_slave_calls;
	int exit_slave_calls;
	int exit_lp11_calls;
	int stream_on_bits_calls;
	int light_on_calls;
	int light_off_calls;
	u64 light_on_ns;
	u64 light_off_ns;
	int exposure_lines;
	int range_min;
	int range_max;
	int range_def;
	u64 clock_ns;
	int write_count;
	u16 writes[128];
};

static struct mock_state g;

static void record_write(u16 reg)
{
	if (g.write_count < (int)(sizeof(g.writes) / sizeof(g.writes[0])))
		g.writes[g.write_count++] = reg;
}

static u64 ktime_get_ns(void)
{
	g.clock_ns += 1000000ULL;
	return g.clock_ns;
}

static unsigned long usecs_to_jiffies(u64 usecs)
{
	return (unsigned long)usecs;
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

static int ar0521_ers_frame_irq(struct v4l2_subdev *sd, u32 status,
				bool *handled);

static void mock_irq(u32 status)
{
	struct ar0521 *sensor = &g.sensor;
	bool handled = false;

	ar0521_ers_frame_irq(&sensor->sd, status, &handled);
}

static int ar0521_read_reg(struct ar0521 *sensor, u16 reg, u16 *value)
{
	UNUSED(sensor);
	if (reg == AR0521_REG_FRAME_STATUS)
		*value = g.standby ? AR0521_REG_FRAME_STATUS_STANDBY : 1;
	else
		*value = g.regs[reg];
	return 0;
}

static int ar0521_write_reg(struct ar0521 *sensor, u16 reg, u16 value)
{
	UNUSED(sensor);
	record_write(reg);
	g.regs[reg] = value;
	return 0;
}

static int ar0521_stream_bit(struct ar0521 *sensor, bool on)
{
	int i;

	UNUSED(sensor);
	if (on) {
		g.stream_on_calls++;
		g.stream_on = true;
		g.standby = false;
		for (i = 0; i < g.fs_inject_count; i++)
			mock_irq(RK_CAMERA_FRAME_START);
		return 0;
	}

	g.stream_off_calls++;
	g.stream_on = false;
	if (g.fail_all_off || (g.stream_off_calls == 1 && g.fail_first_off))
		return -EIO;
	if (!g.fe_injected) {
		for (i = 0; i < g.fe_inject_count; i++) {
			mock_irq(RK_CAMERA_FRAME_END);
			g.fe_injected++;
		}
	}
	if (g.standby_on_off)
		g.standby = true;
	return 0;
}

static int ar0521_wait_standby_locked(struct ar0521 *sensor);

static bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	UNUSED(wq);
	if (pthread_mutex_trylock(&g.sensor.ers_irq_lock.lock) == 0) {
		g.queue_without_lock = true;
		pthread_mutex_unlock(&g.sensor.ers_irq_lock.lock);
	}
	if (work->queued && !work->completed)
		return false;
	work->queued = true;
	work->completed = false;
	return true;
}

static void run_pending_work(void)
{
	if (g.sensor.ers_stream_off_work.queued &&
	    !g.sensor.ers_stream_off_work.completed) {
		g.sensor.ers_stream_off_work.func(
			&g.sensor.ers_stream_off_work);
		g.sensor.ers_stream_off_work.queued = false;
		g.sensor.ers_stream_off_work.completed = true;
	}
}

static bool flush_work(struct work_struct *work)
{
	bool pending = work->queued && !work->completed;

	run_pending_work();
	if (!pending && g.inject_fs_on_empty_flush) {
		g.inject_fs_on_empty_flush = false;
		mock_irq(RK_CAMERA_FRAME_START);
	}
	return pending;
}

static unsigned long wait_for_completion_timeout(struct completion *completion,
						 unsigned long timeout)
{
	UNUSED(timeout);
	g.active_wait = completion;
	if (completion == &g.sensor.ers_off)
		run_pending_work();
	g.active_wait = NULL;
	if (!completion->done)
		return 0;
	completion->done--;
	return 1;
}

static void usleep_range(u32 low, u32 high)
{
	UNUSED(high);
	g.clock_ns += (u64)low * 1000ULL;
}

static void ar0521_sleep_us(u32 us)
{
	g.clock_ns += (u64)us * 1000ULL;
}

static void ar0521_light_source_set_locked(struct ar0521 *sensor, bool on)
{
	UNUSED(sensor);
	if (on) {
		g.light_on_calls++;
		g.light_on_ns = ktime_get_ns();
	} else {
		g.light_off_calls++;
		g.light_off_ns = ktime_get_ns();
	}
}

static int ar0521_enter_slave_locked(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.enter_slave_calls++;
	return 0;
}

static int ar0521_exit_slave_locked(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.exit_slave_calls++;
	return 0;
}

static int ar0521_exit_lp11_locked(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.exit_lp11_calls++;
	return 0;
}

static int ar0521_stream_on_bits(struct ar0521 *sensor)
{
	UNUSED(sensor);
	g.stream_on_bits_calls++;
	return 0;
}

static u32 ar0521_exposure_us_to_lines_locked(struct ar0521 *sensor,
					      u32 exposure_us,
					      u32 frame_lines)
{
	u32 lines = g.exposure_lines ? (u32)g.exposure_lines : exposure_us;
	u32 maximum = frame_lines > AR0521_CIT_MARGIN ?
		      frame_lines - AR0521_CIT_MARGIN : 1;

	return clamp_t(u32, lines, 1, maximum);
}

static u32 ar0521_lines_to_exposure_us(const struct ar0521 *sensor, u32 lines)
{
	UNUSED(sensor);
	return lines * 10;
}

static u32 ar0521_grr_min_exposure_us(void)
{
	return 777;
}

static u64 ar0521_line_period_ns(const struct ar0521 *sensor)
{
	UNUSED(sensor);
	return 10000000ULL;
}

static void __v4l2_ctrl_modify_range(struct v4l2_ctrl *ctrl, int min, int max,
				     int step, int def)
{
	UNUSED(ctrl);
	UNUSED(step);
	g.range_min = min;
	g.range_max = max;
	g.range_def = def;
}

/* ACTUAL_FUNCTIONS */

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

static void reset_mock(void)
{
	if (g.sensor.ers_irq_lock.lock.__data.__lock)
		pthread_mutex_destroy(&g.sensor.ers_irq_lock.lock);
	memset(&g, 0, sizeof(g));
	g.sensor.dev = (struct device *)&g;
	g.sensor.client = (struct i2c_client *)&g;
	g.sensor.sd.priv = &g.sensor;
	g.sensor.streaming = true;
	g.sensor.active_mode = AR0521_TRIGGER_ONE_SHOT;
	g.sensor.grr_rearm = false;
	g.sensor.format.height = 100;
	g.sensor.exposure = (struct v4l2_ctrl *)&g.regs[0];
	g.sensor.vblank = (struct v4l2_ctrl *)&g.regs[2];
	g.sensor.vflip = (struct v4l2_ctrl *)&g.regs[4];
	g.sensor.hflip = (struct v4l2_ctrl *)&g.regs[6];
	g.sensor.exposure->val = 20000;
	g.sensor.vblank->val = 20;
	g.sensor.grr_reclose_wq = &g.wq;
	g.sensor.ers_stream_off_work.func = ar0521_ers_stream_off_work;
	spin_lock_init(&g.sensor.ers_irq_lock);
	init_completion(&g.sensor.ers_fs);
	init_completion(&g.sensor.ers_fe);
	init_completion(&g.sensor.ers_off);
	atomic_set(&g.sensor.ers_phase, AR0521_ERS_IDLE);
	g.standby = true;
	g.regs[AR0521_REG_FRAME_LENGTH_LINES] = 4324;
	g.regs[AR0521_REG_LINE_LENGTH_PCK] = 3164;
	g.regs[AR0521_REG_COARSE_INTEGRATION_TIME] = 2594;
	g.regs[AR0521_REG_RESET] = 0;
}

static int count_write(u16 reg)
{
	int i, count = 0;

	for (i = 0; i < g.write_count; i++)
		if (g.writes[i] == reg)
			count++;
	return count;
}

static int test_normal_single_shot(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == 0);
	CHECK(g.stream_on_calls == 1 && g.stream_off_calls == 1);
	CHECK(g.sensor.ers_fs_count == 1 && g.sensor.ers_fe_count == 1);
	CHECK(atomic_read(&g.sensor.ers_phase) == AR0521_ERS_IDLE);
	CHECK(g.sensor.ers_stream_on_ns < g.sensor.ers_first_fs_ns);
	CHECK(g.sensor.ers_first_fs_ns < g.sensor.ers_off_request_ns);
	CHECK(g.sensor.ers_off_request_ns < g.sensor.ers_first_fe_ns);
	CHECK(g.sensor.ers_first_fe_ns < g.sensor.ers_standby_ns);
	CHECK(count_write(AR0521_REG_VD_TRIG_NEW_FRAME) == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_SEQ_TRIGGER) == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_RST_END) == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_READ_START) == 0);
	CHECK(!g.sensor.grr_faulted);
	CHECK(!g.queue_without_lock);
	return 0;
}

static int test_requires_standby(void)
{
	int ret;

	reset_mock();
	g.standby = false;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EBUSY);
	CHECK(g.stream_on_calls == 0);
	return 0;
}

static int test_extra_fs_faults(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 2;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EPROTO);
	CHECK(g.sensor.ers_fs_count == 2);
	CHECK(g.sensor.grr_faulted);
	CHECK(g.stream_on_calls == 1);
	return 0;
}

static int test_extra_fe_faults(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 2;
	g.standby_on_off = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EPROTO);
	CHECK(g.sensor.ers_fe_count == 2);
	CHECK(g.sensor.grr_faulted);
	CHECK(g.stream_on_calls == 1);
	return 0;
}

static int test_fs_timeout(void)
{
	int ret;

	reset_mock();
	g.fe_inject_count = 0;
	g.sensor.light_source_enabled = true;
	g.sensor.light_source_gpio = &g;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -ETIMEDOUT);
	CHECK(g.stream_on_calls == 1 && g.stream_off_calls == 1);
	CHECK(g.light_on_calls == 1 && g.light_off_calls == 1);
	CHECK(g.sensor.grr_faulted);
	return 0;
}

static int test_fe_timeout(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 0;
	g.standby_on_off = false;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -ETIMEDOUT);
	CHECK(g.sensor.grr_faulted);
	return 0;
}

static int test_standby_timeout(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = false;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -ETIMEDOUT);
	CHECK(g.sensor.grr_faulted);
	return 0;
}

static int test_worker_failure(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	g.fail_first_off = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EIO);
	CHECK(g.stream_off_calls == 2);
	CHECK(g.sensor.grr_faulted);
	return 0;
}

static int test_closing_extra_fs_faults(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	g.inject_fs_on_empty_flush = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EPROTO);
	CHECK(g.sensor.grr_faulted);
	CHECK(g.stream_on_calls == 1);
	return 0;
}

static int test_cleanup_error_is_preserved(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	g.fail_all_off = true;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == -EIO);
	CHECK(g.sensor.ers_cleanup_ret == -EIO);
	CHECK(g.sensor.grr_faulted);
	return 0;
}

static int test_light_source_timing(void)
{
	int ret;

	reset_mock();
	g.fs_inject_count = 1;
	g.fe_inject_count = 1;
	g.standby_on_off = true;
	g.sensor.light_source_enabled = true;
	g.sensor.light_source_gpio = &g;
	g.sensor.light_source_advance_us = 100;
	g.sensor.light_source_off_delay_us = 200;
	ret = ar0521_ers_trigger_locked(&g.sensor);
	CHECK(ret == 0);
	CHECK(g.light_on_calls == 1 && g.light_off_calls == 1);
	CHECK(g.light_on_ns < g.sensor.ers_stream_on_ns);
	CHECK(g.light_off_ns > g.sensor.ers_standby_ns);
	return 0;
}

static int test_mode_regs_keep_ers_out_of_slave(void)
{
	reset_mock();
	g.sensor.grr_rearm = false;
	CHECK(ar0521_apply_mode_regs_locked(&g.sensor,
					    AR0521_TRIGGER_ONE_SHOT) == 0);
	CHECK(g.enter_slave_calls == 0 && g.exit_slave_calls == 0);
	CHECK(count_write(AR0521_REG_VD_TRIG_NEW_FRAME) == 1);
	CHECK(g.regs[AR0521_REG_VD_TRIG_NEW_FRAME] == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_SEQ_TRIGGER) == 1);
	CHECK(g.regs[AR0521_REG_GLOBAL_SEQ_TRIGGER] == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_RST_END) == 0);
	CHECK(count_write(AR0521_REG_GLOBAL_READ_START) == 0);
	reset_mock();
	g.sensor.grr_rearm = true;
	CHECK(ar0521_apply_mode_regs_locked(&g.sensor,
					    AR0521_TRIGGER_ONE_SHOT) == 0);
	CHECK(g.enter_slave_calls == 1);
	return 0;
}

static int test_free_run_ignores_grr_shutter_preference(void)
{
	int ret;

	reset_mock();
	g.sensor.grr_rearm = true;
	g.exposure_lines = 200;
	ret = ar0521_apply_exposure_locked(&g.sensor,
					   AR0521_FREE_RUN, 2000000);
	CHECK(ret == 0);
	CHECK(g.regs[AR0521_REG_FRAME_LENGTH_LINES] == 204);
	CHECK(g.regs[AR0521_REG_COARSE_INTEGRATION_TIME] == 200);
	return 0;
}

static int test_mode_stream_keeps_ers_in_standby(void)
{
	reset_mock();
	g.sensor.grr_rearm = false;
	CHECK(ar0521_prepare_mode_stream_locked(&g.sensor,
						AR0521_TRIGGER_ONE_SHOT) == 0);
	CHECK(g.exit_lp11_calls == 1 && g.stream_on_calls == 0);
	reset_mock();
	g.sensor.grr_rearm = true;
	CHECK(ar0521_prepare_mode_stream_locked(&g.sensor,
						AR0521_TRIGGER_ONE_SHOT) == 0);
	CHECK(g.exit_lp11_calls == 1 && g.stream_on_calls == 1);
	return 0;
}

static int test_ers_exposure_grows_fll(void)
{
	int ret;

	reset_mock();
	g.sensor.grr_rearm = false;
	g.exposure_lines = 200;
	ret = ar0521_apply_exposure_locked(&g.sensor,
					   AR0521_TRIGGER_ONE_SHOT, 2000000);
	CHECK(ret == 0);
	CHECK(g.regs[AR0521_REG_FRAME_LENGTH_LINES] == 204);
	CHECK(g.regs[AR0521_REG_COARSE_INTEGRATION_TIME] == 200);
	reset_mock();
	g.sensor.grr_rearm = true;
	g.exposure_lines = 200;
	ret = ar0521_apply_exposure_locked(&g.sensor,
					   AR0521_TRIGGER_ONE_SHOT, 2000000);
	CHECK(ret == 0);
	CHECK(count_write(AR0521_REG_FRAME_LENGTH_LINES) == 0);
	CHECK(g.regs[AR0521_REG_COARSE_INTEGRATION_TIME] == 116);
	return 0;
}

static int test_ers_exposure_range(void)
{
	reset_mock();
	g.sensor.grr_rearm = false;
	ar0521_update_exposure_range_locked(&g.sensor,
					    AR0521_TRIGGER_ONE_SHOT,
					    g.sensor.vblank->val);
	CHECK(g.range_min == 10);
	CHECK(g.range_max == (AR0521_TOTAL_HEIGHT_MAX - AR0521_CIT_MARGIN) * 10);
	reset_mock();
	g.sensor.grr_rearm = true;
	ar0521_update_exposure_range_locked(&g.sensor,
					    AR0521_TRIGGER_ONE_SHOT,
					    g.sensor.vblank->val);
	CHECK(g.range_min == 777);
	CHECK(g.range_max == AR0521_TRIGGER_EXPOSURE_US_MAX);
	reset_mock();
	g.sensor.grr_rearm = true;
	ar0521_update_exposure_range_locked(&g.sensor, AR0521_FREE_RUN,
					    g.sensor.vblank->val);
	CHECK(g.range_min == 10);
	CHECK(g.range_max == 1160);
	return 0;
}

int main(void)
{
	int failures = 0;

	failures += test_normal_single_shot();
	failures += test_requires_standby();
	failures += test_extra_fs_faults();
	failures += test_extra_fe_faults();
	failures += test_fs_timeout();
	failures += test_fe_timeout();
	failures += test_standby_timeout();
	failures += test_worker_failure();
	failures += test_cleanup_error_is_preserved();
	failures += test_closing_extra_fs_faults();
	failures += test_light_source_timing();
	failures += test_mode_regs_keep_ers_out_of_slave();
	failures += test_free_run_ignores_grr_shutter_preference();
	failures += test_mode_stream_keeps_ers_in_standby();
	failures += test_ers_exposure_grows_fll();
	failures += test_ers_exposure_range();

	puts(failures ? "RESULT: FAIL" : "RESULT: PASS (16 ERS cases)");
	return failures != 0;
}
