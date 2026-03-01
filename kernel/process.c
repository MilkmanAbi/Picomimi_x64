/**
 * Picomimi-x64 Process Management
 * 
 * Core process/thread creation, scheduling, and management
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <arch/cpu.h>

// ============================================================================
// SMP PROCESSOR ID
// ============================================================================

static u32 current_cpu_id = 0;

u32 smp_processor_id(void) {
    return current_cpu_id;
}

// ============================================================================
// SPINLOCK IMPLEMENTATION (non-static for use by other modules)
// ============================================================================

void spin_lock_init(spinlock_t *lock) {
    lock->raw_lock.lock = 0;
}

void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->raw_lock.lock, 1, __ATOMIC_ACQUIRE)) {
        while (lock->raw_lock.lock) {
            __asm__ volatile("pause");
        }
    }
}

void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->raw_lock.lock, 0, __ATOMIC_RELEASE);
}

int spin_trylock(spinlock_t *lock) {
    return !__atomic_exchange_n(&lock->raw_lock.lock, 1, __ATOMIC_ACQUIRE);
}

// ============================================================================
// ATOMIC OPERATIONS
// ============================================================================

void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}

int atomic_read(atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}

void atomic_inc(atomic_t *v) {
    __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

void atomic_dec(atomic_t *v) {
    __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

int atomic_dec_return(atomic_t *v) {
    return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

int atomic_inc_return(atomic_t *v) {
    return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Process table
task_struct_t *task_table[MAX_THREADS];
spinlock_t task_table_lock = { .raw_lock = { 0 } };

// PID hash table
struct list_head pid_hash[PID_HASH_SIZE];

// Current task per CPU
task_struct_t *current_tasks[NR_CPUS];

// PID bitmap for allocation
static u64 pid_bitmap[PID_MAX / 64];
static spinlock_t pid_lock = { .raw_lock = { 0 } };
static pid_t last_pid = 0;

// Init task (PID 0 - idle, PID 1 - init)
static task_struct_t init_task;
static task_struct_t idle_task;

// ============================================================================
// PID ALLOCATION
// ============================================================================

pid_t alloc_pid(void) {
    spin_lock(&pid_lock);
    
    // Start searching from last allocated + 1
    pid_t start = last_pid + 1;
    if (start >= PID_MAX) start = RESERVED_PIDS;
    
    pid_t pid = start;
    do {
        u64 idx = pid / 64;
        u64 bit = pid % 64;
        
        if (!(pid_bitmap[idx] & (1UL << bit))) {
            pid_bitmap[idx] |= (1UL << bit);
            last_pid = pid;
            spin_unlock(&pid_lock);
            return pid;
        }
        
        pid++;
        if (pid >= PID_MAX) pid = RESERVED_PIDS;
    } while (pid != start);
    
    spin_unlock(&pid_lock);
    return -1;  // No PIDs available
}

void free_pid(pid_t pid) {
    if (pid < RESERVED_PIDS || pid >= PID_MAX) return;
    
    spin_lock(&pid_lock);
    u64 idx = pid / 64;
    u64 bit = pid % 64;
    pid_bitmap[idx] &= ~(1UL << bit);
    spin_unlock(&pid_lock);
}

// ============================================================================
// PID HASH TABLE
// ============================================================================

static inline unsigned int __attribute__((unused)) pid_hashfn(pid_t pid) {
    return ((unsigned int)pid * 0x9e370001UL) >> (32 - 10);  // 10 bits = 1024 buckets
}

task_struct_t *find_task_by_pid(pid_t pid) {
    // Simple linear search for now
    for (int i = 0; i < MAX_THREADS; i++) {
        if (task_table[i] && task_table[i]->pid == pid) {
            return task_table[i];
        }
    }
    
    return NULL;
}

task_struct_t *find_task_by_tgid(pid_t tgid) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (task_table[i] && task_table[i]->tgid == tgid) {
            return task_table[i];
        }
    }
    return NULL;
}

// ============================================================================
// CREDENTIAL MANAGEMENT
// ============================================================================

cred_t *cred_alloc(void) {
    cred_t *cred = kmalloc(sizeof(cred_t), GFP_KERNEL);
    if (!cred) return NULL;
    
    memset(cred, 0, sizeof(cred_t));
    atomic_set(&cred->usage, 1);
    return cred;
}

void cred_free(cred_t *cred) {
    if (cred && atomic_dec_return(&cred->usage) == 0) {
        kfree(cred);
    }
}

cred_t *cred_copy(const cred_t *old) {
    cred_t *new = cred_alloc();
    if (!new) return NULL;
    
    new->uid = old->uid;
    new->gid = old->gid;
    new->euid = old->euid;
    new->egid = old->egid;
    new->suid = old->suid;
    new->sgid = old->sgid;
    new->fsuid = old->fsuid;
    new->fsgid = old->fsgid;
    
    return new;
}

// ============================================================================
// FILE DESCRIPTOR TABLE
// ============================================================================

files_struct_t *files_alloc(void) {
    files_struct_t *files = kmalloc(sizeof(files_struct_t), GFP_KERNEL);
    if (!files) return NULL;
    
    memset(files, 0, sizeof(files_struct_t));
    atomic_set(&files->count, 1);
    spin_lock_init(&files->lock);
    
    files->max_fds = 64;
    files->next_fd = 0;
    files->fd_array = files->fd_array_init;
    
    return files;
}

void files_free(files_struct_t *files) {
    if (!files) return;
    
    if (atomic_dec_return(&files->count) == 0) {
        // Close all open files
        for (int i = 0; i < files->max_fds; i++) {
            if (files->fd_array[i].file) {
                // fput(files->fd_array[i].file);
            }
        }
        
        if (files->fd_array != files->fd_array_init) {
            kfree(files->fd_array);
        }
        kfree(files);
    }
}

files_struct_t *files_copy(files_struct_t *old) {
    files_struct_t *new = files_alloc();
    if (!new) return NULL;
    
    spin_lock(&old->lock);
    
    new->max_fds = old->max_fds;
    new->next_fd = old->next_fd;
    new->close_on_exec = old->close_on_exec;
    new->open_fds = old->open_fds;
    
    // Copy file descriptors
    for (int i = 0; i < old->max_fds && i < 64; i++) {
        if (old->fd_array[i].file) {
            new->fd_array[i] = old->fd_array[i];
            atomic_inc(&old->fd_array[i].file->f_count);
        }
    }
    
    spin_unlock(&old->lock);
    return new;
}

int get_unused_fd(void) {
    task_struct_t *task = current;
    files_struct_t *files = task->files;
    
    spin_lock(&files->lock);
    
    // Find first free fd
    for (int i = files->next_fd; i < files->max_fds; i++) {
        if (!files->fd_array[i].file) {
            files->next_fd = i + 1;
            spin_unlock(&files->lock);
            return i;
        }
    }
    
    // Try from beginning
    for (int i = 0; i < files->next_fd; i++) {
        if (!files->fd_array[i].file) {
            files->next_fd = i + 1;
            spin_unlock(&files->lock);
            return i;
        }
    }
    
    spin_unlock(&files->lock);
    return -EMFILE;
}

void put_unused_fd(int fd) {
    task_struct_t *task = current;
    files_struct_t *files = task->files;
    
    spin_lock(&files->lock);
    if (fd >= 0 && fd < files->max_fds) {
        files->fd_array[fd].file = NULL;
        files->fd_array[fd].flags = 0;
        if (fd < files->next_fd) {
            files->next_fd = fd;
        }
    }
    spin_unlock(&files->lock);
}

void fd_install(int fd, file_t *file) {
    task_struct_t *task = current;
    files_struct_t *files = task->files;
    
    spin_lock(&files->lock);
    if (fd >= 0 && fd < files->max_fds) {
        files->fd_array[fd].file = file;
        files->open_fds |= (1UL << fd);
    }
    spin_unlock(&files->lock);
}

file_t *fget(int fd) {
    task_struct_t *task = current;
    files_struct_t *files = task->files;
    file_t *file = NULL;
    
    spin_lock(&files->lock);
    if (fd >= 0 && fd < files->max_fds) {
        file = files->fd_array[fd].file;
        if (file) {
            atomic_inc(&file->f_count);
        }
    }
    spin_unlock(&files->lock);
    
    return file;
}

void fput(file_t *file) {
    if (file && atomic_dec_return(&file->f_count) == 0) {
        // Call release if defined
        // if (file->f_op && file->f_op->release) {
        //     file->f_op->release(file->f_inode, file);
        // }
        kfree(file);
    }
}

int do_close(int fd) {
    task_struct_t *task = current;
    files_struct_t *files = task->files;
    file_t *file = NULL;
    
    spin_lock(&files->lock);
    if (fd >= 0 && fd < files->max_fds) {
        file = files->fd_array[fd].file;
        files->fd_array[fd].file = NULL;
        files->fd_array[fd].flags = 0;
        files->open_fds &= ~(1UL << fd);
        files->close_on_exec &= ~(1UL << fd);
    }
    spin_unlock(&files->lock);
    
    if (file) {
        fput(file);
        return 0;
    }
    return -EBADF;
}

// ============================================================================
// FS STRUCT
// ============================================================================

fs_struct_t *fs_alloc(void) {
    fs_struct_t *fs = kmalloc(sizeof(fs_struct_t), GFP_KERNEL);
    if (!fs) return NULL;
    
    memset(fs, 0, sizeof(fs_struct_t));
    atomic_set(&fs->count, 1);
    spin_lock_init(&fs->lock);
    fs->umask = 0022;
    
    return fs;
}

void fs_free(fs_struct_t *fs) {
    if (fs && atomic_dec_return(&fs->count) == 0) {
        // dput(fs->root);
        // dput(fs->pwd);
        kfree(fs);
    }
}

fs_struct_t *fs_copy(fs_struct_t *old) {
    fs_struct_t *new = fs_alloc();
    if (!new) return NULL;
    
    spin_lock(&old->lock);
    new->umask = old->umask;
    new->root = old->root;
    new->pwd = old->pwd;
    // dget(new->root);
    // dget(new->pwd);
    spin_unlock(&old->lock);
    
    return new;
}

// ============================================================================
// MEMORY DESCRIPTOR
// ============================================================================

mm_struct_t *mm_alloc(void) {
    mm_struct_t *mm = kmalloc(sizeof(mm_struct_t), GFP_KERNEL);
    if (!mm) return NULL;
    
    memset(mm, 0, sizeof(mm_struct_t));
    atomic_set(&mm->mm_users, 1);
    atomic_set(&mm->mm_count, 1);
    spin_lock_init(&mm->page_table_lock);
    spin_lock_init(&mm->mmap_lock);
    
    // Allocate page tables
    mm->pgd = (u64 *)pmm_alloc_page();
    if (!mm->pgd) {
        kfree(mm);
        return NULL;
    }
    memset(mm->pgd, 0, 4096);
    
    // Copy kernel page tables (higher half)
    extern u64 *kernel_pml4;
    for (int i = 256; i < 512; i++) {
        mm->pgd[i] = kernel_pml4[i];
    }
    
    return mm;
}

void mm_free(mm_struct_t *mm) {
    if (!mm) return;
    
    if (atomic_dec_return(&mm->mm_count) == 0) {
        // Free VMAs
        vm_area_t *vma = mm->mmap;
        while (vma) {
            vm_area_t *next = vma->next;
            kfree(vma);
            vma = next;
        }
        
        // Free page tables
        if (mm->pgd) {
            pmm_free_page((phys_addr_t)mm->pgd);
        }
        
        kfree(mm);
    }
}

mm_struct_t *mm_copy(mm_struct_t *old) {
    mm_struct_t *new = mm_alloc();
    if (!new) return NULL;
    
    spin_lock(&old->mmap_lock);
    
    new->start_code = old->start_code;
    new->end_code = old->end_code;
    new->start_data = old->start_data;
    new->end_data = old->end_data;
    new->start_brk = old->start_brk;
    new->brk = old->brk;
    new->start_stack = old->start_stack;
    
    // Copy VMAs
    vm_area_t *old_vma = old->mmap;
    vm_area_t **new_vma_ptr = &new->mmap;
    
    while (old_vma) {
        vm_area_t *vma = kmalloc(sizeof(vm_area_t), GFP_KERNEL);
        if (!vma) {
            spin_unlock(&old->mmap_lock);
            mm_free(new);
            return NULL;
        }
        
        *vma = *old_vma;
        vma->next = NULL;
        vma->prev = NULL;
        
        *new_vma_ptr = vma;
        new_vma_ptr = &vma->next;
        
        old_vma = old_vma->next;
    }
    
    // TODO: Copy page tables with COW
    
    spin_unlock(&old->mmap_lock);
    return new;
}

// ============================================================================
// TASK CREATION
// ============================================================================

static void setup_kernel_stack(task_struct_t *task) {
    // Allocate kernel stack
    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE, GFP_KERNEL);
    if (!task->kernel_stack) {
        printk(KERN_ERR "Failed to allocate kernel stack\n");
        return;
    }
    
    memset(task->kernel_stack, 0, KERNEL_STACK_SIZE);
    
    // Stack grows down - start at top
    task->stack = (void *)((u64)task->kernel_stack + KERNEL_STACK_SIZE);
}

task_struct_t *task_create(const char *name, void (*entry)(void *), void *arg, unsigned int flags) {
    task_struct_t *task = kmalloc(sizeof(task_struct_t), GFP_KERNEL);
    if (!task) return NULL;
    
    memset(task, 0, sizeof(task_struct_t));
    
    // Allocate PID
    task->pid = alloc_pid();
    if (task->pid < 0) {
        kfree(task);
        return NULL;
    }
    task->tgid = task->pid;  // Thread group leader
    
    // Set name
    strncpy(task->comm, name, sizeof(task->comm) - 1);
    
    // Setup kernel stack
    setup_kernel_stack(task);
    
    // Initialize state
    task->state = TASK_RUNNING;
    task->flags = flags | PF_KTHREAD;
    task->prio = 120;  // Default nice 0
    task->static_prio = 120;
    task->normal_prio = 120;
    task->policy = SCHED_NORMAL;
    task->time_slice = 100;  // 100ms
    
    // CPU
    task->cpu = 0;  // TODO: select CPU
    task->cpus_allowed = ~0UL;
    task->nr_cpus_allowed = NR_CPUS;
    
    // Initialize lists
    INIT_LIST_HEAD(&task->children);
    INIT_LIST_HEAD(&task->sibling);
    INIT_LIST_HEAD(&task->thread_group);
    INIT_LIST_HEAD(&task->run_list);
    
    // Parent
    task->real_parent = current;
    task->parent = current;
    task->group_leader = task;
    
    // Credentials
    task->cred = cred_alloc();
    task->real_cred = task->cred;
    
    // Files
    task->files = files_alloc();
    
    // FS
    task->fs = fs_alloc();
    
    // Memory (kernel thread - no user space)
    task->mm = NULL;
    task->active_mm = current ? current->active_mm : NULL;
    
    // Setup context for first switch
    task->context.rip = (u64)entry;
    task->context.rsp = (u64)task->stack - 8;  // Leave room for return address
    task->context.rflags = 0x200;  // Interrupts enabled
    
    // Push arg onto stack
    u64 *stack = (u64 *)task->context.rsp;
    *stack = (u64)arg;
    
    // Add to task table
    spin_lock(&task_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!task_table[i]) {
            task_table[i] = task;
            break;
        }
    }
    spin_unlock(&task_table_lock);
    
    return task;
}

// ============================================================================
// FORK
// ============================================================================

task_struct_t *task_fork(unsigned long clone_flags, unsigned long stack_start, 
                          int *parent_tidptr, int *child_tidptr, unsigned long tls) {
    task_struct_t *parent = current;
    task_struct_t *child = kmalloc(sizeof(task_struct_t), GFP_KERNEL);
    if (!child) return ERR_PTR(-ENOMEM);
    
    // Copy parent task
    memcpy(child, parent, sizeof(task_struct_t));
    
    // Allocate new PID
    child->pid = alloc_pid();
    if (child->pid < 0) {
        kfree(child);
        return ERR_PTR(-EAGAIN);
    }
    
    // Setup thread group
    if (clone_flags & CLONE_THREAD) {
        child->tgid = parent->tgid;
        child->group_leader = parent->group_leader;
    } else {
        child->tgid = child->pid;
        child->group_leader = child;
    }
    
    // Kernel stack
    setup_kernel_stack(child);
    
    // Parent/child relationship
    child->real_parent = parent;
    child->parent = parent;
    INIT_LIST_HEAD(&child->children);
    INIT_LIST_HEAD(&child->sibling);
    
    // Add to parent's children
    list_add_tail(&child->sibling, &parent->children);
    
    // Copy or share memory
    if (clone_flags & CLONE_VM) {
        child->mm = parent->mm;
        atomic_inc(&parent->mm->mm_users);
    } else if (parent->mm) {
        child->mm = mm_copy(parent->mm);
        if (!child->mm) {
            free_pid(child->pid);
            kfree(child->kernel_stack);
            kfree(child);
            return ERR_PTR(-ENOMEM);
        }
    }
    child->active_mm = child->mm;
    
    // Copy or share files
    if (clone_flags & CLONE_FILES) {
        child->files = parent->files;
        atomic_inc(&parent->files->count);
    } else {
        child->files = files_copy(parent->files);
    }
    
    // Copy or share fs
    if (clone_flags & CLONE_FS) {
        child->fs = parent->fs;
        atomic_inc(&parent->fs->count);
    } else {
        child->fs = fs_copy(parent->fs);
    }
    
    // Copy or share signals
    if (clone_flags & CLONE_SIGHAND) {
        child->sighand = parent->sighand;
        atomic_inc(&parent->sighand->count);
    } else {
        // Copy signal handlers
        child->sighand = kmalloc(sizeof(sighand_struct_t), GFP_KERNEL);
        if (child->sighand) {
            memcpy(child->sighand, parent->sighand, sizeof(sighand_struct_t));
            atomic_set(&child->sighand->count, 1);
        }
    }
    
    // Credentials
    child->cred = cred_copy(parent->cred);
    child->real_cred = child->cred;
    
    // TLS
    if (clone_flags & CLONE_SETTLS) {
        child->tls = tls;
    }
    
    // User stack
    if (stack_start) {
        child->context.rsp = stack_start;
    }
    
    // Set child TID pointers
    if (clone_flags & CLONE_CHILD_SETTID) {
        child->set_child_tid = child_tidptr;
    }
    if (clone_flags & CLONE_CHILD_CLEARTID) {
        child->clear_child_tid = child_tidptr;
    }
    
    // Return value: 0 in child
    child->context.rax = 0;
    
    // Parent TID pointer
    if (clone_flags & CLONE_PARENT_SETTID && parent_tidptr) {
        *parent_tidptr = child->pid;
    }
    
    // Initialize scheduling
    child->state = TASK_RUNNING;
    child->on_rq = 0;
    child->time_slice = parent->time_slice / 2;
    if (child->time_slice < 1) child->time_slice = 1;
    parent->time_slice -= child->time_slice;
    
    // Add to task table
    spin_lock(&task_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!task_table[i]) {
            task_table[i] = child;
            break;
        }
    }
    spin_unlock(&task_table_lock);
    
    // Wake up child
    wake_up_process(child);
    
    return child;
}

// ============================================================================
// EXIT
// ============================================================================

void task_exit(int exit_code) {
    task_struct_t *task = current;
    
    printk(KERN_INFO "Process %d (%s) exiting with code %d\n", 
           task->pid, task->comm, exit_code);
    
    task->exit_code = exit_code;
    task->exit_state = EXIT_ZOMBIE;
    task->state = TASK_ZOMBIE;
    
    // Close all files
    if (task->files) {
        files_free(task->files);
        task->files = NULL;
    }
    
    // Release memory
    if (task->mm) {
        if (atomic_dec_return(&task->mm->mm_users) == 0) {
            mm_free(task->mm);
        }
        task->mm = NULL;
    }
    
    // Release credentials
    if (task->cred) {
        cred_free((cred_t *)task->cred);
        task->cred = NULL;
    }
    
    // Signal parent
    if (task->real_parent) {
        // TODO: send_signal(task->real_parent->pid, SIGCHLD);
    }
    
    // Reparent children to init
    task_struct_t *init = find_task_by_pid(1);
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &task->children) {
        task_struct_t *child = list_entry(pos, task_struct_t, sibling);
        child->real_parent = init;
        child->parent = init;
        list_move_tail(&child->sibling, &init->children);
    }
    
    // Remove from run queue and reschedule
    schedule();
    
    // Should never return
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void task_exit_group(int exit_code) {
    task_struct_t *task = current;
    
    // Kill all threads in the group
    for (int i = 0; i < MAX_THREADS; i++) {
        if (task_table[i] && task_table[i]->tgid == task->tgid && task_table[i] != task) {
            task_table[i]->exit_code = exit_code;
            task_table[i]->exit_state = EXIT_ZOMBIE;
            task_table[i]->state = TASK_ZOMBIE;
        }
    }
    
    task_exit(exit_code);
}

// ============================================================================
// SCHEDULER
// ============================================================================

void wake_up_process(task_struct_t *task) {
    if (!task) return;
    
    // Set running state
    task->state = TASK_RUNNING;
    task->on_rq = 1;
    
    // TODO: Add to run queue properly
}

void set_task_state(task_struct_t *task, long state) {
    task->state = state;
}

void yield(void) {
    task_struct_t *task = current;
    task->time_slice = 0;
    schedule();
}

void schedule(void) {
    task_struct_t *prev = current;
    task_struct_t *next = NULL;
    
    // Simple round-robin for now
    // TODO: Implement O(1) scheduler from Picomimi
    
    int current_idx = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (task_table[i] == prev) {
            current_idx = i;
            break;
        }
    }
    
    // Find next runnable task
    for (int i = 1; i < MAX_THREADS; i++) {
        int idx = (current_idx + i) % MAX_THREADS;
        if (task_table[idx] && task_table[idx]->state == TASK_RUNNING) {
            next = task_table[idx];
            break;
        }
    }
    
    // Stay on current if no other runnable
    if (!next) {
        next = prev;
    }
    
    if (next != prev) {
        current_tasks[smp_processor_id()] = next;
        
        // Context switch
        context_switch(&prev->context, &next->context);
    }
}

// Called after context switch completes (from ret_from_fork in assembly)
void schedule_tail(task_struct_t *prev) {
    (void)prev;
    // TODO: finish_task_switch - release locks, etc.
}

// Non-inline version for assembly access
task_struct_t *get_current_task(void) {
    return current_tasks[smp_processor_id()];
}

// ============================================================================
// SIGNALS (STUB)
// ============================================================================

int signal_pending(task_struct_t *task) {
    return task->pending.signal.sig[0] != 0 || task->pending.signal.sig[1] != 0;
}

int send_signal(pid_t pid, int sig) {
    task_struct_t *task = find_task_by_pid(pid);
    if (!task) return -ESRCH;
    
    if (sig < 1 || sig > NSIG) return -EINVAL;
    
    // Set signal pending
    if (sig <= 64) {
        task->pending.signal.sig[0] |= (1UL << (sig - 1));
    } else {
        task->pending.signal.sig[1] |= (1UL << (sig - 65));
    }
    
    // Wake up if sleeping
    if (task->state == TASK_INTERRUPTIBLE) {
        wake_up_process(task);
    }
    
    return 0;
}

void recalc_sigpending(void) {
    task_struct_t *task = current;
    
    // Check blocked vs pending
    u64 sig0 = task->pending.signal.sig[0] & ~task->blocked.sig[0];
    u64 sig1 = task->pending.signal.sig[1] & ~task->blocked.sig[1];
    
    // Set flag if any unblocked signals
    (void)sig0;
    (void)sig1;
    // TODO: Set TIF_SIGPENDING in thread info flags
}

// ============================================================================
// INIT
// ============================================================================

void process_init(void) {
    printk(KERN_INFO "Initializing process management...\n");
    
    // Initialize task table
    memset(task_table, 0, sizeof(task_table));
    
    // Initialize PID hash
    for (int i = 0; i < PID_HASH_SIZE; i++) {
        INIT_LIST_HEAD(&pid_hash[i]);
    }
    
    // Reserve PID 0 for idle
    pid_bitmap[0] |= 1;
    
    // Create idle task (PID 0)
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.pid = 0;
    idle_task.tgid = 0;
    strncpy(idle_task.comm, "swapper/0", sizeof(idle_task.comm));
    idle_task.state = TASK_RUNNING;
    idle_task.flags = PF_KTHREAD;
    idle_task.prio = 140;  // Lowest priority
    idle_task.policy = SCHED_IDLE;
    INIT_LIST_HEAD(&idle_task.children);
    INIT_LIST_HEAD(&idle_task.sibling);
    idle_task.cred = cred_alloc();
    idle_task.files = files_alloc();
    idle_task.fs = fs_alloc();
    
    task_table[0] = &idle_task;
    current_tasks[0] = &idle_task;
    
    // Create init task (PID 1)
    // init_task will be created when we load /sbin/init
    
    printk(KERN_INFO "  Process management initialized\n");
}

task_struct_t *create_init_task(void) {
    // Reserve PID 1
    pid_bitmap[0] |= 2;
    
    memset(&init_task, 0, sizeof(init_task));
    init_task.pid = 1;
    init_task.tgid = 1;
    strncpy(init_task.comm, "init", sizeof(init_task.comm));
    init_task.state = TASK_RUNNING;
    init_task.prio = 120;
    init_task.policy = SCHED_NORMAL;
    init_task.time_slice = 100;
    
    INIT_LIST_HEAD(&init_task.children);
    INIT_LIST_HEAD(&init_task.sibling);
    INIT_LIST_HEAD(&init_task.thread_group);
    
    init_task.real_parent = &init_task;
    init_task.parent = &init_task;
    init_task.group_leader = &init_task;
    
    init_task.cred = cred_alloc();
    init_task.real_cred = init_task.cred;
    init_task.files = files_alloc();
    init_task.fs = fs_alloc();
    init_task.mm = mm_alloc();
    init_task.active_mm = init_task.mm;
    
    setup_kernel_stack(&init_task);
    
    task_table[1] = &init_task;
    
    printk(KERN_INFO "  Init task (PID 1) created\n");
    
    return &init_task;
}

task_struct_t *create_kthread(const char *name, int (*fn)(void *), void *arg) {
    // Wrapper function pointer type
    void (*entry)(void *) = (void (*)(void *))(void *)fn;
    return task_create(name, entry, arg, PF_KTHREAD);
}
