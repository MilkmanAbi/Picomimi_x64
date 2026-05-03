/**
 * Picomimi-x64 Pipe and FIFO Implementation
 *
 * Anonymous pipes (pipe(2) / pipe2(2)):
 *   - Kernel ring buffer (PIPE_BUF = 4096 bytes, atomic writes guaranteed)
 *   - Blocking read when empty (waits for writer)
 *   - Blocking write when full (waits for reader)
 *   - EOF on read when all write ends closed
 *   - SIGPIPE / EPIPE on write when all read ends closed
 *   - O_NONBLOCK support
 *   - F_GETPIPE_SZ / F_SETPIPE_SZ fcntl
 *   - Splicing-ready internal buffer structure
 *
 * Named pipes / FIFOs (mkfifo):
 *   - Backed by same pipe_inode, created in VFS via S_IFIFO mknod
 *   - Open blocks until both ends opened (unless O_RDONLY|O_NONBLOCK)
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

extern int  signal_pending(task_struct_t *task);
extern int  send_signal(pid_t pid, int sig);

/* =========================================================
 * Pipe buffer
 * ========================================================= */

#define PIPE_BUF_SIZE       65536   /* Default pipe capacity (64 KB) */
#define PIPE_MIN_SIZE       PAGE_SIZE
#define PIPE_MAX_SIZE       (1 << 20)  /* 1 MB max */

typedef struct pipe_buffer {
    u8      *data;          /* Ring buffer data */
    size_t   size;          /* Buffer capacity */
    size_t   head;          /* Write position */
    size_t   tail;          /* Read position */
    size_t   count;         /* Bytes in buffer */
} pipe_buffer_t;

static inline size_t pbuf_space(const pipe_buffer_t *pb) {
    return pb->size - pb->count;
}

static inline size_t pbuf_count(const pipe_buffer_t *pb) {
    return pb->count;
}

static inline size_t pbuf_write(pipe_buffer_t *pb, const u8 *src, size_t n) {
    size_t written = 0;
    while (written < n && pb->count < pb->size) {
        pb->data[pb->head] = src[written];
        pb->head = (pb->head + 1) & (pb->size - 1);
        pb->count++;
        written++;
    }
    return written;
}

static inline size_t pbuf_read(pipe_buffer_t *pb, u8 *dst, size_t n) {
    size_t rd = 0;
    while (rd < n && pb->count > 0) {
        dst[rd] = pb->data[pb->tail];
        pb->tail = (pb->tail + 1) & (pb->size - 1);
        pb->count--;
        rd++;
    }
    return rd;
}

/* =========================================================
 * Pipe inode (shared between read and write ends)
 * ========================================================= */

typedef struct pipe_inode {
    pipe_buffer_t   buf;

    /* Reference counts for read/write ends */
    atomic_t        readers;    /* Number of open read fds */
    atomic_t        writers;    /* Number of open write fds */

    /* Locking and wait queues */
    spinlock_t      lock;
    wait_queue_head_t read_wait;   /* Waiters blocked on read */
    wait_queue_head_t write_wait;  /* Waiters blocked on write */

    /* Pipe flags */
    bool            is_fifo;    /* Named FIFO vs anonymous pipe */
    bool            can_read;   /* A reader is waiting (FIFO open) */
    bool            can_write;  /* A writer is waiting (FIFO open) */
} pipe_inode_t;

/* =========================================================
 * Pipe allocation
 * ========================================================= */

