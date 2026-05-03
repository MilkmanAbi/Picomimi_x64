/**
 * Picomimi-x64 Syscall Table Initialization
 *
 * Wires every implemented syscall handler into syscall_table[].
 * Unimplemented entries point to sys_ni_syscall (returns -ENOSYS).
 *
 * The dispatch path:
 *   userspace SYSCALL instruction
 *   → arch/x86_64/idt/interrupts.S (syscall_entry)
 *   → syscall_dispatch(regs)
 *   → syscall_table[rax](rdi, rsi, rdx, r10, r8, r9)
 *
 * Linux x86_64 calling convention:
 *   syscall number: rax
 *   arg1: rdi  arg2: rsi  arg3: rdx
 *   arg4: r10  arg5: r8   arg6: r9
 *   return: rax (negative = -errno)
 */

#include <kernel/types.h>
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/xnu_compat.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

/* =========================================================
 * External syscall implementations
 * ========================================================= */

/* process.c / syscall.c */
extern s64 sys_getpid(void);
extern s64 sys_gettid(void);
extern s64 sys_getppid(void);
extern s64 sys_getuid(void);
extern s64 sys_geteuid(void);
extern s64 sys_getgid(void);
extern s64 sys_getegid(void);
extern s64 sys_fork(void);
extern s64 sys_vfork(void);
extern s64 sys_clone(u64 flags, void *stack, int *parent_tid, int *child_tid, u64 tls);
extern s64 sys_execve(const char *filename, char *const argv[], char *const envp[]);
extern s64 sys_exit(int status);
extern s64 sys_exit_group(int status);
extern s64 sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
extern s64 sys_waitid(int idtype, pid_t id, void *infop, int options, struct rusage *ru);
extern s64 sys_prctl(int option, u64 arg2, u64 arg3, u64 arg4, u64 arg5);
extern s64 sys_arch_prctl(int code, u64 addr);
extern s64 sys_set_tid_address(int *tidptr);
extern s64 sys_sched_yield(void);
extern s64 sys_getrlimit(int resource, struct rlimit *rlim);
extern s64 sys_setrlimit(int resource, const struct rlimit *rlim);
extern s64 sys_getrusage(int who, struct rusage *ru);
extern s64 sys_sysinfo(struct sysinfo *info);
extern s64 sys_setpgid(pid_t pid, pid_t pgid);
extern s64 sys_getpgid(pid_t pid);
extern s64 sys_getpgrp(void);
extern s64 sys_setsid(void);
extern s64 sys_getsid(pid_t pid);
extern s64 sys_setuid(uid_t uid);
extern s64 sys_setgid(gid_t gid);
extern s64 sys_setreuid(uid_t ruid, uid_t euid);
extern s64 sys_setregid(gid_t rgid, gid_t egid);
extern s64 sys_setresuid(uid_t ruid, uid_t euid, uid_t suid);
extern s64 sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
extern s64 sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid);
extern s64 sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
extern s64 sys_setfsuid(uid_t fsuid);
extern s64 sys_setfsgid(gid_t fsgid);
extern s64 sys_ptrace(long request, pid_t pid, void *addr, void *data);

/* signal.c */
extern s64 sys_kill(pid_t pid, int sig);
extern s64 sys_tkill(pid_t tid, int sig);
extern s64 sys_tgkill(pid_t tgid, pid_t tid, int sig);
extern s64 sys_rt_sigaction(int sig, const struct sigaction *act,
                             struct sigaction *oact, size_t sigsetsize);
extern s64 sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset,
                               size_t sigsetsize);
extern s64 sys_rt_sigreturn(void);
extern s64 sys_rt_sigpending(sigset_t *set, size_t sigsetsize);
extern s64 sys_pause(void);
extern s64 sys_sigaltstack(const void *ss, void *oss);

