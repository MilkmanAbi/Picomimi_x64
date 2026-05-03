/**
 * Picomimi-x64 File Descriptor & Directory Syscalls
 *
 * Implements:
 *   getdents64   — directory listing for ls, opendir, etc.
 *   getcwd       — current working directory
 *   chdir/fchdir — change directory
 *   fcntl        — F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL/F_GETPIPE_SZ
 *   dup/dup2/dup3— file descriptor duplication
 *   access       — permission check
 *   flock        — advisory locking stub
 *   fsync/fdatasync
 *   truncate/ftruncate
 *   statfs/fstatfs
 *   readv/writev — scatter-gather I/O
 *   pread64/pwrite64
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>


/* =========================================================
 * getdents64 — read directory entries
 *
 * struct linux_dirent64 layout:
 *   u64  d_ino
 *   s64  d_off
 *   u16  d_reclen
 *   u8   d_type
 *   char d_name[]  (NUL-terminated)
 * ========================================================= */

/* Callback context for VFS readdir → getdents64 */
typedef struct {
    struct linux_dirent64   *buf;       /* User buffer */
    size_t                   buf_size;  /* Total buffer size */
    size_t                   pos;       /* Current write position */
    int                      count;     /* Entries written */
    int                      error;
} getdents_ctx_t;

static int getdents_filldir(void *ctx_ptr, const char *name, int namelen,
                              u64 ino, unsigned int d_type) {
    getdents_ctx_t *ctx = (getdents_ctx_t *)ctx_ptr;

    /* Calculate record length (aligned to 8 bytes) */
    size_t reclen = offsetof(struct linux_dirent64, d_name) + (size_t)namelen + 1;
    reclen = ALIGN(reclen, 8);

    if (ctx->pos + reclen > ctx->buf_size) {
        ctx->error = -EINVAL;   /* Buffer too small */
        return -1;              /* Stop iteration */
    }

    struct linux_dirent64 *d = (struct linux_dirent64 *)
                                ((char *)ctx->buf + ctx->pos);

    d->d_ino    = ino;
    d->d_off    = (s64)(ctx->pos + reclen);
    d->d_reclen = (u16)reclen;
    d->d_type   = (u8)d_type;
    memcpy(d->d_name, name, (size_t)namelen);
    d->d_name[namelen] = 0;

    ctx->pos += reclen;
    ctx->count++;
    return 0;
}

s64 sys_getdents64(int fd, struct linux_dirent64 *dirp, unsigned int count) {
    if (!dirp || count == 0) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    inode_t *inode = file->f_inode;
    if (!inode || !S_ISDIR(inode->i_mode)) {
        fput(file);
        return -ENOTDIR;
    }

    getdents_ctx_t ctx = {
        .buf      = dirp,
        .buf_size = count,
        .pos      = 0,
        .count    = 0,
        .error    = 0,
    };

    /* Call VFS readdir — dispatches through f_op->readdir */
    s64 ret;
    if (file->f_op && file->f_op->readdir) {
        ret = file->f_op->readdir(file, &ctx, getdents_filldir);
        if (ret < 0 && ctx.count == 0) {
            fput(file);
            return ret;
        }
    } else {
        /* Fallback: synthesize "." and ".." from dentry */
        dentry_t *dentry = file->f_dentry;
        if (dentry) {
            /* "." */
            u64 self_ino = dentry->d_inode ? dentry->d_inode->i_ino : 1;
            getdents_filldir(&ctx, ".", 1, self_ino, DT_DIR);

            /* ".." */
            dentry_t *parent = dentry->d_parent ? dentry->d_parent : dentry;
            u64 par_ino = (parent->d_inode) ? parent->d_inode->i_ino : 1;
            getdents_filldir(&ctx, "..", 2, par_ino, DT_DIR);

            /* Children */
            struct list_head *p;
            list_for_each(p, &dentry->d_subdirs) {
                dentry_t *child = list_entry(p, dentry_t, d_child);
                if (!child->d_inode) continue;

                unsigned int dtype = DT_UNKNOWN;
                u32 mode = child->d_inode->i_mode;
                if (S_ISREG(mode))  dtype = DT_REG;
                else if (S_ISDIR(mode)) dtype = DT_DIR;
                else if (S_ISCHR(mode)) dtype = DT_CHR;
                else if (S_ISBLK(mode)) dtype = DT_BLK;
                else if (S_ISFIFO(mode)) dtype = DT_FIFO;
                else if (S_ISLNK(mode)) dtype = DT_LNK;
                else if (S_ISSOCK(mode)) dtype = DT_SOCK;

                int r = getdents_filldir(&ctx,
                    (const char *)child->d_name.name,
                    (int)child->d_name.len,
                    child->d_inode->i_ino,
                    dtype);
                if (r < 0) break;
            }
        }
    }

    fput(file);

    if (ctx.error && ctx.count == 0) return ctx.error;
    return (s64)ctx.pos;   /* Return total bytes written */
}

