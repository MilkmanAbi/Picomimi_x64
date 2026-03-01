/**
 * Picomimi-x64 System Call Implementation
 * 
 * POSIX-compliant syscall handlers
 */

#include <kernel/types.h>
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>

// Spinlock functions (implemented in process.c)
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);

// Task table lock
extern spinlock_t task_table_lock;

// Align macros
#ifndef ALIGN_UP
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#endif

// ============================================================================
// SYSCALL TABLE
// ============================================================================

syscall_fn_t syscall_table[NR_syscalls];

// Not implemented syscall
s64 sys_ni_syscall(void) {
    return -ENOSYS;
}

// ============================================================================
// PROCESS IDENTIFICATION
// ============================================================================

s64 sys_getpid(void) {
    return current->tgid;  // Return thread group ID as PID
}

s64 sys_gettid(void) {
    return current->pid;   // Return actual thread ID
}

s64 sys_getppid(void) {
    return current->real_parent ? current->real_parent->tgid : 0;
}

s64 sys_getuid(void) {
    return current->cred->uid;
}

s64 sys_geteuid(void) {
    return current->cred->euid;
}

s64 sys_getgid(void) {
    return current->cred->gid;
}

s64 sys_getegid(void) {
    return current->cred->egid;
}

// ============================================================================
// PROCESS CONTROL
// ============================================================================

s64 sys_fork(void) {
    task_struct_t *child = task_fork(SIGCHLD, 0, NULL, NULL, 0);
    if (IS_ERR(child)) {
        return PTR_ERR(child);
    }
    return child->pid;
}

s64 sys_vfork(void) {
    task_struct_t *child = task_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, 0, NULL, NULL, 0);
    if (IS_ERR(child)) {
        return PTR_ERR(child);
    }
    // Block parent until child execs or exits
    // TODO: Implement vfork semantics properly
    return child->pid;
}

s64 sys_clone(u64 flags, void *stack, int *parent_tid, int *child_tid, u64 tls) {
    task_struct_t *child = task_fork(flags, (unsigned long)stack, parent_tid, child_tid, tls);
    if (IS_ERR(child)) {
        return PTR_ERR(child);
    }
    return child->pid;
}

s64 sys_exit(int status) {
    task_exit((status & 0xFF) << 8);
    return 0;  // Never reached
}

s64 sys_exit_group(int status) {
    task_exit_group((status & 0xFF) << 8);
    return 0;  // Never reached
}

s64 sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) {
    task_struct_t *child = NULL;
    int found = 0;
    
    // Find matching child
    struct list_head *pos;
    list_for_each(pos, &current->children) {
        task_struct_t *c = list_entry(pos, task_struct_t, sibling);
        
        if (pid > 0 && c->pid != pid) continue;
        if (pid == 0 && c->tgid != current->tgid) continue;
        if (pid < -1 && c->tgid != -pid) continue;
        
        found = 1;
        
        if (c->exit_state == EXIT_ZOMBIE) {
            child = c;
            break;
        }
    }
    
    if (!found) {
        return -ECHILD;
    }
    
    if (!child) {
        if (options & WNOHANG) {
            return 0;
        }
        // TODO: Sleep and wait for child
        return -EAGAIN;
    }
    
    // Collect exit status
    pid_t ret = child->pid;
    
    if (wstatus) {
        *wstatus = child->exit_code;
    }
    
    if (rusage) {
        memset(rusage, 0, sizeof(*rusage));
        rusage->ru_utime.tv_sec = child->utime / 1000000000ULL;
        rusage->ru_utime.tv_usec = (child->utime % 1000000000ULL) / 1000;
        rusage->ru_stime.tv_sec = child->stime / 1000000000ULL;
        rusage->ru_stime.tv_usec = (child->stime % 1000000000ULL) / 1000;
    }
    
    // Remove from parent's children
    list_del(&child->sibling);
    
    // Free child resources
    free_pid(child->pid);
    if (child->kernel_stack) {
        kfree(child->kernel_stack);
    }
    
    // Remove from task table
    spin_lock(&task_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (task_table[i] == child) {
            task_table[i] = NULL;
            break;
        }
    }
    spin_unlock(&task_table_lock);
    
    kfree(child);
    
    return ret;
}

