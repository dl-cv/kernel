/* Real mode_switch/s_stream/register sequencing; no hardware or scheduler model. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define max_t(t, a, b) max((t)(a), (t)(b))
#define DIV_ROUND_UP_ULL(a, b) (((u64)(a) + (b) - 1) / (b))
#define AR0521_PIXEL_RATE 410400000ULL
#define AR0521_REG_RESET 0x301A
#define AR0521_REG_RESET_DEFAULTS 0x0218
#define AR0521_REG_RESET_GPI BIT(8)
#define AR0521_REG_RESET_STREAM BIT(2)
#define AR0521_REG_FRAME_STATUS 0x303C
#define AR0521_REG_FRAME_STATUS_STANDBY BIT(1)
#define AR0521_REG_FRAME_LENGTH_LINES 0x0340
#define AR0521_REG_LINE_LENGTH_PCK 0x0342
#define AR0521_REG_COARSE_INTEGRATION_TIME 0x3012
#define AR0521_REG_GPIO_GPO_1 0x30F8
#define AR0521_REG_GPIO_CTRL2 0x3026
#define AR0521_REG_VD_TRIG_NEW_FRAME 0x3158
#define AR0521_REG_GLOBAL_SEQ_TRIGGER 0x315E
#define AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR BIT(0)
#define AR0521_REG_GLOBAL_SEQ_TRIGGER_BIT1 BIT(1)
#define AR0521_REG_GLOBAL_SEQ_TRIGGER_SCALE_MASK (BIT(4) | BIT(5))
#define AR0521_REG_GLOBAL_RST_END 0x3160
#define AR0521_REG_GLOBAL_READ_START 0x3166
#define AR0521_REG_GLOBAL_READ_START_HI 0x3168
#define AR0521_REG_GROUPED_PARAMETER_HOLD 0x0104
#define AR0521_REG_HISPI_TEST_MODE 0x3066
#define AR0521_REG_HISPI_TIMING 0x31BC
#define AR0521_REG_HISPI_TIMING_CONT_TX_CLK BIT(15)
#define AR0521_REG_HISPI_CONTROL_STATUS 0x31C6
#define AR0521_SLAVE_GPIO_GPO_1 0x0002
#define AR0521_SLAVE_GPIO_CTRL2 0xFD70
#define AR0521_GRR_GPIO_CTRL2 0xFFF0
#define AR0521_SLAVE_VD_TRIG 0x8000
enum ar0521_op_mode { AR0521_FREE_RUN, AR0521_TRIGGER_ONE_SHOT };
struct v4l2_ctrl { int val; };
struct v4l2_subdev { void *priv; };
struct ar0521 {
    void *dev;
    int mutex;
    struct v4l2_subdev sd;
    struct v4l2_ctrl *exposure, *vblank, *anal_gain, *vflip, *hflip, *test_pattern;
    enum ar0521_op_mode pending_mode, active_mode;
    bool streaming, grr_faulted, grr_rearm, slave_backup_valid, light_source_enabled;
    u16 saved_gpio_gpo_1, saved_gpio_ctrl2, saved_vd_trig, saved_global_seq;
    u16 saved_reset, saved_global_rst_end, saved_global_read_start, saved_global_read_start_hi;
};
#define to_ar0521(sd) ((struct ar0521 *)(sd)->priv)
#define mutex_lock(m) ((void)(m))
#define mutex_unlock(m) ((void)(m))
#define dev_err(...) ((void)0)
#define __v4l2_ctrl_grab(c, v) ((void)(c))
static struct {
    struct ar0521 sensor;
    struct v4l2_ctrl controls[6];
    u16 regs[65536];
    u64 now_us, ready_us, delay_us;
    int status_reads, config_writes, unsafe_config, stream_on, unsafe_on, off_calls;
    int pm_refs, read_fail_nth, write_fail_nth, read_errno, write_errno;
    u16 read_fail_reg, write_fail_reg;
    bool stopped, standby_seen, never_standby;
    bool block_after_error, error_seen, fail_stream_on, block_after_stream_error;
    int events[4096], event_count;
} g;
enum { EV_OFF = 1, EV_STANDBY, EV_CONFIG, EV_ON, EV_ERROR, EV_GATE_RELEASE };
static void event(int value) { if (g.event_count < 4096) g.events[g.event_count++] = value; }
static int ar0521_read_reg(struct ar0521 *s, u16 reg, u16 *value)
{
    if (reg == g.read_fail_reg && g.read_fail_nth > 0 && --g.read_fail_nth == 0) {
        event(EV_ERROR);
        return g.read_errno;
    }
    if (reg == AR0521_REG_FRAME_STATUS) {
        g.status_reads++;
        *value = 1; /* framesync alone is NOT standby */
        if (g.stopped && !g.never_standby &&
            !(g.regs[AR0521_REG_VD_TRIG_NEW_FRAME] & AR0521_SLAVE_VD_TRIG) &&
            !(g.block_after_error && g.error_seen) && g.now_us >= g.ready_us) {
            *value |= AR0521_REG_FRAME_STATUS_STANDBY;
            g.standby_seen = true;
            event(EV_STANDBY);
        }
    } else {
        *value = g.regs[reg];
    }
    return 0;
}
static int ar0521_write_reg(struct ar0521 *s, u16 reg, u16 value)
{
    if (reg == g.write_fail_reg && g.write_fail_nth > 0 && --g.write_fail_nth == 0) {
        g.error_seen = true;
        event(EV_ERROR);
        return g.write_errno;
    }
    if (reg == AR0521_REG_RESET) {
        if (value & AR0521_REG_RESET_STREAM) {
            g.stream_on++;
            if (!g.standby_seen) g.unsafe_on++;
            g.stopped = false;
            event(EV_ON);
            if (g.fail_stream_on) {
                /* A failed I2C transfer may still have reached the device. */
                g.regs[reg] = value;
                g.error_seen = true;
                if (g.block_after_stream_error) g.block_after_error = true;
                event(EV_ERROR);
                return -EIO;
            }
        } else if ((g.regs[reg] & AR0521_REG_RESET_STREAM) || !g.stopped ||
                   value == g.regs[reg]) {
            g.off_calls++;
            if (!g.stopped) {
                g.standby_seen = false;
                g.ready_us = g.now_us + g.delay_us;
            }
            g.stopped = true;
            event(EV_OFF);
        } else {
            /* Changing GPI/default bits while stopped is configuration. */
            g.config_writes++;
            if (!g.standby_seen) g.unsafe_config++;
            event(EV_CONFIG);
        }
    } else if (reg == AR0521_REG_VD_TRIG_NEW_FRAME &&
               !(value & AR0521_SLAVE_VD_TRIG) &&
               !(g.regs[AR0521_REG_RESET] & AR0521_REG_RESET_STREAM)) {
        event(EV_GATE_RELEASE);
    } else {
        g.config_writes++;
        if (!g.stopped || !g.standby_seen) g.unsafe_config++;
        event(EV_CONFIG);
    }
    g.regs[reg] = value;
    return 0;
}
static u64 __attribute__((unused)) ktime_get_ns(void) { return g.now_us * 1000; }
static void __attribute__((unused)) usleep_range(u32 low, u32 high) { g.now_us += low; }
static int pm_runtime_resume_and_get(void *d) { g.pm_refs++; return 0; }
static void pm_runtime_put(void *d) { g.pm_refs--; }
static void pm_runtime_put_sync(void *d) { g.pm_refs--; }
static void pm_runtime_put_autosuspend(void *d) { g.pm_refs--; }
static void pm_runtime_mark_last_busy(void *d) { }
static void ar0521_update_ctrl_visibility_locked(struct ar0521 *s, enum ar0521_op_mode m) { }
static void ar0521_update_exposure_range_locked(struct ar0521 *s, enum ar0521_op_mode m, int v) { }
static void ar0521_light_source_set_locked(struct ar0521 *s, bool on) { }
static int ar0521_light_source_init_locked(struct ar0521 *s) { return 0; }
static int ar0521_init_trigger_pwm(struct ar0521 *s) { return 0; }
static int ar0521_apply_analog_gain_locked(struct ar0521 *s, int v) { return ar0521_write_reg(s, 0x305E, v); }
static int ar0521_apply_flip_locked(struct ar0521 *s) { return ar0521_write_reg(s, 0x3040, 0); }
static int ar0521_apply_test_pattern_locked(struct ar0521 *s, int v) { return ar0521_write_reg(s, 0x3070, v); }
static int ar0521_set_geometry(struct ar0521 *s) { return ar0521_write_reg(s, 0x034C, 2592); }
static int ar0521_apply_vblank_locked(struct ar0521 *s, int v) { return ar0521_write_reg(s, 0x0340, 1944 + v); }
static int ar0521_apply_exposure_locked(struct ar0521 *s, enum ar0521_op_mode m, int v) { return ar0521_write_reg(s, 0x3012, 1556); }
static int ar0521_apply_grr_timing_locked(struct ar0521 *s, int v) { return ar0521_write_reg(s, 0x3160, 0xEC); }
static int ar0521_setup(struct ar0521 *s)
{
    int ret = ar0521_write_reg(s, 0x0306, 0x7359);
    return ret ? ret : ar0521_set_geometry(s);
}