/* =========================================================
 * getcwd — copy current working directory path
 * ========================================================= */

s64 sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return -EINVAL;
    if (!current || !current->fs) {
        /* No fs context — return "/" */
        if (size < 2) return -ERANGE;
        buf[0] = '/'; buf[1] = 0;
        return 2;
    }

    dentry_t *cwd = current->fs->pwd;
    if (!cwd) {
        if (size < 2) return -ERANGE;
        buf[0] = '/'; buf[1] = 0;
        return 2;
    }

    /* Build path by walking up dentry tree */
    char tmp[4096];
    int  pos = 4095;
    tmp[pos] = 0;

    dentry_t *d = cwd;
    while (d && d != d->d_parent) {
        const char *name = (const char *)d->d_name.name;
        size_t nlen = d->d_name.len;

        if (pos < (int)nlen + 1) return -ERANGE;
        pos -= (int)nlen;
        memcpy(tmp + pos, name, nlen);
        pos--;
        tmp[pos] = '/';
        d = d->d_parent;
    }

    if (tmp[pos] == 0) {
        pos--;
        tmp[pos] = '/';
    }

    size_t pathlen = (size_t)(4095 - pos + 1);
    if (pathlen > size) return -ERANGE;

    memcpy(buf, tmp + pos, pathlen);
    return (s64)pathlen;
}

/* =========================================================
 * chdir / fchdir
 * ========================================================= */

s64 sys_chdir(const char *path) {
    if (!path) return -EFAULT;
    if (!current || !current->fs) return -ENOENT;

    path_t p;
    int r = kern_path(path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &p);
    if (r < 0) return r;

    if (!S_ISDIR(p.dentry->d_inode->i_mode)) {
        path_put(&p);
        return -ENOTDIR;
    }

    current->fs->pwd = p.dentry;
    return 0;
}

s64 sys_fchdir(int fd) {
    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    inode_t *inode = file->f_inode;
    if (!inode || !S_ISDIR(inode->i_mode)) {
        fput(file);
        return -ENOTDIR;
    }

    if (current && current->fs)
        current->fs->pwd = file->f_dentry;

    fput(file);
    return 0;
}

/* =========================================================
 * dup / dup2 / dup3
 * ========================================================= */

s64 sys_dup(int oldfd) {
    file_t *file = fget((unsigned int)oldfd);
    if (!file) return -EBADF;

    int newfd = get_unused_fd();
    if (newfd < 0) { fput(file); return -EMFILE; }

    fd_install(newfd, file);
    /* Don't fput — fd_install holds the reference */
    return (s64)newfd;
}

s64 sys_dup2(int oldfd, int newfd) {
    if (oldfd == newfd) {
        /* Verify oldfd is valid */
        file_t *f = fget((unsigned int)oldfd);
        if (!f) return -EBADF;
        fput(f);
        return (s64)newfd;
    }

    file_t *file = fget((unsigned int)oldfd);
    if (!file) return -EBADF;

    /* Close newfd if open */
    if (newfd >= 0 && newfd < MAX_FDS_PER_PROCESS) {
        file_t *existing = fget((unsigned int)newfd);
        if (existing) {
            fput(existing);
            do_close(newfd);
        }
    }

    /* Install at newfd */
    if (!current || !current->files) { fput(file); return -EBADF; }

    if (newfd < 0 || newfd >= MAX_FDS_PER_PROCESS) {
        fput(file);
        return -EBADF;
    }

    current->files->fd_array[newfd].file  = file;
    current->files->fd_array[newfd].flags = 0;
    return (s64)newfd;
}

s64 sys_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    s64 r = sys_dup2(oldfd, newfd);
    if (r < 0) return r;
    if (flags & O_CLOEXEC) {
        if (current && current->files)
            current->files->fd_array[newfd].flags |= 1; /* FD_CLOEXEC */
    }
    return r;
}

/* =========================================================
 * fcntl
 * ========================================================= */

#define F_DUPFD             0
#define F_GETFD             1
#define F_SETFD             2
#define F_GETFL             3
#define F_SETFL             4
#define F_GETLK             5
#define F_SETLK             6
#define F_SETLKW            7
#define F_SETOWN            8
#define F_GETOWN            9
#define F_DUPFD_CLOEXEC     1030
#define F_GETPIPE_SZ        1032
#define F_SETPIPE_SZ        1031
#define FD_CLOEXEC          1

