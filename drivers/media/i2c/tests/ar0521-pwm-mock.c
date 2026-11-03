/* Model cold pinmux high, enabled-only active selection and disabled output
 * retaining low OR returning high. Tests validate consumer sequencing, not
 * the physical controller latch, pulse width or disabled-level guarantee. */
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
#define clamp_t(t, v, lo, hi) ((t)(v) < (lo) ? (lo) : ((t)(v) > (hi) ? (hi) : (t)(v)))
#define PWM_POLARITY_NORMAL 0
#define AR0521_TRIGGER_PERIOD_NS_DEFAULT 10000000ULL
#define AR0521_TRIGGER_PERIOD_MARGIN_NS 10000000ULL
#define AR0521_TRIGGER_PULSE_US_MIN 1U
#define AR0521_TRIGGER_PULSE_US_MAX 1000000U
#define AR0521_REG_RESET 0x301A
#define AR0521_REG_RESET_GPI BIT(8)
#define AR0521_REG_GPIO_GPO_1 0x30F8
#define AR0521_REG_GPIO_CTRL2 0x3026
#define AR0521_REG_VD_TRIG_NEW_FRAME 0x3158
#define AR0521_REG_GLOBAL_SEQ_TRIGGER 0x315E
#define AR0521_REG_GLOBAL_RST_END 0x3160
#define AR0521_REG_GLOBAL_READ_START 0x3166
#define AR0521_REG_GLOBAL_READ_START_HI 0x3168
#define AR0521_SLAVE_GPIO_GPO_1 2
#define AR0521_GPI_PIN_GPI2 2
#define AR0521_SLAVE_GPIO_CTRL2 0xFD70
#define AR0521_GRR_GPIO_CTRL2 0xFFF0
#define AR0521_SLAVE_VD_TRIG 0x8000
#define AR0521_TRIGGER_ONE_SHOT 1
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
struct pwm_state { u64 period, duty_cycle; int polarity; u64 oneshot_count; u32 oneshot_repeat; u64 duty_offset; bool enabled, usage_power; };
struct pwm_args { u64 period; };
struct pwm_device { struct pwm_state state; struct pwm_args args; };
struct v4l2_ctrl { int val; };
struct ar0521 {
    struct pwm_device *trigger_pwm;
    struct v4l2_ctrl *exposure;
    void *dev, *light_source_gpio;
    u32 trigger_pulse_us, light_source_advance_us, light_source_off_delay_us;
    bool light_source_enabled, streaming, grr_rearm, slave_backup_valid;
    int active_mode, pending_mode;
    u16 saved_gpio_gpo_1, saved_gpio_ctrl2, saved_vd_trig, saved_global_seq;
    u16 saved_reset, saved_global_rst_end, saved_global_read_start, saved_global_read_start_hi;
};
static struct {
    struct ar0521 sensor;
    struct pwm_device pwm;
    struct v4l2_ctrl exposure;
    u16 regs[65536];
    bool pad_high, stuck_high, disabled_high;
    u64 phase_ns, elapsed_us, high_ns;
    u32 cleanup_delay_us;
    int apply_calls, hardware_calls, fail_call, rising_edges, unsafe_connect;
    int connect_calls, low_reads, pwm_while_connected, active_selects;
} g;
static bool connected(void) { return (g.regs[AR0521_REG_GPIO_CTRL2] & 0x380) == 0x100; }
static void drive(bool high)
{
    high |= g.stuck_high;
    if (high && !g.pad_high && connected()) g.rising_edges++;
    g.pad_high = high;
}
static void pwm_get_state(struct pwm_device *pwm, struct pwm_state *state) { *state = pwm->state; }
static void pwm_get_args(struct pwm_device *pwm, struct pwm_args *args) { *args = pwm->args; }
static int pwm_apply_state(struct pwm_device *pwm, const struct pwm_state *state)
{
    struct pwm_state *old = &pwm->state;
    g.apply_calls++;
    if (g.fail_call == g.apply_calls) return -EIO;
    if (state->period == old->period && state->duty_cycle == old->duty_cycle &&
        state->polarity == old->polarity && state->enabled == old->enabled &&
        state->oneshot_count == old->oneshot_count && state->oneshot_repeat == old->oneshot_repeat &&
        state->duty_offset == old->duty_offset && state->usage_power == old->usage_power)
        return 0; /* Real PWM core caches equal states. */
    g.hardware_calls++;
    if (connected()) g.pwm_while_connected++;
    if (!state->enabled) {
        if (g.disabled_high) drive(true);
    } else {
        g.active_selects++; /* Provider selects active pinctrl only enabled. */
        if (!old->enabled) g.phase_ns = 0;
        drive(state->duty_cycle == state->period ||
              (state->duty_cycle && g.phase_ns < state->duty_cycle));
    }
    *old = *state;
    return 0;
}
static int ar0521_read_reg(struct ar0521 *s, u16 reg, u16 *value)
{
    *value = g.regs[reg];
    if (reg == AR0521_REG_GPIO_CTRL2) {
        *value = (u16)((*value & ~BIT(2)) | (g.pad_high ? BIT(2) : 0));
        if (!g.pad_high && !connected()) g.low_reads++;
    }
    return 0;
}
static int ar0521_write_reg(struct ar0521 *s, u16 reg, u16 value)
{
    if (reg == AR0521_REG_GPIO_CTRL2 && (value & 0x380) == 0x100) {
        g.connect_calls++;
        if (g.pad_high) g.unsafe_connect++;
    }
    g.regs[reg] = value;
    return 0;
}
static int ar0521_config_grr_seq_trigger_locked(struct ar0521 *s) { return 0; }
static int ar0521_apply_grr_timing_locked(struct ar0521 *s, int exposure) { return 0; }
/* ACTUAL_FUNCTIONS */
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); return 1; } } while (0)
static void init(void)
{
    memset(&g, 0, sizeof(g));
    g.sensor.trigger_pwm = &g.pwm;
    g.sensor.exposure = &g.exposure;
    g.sensor.trigger_pulse_us = 800;
    g.sensor.streaming = true;
    g.sensor.active_mode = AR0521_TRIGGER_ONE_SHOT;
    g.pwm.args.period = 10000000;
    g.regs[AR0521_REG_GPIO_CTRL2] = AR0521_GRR_GPIO_CTRL2;
    g.pad_high = true;
}
static int test_probe_primes_low_then_disables(void)
{
    init();
    CHECK(ar0521_init_trigger_pwm(&g.sensor) == 0);
    CHECK(!g.pwm.state.enabled && !g.pwm.state.duty_cycle && !g.pad_high);
    CHECK(g.active_selects == 1 && g.hardware_calls == 2);
    CHECK(g.elapsed_us == 0 && g.rising_edges == 0);
    return 0;
}
static int test_prepare_before_connect(void)
{
    init();
    CHECK(ar0521_enter_slave_locked(&g.sensor) == 0);
    CHECK(g.connect_calls == 1 && g.unsafe_connect == 0 && g.low_reads > 0);
    CHECK(g.pwm_while_connected == 0 && g.rising_edges == 0 && !g.pad_high);
    CHECK(!g.pwm.state.enabled);
    return 0;
}
static int test_prepare_after_context_loss(void)
{
    init();
    /* Cache matches the final disabled state, but pinmux/output was lost. */
    g.pwm.state.period = 10800000;
    CHECK(ar0521_enter_slave_locked(&g.sensor) == 0);
    CHECK(g.active_selects == 1 && g.hardware_calls == 2);
    CHECK(!g.pwm.state.enabled && !g.pad_high && !g.unsafe_connect);
    return 0;
}
static int test_prepare_error_does_not_connect(void)
{
    for (int fail = 1; fail <= 2; fail++) {
        init();
        g.fail_call = fail;
        CHECK(ar0521_enter_slave_locked(&g.sensor) == -EIO);
        CHECK(g.connect_calls == 0 && !connected());
    }
    init();
    g.stuck_high = true;
    CHECK(ar0521_enter_slave_locked(&g.sensor) == -EIO);
    CHECK(g.connect_calls == 0 && !connected());
    return 0;
}
static int test_grr_keeps_input_isolated(void)
{
    init();
    g.sensor.grr_rearm = true;
    CHECK(ar0521_enter_slave_locked(&g.sensor) == 0);
    CHECK(!connected() && g.apply_calls == 0 && !g.rising_edges);
    return 0;
}
static int test_disabled_high_is_not_connected(void)
{
    init();
    /* Zero duty establishes low, but disabling is allowed to return high. */
    g.disabled_high = true;
    CHECK(ar0521_enter_slave_locked(&g.sensor) == -EIO);
    CHECK(g.connect_calls == 0 && !connected() && !g.rising_edges);
    CHECK(!g.pwm.state.enabled && g.pad_high);
    return 0;
}
int main(void)
{
    int ret = test_probe_primes_low_then_disables();
    ret |= test_prepare_before_connect();
    ret |= test_prepare_after_context_loss();
    ret |= test_prepare_error_does_not_connect();
    ret |= test_grr_keeps_input_isolated();
    ret |= test_disabled_high_is_not_connected();
    puts(ret ? "FAIL: PWM preparation contracts" : "PASS: PWM preparation contracts (5 cases)");
    return ret;
}
