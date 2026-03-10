/**
 * Picomimi-x64 Syscall Stubs & Partial Implementations
 *
 * Real implementations for the "easy" syscalls; proper stubs for the
 * complex ones so the kernel compiles fully and userspace doesn't panic
 * on an unhandled syscall — it just gets a meaningful error.
 *
 * Coverage:
 *   sysinfo           — filled from PMM stats
 *   getrlimit/setrlimit — per-task rlim array
 *   getrusage         — from task cpu time counters
 *   process groups    — setpgid/getpgid/setsid/getsid
 *   credentials       — setuid/setgid/setresuid/etc
 *   prctl / arch_prctl
 *   chroot / pivot_root
 *   reboot
 *   futex             — basic WAIT/WAKE
 *   select / poll     — simplified single-threaded spin
 *   epoll             — stub (returns 0)
 *   socket layer      — stub (returns -ENOSYS, network not ready)
 *   eventfd/signalfd/timerfd — stub
 *   inotify           — stub
 *   mmap stubs        — mremap/msync/madvise/mlock/mincore
 *   ptrace            — stub
 *   capabilities      — stub (always permit)
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>
#include <kernel/kernel.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <fs/vfs.h>

extern volatile u64 jiffies;
extern kernel_state_t kernel_state;

/* =========================================================
 * sysinfo
 * ========================================================= */

s64 sys_sysinfo(struct sysinfo *info) {
    if (!info) return -EFAULT;
    memset(info, 0, sizeof(*info));

    info->uptime    = (s64)get_uptime_seconds();
    info->totalram  = pmm_get_total_memory();
    info->freeram   = pmm_get_free_memory();
    info->sharedram = 0;
    info->bufferram = pmm_get_used_memory() / 16;
    info->totalswap = 0;
    info->freeswap  = 0;
    info->procs     = (u16)kernel_state.total_tasks;
    info->mem_unit  = 1;

    /* 1-min load avg (fake: running_tasks / total_tasks * 65536) */
    u32 run  = kernel_state.running_tasks ? kernel_state.running_tasks : 1;
    u32 tot  = kernel_state.total_tasks   ? kernel_state.total_tasks   : 1;
    u64 load = ((u64)run * 65536) / tot;
    info->loads[0] = load;
    info->loads[1] = load;
    info->loads[2] = load;

    return 0;
}

/* =========================================================
 * rlimit
 * ========================================================= */

#define RLIM_INFINITY   (~0ULL)

/* Default limits */
static const struct rlimit default_rlimits[16] = {
    [0]  = { 256,            256            }, /* RLIMIT_CPU (seconds) */
    [1]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_FSIZE */
    [2]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_DATA */
    [3]  = { 8*1024*1024,    RLIM_INFINITY  }, /* RLIMIT_STACK (8 MB) */
    [4]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_CORE */
    [5]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_RSS */
    [6]  = { 4096,           4096           }, /* RLIMIT_NPROC */
    [7]  = { 65536,          65536          }, /* RLIMIT_NOFILE */
    [8]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_MEMLOCK */
    [9]  = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_AS */
    [10] = { 0,              0              }, /* RLIMIT_LOCKS */
    [11] = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_SIGPENDING */
    [12] = { 819200,         819200         }, /* RLIMIT_MSGQUEUE */
    [13] = { 0,              0              }, /* RLIMIT_NICE */
    [14] = { 0,              0              }, /* RLIMIT_RTPRIO */
    [15] = { RLIM_INFINITY,  RLIM_INFINITY  }, /* RLIMIT_RTTIME */
};

s64 sys_getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= 16) return -EINVAL;

    task_struct_t *t = current;
    if (t) {
        memcpy(rlim, &t->rlim[resource], sizeof(*rlim));
    } else {
        memcpy(rlim, &default_rlimits[resource], sizeof(*rlim));
    }
    return 0;
}

s64 sys_setrlimit(int resource, const struct rlimit *rlim) {
    if (!rlim) return -EFAULT;
    if (resource < 0 || resource >= 16) return -EINVAL;

    if (current)
        memcpy(&current->rlim[resource], rlim, sizeof(*rlim));
    return 0;
}

s64 sys_prlimit64(pid_t pid, int resource, const struct rlimit *new_rlim,
                   struct rlimit *old_rlim) {
    task_struct_t *t = pid == 0 ? current : find_task_by_pid(pid);
    if (!t) return -ESRCH;
    if (resource < 0 || resource >= 16) return -EINVAL;

    if (old_rlim) memcpy(old_rlim, &t->rlim[resource], sizeof(*old_rlim));
    if (new_rlim) memcpy(&t->rlim[resource], new_rlim, sizeof(t->rlim[0]));
    return 0;
}

