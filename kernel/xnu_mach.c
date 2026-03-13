/**
 * Picomimi-x64 — Mach Trap Implementations
 * kernel/xnu_mach.c
 *
 * Implements the Mach trap layer (syscall class 0x1000000).
 *
 * Architecture:
 *   - Mach "ports" are fake u32 handles stored in task->mach_ports
 *     (a mach_port_slot_t[] of MACH_PORT_MAX_SLOTS entries).
 *   - mach_task_self()   → MACH_PORT_NAME_TASK_SELF   (always slot 1)
 *   - mach_thread_self() → MACH_PORT_NAME_THREAD_SELF (always slot 2)
 *   - host_self()        → MACH_PORT_NAME_HOST         (always slot 3)
 *   - mach_msg()         → stub: most IPC is used for libSystem bootstrap
 *     which we don't need; returns MACH_SEND_INVALID_DEST for real IPCs,
 *     success for common no-op patterns.
 *   - vm_allocate/vm_deallocate/vm_map → our sys_mmap/sys_munmap
 *   - semaphore_create/wait/signal     → our futex-backed semaphores
 *   - mach_timebase_info               → reports numer=1, denom=1 (ns units)
 *   - mach_absolute_time               → rdtsc-based ns counter
 *   - thread_switch/swtch              → sys_sched_yield
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/xnu_compat.h>
#include <kernel/syscall.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>

/* =========================================================
 * Mach return codes
 * ========================================================= */

#define KERN_SUCCESS                0
#define KERN_INVALID_ADDRESS        1
#define KERN_PROTECTION_FAILURE     2
#define KERN_NO_SPACE               3
#define KERN_INVALID_ARGUMENT       4
#define KERN_FAILURE                5
#define KERN_RESOURCE_SHORTAGE      6
#define KERN_NOT_RECEIVER           7
#define KERN_NO_ACCESS              8
#define KERN_MEMORY_FAILURE         9
#define KERN_MEMORY_ERROR           10
#define KERN_ALREADY_IN_SET         11
#define KERN_NOT_IN_SET             12
#define KERN_NAME_EXISTS            13
#define KERN_ABORTED                14
#define KERN_INVALID_NAME           15
#define KERN_INVALID_TASK           16
#define KERN_INVALID_RIGHT          17
#define KERN_INVALID_VALUE          18
#define KERN_UREFS_OVERFLOW         19
#define KERN_INVALID_CAPABILITY     20
#define KERN_RIGHT_EXISTS           21
#define KERN_INVALID_HOST           22
#define KERN_MEMORY_PRESENT         23
#define KERN_MEMORY_DATA_MOVED      24
#define KERN_MEMORY_RESTART_COPY    25
#define KERN_INVALID_PROCESSOR_SET  26
#define KERN_POLICY_LIMIT           27
#define KERN_INVALID_POLICY         28
#define KERN_INVALID_OBJECT         29
#define KERN_ALREADY_WAITING        30
#define KERN_DEFAULT_SET            31
#define KERN_EXCEPTION_PROTECTED    32
#define KERN_INVALID_LEDGER         33
#define KERN_INVALID_MEMORY_CONTROL 34
#define KERN_INVALID_SECURITY       35
#define KERN_NOT_DEPRESSED          36
#define KERN_TERMINATED             37
#define KERN_LOCK_SET_DESTROYED     38
#define KERN_LOCK_UNSTABLE          39
#define KERN_LOCK_OWNED             40
#define KERN_LOCK_OWNED_SELF        41
#define KERN_SEMAPHORE_DESTROYED    42
#define KERN_RPC_SERVER_TERMINATED  43
#define KERN_RPC_TERMINATE_ORPHAN   44
#define KERN_RPC_CONTINUE_ORPHAN    45
#define KERN_NOT_SUPPORTED          46
#define KERN_NODE_DOWN              47
#define KERN_NOT_WAITING            48
#define KERN_OPERATION_TIMED_OUT    49
#define KERN_CODESIGN_ERROR         50
#define KERN_POLICY_STATIC          51
#define KERN_INSUFFICIENT_BUFFER_SIZE 52

/* mach_msg return codes */
#define MACH_MSG_SUCCESS            0
#define MACH_SEND_INVALID_DEST      0x10000003
#define MACH_RCV_TIMED_OUT          0x10004003
#define MACH_SEND_TIMED_OUT         0x10000004

/* =========================================================
 * Mach port table management
 * ========================================================= */

