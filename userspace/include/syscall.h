/**
 * Picomimi-x64 Userspace Syscall Interface
 * Thin inline wrappers — zero overhead, no libc dependency
 */
#pragma once

typedef long ssize_t;
typedef unsigned long size_t;
typedef long off_t;
typedef int pid_t;

/* Syscall numbers — must match kernel/syscall_table.c */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_stat        4
#define SYS_fstat       5
#define SYS_lstat       6
#define SYS_lseek       8
#define SYS_mmap        9
#define SYS_munmap      11
#define SYS_brk         12
#define SYS_ioctl       16
#define SYS_access      21
#define SYS_getpid      39
#define SYS_fork        57
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_kill        62
#define SYS_getdents64  217
#define SYS_openat      257
#define SYS_nanosleep   35
#define SYS_getcwd      79
#define SYS_chdir       80
#define SYS_mkdir       83
#define SYS_rmdir       84
#define SYS_unlink      87
#define SYS_rename      82
#define SYS_dup         32
#define SYS_dup2        33
#define SYS_pipe        22
#define SYS_fcntl       72
#define SYS_uname       63
#define SYS_socket      41
#define SYS_connect     42
#define SYS_write_to_fb 400   /* Picomimi extension: write pixels via mmap */

/* Raw syscall wrappers */
static inline long __syscall0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n)
                     : "rcx","r11","memory");
    return r;
}
static inline long __syscall1(long n, long a1) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n),"D"(a1)
                     : "rcx","r11","memory");
    return r;
}
static inline long __syscall2(long n, long a1, long a2) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n),"D"(a1),"S"(a2)
                     : "rcx","r11","memory");
    return r;
}
static inline long __syscall3(long n, long a1, long a2, long a3) {
    long r;
    register long _a3 __asm__("rdx") = a3;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n),"D"(a1),"S"(a2),"d"(a3)
                     : "rcx","r11","memory");
    (void)_a3;
    return r;
}
static inline long __syscall4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long _a4 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n),"D"(a1),"S"(a2),"d"(a3),"r"(_a4)
                     : "rcx","r11","memory");
    return r;
}

/* Public wrappers */
static inline ssize_t sys_read(int fd, void *buf, size_t n)
    { return (ssize_t)__syscall3(SYS_read, fd, (long)buf, (long)n); }
static inline ssize_t sys_write(int fd, const void *buf, size_t n)
    { return (ssize_t)__syscall3(SYS_write, fd, (long)buf, (long)n); }
static inline int sys_open(const char *p, int f, int m)
    { return (int)__syscall3(SYS_open, (long)p, f, m); }
static inline int sys_close(int fd)
    { return (int)__syscall1(SYS_close, fd); }
static inline void sys_exit(int code)
    { __syscall1(SYS_exit, code); __builtin_unreachable(); }
static inline pid_t sys_getpid(void)
    { return (pid_t)__syscall0(SYS_getpid); }
static inline pid_t sys_fork(void)
    { return (pid_t)__syscall0(SYS_fork); }
static inline long sys_execve(const char *p, char *const av[], char *const ev[])
    { return __syscall3(SYS_execve, (long)p, (long)av, (long)ev); }
static inline int sys_chdir(const char *p)
    { return (int)__syscall1(SYS_chdir, (long)p); }
static inline long sys_getcwd(char *buf, size_t sz)
    { return __syscall2(SYS_getcwd, (long)buf, (long)sz); }
static inline long sys_getdents64(int fd, void *buf, unsigned int cnt)
    { return __syscall3(SYS_getdents64, fd, (long)buf, cnt); }
static inline int sys_mkdir(const char *p, int m)
    { return (int)__syscall2(SYS_mkdir, (long)p, m); }
static inline int sys_unlink(const char *p)
    { return (int)__syscall1(SYS_unlink, (long)p); }
static inline long sys_lseek(int fd, off_t off, int w)
    { return __syscall3(SYS_lseek, fd, off, w); }
static inline pid_t sys_wait4(pid_t pid, int *st, int opts, void *ru)
    { return (pid_t)__syscall4(SYS_wait4, pid, (long)st, opts, (long)ru); }
static inline int sys_kill(pid_t pid, int sig)
    { return (int)__syscall2(SYS_kill, pid, sig); }
static inline int sys_dup2(int o, int n)
    { return (int)__syscall2(SYS_dup2, o, n); }
static inline int sys_pipe(int *fds)
    { return (int)__syscall1(SYS_pipe, (long)fds); }

/* brk */
#define SYS_brk 12
static inline long sys_brk(long addr)
    { return __syscall1(SYS_brk, addr); }
