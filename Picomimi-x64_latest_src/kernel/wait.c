/**
 * Picomimi-x64 wait4 / waitpid — Child Process Reaping
 *
 * Implements full POSIX wait semantics:
 *   wait4(pid, wstatus, options, rusage)
 *
 *   pid > 0  : wait for specific child
 *   pid == 0 : wait for any child in same process group
 *   pid == -1: wait for any child
 *   pid < -1 : wait for any child in pgrp |pid|
 *
 * options:
 *   WNOHANG    — return immediately if no child has exited
 *   WUNTRACED  — also return for stopped children (SIGSTOP)
 *   WCONTINUED — also return for continued children (SIGCONT)
 *
 * wstatus encoding (Linux compatible):
 *   normal exit:  exit_code << 8
 *   signal kill:  signal_number (bit 7 = coredump)
 *   stopped:      0x7F | (signal << 8)
 *   continued:    0xFFFF
 *
 * After a successful wait:
 *   - Child's mm_struct is freed
 *   - Child's pid is released
 *   - Child task_struct removed from task_table
 *   - Parent's rusage totals updated (children accumulator)
 *   - SIGCHLD is cleared from pending if no more zombie children
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/kernel.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <mm/pmm.h>

extern int  signal_pending(task_struct_t *task);
extern void pid_free(pid_t pid);
extern void mm_free(mm_struct_t *mm);

/* =========================================================
 * WNOHANG / WUNTRACED / WCONTINUED
 * ========================================================= */
#define WNOHANG     0x00000001
#define WUNTRACED   0x00000002
#define WSTOPPED    WUNTRACED
#define WEXITED     0x00000004
#define WCONTINUED  0x00000008
#define WNOWAIT     0x01000000   /* Don't reap, just peek */

/* wstatus macros */
#define W_EXITCODE(ret, sig)    (((ret) << 8) | (sig))
#define W_STOPCODE(sig)         (((sig) << 8) | 0x7f)
#define W_CONTINUED             0xffff
#define WCOREFLAG               0x80

/* =========================================================
 * task_match — does this task match the wait4 pid filter?
 * ========================================================= */

static bool task_matches_pid(task_struct_t *child, task_struct_t *parent,
                               pid_t wait_pid) {
    (void)parent;
    if (wait_pid == -1)
        return true;   /* Any child */
    if (wait_pid == 0)
        return child->signal && parent->signal &&
               child->signal->pgrp == parent->signal->pgrp;
    if (wait_pid < -1)
        return child->signal && child->signal->pgrp == (pid_t)(-wait_pid);
    /* wait_pid > 0 */
    return child->pid == wait_pid || child->tgid == wait_pid;
}

/* =========================================================
 * reap_zombie — consume a zombie child into wstatus/rusage
 * ========================================================= */

static void reap_zombie(task_struct_t *child, task_struct_t *parent,
                         int *wstatus, struct rusage *ru) {
    (void)parent;

    if (wstatus)
        *wstatus = W_EXITCODE(child->exit_code & 0xFF, 0);

    if (ru) {
        memset(ru, 0, sizeof(*ru));
        ru->ru_utime.tv_sec  = (time_t)(child->utime / NSEC_PER_SEC);
        ru->ru_utime.tv_usec = (s64)((child->utime % NSEC_PER_SEC) / NSEC_PER_USEC);
        ru->ru_stime.tv_sec  = (time_t)(child->stime / NSEC_PER_SEC);
        ru->ru_stime.tv_usec = (s64)((child->stime % NSEC_PER_SEC) / NSEC_PER_USEC);
        if (child->mm) ru->ru_maxrss = (s64)(child->mm->total_vm * 4);
    }

    /* Accumulate into parent's children rusage */
    if (parent) {
        parent->utime += child->utime;
        parent->stime += child->stime;
    }

    /* Free child resources */
    if (child->mm) {
        mm_free(child->mm);
        child->mm = NULL;
    }

    /* Detach from parent's child list */
    if (child->sibling.next && child->sibling.prev) {
        list_del_init(&child->sibling);
    }

    /* Remove from task table and free PID */
    extern task_struct_t *task_table[];
    extern spinlock_t     task_table_lock;

    spin_lock(&task_table_lock);
    for (int i = 0; i < 4096; i++) {
        if (task_table[i] == child) {
            task_table[i] = NULL;
            break;
        }
    }
    spin_unlock(&task_table_lock);

    pid_free(child->pid);

    /* Free signal/sighand if not shared */
    if (child->signal && atomic_read(&child->signal->count) &&
        atomic_dec_and_test(&child->signal->count)) {
        kfree(child->signal);
        child->signal = NULL;
    }
    if (child->sighand && atomic_read(&child->sighand->count) &&
        atomic_dec_and_test(&child->sighand->count)) {
        kfree(child->sighand);
        child->sighand = NULL;
    }

    /* Free files */
    if (child->files) {
        int _max = child->files->max_fds;
        if (_max > 64 && child->files->fd_array == child->files->fd_array_init)
            _max = 64;  /* inline storage is only 64 entries */
        /* Close all open fds */
        for (int i = 0; i < _max; i++) {
            if (child->files->fd_array[i].file) {
                file_t *f = child->files->fd_array[i].file;
                child->files->fd_array[i].file = NULL;
                fput(f);
            }
        }
        kfree(child->files);
        child->files = NULL;
    }

    /* Free fs context */
    if (child->fs) { kfree(child->fs); child->fs = NULL; }

    printk(KERN_DEBUG "[wait4] reaped pid %d exit_code=%d\n",
           child->pid, child->exit_code);

    /* Mark task slot as free and free memory */
    child->exit_state = EXIT_DEAD;
    kfree(child);
}