void mach_ports_init_task(void) {
    task_struct_t *t = get_current_task();
    if (!t) return;
    if (t->mach_ports) return; /* already initialised */

    mach_port_slot_t *slots = kmalloc(
        sizeof(mach_port_slot_t) * MACH_PORT_MAX_SLOTS, GFP_KERNEL);
    if (!slots) return;
    memset(slots, 0, sizeof(mach_port_slot_t) * MACH_PORT_MAX_SLOTS);

    /* Slot 0 = NULL (unused) */
    /* Slot 1 = TASK_SELF */
    slots[MACH_PORT_NAME_TASK_SELF].type   = MACH_PORT_TYPE_TASK;
    slots[MACH_PORT_NAME_TASK_SELF].object = t;
    slots[MACH_PORT_NAME_TASK_SELF].refs   = 1;

    /* Slot 2 = THREAD_SELF */
    slots[MACH_PORT_NAME_THREAD_SELF].type   = MACH_PORT_TYPE_THREAD;
    slots[MACH_PORT_NAME_THREAD_SELF].object = t;
    slots[MACH_PORT_NAME_THREAD_SELF].refs   = 1;

    /* Slot 3 = HOST */
    slots[MACH_PORT_NAME_HOST].type   = MACH_PORT_TYPE_HOST;
    slots[MACH_PORT_NAME_HOST].object = NULL;
    slots[MACH_PORT_NAME_HOST].refs   = 1;

    t->mach_ports        = slots;
    t->mach_task_port    = MACH_PORT_NAME_TASK_SELF;
    t->mach_thread_port  = MACH_PORT_NAME_THREAD_SELF;
}

mach_port_t mach_port_alloc_task(void) {
    mach_ports_init_task();
    return MACH_PORT_NAME_TASK_SELF;
}

mach_port_t mach_port_alloc_thread(void) {
    mach_ports_init_task();
    return MACH_PORT_NAME_THREAD_SELF;
}

mach_port_t mach_port_alloc_host(void) {
    mach_ports_init_task();
    return MACH_PORT_NAME_HOST;
}

/* Allocate a new port slot for a semaphore (or other object) */
static mach_port_t mach_port_new_slot(mach_port_type_t type, void *obj) {
    task_struct_t *t = get_current_task();
    if (!t || !t->mach_ports) return MACH_PORT_NULL;
    mach_port_slot_t *slots = (mach_port_slot_t *)t->mach_ports;
    for (int i = MACH_PORT_NAME_FIRST_FREE; i < MACH_PORT_MAX_SLOTS; i++) {
        if (slots[i].type == MACH_PORT_TYPE_NONE) {
            slots[i].type   = type;
            slots[i].object = obj;
            slots[i].refs   = 1;
            return (mach_port_t)i;
        }
    }
    return MACH_PORT_NULL; /* no free slots */
}

static mach_port_slot_t *mach_port_lookup(mach_port_t name) {
    task_struct_t *t = get_current_task();
    if (!t || !t->mach_ports) return NULL;
    if (name == MACH_PORT_NULL || name >= MACH_PORT_MAX_SLOTS) return NULL;
    mach_port_slot_t *slots = (mach_port_slot_t *)t->mach_ports;
    if (slots[name].type == MACH_PORT_TYPE_NONE) return NULL;
    return &slots[name];
}

/* =========================================================
 * Mach semaphore (backed by kernel semaphore / futex)
 * ========================================================= */

typedef struct mach_semaphore {
    int     value;
    int     waiters;
    void   *wait_queue;  /* we'll use futex-style wait */
} mach_semaphore_t;

extern s64 sys_futex(int *uaddr, int op, int val, const struct timespec *timeout,
                     int *uaddr2, int val3);

/* SYNC_POLICY_FIFO = 0 */
static s64 mach_semaphore_create(mach_port_t task, mach_port_t *semaphore_out,
                                  int policy, int value) {
    (void)task; (void)policy;
    mach_semaphore_t *sem = kmalloc(sizeof(mach_semaphore_t), GFP_KERNEL);
    if (!sem) return KERN_RESOURCE_SHORTAGE;
    sem->value   = value;
    sem->waiters = 0;
    sem->wait_queue = NULL;

    mach_ports_init_task();
    mach_port_t name = mach_port_new_slot(MACH_PORT_TYPE_SEMAPHORE, sem);
    if (name == MACH_PORT_NULL) { kfree(sem); return KERN_NO_SPACE; }
    if (semaphore_out) *semaphore_out = name;
    return KERN_SUCCESS;
}