s64 sys_kill(pid_t pid, int sig) {
    if (sig < 0 || sig > NSIG) {
        return -EINVAL;
    }
    
    if (sig == 0) {
        // Check if process exists
        return find_task_by_pid(pid) ? 0 : -ESRCH;
    }
    
    return send_signal(pid, sig);
}

s64 sys_tkill(pid_t tid, int sig) {
    return sys_kill(tid, sig);
}

s64 sys_tgkill(pid_t tgid, pid_t tid, int sig) {
    task_struct_t *task = find_task_by_pid(tid);
    if (!task || task->tgid != tgid) {
        return -ESRCH;
    }
    return send_signal(tid, sig);
}

// ============================================================================
// FILE OPERATIONS
// ============================================================================

s64 sys_read(int fd, void *buf, size_t count) {
    file_t *file = fget(fd);
    if (!file) {
        return -EBADF;
    }
    
    if (!file->f_op || !file->f_op->read) {
        fput(file);
        return -EINVAL;
    }
    
    s64 ret = file->f_op->read(file, buf, count, &file->f_pos);
    fput(file);
    return ret;
}

s64 sys_write(int fd, const void *buf, size_t count) {
    file_t *file = fget(fd);
    if (!file) {
        return -EBADF;
    }
    
    // Special handling for stdout/stderr - write to console
    if (fd == 1 || fd == 2) {
        // Direct console output for now
        const char *p = buf;
        for (size_t i = 0; i < count; i++) {
            printk("%c", p[i]);
        }
        fput(file);
        return count;
    }
    
    if (!file->f_op || !file->f_op->write) {
        fput(file);
        return -EINVAL;
    }
    
    s64 ret = file->f_op->write(file, buf, count, &file->f_pos);
    fput(file);
    return ret;
}

s64 sys_open(const char *filename, int flags, u32 mode) {
    // TODO: Implement VFS open
    printk(KERN_DEBUG "sys_open: %s flags=%x mode=%o\n", filename, flags, mode);
    return -ENOENT;
}

s64 sys_close(int fd) {
    return do_close(fd);
}

s64 sys_lseek(int fd, s64 offset, int whence) {
    file_t *file = fget(fd);
    if (!file) {
        return -EBADF;
    }
    
    s64 new_pos;
    
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        // TODO: Get file size from inode when VFS is implemented
        fput(file);
        return -EINVAL;
    default:
        fput(file);
        return -EINVAL;
    }
    
    if (new_pos < 0) {
        fput(file);
        return -EINVAL;
    }
    
    file->f_pos = new_pos;
    fput(file);
    return new_pos;
}

s64 sys_dup(int oldfd) {
    file_t *file = fget(oldfd);
    if (!file) {
        return -EBADF;
    }
    
    int newfd = get_unused_fd();
    if (newfd < 0) {
        fput(file);
        return newfd;
    }
    
    fd_install(newfd, file);
    return newfd;
}

s64 sys_dup2(int oldfd, int newfd) {
    if (newfd < 0) {
        return -EBADF;
    }
    
    if (oldfd == newfd) {
        return fget(oldfd) ? newfd : -EBADF;
    }
    
    file_t *file = fget(oldfd);
    if (!file) {
        return -EBADF;
    }
    
    // Close newfd if open
    do_close(newfd);
    
    fd_install(newfd, file);
    return newfd;
}

s64 sys_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) {
        return -EINVAL;
    }
    
    s64 ret = sys_dup2(oldfd, newfd);
    if (ret >= 0 && (flags & O_CLOEXEC)) {
        // Set close-on-exec flag
        current->files->close_on_exec |= (1UL << newfd);
    }
    return ret;
}

s64 sys_pipe(int *pipefd) {
    return sys_pipe2(pipefd, 0);
}

s64 sys_pipe2(int *pipefd, int flags) {
    // TODO: Implement pipe
    (void)pipefd;
    (void)flags;
    return -ENOSYS;
}

