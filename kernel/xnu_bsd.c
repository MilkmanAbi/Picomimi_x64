/**
 * Picomimi-x64 — XNU BSD Syscall Shims
 * kernel/xnu_bsd.c
 *
 * Maps Darwin BSD syscall numbers (class 0x2000000) to the kernel's
 * existing sys_* implementations.
 *
 * Three categories:
 *   DIRECT  — argument layout identical, call sys_* straight through
 *   SHIM    — minor fixup (errno sign, struct repack, arg reorder)
 *   STUB    — not yet implemented, returns -ENOSYS or a safe default
 *
 * RULES:
 *   - Never modify existing sys_* functions.
 *   - All error codes translated through xnu_errno() before return.
 *   - struct stat fixups copy from Linux kstat into darwin_stat64_t.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/xnu_compat.h>
#include <kernel/signal.h>
#include <fs/vfs.h>
#include <net/socket.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>

/* Forward-declare structs that live only in .c files */
struct itimerval;
struct pollfd;
struct rusage;
struct iovec;
struct rlimit;
struct timespec;
struct timeval;
struct stat;
struct sigaction;

/* =========================================================
 * errno translation
 * Most Darwin errnos == Linux errnos (both POSIX).
 * A few differ — map them here.
 * ========================================================= */

s64 xnu_errno(s64 linux_err) {
    if (linux_err >= 0) return linux_err;
    int e = (int)(-linux_err);
    /* Most Darwin errnos match Linux (both POSIX) for range 1-44 */
    if (e <= 44) return -(s64)e;
    /* Map Linux errnos above 44 to Darwin equivalents */
    switch (e) {
    case 95:  return (s64)-DARWIN_ENOTSUP;       /* EOPNOTSUPP */
    case 125: return -DARWIN_ECANCELED;     /* ECANCELED  */
    case 130: return -DARWIN_EOWNERDEAD;    /* EOWNERDEAD */
    case 131: return -DARWIN_ENOTRECOVERABLE; /* ENOTRECOVERABLE */
    case 34:  return -34;   /* ERANGE */
    case 62:  return -DARWIN_ETIME;         /* ETIME */
    case 61:  return -DARWIN_ENODATA;       /* ENODATA */
    case 97:  return -DARWIN_EAFNOSUPPORT;  /* EAFNOSUPPORT */
    default:  return -(s64)e;
    }
}

/* =========================================================
 * External sys_* prototypes (all already implemented)
 * ========================================================= */

extern s64 sys_read(int fd, void *buf, size_t count);
extern s64 sys_write(int fd, const void *buf, size_t count);
extern s64 sys_open(const char *path, int flags, u32 mode);
extern s64 sys_close(int fd);
extern s64 sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *ru);
extern s64 sys_link(const char *old, const char *newp);
extern s64 sys_unlink(const char *path);
extern s64 sys_chdir(const char *path);
extern s64 sys_fchdir(int fd);
extern s64 sys_mknod(const char *path, u32 mode, u64 dev);
extern s64 sys_chmod(const char *path, u32 mode);
extern s64 sys_chown(const char *path, u32 uid, u32 gid);
extern s64 sys_getpid(void);
extern s64 sys_setuid(u32 uid);
extern s64 sys_getuid(void);
extern s64 sys_geteuid(void);
extern s64 sys_kill(pid_t pid, int sig);
extern s64 sys_getppid(void);
extern s64 sys_dup(int fd);
extern s64 sys_pipe(int *fds);
extern s64 sys_getegid(void);
extern s64 sys_getgid(void);
extern s64 sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
extern s64 sys_ioctl(int fd, u64 req, u64 arg);
extern s64 sys_symlink(const char *target, const char *linkpath);
extern s64 sys_readlink(const char *path, char *buf, size_t bufsz);
extern s64 sys_execve(const char *filename, char *const argv[], char *const envp[]);
extern s64 sys_umask(u32 mask);
extern s64 sys_chroot(const char *path);
extern s64 sys_msync(void *addr, size_t length, int flags);
extern s64 sys_vfork(void);
extern s64 sys_munmap(void *addr, size_t length);
extern s64 sys_mprotect(void *addr, size_t len, int prot);
extern s64 sys_madvise(void *addr, size_t len, int advice);
extern s64 sys_getgroups(int size, u32 *list);
extern s64 sys_setgroups(int size, const u32 *list);
extern s64 sys_getpgrp(void);
extern s64 sys_setpgid(pid_t pid, pid_t pgid);
extern s64 sys_dup2(int old, int newfd);
extern s64 sys_fcntl(int fd, int cmd, u64 arg);
extern s64 sys_select(int nfds, void *r, void *w, void *e, struct timeval *tv);
extern s64 sys_poll(void *fds, u32 nfds, int timeout);
extern s64 sys_fsync(int fd);
extern s64 sys_socket(int domain, int type, int proto);
extern s64 sys_connect(int fd, const struct sockaddr *addr, u32 addrlen);
extern s64 sys_bind(int fd, const struct sockaddr *addr, u32 addrlen);
extern s64 sys_setsockopt(int fd, int level, int optname, const void *optval, u32 optlen);
extern s64 sys_listen(int fd, int backlog);
struct timezone;
extern s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz);
extern s64 sys_getrusage(int who, struct rusage *ru);
extern s64 sys_getsockopt(int fd, int level, int optname, void *optval, u32 *optlen);
extern s64 sys_readv(int fd, const struct iovec *iov, int iovcnt);
extern s64 sys_writev(int fd, const struct iovec *iov, int iovcnt);
extern s64 sys_fchown(int fd, u32 uid, u32 gid);
extern s64 sys_fchmod(int fd, u32 mode);
extern s64 sys_rename(const char *old, const char *newp);
extern s64 sys_flock(int fd, int op);
extern s64 sys_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *addr, u32 addrlen);
extern s64 sys_shutdown(int fd, int how);
extern s64 sys_socketpair(int domain, int type, int proto, int sv[2]);
extern s64 sys_mkdir(const char *path, u32 mode);
extern s64 sys_rmdir(const char *path);
extern s64 sys_setsid(void);
extern s64 sys_getpgid(pid_t pid);
extern s64 sys_pread64(int fd, void *buf, size_t count, s64 offset);
extern s64 sys_pwrite64(int fd, const void *buf, size_t count, s64 offset);
extern s64 sys_stat(const char *path, struct stat *buf);
extern s64 sys_fstat(int fd, struct stat *buf);
extern s64 sys_lstat(const char *path, struct stat *buf);
/* =========================================================
 * Stubs for syscalls declared but not yet implemented
 * in the base kernel — keeps the linker happy and gives
 * Mach-O apps a safe fallback.
 * ========================================================= */
