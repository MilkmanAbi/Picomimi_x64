/**
 * Picomimi-x64 Scheduler Header
 * 
 * O(1) Preemptive Priority Bitmap Scheduler
 * Ported from Picomimi-AxisOS architecture
 * 
 * Features:
 * - O(1) task selection using priority bitmaps
 * - Priority aging to prevent starvation
 * - Preemption support for high-priority tasks
 * - Real-time task support with separate queue
 * - SMP scheduling with work stealing
 * - Linux-compatible scheduling policies
 */
#ifndef _SCHED_SCHED_H
#define _SCHED_SCHED_H

#include <kernel/types.h>

// ============================================================================
// SCHEDULER CONFIGURATION (from Picomimi)
// ============================================================================

#define SCHED_AGING_INTERVAL_MS     300     // How often to check for aging
#define SCHED_STARVATION_THRESHOLD_MS 1000  // Boost priority after this wait
#define SCHED_IDLE_INJECTION_THRESHOLD 95   // Inject idle when CPU > this %
#define SCHED_WORK_STEAL_THRESHOLD  3       // Steal when queue > N tasks
#define SCHED_DEFAULT_TIMESLICE_MS  10      // Default time slice

// ============================================================================
// PRIORITY BITMAP STRUCTURE (O(1) lookup)
// ============================================================================

typedef struct {
    u64 level_mask;                         // Which priority levels have tasks
    u64 task_masks[SCHED_PRIORITY_LEVELS];  // Tasks at each priority level
} priority_bitmap_t;

// ============================================================================
// RUN QUEUE (per-CPU)
// ============================================================================

typedef struct runqueue {
    // O(1) Bitmaps
    priority_bitmap_t active;       // Active tasks
    priority_bitmap_t expired;      // Expired tasks (need timeslice refresh)

    // Current task
    struct task_struct *curr;
    struct task_struct *idle;
    
    // Statistics
    u64 nr_running;
    u64 nr_switches;
    u64 nr_preemptions;
    u64 last_schedule_time;
    u64 total_runtime;
    u64 idle_time;
    
    // CPU load tracking
    u32 cpu_load;
    u32 cpu_load_avg;
    
    // Lock
    spinlock_t lock;
    
    // CPU ID
    u32 cpu_id;
} runqueue_t;

// ============================================================================
// TASK STRUCTURE (Linux-compatible naming)
// ============================================================================

typedef struct task_struct {
    // Identification
    pid_t pid;
    pid_t tgid;                     // Thread group ID (process ID)
    char comm[16];                  // Command name
    
    // State
    volatile task_state_t state;
    u32 flags;
    
    // Scheduling
    sched_policy_t policy;
    s32 prio;                       // Dynamic priority
    s32 static_prio;                // Nice-based priority
    s32 normal_prio;                // Priority without boost
    s32 rt_priority;                // Real-time priority (0-99)
    u64 timeslice;                  // Remaining timeslice (ns)
    u64 runtime;                    // Total runtime (ns)
    u64 vruntime;                   // Virtual runtime (for CFS-like fairness)
    u64 last_run;                   // Last run timestamp
    u64 sleep_start;                // Sleep start timestamp
    
    // CPU affinity
    cpumask_t cpus_allowed;
    u32 on_cpu;                     // Currently running on CPU
    u32 last_cpu;                   // Last CPU ran on
    
    // Context
    struct {
        u64 rsp;                    // Stack pointer
        u64 rip;                    // Instruction pointer
        u64 rflags;                 // Flags
        u64 cr3;                    // Page table
        // Saved registers
        u64 rbx, rbp, r12, r13, r14, r15;
    } context;
    
    // Stack
    void *kernel_stack;
    size_t stack_size;
    
    // Memory
    struct mm_struct *mm;           // Memory descriptor
    struct mm_struct *active_mm;    // Active mm (for kernel threads)
    
    // File system
    struct fs_struct *fs;
    struct files_struct *files;
    
    // Signals
    sigset_t blocked;
    sigset_t pending;
    
    // Parent/child relationships
    struct task_struct *parent;
    struct list_head children;
    struct list_head sibling;
    
    // Scheduler list
    struct list_head run_list;
    
    // Exit status
    s32 exit_code;
    s32 exit_signal;
    
    // Timestamps
    u64 start_time;                 // Process start time
    u64 utime;                      // User CPU time
    u64 stime;                      // System CPU time
    
    // Credentials
    uid_t uid, euid, suid;
    gid_t gid, egid, sgid;
    
    // Thread info
    u32 preempt_count;              // Preemption disable counter
    u32 need_resched;               // Reschedule flag
} task_struct_t;