/* =========================================================
 * getrusage
 * ========================================================= */

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

s64 sys_getrusage(int who, struct rusage *ru) {
    if (!ru) return -EFAULT;
    memset(ru, 0, sizeof(*ru));

    task_struct_t *t = current;
    if (!t) return 0;

    u64 utime_ns, stime_ns;

    if (who == RUSAGE_SELF || who == RUSAGE_THREAD) {
        utime_ns = t->utime;
        stime_ns = t->stime;
    } else {
        /* RUSAGE_CHILDREN: sum of all zombie children */
        utime_ns = 0;
        stime_ns = 0;
        struct list_head *p;
        list_for_each(p, &t->children) {
            task_struct_t *c = list_entry(p, task_struct_t, sibling);
            if (c->exit_state == EXIT_ZOMBIE) {
                utime_ns += c->utime;
                stime_ns += c->stime;
            }
        }
    }

    ru->ru_utime.tv_sec  = (time_t)(utime_ns / NSEC_PER_SEC);
    ru->ru_utime.tv_usec = (s64)((utime_ns % NSEC_PER_SEC) / NSEC_PER_USEC);
    ru->ru_stime.tv_sec  = (time_t)(stime_ns / NSEC_PER_SEC);
    ru->ru_stime.tv_usec = (s64)((stime_ns % NSEC_PER_SEC) / NSEC_PER_USEC);

    if (t->mm) {
        ru->ru_maxrss = (s64)(t->mm->total_vm * 4);  /* pages → KB */
    }

    return 0;
}

/* =========================================================
 * Process group / session
 * ========================================================= */

s64 sys_setpgid(pid_t pid, pid_t pgid) {
    task_struct_t *t = (pid == 0) ? current : find_task_by_pid(pid);
    if (!t) return -ESRCH;
    if (pgid < 0) return -EINVAL;
    if (pgid == 0) pgid = t->pid;

    if (t->signal) t->signal->pgrp = pgid;
    return 0;
}

s64 sys_getpgid(pid_t pid) {
    task_struct_t *t = (pid == 0) ? current : find_task_by_pid(pid);
    if (!t) return -ESRCH;
    return t->signal ? (s64)t->signal->pgrp : (s64)t->tgid;
}

s64 sys_getpgrp(void) {
    return sys_getpgid(0);
}

s64 sys_setsid(void) {
    if (!current) return -ESRCH;
    pid_t sid = current->pid;
    if (current->signal) {
        current->signal->session = sid;
        current->signal->pgrp    = sid;
    }
    return (s64)sid;
}

s64 sys_getsid(pid_t pid) {
    task_struct_t *t = (pid == 0) ? current : find_task_by_pid(pid);
    if (!t) return -ESRCH;
    return t->signal ? (s64)t->signal->session : (s64)t->tgid;
}

/* =========================================================
 * Credential setters (simplified: no permission checks in kernel mode)
 * ========================================================= */

static cred_t *copy_cred_for_write(void) {
    if (!current) return NULL;
    cred_t *new_cred = cred_copy(current->cred);
    if (!new_cred) return NULL;
    cred_free((cred_t *)current->cred);
    current->cred      = new_cred;
    current->real_cred = new_cred;
    return new_cred;
}



s64 sys_setreuid(uid_t ruid, uid_t euid) {
    cred_t *c = copy_cred_for_write();
    if (!c) return -ENOMEM;
    if (ruid != (uid_t)-1) c->uid  = ruid;
    if (euid != (uid_t)-1) c->euid = euid;
    return 0;
}

s64 sys_setregid(gid_t rgid, gid_t egid) {
    cred_t *c = copy_cred_for_write();
    if (!c) return -ENOMEM;
    if (rgid != (gid_t)-1) c->gid  = rgid;
    if (egid != (gid_t)-1) c->egid = egid;
    return 0;
}





s64 sys_setfsuid(uid_t fsuid) {
    uid_t old = current && current->cred ? current->cred->fsuid : 0;
    cred_t *c = copy_cred_for_write();
    if (c) c->fsuid = fsuid;
    return (s64)old;
}

s64 sys_setfsgid(gid_t fsgid) {
    gid_t old = current && current->cred ? current->cred->fsgid : 0;
    cred_t *c = copy_cred_for_write();
    if (c) c->fsgid = fsgid;
    return (s64)old;
}