static s64 stub_pathconf(const char *p, int n) { (void)p;(void)n; return -ENOSYS; }
static s64 stub_fpathconf(int fd, int n) { (void)fd;(void)n; return -ENOSYS; }
static s64 stub_setitimer(int w, const void *nv, void *ov) { (void)w;(void)nv;(void)ov; return 0; }
static s64 stub_getitimer(int w, void *v) { (void)w;(void)v; return 0; }
static s64 stub_setpriority(int w, u32 who, int p) { (void)w;(void)who;(void)p; return 0; }
static s64 stub_getpriority(int w, u32 who) { (void)w;(void)who; return 0; }
static s64 stub_utimes(const char *p, const void *t) { (void)p;(void)t; return 0; }
static s64 stub_mkfifo(const char *p, u32 m) { (void)p;(void)m; return -ENOSYS; }
static s64 stub_sigsuspend(const void *m) { (void)m; return -EINTR; }
static s64 stub_sigprocmask(int h, const void *s, void *os) {
    (void)h;(void)s;(void)os; return 0;
}
static s64 stub_fstatat(int dfd, const char *p, void *buf, int f) {
    (void)dfd;(void)p;(void)buf;(void)f; return -ENOSYS;
}
extern s64 sys_getrlimit(int resource, struct rlimit *rl);
extern s64 sys_setrlimit(int resource, const struct rlimit *rl);
extern s64 sys_mmap(void *addr, size_t len, int prot, int flags, int fd, s64 off);
extern s64 sys_lseek(int fd, s64 offset, int whence);
extern s64 sys_truncate(const char *path, s64 len);
extern s64 sys_ftruncate(int fd, s64 len);
extern s64 sys_poll(void *fds, u32 nfds, int timeout);
extern s64 sys_getsid(pid_t pid);
extern s64 sys_recvfrom(int fd, void *buf, size_t len, int flags,
                        struct sockaddr *addr, u32 *addrlen);
extern s64 sys_recvmsg(int fd, struct msghdr *msg, int flags);
extern s64 sys_sendmsg(int fd, const struct msghdr *msg, int flags);
extern s64 sys_accept(int fd, struct sockaddr *addr, u32 *addrlen);
extern s64 sys_getpeername(int fd, struct sockaddr *addr, u32 *addrlen);
extern s64 sys_getsockname(int fd, struct sockaddr *addr, u32 *addrlen);
extern s64 sys_access(const char *path, int mode);
extern s64 sys_mincore(void *addr, size_t len, u8 *vec);
extern s64 sys_gettid(void);
extern s64 sys_exit(int status);
extern s64 sys_fork(void);
extern s64 sys_nanosleep(const struct timespec *req, struct timespec *rem);
extern s64 sys_clock_gettime(int clkid, struct timespec *tp);
extern s64 sys_getentropy(void *buf, size_t len);
extern s64 sys_openat(int dirfd, const char *path, int flags, u32 mode);
extern s64 sys_mkdirat(int dirfd, const char *path, u32 mode);
extern s64 sys_unlinkat(int dirfd, const char *path, int flags);
extern s64 sys_renameat(int odfd, const char *old, int ndfd, const char *newp);
extern s64 sys_linkat(int odfd, const char *old, int ndfd, const char *newp, int flags);
extern s64 sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz);
extern s64 sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
extern s64 sys_waitid(int idtype, pid_t id, void *infop, int options, struct rusage *ru);
extern s64 sys_mlock(const void *addr, size_t len);
extern s64 sys_munlock(const void *addr, size_t len);
extern s64 sys_mlockall(int flags);
extern s64 sys_munlockall(void);

/* =========================================================
 * stat64 shim — translate Linux stat → darwin_stat64_t
 * ========================================================= */

/*
 * Linux struct stat (x86_64 — kernel ABI):
 *   u64 st_dev, u64 st_ino, u64 st_nlink,
 *   u32 st_mode, u32 st_uid, u32 st_gid, u32 _pad0,
 *   u64 st_rdev, s64 st_size,
 *   s64 st_blksize, s64 st_blocks,
 *   u64 st_atime, u64 st_atime_nsec,
 *   u64 st_mtime, u64 st_mtime_nsec,
 *   u64 st_ctime, u64 st_ctime_nsec,
 *   s64 _unused[3]
 */
typedef struct {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 _pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    u64 st_atime;
    u64 st_atime_nsec;
    u64 st_mtime;
    u64 st_mtime_nsec;
    u64 st_ctime;
    u64 st_ctime_nsec;
    s64 _unused[3];
} linux_stat_t;

static s64 xnu_stat_shim(const char *path, darwin_stat64_t *dstat) {
    linux_stat_t lst;
    memset(&lst, 0, sizeof(lst));
    s64 r = sys_stat(path, (struct stat *)&lst);
    if (r < 0) return xnu_errno(r);

    darwin_stat64_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.st_dev           = (u32)lst.st_dev;
    ds.st_mode          = (u16)lst.st_mode;
    ds.st_nlink         = (u16)lst.st_nlink;
    ds.st_ino           = lst.st_ino;
    ds.st_uid           = lst.st_uid;
    ds.st_gid           = lst.st_gid;
    ds.st_rdev          = (u32)lst.st_rdev;
    ds.st_size          = lst.st_size;
    ds.st_blocks        = lst.st_blocks;
    ds.st_blksize       = (u32)lst.st_blksize;
    ds.st_atimespec_sec = (s64)lst.st_atime;
    ds.st_atimespec_nsec= (s64)lst.st_atime_nsec;
    ds.st_mtimespec_sec = (s64)lst.st_mtime;
    ds.st_mtimespec_nsec= (s64)lst.st_mtime_nsec;
    ds.st_ctimespec_sec = (s64)lst.st_ctime;
    ds.st_ctimespec_nsec= (s64)lst.st_ctime_nsec;
    /* birthtime = ctime (best we can do without a real birth-time field) */
    ds.st_birthtimespec_sec  = ds.st_ctimespec_sec;
    ds.st_birthtimespec_nsec = ds.st_ctimespec_nsec;
    memcpy(dstat, &ds, sizeof(ds));
    return 0;
}

