#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/logging/log.h>

#include <sl_atomic.h>
#include <sl_sleeptimer.h>

LOG_MODULE_REGISTER(silabs_sleeptimer_timer);

/* Maximum time interval between timer interrupts (in hw_cycles) */
#define MAX_TIMEOUT_CYC (UINT32_MAX >> 1)
#define MIN_DELAY_CYC   (4U)

#define DT_RTC DT_COMPAT_GET_ANY_STATUS_OKAY(silabs_gecko_stimer)
/* Ensure potential interrupt names don't expand to register interface struct pointers
 * when there are name collisions.
 */
#undef RTCC

/* With CONFIG_TIMER_READS_ITS_FREQUENCY_AT_RUNTIME, this global variable holds the clock frequency,
 * and must be written by the driver at init.
 */
extern int z_clock_hw_cycles_per_sec;

/* Number of hw_cycles clocks per 1 kernel tick */
static uint32_t g_cyc_per_tick;

/* MAX_TIMEOUT_CYC expressed as ticks */
static uint32_t g_max_timeout_ticks;

/* Value of sleeptimer counter when the previous kernel tick was announced */
static atomic_t g_last_count;

/* Spinlock to sync between Compare ISR and update of Compare register */
static struct k_spinlock g_lock;

/* Set to true when timer is initialized */
static bool g_init;

static sl_sleeptimer_timer_handle_t sys_timer;

static void sleeptimer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
	ARG_UNUSED(handle);
	ARG_UNUSED(data);
	uint32_t curr = sl_sleeptimer_get_tick_count();
	uint32_t prev = atomic_get(&g_last_count);
	uint32_t pending = curr - prev;

	/* How many ticks have we not announced since the last announcement */
	uint32_t unannounced = pending / g_cyc_per_tick;

	atomic_set(&g_last_count, prev + unannounced * g_cyc_per_tick);

	sys_clock_announce(unannounced);
}

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
	ARG_UNUSED(idle);

	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return;
	}

	ticks = (ticks == K_TICKS_FOREVER) ? g_max_timeout_ticks : ticks;
	ticks = CLAMP(ticks, 0, g_max_timeout_ticks);

	k_spinlock_key_t key = k_spin_lock(&g_lock);

	uint32_t curr = sl_sleeptimer_get_tick_count();
	uint32_t prev = atomic_get(&g_last_count);
	uint32_t pending = curr - prev;
	uint32_t next = ticks * g_cyc_per_tick;

	/* Next timeout is N ticks in the future, minus the current progress
	 * towards the timeout. If we are behind, set the timeout to the first
	 * possible upcoming tick.
	 */
	while (next < (pending + MIN_DELAY_CYC)) {
		next += g_cyc_per_tick;
	}
	next -= pending;

	sl_sleeptimer_restart_timer(&sys_timer, next, sleeptimer_cb, NULL, 0, 0);
	k_spin_unlock(&g_lock, key);
}

uint32_t sys_clock_elapsed(void)
{
	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL) || !g_init) {
		/* By definition, no unannounced ticks have elapsed if not in tickless mode */
		return 0;
	} else {
		return (sl_sleeptimer_get_tick_count() - atomic_get(&g_last_count)) /
		       g_cyc_per_tick;
	}
}

uint32_t sys_clock_cycle_get_32(void)
{
	return g_init ? sl_sleeptimer_get_tick_count() : 0;
}

static int sleeptimer_init(void)
{
	sl_status_t status = SL_STATUS_OK;

	IRQ_CONNECT(DT_IRQ(DT_RTC, irq), DT_IRQ(DT_RTC, priority),
		    CONCAT(DT_STRING_UPPER_TOKEN_BY_IDX(DT_RTC, interrupt_names, 0), _IRQHandler),
		    0, 0);

	sl_sleeptimer_init();

	z_clock_hw_cycles_per_sec = sl_sleeptimer_get_timer_frequency();

	BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC > 0,
		     "Invalid CONFIG_SYS_CLOCK_TICKS_PER_SEC value");

	g_cyc_per_tick = z_clock_hw_cycles_per_sec / CONFIG_SYS_CLOCK_TICKS_PER_SEC;

	__ASSERT(g_cyc_per_tick >= MIN_DELAY_CYC,
		 "A tick of %u cycles is too short to be scheduled "
		 "(min is %u). Config: SYS_CLOCK_TICKS_PER_SEC is "
		 "%d and timer frequency is %u",
		 g_cyc_per_tick, MIN_DELAY_CYC, CONFIG_SYS_CLOCK_TICKS_PER_SEC,
		 z_clock_hw_cycles_per_sec);

	g_max_timeout_ticks = MAX_TIMEOUT_CYC / g_cyc_per_tick;
	g_init = true;

	atomic_set(&g_last_count, sl_sleeptimer_get_tick_count());

	/* Start the timer and announce 1 kernel tick */
	if (IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		status = sl_sleeptimer_start_timer(&sys_timer, g_cyc_per_tick, sleeptimer_cb, NULL,
						   0, 0);
	} else {
		status = sl_sleeptimer_start_periodic_timer(&sys_timer, g_cyc_per_tick,
							    sleeptimer_cb, NULL, 0, 0);
	}
	if (status != SL_STATUS_OK) {
		return -ENODEV;
	}

	return 0;
}

SYS_INIT(sleeptimer_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
