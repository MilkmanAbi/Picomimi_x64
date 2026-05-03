/**
 * Picomimi-x64 Timer Subsystem
 *
 * Provides:
 *   - High-resolution kernel timer infrastructure (timer_list)
 *   - sys_nanosleep / sys_clock_gettime / sys_clock_getres
 *   - sys_alarm — SIGALRM delivery via timer
 *   - sys_gettimeofday
 *   - sys_time
 *   - Boottime / monotonic / realtime clocks
 *   - Per-tick timer wheel (256-slot, 10ms granularity at 100Hz)
 *
 * Clock IDs (POSIX):
 *   CLOCK_REALTIME      0  — wall clock (monotonic + epoch offset)
 *   CLOCK_MONOTONIC     1  — monotonic (jiffies-based)
 *   CLOCK_PROCESS_CPUTIME_ID 2
 *   CLOCK_THREAD_CPUTIME_ID  3
 *   CLOCK_MONOTONIC_RAW 4
 *   CLOCK_BOOTTIME      7
 */

#include <kernel/types.h>
#include <kernel/timer.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>

/* =========================================================
 * External state
 * ========================================================= */
extern volatile u64 jiffies;           /* Incremented by PIT IRQ (100 Hz) */
extern volatile u64 system_ticks;      /* Same as jiffies (alias from handlers.c) */

/* Boot epoch: seconds since Unix epoch at boot.
 * In a real kernel this comes from RTC; we fake it as
 * 2025-01-01 00:00:00 UTC = 1735689600 */
#define BOOT_EPOCH_SEC  1735689600ULL

/* Jiffy-to-nanosecond conversion: 100Hz → 10ms per tick */
#define JIFFIES_PER_SEC         100ULL
#ifndef NSEC_PER_JIFFIE
#define NSEC_PER_JIFFIE         (NSEC_PER_SEC / JIFFIES_PER_SEC)
#endif

/* =========================================================
 * Clock helpers
 * ========================================================= */

static void jiffies_to_timespec(u64 j, struct timespec *ts) {
    ts->tv_sec  = (time_t)(j / JIFFIES_PER_SEC);
    ts->tv_nsec = (s64)((j % JIFFIES_PER_SEC) * NSEC_PER_JIFFIE);
}

ktime_t ktime_get(void) {
    return (ktime_t)(jiffies * NSEC_PER_JIFFIE);
}

ktime_t ktime_get_real(void) {
    return ktime_get() + (ktime_t)(BOOT_EPOCH_SEC * NSEC_PER_SEC);
}

ktime_t ktime_get_boottime(void) {
    return ktime_get();
}

u64 ktime_get_ns(void) {
    return (u64)ktime_get();
}

struct timespec ktime_to_timespec(ktime_t kt) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(kt / NSEC_PER_SEC);
    ts.tv_nsec = (s64)(kt % NSEC_PER_SEC);
    return ts;
}

/* =========================================================
 * Timer wheel
 *
 * Simple 256-slot hashed timer wheel.
 * Each slot holds a list of timer_list entries that expire
 * within the same ~10ms bucket.  The PIT IRQ calls
 * run_timer_softirq() once per tick to fire expired timers.
 * ========================================================= */

#define TIMER_WHEEL_SIZE    256
#define TIMER_WHEEL_MASK    (TIMER_WHEEL_SIZE - 1)

static struct list_head timer_wheel[TIMER_WHEEL_SIZE];
static spinlock_t       timer_lock = { .raw_lock = { 0 } };

void timers_init(void) {
    for (int i = 0; i < TIMER_WHEEL_SIZE; i++)
        INIT_LIST_HEAD(&timer_wheel[i]);
    printk(KERN_INFO "[timer] timer wheel initialized (%d slots, %llu ms/slot)\n",
           TIMER_WHEEL_SIZE, NSEC_PER_JIFFIE / 1000000ULL);
}

void init_timer(timer_list_t *timer) {
    INIT_LIST_HEAD(&timer->entry);
    timer->expires  = 0;
    timer->function = NULL;
    timer->data     = 0;
    timer->flags    = 0;
}

void add_timer(timer_list_t *timer) {
    if (!timer->function) return;

    spin_lock(&timer_lock);
    u64 slot = timer->expires & TIMER_WHEEL_MASK;
    list_add_tail(&timer->entry, &timer_wheel[slot]);
    spin_unlock(&timer_lock);
}

void mod_timer(timer_list_t *timer, u64 expires) {
    spin_lock(&timer_lock);
    if (!list_empty(&timer->entry))
        list_del_init(&timer->entry);
    timer->expires = expires;
    u64 slot = expires & TIMER_WHEEL_MASK;
    list_add_tail(&timer->entry, &timer_wheel[slot]);
    spin_unlock(&timer_lock);
}

int del_timer(timer_list_t *timer) {
    spin_lock(&timer_lock);
    int was_pending = !list_empty(&timer->entry);
    if (was_pending)
        list_del_init(&timer->entry);
    spin_unlock(&timer_lock);
    return was_pending;
}

int timer_pending(const timer_list_t *timer) {
    return !list_empty(&timer->entry);
}

/* Called from PIT IRQ — fires expired timers */
void run_timer_softirq(void) {
    u64 now = jiffies;
    u64 slot = now & TIMER_WHEEL_MASK;

    spin_lock(&timer_lock);

    struct list_head *p, *n;
    list_for_each_safe(p, n, &timer_wheel[slot]) {
        timer_list_t *t = list_entry(p, timer_list_t, entry);

        if ((s64)(t->expires - now) > 0)
            continue;   /* Not yet expired (hash collision from future) */

        list_del_init(&t->entry);
        void (*fn)(unsigned long) = t->function;
        unsigned long data = t->data;

        spin_unlock(&timer_lock);

        if (fn) fn(data);

        spin_lock(&timer_lock);
    }

    spin_unlock(&timer_lock);
}