/* timer.c */
extern s64 sys_alarm(unsigned int seconds);
extern s64 sys_nanosleep(const struct timespec *req, struct timespec *rem);
extern s64 sys_clock_gettime(int clk_id, struct timespec *tp);
extern s64 sys_clock_getres(int clk_id, struct timespec *res);
extern s64 sys_clock_settime(int clk_id, const struct timespec *tp);
extern s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz);
extern s64 sys_settimeofday(const struct timeval *tv, const struct timezone *tz);
extern s64 sys_time(time_t *tloc);

/* fs/vfs.c + kernel/fs_syscalls.c */
extern s64 sys_read(int fd, void *buf, size_t count);
extern s64 sys_write(int fd, const void *buf, size_t count);
extern s64 sys_sendfile(int out_fd, int in_fd, s64 *offset, size_t count);
extern s64 sys_open(const char *filename, int flags, u32 mode);
extern s64 sys_close(int fd);
extern s64 sys_stat(const char *filename, struct linux_stat *statbuf);
extern s64 sys_fstat(int fd, struct linux_stat *statbuf);
extern s64 sys_lstat(const char *name, struct linux_stat *statbuf);
extern s64 sys_lseek(int fd, s64 offset, int whence);
extern s64 sys_mmap(void *addr, size_t len, int prot, int flags, int fd, s64 off);
extern s64 sys_mprotect(void *addr, size_t len, int prot);
extern s64 sys_munmap(void *addr, size_t len);
extern s64 sys_brk(void *addr);
extern s64 sys_ioctl(int fd, u64 cmd, u64 arg);
extern s64 sys_access(const char *filename, int mode);
extern s64 sys_pipe(int *pipefd);
extern s64 sys_pipe2(int *pipefd, int flags);
extern s64 sys_dup(int oldfd);
extern s64 sys_dup2(int oldfd, int newfd);
extern s64 sys_dup3(int oldfd, int newfd, int flags);
extern s64 sys_fcntl(int fd, int cmd, u64 arg);
extern s64 sys_getcwd(char *buf, size_t size);
extern s64 sys_chdir(const char *path);
extern s64 sys_fchdir(int fd);
extern s64 sys_mkdir(const char *pathname, u32 mode);
extern s64 sys_mkdirat(int dirfd, const char *pathname, u32 mode);
extern s64 sys_rmdir(const char *pathname);
extern s64 sys_unlink(const char *pathname);
extern s64 sys_unlinkat(int dirfd, const char *pathname, int flags);
extern s64 sys_rename(const char *oldpath, const char *newpath);
extern s64 sys_link(const char *oldpath, const char *newpath);
extern s64 sys_symlink(const char *target, const char *linkpath);
extern s64 sys_readlink(const char *pathname, char *buf, size_t bufsiz);
extern s64 sys_chmod(const char *pathname, u32 mode);
extern s64 sys_fchmod(int fd, u32 mode);
extern s64 sys_chown(const char *pathname, u32 owner, u32 group);
extern s64 sys_fchown(int fd, u32 owner, u32 group);
extern s64 sys_umask(u32 mask);
extern s64 sys_getdents64(int fd, struct linux_dirent64 *dirp, unsigned int count);
extern s64 sys_flock(int fd, int operation);
extern s64 sys_fsync(int fd);
extern s64 sys_fdatasync(int fd);
extern s64 sys_sync(void);
extern s64 sys_truncate(const char *path, s64 length);
extern s64 sys_ftruncate(int fd, s64 length);
extern s64 sys_statfs(const char *path, struct statfs *buf);
extern s64 sys_fstatfs(int fd, struct statfs *buf);
extern s64 sys_readv(int fd, const struct iovec *iov, int iovcnt);
extern s64 sys_writev(int fd, const struct iovec *iov, int iovcnt);
extern s64 sys_pread64(int fd, void *buf, size_t count, s64 offset);
extern s64 sys_pwrite64(int fd, const void *buf, size_t count, s64 offset);
extern s64 sys_mount(const char *source, const char *target,
                     const char *fstype, u64 flags, const void *data);