/* =========================================================
 * prctl / arch_prctl
 * ========================================================= */

#define PR_SET_NAME     15
#define PR_GET_NAME     16
#define PR_SET_DUMPABLE  4
#define PR_GET_DUMPABLE  3
#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_SET_SECCOMP   22
#define PR_GET_SECCOMP   21

s64 sys_prctl(int option, u64 arg2, u64 arg3, u64 arg4, u64 arg5) {
    (void)arg3; (void)arg4; (void)arg5;

    switch (option) {
    case PR_SET_NAME: {
        char *name = (char *)arg2;
        if (!name) return -EFAULT;
        if (current) {
            strncpy(current->comm, name, 15);
            current->comm[15] = 0;
        }
        return 0;
    }
    case PR_GET_NAME: {
        char *name = (char *)arg2;
        if (!name) return -EFAULT;
        if (current) strncpy(name, current->comm, 16);
        else name[0] = 0;
        return 0;
    }
    case PR_SET_DUMPABLE:
        return 0;   /* Ignore */
    case PR_GET_DUMPABLE:
        return 1;   /* Always dumpable */
    case PR_SET_PDEATHSIG:
        if (current) current->pdeath_signal = (int)arg2;
        return 0;
    case PR_GET_PDEATHSIG:
        if (arg2) *(int *)arg2 = current ? current->pdeath_signal : 0;
        return 0;
    case PR_SET_SECCOMP:
        return -EINVAL;   /* Seccomp not implemented */
    case PR_GET_SECCOMP:
        return 0;
    default:
        return -EINVAL;
    }
}

#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_SET_GS     0x1001
#define ARCH_GET_GS     0x1004

s64 sys_arch_prctl(int code, u64 addr) {
    switch (code) {
    case ARCH_SET_FS:
        if (current) current->tls = addr;
        /* Load FS base via MSR */
        __asm__ volatile("wrmsr" :: "c"(0xC0000100UL),
                         "a"((u32)(addr & 0xFFFFFFFF)),
                         "d"((u32)(addr >> 32)));
        return 0;
    case ARCH_GET_FS:
        if (addr) *(u64 *)addr = current ? current->tls : 0;
        return 0;
    case ARCH_SET_GS:
        __asm__ volatile("wrmsr" :: "c"(0xC0000101UL),
                         "a"((u32)(addr & 0xFFFFFFFF)),
                         "d"((u32)(addr >> 32)));
        return 0;
    case ARCH_GET_GS:
        if (addr) *(u64 *)addr = 0;
        return 0;
    default:
        return -EINVAL;
    }
}

s64 sys_set_tid_address(int *tidptr) {
    if (current) current->clear_child_tid = tidptr;
    return current ? (s64)current->pid : 0;
}

/* =========================================================
 * chroot / pivot_root
 * ========================================================= */

s64 sys_chroot(const char *path) {
    if (!path) return -EFAULT;
    if (!current || !current->cred || current->cred->euid != 0) return -EPERM;
    /* Simplified chroot - just update cwd path string if fs struct exists */
    if (current->fs) {
        /* Look up the dentry via root + path */
        extern dentry_t *vfs_root_dentry(void);
        dentry_t *root = vfs_root_dentry();
        if (root) current->fs->root = root;
    }
    return 0;
}

s64 sys_pivot_root(const char *new_root, const char *put_old) {
    (void)new_root; (void)put_old;
    return -ENOSYS;
}

/* =========================================================
 * reboot
 * ========================================================= */

#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_MAGIC2     0x28121969
#define LINUX_REBOOT_CMD_RESTART   0x01234567
#define LINUX_REBOOT_CMD_HALT      0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

