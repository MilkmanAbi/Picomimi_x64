/**
 * Picomimi-x64 System Call Definitions
 * 
 * Linux x86_64 compatible syscall numbers and structures
 * Full POSIX compliance target
 */
#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <kernel/types.h>

// ============================================================================
// SYSCALL NUMBERS (Linux x86_64 ABI)
// ============================================================================

#define __NR_read               0
#define __NR_write              1
#define __NR_open               2
#define __NR_close              3
#define __NR_stat               4
#define __NR_fstat              5
#define __NR_lstat              6
#define __NR_poll               7
#define __NR_lseek              8
#define __NR_mmap               9
#define __NR_mprotect           10
#define __NR_munmap             11
#define __NR_brk                12
#define __NR_rt_sigaction       13
#define __NR_rt_sigprocmask     14
#define __NR_rt_sigreturn       15
#define __NR_ioctl              16
#define __NR_pread64            17
#define __NR_pwrite64           18
#define __NR_readv              19
#define __NR_writev             20
#define __NR_access             21
#define __NR_pipe               22
#define __NR_select             23
#define __NR_sched_yield        24
#define __NR_mremap             25
#define __NR_msync              26
#define __NR_mincore            27
#define __NR_madvise            28
#define __NR_shmget             29
#define __NR_shmat              30
#define __NR_shmctl             31
#define __NR_dup                32
#define __NR_dup2               33
#define __NR_pause              34
#define __NR_nanosleep          35
#define __NR_getitimer          36
#define __NR_alarm              37
#define __NR_setitimer          38
#define __NR_getpid             39
#define __NR_sendfile           40
#define __NR_socket             41
#define __NR_connect            42
#define __NR_accept             43
#define __NR_sendto             44
#define __NR_recvfrom           45
#define __NR_sendmsg            46
#define __NR_recvmsg            47
#define __NR_shutdown           48
#define __NR_bind               49
#define __NR_listen             50
#define __NR_getsockname        51
#define __NR_getpeername        52
#define __NR_socketpair         53
#define __NR_setsockopt         54
#define __NR_getsockopt         55
#define __NR_clone              56
#define __NR_fork               57
#define __NR_vfork              58
#define __NR_execve             59
#define __NR_exit               60
#define __NR_wait4              61
#define __NR_kill               62
#define __NR_uname              63
#define __NR_semget             64
#define __NR_semop              65
#define __NR_semctl             66
#define __NR_shmdt              67
#define __NR_msgget             68
#define __NR_msgsnd             69
#define __NR_msgrcv             70
#define __NR_msgctl             71
#define __NR_fcntl              72
#define __NR_flock              73
#define __NR_fsync              74
#define __NR_fdatasync          75
#define __NR_truncate           76
#define __NR_ftruncate          77
#define __NR_getdents           78
#define __NR_getcwd             79
#define __NR_chdir              80
#define __NR_fchdir             81
#define __NR_rename             82
#define __NR_mkdir              83
#define __NR_rmdir              84
#define __NR_creat              85
#define __NR_link               86
#define __NR_unlink             87
#define __NR_symlink            88
#define __NR_readlink           89
#define __NR_chmod              90
#define __NR_fchmod             91
#define __NR_chown              92
#define __NR_fchown             93
#define __NR_lchown             94
#define __NR_umask              95
#define __NR_gettimeofday       96
#define __NR_getrlimit          97
#define __NR_getrusage          98
#define __NR_sysinfo            99
#define __NR_times              100
#define __NR_ptrace             101
#define __NR_getuid             102
#define __NR_syslog             103
#define __NR_getgid             104
#define __NR_setuid             105
#define __NR_setgid             106
#define __NR_geteuid            107
#define __NR_getegid            108
#define __NR_setpgid            109
#define __NR_getppid            110
#define __NR_getpgrp            111
#define __NR_setsid             112
#define __NR_setreuid           113
#define __NR_setregid           114
#define __NR_getgroups          115
#define __NR_setgroups          116
#define __NR_setresuid          117
#define __NR_getresuid          118
#define __NR_setresgid          119
#define __NR_getresgid          120
#define __NR_getpgid            121
#define __NR_setfsuid           122
#define __NR_setfsgid           123
#define __NR_getsid             124
#define __NR_capget             125
#define __NR_capset             126
#define __NR_rt_sigpending      127
#define __NR_rt_sigtimedwait    128
#define __NR_rt_sigqueueinfo    129
#define __NR_rt_sigsuspend      130
#define __NR_sigaltstack        131
#define __NR_utime              132
#define __NR_mknod              133
#define __NR_uselib             134
#define __NR_personality        135
#define __NR_ustat              136
#define __NR_statfs             137
#define __NR_fstatfs            138
#define __NR_sysfs              139
#define __NR_getpriority        140
#define __NR_setpriority        141
#define __NR_sched_setparam     142
#define __NR_sched_getparam     143
#define __NR_sched_setscheduler 144
#define __NR_sched_getscheduler 145
#define __NR_sched_get_priority_max 146
#define __NR_sched_get_priority_min 147
#define __NR_sched_rr_get_interval  148
#define __NR_mlock              149
#define __NR_munlock            150
#define __NR_mlockall           151
#define __NR_munlockall         152
#define __NR_vhangup            153
#define __NR_modify_ldt         154
#define __NR_pivot_root         155
#define __NR_prctl              157
#define __NR_arch_prctl         158
#define __NR_adjtimex           159
#define __NR_setrlimit          160
#define __NR_chroot             161
#define __NR_sync               162
#define __NR_acct               163
#define __NR_settimeofday       164
#define __NR_mount              165
#define __NR_umount2            166
#define __NR_swapon             167
#define __NR_swapoff            168
#define __NR_reboot             169
#define __NR_sethostname        170
#define __NR_setdomainname      171
#define __NR_iopl               172
#define __NR_ioperm             173
#define __NR_init_module        175
#define __NR_delete_module      176
#define __NR_quotactl           179
#define __NR_gettid             186
#define __NR_readahead          187
#define __NR_setxattr           188
#define __NR_lsetxattr          189
#define __NR_fsetxattr          190
#define __NR_getxattr           191
#define __NR_lgetxattr          192
#define __NR_fgetxattr          193
#define __NR_listxattr          194
#define __NR_llistxattr         195
#define __NR_flistxattr         196
#define __NR_removexattr        197
#define __NR_lremovexattr       198
#define __NR_fremovexattr       199
#define __NR_tkill              200
#define __NR_time               201
#define __NR_futex              202
#define __NR_sched_setaffinity  203
#define __NR_sched_getaffinity  204
#define __NR_io_setup           206
#define __NR_io_destroy         207
#define __NR_io_getevents       208
#define __NR_io_submit          209
#define __NR_io_cancel          210
#define __NR_lookup_dcookie     212
#define __NR_epoll_create       213
#define __NR_remap_file_pages   216
#define __NR_getdents64         217
#define __NR_set_tid_address    218
#define __NR_restart_syscall    219
#define __NR_semtimedop         220
#define __NR_fadvise64          221
#define __NR_timer_create       222
#define __NR_timer_settime      223
#define __NR_timer_gettime      224
#define __NR_timer_getoverrun   225
#define __NR_timer_delete       226
#define __NR_clock_settime      227
#define __NR_clock_gettime      228
#define __NR_clock_getres       229
#define __NR_clock_nanosleep    230
#define __NR_exit_group         231
#define __NR_epoll_wait         232
#define __NR_epoll_ctl          233
#define __NR_tgkill             234
#define __NR_utimes             235
#define __NR_mbind              237
#define __NR_set_mempolicy      238
#define __NR_get_mempolicy      239
#define __NR_mq_open            240
#define __NR_mq_unlink          241
#define __NR_mq_timedsend       242
#define __NR_mq_timedreceive    243
#define __NR_mq_notify          244
#define __NR_mq_getsetattr      245
#define __NR_kexec_load         246
#define __NR_waitid             247
#define __NR_add_key            248
#define __NR_request_key        249
#define __NR_keyctl             250
#define __NR_ioprio_set         251
#define __NR_ioprio_get         252
#define __NR_inotify_init       253
#define __NR_inotify_add_watch  254
#define __NR_inotify_rm_watch   255
#define __NR_migrate_pages      256
#define __NR_openat             257
#define __NR_mkdirat            258
#define __NR_mknodat            259
#define __NR_fchownat           260
#define __NR_futimesat          261
#define __NR_newfstatat         262
#define __NR_unlinkat           263
#define __NR_renameat           264
#define __NR_linkat             265
#define __NR_symlinkat          266
#define __NR_readlinkat         267
#define __NR_fchmodat           268
#define __NR_faccessat          269
#define __NR_pselect6           270
#define __NR_ppoll              271
#define __NR_unshare            272
#define __NR_set_robust_list    273
#define __NR_get_robust_list    274
#define __NR_splice             275
#define __NR_tee                276
#define __NR_sync_file_range    277
#define __NR_vmsplice           278
#define __NR_move_pages         279
#define __NR_utimensat          280
#define __NR_epoll_pwait        281
#define __NR_signalfd           282
#define __NR_timerfd_create     283
#define __NR_eventfd            284
#define __NR_fallocate          285
#define __NR_timerfd_settime    286
#define __NR_timerfd_gettime    287
#define __NR_accept4            288
#define __NR_signalfd4          289
#define __NR_eventfd2           290
#define __NR_epoll_create1      291
#define __NR_dup3               292
#define __NR_pipe2              293
#define __NR_inotify_init1      294
#define __NR_preadv             295
#define __NR_pwritev            296
#define __NR_rt_tgsigqueueinfo  297
#define __NR_perf_event_open    298
#define __NR_recvmmsg           299
#define __NR_fanotify_init      300
#define __NR_fanotify_mark      301
#define __NR_prlimit64          302
#define __NR_name_to_handle_at  303
#define __NR_open_by_handle_at  304
#define __NR_clock_adjtime      305
#define __NR_syncfs             306
#define __NR_sendmmsg           307
#define __NR_setns              308
#define __NR_getcpu             309
#define __NR_process_vm_readv   310
#define __NR_process_vm_writev  311
#define __NR_kcmp               312
#define __NR_finit_module       313
#define __NR_getrandom          318
#define __NR_memfd_create       319
#define __NR_copy_file_range    326
#define __NR_statx              332