extern s64 sys_umount2(const char *target, int flags);
extern s64 sys_openat(int dirfd, const char *pathname, int flags, u32 mode);
extern s64 sys_mknodat(int dirfd, const char *pathname, u32 mode, dev_t dev);
extern s64 sys_fchownat(int dirfd, const char *pathname, u32 owner, u32 group, int flags);
extern s64 sys_newfstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);
extern s64 sys_renameat(int olddir, const char *old, int newdir, const char *new);
extern s64 sys_linkat(int olddir, const char *old, int newdir, const char *new, int flags);
extern s64 sys_symlinkat(const char *target, int newdir, const char *linkpath);
extern s64 sys_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
extern s64 sys_fchmodat(int dirfd, const char *pathname, u32 mode, int flags);
extern s64 sys_faccessat(int dirfd, const char *pathname, int mode, int flags);

/* sysfs.c */
extern s64 sys_uname(struct utsname *buf);
extern s64 sys_sethostname(const char *name, size_t len);
extern s64 sys_setdomainname(const char *name, size_t len);

/* chrdev.c */
extern s64 sys_getrandom(void *buf, size_t buflen, unsigned int flags);

/* mm/advanced_mm.c */
extern s64 sys_mremap(void *old_addr, size_t old_size, size_t new_size,
                       int flags, void *new_addr);
extern s64 sys_msync(void *addr, size_t len, int flags);
extern s64 sys_madvise(void *addr, size_t len, int advice);
extern s64 sys_mlock(const void *addr, size_t len);
extern s64 sys_munlock(const void *addr, size_t len);
extern s64 sys_mlockall(int flags);
extern s64 sys_munlockall(void);
extern s64 sys_mincore(void *addr, size_t len, unsigned char *vec);

/* misc */
extern s64 sys_reboot(int magic, int magic2, int cmd, void *arg);
extern s64 sys_capget(void *hdrp, void *datap);
extern s64 sys_capset(void *hdrp, const void *datap);
extern s64 sys_chroot(const char *path);
extern s64 sys_pivot_root(const char *new_root, const char *put_old);

/* inotify */
extern s64 sys_inotify_init(void);
extern s64 sys_inotify_init1(int flags);
extern s64 sys_inotify_add_watch(int fd, const char *pathname, u32 mask);
extern s64 sys_inotify_rm_watch(int fd, int wd);

/* epoll */
extern s64 sys_epoll_create(int size);
extern s64 sys_epoll_create1(int flags);
extern s64 sys_epoll_ctl(int epfd, int op, int fd, void *event);
extern s64 sys_epoll_wait(int epfd, void *events, int maxevents, int timeout);
extern s64 sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout,
                            const sigset_t *sigmask, size_t sigsetsize);

/* eventfd / signalfd / timerfd */
extern s64 sys_eventfd(unsigned int initval);
extern s64 sys_eventfd2(unsigned int initval, int flags);
extern s64 sys_signalfd(int fd, const sigset_t *mask, size_t sizemask);
extern s64 sys_signalfd4(int fd, const sigset_t *mask, size_t sizemask, int flags);
extern s64 sys_timerfd_create(int clockid, int flags);
extern s64 sys_timerfd_settime(int fd, int flags, const void *new, void *old);
extern s64 sys_timerfd_gettime(int fd, void *curr);

/* select / poll */
extern s64 sys_select(int nfds, void *readfds, void *writefds, void *exceptfds,
                       struct timeval *timeout);
extern s64 sys_pselect6(int nfds, void *r, void *w, void *e,
                         const struct timespec *ts, const void *sigmask);
extern s64 sys_poll(void *fds, unsigned int nfds, int timeout);
extern s64 sys_ppoll(void *fds, unsigned int nfds, const struct timespec *tmo,
                     const sigset_t *sigmask, size_t sigsetsize);

/* futex */
extern s64 sys_futex(u32 *uaddr, int op, u32 val, const struct timespec *timeout,
                     u32 *uaddr2, u32 val3);

