/**
 * Picomimi-x64 *at Syscall Family
 *
 * POSIX.1-2008 directory file descriptor variants of classic filesystem
 * syscalls. AT_FDCWD (-100) means: use current working directory.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>


/* Forward declarations */
extern char *kstrdup(const char *s, unsigned int gfp);
extern s64 sys_mknod(const char *pathname, u32 mode, u32 dev);
extern s64 sys_lchown(const char *pathname, u32 owner, u32 group);
extern s64 sys_stat(const char *pathname, struct linux_stat *statbuf);
extern s64 sys_lstat(const char *pathname, struct linux_stat *statbuf);

#define AT_FDCWD             (-100)
#define AT_SYMLINK_NOFOLLOW   0x100
#define AT_REMOVEDIR          0x200
#define AT_SYMLINK_FOLLOW     0x400
#define AT_EMPTY_PATH         0x1000


/* =========================================================
 * at_resolve — resolve a path relative to dirfd
 * ========================================================= */

static int at_resolve(int dirfd, const char *pathname, unsigned int flags,
                       path_t *path) {
    if (!pathname) return -EFAULT;

    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return kern_path(pathname, flags, path);

    file_t *dir_file = fget((unsigned int)dirfd);
    if (!dir_file) return -EBADF;

    dentry_t *base = dir_file->f_dentry;
    if (!base || !base->d_inode) { fput(dir_file); return -EBADF; }
    if (!S_ISDIR(base->d_inode->i_mode)) { fput(dir_file); return -ENOTDIR; }

    /* Build full path via dentry_path_raw */
    char *base_path = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!base_path) { fput(dir_file); return -ENOMEM; }

    char *bp = dentry_path_raw(base, base_path, PATH_MAX);
    if (!bp) { kfree(base_path); fput(dir_file); return -ENAMETOOLONG; }

    size_t blen = strlen(bp);
    size_t plen = strlen(pathname);
    char *full = kmalloc(blen + 1 + plen + 1, GFP_KERNEL);
    if (!full) { kfree(base_path); fput(dir_file); return -ENOMEM; }

    memcpy(full, bp, blen);
    full[blen] = '/';
    memcpy(full + blen + 1, pathname, plen + 1);

    int err = kern_path(full, flags, path);
    kfree(full);
    kfree(base_path);
    fput(dir_file);
    return err;
}

/* Helper: get basename from path */
static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return base;
}

/* =========================================================
 * openat
 * ========================================================= */

s64 sys_openat(int dirfd, const char *pathname, int flags, u32 mode) {
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_open(pathname, flags, mode);

    /* Build full path, then delegate */
    file_t *dir_file = fget((unsigned int)dirfd);
    if (!dir_file) return -EBADF;

    dentry_t *base = dir_file->f_dentry;
    if (!base) { fput(dir_file); return -EBADF; }

    char *base_path = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!base_path) { fput(dir_file); return -ENOMEM; }

    char *bp = dentry_path_raw(base, base_path, PATH_MAX);
    if (!bp) { kfree(base_path); fput(dir_file); return -ENAMETOOLONG; }

    size_t blen = strlen(bp);
    size_t plen = strlen(pathname);
    char *full = kmalloc(blen + 1 + plen + 1, GFP_KERNEL);
    if (!full) { kfree(base_path); fput(dir_file); return -ENOMEM; }
    memcpy(full, bp, blen);
    full[blen] = '/';
    memcpy(full + blen + 1, pathname, plen + 1);

    kfree(base_path);
    fput(dir_file);

    s64 ret = sys_open(full, flags, mode);
    kfree(full);
    return ret;
}

/* =========================================================
 * mkdirat
 * ========================================================= */

s64 sys_mkdirat(int dirfd, const char *pathname, u32 mode) {
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_mkdir(pathname, mode);

    path_t parent;
    /* Resolve parent directory */
    char *dup = kstrdup(pathname, GFP_KERNEL);
    if (!dup) return -ENOMEM;

    /* Strip basename */
    char *slash = strrchr(dup, '/');
    if (slash) *slash = 0;
    else { dup[0] = '.'; dup[1] = 0; }

    int err = at_resolve(dirfd, dup, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
    kfree(dup);
    if (err < 0) return err;

    if (!parent.dentry || !parent.dentry->d_inode) {
        path_put(&parent); return -ENOENT;
    }

    const char *base = path_basename(pathname);
    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };

    dentry_t *existing = d_lookup(parent.dentry, &qname);
    if (existing) { dput(existing); path_put(&parent); return -EEXIST; }

    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_mkdir(parent.dentry->d_inode, new_d, mode);
    dput(new_d);
    path_put(&parent);
    return err;
}

/* =========================================================
 * mknodat
 * ========================================================= */

