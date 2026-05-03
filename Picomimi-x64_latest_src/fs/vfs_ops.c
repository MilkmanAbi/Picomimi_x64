/**
 * Picomimi-x64 VFS Operations & MM/Process Helpers
 *
 * Higher-level VFS operations that sit above the inode_operations:
 *   vfs_mkdir / vfs_rmdir
 *   vfs_unlink
 *   vfs_link / vfs_symlink
 *   vfs_rename
 *   vfs_mknod
 *   file_open_root
 *   dentry_path_raw      — build path string from dentry chain
 *   sys_mkdir / sys_mknod / sys_lchown helpers
 *
 * Memory/process helpers:
 *   kstrdup              — kernel strdup
 *   vma_alloc / vma_free
 *   insert_vma
 *   phys_to_virt         — phys → kernel virtual (higher-half offset)
 *   vmm_unmap_page       — clear PTE for a virtual address
 *   vmm_virt_to_phys     — walk page tables to find physical
 *   pid_free             — return PID to pool
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>


/* =========================================================
 * kstrdup — allocate and copy a string
 * ========================================================= */

char *kstrdup(const char *s, gfp_t gfp) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *dup = kmalloc(n, gfp);
    if (dup) memcpy(dup, s, n);
    return dup;
}

char *kstrndup(const char *s, size_t max, gfp_t gfp) {
    if (!s) return NULL;
    size_t n = strnlen(s, max);
    char *dup = kmalloc(n + 1, gfp);
    if (dup) {
        memcpy(dup, s, n);
        dup[n] = 0;
    }
    return dup;
}

/* =========================================================
 * dentry_path_raw — reconstruct path from dentry to root
 *
 * Fills `buf` from the right, returns pointer into buf.
 * Returns NULL on overflow.
 * ========================================================= */

char *dentry_path_raw(dentry_t *dentry, char *buf, int buflen) {
    if (!buf || buflen <= 0) return NULL;

    int end = buflen - 1;
    buf[end] = 0;

    dentry_t *d = dentry;

    while (d && d->d_parent && d != d->d_parent) {
        const char *name = d->d_name.name ? (const char *)d->d_name.name : "";
        int nlen = (int)d->d_name.len;

        if (end - nlen - 1 < 0) return NULL;  /* overflow */

        end -= nlen;
        memcpy(buf + end, name, (size_t)nlen);
        buf[--end] = '/';

        d = d->d_parent;
    }

    if (end == buflen - 1) {
        /* Root itself — path is "/" */
        if (end < 1) return NULL;
        buf[--end] = '/';
    }

    return buf + end;
}

/* =========================================================
 * VFS mkdir
 * ========================================================= */

int vfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode) {
    if (!dir || !dentry) return -EINVAL;
    if (!dir->i_op || !dir->i_op->mkdir) return -EPERM;

    int err = dir->i_op->mkdir(dir, dentry, mode | S_IFDIR);
    if (err == 0) {
        /* Update link count */
        if (dentry->d_inode) set_nlink(dentry->d_inode, 2);
        inode_inc_link_count(dir);   /* ".." inside new dir points back */
    }
    return err;
}

/* =========================================================
 * VFS rmdir
 * ========================================================= */

int vfs_rmdir(inode_t *dir, dentry_t *dentry) {
    if (!dir || !dentry) return -EINVAL;
    if (!dentry->d_inode) return -ENOENT;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -ENOTDIR;

    /* Check directory is empty */
    if (!list_empty(&dentry->d_subdirs)) return -ENOTEMPTY;

    if (dir->i_op && dir->i_op->rmdir)
        return dir->i_op->rmdir(dir, dentry);

    /* Generic fallback: detach from parent */
    list_del_init(&dentry->d_child);
    inode_dec_link_count(dentry->d_inode);
    d_delete(dentry);
    return 0;
}

/* =========================================================
 * VFS unlink
 * ========================================================= */