/* socket (stub) */
extern s64 sys_socket(int domain, int type, int protocol);
extern s64 sys_bind(int sockfd, const void *addr, u32 addrlen);
extern s64 sys_connect(int sockfd, const void *addr, u32 addrlen);
extern s64 sys_listen(int sockfd, int backlog);
extern s64 sys_accept(int sockfd, void *addr, u32 *addrlen);
extern s64 sys_accept4(int sockfd, void *addr, u32 *addrlen, int flags);
extern s64 sys_sendto(int sockfd, const void *buf, size_t len, int flags,
                      const void *dest_addr, u32 addrlen);
extern s64 sys_recvfrom(int sockfd, void *buf, size_t len, int flags,
                        void *src_addr, u32 *addrlen);
extern s64 sys_sendmsg(int sockfd, const void *msg, int flags);
extern s64 sys_recvmsg(int sockfd, void *msg, int flags);
extern s64 sys_shutdown(int sockfd, int how);
extern s64 sys_getsockname(int sockfd, void *addr, u32 *addrlen);
extern s64 sys_getpeername(int sockfd, void *addr, u32 *addrlen);
extern s64 sys_socketpair(int domain, int type, int protocol, int *sv);
extern s64 sys_setsockopt(int sockfd, int level, int optname,
                           const void *optval, u32 optlen);
extern s64 sys_getsockopt(int sockfd, int level, int optname,
                           void *optval, u32 *optlen);

/* =========================================================
 * Stubs for not-yet-implemented syscalls
 * ========================================================= */

static s64 sys_stub_0(void) { return -ENOSYS; }

/* Cast helpers — syscall_table entries take (u64,u64,u64,u64,u64,u64) */
typedef s64 (*sc6)(u64,u64,u64,u64,u64,u64);

#define SYSCALL(nr, fn)  syscall_table[(nr)] = (syscall_fn_t)(fn)
#define STUB(nr)         syscall_table[(nr)] = (syscall_fn_t)sys_stub_0

/* =========================================================
 * syscall_dispatch — called from assembly entry point
 * ========================================================= */

/* Register frame layout from SAVE_REGS in interrupts.S
 *
 * SAVE_REGS pushes in order: r15, r14, r13, r12, r11, r10, r9, r8,
 *                            rbp, rdi, rsi, rdx, rcx, rbx, rax
 * Stack grows DOWN so rax ends up at the LOWEST address (rsp points to it).
 * Preceding that (higher addresses) are the iretq frame fields pushed
 * by the CPU/entry stub:
 *   vector, error_code, rip, cs, rflags, user_rsp, ss
 *
 * Memory layout from rsp upward:
 *   +0   rax    ← rsp after SAVE_REGS
 *   +8   rbx
 *   +16  rcx
 *   +24  rdx
 *   +32  rsi
 *   +40  rdi
 *   +48  rbp
 *   +56  r8
 *   +64  r9
 *   +72  r10
 *   +80  r11
 *   +88  r12
 *   +96  r13
 *   +104 r14
 *   +112 r15
 *   +120 vector      (pushed as $0x80 by syscall_entry)
 *   +128 error_code  (pushed as $0 by syscall_entry)
 *   +136 rip         (rcx = return address)
 *   +144 cs          (0x2B user CS)
 *   +152 rflags      (r11 = rflags)
 *   +160 user_rsp    (from gs:8)
 *   +168 ss          (0x23 user SS)
 */
typedef struct {
    /* Lowest address first — matches SAVE_REGS push order (rax pushed last = lowest) */
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 vector, error_code;
    u64 rip, cs, rflags, user_rsp, ss;
} __packed sc_regs_t;