#define NR_syscalls             512

// ============================================================================
// POSIX CONSTANTS
// ============================================================================

// File open flags
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_NOCTTY        0x0100
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_DSYNC         0x1000
#define O_SYNC          0x101000
#define O_RSYNC         0x101000
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_CLOEXEC       0x80000
#define O_ASYNC         0x2000
#define O_DIRECT        0x4000
#define O_LARGEFILE     0x8000
#define O_NOATIME       0x40000
#define O_PATH          0x200000
#define O_TMPFILE       0x410000

// File mode bits
#define S_IFMT          0170000
#define S_IFSOCK        0140000
#define S_IFLNK         0120000
#define S_IFREG         0100000
#define S_IFBLK         0060000
#define S_IFDIR         0040000
#define S_IFCHR         0020000
#define S_IFIFO         0010000
#define S_ISUID         0004000
#define S_ISGID         0002000
#define S_ISVTX         0001000
#define S_IRWXU         00700
#define S_IRUSR         00400
#define S_IWUSR         00200
#define S_IXUSR         00100
#define S_IRWXG         00070
#define S_IRGRP         00040
#define S_IWGRP         00020
#define S_IXGRP         00010
#define S_IRWXO         00007
#define S_IROTH         00004
#define S_IWOTH         00002
#define S_IXOTH         00001