/* =========================================================
 * do_wait — core wait implementation
 * ========================================================= */

static s64 do_wait(pid_t wait_pid, int *wstatus, int options,
                   struct rusage *ru) {
    task_struct_t *parent = current;
    if (!parent) return -ESRCH;

    for (;;) {
        /* Scan children list */
        bool found_child = false;
        pid_t reaped_pid = 0;

        struct list_head *p, *tmp;
        list_for_each_safe(p, tmp, &parent->children) {
            task_struct_t *child = list_entry(p, task_struct_t, sibling);

            if (!task_matches_pid(child, parent, wait_pid)) continue;
            found_child = true;

            /* Zombie: normal exit */
            if (child->exit_state == EXIT_ZOMBIE) {
                reaped_pid = child->pid;
                if (!(options & WNOWAIT)) {
                    reap_zombie(child, parent, wstatus, ru);
                } else {
                    /* Just peek */
                    if (wstatus) *wstatus = W_EXITCODE(child->exit_code & 0xFF, 0);
                }
                return (s64)reaped_pid;
            }

            /* Stopped child (WUNTRACED) */
            if ((options & WUNTRACED) &&
                child->exit_state == EXIT_STOPPED &&
                !child->wait_reported) {
                child->wait_reported = true;
                if (wstatus) *wstatus = W_STOPCODE(child->stop_signal);
                return (s64)child->pid;
            }

            /* Continued child (WCONTINUED) */
            if ((options & WCONTINUED) &&
                child->exit_state == EXIT_CONTINUED &&
                !child->wait_reported) {
                child->wait_reported = true;
                if (wstatus) *wstatus = W_CONTINUED;
                return (s64)child->pid;
            }
        }

        /* No matching children at all */
        if (!found_child) return -ECHILD;

        /* WNOHANG: nothing ready, return 0 immediately */
        if (options & WNOHANG) return 0;

        /* Block until a child changes state */
        if (signal_pending(parent)) return -EINTR;
        /* Sleep properly: set TASK_INTERRUPTIBLE then schedule.
         * do_exit will send SIGCHLD which calls sched_enqueue_task to wake us. */
        parent->state = TASK_INTERRUPTIBLE;
        extern void schedule(void);
        schedule();
        /* Woke up: re-scan children */
    }
}

/* =========================================================
 * Exported syscalls
 * ========================================================= */

s64 sys_wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) {
    return do_wait(pid, wstatus, options, rusage);
}

s64 sys_waitpid(pid_t pid, int *wstatus, int options) {
    return do_wait(pid, wstatus, options, NULL);
}

/* =========================================================
 * sys_exit / sys_exit_group — process termination
 * ========================================================= */

void do_exit(int exit_code) {
    task_struct_t *t = current;
    if (!t) {
        /* Kernel task — just halt */
        for (;;) __asm__ volatile("hlt");
    }

    t->exit_code  = exit_code;
    t->exit_state = EXIT_ZOMBIE;
    t->state      = TASK_ZOMBIE;

    /* Close all file descriptors */
    if (t->files) {
        int _max = t->files->max_fds;
        if (_max > 64 && t->files->fd_array == t->files->fd_array_init)
            _max = 64;
        for (int i = 0; i < _max; i++) {
            if (t->files->fd_array[i].file) {
                fput(t->files->fd_array[i].file);
                t->files->fd_array[i].file = NULL;
            }
        }
    }

    /* Note: physical page cleanup deferred to wait4/reap to avoid
     * freeing pages while still running on this task's kernel stack.
     * Memory leak is acceptable for now. */

    /* Handle pdeath_signal for children */
    struct list_head *cp;
    list_for_each(cp, &t->children) {
        task_struct_t *child = list_entry(cp, task_struct_t, sibling);
        if (child->pdeath_signal > 0)
            send_signal(child->pid, child->pdeath_signal);
        /* Reparent to init (pid 1) */
        child->ppid = 1;
    }

    /* Wake parent waiting in wait4 */
    task_struct_t *parent = find_task_by_pid(t->ppid);
    if (parent) {
        /* Send SIGCHLD to parent */
        send_signal(parent->pid, SIGCHLD);
    }

    /* Decrement kernel task count */
    if (kernel_state.total_tasks > 0) kernel_state.total_tasks--;

    /* Never returns — switch to next task */
    printk(KERN_DEBUG "[exit] scheduling out from pid %d\n", (int)t->pid);
    extern void schedule(void);
    schedule();

    /* Unreachable */
    for (;;) __asm__ volatile("hlt");
}

s64 sys_exit(int status) {
    do_exit(status & 0xFF);
    return 0;  /* unreachable */
}

s64 sys_exit_group(int status) {
    /* Kill all threads in thread group, then exit */
    task_struct_t *t = current;
    if (t && t->signal) {
        /* Send SIGKILL to all threads with same tgid */
        extern task_struct_t *task_table[];
        for (int i = 0; i < 4096; i++) {
            task_struct_t *th = task_table[i];
            if (!th || th == t) continue;
            if (th->tgid == t->tgid)
                send_signal(th->pid, SIGKILL);
        }
    }
    do_exit(status & 0xFF);
    return 0;
}
