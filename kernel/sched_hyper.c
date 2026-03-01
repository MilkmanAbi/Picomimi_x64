/**
 * Picomimi-x64 Scheduler Hypervisor
 * 
 * A revolutionary hierarchical scheduler that allows multiple scheduling
 * policies to coexist. The main preemptive scheduler manages "scheduler
 * domains", and each domain can have its own sub-scheduler (cooperative,
 * real-time, batch, idle, etc.)
 * 
 * Architecture:
 * ┌─────────────────────────────────────────────────────────┐
 * │              Main Preemptive Scheduler                   │
 * │  (O(1) bitmap, handles domain selection & preemption)   │
 * └────────┬──────────┬──────────┬──────────┬───────────────┘
 *          │          │          │          │
 *     ┌────▼────┐┌────▼────┐┌────▼────┐┌────▼────┐
 *     │  COOP   ││REALTIME ││  BATCH  ││  IDLE   │
 *     │ Domain  ││ Domain  ││ Domain  ││ Domain  │
 *     │(fibers) ││(deadline)││(low pri)││(bg jobs)│
 *     └─────────┘└─────────┘└─────────┘└─────────┘
 * 
 * Use cases:
 * - Fiber/coroutine systems (cooperative under preemptive)
 * - Real-time tasks with deadline scheduling
 * - Batch processing with fair-share
 * - Background/idle tasks
 * - Custom game engine schedulers
 * - Database query schedulers
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// ============================================================================
// SCHEDULER HYPERVISOR TYPES
// ============================================================================

// Maximum scheduler domains
#define SCHED_MAX_DOMAINS       16
#define SCHED_MAX_CLASSES       8

// Scheduler class types (sub-schedulers)
typedef enum {
    SCHED_CLASS_PREEMPTIVE  = 0,    // Standard preemptive (default)
    SCHED_CLASS_COOPERATIVE = 1,    // Cooperative/fiber scheduling
    SCHED_CLASS_REALTIME    = 2,    // Deadline-based real-time
    SCHED_CLASS_BATCH       = 3,    // Batch/throughput-oriented  
    SCHED_CLASS_IDLE        = 4,    // Idle/background tasks
    SCHED_CLASS_FAIR        = 5,    // CFS-like fair scheduler
    SCHED_CLASS_CUSTOM      = 6,    // User-defined scheduler
    SCHED_CLASS_COUNT
} sched_class_type_t;

// Forward declarations
struct sched_domain;
struct sched_class;
struct sched_entity;

// ============================================================================
// SCHEDULER ENTITY (task wrapper for hierarchical scheduling)
// ============================================================================

typedef struct sched_entity {
    // Linkage
    struct list_head        run_list;       // In domain's run queue
    struct sched_domain     *domain;        // Parent domain
    task_struct_t           *task;          // Actual task
    
    // Scheduling parameters
    u64                     vruntime;       // Virtual runtime (for fair sched)
    u64                     deadline;       // Absolute deadline (for RT)
    u64                     period;         // Period (for periodic RT)
    u64                     exec_start;     // Last execution start time
    u64                     sum_exec;       // Total execution time
    s64                     time_slice;     // Remaining time slice
    
    // Cooperative scheduling
    int                     yield_count;    // Voluntary yields
    int                     coop_priority;  // Cooperative priority
    void                    (*coop_entry)(void *);  // Coroutine entry
    void                    *coop_arg;      // Coroutine argument
    void                    *coop_stack;    // Coroutine stack
    u64                     coop_sp;        // Saved stack pointer
    
    // Statistics
    u64                     wait_start;     // Start of wait time
    u64                     wait_sum;       // Total wait time
    u64                     switches;       // Context switches
    
    // State
    int                     on_rq;          // On run queue?
    int                     blocked;        // Blocked?
} sched_entity_t;

// ============================================================================
// SCHEDULER CLASS (pluggable scheduling policy)
// ============================================================================

typedef struct sched_class {
    const char              *name;
    sched_class_type_t      type;
    
    // Class operations
    void    (*init)(struct sched_domain *domain);
    void    (*destroy)(struct sched_domain *domain);
    
    // Entity operations
    void    (*enqueue)(struct sched_domain *domain, sched_entity_t *se);
    void    (*dequeue)(struct sched_domain *domain, sched_entity_t *se);
    void    (*yield)(struct sched_domain *domain, sched_entity_t *se);
    void    (*tick)(struct sched_domain *domain, sched_entity_t *curr);
    
    // Selection
    sched_entity_t *(*pick_next)(struct sched_domain *domain);
    
    // Priority
    void    (*set_priority)(sched_entity_t *se, int priority);
    
    // Custom hook for cooperative schedulers
    int     (*should_preempt)(struct sched_domain *domain, sched_entity_t *curr);
    
    // Linked list
    struct sched_class *next;
} sched_class_t;

// ============================================================================
// SCHEDULER DOMAIN
// ============================================================================

typedef struct sched_domain {
    // Identity
    u32                     id;
    char                    name[32];
    sched_class_t           *sclass;        // Scheduling class
    
    // Run queue
    struct list_head        runqueue;       // Entities ready to run
    u32                     nr_running;     // Number of runnable entities
    sched_entity_t          *curr;          // Currently running entity
    
    // Domain scheduling parameters
    u64                     weight;         // Weight for parent scheduler
    u64                     vruntime;       // Virtual runtime of domain
    int                     priority;       // Domain priority (for parent)
    u64                     timeslice;      // Time allocated to domain
    u64                     timeslice_remaining;
    
    // Statistics
    u64                     total_runtime;
    u64                     switches;
    u64                     preemptions;
    
    // Hierarchical
    struct sched_domain     *parent;        // Parent domain (NULL = root)
    struct list_head        children;       // Child domains
    struct list_head        sibling;        // Sibling list linkage
    
    // Lock
    spinlock_t              lock;
    
    // Private data for scheduler class
    void                    *class_data;
    
    // Active?
    int                     active;
} sched_domain_t;

// ============================================================================
// SCHEDULER HYPERVISOR
// ============================================================================

typedef struct sched_hypervisor {
    // Registered scheduler classes
    sched_class_t           *classes[SCHED_CLASS_COUNT];
    
    // All domains
    sched_domain_t          *domains[SCHED_MAX_DOMAINS];
    u32                     nr_domains;
    
    // Root domain (the main preemptive scheduler)
    sched_domain_t          *root;
    
    // Current domain per CPU
    sched_domain_t          *current_domain[NR_CPUS];
    
    // Global lock
    spinlock_t              lock;
    
    // Statistics
    u64                     domain_switches;
    u64                     total_ticks;
    
    // Time tracking
    u64                     now;            // Current time (ticks)
} sched_hypervisor_t;

// Global hypervisor instance
static sched_hypervisor_t hypervisor;

// External functions
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);
extern void spin_lock_init(spinlock_t *lock);
extern u32 smp_processor_id(void);
extern task_struct_t *current_tasks[NR_CPUS];

// Forward declarations
void sched_hypervisor_schedule(void);

// ============================================================================
// TIME TRACKING
// ============================================================================

static u64 sched_clock(void) {
    return hypervisor.now;
}

// ============================================================================
// COOPERATIVE SCHEDULER CLASS
// ============================================================================

typedef struct coop_domain_data {
    struct list_head        ready_queue;    // Ready fibers
    struct list_head        blocked_queue;  // Blocked fibers
    sched_entity_t          *running;       // Currently running fiber
    u64                     quantum;        // Max time before forced yield
} coop_domain_data_t;

static void coop_init(sched_domain_t *domain) {
    coop_domain_data_t *data = kmalloc(sizeof(*data), GFP_KERNEL);
    INIT_LIST_HEAD(&data->ready_queue);
    INIT_LIST_HEAD(&data->blocked_queue);
    data->running = NULL;
    data->quantum = 1000;  // 1000 ticks max before hint to yield
    domain->class_data = data;
    printk(KERN_INFO "  COOP: Domain '%s' initialized\n", domain->name);
}

static void coop_destroy(sched_domain_t *domain) {
    kfree(domain->class_data);
}

static void coop_enqueue(sched_domain_t *domain, sched_entity_t *se) {
    coop_domain_data_t *data = domain->class_data;
    
    // Add to end of ready queue (FIFO for cooperative)
    list_add_tail(&se->run_list, &data->ready_queue);
    se->on_rq = 1;
    domain->nr_running++;
}

static void coop_dequeue(sched_domain_t *domain, sched_entity_t *se) {
    coop_domain_data_t *data = domain->class_data;
    (void)data;
    
    list_del(&se->run_list);
    se->on_rq = 0;
    domain->nr_running--;
}

static void coop_yield(sched_domain_t *domain, sched_entity_t *se) {
    coop_domain_data_t *data = domain->class_data;
    
    // Move to end of queue
    if (se->on_rq) {
        list_del(&se->run_list);
        list_add_tail(&se->run_list, &data->ready_queue);
    }
    se->yield_count++;
}

static sched_entity_t *coop_pick_next(sched_domain_t *domain) {
    coop_domain_data_t *data = domain->class_data;
    
    if (list_empty(&data->ready_queue)) {
        return NULL;
    }
    
    // Take first from queue (cooperative = voluntary switching)
    sched_entity_t *next = list_first_entry(&data->ready_queue, 
                                             sched_entity_t, run_list);
    return next;
}

static void coop_tick(sched_domain_t *domain, sched_entity_t *curr) {
    coop_domain_data_t *data = domain->class_data;
    (void)data;
    
    if (!curr) return;
    
    curr->sum_exec++;
    
    // Cooperative scheduler doesn't preempt, but we track runtime
    // for debugging and potential soft warnings
}

static int coop_should_preempt(sched_domain_t *domain, sched_entity_t *curr) {
    // Cooperative scheduler never preempts!
    (void)domain;
    (void)curr;
    return 0;
}

static sched_class_t coop_class = {
    .name           = "cooperative",
    .type           = SCHED_CLASS_COOPERATIVE,
    .init           = coop_init,
    .destroy        = coop_destroy,
    .enqueue        = coop_enqueue,
    .dequeue        = coop_dequeue,
    .yield          = coop_yield,
    .pick_next      = coop_pick_next,
    .tick           = coop_tick,
    .should_preempt = coop_should_preempt,
};

// ============================================================================
// REALTIME (DEADLINE) SCHEDULER CLASS
// ============================================================================

typedef struct rt_domain_data {
    struct list_head        deadline_queue; // Sorted by deadline
    u64                     earliest_deadline;
} rt_domain_data_t;

static void rt_init(sched_domain_t *domain) {
    rt_domain_data_t *data = kmalloc(sizeof(*data), GFP_KERNEL);
    INIT_LIST_HEAD(&data->deadline_queue);
    data->earliest_deadline = ~0ULL;
    domain->class_data = data;
    printk(KERN_INFO "  RT: Domain '%s' initialized (deadline scheduler)\n", domain->name);
}

static void rt_destroy(sched_domain_t *domain) {
    kfree(domain->class_data);
}

// Insert sorted by deadline (earliest first)
static void rt_enqueue(sched_domain_t *domain, sched_entity_t *se) {
    rt_domain_data_t *data = domain->class_data;
    struct list_head *pos;
    
    // Find insertion point
    list_for_each(pos, &data->deadline_queue) {
        sched_entity_t *entry = list_entry(pos, sched_entity_t, run_list);
        if (se->deadline < entry->deadline) {
            break;
        }
    }
    
    list_add_tail(&se->run_list, pos);
    se->on_rq = 1;
    domain->nr_running++;
    
    // Update earliest deadline
    if (se->deadline < data->earliest_deadline) {
        data->earliest_deadline = se->deadline;
    }
}

static void rt_dequeue(sched_domain_t *domain, sched_entity_t *se) {
    rt_domain_data_t *data = domain->class_data;
    
    list_del(&se->run_list);
    se->on_rq = 0;
    domain->nr_running--;
    
    // Recalculate earliest deadline
    if (list_empty(&data->deadline_queue)) {
        data->earliest_deadline = ~0ULL;
    } else {
        sched_entity_t *first = list_first_entry(&data->deadline_queue,
                                                  sched_entity_t, run_list);
        data->earliest_deadline = first->deadline;
    }
}

static sched_entity_t *rt_pick_next(sched_domain_t *domain) {
    rt_domain_data_t *data = domain->class_data;
    
    if (list_empty(&data->deadline_queue)) {
        return NULL;
    }
    
    // Earliest Deadline First (EDF)
    return list_first_entry(&data->deadline_queue, sched_entity_t, run_list);
}

static void rt_tick(sched_domain_t *domain, sched_entity_t *curr) {
    rt_domain_data_t *data = domain->class_data;
    
    if (!curr) return;
    
    curr->sum_exec++;
    
    // Check for deadline miss
    u64 now = sched_clock();
    if (now > curr->deadline) {
        printk(KERN_WARNING "RT: Task missed deadline! (task=%s, deadline=%lu, now=%lu)\n",
               curr->task ? curr->task->comm : "?", curr->deadline, now);
    }
    
    // Check if a higher priority (earlier deadline) task arrived
    if (!list_empty(&data->deadline_queue)) {
        sched_entity_t *first = list_first_entry(&data->deadline_queue,
                                                  sched_entity_t, run_list);
        if (first != curr && first->deadline < curr->deadline) {
            // Preemption needed
            domain->preemptions++;
        }
    }
}

static int rt_should_preempt(sched_domain_t *domain, sched_entity_t *curr) {
    rt_domain_data_t *data = domain->class_data;
    
    if (!curr || list_empty(&data->deadline_queue)) {
        return 0;
    }
    
    sched_entity_t *first = list_first_entry(&data->deadline_queue,
                                              sched_entity_t, run_list);
    
    // Preempt if earlier deadline arrived
    return (first != curr && first->deadline < curr->deadline);
}

static sched_class_t rt_class = {
    .name           = "realtime",
    .type           = SCHED_CLASS_REALTIME,
    .init           = rt_init,
    .destroy        = rt_destroy,
    .enqueue        = rt_enqueue,
    .dequeue        = rt_dequeue,
    .yield          = coop_yield,  // Reuse
    .pick_next      = rt_pick_next,
    .tick           = rt_tick,
    .should_preempt = rt_should_preempt,
};

// ============================================================================
// FAIR SCHEDULER CLASS (CFS-like)
// ============================================================================

typedef struct fair_domain_data {
    // Red-black tree would be ideal, but we use sorted list for simplicity
    struct list_head        timeline;       // Sorted by vruntime
    u64                     min_vruntime;   // Minimum vruntime
    u64                     period;         // Scheduling period
    u64                     granularity;    // Minimum granularity
} fair_domain_data_t;

#define FAIR_PERIOD         100     // 100 tick scheduling period
#define FAIR_GRANULARITY    10      // 10 tick minimum granularity
#define FAIR_WEIGHT_DEFAULT 1024    // Default weight

static void fair_init(sched_domain_t *domain) {
    fair_domain_data_t *data = kmalloc(sizeof(*data), GFP_KERNEL);
    INIT_LIST_HEAD(&data->timeline);
    data->min_vruntime = 0;
    data->period = FAIR_PERIOD;
    data->granularity = FAIR_GRANULARITY;
    domain->class_data = data;
    printk(KERN_INFO "  FAIR: Domain '%s' initialized (CFS-like)\n", domain->name);
}

static void fair_destroy(sched_domain_t *domain) {
    kfree(domain->class_data);
}

// Calculate time slice based on weight and number of tasks
static u64 fair_calc_slice(sched_domain_t *domain, sched_entity_t *se) {
    fair_domain_data_t *data = domain->class_data;
    (void)se;
    
    if (domain->nr_running == 0) return data->period;
    
    u64 slice = data->period / domain->nr_running;
    if (slice < data->granularity) {
        slice = data->granularity;
    }
    
    return slice;
}

// Insert sorted by vruntime
static void fair_enqueue(sched_domain_t *domain, sched_entity_t *se) {
    fair_domain_data_t *data = domain->class_data;
    struct list_head *pos;
    
    // Initialize vruntime if new
    if (se->vruntime == 0) {
        se->vruntime = data->min_vruntime;
    }
    
    // Find insertion point (sorted by vruntime)
    list_for_each(pos, &data->timeline) {
        sched_entity_t *entry = list_entry(pos, sched_entity_t, run_list);
        if (se->vruntime < entry->vruntime) {
            break;
        }
    }
    
    list_add_tail(&se->run_list, pos);
    se->on_rq = 1;
    domain->nr_running++;
    
    // Calculate time slice
    se->time_slice = fair_calc_slice(domain, se);
}

static void fair_dequeue(sched_domain_t *domain, sched_entity_t *se) {
    fair_domain_data_t *data = domain->class_data;
    
    list_del(&se->run_list);
    se->on_rq = 0;
    domain->nr_running--;
    
    // Update min_vruntime
    if (!list_empty(&data->timeline)) {
        sched_entity_t *first = list_first_entry(&data->timeline,
                                                  sched_entity_t, run_list);
        if (first->vruntime > data->min_vruntime) {
            data->min_vruntime = first->vruntime;
        }
    }
}

static sched_entity_t *fair_pick_next(sched_domain_t *domain) {
    fair_domain_data_t *data = domain->class_data;
    
    if (list_empty(&data->timeline)) {
        return NULL;
    }
    
    // Pick leftmost (smallest vruntime)
    return list_first_entry(&data->timeline, sched_entity_t, run_list);
}

static void fair_tick(sched_domain_t *domain, sched_entity_t *curr) {
    fair_domain_data_t *data = domain->class_data;
    
    if (!curr) return;
    
    // Update vruntime (weighted by inverse of weight - lower weight = faster vruntime)
    curr->vruntime++;
    curr->sum_exec++;
    curr->time_slice--;
    
    // Update min_vruntime
    if (curr->vruntime > data->min_vruntime) {
        // Only update if current is still the minimum
        if (!list_empty(&data->timeline)) {
            sched_entity_t *first = list_first_entry(&data->timeline,
                                                      sched_entity_t, run_list);
            data->min_vruntime = first->vruntime;
        }
    }
}

static int fair_should_preempt(sched_domain_t *domain, sched_entity_t *curr) {
    fair_domain_data_t *data = domain->class_data;
    
    if (!curr) return 0;
    
    // Preempt if time slice exhausted
    if (curr->time_slice <= 0) {
        return 1;
    }
    
    // Or if there's a task with significantly lower vruntime
    if (!list_empty(&data->timeline)) {
        sched_entity_t *first = list_first_entry(&data->timeline,
                                                  sched_entity_t, run_list);
        if (first != curr && 
            curr->vruntime > first->vruntime + data->granularity) {
            return 1;
        }
    }
    
    return 0;
}

static sched_class_t fair_class = {
    .name           = "fair",
    .type           = SCHED_CLASS_FAIR,
    .init           = fair_init,
    .destroy        = fair_destroy,
    .enqueue        = fair_enqueue,
    .dequeue        = fair_dequeue,
    .yield          = coop_yield,
    .pick_next      = fair_pick_next,
    .tick           = fair_tick,
    .should_preempt = fair_should_preempt,
};

// ============================================================================
// BATCH SCHEDULER CLASS
// ============================================================================

static void batch_init(sched_domain_t *domain) {
    // Reuse fair scheduler data structures
    fair_init(domain);
    
    // But with longer periods for batch jobs
    fair_domain_data_t *data = domain->class_data;
    data->period = 1000;        // 1000 tick period
    data->granularity = 100;    // 100 tick granularity
    
    printk(KERN_INFO "  BATCH: Domain '%s' initialized\n", domain->name);
}

static sched_class_t batch_class = {
    .name           = "batch",
    .type           = SCHED_CLASS_BATCH,
    .init           = batch_init,
    .destroy        = fair_destroy,
    .enqueue        = fair_enqueue,
    .dequeue        = fair_dequeue,
    .yield          = coop_yield,
    .pick_next      = fair_pick_next,
    .tick           = fair_tick,
    .should_preempt = fair_should_preempt,
};

// ============================================================================
// IDLE SCHEDULER CLASS
// ============================================================================

static void idle_init(sched_domain_t *domain) {
    INIT_LIST_HEAD(&domain->runqueue);
    printk(KERN_INFO "  IDLE: Domain '%s' initialized\n", domain->name);
}

static void idle_enqueue(sched_domain_t *domain, sched_entity_t *se) {
    list_add_tail(&se->run_list, &domain->runqueue);
    se->on_rq = 1;
    domain->nr_running++;
}

static void idle_dequeue(sched_domain_t *domain, sched_entity_t *se) {
    list_del(&se->run_list);
    se->on_rq = 0;
    domain->nr_running--;
}

static sched_entity_t *idle_pick_next(sched_domain_t *domain) {
    if (list_empty(&domain->runqueue)) {
        return NULL;
    }
    return list_first_entry(&domain->runqueue, sched_entity_t, run_list);
}

static void idle_tick(sched_domain_t *domain, sched_entity_t *curr) {
    if (curr) curr->sum_exec++;
    (void)domain;
}

static int idle_should_preempt(sched_domain_t *domain, sched_entity_t *curr) {
    // Idle tasks always get preempted by anything
    (void)domain;
    (void)curr;
    return 1;
}

static sched_class_t idle_class = {
    .name           = "idle",
    .type           = SCHED_CLASS_IDLE,
    .init           = idle_init,
    .destroy        = NULL,
    .enqueue        = idle_enqueue,
    .dequeue        = idle_dequeue,
    .yield          = coop_yield,
    .pick_next      = idle_pick_next,
    .tick           = idle_tick,
    .should_preempt = idle_should_preempt,
};

// ============================================================================
// DOMAIN MANAGEMENT
// ============================================================================

sched_domain_t *sched_domain_create(const char *name, sched_class_type_t type,
                                     sched_domain_t *parent) {
    if (hypervisor.nr_domains >= SCHED_MAX_DOMAINS) {
        printk(KERN_ERR "SCHED: Max domains reached\n");
        return NULL;
    }
    
    sched_class_t *sclass = hypervisor.classes[type];
    if (!sclass) {
        printk(KERN_ERR "SCHED: Unknown scheduler class %d\n", type);
        return NULL;
    }
    
    sched_domain_t *domain = kmalloc(sizeof(sched_domain_t), GFP_KERNEL);
    if (!domain) return NULL;
    
    memset(domain, 0, sizeof(*domain));
    
    domain->id = hypervisor.nr_domains;
    strncpy(domain->name, name, sizeof(domain->name) - 1);
    domain->sclass = sclass;
    domain->parent = parent;
    domain->active = 1;
    domain->weight = 1024;
    domain->priority = 100;  // Normal priority
    domain->timeslice = 100;
    domain->timeslice_remaining = 100;
    
    INIT_LIST_HEAD(&domain->runqueue);
    INIT_LIST_HEAD(&domain->children);
    INIT_LIST_HEAD(&domain->sibling);
    spin_lock_init(&domain->lock);
    
    // Initialize scheduler class
    if (sclass->init) {
        sclass->init(domain);
    }
    
    // Add to parent
    if (parent) {
        spin_lock(&parent->lock);
        list_add_tail(&domain->sibling, &parent->children);
        spin_unlock(&parent->lock);
    }
    
    // Register globally
    spin_lock(&hypervisor.lock);
    hypervisor.domains[hypervisor.nr_domains++] = domain;
    spin_unlock(&hypervisor.lock);
    
    printk(KERN_INFO "SCHED: Created domain '%s' (class=%s, id=%d)\n",
           name, sclass->name, domain->id);
    
    return domain;
}

void sched_domain_destroy(sched_domain_t *domain) {
    if (!domain) return;
    
    spin_lock(&hypervisor.lock);
    
    // Remove from parent
    if (domain->parent) {
        spin_lock(&domain->parent->lock);
        list_del(&domain->sibling);
        spin_unlock(&domain->parent->lock);
    }
    
    // Destroy class data
    if (domain->sclass->destroy) {
        domain->sclass->destroy(domain);
    }
    
    // Remove from global list
    for (u32 i = 0; i < hypervisor.nr_domains; i++) {
        if (hypervisor.domains[i] == domain) {
            hypervisor.domains[i] = hypervisor.domains[--hypervisor.nr_domains];
            break;
        }
    }
    
    spin_unlock(&hypervisor.lock);
    
    kfree(domain);
}

// ============================================================================
// SCHEDULER ENTITY MANAGEMENT
// ============================================================================

sched_entity_t *sched_entity_create(task_struct_t *task) {
    sched_entity_t *se = kmalloc(sizeof(sched_entity_t), GFP_KERNEL);
    if (!se) return NULL;
    
    memset(se, 0, sizeof(*se));
    INIT_LIST_HEAD(&se->run_list);
    se->task = task;
    se->time_slice = 100;
    
    return se;
}

void sched_entity_destroy(sched_entity_t *se) {
    if (se->domain && se->on_rq) {
        se->domain->sclass->dequeue(se->domain, se);
    }
    kfree(se);
}

int sched_entity_attach(sched_entity_t *se, sched_domain_t *domain) {
    if (!se || !domain) return -EINVAL;
    
    spin_lock(&domain->lock);
    
    se->domain = domain;
    domain->sclass->enqueue(domain, se);
    
    spin_unlock(&domain->lock);
    
    return 0;
}

void sched_entity_detach(sched_entity_t *se) {
    if (!se || !se->domain) return;
    
    sched_domain_t *domain = se->domain;
    
    spin_lock(&domain->lock);
    
    if (se->on_rq) {
        domain->sclass->dequeue(domain, se);
    }
    se->domain = NULL;
    
    spin_unlock(&domain->lock);
}

// ============================================================================
// HYPERVISOR SCHEDULING
// ============================================================================

// Pick next domain to run (from root's perspective)
static sched_domain_t *hypervisor_pick_domain(void) {
    sched_domain_t *best = NULL;
    int best_priority = 999;
    
    // Simple priority-based selection among active domains
    for (u32 i = 0; i < hypervisor.nr_domains; i++) {
        sched_domain_t *d = hypervisor.domains[i];
        if (d->active && d->nr_running > 0 && d->priority < best_priority) {
            best = d;
            best_priority = d->priority;
        }
    }
    
    return best;
}

// Main scheduler entry point - called from timer tick
void sched_hypervisor_tick(void) {
    u32 cpu = smp_processor_id();
    sched_domain_t *domain = hypervisor.current_domain[cpu];
    
    hypervisor.now++;
    hypervisor.total_ticks++;
    
    if (!domain) {
        // Pick initial domain
        domain = hypervisor_pick_domain();
        if (domain) {
            hypervisor.current_domain[cpu] = domain;
            hypervisor.domain_switches++;
        }
        return;
    }
    
    spin_lock(&domain->lock);
    
    // Tick current domain's scheduler
    if (domain->sclass->tick && domain->curr) {
        domain->sclass->tick(domain, domain->curr);
    }
    
    domain->timeslice_remaining--;
    
    // Check if we need to switch domains
    int need_domain_switch = 0;
    
    if (domain->timeslice_remaining <= 0) {
        need_domain_switch = 1;
        domain->timeslice_remaining = domain->timeslice;
    }
    
    // Check if current entity should be preempted within domain
    int need_entity_switch = 0;
    if (domain->curr && domain->sclass->should_preempt) {
        need_entity_switch = domain->sclass->should_preempt(domain, domain->curr);
    }
    
    spin_unlock(&domain->lock);
    
    // Handle domain switch
    if (need_domain_switch) {
        sched_domain_t *next_domain = hypervisor_pick_domain();
        if (next_domain && next_domain != domain) {
            hypervisor.current_domain[cpu] = next_domain;
            hypervisor.domain_switches++;
            domain = next_domain;
        }
    }
    
    // Handle entity switch within domain
    if (need_entity_switch) {
        sched_hypervisor_schedule();
    }
}

// Schedule within current domain
void sched_hypervisor_schedule(void) {
    u32 cpu = smp_processor_id();
    sched_domain_t *domain = hypervisor.current_domain[cpu];
    
    if (!domain) {
        domain = hypervisor_pick_domain();
        if (!domain) return;
        hypervisor.current_domain[cpu] = domain;
    }
    
    spin_lock(&domain->lock);
    
    // Get current entity
    sched_entity_t *prev = domain->curr;
    
    // Re-enqueue previous if still runnable
    if (prev && prev->task && prev->task->state == TASK_RUNNING) {
        if (!prev->on_rq) {
            domain->sclass->enqueue(domain, prev);
        }
        // Recalculate time slice
        prev->time_slice = 100;
    }
    
    // Pick next entity
    sched_entity_t *next = domain->sclass->pick_next(domain);
    
    if (next && next != prev) {
        // Dequeue next since it's now running
        if (next->on_rq) {
            domain->sclass->dequeue(domain, next);
        }
        
        domain->curr = next;
        domain->switches++;
        
        next->exec_start = sched_clock();
        next->switches++;
        
        // Update current task pointer
        if (next->task) {
            current_tasks[cpu] = next->task;
        }
        
        spin_unlock(&domain->lock);
        
        // Context switch
        if (prev && prev->task && next->task) {
            extern void context_switch(cpu_context_t *old, cpu_context_t *new);
            context_switch(&prev->task->context, &next->task->context);
        }
    } else {
        domain->curr = next;
        spin_unlock(&domain->lock);
    }
}

// Yield current entity (cooperative)
void sched_hypervisor_yield(void) {
    u32 cpu = smp_processor_id();
    sched_domain_t *domain = hypervisor.current_domain[cpu];
    
    if (!domain || !domain->curr) return;
    
    spin_lock(&domain->lock);
    
    if (domain->sclass->yield) {
        domain->sclass->yield(domain, domain->curr);
    }
    
    spin_unlock(&domain->lock);
    
    sched_hypervisor_schedule();
}

// ============================================================================
// STATISTICS
// ============================================================================

void sched_hypervisor_stats(void) {
    printk(KERN_INFO "\n=== Scheduler Hypervisor Statistics ===\n");
    printk(KERN_INFO "Total ticks: %lu\n", hypervisor.total_ticks);
    printk(KERN_INFO "Domain switches: %lu\n", hypervisor.domain_switches);
    printk(KERN_INFO "Active domains: %u\n", hypervisor.nr_domains);
    
    for (u32 i = 0; i < hypervisor.nr_domains; i++) {
        sched_domain_t *d = hypervisor.domains[i];
        printk(KERN_INFO "  Domain '%s' (class=%s): running=%u switches=%lu preempts=%lu\n",
               d->name, d->sclass->name, d->nr_running, d->switches, d->preemptions);
    }
    printk(KERN_INFO "========================================\n\n");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void sched_hypervisor_init(void) {
    printk(KERN_INFO "Initializing Scheduler Hypervisor...\n");
    
    memset(&hypervisor, 0, sizeof(hypervisor));
    spin_lock_init(&hypervisor.lock);
    
    // Register built-in scheduler classes
    hypervisor.classes[SCHED_CLASS_COOPERATIVE] = &coop_class;
    hypervisor.classes[SCHED_CLASS_REALTIME] = &rt_class;
    hypervisor.classes[SCHED_CLASS_FAIR] = &fair_class;
    hypervisor.classes[SCHED_CLASS_BATCH] = &batch_class;
    hypervisor.classes[SCHED_CLASS_IDLE] = &idle_class;
    
    printk(KERN_INFO "  Registered scheduler classes:\n");
    printk(KERN_INFO "    - cooperative (fibers/coroutines)\n");
    printk(KERN_INFO "    - realtime (EDF deadline)\n");
    printk(KERN_INFO "    - fair (CFS-like)\n");
    printk(KERN_INFO "    - batch (throughput)\n");
    printk(KERN_INFO "    - idle (background)\n");
    
    // Create default domains
    sched_domain_create("realtime", SCHED_CLASS_REALTIME, NULL);
    sched_domain_create("normal", SCHED_CLASS_FAIR, NULL);
    sched_domain_create("batch", SCHED_CLASS_BATCH, NULL);
    sched_domain_create("idle", SCHED_CLASS_IDLE, NULL);
    
    // Set priorities (lower = higher priority)
    hypervisor.domains[0]->priority = 0;    // realtime highest
    hypervisor.domains[1]->priority = 100;  // normal
    hypervisor.domains[2]->priority = 120;  // batch
    hypervisor.domains[3]->priority = 140;  // idle lowest
    
    printk(KERN_INFO "  Scheduler Hypervisor initialized!\n");
}

// ============================================================================
// COOPERATIVE SCHEDULER API (for user-space fibers)
// ============================================================================

// Create a fiber domain for an application
sched_domain_t *sched_create_fiber_domain(const char *name) {
    return sched_domain_create(name, SCHED_CLASS_COOPERATIVE, NULL);
}

// Create a fiber within a domain
sched_entity_t *sched_create_fiber(sched_domain_t *domain, 
                                    void (*entry)(void *), void *arg) {
    sched_entity_t *fiber = sched_entity_create(NULL);
    if (!fiber) return NULL;
    
    fiber->coop_entry = entry;
    fiber->coop_arg = arg;
    
    // Allocate fiber stack
    fiber->coop_stack = kmalloc(8192, GFP_KERNEL);  // 8KB stack
    if (!fiber->coop_stack) {
        kfree(fiber);
        return NULL;
    }
    
    // Setup stack
    fiber->coop_sp = (u64)fiber->coop_stack + 8192 - 8;
    
    sched_entity_attach(fiber, domain);
    
    return fiber;
}

// Yield current fiber
void sched_fiber_yield(void) {
    sched_hypervisor_yield();
}
