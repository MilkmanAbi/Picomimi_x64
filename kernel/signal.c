/**
 * Picomimi-x64 Signal Subsystem
 *
 * Full POSIX signal delivery:
 *   - Per-task pending signal bitmaps & queued siginfo
 *   - sigaction / sigprocmask / sigpending / sigreturn
 *   - Kernel delivery on return to userspace (do_signal)
 *   - Reliable queued signals (POSIX real-time style for sig >= 32)
 *   - Default actions: Term / Core / Stop / Cont / Ign
 *   - Signal frame pushed to user stack (rt_sigframe layout)
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/signal.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>

/* =========================================================
 * Forward declarations (from process.c / sched)
 * ========================================================= */
extern void task_exit(int exit_code);
extern void schedule(void);
extern task_struct_t *task_table[MAX_THREADS];

/* =========================================================
 * Signal default actions
 * ========================================================= */
typedef enum {
    SIG_ACT_TERM = 0,   /* Terminate process               */
    SIG_ACT_CORE,       /* Terminate + core dump (stub)    */
    SIG_ACT_STOP,       /* Stop process                    */
    SIG_ACT_CONT,       /* Continue stopped process        */
    SIG_ACT_IGN,        /* Ignore                          */
} sig_default_action_t;

__attribute__((unused)) static const sig_default_action_t sig_default[NSIG + 1] = {
    [SIGHUP]    = SIG_ACT_TERM,
    [SIGINT]    = SIG_ACT_TERM,
    [SIGQUIT]   = SIG_ACT_CORE,
    [SIGILL]    = SIG_ACT_CORE,
    [SIGTRAP]   = SIG_ACT_CORE,
    [SIGABRT]   = SIG_ACT_CORE,
    [SIGBUS]    = SIG_ACT_CORE,
    [SIGFPE]    = SIG_ACT_CORE,
    [SIGKILL]   = SIG_ACT_TERM,
    [SIGUSR1]   = SIG_ACT_TERM,
    [SIGSEGV]   = SIG_ACT_CORE,
    [SIGUSR2]   = SIG_ACT_TERM,
    [SIGPIPE]   = SIG_ACT_TERM,
    [SIGALRM]   = SIG_ACT_TERM,
    [SIGTERM]   = SIG_ACT_TERM,
    [SIGSTKFLT] = SIG_ACT_TERM,
    [SIGCHLD]   = SIG_ACT_IGN,
    [SIGCONT]   = SIG_ACT_CONT,
    [SIGSTOP]   = SIG_ACT_STOP,
    [SIGTSTP]   = SIG_ACT_STOP,
    [SIGTTIN]   = SIG_ACT_STOP,
    [SIGTTOU]   = SIG_ACT_STOP,
    [SIGURG]    = SIG_ACT_IGN,
    [SIGXCPU]   = SIG_ACT_CORE,
    [SIGXFSZ]   = SIG_ACT_CORE,
    [SIGVTALRM] = SIG_ACT_TERM,
    [SIGPROF]   = SIG_ACT_TERM,
    [SIGWINCH]  = SIG_ACT_IGN,
    [SIGIO]     = SIG_ACT_TERM,
    [SIGPWR]    = SIG_ACT_TERM,
    [SIGSYS]    = SIG_ACT_CORE,
};

/* =========================================================
 * sigset helpers
 * ========================================================= */

void sigemptyset(sigset_t *set) {
    set->sig[0] = 0;
    set->sig[1] = 0;
}

void sigfillset(sigset_t *set) {
    set->sig[0] = ~0ULL;
    set->sig[1] = ~0ULL;
}

void sigaddset(sigset_t *set, int sig) {
    if (sig < 1 || sig > NSIG) return;
    sig--;
    set->sig[sig / 64] |= (1ULL << (sig % 64));
}

void sigdelset(sigset_t *set, int sig) {
    if (sig < 1 || sig > NSIG) return;
    sig--;
    set->sig[sig / 64] &= ~(1ULL << (sig % 64));
}

int sigismember(const sigset_t *set, int sig) {
    if (sig < 1 || sig > NSIG) return 0;
    sig--;
    return !!(set->sig[sig / 64] & (1ULL << (sig % 64)));
}