static s64 xnu_fstat_shim(int fd, darwin_stat64_t *dstat) {
    linux_stat_t lst;
    memset(&lst, 0, sizeof(lst));
    s64 r = sys_fstat(fd, (struct stat *)&lst);
    if (r < 0) return xnu_errno(r);

    darwin_stat64_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.st_dev           = (u32)lst.st_dev;
    ds.st_mode          = (u16)lst.st_mode;
    ds.st_nlink         = (u16)lst.st_nlink;
    ds.st_ino           = lst.st_ino;
    ds.st_uid           = lst.st_uid;
    ds.st_gid           = lst.st_gid;
    ds.st_rdev          = (u32)lst.st_rdev;
    ds.st_size          = lst.st_size;
    ds.st_blocks        = lst.st_blocks;
    ds.st_blksize       = (u32)lst.st_blksize;
    ds.st_atimespec_sec = (s64)lst.st_atime;
    ds.st_atimespec_nsec= (s64)lst.st_atime_nsec;
    ds.st_mtimespec_sec = (s64)lst.st_mtime;
    ds.st_mtimespec_nsec= (s64)lst.st_mtime_nsec;
    ds.st_ctimespec_sec = (s64)lst.st_ctime;
    ds.st_ctimespec_nsec= (s64)lst.st_ctime_nsec;
    ds.st_birthtimespec_sec  = ds.st_ctimespec_sec;
    ds.st_birthtimespec_nsec = ds.st_ctimespec_nsec;
    memcpy(dstat, &ds, sizeof(ds));
    return 0;
}

static s64 xnu_lstat_shim(const char *path, darwin_stat64_t *dstat) {
    linux_stat_t lst;
    memset(&lst, 0, sizeof(lst));
    s64 r = sys_lstat(path, (struct stat *)&lst);
    if (r < 0) return xnu_errno(r);

    darwin_stat64_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.st_dev           = (u32)lst.st_dev;
    ds.st_mode          = (u16)lst.st_mode;
    ds.st_nlink         = (u16)lst.st_nlink;
    ds.st_ino           = lst.st_ino;
    ds.st_uid           = lst.st_uid;
    ds.st_gid           = lst.st_gid;
    ds.st_rdev          = (u32)lst.st_rdev;
    ds.st_size          = lst.st_size;
    ds.st_blocks        = lst.st_blocks;
    ds.st_blksize       = (u32)lst.st_blksize;
    ds.st_atimespec_sec = (s64)lst.st_atime;
    ds.st_atimespec_nsec= (s64)lst.st_atime_nsec;
    ds.st_mtimespec_sec = (s64)lst.st_mtime;
    ds.st_mtimespec_nsec= (s64)lst.st_mtime_nsec;
    ds.st_ctimespec_sec = (s64)lst.st_ctime;
    ds.st_ctimespec_nsec= (s64)lst.st_ctime_nsec;
    ds.st_birthtimespec_sec  = ds.st_ctimespec_sec;
    ds.st_birthtimespec_nsec = ds.st_ctimespec_nsec;
    memcpy(dstat, &ds, sizeof(ds));
    return 0;
}

/* =========================================================
 * Darwin-specific: csops (code signing ops) — always succeed
 * ========================================================= */

#define CS_OPS_STATUS       0
#define CS_VALID            0x00000001
#define CS_ADHOC            0x00000002

static s64 xnu_csops(pid_t pid, u32 ops, void *useraddr, u32 usersize) {
    (void)pid;
    if (ops == CS_OPS_STATUS) {
        /* Report binary as ad-hoc signed and valid — required for dyld */
        if (useraddr && usersize >= 4) {
            u32 flags = CS_VALID | CS_ADHOC;
            memcpy(useraddr, &flags, 4);
        }
        return 0;
    }
    /* All other cs ops: silently succeed */
    return 0;
}

/* =========================================================
 * Darwin: shared_region_check_np — no shared cache, addr=0
 * ========================================================= */
static s64 xnu_shared_region_check_np(u64 *start_address) {
    if (start_address) *start_address = 0;
    return (s64)-DARWIN_ENOTSUP;  /* signals dyld to not use shared region */
}

/* =========================================================
 * Darwin: issetugid — always return 0 (not setuid/setgid)
 * ========================================================= */
static s64 xnu_issetugid(void) { return 0; }

/* =========================================================
 * Darwin: proc_info — stub (returns -EPERM for most ops)
 * ========================================================= */
static s64 xnu_proc_info(int callnum, int pid, u32 flavor,
                          u64 arg, void *buffer, u32 bufsize) {
    (void)callnum; (void)pid; (void)flavor;
    (void)arg; (void)buffer; (void)bufsize;
    return -EPERM;
}

/* =========================================================
 * Darwin: thread_selfid — return tid
 * ========================================================= */
static s64 xnu_thread_selfid(void) {
    return sys_gettid();
}

/* =========================================================
 * Darwin: bsdthread_register — needed by libpthread init
 * Just store values and return success.
 * ========================================================= */
static s64 xnu_bsdthread_register(u64 threadstart, u64 wqthread,
                                   u32 flags, void *stack_addr_hint,
                                   void *targetconc_ptr,
                                   u32 dispatchqueue_offset,
                                   u32 tsd_offset) {
    (void)threadstart; (void)wqthread; (void)flags;
    (void)stack_addr_hint; (void)targetconc_ptr;
    (void)dispatchqueue_offset; (void)tsd_offset;
    /* Success — we don't implement the pthread work queue but dyld
     * and basic pthread_create go through bsdthread_create instead */
    return 0;
}

/* =========================================================
 * Darwin: bsdthread_create — create a pthread
 * Maps to sys_clone (same as Linux pthreads).
 * ========================================================= */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_SYSVSEM   0x00040000
#define CLONE_SETTLS    0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

extern s64 sys_clone(u64 flags, void *stack, int *ptid, int *ctid, u64 tls);

static s64 xnu_bsdthread_create(void *func, void *func_arg,
                                 void *stack, void *pthread,
                                 u32 flags) {
    (void)func; (void)func_arg; (void)stack; (void)pthread; (void)flags;
    /*
     * Real implementation needs to set up the thread in Darwin's
     * libpthread convention (func is called with func_arg, pthread ptr
     * is stored in %gs). For now return ENOTSUP — apps that use
     * pthread_create will see it fail gracefully; single-threaded
     * Mach-O binaries work fine.
     */
    return (s64)-DARWIN_ENOTSUP;
}

/* =========================================================
 * Darwin: bsdthread_terminate
 * ========================================================= */
static s64 xnu_bsdthread_terminate(void *stackaddr, u32 freesize,
                                    u32 port, u32 sem) {
    (void)stackaddr; (void)freesize; (void)port; (void)sem;
    sys_exit(0);
    __builtin_unreachable();
}

/* =========================================================
 * Darwin: getentropy — map to Linux getrandom / our /dev/random
 * ========================================================= */
static s64 xnu_getentropy(void *buf, size_t len) {
    if (!buf || len > 256) return -EINVAL;
    /* Use our existing getentropy / getrandom */
    extern s64 sys_getrandom(void *buf, size_t buflen, u32 flags);
    s64 r = sys_getrandom(buf, len, 0);
    return r < 0 ? xnu_errno(r) : 0;
}

