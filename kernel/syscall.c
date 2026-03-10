/**
 * Picomimi-x64 System Call Implementation
 * 
 * POSIX-compliant syscall handlers
 */

#include <kernel/types.h>
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>

/* VFS open/close */
extern file_t *filp_open(const char *filename, int flags, u32 mode);
extern void    filp_close(file_t *filp, void *id);
extern int     get_unused_fd(void);
extern void    fd_install(int fd, file_t *file);

// Spinlock functions (implemented in process.c)

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

s64 sys_sendfile(int out_fd, int in_fd, s64 *offset, size_t count) {
    file_t *in_file  = fget(in_fd);
    file_t *out_file = fget(out_fd);
    if (!in_file || !out_file) {
        if (in_file)  fput(in_file);
        if (out_file) fput(out_file);
        return -EBADF;
    }
    u64 pos = offset ? (u64)*offset : in_file->f_pos;
    s64 total = 0;
    char buf[4096];
    while (total < (s64)count) {
        size_t chunk = count - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        s64 n = vfs_read(in_file, buf, chunk, &pos);
        if (n <= 0) break;
        s64 w = 0;
        while (w < n) {
            s64 wn;
            if (out_fd == 1 || out_fd == 2) {
                for (s64 i = w; i < n; i++) printk("%c", buf[i]);
                wn = n - w;
            } else {
                wn = vfs_write(out_file, buf + w, n - w, &out_file->f_pos);
                if (wn <= 0) break;
            }
            w += wn;
        }
        total += w;
        if (w < n) break;
    }
    if (!offset) in_file->f_pos = pos;
    else *offset = (s64)pos;
    fput(in_file);
    fput(out_file);
    return total;
}

s64 sys_open(const char *filename, int flags, u32 mode) {
    if (!filename) return -EFAULT;

    /* For O_CREAT we need to handle creation through the VFS layer.
     * filp_open will check for O_CREAT internally.
     * We pass flags straight through — filp_open handles O_DIRECTORY too. */
    file_t *f = filp_open(filename, flags, mode);
    if (IS_ERR(f)) {
        return PTR_ERR(f);
    }

    /* Allocate a file descriptor */
    int fd = get_unused_fd();
    if (fd < 0) {
        filp_close(f, NULL);
        return (s64)fd;
    }

    fd_install(fd, f);
    return (s64)fd;
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
    case SEEK_END: {
        s64 size = 0;
        if (file->f_inode) size = (s64)file->f_inode->i_size;
        new_pos = size + offset;
        break;
    }
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













// ============================================================================
// TIME
// ============================================================================


// ============================================================================
// SIGNALS
// ============================================================================


s64 sys_ioctl(int fd, u64 cmd, u64 arg) {
    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;
    s64 ret = -ENOTTY;
    if (file->f_op && file->f_op->ioctl)
        ret = file->f_op->ioctl(file, cmd, arg);
    fput(file);
    return ret;
}

s64 sys_execve(const char *filename, char *const argv[], char *const envp[]) {
    if (!filename) return -EFAULT;
    extern s64 do_execve(const char *filename, char *const argv[], char *const envp[]);
    return do_execve(filename, argv, envp);
}

s64 sys_sched_yield(void) {
    schedule();
    return 0;
}