s64 sys_fcntl(int fd, int cmd, u64 arg) {
    file_t *file = fget(fd);
    if (!file) {
        return -EBADF;
    }
    
    s64 ret = 0;
    
    switch (cmd) {
    case F_DUPFD:
        ret = sys_dup(fd);
        break;
    case F_DUPFD_CLOEXEC:
        ret = sys_dup(fd);
        if (ret >= 0) {
            current->files->close_on_exec |= (1UL << ret);
        }
        break;
    case F_GETFD:
        ret = (current->files->close_on_exec & (1UL << fd)) ? FD_CLOEXEC : 0;
        break;
    case F_SETFD:
        if (arg & FD_CLOEXEC) {
            current->files->close_on_exec |= (1UL << fd);
        } else {
            current->files->close_on_exec &= ~(1UL << fd);
        }
        break;
    case F_GETFL:
        ret = file->f_flags;
        break;
    case F_SETFL:
        file->f_flags = (file->f_flags & ~O_NONBLOCK) | (arg & O_NONBLOCK);
        break;
    default:
        ret = -EINVAL;
    }
    
    fput(file);
    return ret;
}

s64 sys_ioctl(int fd, u64 cmd, u64 arg) {
    file_t *file = fget(fd);
    if (!file) {
        return -EBADF;
    }
    
    if (!file->f_op || !file->f_op->ioctl) {
        fput(file);
        return -ENOTTY;
    }
    
    s64 ret = file->f_op->ioctl(file, cmd, arg);
    fput(file);
    return ret;
}

// ============================================================================
// MEMORY
// ============================================================================

s64 sys_brk(void *addr) {
    mm_struct_t *mm = current->mm;
    if (!mm) {
        return 0;
    }
    
    u64 brk = (u64)addr;
    
    if (brk == 0) {
        return mm->brk;
    }
    
    // Align to page
    brk = ALIGN_UP(brk, PAGE_SIZE);
    
    if (brk < mm->start_brk) {
        return mm->brk;
    }
    
    // TODO: Allocate/free pages as needed
    mm->brk = brk;
    
    return mm->brk;
}

s64 sys_mmap(void *addr, size_t len, int prot, int flags, int fd, s64 offset) {
    (void)fd;  // TODO: Use when file mapping is implemented
    
    if (len == 0) {
        return -EINVAL;
    }
    
    mm_struct_t *mm = current->mm;
    if (!mm) {
        return -ENOMEM;
    }
    
    // Align
    len = ALIGN_UP(len, PAGE_SIZE);
    
    u64 start = (u64)addr;
    
    // Find free region if not fixed
    if (!(flags & MAP_FIXED)) {
        // Simple bump allocator for mmap region
        static u64 mmap_base = 0x7F0000000000UL;
        start = mmap_base;
        mmap_base += len;
    }
    
    // Create VMA
    vm_area_t *vma = kmalloc(sizeof(vm_area_t), GFP_KERNEL);
    if (!vma) {
        return -ENOMEM;
    }
    
    vma->start = start;
    vma->end = start + len;
    vma->flags = 0;
    if (prot & PROT_READ) vma->flags |= VM_READ;
    if (prot & PROT_WRITE) vma->flags |= VM_WRITE;
    if (prot & PROT_EXEC) vma->flags |= VM_EXEC;
    vma->vm_flags = flags;
    vma->file = NULL;
    vma->offset = offset;
    vma->next = NULL;
    vma->prev = NULL;
    
    // Add to MM
    spin_lock(&mm->mmap_lock);
    
    if (!mm->mmap) {
        mm->mmap = vma;
    } else {
        vm_area_t *last = mm->mmap;
        while (last->next) last = last->next;
        last->next = vma;
        vma->prev = last;
    }
    
    mm->total_vm += len / PAGE_SIZE;
    
    spin_unlock(&mm->mmap_lock);
    
    // TODO: Actually map pages
    
    return start;
}

s64 sys_munmap(void *addr, size_t len) {
    mm_struct_t *mm = current->mm;
    if (!mm) {
        return -EINVAL;
    }
    
    u64 start = (u64)addr;
    u64 end = start + ALIGN_UP(len, PAGE_SIZE);
    
    spin_lock(&mm->mmap_lock);
    
    vm_area_t *vma = mm->mmap;
    while (vma) {
        if (vma->start >= start && vma->end <= end) {
            // Remove VMA
            if (vma->prev) {
                vma->prev->next = vma->next;
            } else {
                mm->mmap = vma->next;
            }
            if (vma->next) {
                vma->next->prev = vma->prev;
            }
            
            mm->total_vm -= (vma->end - vma->start) / PAGE_SIZE;
            
            vm_area_t *to_free = vma;
            vma = vma->next;
            kfree(to_free);
            continue;
        }
        vma = vma->next;
    }
    
    spin_unlock(&mm->mmap_lock);
    
    return 0;
}