/* =========================================================
 * Darwin: sysctl stub
 * Apps probe sysctl for hw.cpucount, hw.memsize, kern.version, etc.
 * Return plausible values for the most common queries.
 * ========================================================= */
static s64 xnu_sysctl(int *name, u32 namelen, void *oldp, size_t *oldlenp,
                       void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || namelen == 0) return -EINVAL;

    /* CTL_KERN=1 CTL_HW=6 */
    if (namelen >= 2) {
        int ctl = name[0];
        int sub = name[1];

        /* CTL_HW */
        if (ctl == 6) {
            u32 val32 = 0;
            u64 val64 = 0;
            bool use32 = true;
            switch (sub) {
            case 3: /* HW_NCPU    */ val32 = 1; break;
            case 7: /* HW_MEMSIZE */ val64 = 256ULL * 1024 * 1024; use32 = false; break;
            case 25:/* HW_NCPUONLINE */ val32 = 1; break;
            default: return (s64)-DARWIN_ENOTSUP;
            }
            if (oldp && oldlenp) {
                if (use32) {
                    if (*oldlenp >= 4) { memcpy(oldp, &val32, 4); *oldlenp = 4; }
                } else {
                    if (*oldlenp >= 8) { memcpy(oldp, &val64, 8); *oldlenp = 8; }
                }
            }
            return 0;
        }

        /* CTL_KERN */
        if (ctl == 1) {
            switch (sub) {
            case 14: /* KERN_PROC */ return -EPERM; /* permission denied = expected */
            case 65: /* KERN_USRSTACK64 */ {
                u64 stk = 0x00007FFFFFFFE000ULL;
                if (oldp && oldlenp && *oldlenp >= 8) {
                    memcpy(oldp, &stk, 8); *oldlenp = 8;
                }
                return 0;
            }
            default: return (s64)-DARWIN_ENOTSUP;
            }
        }
    }
    return (s64)-DARWIN_ENOTSUP;
}

static s64 xnu_sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                              void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name) return -EINVAL;

    /* Commonly queried by dyld / libSystem */
    if (strcmp(name, "hw.ncpu") == 0 ||
        strcmp(name, "hw.activecpu") == 0 ||
        strcmp(name, "hw.logicalcpu") == 0 ||
        strcmp(name, "hw.physicalcpu") == 0) {
        u32 v = 1;
        if (oldp && oldlenp && *oldlenp >= 4) { memcpy(oldp, &v, 4); *oldlenp = 4; }
        return 0;
    }
    if (strcmp(name, "hw.memsize") == 0) {
        u64 v = 256ULL * 1024 * 1024;
        if (oldp && oldlenp && *oldlenp >= 8) { memcpy(oldp, &v, 8); *oldlenp = 8; }
        return 0;
    }
    if (strcmp(name, "kern.ostype") == 0) {
        const char *v = "Darwin";
        size_t len = strlen(v) + 1;
        if (oldp && oldlenp && *oldlenp >= len) {
            memcpy(oldp, v, len); *oldlenp = len;
        }
        return 0;
    }
    if (strcmp(name, "kern.osrelease") == 0 ||
        strcmp(name, "kern.osversion") == 0) {
        const char *v = "24.0.0";   /* macOS Sequoia kernel version */
        size_t len = strlen(v) + 1;
        if (oldp && oldlenp && *oldlenp >= len) {
            memcpy(oldp, v, len); *oldlenp = len;
        }
        return 0;
    }
    if (strcmp(name, "kern.version") == 0) {
        const char *v = "Darwin Kernel Version 24.0.0: Picomimi-x64 XNU compat";
        size_t len = strlen(v) + 1;
        if (oldp && oldlenp && *oldlenp >= len) {
            memcpy(oldp, v, len); *oldlenp = len;
        }
        return 0;
    }
    if (strcmp(name, "kern.hostname") == 0) {
        const char *v = "picomimi";
        size_t len = strlen(v) + 1;
        if (oldp && oldlenp && *oldlenp >= len) {
            memcpy(oldp, v, len); *oldlenp = len;
        }
        return 0;
    }
    /* Unrecognised — return ENOENT (sysctl convention on Darwin) */
    return -2; /* ENOENT in Darwin */
}

/* =========================================================
 * kqueue / kevent implementation
 *
 * kqueue() creates a special fd backed by a kqueue_state_t.
 * kevent() registers/dequeues events.
 *
 * Full epoll-grade implementation is future work.
 * For now we implement enough for basic polling (EVFILT_READ/WRITE)
 * so that CF-based apps don't hang at startup.
 * ========================================================= */

/* We store kqueue state in the file's private_data field.
 * The fd is a normal fd pointing to a dummy devfs node, with private_data
 * pointing to our kqueue_state_t. */