s64 sys_fcntl(int fd, int cmd, u64 arg) {
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= MAX_FDS_PER_PROCESS) return -EBADF;

    fd_entry_t *entry = &current->files->fd_array[fd];
    file_t *file = entry->file;
    if (!file) return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        /* Find first free fd >= arg */
        for (int i = (int)arg; i < MAX_FDS_PER_PROCESS; i++) {
            if (!current->files->fd_array[i].file) {
                current->files->fd_array[i].file  = file;
                current->files->fd_array[i].flags =
                    (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                atomic_inc(&file->f_count);
                return (s64)i;
            }
        }
        return -EMFILE;
    }

    case F_GETFD:
        return (s64)(entry->flags & FD_CLOEXEC);

    case F_SETFD:
        entry->flags = (u32)arg & FD_CLOEXEC;
        return 0;

    case F_GETFL:
        return (s64)file->f_flags;

    case F_SETFL:
        /* Allow setting O_APPEND, O_NONBLOCK, O_ASYNC */
        file->f_flags = (file->f_flags & ~(O_APPEND | O_NONBLOCK | O_ASYNC))
                      | ((u32)arg & (O_APPEND | O_NONBLOCK | O_ASYNC));
        return 0;

    case F_GETPIPE_SZ: {
        /* Pipe size — check if this is a pipe fd */
        if (file->private_data) {
            /* Assume private_data is pipe_inode_t */

            /* Size is stored at offset 0 of pipe_buffer_t → pipe_inode_t */
            /* Safest: return default */
            return 65536;
        }
        return -EINVAL;
    }

    case F_SETPIPE_SZ:
        /* TODO: resize pipe buffer */
        return (s64)arg;

    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        /* Advisory locking — stub, always succeed */
        return 0;

    case F_GETOWN:
        return current ? (s64)current->pid : 0;

    case F_SETOWN:
        return 0;

    default:
        return -EINVAL;
    }
}

/* =========================================================
 * access — check file permissions
 * ========================================================= */

#define F_OK    0
#define X_OK    1
#define W_OK    2
#define R_OK    4

s64 sys_access(const char *filename, int mode) {
    if (!filename) return -EFAULT;

    path_t p;
    int r = kern_path(filename, LOOKUP_FOLLOW, &p);
    if (r < 0) return r;

    inode_t *inode = p.dentry->d_inode;
    if (!inode) { path_put(&p); return -ENOENT; }

    if (mode == F_OK) { path_put(&p); return 0; }

    /* Simplified: root can always access anything */
    if (current && current->cred && current->cred->euid == 0) {
        path_put(&p);
        return 0;
    }

    /* Check mode bits (world permissions for simplicity) */
    u32 fmode = inode->i_mode;
    bool ok = true;
    if ((mode & R_OK) && !(fmode & S_IROTH)) ok = false;
    if ((mode & W_OK) && !(fmode & S_IWOTH)) ok = false;
    if ((mode & X_OK) && !(fmode & S_IXOTH)) ok = false;

    path_put(&p);
    return ok ? 0 : -EACCES;
}

/* =========================================================
 * flock — advisory record locking stub
 * ========================================================= */

s64 sys_flock(int fd, int operation) {
    (void)fd; (void)operation;
    /* Stub: always succeed (single-process scenario anyway) */
    return 0;
}

/* =========================================================
 * fsync / fdatasync
 * ========================================================= */

s64 sys_fsync(int fd) {
    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    s64 r = 0;
    if (file->f_op && file->f_op->fsync)
        r = (s64)file->f_op->fsync(file);

    fput(file);
    return r;
}

s64 sys_fdatasync(int fd) {
    return sys_fsync(fd);   /* We don't distinguish data vs metadata sync */
}

s64 sys_sync(void) {
    /* Walk all open files and fsync them — stub */
    return 0;
}

/* =========================================================
 * truncate / ftruncate
 * ========================================================= */

s64 sys_truncate(const char *path, s64 length) {
    if (!path) return -EFAULT;
    if (length < 0) return -EINVAL;

    path_t p;
    int r = kern_path(path, LOOKUP_FOLLOW, &p);
    if (r < 0) return r;

    inode_t *inode = p.dentry->d_inode;
    if (!inode) { path_put(&p); return -ENOENT; }
    if (S_ISDIR(inode->i_mode)) { path_put(&p); return -EISDIR; }

    inode->i_size = length;
    path_put(&p);
    return 0;
}

