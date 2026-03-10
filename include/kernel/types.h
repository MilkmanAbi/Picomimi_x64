/**
 * Picomimi-x64 Kernel Type Definitions
 * Linux-compatible naming with custom implementation
 * 
 * Based on Picomimi-AxisOS architecture for RP2040/RP2350
 * Ported to x86_64 long mode with SMP support
 */
#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// LINUX-COMPATIBLE BASE TYPES
// ============================================================================

typedef int8_t      __s8;
typedef uint8_t     __u8;
typedef int16_t     __s16;
typedef uint16_t    __u16;
typedef int32_t     __s32;
typedef uint32_t    __u32;
typedef int64_t     __s64;
typedef uint64_t    __u64;

typedef __s8        s8;
typedef __u8        u8;
typedef __s16       s16;
typedef __u16       u16;
typedef __s32       s32;
typedef __u32       u32;
typedef __s64       s64;
typedef __u64       u64;

typedef __u64       size_t;
typedef __s64       ssize_t;
typedef __s64       off_t;
typedef __s64       loff_t;
typedef __u64       uintptr_t;
typedef __s64       intptr_t;
typedef __s64       ptrdiff_t;

typedef __s32       pid_t;
typedef __u32       uid_t;
typedef __u32       gid_t;
typedef __u32       mode_t;
typedef __u32       dev_t;
typedef __u64       ino_t;
typedef __s64       time_t;
typedef __s64       clock_t;

typedef struct { volatile __s32 counter; } atomic_t;
typedef struct { volatile __s64 counter; } atomic64_t;

typedef __u64       phys_addr_t;
typedef __u64       virt_addr_t;
typedef __u64       dma_addr_t;
typedef __u64       resource_size_t;
typedef __u64       sector_t;
typedef __u64       blkcnt_t;

// ============================================================================
// POSIX ERROR CODES
// ============================================================================

#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EWOULDBLOCK     EAGAIN
#define ENOMSG          42
#define EIDRM           43
#define ENOSTR          60
#define ENODATA         61
#define ETIME           62
#define ENOSR           63
#define ENOLINK         67
#define EPROTO          71
#define EMULTIHOP       72
#define EBADMSG         74
#define EOVERFLOW       75
#define EILSEQ          84
#define EUSERS          87
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP      95
#define EPFNOSUPPORT    96
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ENETRESET       102
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ESHUTDOWN       108
#define ETOOMANYREFS    109
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#define ESTALE          116

// Exit status
#define EXIT_ZOMBIE     16
#define EXIT_DEAD       32

// ============================================================================
// RESULT CODES (deprecated, use POSIX errors)
// ============================================================================

typedef enum {
    KERN_OK = 0, KERN_ERROR = -1, KERN_ENOMEM = -12, KERN_EINVAL = -22,
    KERN_EBUSY = -16, KERN_ENOENT = -2, KERN_EAGAIN = -11, KERN_EPERM = -1,
    KERN_EFAULT = -14, KERN_ENOSYS = -38, KERN_ETIMEDOUT = -110, KERN_EDEADLK = -35,
} kern_result_t;

// ============================================================================
// TASK STATES
// ============================================================================

typedef enum {
    TASK_RUNNING = 0, TASK_INTERRUPTIBLE = 1, TASK_UNINTERRUPTIBLE = 2,
    TASK_STOPPED = 4, TASK_TRACED = 8, TASK_DEAD = 16, TASK_ZOMBIE = 32,
    TASK_PARKED = 64, TASK_IDLE = 0x402,
} task_state_t;

typedef enum {
    SCHED_NORMAL = 0, SCHED_FIFO = 1, SCHED_RR = 2, SCHED_BATCH = 3,
    SCHED_IDLE = 5, SCHED_DEADLINE = 6,
} sched_policy_t;

#define MAX_NICE        19
#define MIN_NICE        -20
#define NICE_WIDTH      40
#define MAX_RT_PRIO     100
#define MAX_PRIO        140
#define DEFAULT_PRIO    120

// ============================================================================
// CPU/SMP TYPES
// ============================================================================