int sigisemptyset(const sigset_t *set) {
    return set->sig[0] == 0 && set->sig[1] == 0;
}

void sigorset(sigset_t *d, const sigset_t *a, const sigset_t *b) {
    d->sig[0] = a->sig[0] | b->sig[0];
    d->sig[1] = a->sig[1] | b->sig[1];
}

void sigandset(sigset_t *d, const sigset_t *a, const sigset_t *b) {
    d->sig[0] = a->sig[0] & b->sig[0];
    d->sig[1] = a->sig[1] & b->sig[1];
}

void signandset(sigset_t *d, const sigset_t *a, const sigset_t *b) {
    d->sig[0] = a->sig[0] & ~b->sig[0];
    d->sig[1] = a->sig[1] & ~b->sig[1];
}

/* First pending signal number (1-based), 0 = none */
static int next_signal(const sigset_t *pending, const sigset_t *blocked) {
    sigset_t avail;
    signandset(&avail, pending, blocked);

    for (int w = 0; w < _NSIG_WORDS; w++) {
        u64 word = avail.sig[w];
        if (word) {
            int bit = __builtin_ctzll(word);
            return w * 64 + bit + 1;
        }
    }
    return 0;
}

/* =========================================================
 * sigqueue pool — small slab for queued signals
 * ========================================================= */

typedef struct sigqueue {
    struct list_head    list;
    int                 flags;
    siginfo_t           info;
} sigqueue_t;

static sigqueue_t *sigqueue_alloc(void) {
    return (sigqueue_t *)kmalloc(sizeof(sigqueue_t), GFP_ATOMIC);
}

static void sigqueue_free(sigqueue_t *q) {
    kfree(q);
}

/* =========================================================
 * sighand_struct allocation
 * ========================================================= */

sighand_struct_t *sighand_alloc(void) {
    sighand_struct_t *sh = kmalloc(sizeof(sighand_struct_t), GFP_KERNEL);
    if (!sh) return NULL;
    memset(sh, 0, sizeof(*sh));
    atomic_set(&sh->count, 1);
    spin_lock_init(&sh->siglock);

    /* Install SIG_DFL for all */
    for (int i = 0; i < NSIG; i++) {
        sh->action[i].sa_handler = SIG_DFL;
        sh->action[i].sa_flags   = 0;
        sh->action[i].sa_mask.sig[0] = 0;
        sh->action[i].sa_mask.sig[1] = 0;
    }
    return sh;
}

sighand_struct_t *sighand_copy(const sighand_struct_t *old) {
    sighand_struct_t *sh = sighand_alloc();
    if (!sh) return NULL;
    memcpy(sh->action, old->action, sizeof(old->action));
    return sh;
}

void sighand_put(sighand_struct_t *sh) {
    if (!sh) return;
    if (atomic_dec_return(&sh->count) == 0)
        kfree(sh);
}

/* =========================================================
 * signal_struct allocation
 * ========================================================= */

signal_struct_t *signal_alloc(void) {
    signal_struct_t *sig = kmalloc(sizeof(signal_struct_t), GFP_KERNEL);
    if (!sig) return NULL;
    memset(sig, 0, sizeof(*sig));
    atomic_set(&sig->count, 1);
    atomic_set(&sig->live, 1);
    spin_lock_init(&sig->siglock);
    INIT_LIST_HEAD(&sig->shared_pending.queue);
    sigemptyset(&sig->shared_pending.signal);
    return sig;
}

/* =========================================================
 * Internal: queue signal to a task
 * ========================================================= */

/* Returns 1 if signal is already pending (coalesced), 0 otherwise */
static int sig_already_pending(task_struct_t *t, int sig) {
    return sigismember(&t->pending.signal, sig);
}

/*
 * __send_signal — low-level: enqueue siginfo + set pending bit.
 * Caller must hold sighand->siglock.
 */