s64 sys_reboot(int magic, int magic2, int cmd, void *arg) {
    (void)arg;
    if ((u32)magic != LINUX_REBOOT_MAGIC1) return -EINVAL;
    /* Only allow CAP_SYS_BOOT — for now just check root */
    if (!current || !current->cred || current->cred->euid != 0) return -EPERM;

    switch ((u32)cmd) {
    case LINUX_REBOOT_CMD_RESTART:
        printk(KERN_EMERG "[reboot] RESTARTING system\n");
        /* Triple fault: write bad value to IDTR */
        __asm__ volatile("lidt (0)");
        __asm__ volatile("int $0");
        break;
    case LINUX_REBOOT_CMD_HALT:
    case LINUX_REBOOT_CMD_POWER_OFF:
        printk(KERN_EMERG "[reboot] HALTING system\n");
        /* ACPI S5 shutdown: write 0x2000 to PM1a_CNT (typical QEMU port 0x604) */
        __asm__ volatile("outw %0, %1" :: "a"((u16)0x2000), "Nd"((u16)0x604));
        /* Fallback: HLT loop */
        for (;;) { __asm__ volatile("cli; hlt"); }
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/* =========================================================
 * Capabilities (stub: root has all caps)
 * ========================================================= */

s64 sys_capget(void *hdrp, void *datap) {
    if (datap) {
        /* Fake capability data: all bits set for root */
        u32 *data = (u32 *)datap;
        bool is_root = current && current->cred && current->cred->euid == 0;
        u32 caps = is_root ? 0xffffffff : 0;
        data[0] = caps;   /* effective */
        data[1] = caps;   /* permitted */
        data[2] = caps;   /* inheritable */
        data[3] = caps; data[4] = caps; data[5] = caps;
    }
    return 0;
}

s64 sys_capset(void *hdrp, const void *datap) {
    (void)hdrp; (void)datap;
    return 0;  /* Ignored */
}

/* =========================================================
 * ptrace stub
 * ========================================================= */

s64 sys_ptrace(long request, pid_t pid, void *addr, void *data) {
    (void)request; (void)pid; (void)addr; (void)data;
    return -EPERM;  /* Not implemented */
}

/* =========================================================
 * setdomainname
 * ========================================================= */

static char domainname[256] = "picomimi.local";

s64 sys_setdomainname(const char *name, size_t len) {
    if (!name || len > 255) return -EINVAL;
    memcpy(domainname, name, len);
    domainname[len] = 0;
    return 0;
}

/* =========================================================
 * Futex — basic WAIT/WAKE (no timeout, no priority inheritance)
 * ========================================================= */

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_RT      256
#define FUTEX_CMD_MASK      (~(FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_RT))

s64 sys_futex(u32 *uaddr, int op, u32 val,
               const struct timespec *timeout,
               u32 *uaddr2, u32 val3) {
    (void)uaddr2; (void)val3; (void)timeout;

    if (!uaddr) return -EFAULT;

    int cmd = op & FUTEX_CMD_MASK;

    switch (cmd) {
    case FUTEX_WAIT:
        /* If *uaddr == val, sleep until woken */
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val)
            return -EAGAIN;
        /* Spin wait — a real implementation would block */
        {
            u64 deadline = jiffies + (timeout ? 100 : 1000); /* ~1-10s */
            while ((s64)(deadline - jiffies) > 0) {
                if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val)
                    return 0;
                if (current && signal_pending(current)) return -EINTR;
                __asm__ volatile("pause");
            }
        }
        return -ETIMEDOUT;

    case FUTEX_WAKE:
        /* Wake up to val waiters — since we're single-threaded in userspace,
         * just return the number we'd wake */
        return (s64)(val > 0 ? 1 : 0);

    default:
        return -ENOSYS;
    }
}

/* =========================================================
 * select / poll — simplified implementations
 * ========================================================= */

typedef struct {
    u64 fds_bits[1024 / 64];
} fd_set_t;

#define FD_SET(fd, set) \
    ((set)->fds_bits[(fd)/64] |= (1ULL << ((fd)%64)))
#define FD_CLR(fd, set) \
    ((set)->fds_bits[(fd)/64] &= ~(1ULL << ((fd)%64)))
#define FD_ISSET(fd, set) \
    (!!((set)->fds_bits[(fd)/64] & (1ULL << ((fd)%64))))
#define FD_ZERO(set) \
    memset((set), 0, sizeof(fd_set_t))