#define S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)      (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)      (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)     (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)      (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m)     (((m) & S_IFMT) == S_IFSOCK)

// Seek constants
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2
#define SEEK_DATA       3
#define SEEK_HOLE       4

// mmap protection flags
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

// mmap flags
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_GROWSDOWN   0x0100
#define MAP_DENYWRITE   0x0800
#define MAP_EXECUTABLE  0x1000
#define MAP_LOCKED      0x2000
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MAP_NONBLOCK    0x10000
#define MAP_STACK       0x20000
#define MAP_HUGETLB     0x40000

#define MAP_FAILED      ((void *)-1)

// Clone flags
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_PTRACE    0x00002000
#define CLONE_VFORK     0x00004000
#define CLONE_PARENT    0x00008000
#define CLONE_THREAD    0x00010000
#define CLONE_NEWNS     0x00020000
#define CLONE_SYSVSEM   0x00040000
#define CLONE_SETTLS    0x00080000
#define CLONE_PARENT_SETTID     0x00100000
#define CLONE_CHILD_CLEARTID    0x00200000
#define CLONE_DETACHED  0x00400000
#define CLONE_UNTRACED  0x00800000
#define CLONE_CHILD_SETTID      0x01000000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000
#define CLONE_IO        0x80000000

// Wait options
#define WNOHANG         0x00000001
#define WUNTRACED       0x00000002
#define WSTOPPED        WUNTRACED
#define WEXITED         0x00000004
#define WCONTINUED      0x00000008
#define WNOWAIT         0x01000000