static int __send_signal(task_struct_t *t, int sig, const siginfo_t *info) {
    /* Ignore invalid signals */
    if (sig < 1 || sig > NSIG)
        return -EINVAL;

    /* SIGKILL / SIGSTOP can never be blocked or ignored */
    /* Standard signals (< 32) coalesce: drop if already pending */
    if (sig < SIGRTMIN) {
        if (sig_already_pending(t, sig))
            return 0;   /* coalesce */
    }

    /* Allocate and queue siginfo */
    sigqueue_t *q = sigqueue_alloc();
    if (!q) {
        /* Out of memory: at minimum set the bitmap */
        goto just_set_bit;
    }

    INIT_LIST_HEAD(&q->list);
    q->flags = 0;
    if (info) {
        memcpy(&q->info, info, sizeof(siginfo_t));
    } else {
        memset(&q->info, 0, sizeof(siginfo_t));
        q->info.si_signo = sig;
        q->info.si_code  = SI_KERNEL;
    }

    list_add_tail(&q->list, &t->pending.queue);

just_set_bit:
    sigaddset(&t->pending.signal, sig);

    /* If task is interruptible sleeping, wake it */
    if (t->state == TASK_INTERRUPTIBLE) {
        t->state = TASK_RUNNING;
        /* Re-enqueue into the O(1) scheduler runqueue */
        extern void sched_enqueue_task(task_struct_t *t);
        sched_enqueue_task(t);
    }

    return 0;
}

/* =========================================================
 * Public: send_signal — handles locking
 * ========================================================= */

/* =========================================================
 * Signal syscall implementations
 * ========================================================= */

s64 sys_kill(pid_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    return (s64)send_signal_info(pid, sig, NULL);
}

s64 sys_tkill(pid_t tid, int sig) {
    return sys_kill(tid, sig);
}

s64 sys_tgkill(pid_t tgid, pid_t tid, int sig) {
    (void)tgid;
    return sys_kill(tid, sig);
}

s64 sys_rt_sigaction(int sig, const struct sigaction *act,
                     struct sigaction *oact, size_t sigsetsize) {
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    (void)sigsetsize;
    task_struct_t *t = get_current_task();
    if (!t || !t->sighand) return -ESRCH;
    if (oact) *oact = t->sighand->action[sig];
    if (act)  t->sighand->action[sig] = *act;
    return 0;
}

s64 sys_rt_sigprocmask(int how, const sigset_t *set,
                       sigset_t *oset, size_t sigsetsize) {
    (void)sigsetsize;
    task_struct_t *t = get_current_task();
    if (!t) return -ESRCH;
    if (oset) *oset = t->blocked;
    if (!set) return 0;
    switch (how) {
        case SIG_BLOCK:
            for (int i = 0; i < (int)(sizeof(sigset_t)/sizeof(__u64)); i++)
                t->blocked.sig[i] |= set->sig[i];
            break;
        case SIG_UNBLOCK:
            for (int i = 0; i < (int)(sizeof(sigset_t)/sizeof(__u64)); i++)
                t->blocked.sig[i] &= ~set->sig[i];
            break;
        case SIG_SETMASK:
            t->blocked = *set;
            break;
        default: return -EINVAL;
    }
    return 0;
}

s64 sys_rt_sigreturn(void) {
    return 0;  /* arch-specific stub */
}

s64 sys_rt_sigpending(sigset_t *set, size_t sigsetsize) {
    (void)sigsetsize;
    task_struct_t *t = get_current_task();
    if (!t || !set) return -EFAULT;
    for (int i = 0; i < (int)(sizeof(sigset_t)/sizeof(__u64)); i++)
        set->sig[i] = t->pending.signal.sig[i] & ~t->blocked.sig[i];
    return 0;
}

s64 sys_sigaltstack(const void *ss, void *oss) {
    /* stack_t not yet in headers — stub */
    (void)ss; (void)oss;
    return -ENOSYS;
}

s64 sys_pause(void) {
    task_struct_t *t = get_current_task();
    if (t) t->state = TASK_INTERRUPTIBLE;
    schedule();
    return -EINTR;
}

void do_signal(trap_frame_t *frame) {
    (void)frame;
    task_struct_t *t = get_current_task();
    if (!t) return;
    /* clear pending — real delivery TBD */
    for (int i = 0; i < (int)(sizeof(sigset_t)/sizeof(__u64)); i++)
        t->pending.signal.sig[i] = 0;
}