s64 sys_select(int nfds, void *readfds, void *writefds, void *exceptfds,
               struct timeval *timeout) {
    (void)exceptfds;
    /* Simplified: check all listed fds immediately (no blocking) */
    if (nfds < 0 || nfds > 1024) return -EINVAL;

    fd_set_t *rfds = (fd_set_t *)readfds;
    fd_set_t *wfds = (fd_set_t *)writefds;

    int ready = 0;

    for (int fd = 0; fd < nfds; fd++) {
        if (rfds && FD_ISSET(fd, rfds)) {
            file_t *f = fget((unsigned int)fd);
            if (!f) { FD_CLR(fd, rfds); continue; }
            /* Assume readable if it has data (simplified: always ready) */
            ready++;
            fput(f);
        }
        if (wfds && FD_ISSET(fd, wfds)) {
            file_t *f = fget((unsigned int)fd);
            if (!f) { FD_CLR(fd, wfds); continue; }
            ready++;
            fput(f);
        }
    }

    if (ready == 0 && timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0)
        return 0;   /* Non-blocking: nothing ready */

    /* For blocking select: spin until timeout */
    if (ready == 0 && timeout) {
        u64 ns = (u64)timeout->tv_sec * NSEC_PER_SEC +
                 (u64)timeout->tv_usec * NSEC_PER_USEC;
        u64 ticks = ns / NSEC_PER_JIFFIE;
        u64 deadline = jiffies + ticks;
        while ((s64)(deadline - jiffies) > 0) {
            if (current && signal_pending(current)) return -EINTR;
            __asm__ volatile("hlt");
        }
    } else if (ready == 0) {
        /* Block indefinitely */
        while (true) {
            if (current && signal_pending(current)) return -EINTR;
            __asm__ volatile("hlt");
        }
    }

    return (s64)ready;
}

s64 sys_pselect6(int nfds, void *r, void *w, void *e,
                  const struct timespec *ts, const void *sigmask) {
    (void)sigmask;
    struct timeval tv = {0, 0};
    if (ts) {
        tv.tv_sec  = ts->tv_sec;
        tv.tv_usec = ts->tv_nsec / 1000;
    }
    return sys_select(nfds, r, w, e, ts ? &tv : NULL);
}

/* ---- poll ---- */

typedef struct pollfd {
    int     fd;
    short   events;
    short   revents;
} pollfd_t;

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

s64 sys_poll(void *fds_ptr, unsigned int nfds, int timeout_ms) {
    pollfd_t *fds = (pollfd_t *)fds_ptr;
    if (!fds && nfds > 0) return -EFAULT;

    int ready = 0;
    for (unsigned int i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;

        file_t *f = fget((unsigned int)fds[i].fd);
        if (!f) {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }

        /* Simplified: assume all fds are ready */
        if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
        if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
        if (fds[i].revents) ready++;

        fput(f);
    }

    if (ready == 0 && timeout_ms != 0) {
        u64 deadline = jiffies + (u64)(timeout_ms > 0 ? timeout_ms : 1000000) / 10;
        while ((s64)(deadline - jiffies) > 0) {
            if (current && signal_pending(current)) return -EINTR;
            __asm__ volatile("hlt");
        }
    }

    return (s64)ready;
}

s64 sys_ppoll(void *fds, unsigned int nfds, const struct timespec *tmo,
              const sigset_t *sigmask, size_t sigsetsize) {
    (void)sigmask; (void)sigsetsize;
    int timeout_ms = tmo ? (int)(tmo->tv_sec * 1000 + tmo->tv_nsec / 1000000) : -1;
    return sys_poll(fds, nfds, timeout_ms);
}

/* =========================================================
 * epoll stubs (functional enough for basic programs)
 * ========================================================= */

typedef struct epoll_file {
    int  fd;            /* dummy fd */
    bool valid;
} epoll_file_t;

/* Very simple: store a list of watched fds, poll returns them all ready */

s64 sys_epoll_create(int size) {
    (void)size;
    /* Return a dummy fd; programs will use it as an epoll instance */
    int fd = get_unused_fd();
    if (fd < 0) return -EMFILE;
    /* Install a null file for the epoll fd */
    extern const file_operations_t null_fops;
    file_t *f = kzalloc(sizeof(file_t), GFP_KERNEL);
    if (!f) { put_unused_fd(fd); return -ENOMEM; }
    atomic_set(&f->f_count, 1);
    f->f_op    = &null_fops;
    f->f_flags = O_RDWR;
    fd_install(fd, f);
    return (s64)fd;
}

s64 sys_epoll_create1(int flags) {
    (void)flags;
    return sys_epoll_create(1);
}

s64 sys_epoll_ctl(int epfd, int op, int fd, void *event) {
    (void)epfd; (void)op; (void)fd; (void)event;
    return 0;   /* Stub: pretend success */
}

s64 sys_epoll_wait(int epfd, void *events, int maxevents, int timeout) {
    (void)epfd; (void)events; (void)maxevents;
    if (timeout == 0) return 0;
    if (timeout > 0) {
        u64 deadline = jiffies + (u64)timeout / 10;
        while ((s64)(deadline - jiffies) > 0) {
            if (current && signal_pending(current)) return -EINTR;
            __asm__ volatile("hlt");
        }
    }
    return 0;
}