#define NR_CPUS         256
#define BITS_PER_LONG   64

typedef __u32 cpu_id_t;
typedef __u32 apic_id_t;

typedef struct { __u64 bits[(NR_CPUS + 63) / 64]; } cpumask_t;

extern __u64 __per_cpu_offset[NR_CPUS];

// ============================================================================
// MEMORY TYPES
// ============================================================================

typedef enum { ZONE_DMA = 0, ZONE_DMA32, ZONE_NORMAL, ZONE_HIGHMEM, ZONE_MOVABLE, MAX_NR_ZONES } zone_type_t;
typedef enum { MEM_PRESSURE_NONE = 0, MEM_PRESSURE_LOW, MEM_PRESSURE_MODERATE, MEM_PRESSURE_HIGH, MEM_PRESSURE_CRITICAL, MEM_PRESSURE_OOM } mem_pressure_t;

typedef __u32 gfp_t;
#define __GFP_DMA       0x01u
#define __GFP_DMA32     0x04u
#define __GFP_WAIT      0x10u
#define __GFP_HIGH      0x20u
#define __GFP_IO        0x40u
#define __GFP_FS        0x80u
#define __GFP_ZERO      0x100u
#define GFP_KERNEL      (__GFP_WAIT | __GFP_IO | __GFP_FS)
#define GFP_ATOMIC      (__GFP_HIGH)
#define GFP_USER        (__GFP_WAIT | __GFP_IO | __GFP_FS)

// ============================================================================
// LINKED LIST
// ============================================================================

struct list_head { struct list_head *next, *prev; };

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) { list->next = list; list->prev = list; }
static inline int list_empty(const struct list_head *head) { return head->next == head; }

static inline void __list_add(struct list_head *new, struct list_head *prev, struct list_head *next) {
    next->prev = new; new->next = next; new->prev = prev; prev->next = new;
}
static inline void list_add(struct list_head *new, struct list_head *head) { __list_add(new, head, head->next); }
static inline void list_add_tail(struct list_head *new, struct list_head *head) { __list_add(new, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head *entry) { __list_del(entry->prev, entry->next); entry->next = (void *)0; entry->prev = (void *)0; }
static inline void list_del_init(struct list_head *entry) { __list_del(entry->prev, entry->next); entry->next = entry; entry->prev = entry; }
static inline void list_move(struct list_head *list, struct list_head *head) { __list_del(list->prev, list->next); list_add(list, head); }
static inline void list_move_tail(struct list_head *list, struct list_head *head) { __list_del(list->prev, list->next); list_add_tail(list, head); }
static inline int list_is_first(const struct list_head *list, const struct list_head *head) { return list->prev == head; }
static inline int list_is_last(const struct list_head *list, const struct list_head *head) { return list->next == head; }

// Hash list (single-pointer head)
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };

#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = { .first = NULL }
static inline void INIT_HLIST_HEAD(struct hlist_head *h) { h->first = NULL; }
static inline int hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h) {
    struct hlist_node *first = h->first;
    n->next = first;
    if (first) first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}
static inline void hlist_del(struct hlist_node *n) {
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;
    *pprev = next;
    if (next) next->pprev = pprev;
}

#define list_entry(ptr, type, member) ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) for (pos = list_entry((head)->next, typeof(*pos), member); &pos->member != (head); pos = list_entry(pos->member.next, typeof(*pos), member))

// ============================================================================
// SYNCHRONIZATION
// ============================================================================

typedef struct { volatile __u32 lock; } arch_spinlock_t;
typedef struct { arch_spinlock_t raw_lock; } spinlock_t;
typedef struct { atomic_t count; spinlock_t wait_lock; struct list_head wait_list; void *owner; } mutex_t;
typedef struct { atomic_t count; spinlock_t lock; struct list_head wait_list; } semaphore_t;
typedef struct { volatile __s32 lock; } rwlock_t;

// ============================================================================
// WAIT QUEUE
// ============================================================================