static pipe_inode_t *pipe_alloc(size_t buf_size) {
    pipe_inode_t *pi = kzalloc(sizeof(pipe_inode_t), GFP_KERNEL);
    if (!pi) return NULL;

    /* Round up to power of two for efficient masking */
    size_t sz = PAGE_SIZE;
    while (sz < buf_size) sz <<= 1;
    if (sz > PIPE_MAX_SIZE) sz = PIPE_MAX_SIZE;

    pi->buf.data = kmalloc(sz, GFP_KERNEL);
    if (!pi->buf.data) { kfree(pi); return NULL; }

    pi->buf.size  = sz;
    pi->buf.head  = 0;
    pi->buf.tail  = 0;
    pi->buf.count = 0;

    atomic_set(&pi->readers, 0);
    atomic_set(&pi->writers, 0);
    spin_lock_init(&pi->lock);
    /* wait queues — simple linked-list heads */
    INIT_LIST_HEAD(&pi->read_wait.head);
    spin_lock_init(&pi->read_wait.lock);
    INIT_LIST_HEAD(&pi->write_wait.head);
    spin_lock_init(&pi->write_wait.lock);

    return pi;
}

static void pipe_free(pipe_inode_t *pi) {
    if (!pi) return;
    if (pi->buf.data) kfree(pi->buf.data);
    kfree(pi);
}

/* =========================================================
 * Wake helpers
 * ========================================================= */

extern void sched_wake_up(task_struct_t *task);

/* Walk a wait_queue_head and wake every task sleeping on it.
 * private == task_struct_t* per the kernel wait_queue_entry_t convention. */
static void pipe_wake_queue(wait_queue_head_t *wq) {
    spin_lock(&wq->lock);
    struct list_head *p, *tmp;
    list_for_each_safe(p, tmp, &wq->head) {
        wait_queue_entry_t *e = list_entry(p, wait_queue_entry_t, entry);
        task_struct_t *t = (task_struct_t *)e->private;
        if (t) sched_wake_up(t);
    }
    spin_unlock(&wq->lock);
}

static void pipe_wake_readers(pipe_inode_t *pi) {
    pipe_wake_queue(&pi->read_wait);
}

static void pipe_wake_writers(pipe_inode_t *pi) {
    pipe_wake_queue(&pi->write_wait);
}

/* Add current task to a wait queue and sleep until woken.
 * Returns -EINTR if a signal arrived. */
static int pipe_wait(pipe_inode_t *pi, wait_queue_head_t *wq, spinlock_t *pipe_lock) {
    task_struct_t *me = current;
    if (!me) return -EINVAL;

    wait_queue_entry_t entry;
    entry.flags   = 0;
    entry.private = me;
    entry.func    = NULL;
    INIT_LIST_HEAD(&entry.entry);

    /* Enqueue on the wait list */
    spin_lock(&wq->lock);
    list_add_tail(&entry.entry, &wq->head);
    spin_unlock(&wq->lock);

    /* Drop the pipe lock, set interruptible, schedule */
    spin_unlock(pipe_lock);
    me->state = TASK_INTERRUPTIBLE;
    extern void schedule(void);
    schedule();

    /* Re-acquire pipe lock after wakeup */
    spin_lock(pipe_lock);

    /* Remove ourselves from the wait queue */
    spin_lock(&wq->lock);
    list_del_init(&entry.entry);
    spin_unlock(&wq->lock);

    extern int signal_pending(task_struct_t *task);
    return signal_pending(me) ? -EINTR : 0;
}

/* =========================================================
 * File operations: read end
 * ========================================================= */

static s64 pipe_read(file_t *file, char *buf, size_t count, u64 *ppos) {
    (void)ppos;
    if (!file || !buf) return -EFAULT;
    if (count == 0) return 0;

    pipe_inode_t *pi = (pipe_inode_t *)file->private_data;
    if (!pi) return -EINVAL;

    bool nonblock = (file->f_flags & O_NONBLOCK) != 0;

    for (;;) {
        spin_lock(&pi->lock);

        if (pbuf_count(&pi->buf) > 0) {
            /* Data available — read it */
            size_t n = pbuf_read(&pi->buf, (u8 *)buf, count);
            spin_unlock(&pi->lock);
            pipe_wake_writers(pi);
            return (s64)n;
        }

        /* Buffer empty */
        if (atomic_read(&pi->writers) == 0) {
            /* All write ends closed → EOF */
            spin_unlock(&pi->lock);
            return 0;
        }

        if (nonblock) { spin_unlock(&pi->lock); return -EAGAIN; }

        /* Block until data arrives or write end closes.
         * pipe_wait drops pi->lock, sleeps, re-acquires on wakeup. */
        int rerr = pipe_wait(pi, &pi->read_wait, &pi->lock);
        spin_unlock(&pi->lock);
        if (rerr) return rerr;
    }
}