int vfs_unlink(inode_t *dir, dentry_t *dentry, inode_t **delegated) {
    (void)delegated;
    if (!dir || !dentry) return -EINVAL;
    if (!dentry->d_inode) return -ENOENT;
    if (S_ISDIR(dentry->d_inode->i_mode)) return -EISDIR;

    if (dir->i_op && dir->i_op->unlink)
        return dir->i_op->unlink(dir, dentry);

    /* Generic fallback */
    list_del_init(&dentry->d_child);
    inode_dec_link_count(dentry->d_inode);
    d_delete(dentry);
    return 0;
}

/* =========================================================
 * VFS link (hard link)
 * ========================================================= */

int vfs_link(dentry_t *old_dentry, inode_t *new_dir, dentry_t *new_dentry,
              inode_t **delegated) {
    (void)delegated;
    if (!old_dentry || !old_dentry->d_inode || !new_dir || !new_dentry)
        return -EINVAL;
    if (S_ISDIR(old_dentry->d_inode->i_mode)) return -EPERM;

    if (new_dir->i_op && new_dir->i_op->link)
        return new_dir->i_op->link(old_dentry, new_dir, new_dentry);

    /* Generic fallback: share the inode */
    inode_t *inode = old_dentry->d_inode;
    d_instantiate(new_dentry, inode);
    inode_inc_link_count(inode);
    return 0;
}

/* =========================================================
 * VFS symlink
 * ========================================================= */

int vfs_symlink(inode_t *dir, dentry_t *dentry, const char *oldname) {
    if (!dir || !dentry || !oldname) return -EINVAL;

    if (dir->i_op && dir->i_op->symlink)
        return dir->i_op->symlink(dir, dentry, oldname);

    /* Generic: create an inode with S_IFLNK and store target */
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) return -ENOMEM;

    static unsigned long next_ino = 0x8000;
    inode->i_ino  = next_ino++;
    inode->i_mode = S_IFLNK | 0777;
    set_nlink(inode, 1);

    /* Store link target in i_private */
    size_t tlen = strlen(oldname) + 1;
    char *target = kmalloc(tlen, GFP_KERNEL);
    if (!target) { iput(inode); return -ENOMEM; }
    memcpy(target, oldname, tlen);
    inode->i_private = target;
    inode->i_size    = (s64)tlen - 1;

    d_instantiate(dentry, inode);
    return 0;
}

/* =========================================================
 * VFS rename
 * ========================================================= */

int vfs_rename(inode_t *old_dir, dentry_t *old_dentry,
               inode_t *new_dir,  dentry_t *new_dentry,
               inode_t **delegated, unsigned int flags) {
    (void)delegated; (void)flags;
    if (!old_dir || !old_dentry || !new_dir) return -EINVAL;
    if (!old_dentry->d_inode) return -ENOENT;

    if (old_dir->i_op && old_dir->i_op->rename)
        return old_dir->i_op->rename(old_dir, old_dentry,
                                      new_dir, new_dentry, flags);

    /* Generic: re-link dentry */
    if (new_dentry && new_dentry->d_inode) {
        /* Remove existing target */
        list_del_init(&new_dentry->d_child);
        inode_dec_link_count(new_dentry->d_inode);
        d_delete(new_dentry);
    }

    /* Move old_dentry under new_dir */
    list_del_init(&old_dentry->d_child);
    old_dentry->d_parent = old_dentry->d_parent; /* keep parent, dir ref via dentry */
    if (new_dentry) {
        /* Rename: replace d_name */
        old_dentry->d_name = new_dentry->d_name;
    }
    return 0;
}

/* =========================================================
 * VFS mknod
 * ========================================================= */