s64 sys_mprotect(void *addr, size_t len, int prot) {
    mm_struct_t *mm = current->mm;
    if (!mm) {
        return -EINVAL;
    }
    
    u64 start = (u64)addr;
    u64 end = start + ALIGN_UP(len, PAGE_SIZE);
    
    spin_lock(&mm->mmap_lock);
    
    vm_area_t *vma = mm->mmap;
    while (vma) {
        if (vma->start < end && vma->end > start) {
            vma->flags = 0;
            if (prot & PROT_READ) vma->flags |= VM_READ;
            if (prot & PROT_WRITE) vma->flags |= VM_WRITE;
            if (prot & PROT_EXEC) vma->flags |= VM_EXEC;
        }
        vma = vma->next;
    }
    
    spin_unlock(&mm->mmap_lock);
    
    return 0;
}

// ============================================================================
// FILESYSTEM
// ============================================================================

s64 sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        return -EINVAL;
    }
    
    // TODO: Implement proper path resolution
    if (size < 2) {
        return -ERANGE;
    }
    
    buf[0] = '/';
    buf[1] = '\0';
    
    return 2;
}

s64 sys_chdir(const char *path) {
    // TODO: Implement
    (void)path;
    return -ENOENT;
}

s64 sys_fchdir(int fd) {
    (void)fd;
    return -EBADF;
}

s64 sys_mkdir(const char *pathname, u32 mode) {
    // TODO: Implement
    (void)pathname;
    (void)mode;
    return -ENOSYS;
}

s64 sys_rmdir(const char *pathname) {
    (void)pathname;
    return -ENOSYS;
}

s64 sys_unlink(const char *pathname) {
    (void)pathname;
    return -ENOSYS;
}

s64 sys_rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -ENOSYS;
}

s64 sys_chmod(const char *pathname, u32 mode) {
    (void)pathname;
    (void)mode;
    return -ENOSYS;
}

s64 sys_chown(const char *pathname, u32 owner, u32 group) {
    (void)pathname;
    (void)owner;
    (void)group;
    return -ENOSYS;
}

s64 sys_umask(u32 mask) {
    u32 old = current->fs->umask;
    current->fs->umask = mask & 0777;
    return old;
}

// ============================================================================
// TIME
// ============================================================================

s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv) {
        // TODO: Get actual time
        extern volatile u64 system_ticks;
        tv->tv_sec = system_ticks / 100;
        tv->tv_usec = (system_ticks % 100) * 10000;
    }
    
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    
    return 0;
}

s64 sys_clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) {
        return -EFAULT;
    }
    
    extern volatile u64 system_ticks;
    
    switch (clk_id) {
    case 0:  // CLOCK_REALTIME
    case 1:  // CLOCK_MONOTONIC
        tp->tv_sec = system_ticks / 100;
        tp->tv_nsec = (system_ticks % 100) * 10000000;
        break;
    default:
        return -EINVAL;
    }
    
    return 0;
}

s64 sys_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        return -EFAULT;
    }
    
    // TODO: Implement proper sleep
    // For now, just yield
    yield();
    
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    return 0;
}

// ============================================================================
// SYSTEM INFO
// ============================================================================

s64 sys_uname(struct utsname *buf) {
    if (!buf) {
        return -EFAULT;
    }
    
    strcpy(buf->sysname, "Picomimi-x64");
    strcpy(buf->nodename, "picomimi");
    strcpy(buf->release, "1.0.0");
    strcpy(buf->version, "#1 SMP " __DATE__ " " __TIME__);
    strcpy(buf->machine, "x86_64");
    strcpy(buf->domainname, "(none)");
    
    return 0;
}

s64 sys_sysinfo(struct sysinfo *info) {
    if (!info) {
        return -EFAULT;
    }
    
    extern volatile u64 system_ticks;
    extern u64 pmm_get_total_memory(void);
    extern u64 pmm_get_free_memory(void);
    
    memset(info, 0, sizeof(*info));
    
    info->uptime = system_ticks / 100;
    info->loads[0] = 0;  // 1 min load
    info->loads[1] = 0;  // 5 min load
    info->loads[2] = 0;  // 15 min load
    info->totalram = pmm_get_total_memory();
    info->freeram = pmm_get_free_memory();
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 2;  // TODO: Count processes
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = 1;
    
    return 0;
}

