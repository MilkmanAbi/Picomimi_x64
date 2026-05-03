/**
 * Picomimi-x64 Signal Header
 */
#ifndef _KERNEL_SIGNAL_H
#define _KERNEL_SIGNAL_H

#include <kernel/types.h>

/* SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* First real-time signal */
#ifndef SIGRTMIN
#define SIGRTMIN    32
#define SIGRTMAX    NSIG
#endif

/* si_code values */
#define SI_USER     0       /* Sent by kill() */
#define SI_KERNEL   0x80    /* Sent by kernel */
#define SI_QUEUE    -1      /* Sent by sigqueue() */
#define SI_TIMER    -2      /* POSIX timer expired */
#define SI_MESGQ    -3      /* POSIX message queue */
#define SI_ASYNCIO  -4      /* AIO completed */
#define SI_SIGIO    -5      /* Queued SIGIO */
#define SI_TKILL    -6      /* Sent by tkill/tgkill */

/* Signal info structure */
typedef struct siginfo {
    int     si_signo;   /* Signal number */
    int     si_errno;   /* Error value */
    int     si_code;    /* Signal code */
    union {
        /* SIGCHLD */
        struct {
            pid_t   si_pid;
            uid_t   si_uid;
            int     si_status;
            clock_t si_utime;
            clock_t si_stime;
        } _sigchld;
        /* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
        struct {
            void *si_addr;
            short si_addr_lsb;
        } _sigfault;
        /* SIGPOLL */
        struct {
            s64     si_band;
            int     si_fd;
        } _sigpoll;
        /* Generic */
        struct {
            pid_t   si_pid;
            uid_t   si_uid;
        } _kill;
        /* For padding */
        u8 _pad[128 - 3 * sizeof(int)];
    };
} __packed siginfo_t;

/* Convenience accessors */
#define si_pid      _kill.si_pid
#define si_uid      _kill.si_uid
#define si_addr     _sigfault.si_addr
#define si_status   _sigchld.si_status
#define si_band     _sigpoll.si_band
#define si_fd       _sigpoll.si_fd

/* Extended sigpending with queue */
typedef struct sigpending_full {
    sigset_t            signal;
    struct list_head    queue;
} sigpending_full_t;

/* Re-define sigpending_t in process.h to include queue */
/* (We use sigpending_t from process.h as-is, extended here) */

/* Public API */
void sigemptyset(sigset_t *set);
void sigfillset(sigset_t *set);
void sigaddset(sigset_t *set, int sig);
void sigdelset(sigset_t *set, int sig);
int  sigismember(const sigset_t *set, int sig);
int  sigisemptyset(const sigset_t *set);
void sigorset(sigset_t *d, const sigset_t *a, const sigset_t *b);
void sigandset(sigset_t *d, const sigset_t *a, const sigset_t *b);
void signandset(sigset_t *d, const sigset_t *a, const sigset_t *b);

int  send_signal(pid_t pid, int sig);
int  send_signal_info(pid_t pid, int sig, void *info);
int  signal_pending(task_struct_t *task);
void recalc_sigpending(void);
void do_signal(trap_frame_t *frame);

struct sighand_struct;
struct signal_struct;
struct task_struct;

struct sighand_struct *sighand_alloc(void);
struct sighand_struct *sighand_copy(const struct sighand_struct *old);
void sighand_put(struct sighand_struct *sh);

struct signal_struct *signal_alloc(void);

void signal_init_task(task_struct_t *t, bool copy_parent);
void signal_cleanup_task(task_struct_t *t);

/* Syscalls */
s64 sys_rt_sigaction(int sig, const struct sigaction *act,
                     struct sigaction *oact, size_t sigsetsize);
s64 sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset,
                        size_t sigsetsize);
s64 sys_rt_sigreturn(void);
s64 sys_rt_sigpending(sigset_t *set, size_t sigsetsize);
s64 sys_kill(pid_t pid, int sig);
s64 sys_tkill(pid_t tid, int sig);
s64 sys_tgkill(pid_t tgid, pid_t tid, int sig);
s64 sys_pause(void);
s64 sys_sigaltstack(const void *ss, void *oss);

#endif /* _KERNEL_SIGNAL_H */