/* =========================================================
 * File operations: write end
 * ========================================================= */

static s64 pipe_write(file_t *file, const char *buf, size_t count, u64 *ppos) {
    (void)ppos;
    if (!file || !buf) return -EFAULT;
    if (count == 0) return 0;

    pipe_inode_t *pi = (pipe_inode_t *)file->private_data;
    if (!pi) return -EINVAL;

    bool nonblock = (file->f_flags & O_NONBLOCK) != 0;

    /* POSIX: if all read ends closed, send SIGPIPE and return EPIPE */
    if (atomic_read(&pi->readers) == 0) {
        if (current) send_signal(current->pid, SIGPIPE);
        return -EPIPE;
    }

    size_t written = 0;

    /* Writes <= PIPE_BUF must be atomic (all or none w.r.t. interleaving) */
    bool must_be_atomic = (count <= 4096 /* PIPE_BUF */);

    while (written < count) {
        spin_lock(&pi->lock);

        /* Check readers again (could have changed while we slept) */
        if (atomic_read(&pi->readers) == 0) {
            spin_unlock(&pi->lock);
            if (current) send_signal(current->pid, SIGPIPE);
            return written > 0 ? (s64)written : -EPIPE;
        }

        size_t space = pbuf_space(&pi->buf);

        if (space == 0 || (must_be_atomic && space < count)) {
            /* Buffer full — block until reader drains some data.
             * pipe_wait drops pi->lock, sleeps, re-acquires on wakeup. */
            if (nonblock) {
                spin_unlock(&pi->lock);
                return written > 0 ? (s64)written : -EAGAIN;
            }
            int werr = pipe_wait(pi, &pi->write_wait, &pi->lock);
            /* pi->lock is held again after wakeup */
            if (werr) {
                spin_unlock(&pi->lock);
                return written > 0 ? (s64)written : werr;
            }
            continue;
        }

        size_t n = pbuf_write(&pi->buf, (const u8 *)buf + written,
                               count - written);
        spin_unlock(&pi->lock);

        written += n;
        pipe_wake_readers(pi);
    }

    return (s64)written;
}

/* =========================================================
 * Release (close) — decrement reader/writer counts
 * ========================================================= */

static int pipe_read_release(inode_t *inode, file_t *file) {
    (void)inode;
    pipe_inode_t *pi = (pipe_inode_t *)file->private_data;
    if (!pi) return 0;

    atomic_dec(&pi->readers);
    pipe_wake_writers(pi);   /* Writers waiting on space may get EPIPE now */

    /* If both ends closed, free the pipe */
    if (atomic_read(&pi->readers) == 0 && atomic_read(&pi->writers) == 0) {
        pipe_free(pi);
        file->private_data = NULL;
    }
    return 0;
}

static int pipe_write_release(inode_t *inode, file_t *file) {
    (void)inode;
    pipe_inode_t *pi = (pipe_inode_t *)file->private_data;
    if (!pi) return 0;

    atomic_dec(&pi->writers);
    pipe_wake_readers(pi);   /* Blocked readers will get EOF */

    if (atomic_read(&pi->readers) == 0 && atomic_read(&pi->writers) == 0) {
        pipe_free(pi);
        file->private_data = NULL;
    }
    return 0;
}

/* pipe ioctl — F_GETPIPE_SZ / F_SETPIPE_SZ handled via fcntl */
static s64 pipe_ioctl(file_t *file, unsigned int cmd, unsigned long arg) {
    (void)file; (void)cmd; (void)arg;
    return -ENOTTY;
}

