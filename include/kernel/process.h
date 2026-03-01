/**
 * Picomimi-x64 Process Management
 * 
 * Full POSIX-compliant process and thread management
 */
#ifndef _KERNEL_PROCESS_H
#define _KERNEL_PROCESS_H

#include <kernel/types.h>
#include <kernel/syscall.h>

// ============================================================================
// LIMITS (use types.h defaults if not defined)
// ============================================================================

#define MAX_PROCESSES       4096
#define MAX_THREADS         65536
#define MAX_FDS_PER_PROCESS 1024
#define MAX_PATH_LEN        4096
#define MAX_ARGV            256
#define MAX_ENVP            256

// PID allocation
#define PID_MAX             32768
#define RESERVED_PIDS       300

// ============================================================================
// CPU CONTEXT
// ============================================================================

// Saved CPU state for context switching
typedef struct {
    // Callee-saved registers (System V AMD64 ABI)
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 rbp;
    u64 rbx;
    
    // Return address
    u64 rip;
    
    // Stack pointer
    u64 rsp;
    
    // Flags
    u64 rflags;
    
    // Return value (for fork)
    u64 rax;
    
    // Segment registers (for user mode)
    u64 cs;
    u64 ss;
    u64 ds;
    u64 es;
    u64 fs;
    u64 gs;
    
    // FS/GS base for TLS
    u64 fs_base;
    u64 gs_base;
    
    // FPU/SSE state pointer (lazily saved)
    void *fpu_state;
    bool fpu_used;
} cpu_context_t;

// Full trap frame (pushed by interrupt/exception)
typedef struct {
    // Pushed by our handler
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    
    // Interrupt info
    u64 int_no;
    u64 error_code;
    
    // Pushed by CPU
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} __packed trap_frame_t;

// ============================================================================
// MEMORY MANAGEMENT STRUCTURES
// ============================================================================

// Virtual Memory Area (VMA)
typedef struct vm_area {
    u64                 start;          // Start address
    u64                 end;            // End address
    u32                 flags;          // Protection flags
    u32                 vm_flags;       // VM flags (MAP_*)
    struct file         *file;          // Mapped file (or NULL)
    u64                 offset;         // Offset in file
    struct vm_area      *next;          // Next VMA in list
    struct vm_area      *prev;          // Previous VMA
} vm_area_t;

// VM flags
#define VM_READ         0x0001
#define VM_WRITE        0x0002
#define VM_EXEC         0x0004
#define VM_SHARED       0x0008
#define VM_GROWSDOWN    0x0100  // Stack
#define VM_GROWSUP      0x0200  // Heap
#define VM_DENYWRITE    0x0800
#define VM_LOCKED       0x2000
#define VM_IO           0x4000

// Memory descriptor (per-process address space)
typedef struct mm_struct {
    vm_area_t           *mmap;          // List of VMAs
    u64                 *pgd;           // Page Global Directory (PML4)
    
    // Memory regions
    u64                 start_code;     // Code start
    u64                 end_code;       // Code end
    u64                 start_data;     // Data start
    u64                 end_data;       // Data end
    u64                 start_brk;      // Heap start
    u64                 brk;            // Current break
    u64                 start_stack;    // Stack start
    u64                 arg_start;      // Arguments start
    u64                 arg_end;        // Arguments end
    u64                 env_start;      // Environment start
    u64                 env_end;        // Environment end
    
    // Memory statistics
    u64                 total_vm;       // Total pages mapped
    u64                 locked_vm;      // Locked pages
    u64                 pinned_vm;      // Pinned pages
    u64                 data_vm;        // Data pages
    u64                 exec_vm;        // Executable pages
    u64                 stack_vm;       // Stack pages
    
    // Reference counting
    atomic_t            mm_users;       // Users of this mm
    atomic_t            mm_count;       // References to mm_struct
    
    spinlock_t          page_table_lock;
    spinlock_t          mmap_lock;
} mm_struct_t;

// ============================================================================
// FILE DESCRIPTOR
// ============================================================================

// Include VFS for file_t and file_operations_t
// Note: vfs.h must be included after process.h in .c files
// Forward declare here
struct dentry;
struct inode;
struct super_block;
struct vm_area;