/* ACTUAL_FUNCTIONS */

static int failures, cases;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); failures++; return; } } while (0)
static void init(bool streaming, enum ar0521_op_mode mode)
{
    memset(&g, 0, sizeof(g));
    g.sensor.sd.priv = &g.sensor;
    g.sensor.exposure = &g.controls[0]; g.controls[0].val = 12000;
    g.sensor.vblank = &g.controls[1]; g.controls[1].val = 2380;
    g.sensor.anal_gain = &g.controls[2];
    g.sensor.vflip = &g.controls[3]; g.sensor.hflip = &g.controls[4];
    g.sensor.test_pattern = &g.controls[5];
    g.sensor.streaming = streaming;
    g.sensor.active_mode = g.sensor.pending_mode = mode;
    g.sensor.grr_rearm = true;
    g.regs[AR0521_REG_RESET] = 0x021C;
    g.regs[0x0340] = 4324; g.regs[0x0342] = 3164; g.regs[0x3012] = 1556;
    g.delay_us = 30000;
    g.pm_refs = streaming ? 1 : 0;
    g.read_errno = -EREMOTEIO; g.write_errno = -EIO;
}
static void check_sequence(void)
{
    int i, last_on = -1;
    CHECK(g.status_reads > 1);
    CHECK(g.unsafe_config == 0 && g.unsafe_on == 0);
    CHECK(g.stream_on == 1);
    for (i = 0; i < g.event_count; i++) if (g.events[i] == EV_ON) last_on = i;
    for (i = last_on + 1; i < g.event_count; i++) CHECK(g.events[i] != EV_CONFIG);
}
static void test_mode_sequence(void)
{
    init(true, AR0521_FREE_RUN);
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_TRIGGER_ONE_SHOT) == 0);
    CHECK(g.sensor.active_mode == AR0521_TRIGGER_ONE_SHOT && g.pm_refs == 1);
    CHECK(g.regs[0x3026] == 0xFFF0);
    check_sequence();
}
static void test_start_sequence(void)
{
    init(false, AR0521_TRIGGER_ONE_SHOT);
    CHECK(ar0521_s_stream(&g.sensor.sd, 1) == 0);
    CHECK(g.sensor.streaming && g.pm_refs == 1);
    check_sequence();
}
static void test_free_start_sequence(void)
{
    init(false, AR0521_FREE_RUN);
    CHECK(ar0521_s_stream(&g.sensor.sd, 1) == 0);
    check_sequence();
}
static void test_stop_sequence(void)
{
    init(true, AR0521_TRIGGER_ONE_SHOT);
    g.sensor.slave_backup_valid = true;
    CHECK(ar0521_s_stream(&g.sensor.sd, 0) == 0);
    CHECK(g.status_reads > 1 && g.unsafe_config == 0);
    CHECK(!g.sensor.streaming && g.stream_on == 0 && g.pm_refs == 0);
}
static void test_stop_failure(int kind, bool startup)
{
    init(!startup, startup ? AR0521_TRIGGER_ONE_SHOT : AR0521_FREE_RUN);
    int expected = -ETIMEDOUT;
    if (kind == 0) { g.never_standby = true; g.sensor.grr_rearm = false; }
    if (kind == 1) { g.read_fail_reg = 0x303C; g.read_fail_nth = 2; expected = g.read_errno; }
    if (kind == 2) { g.write_fail_reg = 0x301A; g.write_fail_nth = 1; expected = g.write_errno; }
    if (kind == 3) { g.read_fail_reg = 0x0342; g.read_fail_nth = 1; expected = g.read_errno; }
    int ret = startup ? ar0521_s_stream(&g.sensor.sd, 1) : ar0521_mode_switch(&g.sensor, AR0521_TRIGGER_ONE_SHOT);
    CHECK(ret == expected);
    CHECK(g.config_writes == 0 && g.stream_on == 0);
    CHECK(g.sensor.grr_faulted);
    CHECK(g.pm_refs == (startup ? 0 : 1));
    CHECK(g.now_us < 2000000);
}
static void test_long_frame(void)
{
    init(true, AR0521_FREE_RUN);
    g.regs[0x0340] = 60000;
    g.delay_us = 450000;
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_TRIGGER_ONE_SHOT) == 0);
    CHECK(g.now_us >= 450000);
    check_sequence();
}
static void test_long_exposure(void)
{
    init(true, AR0521_FREE_RUN);
    g.sensor.exposure->val = 800000;
    g.delay_us = 750000;
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_TRIGGER_ONE_SHOT) == 0);
    CHECK(g.now_us >= 750000);
    check_sequence();
}
static void test_restore_sequence(void)
{
    init(true, AR0521_TRIGGER_ONE_SHOT);
    g.sensor.slave_backup_valid = true;
    g.write_fail_reg = 0x034C; g.write_fail_nth = 1; g.write_errno = -EREMOTEIO;
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_FREE_RUN) == -EREMOTEIO);
    CHECK(g.sensor.active_mode == AR0521_TRIGGER_ONE_SHOT && g.pm_refs == 1);
    check_sequence();
}
static void test_restore_stop_failure(void)
{
    init(true, AR0521_FREE_RUN);
    g.write_fail_reg = 0x034C; g.write_fail_nth = 1; g.write_errno = -EREMOTEIO;
    g.block_after_error = true;
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_TRIGGER_ONE_SHOT) == -EREMOTEIO);
    CHECK(g.stream_on == 0 && g.sensor.grr_faulted);
    int i, failed = -1;
    for (i = 0; i < g.event_count; i++) if (g.events[i] == EV_ERROR) failed = i;
    CHECK(failed >= 0);
    /* Group-hold release may follow the failed geometry write; no restore after stop timeout. */
    int last_off = -1;
    for (i = failed + 1; i < g.event_count; i++) if (g.events[i] == EV_OFF) last_off = i;
    CHECK(last_off > failed);
    for (i = last_off + 1; i < g.event_count; i++) CHECK(g.events[i] != EV_CONFIG);
    CHECK(g.pm_refs == 1 && g.sensor.streaming);
}
static void test_restore_stream_failure(bool stop_blocks)
{
    init(true, AR0521_TRIGGER_ONE_SHOT);
    g.sensor.slave_backup_valid = true;
    g.write_fail_reg = 0x034C; g.write_fail_nth = 1; g.write_errno = -EREMOTEIO;
    g.fail_stream_on = true;
    g.block_after_stream_error = stop_blocks;
    CHECK(ar0521_mode_switch(&g.sensor, AR0521_FREE_RUN) == -EREMOTEIO);
    CHECK(g.stream_on == 1 && g.sensor.grr_faulted);
    CHECK(!(g.regs[0x301A] & AR0521_REG_RESET_STREAM));
    CHECK(g.sensor.streaming == stop_blocks && g.pm_refs == (stop_blocks ? 1 : 0));
    CHECK(stop_blocks || g.standby_seen);
}
static void test_disable_failure(void)
{
    init(true, AR0521_TRIGGER_ONE_SHOT);
    g.sensor.slave_backup_valid = true;
    g.never_standby = true;
    CHECK(ar0521_s_stream(&g.sensor.sd, 0) == -ETIMEDOUT);
    CHECK(g.config_writes == 0 && g.stream_on == 0);
    CHECK(g.sensor.streaming && g.sensor.grr_faulted && g.pm_refs == 1);
}
static void test_entry_leaves_stopped(void)
{
    init(false, AR0521_TRIGGER_ONE_SHOT);
    g.stopped = g.standby_seen = true;
    g.regs[0x301A] &= ~AR0521_REG_RESET_STREAM;
    CHECK(ar0521_enter_slave_locked(&g.sensor) == 0);
    CHECK(g.stream_on == 0 && g.unsafe_config == 0);
}
static void test_slave_gate_stop(int failure)
{
    init(true, AR0521_TRIGGER_ONE_SHOT);
    g.sensor.slave_backup_valid = true;
    g.regs[AR0521_REG_VD_TRIG_NEW_FRAME] = AR0521_SLAVE_VD_TRIG | 0x12;
    g.regs[AR0521_REG_GLOBAL_SEQ_TRIGGER] = AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR;
    if (failure == 1) { g.write_fail_reg = AR0521_REG_VD_TRIG_NEW_FRAME; g.write_fail_nth = 1; }
    if (failure == 2) { g.read_fail_reg = AR0521_REG_VD_TRIG_NEW_FRAME; g.read_fail_nth = 1; }
    int ret = ar0521_stream_off_bits(&g.sensor);
    CHECK(ret == (failure == 1 ? g.write_errno : failure == 2 ? g.read_errno : 0));
    CHECK(g.stream_on == 0 && g.config_writes == 0);
    if (failure) { CHECK(!g.standby_seen); return; }
    CHECK(g.standby_seen && g.regs[AR0521_REG_VD_TRIG_NEW_FRAME] == 0x12);
    CHECK(g.regs[AR0521_REG_GLOBAL_SEQ_TRIGGER] == AR0521_REG_GLOBAL_SEQ_TRIGGER_GRR);
    CHECK(g.events[0] == EV_OFF && g.events[1] == EV_GATE_RELEASE);
}
#define RUN(expr) do { int before = failures; cases++; expr; printf("%s: %s\n", failures == before ? "PASS" : "FAILED", #expr); } while (0)
int main(void)
{
    RUN(test_mode_sequence()); RUN(test_start_sequence()); RUN(test_free_start_sequence());
    RUN(test_stop_sequence()); RUN(test_entry_leaves_stopped());
    for (int k = 0; k < 4; k++) { RUN(test_stop_failure(k, false)); RUN(test_stop_failure(k, true)); }
    RUN(test_long_frame()); RUN(test_long_exposure());
    RUN(test_restore_sequence()); RUN(test_restore_stop_failure());
    RUN(test_restore_stream_failure(false)); RUN(test_restore_stream_failure(true));
    RUN(test_disable_failure());
    for (int failure = 0; failure < 3; failure++) RUN(test_slave_gate_stop(failure));
    printf("RESULT: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