s64 syscall_dispatch(sc_regs_t *regs) {
    u64 nr = regs->rax;
    u64 a1 = regs->rdi;
    u64 a2 = regs->rsi;
    u64 a3 = regs->rdx;
    u64 a4 = regs->r10;   /* Linux ABI: syscall arg4 = R10, not RCX */
    u64 a5 = regs->r8;
    u64 a6 = regs->r9;

    /* ----------------------------------------------------------------
     * XNU/Darwin personality: intercept before Linux table lookup.
     * Mach-O binaries issue syscalls as 0x2000000|bsd_nr (BSD class)
     * or 0x1000000|mach_nr (Mach class). Linux syscall numbers are
     * always < 512 so the high bits are unambiguous.
     * ---------------------------------------------------------------- */
    if (current && current->personality == PERSONALITY_XNU) {
        u32 cls = XNU_SYSCALL_CLASS(nr);
        if (cls == XNU_CLASS_BSD || cls == XNU_CLASS_MACH || cls == XNU_CLASS_MDEP) {
            /* Store frame for Mach trap use */
            if (current) current->trap_frame = (trap_frame_t *)regs;
            s64 ret = xnu_syscall_dispatch(nr, a1, a2, a3, a4, a5, a6);
            if (current) current->trap_frame = NULL;
            regs->rax = (u64)ret;
            if (current && current->sighand) {
                extern void do_signal(trap_frame_t *frame);
                do_signal(current->trap_frame);
            }
            return ret;
        }
        /* Falls through if a Linux-range syscall is issued from XNU task
         * (e.g. in a compatibility shim that deliberately uses Linux ABI) */
    }
    /* Trace all syscalls from non-kernel tasks (pids > 1) */
    if (current && current->pid >= 2) {
        static int strace_total = 0;
        if (0 && strace_total < 100) {
            strace_total++;
            printk(KERN_INFO "[strace] pid=%d nr=%llu a1=0x%llx a2=0x%llx a3=0x%llx\n",
                   (int)current->pid, nr, a1, a2, a3);
        }
    }

    if (nr >= NR_syscalls) {
        printk(KERN_DEBUG "[syscall] unknown syscall %llu\n", nr);
        return -ENOSYS;
    }

    syscall_fn_t fn = syscall_table[nr];
    if (!fn) {
        printk(KERN_WARNING "[syscall] UNIMPL syscall %llu from pid %d\n",
               nr, current ? (int)current->pid : -1);
        return -ENOSYS;
    }

    /* Store syscall frame so task_fork can access user return context */
    if (current) current->trap_frame = (trap_frame_t *)regs;
    s64 ret = fn(a1, a2, a3, a4, a5, a6);
    if (current) current->trap_frame = NULL;
    /* Write return value back into saved RAX so iretq restores it */
    regs->rax = (u64)ret;

    /* Deliver pending signals on syscall return */
    if (current && current->sighand) {
        extern void do_signal(trap_frame_t *frame);
        do_signal(current->trap_frame);
    }

    return ret;
}

/* =========================================================
 * syscall_init_table — fill the dispatch table
 * ========================================================= */