typedef struct wait_queue_entry {
    __u32 flags; void *private;
    int (*func)(struct wait_queue_entry *, unsigned, int, void *);
    struct list_head entry;
} wait_queue_entry_t;

typedef struct wait_queue_head { spinlock_t lock; struct list_head head; } wait_queue_head_t;

// ============================================================================
// INTERRUPTS
// ============================================================================

typedef __u64 irqflags_t;
typedef __u32 irq_t;
typedef enum { IRQ_NONE = 0, IRQ_HANDLED = 1, IRQ_WAKE_THREAD = 2 } irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(irq_t irq, void *dev_id);

// ============================================================================
// TIME
// ============================================================================

struct timespec { time_t tv_sec; __s64 tv_nsec; };
struct timeval { time_t tv_sec; __s64 tv_usec; };
struct timespec64 { s64 tv_sec; s64 tv_nsec; };
typedef __s64 ktime_t;

#define NSEC_PER_SEC    1000000000L
#define USEC_PER_SEC    1000000L
#define NSEC_PER_USEC   1000L
#define NSEC_PER_MSEC   1000000L
#define HZ              1000

extern volatile __u64 jiffies;

// Read-write semaphore
struct rw_semaphore {
    atomic_t count;
    spinlock_t wait_lock;
    struct list_head wait_list;
};

// Lockref (combined spinlock and reference count)
struct lockref {
    union {
        __u64 lock_count;
        struct {
            spinlock_t lock;
            int count;
        };
    };
};

// Iattr for setattr
struct iattr {
    unsigned int ia_valid;
    u32 ia_mode;
    uid_t ia_uid;
    gid_t ia_gid;
    s64 ia_size;
    struct timespec64 ia_atime;
    struct timespec64 ia_mtime;
    struct timespec64 ia_ctime;
};

#define ATTR_MODE   (1 << 0)
#define ATTR_UID    (1 << 1)
#define ATTR_GID    (1 << 2)
#define ATTR_SIZE   (1 << 3)
#define ATTR_ATIME  (1 << 4)
#define ATTR_MTIME  (1 << 5)
#define ATTR_CTIME  (1 << 6)

// Fiemap (file extent mapping)
struct fiemap_extent_info {
    unsigned int fi_flags;
    unsigned int fi_extents_mapped;
    unsigned int fi_extents_max;
    void *fi_extents_start;
};

// Statfs
struct statfs {
    s64 f_type;
    s64 f_bsize;
    s64 f_blocks;
    s64 f_bfree;
    s64 f_bavail;
    s64 f_files;
    s64 f_ffree;
    s64 f_fsid;
    s64 f_namelen;
    s64 f_frsize;
    s64 f_flags;
    s64 f_spare[4];
};

// ============================================================================
// SIGNALS
// ============================================================================

#define NSIG 64
#define _NSIG_WORDS 2
typedef struct { __u64 sig[_NSIG_WORDS]; } sigset_t;

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGCHLD 17
#define SIGSTOP 19

// ============================================================================
// COMPILER ATTRIBUTES
// ============================================================================

#define __packed            __attribute__((packed))
#define __aligned(x)        __attribute__((aligned(x)))
#define __section(x)        __attribute__((section(x)))
#define __used              __attribute__((used))
#define __unused            __attribute__((unused))
#define __weak              __attribute__((weak))
#define __always_inline     __attribute__((always_inline)) inline
#define __noinline          __attribute__((noinline))
#define __noreturn          __attribute__((noreturn))
#define __init              __section(".init.text")
#define __initdata          __section(".init.data")

#define likely(x)           __builtin_expect(!!(x), 1)
#define unlikely(x)         __builtin_expect(!!(x), 0)
#define barrier()           __asm__ __volatile__("" ::: "memory")
#define cpu_relax()         __asm__ __volatile__("pause" ::: "memory")

#define container_of(ptr, type, member) ({ const typeof(((type *)0)->member) *__mptr = (ptr); (type *)((char *)__mptr - __builtin_offsetof(type, member)); })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define BIT(n)              (1UL << (n))
#define GENMASK(h, l)       (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define ALIGN(x, a)         (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((a) - 1)) == 0)

