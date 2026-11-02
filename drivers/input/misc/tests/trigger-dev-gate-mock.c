#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
typedef unsigned long long u64;
typedef unsigned int u32;
typedef struct { int value; } atomic_t;
typedef struct { long long value; } atomic64_t;
struct mutex { atomic_flag locked; };
#define TEST_MUTEX_INIT { .locked = ATOMIC_FLAG_INIT }
#define atomic_read(a) __atomic_load_n(&(a)->value, __ATOMIC_SEQ_CST)
#define atomic_set(a, v) __atomic_store_n(&(a)->value, (v), __ATOMIC_SEQ_CST)
static int atomic_inc_return(atomic_t *a);
#define atomic_xchg(a, v) __atomic_exchange_n(&(a)->value, (v), __ATOMIC_SEQ_CST)
#define atomic64_read(a) __atomic_load_n(&(a)->value, __ATOMIC_SEQ_CST)
#define atomic64_add(v, a) __atomic_add_fetch(&(a)->value, (v), __ATOMIC_SEQ_CST)
#define atomic64_inc(a) __atomic_add_fetch(&(a)->value, 1, __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define mutex_lock(m) do { while (atomic_flag_test_and_set(&(m)->locked)) sched_yield(); } while (0)
#define mutex_unlock(m) atomic_flag_clear(&(m)->locked)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define to_delayed_work(work) container_of(work, struct delayed_work, work)
#define IRQCHIP_STATE_PENDING 0
struct work_struct { int pending; };
struct delayed_work { struct work_struct work; int pending; };
enum trigger_dev_input_mode { TRIGGER_DEV_INPUT_MODE_EDGE, TRIGGER_DEV_INPUT_MODE_BUTTON };
typedef int irqreturn_t;
#define IRQ_HANDLED 1
struct trigger_dev { int irq; bool enabled; struct mutex config_mutex; struct mutex gate_mutex; atomic_t trigger_pending; atomic64_t gate_pending_dropped; atomic64_t irq_count; atomic64_t gate_irq_count; u64 last_irq_ns; enum trigger_dev_input_mode input_mode; u32 max_pending; u32 debounce_ms; void *input_gpiod; void *dev; struct work_struct trigger_work; struct delayed_work debounce_work; };
static int disable_calls, enable_calls, sync_calls, cancel_work_calls, cancel_debounce_calls, pending_clear_calls, schedule_calls, debounce_state_calls;
static int order, sync_order, debounce_cancel_order, work_cancel_order;
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_cond = PTHREAD_COND_INITIALIZER;
static int block_cancel, cancel_entered, release_cancel, enable_started, enable_finished;
static int active_irq, sync_entered, block_atomic_inc, atomic_inc_entered, release_atomic_inc;
static int atomic_inc_return(atomic_t *a) {
 pthread_mutex_lock(&block_lock);
 if (block_atomic_inc) { atomic_inc_entered = 1; pthread_cond_broadcast(&block_cond); while (!release_atomic_inc) pthread_cond_wait(&block_cond, &block_lock); }
 pthread_mutex_unlock(&block_lock);
 return __atomic_add_fetch(&a->value, 1, __ATOMIC_SEQ_CST);
}
static void synchronize_irq(int irq) {
 sync_calls++;
 pthread_mutex_lock(&block_lock);
 sync_entered = 1; pthread_cond_broadcast(&block_cond);
 while (active_irq) pthread_cond_wait(&block_cond, &block_lock);
 pthread_mutex_unlock(&block_lock);
 sync_order = ++order;
}
static void cancel_work_sync(struct work_struct *work) {
 cancel_work_calls++;
 pthread_mutex_lock(&block_lock);
 if (block_cancel) { cancel_entered = 1; pthread_cond_broadcast(&block_cond); while (!release_cancel) pthread_cond_wait(&block_cond, &block_lock); }
 pthread_mutex_unlock(&block_lock);
 work_cancel_order = ++order;
}
static void cancel_delayed_work_sync(struct delayed_work *work) { cancel_debounce_calls++; debounce_cancel_order = ++order; }
static u64 ktime_get_ns(void) { return 1; }
static void schedule_work(struct work_struct *work) { schedule_calls++; work->pending = 1; }
static void mod_delayed_work(void *wq, struct delayed_work *work, unsigned long delay) { schedule_calls++; work->pending = 1; }
static void *system_wq;
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static int gpiod_get_value_cansleep(void *gpio) { return 1; }
static void trigger_dev_handle_state(struct trigger_dev *tdev, bool pressed) { debounce_state_calls++; }
#define dev_warn(dev, fmt, ...) ((void)0)
#define dev_err(dev, fmt, ...) ((void)0)