void syscall_init_table(void) {
    /* Zero out entire table → all unimplemented */
    memset(syscall_table, 0, sizeof(syscall_table));

    /* ---- File I/O ---- */
    SYSCALL(__NR_read,          sys_read);
    SYSCALL(__NR_write,         sys_write);
    SYSCALL(40,                 sys_sendfile);  /* sendfile */
    SYSCALL(__NR_open,          sys_open);
    SYSCALL(__NR_close,         sys_close);
    SYSCALL(__NR_stat,          sys_stat);
    SYSCALL(__NR_fstat,         sys_fstat);
    SYSCALL(__NR_lstat,         sys_lstat);
    SYSCALL(__NR_lseek,         sys_lseek);
    SYSCALL(__NR_ioctl,         sys_ioctl);
    SYSCALL(__NR_access,        sys_access);
    SYSCALL(__NR_dup,           sys_dup);
    SYSCALL(__NR_dup2,          sys_dup2);
    SYSCALL(__NR_dup3,          sys_dup3);
    SYSCALL(__NR_fcntl,         sys_fcntl);
    SYSCALL(__NR_getcwd,        sys_getcwd);
    SYSCALL(__NR_chdir,         sys_chdir);
    SYSCALL(__NR_fchdir,        sys_fchdir);
    SYSCALL(__NR_mkdir,         sys_mkdir);
    SYSCALL(__NR_mkdirat,       sys_mkdirat);
    SYSCALL(__NR_rmdir,         sys_rmdir);
    SYSCALL(__NR_unlink,        sys_unlink);
    SYSCALL(__NR_unlinkat,      sys_unlinkat);
    SYSCALL(__NR_rename,        sys_rename);
    SYSCALL(__NR_link,          sys_link);
    SYSCALL(__NR_symlink,       sys_symlink);
    SYSCALL(__NR_readlink,      sys_readlink);
    SYSCALL(__NR_chmod,         sys_chmod);
    SYSCALL(__NR_fchmod,        sys_fchmod);
    SYSCALL(__NR_chown,         sys_chown);
    SYSCALL(__NR_fchown,        sys_fchown);
    SYSCALL(__NR_umask,         sys_umask);
    SYSCALL(__NR_getdents64,    sys_getdents64);
    SYSCALL(__NR_getdents,      sys_getdents64);   /* alias */
    SYSCALL(__NR_flock,         sys_flock);
    SYSCALL(__NR_fsync,         sys_fsync);
    SYSCALL(__NR_fdatasync,     sys_fdatasync);
    SYSCALL(__NR_sync,          sys_sync);
    SYSCALL(__NR_truncate,      sys_truncate);
    SYSCALL(__NR_ftruncate,     sys_ftruncate);
    SYSCALL(__NR_statfs,        sys_statfs);
    SYSCALL(__NR_fstatfs,       sys_fstatfs);
    SYSCALL(__NR_readv,         sys_readv);
    SYSCALL(__NR_writev,        sys_writev);
    SYSCALL(__NR_pread64,       sys_pread64);
    SYSCALL(__NR_pwrite64,      sys_pwrite64);
    SYSCALL(__NR_mount,         sys_mount);
    SYSCALL(__NR_umount2,       sys_umount2);
    SYSCALL(__NR_openat,        sys_openat);
    SYSCALL(__NR_mknodat,       sys_mknodat);
    SYSCALL(__NR_fchownat,      sys_fchownat);
    SYSCALL(__NR_newfstatat,    sys_newfstatat);
    SYSCALL(__NR_renameat,      sys_renameat);
    SYSCALL(__NR_linkat,        sys_linkat);
    SYSCALL(__NR_symlinkat,     sys_symlinkat);
    SYSCALL(__NR_readlinkat,    sys_readlinkat);
    SYSCALL(__NR_fchmodat,      sys_fchmodat);
    SYSCALL(__NR_faccessat,     sys_faccessat);
    SYSCALL(__NR_creat,         sys_open);         /* creat = open with flags */
    SYSCALL(__NR_chroot,        sys_chroot);
    SYSCALL(__NR_pivot_root,    sys_pivot_root);

    /* ---- Pipes ---- */
    SYSCALL(__NR_pipe,          sys_pipe);
    SYSCALL(__NR_pipe2,         sys_pipe2);

    /* ---- Memory ---- */
    SYSCALL(__NR_mmap,          sys_mmap);
    SYSCALL(__NR_mprotect,      sys_mprotect);
    SYSCALL(__NR_munmap,        sys_munmap);
    SYSCALL(__NR_brk,           sys_brk);
    SYSCALL(__NR_mremap,        sys_mremap);
    SYSCALL(__NR_msync,         sys_msync);
    SYSCALL(__NR_madvise,       sys_madvise);
    SYSCALL(__NR_mlock,         sys_mlock);
    SYSCALL(__NR_munlock,       sys_munlock);
    SYSCALL(__NR_mlockall,      sys_mlockall);
    SYSCALL(__NR_munlockall,    sys_munlockall);
    SYSCALL(__NR_mincore,       sys_mincore);

    /* ---- Process ---- */
    SYSCALL(__NR_getpid,        sys_getpid);
    SYSCALL(__NR_gettid,        sys_gettid);
    SYSCALL(__NR_getppid,       sys_getppid);
    SYSCALL(__NR_getuid,        sys_getuid);
    SYSCALL(__NR_geteuid,       sys_geteuid);
    SYSCALL(__NR_getgid,        sys_getgid);
    SYSCALL(__NR_getegid,       sys_getegid);
    SYSCALL(__NR_setuid,        sys_setuid);
    SYSCALL(__NR_setgid,        sys_setgid);
    SYSCALL(__NR_setreuid,      sys_setreuid);
    SYSCALL(__NR_setregid,      sys_setregid);
    SYSCALL(__NR_setresuid,     sys_setresuid);
    SYSCALL(__NR_getresuid,     sys_getresuid);
    SYSCALL(__NR_setresgid,     sys_setresgid);
    SYSCALL(__NR_getresgid,     sys_getresgid);
    SYSCALL(__NR_setfsuid,      sys_setfsuid);
    SYSCALL(__NR_setfsgid,      sys_setfsgid);
    SYSCALL(__NR_setpgid,       sys_setpgid);
    SYSCALL(__NR_getpgid,       sys_getpgid);
    SYSCALL(__NR_getpgrp,       sys_getpgrp);
    SYSCALL(__NR_setsid,        sys_setsid);
    SYSCALL(__NR_getsid,        sys_getsid);
    SYSCALL(__NR_fork,          sys_fork);
    SYSCALL(__NR_vfork,         sys_vfork);
    SYSCALL(__NR_clone,         sys_clone);
    SYSCALL(__NR_execve,        sys_execve);
    SYSCALL(__NR_exit,          sys_exit);
    SYSCALL(__NR_exit_group,    sys_exit_group);
    SYSCALL(__NR_wait4,         sys_wait4);
    SYSCALL(__NR_waitid,        sys_waitid);
    SYSCALL(__NR_prctl,         sys_prctl);
    SYSCALL(__NR_arch_prctl,    sys_arch_prctl);
    SYSCALL(__NR_set_tid_address, sys_set_tid_address);
    SYSCALL(__NR_sched_yield,   sys_sched_yield);
    SYSCALL(__NR_getrlimit,     sys_getrlimit);
    SYSCALL(__NR_setrlimit,     sys_setrlimit);
    SYSCALL(__NR_getrusage,     sys_getrusage);
    SYSCALL(__NR_sysinfo,       sys_sysinfo);
    SYSCALL(__NR_ptrace,        sys_ptrace);
    SYSCALL(__NR_capget,        sys_capget);
    SYSCALL(__NR_capset,        sys_capset);
    SYSCALL(__NR_reboot,        sys_reboot);

    /* ---- Signals ---- */
    SYSCALL(__NR_kill,              sys_kill);
    SYSCALL(__NR_tkill,             sys_tkill);
    SYSCALL(__NR_tgkill,            sys_tgkill);
    SYSCALL(__NR_rt_sigaction,      sys_rt_sigaction);
    SYSCALL(__NR_rt_sigprocmask,    sys_rt_sigprocmask);
    SYSCALL(__NR_rt_sigreturn,      sys_rt_sigreturn);
    SYSCALL(__NR_rt_sigpending,     sys_rt_sigpending);
    SYSCALL(__NR_pause,             sys_pause);
    SYSCALL(__NR_sigaltstack,       sys_sigaltstack);

    /* ---- Timers ---- */
    SYSCALL(__NR_alarm,             sys_alarm);
    SYSCALL(__NR_nanosleep,         sys_nanosleep);
    SYSCALL(__NR_clock_gettime,     sys_clock_gettime);
    SYSCALL(__NR_clock_getres,      sys_clock_getres);
    SYSCALL(__NR_clock_settime,     sys_clock_settime);
    SYSCALL(__NR_gettimeofday,      sys_gettimeofday);
    SYSCALL(__NR_settimeofday,      sys_settimeofday);
    SYSCALL(__NR_time,              sys_time);
    SYSCALL(__NR_timerfd_create,    sys_timerfd_create);
    SYSCALL(__NR_timerfd_settime,   sys_timerfd_settime);
    SYSCALL(__NR_timerfd_gettime,   sys_timerfd_gettime);

    /* ---- Polling ---- */
    SYSCALL(__NR_select,            sys_select);
    SYSCALL(__NR_pselect6,          sys_pselect6);
    SYSCALL(__NR_poll,              sys_poll);
    SYSCALL(__NR_ppoll,             sys_ppoll);
    SYSCALL(__NR_epoll_create,      sys_epoll_create);
    SYSCALL(__NR_epoll_create1,     sys_epoll_create1);
    SYSCALL(__NR_epoll_ctl,         sys_epoll_ctl);
    SYSCALL(__NR_epoll_wait,        sys_epoll_wait);
    SYSCALL(__NR_epoll_pwait,       sys_epoll_pwait);
    SYSCALL(__NR_inotify_init,      sys_inotify_init);
    SYSCALL(__NR_inotify_init1,     sys_inotify_init1);
    SYSCALL(__NR_inotify_add_watch, sys_inotify_add_watch);
    SYSCALL(__NR_inotify_rm_watch,  sys_inotify_rm_watch);

    /* ---- eventfd / signalfd ---- */
    SYSCALL(__NR_eventfd,           sys_eventfd);
    SYSCALL(__NR_eventfd2,          sys_eventfd2);
    SYSCALL(__NR_signalfd,          sys_signalfd);
    SYSCALL(__NR_signalfd4,         sys_signalfd4);

    /* ---- Futex ---- */
    SYSCALL(__NR_futex,             sys_futex);

    /* ---- Sockets (stub layer) ---- */
    SYSCALL(__NR_socket,        sys_socket);
    SYSCALL(__NR_bind,          sys_bind);
    SYSCALL(__NR_connect,       sys_connect);
    SYSCALL(__NR_listen,        sys_listen);
    SYSCALL(__NR_accept,        sys_accept);
    SYSCALL(__NR_accept4,       sys_accept4);
    SYSCALL(__NR_sendto,        sys_sendto);
    SYSCALL(__NR_recvfrom,      sys_recvfrom);
    SYSCALL(__NR_sendmsg,       sys_sendmsg);
    SYSCALL(__NR_recvmsg,       sys_recvmsg);
    SYSCALL(__NR_shutdown,      sys_shutdown);
    SYSCALL(__NR_getsockname,   sys_getsockname);
    SYSCALL(__NR_getpeername,   sys_getpeername);
    SYSCALL(__NR_socketpair,    sys_socketpair);
    SYSCALL(__NR_setsockopt,    sys_setsockopt);
    SYSCALL(__NR_getsockopt,    sys_getsockopt);

    /* ---- System info ---- */
    SYSCALL(__NR_uname,             sys_uname);
    SYSCALL(__NR_sethostname,       sys_sethostname);
    SYSCALL(__NR_setdomainname,     sys_setdomainname);
    SYSCALL(__NR_getrandom,         sys_getrandom);

    /* --- Additional syscalls needed by busybox/glibc startup --- */
    extern s64 sys_prlimit64(pid_t, int, const void*, void*);
    extern s64 sys_set_robust_list(void*, size_t);
    extern s64 sys_rseq_stub(u32*, u32, int, u32);
    SYSCALL(__NR_set_robust_list,   sys_set_robust_list);
    SYSCALL(__NR_prlimit64,         sys_prlimit64);
    /* rseq (334) - restartable sequences, just stub */
    SYSCALL(334,                    sys_rseq_stub);

    printk(KERN_INFO "[syscall] table initialized: %d entries wired\n",
           NR_syscalls);
}