// File operations (forward declare)
typedef struct file_operations {
    s64 (*read)(struct file *, char *, size_t, u64 *);
    s64 (*write)(struct file *, const char *, size_t, u64 *);
    s64 (*llseek)(struct file *, s64, int);
    s64 (*ioctl)(struct file *, unsigned int, unsigned long);
    s64 (*mmap)(struct file *, struct vm_area *);
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    int (*fsync)(struct file *);
    s64 (*readdir)(struct file *, void *, int (*)(void *, const char *, int, u64, unsigned int));
    unsigned int (*poll)(struct file *, void *);
} file_operations_t;

// Open file structure
typedef struct file {
    struct dentry       *f_dentry;      // Associated dentry
    struct inode        *f_inode;       // Cached inode
    const file_operations_t *f_op;      // Operations
    
    u32                 f_flags;        // Open flags
    u32                 f_mode;         // File mode
    u64                 f_pos;          // Current position
    
    atomic_t            f_count;        // Reference count
    spinlock_t          f_lock;
    
    void                *private_data;  // Driver private data
} file_t;

// File descriptor table entry
typedef struct {
    file_t              *file;
    u32                 flags;          // FD_CLOEXEC, etc.
} fd_entry_t;

// File descriptor table
typedef struct files_struct {
    atomic_t            count;          // Reference count
    spinlock_t          lock;
    
    int                 max_fds;        // Current max
    int                 next_fd;        // Next free fd hint
    
    u64                 close_on_exec;  // Bitmap (for first 64)
    u64                 open_fds;       // Bitmap (for first 64)
    
    fd_entry_t          *fd_array;      // FD table
    fd_entry_t          fd_array_init[64]; // Initial inline storage
} files_struct_t;

// ============================================================================
// SIGNALS
// ============================================================================

#define SIGHUP          1
#define SIGINT          2
#define SIGQUIT         3
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6
#define SIGIOT          6
#define SIGBUS          7
#define SIGFPE          8
#define SIGKILL         9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGSTKFLT       16
#define SIGCHLD         17
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGXCPU         24
#define SIGXFSZ         25
#define SIGVTALRM       26
#define SIGPROF         27
#define SIGWINCH        28
#define SIGIO           29
#define SIGPOLL         SIGIO
#define SIGPWR          30
#define SIGSYS          31
#define NSIG            64

// Signal action flags
#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000
#define SA_RESTORER     0x04000000

#define SIG_DFL         ((void (*)(int))0)
#define SIG_IGN         ((void (*)(int))1)
#define SIG_ERR         ((void (*)(int))-1)

// Pending signal
typedef struct sigpending {
    sigset_t            signal;         // Pending signals bitmap
    struct sigqueue     *queue;         // Queue of siginfo
} sigpending_t;

// Signal handler info
typedef struct signal_struct {
    atomic_t            count;          // Reference count
    atomic_t            live;           // Number of live threads
    
    struct sigaction    action[NSIG];   // Signal actions
    spinlock_t          siglock;
    
    // Group signal info
    sigpending_t        shared_pending;
    
    // Process group/session
    pid_t               pgrp;
    pid_t               session;
    
    // Controlling terminal
    // struct tty_struct *tty;
    
    // Exit info
    int                 group_exit_code;
    int                 group_stop_count;
    unsigned int        flags;
} signal_struct_t;

// Per-thread signal state
typedef struct sighand_struct {
    atomic_t            count;
    struct sigaction    action[NSIG];
    spinlock_t          siglock;
} sighand_struct_t;

// ============================================================================
// CREDENTIALS
// ============================================================================

typedef struct cred {
    atomic_t            usage;
    
    uid_t               uid;            // Real UID
    gid_t               gid;            // Real GID
    uid_t               euid;           // Effective UID
    gid_t               egid;           // Effective GID
    uid_t               suid;           // Saved UID
    gid_t               sgid;           // Saved GID
    uid_t               fsuid;          // Filesystem UID
    gid_t               fsgid;          // Filesystem GID
    
    unsigned int        securebits;
    // kernel_cap_t     cap_inheritable;
    // kernel_cap_t     cap_permitted;
    // kernel_cap_t     cap_effective;
    // kernel_cap_t     cap_bset;
    // kernel_cap_t     cap_ambient;
    
    // struct user_struct *user;
    // struct group_info *group_info;
} cred_t;

// ============================================================================
// FILESYSTEM STATE
// ============================================================================

typedef struct fs_struct {
    atomic_t            count;
    spinlock_t          lock;
    
    int                 umask;
    int                 in_exec;
    
    struct dentry       *root;          // Root directory
    struct dentry       *pwd;           // Current working directory
} fs_struct_t;