/* =========================================================
 * ALARM — per-task SIGALRM via timer
 * ========================================================= */

static void alarm_timer_fn(unsigned long data) {
    pid_t pid = (pid_t)(int)data;
    send_signal(pid, SIGALRM);
}

s64 sys_alarm(unsigned int seconds) {
    task_struct_t *t = current;

    u64 remaining = 0;

    if (timer_pending(&t->alarm_timer)) {
        u64 exp = t->alarm_timer.expires;
        if ((s64)(exp - jiffies) > 0)
            remaining = (u64)(exp - jiffies) / JIFFIES_PER_SEC;
        del_timer(&t->alarm_timer);
    }

    if (seconds > 0) {
        init_timer(&t->alarm_timer);
        t->alarm_timer.expires  = jiffies + (u64)seconds * JIFFIES_PER_SEC;
        t->alarm_timer.function = alarm_timer_fn;
        t->alarm_timer.data     = (unsigned long)t->pid;
        add_timer(&t->alarm_timer);
    }

    return (s64)remaining;
}

/* =========================================================
 * nanosleep
 * ========================================================= */

s64 sys_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) return -EFAULT;

    if (req->tv_nsec < 0 || req->tv_nsec >= NSEC_PER_SEC)
        return -EINVAL;

    /* Convert requested duration to jiffies (ceiling) */
    u64 ns_total = (u64)req->tv_sec * NSEC_PER_SEC + (u64)req->tv_nsec;
    u64 ticks    = (ns_total + NSEC_PER_JIFFIE - 1) / NSEC_PER_JIFFIE;

    if (ticks == 0) return 0;

    u64 deadline = jiffies + ticks;

    /* Busy-wait with HLT for now (no real sleep queue yet) */
    while ((s64)(deadline - jiffies) > 0) {
        if (signal_pending(current)) {
            /* Report remaining time */
            if (rem) {
                u64 left = (u64)(deadline - jiffies) * NSEC_PER_JIFFIE;
                rem->tv_sec  = (time_t)(left / NSEC_PER_SEC);
                rem->tv_nsec = (s64)(left % NSEC_PER_SEC);
            }
            return -EINTR;
        }
        __asm__ volatile("hlt");
    }

    if (rem) {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}

/* =========================================================
 * clock_gettime / clock_getres
 * ========================================================= */

#define CLOCK_REALTIME              0
#define CLOCK_MONOTONIC             1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID     3
#define CLOCK_MONOTONIC_RAW         4
#define CLOCK_BOOTTIME              7

s64 sys_clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) return -EFAULT;

    struct timespec ts;

    switch (clk_id) {
    case CLOCK_REALTIME: {
        ktime_t kt = ktime_get_real();
        ts = ktime_to_timespec(kt);
        break;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME: {
        jiffies_to_timespec(jiffies, &ts);
        break;
    }
    case CLOCK_PROCESS_CPUTIME_ID: {
        task_struct_t *t = current;
        u64 ns = t ? (t->utime + t->stime) : 0;
        ts.tv_sec  = (time_t)(ns / NSEC_PER_SEC);
        ts.tv_nsec = (s64)(ns % NSEC_PER_SEC);
        break;
    }
    case CLOCK_THREAD_CPUTIME_ID: {
        task_struct_t *t = current;
        u64 ns = t ? t->utime : 0;
        ts.tv_sec  = (time_t)(ns / NSEC_PER_SEC);
        ts.tv_nsec = (s64)(ns % NSEC_PER_SEC);
        break;
    }
    default:
        return -EINVAL;
    }

    memcpy(tp, &ts, sizeof(ts));
    return 0;
}

s64 sys_clock_getres(int clk_id, struct timespec *res) {
    if (clk_id < 0) return -EINVAL;
    if (res) {
        res->tv_sec  = 0;
        res->tv_nsec = (s64)NSEC_PER_JIFFIE;
    }
    return 0;
}

s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv) {
        ktime_t kt = ktime_get_real();
        tv->tv_sec  = (time_t)(kt / NSEC_PER_SEC);
        tv->tv_usec = (s64)((kt % NSEC_PER_SEC) / NSEC_PER_USEC);
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime     = 0;
    }
    return 0;
}

s64 sys_time(time_t *tloc) {
    ktime_t kt = ktime_get_real();
    time_t t   = (time_t)(kt / NSEC_PER_SEC);
    if (tloc) *tloc = t;
    return (s64)t;
}

s64 sys_clock_settime(int clk_id, const struct timespec *tp) {
    /* Only root can set time — stub */
    (void)clk_id; (void)tp;
    return -EPERM;
}

s64 sys_settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tv; (void)tz;
    return -EPERM;
}

/* =========================================================
 * Uptime helper (exported for sysinfo)
 * ========================================================= */

u64 get_uptime_seconds(void) {
    return jiffies / JIFFIES_PER_SEC;
}

/* =========================================================
 * Delayed work (simple one-shot, fires after 'delay_jiffies')
 * ========================================================= */

void schedule_delayed_work(timer_list_t *timer, u64 delay_jiffies,
                            void (*fn)(unsigned long), unsigned long data) {
    init_timer(timer);
    timer->expires  = jiffies + delay_jiffies;
    timer->function = fn;
    timer->data     = data;
    add_timer(timer);
}