static int kqueue_release(file_t *file) {
    if (file && file->private_data) {
        kfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

int kqueue_create(void) {
    /* Allocate kqueue state */
    kqueue_state_t *kq = kmalloc(sizeof(kqueue_state_t), GFP_KERNEL);
    if (!kq) return -ENOMEM;
    memset(kq, 0, sizeof(*kq));
    spin_lock_init(&kq->lock);

    /* Allocate a file_t to represent the kqueue fd */
    file_t *f = kmalloc(sizeof(file_t), GFP_KERNEL);
    if (!f) { kfree(kq); return -ENOMEM; }
    memset(f, 0, sizeof(file_t));

    f->private_data = kq;
    /* f_op: we only need release to clean up; other ops return ENOSYS */
    /* Use a static fops — can't use compound literal at file scope with C99 easily,
     * so store the release pointer directly and check it in kqueue_kevent */
    /* For now: leave f_op NULL, kqueue_kevent will find kq via private_data */

    extern int get_unused_fd(void);
    extern void fd_install(int fd, file_t *file);

    int fd = get_unused_fd();
    if (fd < 0) { kfree(kq); kfree(f); return -EMFILE; }
    fd_install(fd, f);
    return fd;
}

/* Resolve a kqueue fd → kqueue_state_t */
static kqueue_state_t *kq_from_fd(int kqfd) {
    extern file_t *fget(unsigned int fd);
    file_t *f = fget((unsigned int)kqfd);
    if (!f || !f->private_data) return NULL;
    return (kqueue_state_t *)f->private_data;
}

int kqueue_kevent(int kqfd, const kevent_t *changelist, int nchanges,
                   kevent_t *eventlist, int nevents,
                   const struct timespec *timeout)
{
    kqueue_state_t *kq = kq_from_fd(kqfd);
    if (!kq) return -EBADF;

    /* Process changelist (registrations) */
    for (int i = 0; i < nchanges; i++) {
        const kevent_t *ev = &changelist[i];
        spin_lock(&kq->lock);
        if (ev->flags & EV_ADD) {
            /* Find existing or free slot */
            int slot = -1;
            for (int j = 0; j < kq->nregistered; j++) {
                if (kq->registered[j].ident == ev->ident &&
                    kq->registered[j].filter == (s16)ev->filter) {
                    slot = j; break;
                }
            }
            if (slot < 0 && kq->nregistered < KQUEUE_MAX_EVENTS)
                slot = kq->nregistered++;
            if (slot >= 0) {
                kq->registered[slot].ident  = ev->ident;
                kq->registered[slot].filter = (s16)ev->filter;
                kq->registered[slot].flags  = ev->flags;
                kq->registered[slot].fflags = ev->fflags;
                kq->registered[slot].data   = ev->data;
                kq->registered[slot].udata  = (u64)(uintptr_t)ev->udata;
            }
        } else if (ev->flags & EV_DELETE) {
            for (int j = 0; j < kq->nregistered; j++) {
                if (kq->registered[j].ident == ev->ident &&
                    kq->registered[j].filter == (s16)ev->filter) {
                    /* Shift remaining down */
                    for (int k = j; k < kq->nregistered - 1; k++)
                        kq->registered[k] = kq->registered[k + 1];
                    kq->nregistered--;
                    break;
                }
            }
        }
        spin_unlock(&kq->lock);
    }

    if (nevents == 0 || !eventlist) return 0;

    /*
     * Collect ready events.
     * For EVFILT_READ/WRITE we use a non-blocking poll on the underlying fd.
     * This is best-effort — a proper implementation would block here.
     */
    int nready = 0;
    spin_lock(&kq->lock);
    for (int i = 0; i < kq->nregistered && nready < nevents; i++) {
        kevent64_t *reg = &kq->registered[i];
        if (reg->flags & EV_DISABLE) continue;

        bool ready = false;
        s64  data  = 0;

        if (reg->filter == EVFILT_READ || reg->filter == EVFILT_WRITE) {
            int rfd = (int)reg->ident;
            /* Use a raw poll struct — layout: fd(int), events(short), revents(short) */
            struct { int fd; short events; short revents; } pfd;
            pfd.fd      = rfd;
            pfd.events  = (short)((reg->filter == EVFILT_READ) ? 1 : 4); /* POLLIN=1 POLLOUT=4 */
            pfd.revents = 0;
            s64 pr = sys_poll((void*)&pfd, 1, 0);
            if (pr > 0 && (pfd.revents & pfd.events)) {
                ready = true;
                data  = 1;
            }
        } else if (reg->filter == EVFILT_SIGNAL) {
            /* Signal events: check if signal is pending */
            ready = false; /* conservative: don't report */
        } else if (reg->filter == EVFILT_TIMER) {
            /* Timer events: report as ready (stub) */
            ready = true;
            data  = reg->data;
        }

        if (ready) {
            eventlist[nready].ident  = reg->ident;
            eventlist[nready].filter = reg->filter;
            eventlist[nready].flags  = EV_EOF; /* mark */
            eventlist[nready].fflags = 0;
            eventlist[nready].data   = data;
            eventlist[nready].udata  = (void *)(uintptr_t)reg->udata;
            nready++;
            if (reg->flags & EV_ONESHOT) {
                /* Remove */
                for (int k = i; k < kq->nregistered - 1; k++)
                    kq->registered[k] = kq->registered[k + 1];
                kq->nregistered--;
                i--;
            }
        }
    }
    spin_unlock(&kq->lock);

    if (nready == 0 && timeout) {
        /* Block if timeout is non-zero — use nanosleep as a proxy */
        if (timeout->tv_sec > 0 || timeout->tv_nsec > 0) {
            struct timespec ts = *timeout;
            sys_nanosleep(&ts, NULL);
        }
    }

    return nready;
}

/* =========================================================
 * BSD dispatch table
 * ========================================================= */

typedef s64 (*xnu_bsd_fn_t)(u64, u64, u64, u64, u64, u64);

/*
 * Macro helpers to cast our typed sys_* functions to the generic
 * 6-u64-argument dispatch type. This is the same pattern used by
 * the Linux syscall table.
 */
#define BSD_DIRECT(fn)  ((xnu_bsd_fn_t)(void*)(fn))
#define BSD_STUB()      ((xnu_bsd_fn_t)NULL)

/* Stub that returns -ENOSYS */
static s64 bsd_ni(u64 a1,u64 a2,u64 a3,u64 a4,u64 a5,u64 a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return -ENOSYS;
}

/*
 * xnu_bsd_dispatch — called for class-2 (BSD) Darwin syscalls
 */
s64 xnu_bsd_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    s64 ret;

    switch ((int)nr) {

    /* ---- trivial direct maps ---- */
    case XNU_NR_exit:           ret = sys_exit((int)a1); break;
    case XNU_NR_fork:           ret = sys_fork(); break;
    case XNU_NR_read:           ret = sys_read((int)a1, (void*)a2, (size_t)a3); break;
    case XNU_NR_write:          ret = sys_write((int)a1, (void*)a2, (size_t)a3); break;
    case XNU_NR_open:           ret = sys_open((const char*)a1, (int)a2, (u32)a3); break;
    case XNU_NR_open_nocancel:  ret = sys_open((const char*)a1, (int)a2, (u32)a3); break;
    case XNU_NR_close:
    case XNU_NR_close_nocancel: ret = sys_close((int)a1); break;
    case XNU_NR_wait4:
    case XNU_NR_wait4_nocancel: ret = sys_wait4((pid_t)a1,(int*)a2,(int)a3,(struct rusage*)a4); break;
    case XNU_NR_link:           ret = sys_link((const char*)a1,(const char*)a2); break;
    case XNU_NR_unlink:         ret = sys_unlink((const char*)a1); break;
    case XNU_NR_chdir:          ret = sys_chdir((const char*)a1); break;
    case XNU_NR_fchdir:         ret = sys_fchdir((int)a1); break;
    case XNU_NR_mknod:          ret = sys_mknod((const char*)a1,(u32)a2,(u64)a3); break;
    case XNU_NR_chmod:          ret = sys_chmod((const char*)a1,(u32)a2); break;
    case XNU_NR_chown:          ret = sys_chown((const char*)a1,(u32)a2,(u32)a3); break;
    case XNU_NR_getpid:         ret = sys_getpid(); break;
    case XNU_NR_setuid:         ret = sys_setuid((u32)a1); break;
    case XNU_NR_getuid:         ret = sys_getuid(); break;
    case XNU_NR_geteuid:        ret = sys_geteuid(); break;
    case XNU_NR_kill:           ret = sys_kill((pid_t)a1,(int)a2); break;
    case XNU_NR_getppid:        ret = sys_getppid(); break;
    case XNU_NR_dup:            ret = sys_dup((int)a1); break;
    case XNU_NR_pipe:           ret = sys_pipe((int*)a1); break;
    case XNU_NR_getegid:        ret = sys_getegid(); break;
    case XNU_NR_getgid:         ret = sys_getgid(); break;
    case XNU_NR_sigprocmask:    ret = stub_sigprocmask((int)a1,(const sigset_t*)a2,(sigset_t*)a3); break;
    case XNU_NR___pthread_sigmask: ret = stub_sigprocmask((int)a1,(const sigset_t*)a2,(sigset_t*)a3); break;
    case XNU_NR_ioctl:          ret = sys_ioctl((int)a1,(u64)a2,(u64)a3); break;
    case XNU_NR_symlink:        ret = sys_symlink((const char*)a1,(const char*)a2); break;
    case XNU_NR_readlink:       ret = sys_readlink((const char*)a1,(char*)a2,(size_t)a3); break;
    case XNU_NR_execve:         ret = sys_execve((const char*)a1,(char*const*)a2,(char*const*)a3); break;
    case XNU_NR_umask:          ret = sys_umask((u32)a1); break;
    case XNU_NR_chroot:         ret = sys_chroot((const char*)a1); break;
    case XNU_NR_msync:
    case XNU_NR_msync_nocancel: ret = sys_msync((void*)a1,(size_t)a2,(int)a3); break;
    case XNU_NR_vfork:          ret = sys_vfork(); break;
    case XNU_NR_munmap:         ret = sys_munmap((void*)a1,(size_t)a2); break;
    case XNU_NR_mprotect:       ret = sys_mprotect((void*)a1,(size_t)a2,(int)a3); break;
    case XNU_NR_madvise:        ret = sys_madvise((void*)a1,(size_t)a2,(int)a3); break;
    case XNU_NR_mincore:        ret = sys_mincore((void*)a1,(size_t)a2,(u8*)a3); break;
    case XNU_NR_getgroups:      ret = sys_getgroups((int)a1,(u32*)a2); break;
    case XNU_NR_setgroups:      ret = sys_setgroups((int)a1,(const u32*)a2); break;
    case XNU_NR_getpgrp:        ret = sys_getpgrp(); break;
    case XNU_NR_setpgid:        ret = sys_setpgid((pid_t)a1,(pid_t)a2); break;
    case XNU_NR_setitimer:      ret = stub_setitimer((int)a1,(const struct itimerval*)a2,(struct itimerval*)a3); break;
    case XNU_NR_getitimer:      ret = stub_getitimer((int)a1,(struct itimerval*)a2); break;
    case XNU_NR_dup2:           ret = sys_dup2((int)a1,(int)a2); break;
    case XNU_NR_fcntl:
    case XNU_NR_fcntl_nocancel: ret = sys_fcntl((int)a1,(int)a2,(u64)a3); break;
    case XNU_NR_select:
    case XNU_NR_select_nocancel: ret = sys_select((int)a1,(void*)a2,(void*)a3,(void*)a4,(struct timeval*)a5); break;
    case XNU_NR_fsync:
    case XNU_NR_fsync_nocancel: ret = sys_fsync((int)a1); break;
    case XNU_NR_fdatasync:      ret = sys_fsync((int)a1); break;
    case XNU_NR_setpriority:    ret = stub_setpriority((int)a1,(u32)a2,(int)a3); break;
    case XNU_NR_socket:         ret = sys_socket((int)a1,(int)a2,(int)a3); break;
    case XNU_NR_connect:
    case XNU_NR_connect_nocancel: ret = sys_connect((int)a1,(const struct sockaddr*)a2,(u32)a3); break;
    case XNU_NR_getpriority:    ret = stub_getpriority((int)a1,(u32)a2); break;
    case XNU_NR_bind:           ret = sys_bind((int)a1,(const struct sockaddr*)a2,(u32)a3); break;
    case XNU_NR_setsockopt:     ret = sys_setsockopt((int)a1,(int)a2,(int)a3,(const void*)a4,(u32)a5); break;
    case XNU_NR_listen:         ret = sys_listen((int)a1,(int)a2); break;
    case XNU_NR_sigsuspend:
    case XNU_NR_sigsuspend_nocancel: ret = stub_sigsuspend((const sigset_t*)a1); break;
    case XNU_NR_gettimeofday:   ret = sys_gettimeofday((struct timeval*)a1,(void*)a2); break;
    case XNU_NR_getrusage:      ret = sys_getrusage((int)a1,(struct rusage*)a2); break;
    case XNU_NR_getsockopt:     ret = sys_getsockopt((int)a1,(int)a2,(int)a3,(void*)a4,(u32*)a5); break;
    case XNU_NR_readv:
    case XNU_NR_readv_nocancel: ret = sys_readv((int)a1,(const struct iovec*)a2,(int)a3); break;
    case XNU_NR_writev:
    case XNU_NR_writev_nocancel: ret = sys_writev((int)a1,(const struct iovec*)a2,(int)a3); break;
    case XNU_NR_fchown:         ret = sys_fchown((int)a1,(u32)a2,(u32)a3); break;
    case XNU_NR_fchmod:         ret = sys_fchmod((int)a1,(u32)a2); break;
    case XNU_NR_rename:         ret = sys_rename((const char*)a1,(const char*)a2); break;
    case XNU_NR_flock:          ret = sys_flock((int)a1,(int)a2); break;
    case XNU_NR_mkfifo:         ret = stub_mkfifo((const char*)a1,(u32)a2); break;
    case XNU_NR_sendto:
    case XNU_NR_sendto_nocancel: ret = sys_sendto((int)a1,(void*)a2,(size_t)a3,(int)a4,(const struct sockaddr*)a5,(u32)a6); break;
    case XNU_NR_shutdown:       ret = sys_shutdown((int)a1,(int)a2); break;
    case XNU_NR_socketpair:     ret = sys_socketpair((int)a1,(int)a2,(int)a3,(int*)a4); break;
    case XNU_NR_mkdir:          ret = sys_mkdir((const char*)a1,(u32)a2); break;
    case XNU_NR_rmdir:          ret = sys_rmdir((const char*)a1); break;
    case XNU_NR_utimes:         ret = stub_utimes((const char*)a1,(const struct timeval*)a2); break;
    case XNU_NR_setsid:         ret = sys_setsid(); break;
    case XNU_NR_getpgid:        ret = sys_getpgid((pid_t)a1); break;
    case XNU_NR_getsid:         ret = sys_getsid((pid_t)a1); break;
    case XNU_NR_pread:
    case XNU_NR_pread_nocancel: ret = sys_pread64((int)a1,(void*)a2,(size_t)a3,(s64)a4); break;
    case XNU_NR_pwrite:
    case XNU_NR_pwrite_nocancel: ret = sys_pwrite64((int)a1,(void*)a2,(size_t)a3,(s64)a4); break;
    case XNU_NR_getrlimit:      ret = sys_getrlimit((int)a1,(struct rlimit*)a2); break;
    case XNU_NR_setrlimit:      ret = sys_setrlimit((int)a1,(const struct rlimit*)a2); break;
    case XNU_NR_mmap:           ret = sys_mmap((void*)a1,(size_t)a2,(int)a3,(int)a4,(int)a5,(s64)a6); break;
    case XNU_NR_lseek:          ret = sys_lseek((int)a1,(s64)a2,(int)a3); break;
    case XNU_NR_truncate:       ret = sys_truncate((const char*)a1,(s64)a2); break;
    case XNU_NR_ftruncate:      ret = sys_ftruncate((int)a1,(s64)a2); break;
    case XNU_NR_poll:
    case XNU_NR_poll_nocancel:  ret = sys_poll((struct pollfd*)a1,(u32)a2,(int)a3); break;
    case XNU_NR_recvfrom:
    case XNU_NR_recvfrom_nocancel: ret = sys_recvfrom((int)a1,(void*)a2,(size_t)a3,(int)a4,(struct sockaddr*)a5,(u32*)a6); break;
    case XNU_NR_recvmsg:
    case XNU_NR_recvmsg_nocancel: ret = sys_recvmsg((int)a1,(struct msghdr*)a2,(int)a3); break;
    case XNU_NR_sendmsg:
    case XNU_NR_sendmsg_nocancel: ret = sys_sendmsg((int)a1,(const struct msghdr*)a2,(int)a3); break;
    case XNU_NR_accept:
    case XNU_NR_accept_nocancel: ret = sys_accept((int)a1,(struct sockaddr*)a2,(u32*)a3); break;
    case XNU_NR_getpeername:    ret = sys_getpeername((int)a1,(struct sockaddr*)a2,(u32*)a3); break;
    case XNU_NR_getsockname:    ret = sys_getsockname((int)a1,(struct sockaddr*)a2,(u32*)a3); break;
    case XNU_NR_access:         ret = sys_access((const char*)a1,(int)a2); break;
    case XNU_NR_mlock:          ret = sys_mlock((const void*)a1,(size_t)a2); break;
    case XNU_NR_munlock:        ret = sys_munlock((const void*)a1,(size_t)a2); break;
    case XNU_NR_mlockall:       ret = sys_mlockall((int)a1); break;
    case XNU_NR_munlockall:     ret = sys_munlockall(); break;
    case XNU_NR_pathconf:       ret = stub_pathconf((const char*)a1,(int)a2); break;
    case XNU_NR_fpathconf:      ret = stub_fpathconf((int)a1,(int)a2); break;
    case XNU_NR_waitid:
    case XNU_NR_waitid_nocancel: ret = sys_waitid((int)a1,(pid_t)a2,(void*)a3,(int)a4,(struct rusage*)a5); break;
    case XNU_NR_sendfile:       ret = -ENOSYS; break; /* future */

    /* ---- stat shims (struct layout differs) ---- */
    case XNU_NR_stat:
    case XNU_NR_stat64:         ret = xnu_stat_shim((const char*)a1,(darwin_stat64_t*)a2); break;
    case XNU_NR_fstat:
    case XNU_NR_fstat64:        ret = xnu_fstat_shim((int)a1,(darwin_stat64_t*)a2); break;
    case XNU_NR_lstat:
    case XNU_NR_lstat64:        ret = xnu_lstat_shim((const char*)a1,(darwin_stat64_t*)a2); break;
    case XNU_NR_fstatat:
    case XNU_NR_fstatat64: {
        /* fstatat: if AT_FDCWD (-100) or no path, use stat; AT_SYMLINK_NOFOLLOW → lstat */
        #define AT_SYMLINK_NOFOLLOW 0x100
        linux_stat_t lst; memset(&lst,0,sizeof(lst));
        s64 r;
        if (a4 & AT_SYMLINK_NOFOLLOW)
            r = sys_lstat((const char*)a2,(struct stat*)&lst);
        else
            r = sys_stat((const char*)a2,(struct stat*)&lst);
        if (r < 0) { ret = xnu_errno(r); break; }
        darwin_stat64_t ds; memset(&ds,0,sizeof(ds));
        ds.st_dev=(u32)lst.st_dev; ds.st_mode=(u16)lst.st_mode;
        ds.st_nlink=(u16)lst.st_nlink; ds.st_ino=lst.st_ino;
        ds.st_uid=lst.st_uid; ds.st_gid=lst.st_gid; ds.st_rdev=(u32)lst.st_rdev;
        ds.st_size=lst.st_size; ds.st_blocks=lst.st_blocks; ds.st_blksize=(u32)lst.st_blksize;
        ds.st_atimespec_sec=(s64)lst.st_atime; ds.st_atimespec_nsec=(s64)lst.st_atime_nsec;
        ds.st_mtimespec_sec=(s64)lst.st_mtime; ds.st_mtimespec_nsec=(s64)lst.st_mtime_nsec;
        ds.st_ctimespec_sec=(s64)lst.st_ctime; ds.st_ctimespec_nsec=(s64)lst.st_ctime_nsec;
        ds.st_birthtimespec_sec=ds.st_ctimespec_sec;
        ds.st_birthtimespec_nsec=ds.st_ctimespec_nsec;
        memcpy((void*)a3,&ds,sizeof(ds)); ret=0; break;
    }

    /* ---- at-variants ---- */
    case XNU_NR_openat:
    case XNU_NR_openat_nocancel: ret = sys_openat((int)a1,(const char*)a2,(int)a3,(u32)a4); break;
    case XNU_NR_mkdirat:        ret = sys_mkdirat((int)a1,(const char*)a2,(u32)a3); break;
    case XNU_NR_unlinkat:       ret = sys_unlinkat((int)a1,(const char*)a2,(int)a3); break;
    case XNU_NR_renameat:       ret = sys_renameat((int)a1,(const char*)a2,(int)a3,(const char*)a4); break;
    case XNU_NR_faccessat:      ret = sys_access((const char*)a2,(int)a3); break; /* simplified */
    case XNU_NR_linkat:         ret = sys_linkat((int)a1,(const char*)a2,(int)a3,(const char*)a4,(int)a5); break;
    case XNU_NR_readlinkat:     ret = sys_readlinkat((int)a1,(const char*)a2,(char*)a3,(size_t)a4); break;
    case XNU_NR_symlinkat:      ret = sys_symlinkat((const char*)a1,(int)a2,(const char*)a3); break;

    /* ---- Darwin-specific ---- */
    case XNU_NR_csops:
    case XNU_NR_csops_audittoken: ret = xnu_csops((pid_t)a1,(u32)a2,(void*)a3,(u32)a4); break;
    case XNU_NR_shared_region_check_np: ret = xnu_shared_region_check_np((u64*)a1); break;
    case XNU_NR_issetugid:      ret = xnu_issetugid(); break;
    case XNU_NR_proc_info:      ret = xnu_proc_info((int)a1,(int)a2,(u32)a3,(u64)a4,(void*)a5,(u32)a6); break;
    case XNU_NR_thread_selfid:  ret = xnu_thread_selfid(); break;
    case XNU_NR_gettid:         ret = sys_gettid(); break;
    case XNU_NR_bsdthread_register: ret = xnu_bsdthread_register(a1,a2,(u32)a3,(void*)a4,(void*)a5,(u32)a6,0); break;
    case XNU_NR_bsdthread_create: ret = xnu_bsdthread_create((void*)a1,(void*)a2,(void*)a3,(void*)a4,(u32)a5); break;
    case XNU_NR_bsdthread_terminate: ret = xnu_bsdthread_terminate((void*)a1,(u32)a2,(u32)a3,(u32)a4); break;
    case XNU_NR_workq_open:     ret = 0; break;   /* stub success */
    case XNU_NR_workq_kernreturn: ret = -DARWIN_ENOTSUP; break;
    case XNU_NR_getentropy:     ret = xnu_getentropy((void*)a1,(size_t)a2); break;
    case XNU_NR_sysctl:         ret = xnu_sysctl((int*)a1,(u32)a2,(void*)a3,(size_t*)a4,(void*)a5,(size_t)a6); break;
    case XNU_NR_sysctlbyname:   ret = xnu_sysctlbyname((const char*)a1,(void*)a2,(size_t*)a3,(void*)a4,(size_t)a5); break;

    /* kqueue / kevent */
    case XNU_NR_kqueue:         ret = kqueue_create(); break;
    case XNU_NR_kevent:
    case XNU_NR_kevent64: {
        /* kevent(kqfd, changelist, nchanges, eventlist, nevents, timeout) */
        ret = kqueue_kevent((int)a1,
                            (const kevent_t*)a2, (int)a3,
                            (kevent_t*)a4, (int)a5,
                            (const struct timespec*)a6);
        break;
    }

    /* Mac-specific policy / audit / sandbox — silently succeed or ENOSYS */
    case XNU_NR___mac_syscall:
    case XNU_NR___mac_get_proc:
    case XNU_NR___mac_set_proc:
    case XNU_NR___mac_get_file:
    case XNU_NR___mac_set_file:
    case XNU_NR___mac_get_fd:
    case XNU_NR___mac_set_fd:   ret = 0; break;   /* pretend sandbox is disabled */

    /* Memory pressure / iopolicy — stub success */
    case XNU_NR_memorystatus_control:
    case XNU_NR_iopolicysys:
    case XNU_NR_process_policy: ret = 0; break;

    /* Audit — stub */
    case XNU_NR_audit:
    case XNU_NR_auditon:
    case XNU_NR_getauid:
    case XNU_NR_setauid:
    case XNU_NR_getaudit_addr:
    case XNU_NR_setaudit_addr:
    case XNU_NR_auditctl:       ret = -ENOSYS; break;

    /* kdebug tracing — silently drop */
    case XNU_NR_kdebug_trace:
    case XNU_NR_kdebug_trace64:
    case XNU_NR_kdebug_trace_string:
    case XNU_NR_kdebug_typefilter: ret = 0; break;

    /* xattr — not yet implemented */
    case XNU_NR_getxattr:
    case XNU_NR_fgetxattr:
    case XNU_NR_setxattr:
    case XNU_NR_fsetxattr:
    case XNU_NR_removexattr:
    case XNU_NR_fremovexattr:
    case XNU_NR_listxattr:
    case XNU_NR_flistxattr:     ret = -ENOSYS; break;

    /* psynch (Darwin pthread sync) — stub */
    case XNU_NR_psynch_mutexwait:
    case XNU_NR_psynch_mutexdrop:
    case XNU_NR_psynch_cvbroad:
    case XNU_NR_psynch_cvsignal:
    case XNU_NR_psynch_cvwait:
    case XNU_NR_psynch_rw_rdlock:
    case XNU_NR_psynch_rw_wrlock:
    case XNU_NR_psynch_rw_unlock:
    case XNU_NR_psynch_cvclrprepost: ret = 0; break;

    /* guarded fds — treat as regular */
    case XNU_NR_guarded_open_np:    ret = sys_open((const char*)a1,(int)a3,(u32)a4); break;
    case XNU_NR_guarded_close_np:   ret = sys_close((int)a1); break;
    case XNU_NR_guarded_kqueue_np:  ret = kqueue_create(); break;

    /* getattrlist — stub ENOSYS (only used by Finder-type tools) */
    case XNU_NR_getattrlist:
    case XNU_NR_fgetattrlist:
    case XNU_NR_fsetattrlist:
    case XNU_NR_getattrlistat:  ret = -ENOSYS; break;

    /* Persona, necp, nexus, kas_info — return permission denied */
    case XNU_NR_persona:
    case XNU_NR_necp_open:
    case XNU_NR_necp_client_action:
    case XNU_NR_kas_info:
    case XNU_NR_pid_suspend:
    case XNU_NR_pid_resume:     ret = -EPERM; break;

    /* gethostuuid — return zeroed UUID */
    case XNU_NR_gethostuuid: {
        if (a1) memset((void*)a1, 0, 16);
        ret = 0; break;
    }

    /* setprivexec, chflags, fchflags — ignore */
    case XNU_NR_setprivexec:
    case XNU_NR_chflags:
    case XNU_NR_fchflags:       ret = 0; break;

    /* thread_selfcounts */
    case XNU_NR_thread_selfcounts: ret = -ENOSYS; break;

    /* Default */
    default:
        printk(KERN_DEBUG "[xnu_bsd] unimplemented nr=%lu\n", (unsigned long)nr);
        ret = -ENOSYS;
        break;
    }

    return ret < 0 ? xnu_errno(ret) : ret;
}