// ============================================================================
// TASK FLAGS
// ============================================================================

#define PF_STARTING     0x00000002  // Being created
#define PF_EXITING      0x00000004  // Getting shut down
#define PF_KTHREAD      0x00200000  // Kernel thread
#define PF_IDLE         0x00000100  // Idle task
#define PF_WQ_WORKER    0x00000020  // Workqueue worker

// ============================================================================
// SCHEDULER FUNCTIONS
// ============================================================================

// Initialization
void sched_init(void);
void sched_init_cpu(u32 cpu_id);

// Task management
task_struct_t *task_create(const char *name, void (*entry)(void *), void *arg, u32 flags);
void task_destroy(task_struct_t *task);
void task_exit(s32 exit_code);

// Scheduler operations
void schedule(void);
void schedule_tail(void);
void sched_yield(void);
void sched_setscheduler(task_struct_t *task, sched_policy_t policy, s32 priority);

// Wake/sleep
void wake_up_task(task_struct_t *task);
void sleep_task(void);
void sleep_task_timeout(u64 timeout_ns);

// Preemption control
void preempt_disable(void);
void preempt_enable(void);
bool preempt_needed(void);

// CPU load
u32 sched_get_cpu_load(u32 cpu_id);
void sched_update_load(void);

// Work stealing (SMP)
bool sched_try_steal_work(u32 from_cpu);

// Current task
task_struct_t *get_current(void);
#define current get_current()

// Bitmap operations (from Picomimi)
static inline int bitmap_ffs(u64 bitmap) {
    if (bitmap == 0) return 0;
    return __builtin_ffsll(bitmap);
}

static inline int bitmap_fls(u64 bitmap) {
    if (bitmap == 0) return 0;
    return 64 - __builtin_clzll(bitmap);
}

static inline void bitmap_set(u64 *bitmap, u32 bit) {
    if (bit < 64) *bitmap |= (1UL << bit);
}

static inline void bitmap_clear(u64 *bitmap, u32 bit) {
    if (bit < 64) *bitmap &= ~(1UL << bit);
}

static inline bool bitmap_test(u64 bitmap, u32 bit) {
    return (bit < 64) && (bitmap & (1UL << bit));
}

// ============================================================================
// SYSCALL INTERFACE
// ============================================================================

void syscall_init(void);

// Linux-compatible syscall numbers
#define __NR_read           0
#define __NR_write          1
#define __NR_open           2
#define __NR_close          3
#define __NR_stat           4
#define __NR_fstat          5
#define __NR_lstat          6
#define __NR_poll           7
#define __NR_lseek          8
#define __NR_mmap           9
#define __NR_mprotect       10
#define __NR_munmap         11
#define __NR_brk            12
#define __NR_rt_sigaction   13
#define __NR_rt_sigprocmask 14
#define __NR_rt_sigreturn   15
#define __NR_ioctl          16
#define __NR_pread64        17
#define __NR_pwrite64       18
#define __NR_readv          19
#define __NR_writev         20
#define __NR_access         21
#define __NR_pipe           22
#define __NR_select         23
#define __NR_sched_yield    24
#define __NR_mremap         25
#define __NR_msync          26
#define __NR_mincore        27
#define __NR_madvise        28
#define __NR_dup            32
#define __NR_dup2           33
#define __NR_pause          34
#define __NR_nanosleep      35
#define __NR_getitimer      36
#define __NR_alarm          37
#define __NR_setitimer      38
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_vfork          58
#define __NR_execve         59
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_uname          63
#define __NR_getuid         102
#define __NR_getgid         104
#define __NR_setuid         105
#define __NR_setgid         106
#define __NR_geteuid        107
#define __NR_getegid        108
#define __NR_getppid        110
#define __NR_getpgrp        111
#define __NR_setsid         112
#define __NR_gettid         186
#define __NR_clone          56
#define __NR_exit_group     231
#define __NR_clock_gettime  228
#define __NR_clock_nanosleep 230

#define NR_SYSCALLS         512

#endif // _SCHED_SCHED_H