int vfs_mknod(inode_t *dir, dentry_t *dentry, u32 mode, dev_t dev) {
    if (!dir || !dentry) return -EINVAL;

    if (dir->i_op && dir->i_op->mknod)
        return dir->i_op->mknod(dir, dentry, mode, dev);

    /* Generic fallback */
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) return -ENOMEM;

    static unsigned long next_ino = 0xA000;
    inode->i_ino   = next_ino++;
    inode->i_mode  = mode;
    inode->i_rdev  = dev;
    set_nlink(inode, 1);
    d_instantiate(dentry, inode);
    return 0;
}

/* =========================================================
 * file_open_root — open a file from a known dentry
 * ========================================================= */

file_t *file_open_root(dentry_t *dentry, vfsmount_t *mnt, const char *filename,
                        int flags, u32 mode) {
    (void)mnt; (void)filename; (void)mode;
    if (!dentry || !dentry->d_inode) return ERR_PTR(-ENOENT);

    file_t *file = kzalloc(sizeof(file_t), GFP_KERNEL);
    if (!file) return ERR_PTR(-ENOMEM);

    atomic_set(&file->f_count, 1);
    file->f_dentry = dentry;
    file->f_inode  = dentry->d_inode;
    file->f_flags  = (u32)flags;
    file->f_pos    = 0;
    file->f_op     = dentry->d_inode->i_fop;
    spin_lock_init(&file->f_lock);

    if (file->f_op && file->f_op->open) {
        int err = file->f_op->open(dentry->d_inode, file);
        if (err) { kfree(file); return ERR_PTR(err); }
    }
    return file;
}

/* =========================================================
 * sys_mkdir / sys_mknod / sys_lchown helpers
 * (thin wrappers over kern_path + vfs_* used by at_syscalls.c)
 * ========================================================= */

s64 sys_mkdir(const char *pathname, u32 mode) {
    if (!pathname) return -EFAULT;

    path_t parent;
    int err = kern_path(pathname, LOOKUP_PARENT, &parent);
    if (err < 0) return err;

    const char *base = pathname;
    for (const char *p = pathname; *p; p++)
        if (*p == '/') base = p + 1;
    if (!*base) { path_put(&parent); return -ENOENT; }

    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };

    /* Check if it already exists — if so, return EEXIST without
     * creating a second dentry with the same name. */
    dentry_t *existing = d_lookup(parent.dentry, &qname);
    if (existing) {
        dput(existing);
        path_put(&parent);
        return -EEXIST;
    }

    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_mkdir(parent.dentry->d_inode, new_d, mode);
    dput(new_d);
    path_put(&parent);
    return err;
}

s64 sys_rmdir(const char *pathname) {
    if (!pathname) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, LOOKUP_DIRECTORY, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    err = vfs_rmdir(p.dentry->d_parent ? p.dentry->d_parent->d_inode : NULL, p.dentry);
    path_put(&p);
    return err;
}

s64 sys_mknod(const char *pathname, u32 mode, dev_t dev) {
    if (!pathname) return -EFAULT;
    path_t parent;
    int err = kern_path(pathname, LOOKUP_PARENT, &parent);
    if (err < 0) return err;

    const char *base = pathname;
    for (const char *p = pathname; *p; p++)
        if (*p == '/') base = p + 1;

    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };

    dentry_t *existing = d_lookup(parent.dentry, &qname);
    if (existing) { dput(existing); path_put(&parent); return -EEXIST; }

    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_mknod(parent.dentry->d_inode, new_d, mode, dev);
    dput(new_d);
    path_put(&parent);
    return err;
}

s64 sys_lchown(const char *pathname, u32 owner, u32 group) {
    if (!pathname) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, 0 /* no follow */, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    p.dentry->d_inode->i_uid = owner;
    p.dentry->d_inode->i_gid = group;
    path_put(&p);
    return 0;
}

s64 sys_fchown(int fd, u32 owner, u32 group) {
    file_t *f = fget((unsigned int)fd);
    if (!f) return -EBADF;
    if (f->f_inode) {
        f->f_inode->i_uid = owner;
        f->f_inode->i_gid = group;
    }
    fput(f);
    return 0;
}