static const file_operations_t pipe_read_fops = {
    .read    = pipe_read,
    .write   = NULL,    /* Read end cannot be written */
    .ioctl   = pipe_ioctl,
    .release = pipe_read_release,
};

static const file_operations_t pipe_write_fops = {
    .read    = NULL,    /* Write end cannot be read */
    .write   = pipe_write,
    .ioctl   = pipe_ioctl,
    .release = pipe_write_release,
};

/* =========================================================
 * Create a pipe — returns two file objects
 * ========================================================= */

static file_t *make_pipe_file(const file_operations_t *fops,
                               pipe_inode_t *pi, int flags) {
    file_t *f = kzalloc(sizeof(file_t), GFP_KERNEL);
    if (!f) return NULL;

    atomic_set(&f->f_count, 1);
    f->f_op          = fops;
    f->f_flags       = (u32)flags;
    f->f_mode        = (fops == &pipe_read_fops) ? FMODE_READ : FMODE_WRITE;
    f->f_pos         = 0;
    f->private_data  = pi;
    spin_lock_init(&f->f_lock);
    return f;
}

int do_pipe(int pipefd[2], int flags) {
    pipe_inode_t *pi = pipe_alloc(PIPE_BUF_SIZE);
    if (!pi) return -ENOMEM;

    atomic_set(&pi->readers, 1);
    atomic_set(&pi->writers, 1);

    file_t *rf = make_pipe_file(&pipe_read_fops,  pi, O_RDONLY | flags);
    file_t *wf = make_pipe_file(&pipe_write_fops, pi, O_WRONLY | flags);

    if (!rf || !wf) {
        if (rf) kfree(rf);
        if (wf) kfree(wf);
        pipe_free(pi);
        return -ENOMEM;
    }

    /* Install into current process's fd table */
    if (!current || !current->files) {
        kfree(rf); kfree(wf);
        pipe_free(pi);
        return -EBADF;
    }

    int rfd = get_unused_fd();
    if (rfd < 0) { kfree(rf); kfree(wf); pipe_free(pi); return -EMFILE; }
    fd_install(rfd, rf);

    int wfd = get_unused_fd();
    if (wfd < 0) {
        do_close(rfd);
        kfree(wf); pipe_free(pi);
        return -EMFILE;
    }
    fd_install(wfd, wf);

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    printk(KERN_DEBUG "[pipe] created pipe: rfd=%d wfd=%d buf=%zu\n",
           rfd, wfd, pi->buf.size);
    return 0;
}

/* =========================================================
 * Syscalls
 * ========================================================= */

s64 sys_pipe(int *pipefd) {
    if (!pipefd) return -EFAULT;
    int fds[2];
    int r = do_pipe(fds, 0);
    if (r < 0) return r;
    pipefd[0] = fds[0];
    pipefd[1] = fds[1];
    return 0;
}

s64 sys_pipe2(int *pipefd, int flags) {
    if (!pipefd) return -EFAULT;
    /* Only O_CLOEXEC and O_NONBLOCK are valid */
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    int fds[2];
    int r = do_pipe(fds, flags);
    if (r < 0) return r;
    pipefd[0] = fds[0];
    pipefd[1] = fds[1];
    return 0;
}

/* =========================================================
 * Pipefs (pseudo-filesystem backing pipe inodes)
 * ========================================================= */

static struct dentry *pipefs_mount(file_system_type_t *fs_type, int flags,
                                    const char *dev_name, void *data) {
    (void)fs_type; (void)flags; (void)dev_name; (void)data;
    return ERR_PTR(-EINVAL);   /* Internal filesystem — never mounted directly */
}

static void pipefs_kill_sb(super_block_t *sb) { (void)sb; }

file_system_type_t pipefs_fs_type = {
    .name    = "pipefs",
    .fs_flags = 0,
    .mount   = pipefs_mount,
    .kill_sb = pipefs_kill_sb,
};

int init_pipefs(void) {
    register_filesystem(&pipefs_fs_type);
    printk(KERN_INFO "[pipefs] initialized\n");
    return 0;
}