#define PAGE_SHIFT          12
#define PAGE_SIZE           (1UL << PAGE_SHIFT)
#define PAGE_MASK           (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr)    ALIGN(addr, PAGE_SIZE)

#define MAX_ERRNO           4095
#define IS_ERR_VALUE(x)     unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }

// ============================================================================
// KERNEL CONSTANTS
// ============================================================================

#define KERNEL_VERSION_STRING   "1.0.0-picomimi-x64"
#define KERNEL_NAME             "Picomimi-x64"
#define MAX_TASKS               4096
#define MAX_FDS_PER_PROCESS     1024
#define KERNEL_STACK_SIZE       (16 * PAGE_SIZE)
#define USER_STACK_SIZE         (2 * 1024 * 1024)
#define SCHED_TIMESLICE_MS      10
#define SCHED_PRIORITY_LEVELS   140

// ============================================================================
// MISSING SIGNAL NUMBERS
// ============================================================================

#define SIGUSR1     10
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGCONT     18
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPWR      30
#define SIGSYS      31
#define SIGRTMIN    34
#define SIGRTMAX    64

// ============================================================================
// PROCESS EXIT STATES
// ============================================================================

#define EXIT_STOPPED    4
#define EXIT_CONTINUED  8
#define EXIT_DEAD       32

// ============================================================================
// VM AREA FLAGS
// ============================================================================

#ifndef VM_READ
#define VM_READ         0x00000001
#define VM_WRITE        0x00000002
#define VM_EXEC         0x00000004
#define VM_SHARED       0x00000008
#define VM_GROWSDOWN    0x00000100
#define VM_GROWSUP      0x00000200
#define VM_LOCKED       0x00002000
#define VM_IO           0x00004000
#define VM_DONTEXPAND   0x00040000
#define VM_HEAP         0x00080000
#define VM_STACK        0x00100000
#define VM_FILE         0x00200000
#endif

// ============================================================================
// PTE FLAGS (x86_64)
// ============================================================================

#ifndef PTE_PRESENT
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITE       (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_PWT         (1ULL << 3)
#define PTE_PCD         (1ULL << 4)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)
#endif

// ============================================================================
// ADDITIONAL PROCESS TASK FIELDS  (extend task_struct_t above if needed)
// ============================================================================

// These macros allow accessing optional fields safely
#define task_tls(t)             ((t)->tls)
#define task_pdeath_sig(t)      ((t)->pdeath_signal)

// ============================================================================
// struct utsname
// ============================================================================

#ifndef _STRUCT_UTSNAME
#define _STRUCT_UTSNAME
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
#endif

// ============================================================================
// struct linux_dirent64 — used by getdents64
// ============================================================================

#ifndef _STRUCT_LINUX_DIRENT64
#define _STRUCT_LINUX_DIRENT64
struct linux_dirent64 {
    u64     d_ino;
    s64     d_off;
    u16     d_reclen;
    u8      d_type;
    char    d_name[];   /* variable length */
};
#endif

// d_type values
#ifndef DT_UNKNOWN
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14
#endif

// ============================================================================
// struct iovec — scatter/gather I/O
// ============================================================================

#ifndef _STRUCT_IOVEC
#define _STRUCT_IOVEC
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};
#endif

// ============================================================================
// struct rlimit
// ============================================================================

#ifndef _STRUCT_RLIMIT
#define _STRUCT_RLIMIT
struct rlimit {
    u64 rlim_cur;
    u64 rlim_max;
};
#endif

// ============================================================================
// struct rusage
// ============================================================================

#ifndef _STRUCT_RUSAGE
#define _STRUCT_RUSAGE
struct rusage {
    struct timeval ru_utime;    /* user time used */
    struct timeval ru_stime;    /* system time used */
    s64 ru_maxrss;
    s64 ru_ixrss;
    s64 ru_idrss;
    s64 ru_isrss;
    s64 ru_minflt;
    s64 ru_majflt;
    s64 ru_nswap;
    s64 ru_inblock;
    s64 ru_oublock;
    s64 ru_msgsnd;
    s64 ru_msgrcv;
    s64 ru_nsignals;
    s64 ru_nvcsw;
    s64 ru_nivcsw;
};
#endif