s64 sys_chmod(const char *pathname, u32 mode) {
    if (!pathname) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, LOOKUP_FOLLOW, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    p.dentry->d_inode->i_mode = (p.dentry->d_inode->i_mode & S_IFMT) | (mode & 07777);
    path_put(&p);
    return 0;
}

s64 sys_fchmod(int fd, u32 mode) {
    file_t *f = fget((unsigned int)fd);
    if (!f) return -EBADF;
    if (f->f_inode)
        f->f_inode->i_mode = (f->f_inode->i_mode & S_IFMT) | (mode & 07777);
    fput(f);
    return 0;
}

s64 sys_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EFAULT;
    path_t op, np_parent;

    int err = kern_path(oldpath, LOOKUP_FOLLOW, &op);
    if (err < 0) return err;

    err = kern_path(newpath, LOOKUP_PARENT, &np_parent);
    if (err < 0) { path_put(&op); return err; }

    const char *base = newpath;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') base = p + 1;

    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };
    dentry_t *new_d = d_alloc(np_parent.dentry, &qname);
    if (!new_d) { path_put(&op); path_put(&np_parent); return -ENOMEM; }

    err = vfs_link(op.dentry, np_parent.dentry->d_inode, new_d, NULL);
    dput(new_d);
    path_put(&op);
    path_put(&np_parent);
    return err;
}

s64 sys_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    path_t parent;
    int err = kern_path(linkpath, LOOKUP_PARENT, &parent);
    if (err < 0) return err;

    const char *base = linkpath;
    for (const char *p = linkpath; *p; p++)
        if (*p == '/') base = p + 1;

    qstr_t qname = { .name = (const u8 *)base,
                     .len  = (u32)strlen(base), .hash = 0 };

    dentry_t *existing = d_lookup(parent.dentry, &qname);
    if (existing) { dput(existing); path_put(&parent); return -EEXIST; }

    dentry_t *new_d = d_alloc(parent.dentry, &qname);
    if (!new_d) { path_put(&parent); return -ENOMEM; }

    err = vfs_symlink(parent.dentry->d_inode, new_d, target);
    dput(new_d);
    path_put(&parent);
    return err;
}

s64 sys_readlink(const char *pathname, char *buf, size_t bufsiz) {
    if (!pathname || !buf) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, 0, &p);  /* no follow */
    if (err < 0) return err;

    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    if (!S_ISLNK(p.dentry->d_inode->i_mode)) { path_put(&p); return -EINVAL; }

    s64 ret = -EINVAL;
    if (p.dentry->d_inode->i_op && p.dentry->d_inode->i_op->readlink) {
        ret = p.dentry->d_inode->i_op->readlink(p.dentry, buf, (int)bufsiz);
    } else if (p.dentry->d_inode->i_private) {
        /* Generic: stored in i_private */
        const char *target = (const char *)p.dentry->d_inode->i_private;
        size_t n = strlen(target);
        if (n > bufsiz) n = bufsiz;
        memcpy(buf, target, n);
        ret = (s64)n;
    }
    path_put(&p);
    return ret;
}

s64 sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EFAULT;
    path_t op, np;
    int err = kern_path(oldpath, 0, &op);
    if (err < 0) return err;
    err = kern_path(newpath, LOOKUP_PARENT, &np);
    if (err < 0) { path_put(&op); return err; }

    const char *nb = newpath;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') nb = p + 1;

    qstr_t qname = { .name = (const u8 *)nb, .len = (u32)strlen(nb), .hash = 0 };
    dentry_t *new_d = d_alloc(np.dentry, &qname);
    if (!new_d) { path_put(&op); path_put(&np); return -ENOMEM; }

    err = vfs_rename(op.dentry->d_parent ? op.dentry->d_parent->d_inode : NULL,
                     op.dentry,
                     np.dentry->d_inode, new_d, NULL, 0);
    dput(new_d);
    path_put(&op);
    path_put(&np);
    return err;
}