static s64 mach_semaphore_destroy(mach_port_t task, mach_port_t semaphore) {
    (void)task;
    mach_port_slot_t *slot = mach_port_lookup(semaphore);
    if (!slot || slot->type != MACH_PORT_TYPE_SEMAPHORE)
        return KERN_INVALID_ARGUMENT;
    kfree(slot->object);
    slot->type   = MACH_PORT_TYPE_NONE;
    slot->object = NULL;
    slot->refs   = 0;
    return KERN_SUCCESS;
}

static s64 mach_semaphore_signal(mach_port_t semaphore) {
    mach_port_slot_t *slot = mach_port_lookup(semaphore);
    if (!slot || slot->type != MACH_PORT_TYPE_SEMAPHORE)
        return KERN_INVALID_ARGUMENT;
    mach_semaphore_t *sem = (mach_semaphore_t *)slot->object;
    /* Atomic increment — simple for now (single-core boot path) */
    __sync_fetch_and_add(&sem->value, 1);
    return KERN_SUCCESS;
}

static s64 mach_semaphore_signal_all(mach_port_t semaphore) {
    mach_port_slot_t *slot = mach_port_lookup(semaphore);
    if (!slot || slot->type != MACH_PORT_TYPE_SEMAPHORE)
        return KERN_INVALID_ARGUMENT;
    mach_semaphore_t *sem = (mach_semaphore_t *)slot->object;
    /* Wake all — set value to max */
    int w = sem->waiters;
    if (w > 0) __sync_fetch_and_add(&sem->value, w);
    return KERN_SUCCESS;
}

static s64 mach_semaphore_wait(mach_port_t semaphore) {
    mach_port_slot_t *slot = mach_port_lookup(semaphore);
    if (!slot || slot->type != MACH_PORT_TYPE_SEMAPHORE)
        return KERN_INVALID_ARGUMENT;
    mach_semaphore_t *sem = (mach_semaphore_t *)slot->object;

    /* Spin-wait (safe for single-threaded init paths) */
    int tries = 0;
    while (sem->value <= 0) {
        extern s64 sys_sched_yield(void);
        sys_sched_yield();
        if (++tries > 10000) return KERN_ABORTED;
    }
    __sync_fetch_and_sub(&sem->value, 1);
    return KERN_SUCCESS;
}

/* =========================================================
 * mach_msg — the Mach IPC workhorse.
 *
 * Real XNU uses this for bootstrap, launchd, IOKit, etc.
 * We stub it safely: any send to a non-task-self port returns
 * MACH_SEND_INVALID_DEST. This makes dyld's bootstrap_look_up
 * calls fail gracefully (it falls back to flat namespace).
 * ========================================================= */

#define MACH_SEND_MSG       0x00000001
#define MACH_RCV_MSG        0x00000002
#define MACH_SEND_TIMEOUT   0x00000010
#define MACH_RCV_TIMEOUT    0x00000100

/* Minimal mach_msg_header */
typedef struct mach_msg_header {
    u32 msgh_bits;
    u32 msgh_size;
    u32 msgh_remote_port;
    u32 msgh_local_port;
    u32 msgh_voucher_port;
    s32 msgh_id;
} mach_msg_header_t;

static s64 xnu_mach_msg(mach_msg_header_t *msg, int option,
                         u32 send_size, u32 rcv_size,
                         mach_port_t rcv_name, u32 timeout,
                         mach_port_t notify)
{
    (void)rcv_size; (void)timeout; (void)notify;

    if (option & MACH_SEND_MSG) {
        if (!msg) return MACH_SEND_INVALID_DEST;
        /* Check if the remote port is one of our known ports */
        u32 remote = msg->msgh_remote_port;
        mach_port_slot_t *slot = mach_port_lookup((mach_port_t)remote);
        if (!slot) return MACH_SEND_INVALID_DEST;
        /* We don't actually deliver the message — task_self and host_self
         * sends during libSystem init can be silently dropped */
        return MACH_MSG_SUCCESS;
    }

    if (option & MACH_RCV_MSG) {
        /* No messages queued — return timeout */
        return MACH_RCV_TIMED_OUT;
    }

    return MACH_MSG_SUCCESS;
}

/* =========================================================
 * vm_ traps — map to our mm syscalls
 * ========================================================= */