// ============================================================================
// SCHEDULING
// ============================================================================

s64 sys_sched_yield(void) {
    yield();
    return 0;
}

// ============================================================================
// SIGNALS
// ============================================================================

s64 sys_rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact, size_t sigsetsize) {
    if (sigsetsize != sizeof(sigset_t)) {
        return -EINVAL;
    }
    
    if (sig < 1 || sig > NSIG || sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }
    
    sighand_struct_t *sighand = current->sighand;
    if (!sighand) {
        return -EINVAL;
    }
    
    spin_lock(&sighand->siglock);
    
    if (oact) {
        *oact = sighand->action[sig - 1];
    }
    
    if (act) {
        sighand->action[sig - 1] = *act;
    }
    
    spin_unlock(&sighand->siglock);
    
    return 0;
}

s64 sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize) {
    if (sigsetsize != sizeof(sigset_t)) {
        return -EINVAL;
    }
    
    if (oset) {
        *oset = current->blocked;
    }
    
    if (set) {
        switch (how) {
        case 0:  // SIG_BLOCK
            current->blocked.sig[0] |= set->sig[0];
            current->blocked.sig[1] |= set->sig[1];
            break;
        case 1:  // SIG_UNBLOCK
            current->blocked.sig[0] &= ~set->sig[0];
            current->blocked.sig[1] &= ~set->sig[1];
            break;
        case 2:  // SIG_SETMASK
            current->blocked = *set;
            break;
        default:
            return -EINVAL;
        }
        
        // Never block SIGKILL or SIGSTOP
        current->blocked.sig[0] &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
        
        recalc_sigpending();
    }
    
    return 0;
}

s64 sys_pause(void) {
    // Sleep until signal
    set_task_state(current, TASK_INTERRUPTIBLE);
    schedule();
    
    return -EINTR;
}

// ============================================================================
// MISC
// ============================================================================

s64 sys_set_tid_address(int *tidptr) {
    current->clear_child_tid = tidptr;
    return current->pid;
}

s64 sys_arch_prctl(int code, u64 addr) {
    switch (code) {
    case 0x1002:  // ARCH_SET_FS
        current->tls = addr;
        wrmsr(0xC0000100, addr);  // FS_BASE MSR
        return 0;
    case 0x1001:  // ARCH_SET_GS
        wrmsr(0xC0000101, addr);  // GS_BASE MSR
        return 0;
    case 0x1003:  // ARCH_GET_FS
        *(u64 *)addr = current->tls;
        return 0;
    case 0x1004:  // ARCH_GET_GS
        *(u64 *)addr = rdmsr(0xC0000101);
        return 0;
    default:
        return -EINVAL;
    }
}

s64 sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    
    // Simple PRNG for now
    static u64 seed = 0x123456789ABCDEF0ULL;
    
    u8 *p = buf;
    for (size_t i = 0; i < buflen; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (seed >> 32) & 0xFF;
    }
    
    return buflen;
}

s64 sys_prctl(int option, u64 arg2, u64 arg3, u64 arg4, u64 arg5) {
    (void)arg3;
    (void)arg4;
    (void)arg5;
    
    switch (option) {
    case 15:  // PR_SET_NAME
        strncpy(current->comm, (const char *)arg2, sizeof(current->comm) - 1);
        return 0;
    case 16:  // PR_GET_NAME
        strcpy((char *)arg2, current->comm);
        return 0;
    default:
        return -EINVAL;
    }
}

// ============================================================================
// SYSCALL TABLE INIT
// ============================================================================

// Helper to avoid cast warnings
#define SYSCALL(fn) ((syscall_fn_t)(void *)(fn))