s64 sys_ftruncate(int fd, s64 length) {
    if (length < 0) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    if (!(file->f_mode & FMODE_WRITE)) { fput(file); return -EBADF; }

    inode_t *inode = file->f_inode;
    if (inode) inode->i_size = length;

    fput(file);
    return 0;
}

/* =========================================================
 * statfs / fstatfs
 * ========================================================= */

s64 sys_statfs(const char *path, struct statfs *buf) {
    if (!path || !buf) return -EFAULT;

    /* Provide reasonable fake stats */
    memset(buf, 0, sizeof(*buf));
    buf->f_type   = 0x858458F6;  /* RAMFS_MAGIC */
    buf->f_bsize  = 4096;
    buf->f_blocks = 65536;       /* 256 MB */
    buf->f_bfree  = 32768;
    buf->f_bavail = 32768;
    buf->f_files  = 8192;
    buf->f_ffree  = 7000;
    buf->f_namelen = 255;
    return 0;
}

s64 sys_fstatfs(int fd, struct statfs *buf) {
    (void)fd;
    if (!buf) return -EFAULT;
    return sys_statfs("/", buf);
}

/* =========================================================
 * readv / writev — scatter-gather I/O
 * ========================================================= */

s64 sys_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    if (iovcnt > 1024) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    s64 total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;

        s64 r = vfs_read(file, (char *)iov[i].iov_base,
                         iov[i].iov_len, &file->f_pos);
        if (r < 0) {
            if (total == 0) total = r;
            break;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) break;  /* Short read */
    }

    fput(file);
    return total;
}

s64 sys_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    if (iovcnt > 1024) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    s64 total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;

        s64 r = vfs_write(file, (const char *)iov[i].iov_base,
                          iov[i].iov_len, &file->f_pos);
        if (r < 0) {
            if (total == 0) total = r;
            break;
        }
        total += r;
    }

    fput(file);
    return total;
}

/* =========================================================
 * pread64 / pwrite64 — positional I/O (no seek side-effect)
 * ========================================================= */

s64 sys_pread64(int fd, void *buf, size_t count, s64 offset) {
    if (!buf) return -EFAULT;
    if (offset < 0) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    u64 pos = (u64)offset;
    s64 r = vfs_read(file, (char *)buf, count, &pos);
    fput(file);
    return r;
}

s64 sys_pwrite64(int fd, const void *buf, size_t count, s64 offset) {
    if (!buf) return -EFAULT;
    if (offset < 0) return -EINVAL;

    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;

    u64 pos = (u64)offset;
    s64 r = vfs_write(file, (const char *)buf, count, &pos);
    fput(file);
    return r;
}

/* =========================================================
 * umask
 * ========================================================= */

s64 sys_umask(u32 mask) {
    u32 old = 0022;
    if (current && current->fs) {
        old = (u32)current->fs->umask;
        current->fs->umask = (int)(mask & 0777);
    }
    return (s64)old;
}

/* =========================================================
 * sys_stat / sys_fstat / sys_lstat
 * ========================================================= */

s64 sys_stat(const char *pathname, struct linux_stat *statbuf) {
    if (!pathname || !statbuf) return -EFAULT;
    /* Lookup dentry and get inode */
    extern inode_t *kern_path_to_inode(const char *path);
    inode_t *inode = kern_path_to_inode(pathname);
    if (!inode) return -ENOENT;
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_ino   = inode->i_ino;
    statbuf->st_mode  = inode->i_mode;
    statbuf->st_nlink = inode->i_nlink;
    statbuf->st_uid   = inode->i_uid;
    statbuf->st_gid   = inode->i_gid;
    statbuf->st_size  = inode->i_size;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks  = (inode->i_size + 511) / 512;
    return 0;
}

s64 sys_lstat(const char *pathname, struct linux_stat *statbuf) {
    /* For now same as stat (no symlink following difference) */
    return sys_stat(pathname, statbuf);
}

s64 sys_fstat(int fd, struct linux_stat *statbuf) {
    if (!statbuf) return -EFAULT;
    file_t *file = fget((unsigned int)fd);
    if (!file) return -EBADF;
    inode_t *inode = file->f_inode;
    fput(file);
    if (!inode) return -ENOENT;
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_ino   = inode->i_ino;
    statbuf->st_mode  = inode->i_mode;
    statbuf->st_nlink = inode->i_nlink;
    statbuf->st_uid   = inode->i_uid;
    statbuf->st_gid   = inode->i_gid;
    statbuf->st_size  = inode->i_size;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks  = (inode->i_size + 511) / 512;
    return 0;
}