s64 sys_mknodat(int dirfd, const char *pathname, u32 mode, dev_t dev) {
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_mknod(pathname, mode, dev);

    path_t parent;
    char *dup = kstrdup(pathname, GFP_KERNEL);
    if (!dup) return -ENOMEM;
    char *slash = strrchr(dup, '/');
    if (slash) *slash = 0; else { dup[0] = '.'; dup[1] = 0; }

    int err = at_resolve(dirfd, dup, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
    kfree(dup);
    if (err < 0) return err;

    const char *base = path_basename(pathname);
    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };
    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_mknod(parent.dentry->d_inode, new_d, mode, dev);
    dput(new_d);
    path_put(&parent);
    return err;
}

/* =========================================================
 * fchownat
 * ========================================================= */

s64 sys_fchownat(int dirfd, const char *pathname, u32 owner, u32 group,
                  int flags) {
    if (!pathname || pathname[0] == 0) {
        if ((flags & AT_EMPTY_PATH) && dirfd != AT_FDCWD)
            return sys_fchown(dirfd, owner, group);
        return -ENOENT;
    }
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return (flags & AT_SYMLINK_NOFOLLOW)
               ? sys_lchown(pathname, owner, group)
               : sys_chown(pathname, owner, group);

    path_t p;
    unsigned int lf = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
    int err = at_resolve(dirfd, pathname, lf, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }

    p.dentry->d_inode->i_uid = owner;
    p.dentry->d_inode->i_gid = group;
    path_put(&p);
    return 0;
}

/* =========================================================
 * newfstatat
 * ========================================================= */

s64 sys_newfstatat(int dirfd, const char *pathname, struct stat *statbuf,
                    int flags) {
    if (!statbuf) return -EFAULT;

    if (!pathname || pathname[0] == 0) {
        if (flags & AT_EMPTY_PATH) return sys_fstat(dirfd, statbuf);
        return -ENOENT;
    }
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return (flags & AT_SYMLINK_NOFOLLOW)
               ? sys_lstat(pathname, statbuf)
               : sys_stat(pathname, statbuf);

    path_t p;
    unsigned int lf = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
    int err = at_resolve(dirfd, pathname, lf, &p);
    if (err < 0) return err;

    inode_t *inode = p.dentry ? p.dentry->d_inode : NULL;
    if (!inode) { path_put(&p); return -ENOENT; }

    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_ino     = inode->i_ino;
    statbuf->st_mode    = inode->i_mode;
    statbuf->st_nlink   = inode->i_nlink;
    statbuf->st_uid     = inode->i_uid;
    statbuf->st_gid     = inode->i_gid;
    statbuf->st_size    = inode->i_size;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks  = (inode->i_size + 511) / 512;

    path_put(&p);
    return 0;
}

/* =========================================================
 * unlinkat
 * ========================================================= */

s64 sys_unlinkat(int dirfd, const char *pathname, int flags) {
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return (flags & AT_REMOVEDIR) ? sys_rmdir(pathname) : sys_unlink(pathname);

    path_t p;
    int err = at_resolve(dirfd, pathname, 0, &p);
    if (err < 0) return err;
    if (!p.dentry) { path_put(&p); return -ENOENT; }

    if (flags & AT_REMOVEDIR) {
        err = vfs_rmdir(p.dentry->d_parent ? p.dentry->d_parent->d_inode : NULL,
                        p.dentry);
    } else {
        err = vfs_unlink(p.dentry->d_parent ? p.dentry->d_parent->d_inode : NULL,
                         p.dentry, NULL);
    }
    path_put(&p);
    return err;
}

/* =========================================================
 * renameat / renameat2
 * ========================================================= */

s64 sys_renameat(int olddir, const char *oldpath, int newdir, const char *newpath) {
    if (!oldpath || !newpath) return -EFAULT;
    if ((oldpath[0] == '/' || olddir == AT_FDCWD) &&
        (newpath[0] == '/' || newdir == AT_FDCWD))
        return sys_rename(oldpath, newpath);

    /* Resolve both paths and build full names */
    path_t op, np;
    int err = at_resolve(olddir, oldpath, 0, &op);
    if (err < 0) return err;

    err = at_resolve(newdir, newpath, LOOKUP_PARENT, &np);
    if (err < 0) { path_put(&op); return err; }

    if (!op.dentry || !np.dentry) {
        path_put(&op); path_put(&np); return -ENOENT;
    }

    /* Build target dentry for the new name */
    const char *new_base = path_basename(newpath);
    qstr_t qname = { .name = (const u8 *)new_base,
                     .len  = (u32)strlen(new_base), .hash = 0 };
    dentry_t *new_d = d_alloc(np.dentry, &qname);
    if (!new_d) { path_put(&op); path_put(&np); return -ENOMEM; }

    err = vfs_rename(op.dentry->d_parent ? op.dentry->d_parent->d_inode : NULL,
                     op.dentry,
                     np.dentry->d_inode,
                     new_d, NULL, 0);
    dput(new_d);
    path_put(&op);
    path_put(&np);
    return err;
}