void syscall_init_table(void) {
    // Initialize all to not implemented
    for (int i = 0; i < NR_syscalls; i++) {
        syscall_table[i] = SYSCALL(sys_ni_syscall);
    }
    
    // Process ID
    syscall_table[__NR_getpid] = SYSCALL(sys_getpid);
    syscall_table[__NR_gettid] = SYSCALL(sys_gettid);
    syscall_table[__NR_getppid] = SYSCALL(sys_getppid);
    syscall_table[__NR_getuid] = SYSCALL(sys_getuid);
    syscall_table[__NR_geteuid] = SYSCALL(sys_geteuid);
    syscall_table[__NR_getgid] = SYSCALL(sys_getgid);
    syscall_table[__NR_getegid] = SYSCALL(sys_getegid);
    
    // Process control
    syscall_table[__NR_fork] = SYSCALL(sys_fork);
    syscall_table[__NR_vfork] = SYSCALL(sys_vfork);
    syscall_table[__NR_clone] = SYSCALL(sys_clone);
    syscall_table[__NR_exit] = SYSCALL(sys_exit);
    syscall_table[__NR_exit_group] = SYSCALL(sys_exit_group);
    syscall_table[__NR_wait4] = SYSCALL(sys_wait4);
    syscall_table[__NR_kill] = SYSCALL(sys_kill);
    syscall_table[__NR_tkill] = SYSCALL(sys_tkill);
    syscall_table[__NR_tgkill] = SYSCALL(sys_tgkill);
    
    // File operations
    syscall_table[__NR_read] = SYSCALL(sys_read);
    syscall_table[__NR_write] = SYSCALL(sys_write);
    syscall_table[__NR_open] = SYSCALL(sys_open);
    syscall_table[__NR_close] = SYSCALL(sys_close);
    syscall_table[__NR_lseek] = SYSCALL(sys_lseek);
    syscall_table[__NR_dup] = SYSCALL(sys_dup);
    syscall_table[__NR_dup2] = SYSCALL(sys_dup2);
    syscall_table[__NR_dup3] = SYSCALL(sys_dup3);
    syscall_table[__NR_pipe] = SYSCALL(sys_pipe);
    syscall_table[__NR_pipe2] = SYSCALL(sys_pipe2);
    syscall_table[__NR_fcntl] = SYSCALL(sys_fcntl);
    syscall_table[__NR_ioctl] = SYSCALL(sys_ioctl);
    
    // Memory
    syscall_table[__NR_brk] = SYSCALL(sys_brk);
    syscall_table[__NR_mmap] = SYSCALL(sys_mmap);
    syscall_table[__NR_munmap] = SYSCALL(sys_munmap);
    syscall_table[__NR_mprotect] = SYSCALL(sys_mprotect);
    
    // Filesystem
    syscall_table[__NR_getcwd] = SYSCALL(sys_getcwd);
    syscall_table[__NR_chdir] = SYSCALL(sys_chdir);
    syscall_table[__NR_fchdir] = SYSCALL(sys_fchdir);
    syscall_table[__NR_mkdir] = SYSCALL(sys_mkdir);
    syscall_table[__NR_rmdir] = SYSCALL(sys_rmdir);
    syscall_table[__NR_unlink] = SYSCALL(sys_unlink);
    syscall_table[__NR_rename] = SYSCALL(sys_rename);
    syscall_table[__NR_chmod] = SYSCALL(sys_chmod);
    syscall_table[__NR_chown] = SYSCALL(sys_chown);
    syscall_table[__NR_umask] = SYSCALL(sys_umask);
    
    // Time
    syscall_table[__NR_gettimeofday] = SYSCALL(sys_gettimeofday);
    syscall_table[__NR_clock_gettime] = SYSCALL(sys_clock_gettime);
    syscall_table[__NR_nanosleep] = SYSCALL(sys_nanosleep);
    
    // System info
    syscall_table[__NR_uname] = SYSCALL(sys_uname);
    syscall_table[__NR_sysinfo] = SYSCALL(sys_sysinfo);
    
    // Scheduling
    syscall_table[__NR_sched_yield] = SYSCALL(sys_sched_yield);
    
    // Signals
    syscall_table[__NR_rt_sigaction] = SYSCALL(sys_rt_sigaction);
    syscall_table[__NR_rt_sigprocmask] = SYSCALL(sys_rt_sigprocmask);
    syscall_table[__NR_pause] = SYSCALL(sys_pause);
    
    // Misc
    syscall_table[__NR_set_tid_address] = SYSCALL(sys_set_tid_address);
    syscall_table[__NR_arch_prctl] = SYSCALL(sys_arch_prctl);
    syscall_table[__NR_getrandom] = SYSCALL(sys_getrandom);
    syscall_table[__NR_prctl] = SYSCALL(sys_prctl);
    
    printk(KERN_INFO "  Syscall table initialized (%d syscalls)\n", NR_syscalls);
}