s64 sys_unlink(const char *pathname) {
    if (!pathname) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, 0, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_parent) { path_put(&p); return -ENOENT; }
    err = vfs_unlink(p.dentry->d_parent->d_inode, p.dentry, NULL);
    path_put(&p);
    return err;
}

s64 sys_chown(const char *pathname, u32 owner, u32 group) {
    if (!pathname) return -EFAULT;
    path_t p;
    int err = kern_path(pathname, LOOKUP_FOLLOW, &p);
    if (err < 0) return err;
    if (!p.dentry || !p.dentry->d_inode) { path_put(&p); return -ENOENT; }
    p.dentry->d_inode->i_uid = owner;
    p.dentry->d_inode->i_gid = group;
    path_put(&p);
    return 0;
}

/* =========================================================
 * do_mount wrapper
 * ========================================================= */

s64 sys_mount(const char *source, const char *target, const char *fstype,
               u64 flags, const void *data) {
    if (!target || !fstype) return -EFAULT;
    return (s64)do_mount(source, target, fstype, (unsigned long)flags, (void *)data);
}

s64 sys_umount2(const char *target, int flags) {
    (void)target; (void)flags;
    /* TODO: implement proper path->vfsmount lookup for umount */
    return -ENOSYS;
}

/* =========================================================
 * VMA helpers
 * ========================================================= */

vm_area_t *vma_alloc(void) {
    vm_area_t *vma = kzalloc(sizeof(vm_area_t), GFP_KERNEL);
    if (vma) INIT_LIST_HEAD(&vma->vm_list);
    return vma;
}

void vma_free(vm_area_t *vma) {
    if (vma) kfree(vma);
}

