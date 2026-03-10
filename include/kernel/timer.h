/**
 * Picomimi-x64 Timer Header
 */
#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <kernel/types.h>

/* timer_list — kernel timer entry */
#ifndef _TIMER_LIST_DEFINED
#define _TIMER_LIST_DEFINED
typedef struct timer_list {
    struct list_head    entry;
    u64                 expires;            /* Expiry in jiffies */
    void                (*function)(unsigned long);
    unsigned long       data;
    u32                 flags;
} timer_list_t;
#endif

/* Clock IDs */
#define CLOCK_REALTIME              0
#define CLOCK_MONOTONIC             1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID     3
#define CLOCK_MONOTONIC_RAW         4
#define CLOCK_BOOTTIME              7

/* Boot epoch */
#define BOOT_EPOCH_SEC  1735689600ULL

/* ktime API */
ktime_t ktime_get(void);
ktime_t ktime_get_real(void);
ktime_t ktime_get_boottime(void);
u64     ktime_get_ns(void);
struct timespec ktime_to_timespec(ktime_t kt);

/* Timer wheel API */
void timers_init(void);
void init_timer(timer_list_t *timer);
void add_timer(timer_list_t *timer);
void mod_timer(timer_list_t *timer, u64 expires);
int  del_timer(timer_list_t *timer);
int  timer_pending(const timer_list_t *timer);
void run_timer_softirq(void);
void schedule_delayed_work(timer_list_t *timer, u64 delay_jiffies,
                            void (*fn)(unsigned long), unsigned long data);

/* Uptime */
u64 get_uptime_seconds(void);

/* Syscalls */
struct timezone;  /* forward declaration */
s64 sys_nanosleep(const struct timespec *req, struct timespec *rem);
s64 sys_clock_gettime(int clk_id, struct timespec *tp);
s64 sys_clock_getres(int clk_id, struct timespec *res);
s64 sys_clock_settime(int clk_id, const struct timespec *tp);
s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz);
s64 sys_settimeofday(const struct timeval *tv, const struct timezone *tz);
s64 sys_time(time_t *tloc);
s64 sys_alarm(unsigned int seconds);

#endif /* _KERNEL_TIMER_H */