// ============================================================================
// TASK (THREAD) STRUCTURE
// ============================================================================

typedef struct task_struct {
    // Thread info (must be first for assembly access)
    volatile long       state;          // Task state
    void                *stack;         // Kernel stack pointer
    
    // Flags
    unsigned int        flags;
    unsigned int        ptrace;
    
    // Scheduling
    int                 on_rq;
    int                 prio;           // Dynamic priority
    int                 static_prio;    // Static priority (nice)
    int                 normal_prio;
    unsigned int        rt_priority;    // Real-time priority
    
    int                 policy;         // Scheduling policy
    u64                 time_slice;     // Remaining timeslice
    u64                 sum_exec_runtime;
    u64                 vruntime;       // CFS virtual runtime
    
    // CPU affinity
    u64                 cpus_allowed;
    int                 nr_cpus_allowed;
    int                 cpu;            // Current CPU
    
    // Process identification
    pid_t               pid;            // Thread ID
    pid_t               tgid;           // Thread Group ID (= PID of main thread)
    
    // Real parent (fork)
    struct task_struct  *real_parent;
    // Recipient of SIGCHLD and wait4()
    struct task_struct  *parent;
    
    // Children and siblings
    struct list_head    children;       // List of children
    struct list_head    sibling;        // Linkage in parent's children list
    struct task_struct  *group_leader;  // Thread group leader
    
    // Thread group linkage
    struct list_head    thread_group;
    struct list_head    thread_node;
    
    // Pointer for run queue
    struct list_head    run_list;
    
    // PID/TID hash linkage
    struct list_head    pid_link;
    
    // Memory descriptor
    mm_struct_t         *mm;            // Memory (NULL for kernel threads)
    mm_struct_t         *active_mm;     // Active mm (always valid)
    
    // Exit info
    int                 exit_state;
    int                 exit_code;
    int                 exit_signal;
    int                 pdeath_signal;  // Signal to send on parent death
    
    // CPU context
    cpu_context_t       context;
    trap_frame_t        *trap_frame;    // Current trap frame
    
    // Kernel stack
    void                *kernel_stack;
    
    // Credentials
    const cred_t        *cred;
    const cred_t        *real_cred;
    
    // Filesystem
    fs_struct_t         *fs;
    
    // Open files
    files_struct_t      *files;
    
    // Signals
    signal_struct_t     *signal;
    sighand_struct_t    *sighand;
    sigpending_t        pending;
    sigset_t            blocked;
    sigset_t            real_blocked;
    sigset_t            saved_sigmask;
    
    // Timers
    u64                 utime;          // User time
    u64                 stime;          // System time
    u64                 start_time;     // Monotonic start time
    u64                 real_start_time; // Boot-based start time
    
    // Name
    char                comm[16];       // Executable name
    
    // Wait queue for sleeping
    void                *wait_queue;
    
    // TLS (Thread Local Storage)
    u64                 tls;            // FS base for user TLS
    
    // set_tid_address pointer
    int                 *clear_child_tid;
    int                 *set_child_tid;
    
    // Resource limits
    struct rlimit       rlim[16];
    
    // Process accounting
    u64                 acct_rss_mem1;
    u64                 acct_vm_mem1;
    u64                 acct_timexpd;
    
} task_struct_t;

// Task flags
#define PF_EXITING      0x00000004  // Getting shut down
#define PF_VCPU         0x00000010  // I'm a virtual CPU
#define PF_WQ_WORKER    0x00000020  // I'm a workqueue worker
#define PF_FORKNOEXEC   0x00000040  // Forked but didn't exec
#define PF_MCE_PROCESS  0x00000080  // Process policy on mce errors
#define PF_SUPERPRIV    0x00000100  // Used super-user privileges
#define PF_DUMPCORE     0x00000200  // Dumped core
#define PF_SIGNALED     0x00000400  // Killed by a signal
#define PF_MEMALLOC     0x00000800  // Allocating memory
#define PF_NPROC_EXCEEDED 0x00001000 // Set_user() noticed RLIMIT_NPROC exceeded
#define PF_USED_MATH    0x00002000  // Used FPU this quantum
#define PF_USED_ASYNC   0x00004000  // Used async_schedule*(), used by module init
#define PF_NOFREEZE     0x00008000  // This thread should not be frozen
#define PF_FROZEN       0x00010000  // Frozen for system suspend
#define PF_KSWAPD       0x00020000  // I am kswapd
#define PF_MEMALLOC_NOFS 0x00040000 // All allocation requests will inherit GFP_NOFS
#define PF_MEMALLOC_NOIO 0x00080000 // All allocation requests will inherit GFP_NOIO
#define PF_KTHREAD      0x00200000  // I am a kernel thread
#define PF_RANDOMIZE    0x00400000  // Randomize virtual address space
#define PF_SWAPWRITE    0x00800000  // Allowed to write to swap
#define PF_NO_SETAFFINITY 0x04000000 // Userland is not allowed to meddle with cpus_allowed
#define PF_MCE_EARLY    0x08000000  // Early kill for mce process policy
#define PF_SUSPEND_TASK 0x80000000  // This thread called freeze_processes() and should not be frozen

