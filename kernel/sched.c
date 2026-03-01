/**
 * Picomimi-x64 O(1) Scheduler
 * 
 * Based on Picomimi-AxisOS priority bitmap scheduler
 * Ported to x86_64 with SMP support
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>

// ============================================================================
// SCHEDULER CONSTANTS
// ============================================================================

#define SCHED_BITMAP_SIZE       8       // 8 * 64 = 512 priority levels (we use 140)
#define SCHED_TIMESLICE_BASE    100     // Base timeslice in ms
#define SCHED_TIMESLICE_MIN     10      // Minimum timeslice
#define SCHED_TIMESLICE_MAX     200     // Maximum timeslice

// Priority to timeslice mapping
#define PRIO_TO_TIMESLICE(p)    (SCHED_TIMESLICE_BASE + (120 - (p)))

// ============================================================================
// RUN QUEUE STRUCTURE
// ============================================================================

// Per-priority queue
typedef struct prio_queue {
    struct list_head    tasks;          // List of tasks at this priority
    int                 count;          // Number of tasks
} prio_queue_t;

// Per-CPU run queue
typedef struct runqueue {
    spinlock_t          lock;
    
    // O(1) scheduling structures
    u64                 bitmap[SCHED_BITMAP_SIZE];  // Priority bitmap
    prio_queue_t        queues[MAX_PRIO];           // Priority queues (140 levels)
    
    // Active/expired arrays for round-robin within priority
    prio_queue_t        *active;
    prio_queue_t        *expired;
    prio_queue_t        arrays[2][MAX_PRIO];
    u64                 active_bitmap[SCHED_BITMAP_SIZE];
    u64                 expired_bitmap[SCHED_BITMAP_SIZE];
    
    // Current task
    task_struct_t       *curr;
    task_struct_t       *idle;
    
    // Statistics
    u64                 nr_running;
    u64                 nr_switches;
    u64                 nr_migrations;
    
    // CPU info
    int                 cpu;
    
    // Load balancing
    u64                 load;
    u64                 calc_load_update;
    
    // Tick accounting
    u64                 clock;
    u64                 clock_task;
    
} runqueue_t;

// Per-CPU run queues
static runqueue_t runqueues[NR_CPUS];

// ============================================================================
// BITMAP OPERATIONS
// ============================================================================

static inline int find_first_set_bit(u64 bitmap[SCHED_BITMAP_SIZE]) {
    for (int i = 0; i < SCHED_BITMAP_SIZE; i++) {
        if (bitmap[i]) {
            return i * 64 + __builtin_ctzll(bitmap[i]);
        }
    }
    return -1;  // No bits set
}

static inline void set_prio_bit(u64 bitmap[SCHED_BITMAP_SIZE], int prio) {
    bitmap[prio / 64] |= (1ULL << (prio % 64));
}

static inline void clear_prio_bit(u64 bitmap[SCHED_BITMAP_SIZE], int prio) {
    bitmap[prio / 64] &= ~(1ULL << (prio % 64));
}

static inline int test_prio_bit(u64 bitmap[SCHED_BITMAP_SIZE], int prio) {
    return (bitmap[prio / 64] >> (prio % 64)) & 1;
}

// ============================================================================
// PRIORITY CALCULATION
// ============================================================================

// Calculate effective priority based on nice value and CPU usage
static int effective_prio(task_struct_t *task) {
    int prio = task->static_prio;
    
    // Real-time tasks get their RT priority
    if (task->policy == SCHED_FIFO || task->policy == SCHED_RR) {
        return MAX_RT_PRIO - 1 - task->rt_priority;
    }
    
    // Normal tasks: static priority + dynamic bonus/penalty
    // Based on sleep/CPU ratio
    // TODO: Implement interactivity bonus
    
    return prio;
}

// Calculate time slice based on priority
static u64 calculate_timeslice(task_struct_t *task) {
    int prio = task->prio;
    u64 timeslice;
    
    if (prio < MAX_RT_PRIO) {
        // RT tasks get longer slices
        timeslice = SCHED_TIMESLICE_MAX;
    } else {
        // Nice-based timeslice
        int nice = prio - 120;  // Nice 0 = prio 120
        if (nice >= 0) {
            timeslice = SCHED_TIMESLICE_BASE - (nice * 5);
        } else {
            timeslice = SCHED_TIMESLICE_BASE - (nice * 5);
        }
    }
    
    if (timeslice < SCHED_TIMESLICE_MIN) timeslice = SCHED_TIMESLICE_MIN;
    if (timeslice > SCHED_TIMESLICE_MAX) timeslice = SCHED_TIMESLICE_MAX;
    
    return timeslice;
}

// ============================================================================
// ENQUEUE / DEQUEUE
// ============================================================================

// External spinlock functions
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);
extern void spin_lock_init(spinlock_t *lock);

static void enqueue_task(runqueue_t *rq, task_struct_t *task) {
    int prio = task->prio;
    prio_queue_t *queue = &rq->active[prio];
    
    list_add_tail(&task->run_list, &queue->tasks);
    queue->count++;
    
    set_prio_bit(rq->active_bitmap, prio);
    
    rq->nr_running++;
    task->on_rq = 1;
    
    // Update load
    rq->load += (MAX_PRIO - prio);
}

static void dequeue_task(runqueue_t *rq, task_struct_t *task) {
    int prio = task->prio;
    prio_queue_t *queue = &rq->active[prio];
    
    list_del(&task->run_list);
    queue->count--;
    
    if (queue->count == 0) {
        clear_prio_bit(rq->active_bitmap, prio);
    }
    
    rq->nr_running--;
    task->on_rq = 0;
    
    // Update load
    rq->load -= (MAX_PRIO - prio);
}

// Move task to expired array
static void expire_task(runqueue_t *rq, task_struct_t *task) {
    int prio = task->prio;
    prio_queue_t *exp_queue = &rq->expired[prio];
    
    // Remove from active
    dequeue_task(rq, task);
    
    // Add to expired
    list_add_tail(&task->run_list, &exp_queue->tasks);
    exp_queue->count++;
    set_prio_bit(rq->expired_bitmap, prio);
    
    rq->nr_running++;
    task->on_rq = 1;
    
    // Recalculate timeslice
    task->time_slice = calculate_timeslice(task);
}

// Swap active and expired arrays
static void swap_arrays(runqueue_t *rq) {
    prio_queue_t *tmp = rq->active;
    rq->active = rq->expired;
    rq->expired = tmp;
    
    // Swap bitmaps
    for (int i = 0; i < SCHED_BITMAP_SIZE; i++) {
        u64 t = rq->active_bitmap[i];
        rq->active_bitmap[i] = rq->expired_bitmap[i];
        rq->expired_bitmap[i] = t;
    }
}

// ============================================================================
// SCHEDULER CORE
// ============================================================================

// Pick next task to run (O(1) complexity)
static task_struct_t *pick_next_task(runqueue_t *rq) {
    // Find highest priority with runnable tasks
    int prio = find_first_set_bit(rq->active_bitmap);
    
    if (prio < 0) {
        // No active tasks - swap arrays if expired has tasks
        prio = find_first_set_bit(rq->expired_bitmap);
        if (prio >= 0) {
            swap_arrays(rq);
            prio = find_first_set_bit(rq->active_bitmap);
        }
    }
    
    if (prio < 0) {
        return rq->idle;
    }
    
    // Get first task from this priority queue
    prio_queue_t *queue = &rq->active[prio];
    if (list_empty(&queue->tasks)) {
        return rq->idle;
    }
    
    task_struct_t *task = list_first_entry(&queue->tasks, task_struct_t, run_list);
    return task;
}

// Main scheduler function
void schedule_impl(void) {
    int cpu = smp_processor_id();
    runqueue_t *rq = &runqueues[cpu];
    task_struct_t *prev = rq->curr;
    task_struct_t *next;
    
    spin_lock(&rq->lock);
    
    // Handle previous task
    if (prev && prev != rq->idle) {
        if (prev->state == TASK_RUNNING) {
            // Timeslice expired - move to expired array
            if (prev->time_slice <= 0) {
                expire_task(rq, prev);
            }
        } else {
            // Task is sleeping/blocked - remove from run queue
            if (prev->on_rq) {
                dequeue_task(rq, prev);
            }
        }
    }
    
    // Pick next task
    next = pick_next_task(rq);
    
    rq->nr_switches++;
    rq->clock++;
    
    if (next != prev) {
        rq->curr = next;
        spin_unlock(&rq->lock);
        
        // Context switch
        if (prev) {
            context_switch(&prev->context, &next->context);
        }
    } else {
        spin_unlock(&rq->lock);
    }
}

// Timer tick handler
void scheduler_tick(void) {
    int cpu = smp_processor_id();
    runqueue_t *rq = &runqueues[cpu];
    task_struct_t *curr = rq->curr;
    
    if (!curr || curr == rq->idle) {
        return;
    }
    
    // Decrement timeslice
    if (curr->time_slice > 0) {
        curr->time_slice--;
    }
    
    // Update runtime
    curr->sum_exec_runtime++;
    
    // Need reschedule?
    if (curr->time_slice <= 0) {
        // Request reschedule
        // set_tsk_need_resched(curr);
    }
}

// ============================================================================
// TASK WAKEUP
// ============================================================================

void sched_wake_up(task_struct_t *task) {
    if (!task) return;
    
    int cpu = task->cpu;
    runqueue_t *rq = &runqueues[cpu];
    
    spin_lock(&rq->lock);
    
    if (task->state != TASK_RUNNING) {
        task->state = TASK_RUNNING;
        task->prio = effective_prio(task);
        task->time_slice = calculate_timeslice(task);
        
        enqueue_task(rq, task);
    }
    
    spin_unlock(&rq->lock);
}

// ============================================================================
// SLEEP/BLOCK
// ============================================================================

void sched_sleep(long state) {
    int cpu = smp_processor_id();
    runqueue_t *rq = &runqueues[cpu];
    task_struct_t *task = rq->curr;
    
    spin_lock(&rq->lock);
    task->state = state;
    spin_unlock(&rq->lock);
    
    schedule_impl();
}

// ============================================================================
// YIELD
// ============================================================================

void sched_yield_impl(void) {
    int cpu = smp_processor_id();
    runqueue_t *rq = &runqueues[cpu];
    task_struct_t *task = rq->curr;
    
    if (task) {
        task->time_slice = 0;
    }
    
    schedule_impl();
}

// ============================================================================
// SETPRIORITY / NICE
// ============================================================================

int sched_setscheduler(task_struct_t *task, int policy, int prio) {
    if (!task) return -EINVAL;
    
    int cpu = task->cpu;
    runqueue_t *rq = &runqueues[cpu];
    
    spin_lock(&rq->lock);
    
    bool on_rq = task->on_rq;
    if (on_rq) {
        dequeue_task(rq, task);
    }
    
    task->policy = policy;
    
    switch (policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        task->rt_priority = prio;
        task->prio = MAX_RT_PRIO - 1 - prio;
        break;
    case SCHED_NORMAL:
    case SCHED_BATCH:
    case SCHED_IDLE:
        task->static_prio = 120 + prio;  // prio is nice value (-20 to 19)
        task->prio = effective_prio(task);
        break;
    default:
        spin_unlock(&rq->lock);
        return -EINVAL;
    }
    
    task->time_slice = calculate_timeslice(task);
    
    if (on_rq) {
        enqueue_task(rq, task);
    }
    
    spin_unlock(&rq->lock);
    return 0;
}

int sched_set_nice(task_struct_t *task, int nice) {
    if (nice < MIN_NICE) nice = MIN_NICE;
    if (nice > MAX_NICE) nice = MAX_NICE;
    
    return sched_setscheduler(task, task->policy, nice);
}

// ============================================================================
// LOAD BALANCING (for SMP)
// ============================================================================

static runqueue_t *find_busiest_queue(int this_cpu) {
    runqueue_t *busiest = NULL;
    u64 max_load = 0;
    
    for (int cpu = 0; cpu < NR_CPUS; cpu++) {
        if (cpu == this_cpu) continue;
        if (!runqueues[cpu].idle) continue;  // CPU not online
        
        if (runqueues[cpu].load > max_load) {
            max_load = runqueues[cpu].load;
            busiest = &runqueues[cpu];
        }
    }
    
    return busiest;
}

void sched_load_balance(void) {
    int this_cpu = smp_processor_id();
    runqueue_t *this_rq = &runqueues[this_cpu];
    runqueue_t *busiest = find_busiest_queue(this_cpu);
    
    if (!busiest) return;
    
    // Calculate imbalance
    u64 imbalance = (busiest->load - this_rq->load) / 2;
    if (imbalance < (MAX_PRIO - 120)) {
        return;  // Not enough imbalance
    }
    
    // TODO: Actually migrate tasks
    // This requires careful locking and is complex
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void sched_init_rq(int cpu) {
    runqueue_t *rq = &runqueues[cpu];
    
    memset(rq, 0, sizeof(runqueue_t));
    spin_lock_init(&rq->lock);
    
    // Initialize priority queues
    for (int i = 0; i < MAX_PRIO; i++) {
        INIT_LIST_HEAD(&rq->arrays[0][i].tasks);
        INIT_LIST_HEAD(&rq->arrays[1][i].tasks);
        rq->arrays[0][i].count = 0;
        rq->arrays[1][i].count = 0;
    }
    
    rq->active = rq->arrays[0];
    rq->expired = rq->arrays[1];
    
    rq->cpu = cpu;
    rq->nr_running = 0;
    rq->nr_switches = 0;
    
    // Idle task will be set later
    rq->idle = NULL;
    rq->curr = NULL;
}

void sched_set_idle(int cpu, task_struct_t *idle) {
    runqueues[cpu].idle = idle;
    runqueues[cpu].curr = idle;
}

void o1_sched_init(void) {
    printk(KERN_INFO "Initializing O(1) scheduler...\n");
    
    for (int cpu = 0; cpu < NR_CPUS; cpu++) {
        sched_init_rq(cpu);
    }
    
    printk(KERN_INFO "  O(1) scheduler initialized (%d priority levels)\n", MAX_PRIO);
}

// ============================================================================
// DEBUG / STATS
// ============================================================================

void sched_print_stats(void) {
    for (int cpu = 0; cpu < 1; cpu++) {  // Just BSP for now
        runqueue_t *rq = &runqueues[cpu];
        
        printk(KERN_INFO "CPU %d: running=%lu switches=%lu load=%lu\n",
               cpu, rq->nr_running, rq->nr_switches, rq->load);
    }
}

// Count runnable tasks at each priority level
void sched_print_prio_stats(void) {
    runqueue_t *rq = &runqueues[0];
    
    printk(KERN_INFO "Priority distribution:\n");
    for (int p = 0; p < MAX_PRIO; p++) {
        int active = rq->active[p].count;
        int expired = rq->expired[p].count;
        if (active || expired) {
            printk(KERN_INFO "  Prio %d: active=%d expired=%d\n", 
                   p, active, expired);
        }
    }
}