s64 sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout,
                    const sigset_t *sigmask, size_t sigsetsize) {
    (void)sigmask; (void)sigsetsize;
    return sys_epoll_wait(epfd, events, maxevents, timeout);
}

/* =========================================================
 * inotify stubs
 * ========================================================= */

s64 sys_inotify_init(void)  { return sys_epoll_create(1); }
s64 sys_inotify_init1(int flags) { (void)flags; return sys_epoll_create(1); }
s64 sys_inotify_add_watch(int fd, const char *pathname, u32 mask) {
    (void)fd; (void)pathname; (void)mask;
    return 1;   /* Return watch descriptor 1 */
}
s64 sys_inotify_rm_watch(int fd, int wd) { (void)fd; (void)wd; return 0; }

/* =========================================================
 * eventfd / signalfd / timerfd stubs
 * ========================================================= */

s64 sys_eventfd(unsigned int initval) {
    (void)initval;
    return sys_epoll_create(1);
}

s64 sys_eventfd2(unsigned int initval, int flags) {
    (void)initval; (void)flags;
    return sys_epoll_create(1);
}

s64 sys_signalfd(int fd, const sigset_t *mask, size_t sizemask) {
    (void)mask; (void)sizemask;
    if (fd != -1) return fd;
    return sys_epoll_create(1);
}

s64 sys_signalfd4(int fd, const sigset_t *mask, size_t sizemask, int flags) {
    (void)flags;
    return sys_signalfd(fd, mask, sizemask);
}

s64 sys_timerfd_create(int clockid, int flags) {
    (void)clockid; (void)flags;
    return sys_epoll_create(1);
}

s64 sys_timerfd_settime(int fd, int flags, const void *new, void *old) {
    (void)fd; (void)flags; (void)new; (void)old;
    return 0;
}

s64 sys_timerfd_gettime(int fd, void *curr) {
    (void)fd; (void)curr;
    return 0;
}

/* =========================================================
 * mmap variants
 * ========================================================= */

s64 sys_mremap(void *old_addr, size_t old_size, size_t new_size,
               int flags, void *new_addr) {
    (void)flags;
    /* Simplified: if same size, succeed. Otherwise ENOMEM. */
    if (old_size == new_size) return (s64)(uintptr_t)old_addr;
    if (!old_addr) return -EINVAL;
    /* Try to extend in-place (only works if pages after are free) */
    if (new_size < old_size) return (s64)(uintptr_t)old_addr;
    (void)new_addr;
    return -ENOMEM;
}

s64 sys_msync(void *addr, size_t len, int flags) {
    (void)addr; (void)len; (void)flags;
    return 0;  /* RAM-backed: always in sync */
}

s64 sys_madvise(void *addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;   /* Ignore hints */
}

s64 sys_mlock(const void *addr, size_t len) {
    (void)addr; (void)len;
    return 0;
}

s64 sys_munlock(const void *addr, size_t len) {
    (void)addr; (void)len;
    return 0;
}

s64 sys_mlockall(int flags) { (void)flags; return 0; }
s64 sys_munlockall(void)    { return 0; }

s64 sys_mincore(void *addr, size_t len, unsigned char *vec) {
    if (!vec) return -EFAULT;
    /* Report all pages as resident */
    size_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    memset(vec, 1, npages);
    (void)addr;
    return 0;
}

/* =========================================================
 * Socket syscalls — implemented in net/socket_syscalls.c
 * Stubs removed; real implementations linked from net/
 * =========================================================
 */

/* =========================================================
 * waitid
 * ========================================================= */

s64 sys_waitid(int idtype, pid_t id, void *infop, int options, struct rusage *ru) {
    (void)idtype; (void)id; (void)infop; (void)options; (void)ru;
    /* Delegate to wait4 */
    return sys_wait4(-1, NULL, options, ru);
}

/* =========================================================
 * set_robust_list / rseq stubs
 * ========================================================= */

s64 sys_set_robust_list(void *head, size_t len) {
    (void)head; (void)len;
    /* Futex robust list - not needed for single-threaded busybox */
    return 0;
}

s64 sys_rseq_stub(u32 *rseq, u32 rseq_len, int flags, u32 sig) {
    (void)rseq; (void)rseq_len; (void)flags; (void)sig;
    /* Restartable sequences - kernel-side support not implemented */
    return -ENOSYS;  /* glibc handles -ENOSYS gracefully */
}