// ============================================================================
// PROCESS TABLE & SCHEDULER
// ============================================================================

// Global process/thread table
extern task_struct_t *task_table[MAX_THREADS];
extern spinlock_t task_table_lock;

// PID hash table
#define PID_HASH_SIZE   1024
extern struct list_head pid_hash[PID_HASH_SIZE];

// Current task per CPU
extern task_struct_t *current_tasks[NR_CPUS];

// Get current task (implemented in process.c)
extern u32 smp_processor_id(void);
extern task_struct_t *get_current_task(void);

#define current get_current_task()

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Process creation
task_struct_t *task_create(const char *name, void (*entry)(void *), void *arg, unsigned int flags);
task_struct_t *task_fork(unsigned long clone_flags, unsigned long stack, int *parent_tidptr, int *child_tidptr, unsigned long tls);
int task_exec(const char *path, char *const argv[], char *const envp[]);
void task_exit(int exit_code);
void task_exit_group(int exit_code);

// Process lookup
task_struct_t *find_task_by_pid(pid_t pid);
task_struct_t *find_task_by_tgid(pid_t tgid);

// PID allocation
pid_t alloc_pid(void);
void free_pid(pid_t pid);

// Scheduler
void schedule(void);
void schedule_tail(task_struct_t *prev);
void wake_up_process(task_struct_t *task);
void set_task_state(task_struct_t *task, long state);
int set_cpus_allowed(task_struct_t *task, u64 mask);
void yield(void);

// Context switch (assembly)
extern void context_switch(cpu_context_t *old, cpu_context_t *new);
extern void switch_to_user(trap_frame_t *frame);

// Wait queues
void init_waitqueue(void *wq);
void add_wait_queue(void *wq, task_struct_t *task);
void remove_wait_queue(void *wq, task_struct_t *task);
void wake_up(void *wq);
void wake_up_all(void *wq);

// Signals
int signal_pending(task_struct_t *task);
void do_signal(trap_frame_t *frame);
int send_signal(pid_t pid, int sig);
int send_signal_info(pid_t pid, int sig, void *info);
void recalc_sigpending(void);

// Memory
mm_struct_t *mm_alloc(void);
void mm_free(mm_struct_t *mm);
mm_struct_t *mm_copy(mm_struct_t *old);
vm_area_t *find_vma(mm_struct_t *mm, u64 addr);
vm_area_t *vma_alloc(void);
void vma_free(vm_area_t *vma);
int insert_vma(mm_struct_t *mm, vm_area_t *vma);
int do_mmap(u64 addr, size_t len, int prot, int flags, file_t *file, u64 offset);
int do_munmap(u64 addr, size_t len);
int do_brk(u64 addr);

// Files
files_struct_t *files_alloc(void);
void files_free(files_struct_t *files);
files_struct_t *files_copy(files_struct_t *old);
int get_unused_fd(void);
void put_unused_fd(int fd);
void fd_install(int fd, file_t *file);
file_t *fget(int fd);
void fput(file_t *file);
int do_close(int fd);

// Credentials
cred_t *cred_alloc(void);
void cred_free(cred_t *cred);
cred_t *cred_copy(const cred_t *old);

// FS struct
fs_struct_t *fs_alloc(void);
void fs_free(fs_struct_t *fs);
fs_struct_t *fs_copy(fs_struct_t *old);

// Init
void process_init(void);
task_struct_t *create_init_task(void);
task_struct_t *create_kthread(const char *name, int (*fn)(void *), void *arg);

#endif // _KERNEL_PROCESS_H