#define WEXITSTATUS(s)  (((s) & 0xff00) >> 8)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WIFSIGNALED(s)  (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WCOREDUMP(s)    ((s) & 0x80)

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_SETSIG        10
#define F_GETSIG        11
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC      1

// ============================================================================
// STRUCTURES (use types from kernel/types.h)
// ============================================================================

// stat structure for Linux x86_64
struct linux_stat {
    u64     st_dev;
    u64     st_ino;
    u64     st_nlink;
    u32     st_mode;
    u32     st_uid;
    u32     st_gid;
    u32     __pad0;
    u64     st_rdev;
    s64     st_size;
    s64     st_blksize;
    s64     st_blocks;
    u64     st_atime;
    u64     st_atime_nsec;
    u64     st_mtime;
    u64     st_mtime_nsec;
    u64     st_ctime;
    u64     st_ctime_nsec;
    s64     __reserved[3];
} __packed;

// Use stat from types.h, alias linux_stat
#define stat linux_stat

struct timezone {
    s32     tz_minuteswest;
    s32     tz_dsttime;
};

#ifndef _STRUCT_UTSNAME
#define _STRUCT_UTSNAME
struct utsname {
    char    sysname[65];
    char    nodename[65];
    char    release[65];
    char    version[65];
    char    machine[65];
    char    domainname[65];
};
#endif

#ifndef _STRUCT_RUSAGE
#define _STRUCT_RUSAGE
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    s64     ru_maxrss;
    s64     ru_ixrss;
    s64     ru_idrss;
    s64     ru_isrss;
    s64     ru_minflt;
    s64     ru_majflt;
    s64     ru_nswap;
    s64     ru_inblock;
    s64     ru_oublock;
    s64     ru_msgsnd;
    s64     ru_msgrcv;
    s64     ru_nsignals;
    s64     ru_nvcsw;
    s64     ru_nivcsw;
};
#endif

#ifndef _STRUCT_RLIMIT
#define _STRUCT_RLIMIT
struct rlimit {
    u64     rlim_cur;
    u64     rlim_max;
};
#endif

#ifndef _STRUCT_SYSINFO
#define _STRUCT_SYSINFO
struct sysinfo {
    s64     uptime;
    u64     loads[3];
    u64     totalram;
    u64     freeram;
    u64     sharedram;
    u64     bufferram;
    u64     totalswap;
    u64     freeswap;
    u16     procs;
    u16     pad;
    u64     totalhigh;
    u64     freehigh;
    u32     mem_unit;
    char    _f[20-2*sizeof(u64)-sizeof(u32)];
};
#endif

#ifndef _STRUCT_IOVEC
#define _STRUCT_IOVEC
struct iovec {
    void    *iov_base;
    size_t  iov_len;
};
#endif

struct sigaction {
    void    (*sa_handler)(int);
    u64     sa_flags;
    void    (*sa_restorer)(void);
    sigset_t sa_mask;
};

// Dirent for getdents64
#ifndef _STRUCT_LINUX_DIRENT64
#define _STRUCT_LINUX_DIRENT64
struct linux_dirent64 {
    u64     d_ino;
    s64     d_off;
    u16     d_reclen;
    u8      d_type;
    char    d_name[];
};
#endif

#ifndef DT_UNKNOWN
#define DT_UNKNOWN      0
#define DT_FIFO         1
#define DT_CHR          2
#define DT_DIR          4
#endif
#define DT_BLK          6
#define DT_REG          8
#define DT_LNK          10
#define DT_SOCK         12
#define DT_WHT          14

// ============================================================================
// SYSCALL TABLE ENTRY
// ============================================================================

typedef s64 (*syscall_fn_t)(u64, u64, u64, u64, u64, u64);

extern syscall_fn_t syscall_table[NR_syscalls];

// Register frame for syscalls
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error_code;
    u64 rip, cs, rflags, rsp, ss;
} __packed syscall_regs_t;

// Syscall handler prototypes
void syscall_init_table(void);
s64 sys_ni_syscall(void);  // Not implemented