s64 sys_renameat2(int olddir, const char *oldpath, int newdir, const char *newpath,
                   unsigned int flags) {
    /* flags: RENAME_NOREPLACE, RENAME_EXCHANGE — not fully supported */
    (void)flags;
    return sys_renameat(olddir, oldpath, newdir, newpath);
}

/* =========================================================
 * linkat
 * ========================================================= */

s64 sys_linkat(int olddir, const char *oldpath, int newdir, const char *newpath,
               int flags) {
    (void)flags;
    if (!oldpath || !newpath) return -EFAULT;
    if ((oldpath[0] == '/' || olddir == AT_FDCWD) &&
        (newpath[0] == '/' || newdir == AT_FDCWD))
        return sys_link(oldpath, newpath);

    /* Relative linkat — resolve old, build new */
    path_t op;
    unsigned int lf = (flags & AT_SYMLINK_FOLLOW) ? LOOKUP_FOLLOW : 0;
    int err = at_resolve(olddir, oldpath, lf, &op);
    if (err < 0) return err;

    path_t np;
    err = at_resolve(newdir, newpath, LOOKUP_PARENT, &np);
    if (err < 0) { path_put(&op); return err; }

    if (!op.dentry || !np.dentry) {
        path_put(&op); path_put(&np); return -ENOENT;
    }

    const char *nb = path_basename(newpath);
    qstr_t qname = { .name = (const u8 *)nb, .len = (u32)strlen(nb), .hash = 0 };
    dentry_t *new_d = d_alloc(np.dentry, &qname);
    if (!new_d) { path_put(&op); path_put(&np); return -ENOMEM; }

    err = vfs_link(op.dentry, np.dentry->d_inode, new_d, NULL);
    dput(new_d);
    path_put(&op);
    path_put(&np);
    return err;
}

/* =========================================================
 * symlinkat
 * ========================================================= */

s64 sys_symlinkat(const char *target, int newdir, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    if (linkpath[0] == '/' || newdir == AT_FDCWD)
        return sys_symlink(target, linkpath);

    path_t parent;
    char *dup = kstrdup(linkpath, GFP_KERNEL);
    if (!dup) return -ENOMEM;
    char *slash = strrchr(dup, '/');
    if (slash) *slash = 0; else { dup[0] = '.'; dup[1] = 0; }

    int err = at_resolve(newdir, dup, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
    kfree(dup);
    if (err < 0) return err;

    const char *base = path_basename(linkpath);
    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };
    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_symlink(parent.dentry->d_inode, new_d, target);
    dput(new_d);
    path_put(&parent);
    return err;
}

/* =========================================================
 * readlinkat
 * ========================================================= */

s64 sys_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    if (!pathname || !buf) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_readlink(pathname, buf, bufsiz);

    path_t p;
    int err = at_resolve(dirfd, pathname, 0 /* no follow */, &p);
    if (err < 0) return err;

    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    if (!S_ISLNK(p.dentry->d_inode->i_mode)) { path_put(&p); return -EINVAL; }

    inode_t *inode = p.dentry->d_inode;
    s64 ret = -EINVAL;
    if (inode->i_op && inode->i_op->readlink) {
        ret = inode->i_op->readlink(p.dentry, buf, (int)bufsiz);
    }
    path_put(&p);
    return ret;
}

/* =========================================================
 * fchmodat
 * ========================================================= */

s64 sys_fchmodat(int dirfd, const char *pathname, u32 mode, int flags) {
    (void)flags;
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_chmod(pathname, mode);

    path_t p;
    int err = at_resolve(dirfd, pathname, LOOKUP_FOLLOW, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }

    p.dentry->d_inode->i_mode = (p.dentry->d_inode->i_mode & S_IFMT) | (mode & 0777);
    path_put(&p);
    return 0;
}

/* =========================================================
 * faccessat / faccessat2
 * ========================================================= */

s64 sys_faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)flags;
    if (!pathname) return -EFAULT;
    if (pathname[0] == '/' || dirfd == AT_FDCWD)
        return sys_access(pathname, mode);

    path_t p;
    int err = at_resolve(dirfd, pathname, LOOKUP_FOLLOW, &p);
    if (err < 0) return err;

    bool ok = (p.dentry && p.dentry->d_inode) ? true : false;
    path_put(&p);
    return ok ? 0 : -ENOENT;
}

s64 sys_faccessat2(int dirfd, const char *pathname, int mode, int flags) {
    return sys_faccessat(dirfd, pathname, mode, flags);
}
/* Forward declarations for vfs_ops functions */