// ============================================================================
// struct sysinfo
// ============================================================================

#ifndef _STRUCT_SYSINFO
#define _STRUCT_SYSINFO
struct sysinfo {
    s64  uptime;
    u64  loads[3];
    u64  totalram;
    u64  freeram;
    u64  sharedram;
    u64  bufferram;
    u64  totalswap;
    u64  freeswap;
    u16  procs;
    u16  pad;
    u32  pad2;
    u64  totalhigh;
    u64  freehigh;
    u32  mem_unit;
};
#endif

// ============================================================================
// NSEC / timing constants (additional)
// ============================================================================

#define NSEC_PER_JIFFIE (NSEC_PER_SEC / 100)   /* 100 Hz clock */

// ============================================================================
// PATH_MAX
// ============================================================================

#ifndef PATH_MAX
#define PATH_MAX        4096
#endif

// ============================================================================
// O_* flags (used across syscalls)
// ============================================================================

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_NOCTTY        0x0100
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_DSYNC         0x1000
#define O_ASYNC         0x2000
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_CLOEXEC       0x80000
#define O_SYNC          0x101000
#define O_NOATIME       0x40000

// seek
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// ============================================================================
// FMODE flags
// ============================================================================

#define FMODE_READ      0x1
#define FMODE_WRITE     0x2
#define FMODE_EXEC      0x20
#define FMODE_CREATED   0x100000

// ============================================================================
// Additional task fields used across new code
// These must match the task_struct_t layout in process.h
// Defined here as documentation; actual struct is in process.h
// ============================================================================

// u64  utime           — user CPU time (ns) — must be in task_struct_t
// u64  stime           — kernel CPU time (ns)
// u64  tls             — FS base for arch_prctl
// int  pdeath_signal   — signal sent when parent dies
// int  stop_signal     — signal that caused SIGSTOP
// bool wait_reported   — whether stopped/continued has been reported
// int  exit_code       — process exit code
// struct list_head children  — child list
// struct list_head sibling   — entry in parent's child list
// pid_t ppid           — parent pid
// pid_t tgid           — thread group id


// ============================================================================
// KERNEL TIMER (defined here to avoid circular dependency with timer.h)
// ============================================================================
#ifndef _TIMER_LIST_DEFINED
#define _TIMER_LIST_DEFINED
typedef struct timer_list {
    struct list_head    entry;
    u64                 expires;
    void                (*function)(unsigned long);
    unsigned long       data;
    u32                 flags;
} timer_list_t;
#endif

// ============================================================================
// ATOMIC OPERATIONS (implemented in kernel/process.c)
// ============================================================================
#ifndef __ATOMIC_OPS_DECLARED__
#define __ATOMIC_OPS_DECLARED__
extern void atomic_set(atomic_t *v, int i);
extern int  atomic_read(atomic_t *v);
extern void atomic_inc(atomic_t *v);
extern void atomic_dec(atomic_t *v);
extern int  atomic_dec_and_test(atomic_t *v);
extern int  atomic_inc_return(atomic_t *v);
extern int  atomic_dec_return(atomic_t *v);
extern void atomic_add(int i, atomic_t *v);
extern void atomic_sub(int i, atomic_t *v);
extern int  atomic_cmpxchg(atomic_t *v, int old, int new_val);
#endif

// ============================================================================
// SPINLOCK OPERATIONS (implemented in kernel/process.c)
// ============================================================================
#ifndef __SPINLOCK_DECLS__
#define __SPINLOCK_DECLS__
extern void spin_lock_init(spinlock_t *lock);
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);
extern int  spin_trylock(spinlock_t *lock);
#define spin_lock_irqsave(l, f)   do { (void)(f); spin_lock(l); } while(0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); spin_unlock(l); } while(0)
#endif

#endif // _KERNEL_TYPES_H