/* ACTUAL_FUNCTIONS */
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); return 1; } } while (0)
static int test_disable_drains_pending(void)
{
 struct trigger_dev d = { .irq = 5, .enabled = true, .config_mutex = TEST_MUTEX_INIT, .gate_mutex = TEST_MUTEX_INIT, .trigger_pending = { 3 } };
 CHECK(trigger_dev_set_enabled(&d, false) == 0);
 CHECK(!d.enabled && atomic_read(&d.trigger_pending) == 0);
 CHECK(atomic64_read(&d.gate_pending_dropped) == 3);
 CHECK(sync_calls == 1 && cancel_work_calls == 1 && cancel_debounce_calls == 1);
 CHECK(sync_order < debounce_cancel_order && debounce_cancel_order < work_cancel_order);
 CHECK(disable_calls == 0 && enable_calls == 0 && pending_clear_calls == 0);
 return 0;
}
static int test_enable_is_logical_only(void)
{
 struct trigger_dev d = { .irq = 5, .enabled = false, .config_mutex = TEST_MUTEX_INIT, .gate_mutex = TEST_MUTEX_INIT };
 CHECK(trigger_dev_set_enabled(&d, true) == 0);
 CHECK(d.enabled && pending_clear_calls == 0 && enable_calls == 0);
 return 0;
}
static int test_disabled_irq_is_counted_not_queued(void)
{
 struct trigger_dev d = { .irq = 5, .enabled = false, .config_mutex = TEST_MUTEX_INIT, .gate_mutex = TEST_MUTEX_INIT, .input_mode = TRIGGER_DEV_INPUT_MODE_EDGE };
 int before_schedule = schedule_calls;
 CHECK(trigger_dev_irq(5, &d) == IRQ_HANDLED);
 CHECK(atomic64_read(&d.irq_count) == 1 && atomic64_read(&d.gate_irq_count) == 1);
 CHECK(atomic_read(&d.trigger_pending) == 0 && schedule_calls == before_schedule);
 return 0;
}
static int test_disabled_debounce_does_not_report_state(void)
{
 struct trigger_dev d = { .irq = 5, .enabled = false, .config_mutex = TEST_MUTEX_INIT, .gate_mutex = TEST_MUTEX_INIT };
 trigger_dev_debounce_work(&d.debounce_work.work);
 CHECK(debounce_state_calls == 0 && atomic_read(&d.trigger_pending) == 0);
 return 0;
}
struct gate_thread { struct trigger_dev *dev; bool enable; };
static void *set_enabled_thread(void *arg)
{
 struct gate_thread *t = arg;
 if (t->enable) { pthread_mutex_lock(&block_lock); enable_started = 1; pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_lock); }
 int ret = trigger_dev_set_enabled(t->dev, t->enable);
 if (t->enable) { pthread_mutex_lock(&block_lock); enable_finished = (ret == 0); pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_lock); }
 return (void *)(intptr_t)ret;
}
static void *irq_thread(void *arg)
{
 struct trigger_dev *d = arg;
 pthread_mutex_lock(&block_lock); active_irq++; pthread_mutex_unlock(&block_lock);
 trigger_dev_irq(d->irq, d);
 pthread_mutex_lock(&block_lock); active_irq--; pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_lock);
 return NULL;
}
static int test_inflight_irq_drain_and_concurrent_reopen(void)
{
 struct trigger_dev d = { .irq = 5, .enabled = true, .config_mutex = TEST_MUTEX_INIT, .gate_mutex = TEST_MUTEX_INIT, .input_mode = TRIGGER_DEV_INPUT_MODE_EDGE };
 struct gate_thread off = { &d, false }, on = { &d, true };
 pthread_t irq, disable_thread, enable_thread;
 void *result;
 block_atomic_inc = 1; atomic_inc_entered = release_atomic_inc = sync_entered = 0;
 block_cancel = 1; cancel_entered = release_cancel = enable_started = enable_finished = 0;
 CHECK(pthread_create(&irq, NULL, irq_thread, &d) == 0);
 pthread_mutex_lock(&block_lock); while (!atomic_inc_entered) pthread_cond_wait(&block_cond, &block_lock); pthread_mutex_unlock(&block_lock);
 CHECK(pthread_create(&disable_thread, NULL, set_enabled_thread, &off) == 0);
 pthread_mutex_lock(&block_lock); while (!sync_entered) pthread_cond_wait(&block_cond, &block_lock); pthread_mutex_unlock(&block_lock);
 CHECK(pthread_create(&enable_thread, NULL, set_enabled_thread, &on) == 0);
 pthread_mutex_lock(&block_lock); while (!enable_started) pthread_cond_wait(&block_cond, &block_lock); pthread_mutex_unlock(&block_lock);
 for (int i = 0; i < 10000; i++) sched_yield();
 CHECK(!enable_finished);
 pthread_mutex_lock(&block_lock); release_atomic_inc = 1; pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_lock);
 CHECK(pthread_join(irq, NULL) == 0);
 pthread_mutex_lock(&block_lock); while (!cancel_entered) pthread_cond_wait(&block_cond, &block_lock); pthread_mutex_unlock(&block_lock);
 CHECK(atomic_read(&d.trigger_pending) == 1 && schedule_calls == 1);
 pthread_mutex_lock(&block_lock); release_cancel = 1; pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_lock);
 CHECK(pthread_join(disable_thread, &result) == 0 && (intptr_t)result == 0);
 CHECK(pthread_join(enable_thread, &result) == 0 && (intptr_t)result == 0);
 CHECK(d.enabled && atomic_read(&d.trigger_pending) == 0);
 CHECK(atomic64_read(&d.gate_pending_dropped) == 1);
 CHECK(atomic64_read(&d.irq_count) == 1 && atomic64_read(&d.gate_irq_count) == 0);
 CHECK(disable_calls == 0 && enable_calls == 0 && pending_clear_calls == 0);
 block_atomic_inc = block_cancel = 0;
 return 0;
}
int main(void)
{
 int ret = test_disable_drains_pending() | test_enable_is_logical_only() |
           test_disabled_irq_is_counted_not_queued() |
           test_disabled_debounce_does_not_report_state();
 ret |= test_inflight_irq_drain_and_concurrent_reopen();
 puts(ret ? "FAIL" : "PASS: trigger-dev gate behavioral cases");
 return ret;
}