// Core syscalls
s64 sys_read(int fd, void *buf, size_t count);
s64 sys_write(int fd, const void *buf, size_t count);
s64 sys_open(const char *filename, int flags, u32 mode);
s64 sys_close(int fd);
s64 sys_stat(const char *filename, struct linux_stat *statbuf);
s64 sys_fstat(int fd, struct linux_stat *statbuf);
s64 sys_lseek(int fd, s64 offset, int whence);
s64 sys_mmap(void *addr, size_t len, int prot, int flags, int fd, s64 off);
s64 sys_mprotect(void *addr, size_t len, int prot);
s64 sys_munmap(void *addr, size_t len);
s64 sys_brk(void *addr);
s64 sys_ioctl(int fd, u64 cmd, u64 arg);
s64 sys_access(const char *filename, int mode);
s64 sys_pipe(int *pipefd);
s64 sys_pipe2(int *pipefd, int flags);
s64 sys_dup(int oldfd);
s64 sys_dup2(int oldfd, int newfd);
s64 sys_dup3(int oldfd, int newfd, int flags);
s64 sys_fcntl(int fd, int cmd, u64 arg);
s64 sys_getpid(void);
s64 sys_getppid(void);
s64 sys_gettid(void);
s64 sys_getuid(void);
s64 sys_geteuid(void);
s64 sys_getgid(void);
s64 sys_getegid(void);
s64 sys_fork(void);
s64 sys_vfork(void);
s64 sys_clone(u64 flags, void *stack, int *parent_tid, int *child_tid, u64 tls);
s64 sys_execve(const char *filename, char *const argv[], char *const envp[]);
s64 sys_exit(int status);
s64 sys_exit_group(int status);
s64 sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
s64 sys_waitid(int idtype, pid_t id, void *infop, int options, struct rusage *ru);
s64 sys_kill(pid_t pid, int sig);
s64 sys_tkill(pid_t tid, int sig);
s64 sys_tgkill(pid_t tgid, pid_t tid, int sig);
s64 sys_rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact, size_t sigsetsize);
s64 sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize);
s64 sys_rt_sigreturn(void);
s64 sys_pause(void);
s64 sys_nanosleep(const struct timespec *req, struct timespec *rem);
s64 sys_alarm(unsigned int seconds);
s64 sys_getcwd(char *buf, size_t size);
s64 sys_chdir(const char *path);
s64 sys_fchdir(int fd);
s64 sys_mkdir(const char *pathname, u32 mode);
s64 sys_mkdirat(int dirfd, const char *pathname, u32 mode);
s64 sys_rmdir(const char *pathname);
s64 sys_unlink(const char *pathname);
s64 sys_unlinkat(int dirfd, const char *pathname, int flags);
s64 sys_rename(const char *oldpath, const char *newpath);
s64 sys_link(const char *oldpath, const char *newpath);
s64 sys_symlink(const char *target, const char *linkpath);
s64 sys_readlink(const char *pathname, char *buf, size_t bufsiz);
s64 sys_chmod(const char *pathname, u32 mode);
s64 sys_fchmod(int fd, u32 mode);
s64 sys_chown(const char *pathname, u32 owner, u32 group);
s64 sys_fchown(int fd, u32 owner, u32 group);
s64 sys_umask(u32 mask);
s64 sys_uname(struct utsname *buf);
s64 sys_gettimeofday(struct timeval *tv, struct timezone *tz);
s64 sys_clock_gettime(int clk_id, struct timespec *tp);
s64 sys_clock_getres(int clk_id, struct timespec *res);
s64 sys_getdents64(int fd, struct linux_dirent64 *dirp, unsigned int count);
s64 sys_sched_yield(void);
s64 sys_getrlimit(int resource, struct rlimit *rlim);
s64 sys_setrlimit(int resource, const struct rlimit *rlim);
s64 sys_sysinfo(struct sysinfo *info);
s64 sys_sync(void);
s64 sys_fsync(int fd);
s64 sys_mount(const char *source, const char *target, const char *fstype, u64 flags, const void *data);
s64 sys_umount2(const char *target, int flags);
s64 sys_reboot(int magic, int magic2, int cmd, void *arg);
s64 sys_getrandom(void *buf, size_t buflen, unsigned int flags);
s64 sys_prctl(int option, u64 arg2, u64 arg3, u64 arg4, u64 arg5);
s64 sys_arch_prctl(int code, u64 addr);
s64 sys_set_tid_address(int *tidptr);

#endif // _KERNEL_SYSCALL_H

s64 sys_set_robust_list(void *head, size_t len);
s64 sys_rseq_stub(u32 *rseq, u32 rseq_len, int flags, u32 sig);