void insert_vma(mm_struct_t *mm, vm_area_t *vma) {
    if (!mm || !vma) return;
    /* Insert in sorted order by start address */
    vm_area_t *prev = NULL;
    vm_area_t *cur = mm->mmap;
    while (cur) {
        if (vma->start < cur->start) {
            /* Insert before cur */
            vma->next = cur;
            vma->prev = cur->prev;
            if (cur->prev) cur->prev->next = vma;
            else mm->mmap = vma;
            cur->prev = vma;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    /* Append at end */
    vma->next = NULL;
    vma->prev = prev;
    if (prev) prev->next = vma;
    else mm->mmap = vma;
}

/* =========================================================
 * Physical memory helpers
 * ========================================================= */

#define KERNEL_PHYS_BASE    0xFFFFFFFF80000000ULL

static u64 phys_to_virt_fn(u64 phys) {
    return phys + KERNEL_PHYS_BASE;
}

u64 virt_to_phys_kernel(u64 virt) {
    if (virt >= KERNEL_PHYS_BASE) return virt - KERNEL_PHYS_BASE;
    return 0;
}

/* Walk page tables to find physical address for a virtual address */
/* Walk the user PML4 (current CR3) to translate a user virtual address */
/* Return the raw PTE for a user virtual address in a given PGD, or 0 if not mapped */
u64 vmm_pte_for_virt_in(u64 pgd_phys, u64 virt) {
    u64 *pml4 = (u64 *)(pgd_phys + KERNEL_PHYS_BASE);
    int pml4_i = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & 1)) return 0;
    u64 *pdpt = (u64 *)((pml4[pml4_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pdpt_i = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & 1)) return 0;
    if (pdpt[pdpt_i] & (1ULL << 7)) return pdpt[pdpt_i]; /* 1GB */
    u64 *pd = (u64 *)((pdpt[pdpt_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pd_i = (virt >> 21) & 0x1FF;
    if (!(pd[pd_i] & 1)) return 0;
    if (pd[pd_i] & (1ULL << 7)) return pd[pd_i]; /* 2MB */
    u64 *pt = (u64 *)((pd[pd_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pt_i = (virt >> 12) & 0x1FF;
    return pt[pt_i]; /* 4KB */
}

/* Walk a specific PGD (given as physical address) to resolve a user virtual address */
u64 vmm_user_virt_to_phys_in(u64 pgd_phys, u64 virt) {
    u64 *pml4 = (u64 *)(pgd_phys + KERNEL_PHYS_BASE);
    int pml4_i = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & 1)) return 0;
    u64 *pdpt = (u64 *)((pml4[pml4_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pdpt_i = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & 1)) return 0;
    if (pdpt[pdpt_i] & (1ULL << 7))
        return (pdpt[pdpt_i] & ~((1ULL<<30)-1)) | (virt & ((1ULL<<30)-1));
    u64 *pd = (u64 *)((pdpt[pdpt_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pd_i = (virt >> 21) & 0x1FF;
    if (!(pd[pd_i] & 1)) return 0;
    if (pd[pd_i] & (1ULL << 7))
        return (pd[pd_i] & ~((1ULL<<21)-1)) | (virt & ((1ULL<<21)-1));
    u64 *pt = (u64 *)((pd[pd_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & 1)) return 0;
    return (pt[pt_i] & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

u64 vmm_user_virt_to_phys(u64 virt) {
    u64 cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3)); cr3 &= ~0xFFFULL & ~0xFFFULL;  /* current user PML4 physical addr */
    u64 *pml4 = (u64 *)(cr3 + KERNEL_PHYS_BASE);

    int pml4_i = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & 1)) return 0;

    u64 *pdpt = (u64 *)((pml4[pml4_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pdpt_i = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & 1)) return 0;
    if (pdpt[pdpt_i] & (1ULL << 7))
        return (pdpt[pdpt_i] & ~((1ULL<<30)-1)) | (virt & ((1ULL<<30)-1));

    u64 *pd = (u64 *)((pdpt[pdpt_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pd_i = (virt >> 21) & 0x1FF;
    if (!(pd[pd_i] & 1)) return 0;
    if (pd[pd_i] & (1ULL << 7))
        return (pd[pd_i] & ~((1ULL<<21)-1)) | (virt & ((1ULL<<21)-1));

    u64 *pt = (u64 *)((pd[pd_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & 1)) return 0;

    return (pt[pt_i] & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

u64 vmm_virt_to_phys(u64 virt) {
    extern u64 *kernel_pml4;
    if (!kernel_pml4) return 0;

    u64 *pml4 = (u64 *)((u64)kernel_pml4 + KERNEL_PHYS_BASE);
    int pml4_i = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & 1)) return 0;

    u64 *pdpt = (u64 *)((pml4[pml4_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pdpt_i = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & 1)) return 0;
    if (pdpt[pdpt_i] & (1ULL << 7)) { /* 1GB page */
        return (pdpt[pdpt_i] & ~((1ULL<<30)-1)) | (virt & ((1ULL<<30)-1));
    }

    u64 *pd = (u64 *)((pdpt[pdpt_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pd_i = (virt >> 21) & 0x1FF;
    if (!(pd[pd_i] & 1)) return 0;
    if (pd[pd_i] & (1ULL << 7)) { /* 2MB page */
        return (pd[pd_i] & ~((1ULL<<21)-1)) | (virt & ((1ULL<<21)-1));
    }

    u64 *pt = (u64 *)((pd[pd_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pt_i = (virt >> 12) & 0x1FF;
    if (!(pt[pt_i] & 1)) return 0;

    /* Strip NX (bit 63) and other attribute bits; keep phys[51:12] */
    return (pt[pt_i] & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

void vmm_unmap_page(u64 virt) {
    extern u64 *kernel_pml4;
    if (!kernel_pml4) return;

    u64 *pml4 = (u64 *)((u64)kernel_pml4 + KERNEL_PHYS_BASE);
    int pml4_i = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_i] & 1)) return;

    u64 *pdpt = (u64 *)((pml4[pml4_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pdpt_i = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_i] & 1)) return;

    u64 *pd = (u64 *)((pdpt[pdpt_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pd_i = (virt >> 21) & 0x1FF;
    if (!(pd[pd_i] & 1)) return;

    u64 *pt = (u64 *)((pd[pd_i] & ~0xFFFULL) + KERNEL_PHYS_BASE);
    int pt_i = (virt >> 12) & 0x1FF;
    pt[pt_i] = 0;

    /* Flush TLB */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* vmm_alloc_pages — allocate and map a range of pages */
u64 vmm_alloc_pages(u64 virt, u64 size, u32 prot) {
    extern void vmm_map_page(u64 virt, u64 phys, u64 flags);

    u64 flags = PTE_PRESENT;
    if (prot & 0x08 /* VMM_PROT_USER */) flags |= PTE_USER;
    if (prot & 0x02 /* VMM_PROT_WRITE */) flags |= PTE_WRITE;
    if (!(prot & 0x04 /* VMM_PROT_EXEC */)) flags |= PTE_NX;

    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) return 0;
        if (virt + off == 0x603000 || virt + off == 0x604000) {
        }
        vmm_map_page(virt + off, phys, flags);
    }
    return virt;
}

/* =========================================================
 * PID management
 * ========================================================= */

/* PID bitmap — simple round-robin allocator */
#define MAX_PID         65536
static u64 pid_bitmap[MAX_PID / 64];
static spinlock_t pid_lock = { .raw_lock = {0} };
static pid_t pid_last = 1;

void pid_init(void) {
    memset(pid_bitmap, 0xFF, sizeof(pid_bitmap));  /* All free */
    /* Mark PID 0 as used */
    pid_bitmap[0] &= ~1ULL;
    spin_lock_init(&pid_lock);
}

pid_t pid_alloc(void) {
    spin_lock(&pid_lock);
    for (int tries = 0; tries < MAX_PID; tries++) {
        pid_last++;
        if (pid_last >= MAX_PID) pid_last = 1;
        int idx = pid_last / 64;
        int bit = pid_last % 64;
        if (pid_bitmap[idx] & (1ULL << bit)) {
            pid_bitmap[idx] &= ~(1ULL << bit);
            spin_unlock(&pid_lock);
            return (pid_t)pid_last;
        }
    }
    spin_unlock(&pid_lock);
    return -1;  /* No PIDs available */
}

void pid_free(pid_t pid) {
    if (pid <= 0 || pid >= MAX_PID) return;
    spin_lock(&pid_lock);
    pid_bitmap[pid / 64] |= (1ULL << (pid % 64));
    spin_unlock(&pid_lock);
}

/* =========================================================
 * do_mount / do_umount stubs (called by sys_mount wrapper)
 * ========================================================= */


/* =========================================================
 * kern_path_to_inode - simple path lookup returning inode
 * ========================================================= */
inode_t *kern_path_to_inode(const char *path) {
    if (!path) return NULL;
    extern int kern_path(const char *name, unsigned int flags, path_t *path);
    path_t p;
    int err = kern_path(path, LOOKUP_FOLLOW, &p);
    if (err < 0) return NULL;
    return p.dentry ? p.dentry->d_inode : NULL;
}

dentry_t *vfs_root_dentry(void) {
    extern dentry_t *root_dentry;
    return root_dentry;
}

/* ------------------------------------------------------------------ */
/*  vfs_mkdir_path — create a directory by absolute path               */
/* ------------------------------------------------------------------ */

int vfs_mkdir_path(const char *path, u32 mode)
{
    if (!path || !*path) return -EINVAL;
    /* Delegate to sys_mkdir which already handles VFS lookup */
    extern s64 sys_mkdir(const char *pathname, u32 mode);
    s64 ret = sys_mkdir(path, mode);
    /* EEXIST is fine — directory may already exist */
    if (ret == -EEXIST) return 0;
    return (int)ret;
}