/* vm_allocate: allocate memory in task's address space */
static s64 xnu_vm_allocate(mach_port_t task, u64 *addr, u64 size, int flags) {
    (void)task;
    /* flags & 1 = VM_FLAGS_ANYWHERE (find a free region) */
    extern s64 sys_mmap(void *addr, size_t len, int prot, int flags,
                        int fd, s64 off);
    #ifndef MAP_ANON
    #define MAP_ANON    0x20
    #endif
    #ifndef MAP_PRIVATE
    #define MAP_PRIVATE 0x02
    #endif
    #ifndef MAP_FIXED
    #define MAP_FIXED   0x10
    #endif
    #ifndef PROT_READ
    #define PROT_READ   0x01
    #endif
    #ifndef PROT_WRITE
    #define PROT_WRITE  0x02
    #endif

    int mmap_flags = MAP_ANON | MAP_PRIVATE;
    void *hint = (addr && !(flags & 1)) ? (void *)*addr : NULL;
    if (hint && !(flags & 1)) mmap_flags |= MAP_FIXED;

    s64 r = sys_mmap(hint, (size_t)size,
                     PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    if (r < 0) return KERN_NO_SPACE;
    if (addr) *addr = (u64)r;
    return KERN_SUCCESS;
}

static s64 xnu_vm_deallocate(mach_port_t task, u64 addr, u64 size) {
    (void)task;
    extern s64 sys_munmap(void *addr, size_t length);
    s64 r = sys_munmap((void *)addr, (size_t)size);
    return r < 0 ? KERN_INVALID_ADDRESS : KERN_SUCCESS;
}

static s64 xnu_vm_protect(mach_port_t task, u64 addr, u64 size,
                            int set_maximum, int new_prot) {
    (void)task; (void)set_maximum;
    extern s64 sys_mprotect(void *addr, size_t len, int prot);
    s64 r = sys_mprotect((void *)addr, (size_t)size, new_prot);
    return r < 0 ? KERN_PROTECTION_FAILURE : KERN_SUCCESS;
}

/* =========================================================
 * mach_timebase_info
 *
 * Darwin uses mach_absolute_time() / mach_timebase_info to get
 * nanosecond timestamps. We report numer=1, denom=1 (1 tick = 1 ns)
 * and implement mach_absolute_time as rdtsc calibrated to ns.
 * ========================================================= */

typedef struct {
    u32 numer;
    u32 denom;
} mach_timebase_info_t;

static s64 xnu_mach_timebase_info(mach_timebase_info_t *info) {
    if (!info) return KERN_INVALID_ARGUMENT;
    info->numer = 1;
    info->denom = 1;
    return KERN_SUCCESS;
}

/* mach_absolute_time: return nanoseconds via RDTSC.
 * We assume 1 GHz TSC for simplicity (real calibration is future work). */
static u64 mach_absolute_time_impl(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 tsc = ((u64)hi << 32) | lo;
    /* Divide by ~1 (assume 1ns/tick at 1GHz) — good enough for relative timing */
    return tsc;
}

/* mach_wait_until: sleep until absolute time */
static s64 xnu_mach_wait_until(u64 deadline) {
    u64 now = mach_absolute_time_impl();
    if (deadline <= now) return KERN_SUCCESS;
    u64 delta_ns = deadline - now;
    struct timespec ts;
    ts.tv_sec  = (s64)(delta_ns / 1000000000ULL);
    ts.tv_nsec = (s64)(delta_ns % 1000000000ULL);
    extern s64 sys_nanosleep(const struct timespec *req, struct timespec *rem);
    sys_nanosleep(&ts, NULL);
    return KERN_SUCCESS;
}

/* =========================================================
 * mk_timer — Mach timer (used by some frameworks)
 * We stub with a fake port handle.
 * ========================================================= */

static s64 xnu_mk_timer_create(void) {
    mach_ports_init_task();
    /* Allocate a slot of type TASK (re-used as a generic handle) */
    mach_port_t p = mach_port_new_slot(MACH_PORT_TYPE_TASK, NULL);
    return (s64)p;
}

static s64 xnu_mk_timer_destroy(mach_port_t timer) {
    mach_port_slot_t *slot = mach_port_lookup(timer);
    if (!slot) return KERN_INVALID_ARGUMENT;
    slot->type = MACH_PORT_TYPE_NONE;
    return KERN_SUCCESS;
}

static s64 xnu_mk_timer_arm(mach_port_t timer, u64 expire_time) {
    (void)timer; (void)expire_time;
    return KERN_SUCCESS; /* stub */
}

static s64 xnu_mk_timer_cancel(mach_port_t timer, u64 *result) {
    (void)timer;
    if (result) *result = 0;
    return KERN_SUCCESS;
}

/* =========================================================
 * task_for_pid / pid_for_task
 * ========================================================= */

static s64 xnu_task_for_pid(mach_port_t host, pid_t pid, mach_port_t *task_out) {
    (void)host;
    extern task_struct_t *find_task_by_pid(pid_t pid);
    task_struct_t *t = find_task_by_pid(pid);
    if (!t) return KERN_FAILURE;
    /* Return task_self port (simplified — real XNU gives a per-target port) */
    if (task_out) *task_out = MACH_PORT_NAME_TASK_SELF;
    return KERN_SUCCESS;
}

static s64 xnu_pid_for_task(mach_port_t task, pid_t *pid_out) {
    (void)task;
    if (pid_out) *pid_out = (pid_t)get_current_task()->tgid;
    return KERN_SUCCESS;
}

/* =========================================================
 * iokit_user_client_trap — IOKit userspace → kernel
 * Stub: return KERN_NOT_SUPPORTED. Apps that need real IOKit
 * (GPU, HID, etc.) won't work until proper IOKit is added.
 * ========================================================= */

static s64 xnu_iokit_user_client_trap(void *userClient, u32 index,
                                       u64 p1, u64 p2, u64 p3,
                                       u64 p4, u64 p5, u64 p6) {
    (void)userClient; (void)index;
    (void)p1; (void)p2; (void)p3; (void)p4; (void)p5; (void)p6;
    return -KERN_NOT_SUPPORTED;
}

/* =========================================================
 * Mach trap dispatch
 * ========================================================= */

s64 xnu_mach_dispatch(u64 nr, u64 a1, u64 a2, u64 a3,
                       u64 a4, u64 a5, u64 a6)
{
    switch ((int)nr) {

    /* Port self-identification */
    case MACH_NR_task_self_trap:
        mach_ports_init_task();
        return (s64)get_current_task()->mach_task_port;

    case MACH_NR_thread_self_trap:
        mach_ports_init_task();
        return (s64)get_current_task()->mach_thread_port;

    case MACH_NR_host_self_trap:
        mach_ports_init_task();
        return (s64)MACH_PORT_NAME_HOST;

    case MACH_NR_reply_port:
        /* Allocate a fresh port for reply — reuse thread_self for now */
        mach_ports_init_task();
        return (s64)get_current_task()->mach_thread_port;

    /* mach_msg */
    case MACH_NR_mach_msg_trap:
    case MACH_NR_mach_msg_overwrite_trap:
    case MACH_NR_mach_msg2_trap:
        return xnu_mach_msg((mach_msg_header_t*)a1, (int)a2,
                             (u32)a3, (u32)a4, (mach_port_t)a5,
                             (u32)a6, MACH_PORT_NULL);

    /* Semaphores */
    case MACH_NR_semaphore_signal_trap:
        return mach_semaphore_signal((mach_port_t)a1);
    case MACH_NR_semaphore_signal_all_trap:
        return mach_semaphore_signal_all((mach_port_t)a1);
    case MACH_NR_semaphore_wait_trap:
        return mach_semaphore_wait((mach_port_t)a1);
    case MACH_NR_semaphore_timedwait_trap:
        /* a1=semaphore a2=sec a3=nsec — just call plain wait for now */
        return mach_semaphore_wait((mach_port_t)a1);
    case MACH_NR_semaphore_signal_thread_trap:
        /* signal specific thread — signal all as approximation */
        return mach_semaphore_signal_all((mach_port_t)a1);
    case MACH_NR_semaphore_wait_signal_trap:
        /* wait a1, then signal a2 */
        { s64 r = mach_semaphore_wait((mach_port_t)a1);
          mach_semaphore_signal((mach_port_t)a2);
          return r; }
    case MACH_NR_semaphore_timedwait_signal_trap:
        { s64 r = mach_semaphore_wait((mach_port_t)a1);
          mach_semaphore_signal((mach_port_t)a2);
          return r; }

    /* Thread scheduling */
    case MACH_NR_swtch_pri:
    case MACH_NR_swtch: {
        extern s64 sys_sched_yield(void);
        sys_sched_yield();
        return KERN_SUCCESS;
    }
    case MACH_NR_thread_switch: {
        extern s64 sys_sched_yield(void);
        sys_sched_yield();
        return KERN_SUCCESS;
    }
    case MACH_NR_clock_sleep_trap: {
        /* a1=clock_name a2=sleep_type a3=sleep_sec a4=sleep_nsec a5=*wakeup */
        struct timespec ts;
        ts.tv_sec  = (s64)a3;
        ts.tv_nsec = (s64)a4;
        extern s64 sys_nanosleep(const struct timespec *req,struct timespec *rem);
        sys_nanosleep(&ts, NULL);
        return KERN_SUCCESS;
    }

    /* Timebase / time */
    case MACH_NR_mach_timebase_info:
        return xnu_mach_timebase_info((mach_timebase_info_t*)a1);

    case MACH_NR_mach_wait_until:
        return xnu_mach_wait_until(a1);

    /* mk_timer */
    case MACH_NR_mk_timer_create:  return xnu_mk_timer_create();
    case MACH_NR_mk_timer_destroy: return xnu_mk_timer_destroy((mach_port_t)a1);
    case MACH_NR_mk_timer_arm:     return xnu_mk_timer_arm((mach_port_t)a1, a2);
    case MACH_NR_mk_timer_cancel:  return xnu_mk_timer_cancel((mach_port_t)a1, (u64*)a2);

    /* task_for_pid / pid_for_task */
    case MACH_NR_task_name_for_pid: /* fall-through */
        return xnu_task_for_pid((mach_port_t)a1, (pid_t)a2, (mach_port_t*)a3);
    case MACH_NR_pid_for_task:
        return xnu_pid_for_task((mach_port_t)a1, (pid_t*)a2);

    /* IOKit */
    case MACH_NR_iokit_user_client_trap:
        return xnu_iokit_user_client_trap((void*)a1,(u32)a2,a3,a4,a5,a6,0,0);

    default:
        printk(KERN_DEBUG "[xnu_mach] unimplemented trap %llu\n", nr);
        return KERN_NOT_SUPPORTED;
    }
}

/* =========================================================
 * Mach Kernel Server routines
 * These are called via MIG-generated stubs from userspace.
 * We expose them as exported kernel functions so that if we
 * ever implement a real IPC path, they can be called directly.
 * ========================================================= */

s64 mach_semaphore_create_kfn(mach_port_t task, mach_port_t *out,
                                int policy, int value) {
    return mach_semaphore_create(task, out, policy, value);
}

s64 mach_semaphore_destroy_kfn(mach_port_t task, mach_port_t sem) {
    return mach_semaphore_destroy(task, sem);
}

s64 mach_vm_allocate_kfn(mach_port_t task, u64 *addr, u64 size, int flags) {
    return xnu_vm_allocate(task, addr, size, flags);
}

s64 mach_vm_deallocate_kfn(mach_port_t task, u64 addr, u64 size) {
    return xnu_vm_deallocate(task, addr, size);
}

s64 mach_vm_protect_kfn(mach_port_t task, u64 addr, u64 size,
                          int set_max, int prot) {
    return xnu_vm_protect(task, addr, size, set_max, prot);
}

/* =========================================================
 * Top-level XNU syscall entry (called from syscall_dispatch)
 * ========================================================= */

s64 xnu_syscall_dispatch(u64 raw_nr, u64 a1, u64 a2, u64 a3,
                          u64 a4, u64 a5, u64 a6)
{
    u32 cls = XNU_SYSCALL_CLASS(raw_nr);
    u32 nr  = XNU_SYSCALL_NUMBER(raw_nr);

    switch (cls) {
    case XNU_CLASS_BSD:
        return xnu_bsd_dispatch(nr, a1, a2, a3, a4, a5, a6);

    case XNU_CLASS_MACH:
        return xnu_mach_dispatch(nr, a1, a2, a3, a4, a5, a6);

    case XNU_CLASS_MDEP:
        /* Machine-dependent: thread_fast_set_cthread_self etc. */
        if (nr == 3) {
            /* set_thread_self_fast — sets %gs base to a1 (TLS) */
            extern void write_gs_base(u64);
            /* We store it in TLS just like Linux arch_prctl(ARCH_SET_GS) */
            get_current_task()->tls = a1;
            __asm__ volatile("wrmsr" :: "c"(0xC0000101UL),
                             "a"((u32)a1), "d"((u32)(a1 >> 32)));
            return 0;
        }
        return -ENOSYS;

    default:
        printk(KERN_DEBUG "[xnu] unknown syscall class %u nr %u\n", cls, nr);
        return -ENOSYS;
    }
}
