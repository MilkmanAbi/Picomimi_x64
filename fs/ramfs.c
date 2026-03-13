/**
 * Picomimi-x64 RAM Filesystem
 * 
 * In-memory filesystem for root and tmpfs
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// External functions

// ============================================================================
// RAMFS STRUCTURES
// ============================================================================

#define RAMFS_MAGIC 0x858458F6

typedef struct ramfs_inode {
    void            *data;          // File data
    size_t          size;           // Data size
    size_t          capacity;       // Allocated capacity
} ramfs_inode_t;

// ============================================================================
// INODE OPERATIONS
// ============================================================================

static struct dentry *ramfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    (void)dir;
    (void)flags;
    
    // ramfs doesn't have persistent storage, so lookup always fails
    // unless the dentry was already added via create/mkdir
    d_add(dentry, NULL);  // Negative dentry
    return NULL;
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, u32 mode, bool excl) {
    (void)excl;
    
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) {
        return -ENOMEM;
    }
    
    // Allocate ramfs-specific data
    ramfs_inode_t *ri = kmalloc(sizeof(ramfs_inode_t), GFP_KERNEL);
    if (!ri) {
        iput(inode);
        return -ENOMEM;
    }
    
    memset(ri, 0, sizeof(ramfs_inode_t));
    inode->i_private = ri;
    
    // Setup inode
    static unsigned long next_ino = 2;
    inode->i_ino = next_ino++;
    inode->i_mode = mode | S_IFREG;
    inode->i_uid = 0;
    inode->i_gid = 0;
    set_nlink(inode, 1);
    inode->i_size = 0;
    
    // Set file operations
    extern const file_operations_t ramfs_file_ops;
    inode->i_fop = &ramfs_file_ops;
    
    d_instantiate(dentry, inode);
    return 0;
}

static int ramfs_mkdir(struct inode *dir, struct dentry *dentry, u32 mode) {
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) {
        return -ENOMEM;
    }
    
    static unsigned long next_ino = 2;
    inode->i_ino = next_ino++;
    inode->i_mode = mode | S_IFDIR;
    inode->i_uid = 0;
    inode->i_gid = 0;
    set_nlink(inode, 2);  // . and parent
    
    inc_nlink(dir);  // For ..
    
    // Set directory operations
    extern const inode_operations_t ramfs_dir_inode_ops;
    extern const file_operations_t ramfs_dir_ops;
    inode->i_op = &ramfs_dir_inode_ops;
    inode->i_fop = &ramfs_dir_ops;
    
    d_instantiate(dentry, inode);
    return 0;
}

static int ramfs_rmdir(struct inode *dir, struct dentry *dentry) {
    if (!list_empty(&dentry->d_subdirs)) {
        return -ENOTEMPTY;
    }
    
    drop_nlink(dentry->d_inode);
    drop_nlink(dentry->d_inode);  // For .
    drop_nlink(dir);  // For ..
    
    return 0;
}

static int ramfs_unlink(struct inode *dir, struct dentry *dentry) {
    (void)dir;
    
    drop_nlink(dentry->d_inode);
    return 0;
}

static int ramfs_mknod(struct inode *dir, struct dentry *dentry, u32 mode, dev_t dev) {
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) {
        return -ENOMEM;
    }
    
    static unsigned long next_ino = 2;
    inode->i_ino = next_ino++;
    inode->i_mode = mode;
    inode->i_rdev = dev;
    inode->i_uid = 0;
    inode->i_gid = 0;
    set_nlink(inode, 1);
    
    d_instantiate(dentry, inode);
    return 0;
}

static int ramfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname) {
    inode_t *inode = new_inode(dir->i_sb);
    if (!inode) {
        return -ENOMEM;
    }
    
    size_t len = strlen(symname);
    char *link = kmalloc(len + 1, GFP_KERNEL);
    if (!link) {
        iput(inode);
        return -ENOMEM;
    }
    
    memcpy(link, symname, len + 1);
    inode->i_private = link;
    
    static unsigned long next_ino = 2;
    inode->i_ino = next_ino++;
    inode->i_mode = S_IFLNK | 0777;
    inode->i_size = len;
    set_nlink(inode, 1);
    
    d_instantiate(dentry, inode);
    return 0;
}

const inode_operations_t ramfs_dir_inode_ops = {
    .lookup     = ramfs_lookup,
    .create     = ramfs_create,
    .mkdir      = ramfs_mkdir,
    .rmdir      = ramfs_rmdir,
    .unlink     = ramfs_unlink,
    .mknod      = ramfs_mknod,
    .symlink    = ramfs_symlink,
};

// ============================================================================
// FILE OPERATIONS
// ============================================================================

static s64 ramfs_read(struct file *file, char *buf, size_t count, u64 *ppos) {
    inode_t *inode = file->f_inode;
    ramfs_inode_t *ri = inode->i_private;
    
    if (!ri || !ri->data) {
        return 0;
    }
    
    u64 pos = *ppos;
    
    if (pos >= ri->size) {
        return 0;
    }
    
    if (pos + count > ri->size) {
        count = ri->size - pos;
    }
    
    memcpy(buf, (char *)ri->data + pos, count);
    *ppos = pos + count;
    
    return count;
}

static s64 ramfs_write(struct file *file, const char *buf, size_t count, u64 *ppos) {
    inode_t *inode = file->f_inode;
    ramfs_inode_t *ri = inode->i_private;
    
    if (!ri) {
        return -EIO;
    }
    
    u64 pos = *ppos;
    u64 end = pos + count;
    
    // Grow buffer if needed
    if (end > ri->capacity) {
        size_t new_cap = end * 2;
        if (new_cap < 4096) new_cap = 4096;
        
        void *new_data = kmalloc(new_cap, GFP_KERNEL);
        if (!new_data) {
            return -ENOMEM;
        }
        
        if (ri->data) {
            memcpy(new_data, ri->data, ri->size);
            kfree(ri->data);
        }
        
        ri->data = new_data;
        ri->capacity = new_cap;
    }
    
    memcpy((char *)ri->data + pos, buf, count);
    
    if (end > ri->size) {
        ri->size = end;
        inode->i_size = end;
    }
    
    *ppos = end;
    mark_inode_dirty(inode);
    
    return count;
}

static s64 ramfs_llseek(struct file *file, s64 offset, int whence) {
    inode_t *inode = file->f_inode;
    s64 new_pos;
    
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = inode->i_size + offset;
        break;
    default:
        return -EINVAL;
    }
    
    if (new_pos < 0) {
        return -EINVAL;
    }
    
    file->f_pos = new_pos;
    return new_pos;
}

const file_operations_t ramfs_file_ops = {
    .read   = ramfs_read,
    .write  = ramfs_write,
    .llseek = ramfs_llseek,
};

// ============================================================================
// DIRECTORY OPERATIONS
// ============================================================================

static s64 ramfs_readdir(struct file *file, void *dirent, 
                         int (*filldir)(void *, const char *, int, u64, unsigned int)) {
    dentry_t *dentry = file->f_dentry;
    struct list_head *pos;

    /* f_pos encoding:
     *   0  = not yet emitted '.'
     *   1  = not yet emitted '..'
     *   2+ = child index (0-based) + 2, i.e. child 0 → f_pos=2, child 1 → f_pos=3 ...
     */

    if (file->f_pos == 0) {
        if (filldir(dirent, ".", 1, dentry->d_inode->i_ino, DT_DIR) < 0)
            return 0;
        file->f_pos = 1;
    }

    if (file->f_pos == 1) {
        u64 parent_ino = dentry->d_parent && dentry->d_parent->d_inode ?
                         dentry->d_parent->d_inode->i_ino :
                         dentry->d_inode->i_ino;
        if (filldir(dirent, "..", 2, parent_ino, DT_DIR) < 0)
            return 0;
        file->f_pos = 2;
    }

    /* Walk children; skip any we've already emitted (f_pos > child_index+2) */
    u64 child_idx = 0;
    list_for_each(pos, &dentry->d_subdirs) {
        dentry_t *child = list_entry(pos, dentry_t, d_child);

        if (child_idx + 2 < (u64)file->f_pos) {
            /* Already emitted in a previous getdents64 call */
            child_idx++;
            continue;
        }

        if (!child->d_inode) {
            /* Negative dentry — skip but advance position */
            child_idx++;
            file->f_pos++;
            continue;
        }

        unsigned int type = DT_UNKNOWN;
        u32 mode = child->d_inode->i_mode;
        if      (S_ISREG(mode))  type = DT_REG;
        else if (S_ISDIR(mode))  type = DT_DIR;
        else if (S_ISLNK(mode))  type = DT_LNK;
        else if (S_ISCHR(mode))  type = DT_CHR;
        else if (S_ISBLK(mode))  type = DT_BLK;
        else if (S_ISFIFO(mode)) type = DT_FIFO;
        else if (S_ISSOCK(mode)) type = DT_SOCK;

        if (filldir(dirent, (const char *)child->d_name.name, child->d_name.len,
                    child->d_inode->i_ino, type) < 0) {
            return 0;   /* Buffer full — caller will call us again with same f_pos */
        }

        child_idx++;
        file->f_pos++;
    }

    return 0;
}

const file_operations_t ramfs_dir_ops = {
    .readdir = ramfs_readdir,
};

// ============================================================================
// SUPERBLOCK OPERATIONS
// ============================================================================

static void ramfs_put_super(struct super_block *sb) {
    (void)sb;
    // Nothing to do for ramfs
}

static const super_operations_t ramfs_super_ops = {
    .put_super = ramfs_put_super,
};

// ============================================================================
// MOUNT
// ============================================================================

static dentry_t *ramfs_mount(file_system_type_t *fs_type, int flags,
                             const char *dev_name, void *data) {
    (void)dev_name;
    (void)data;
    (void)flags;
    
    // Allocate superblock
    super_block_t *sb = kmalloc(sizeof(super_block_t), GFP_KERNEL);
    if (!sb) {
        return ERR_PTR(-ENOMEM);
    }
    
    memset(sb, 0, sizeof(super_block_t));
    sb->s_blocksize = 4096;
    sb->s_blocksize_bits = 12;
    sb->s_magic = RAMFS_MAGIC;
    sb->s_type = fs_type;
    sb->s_op = &ramfs_super_ops;
    spin_lock_init(&sb->s_lock);
    INIT_LIST_HEAD(&sb->s_inodes);
    INIT_LIST_HEAD(&sb->s_dirty);
    INIT_LIST_HEAD(&sb->s_files);
    INIT_LIST_HEAD(&sb->s_mounts);
    atomic_set(&sb->s_active, 1);
    
    // Create root inode
    inode_t *root_inode = new_inode(sb);
    if (!root_inode) {
        kfree(sb);
        return ERR_PTR(-ENOMEM);
    }
    
    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_uid = 0;
    root_inode->i_gid = 0;
    set_nlink(root_inode, 2);
    root_inode->i_op = &ramfs_dir_inode_ops;
    root_inode->i_fop = &ramfs_dir_ops;
    
    // Create root dentry
    dentry_t *root = d_make_root(root_inode);
    if (!root) {
        iput(root_inode);
        kfree(sb);
        return ERR_PTR(-ENOMEM);
    }
    
    sb->s_root = root;
    
    return root;
}

static void ramfs_kill_sb(struct super_block *sb) {
    // TODO: Free all inodes and dentries
    kfree(sb);
}

// ============================================================================
// FILESYSTEM TYPE
// ============================================================================

file_system_type_t ramfs_fs_type = {
    .name       = "ramfs",
    .fs_flags   = 0,
    .mount      = ramfs_mount,
    .kill_sb    = ramfs_kill_sb,
};

file_system_type_t rootfs_fs_type = {
    .name       = "rootfs",
    .fs_flags   = 0,
    .mount      = ramfs_mount,
    .kill_sb    = ramfs_kill_sb,
};

// ============================================================================
// INIT
// ============================================================================

int init_rootfs(void) {
    int err;
    
    printk(KERN_INFO "VFS: Registering ramfs...\n");
    
    err = register_filesystem(&ramfs_fs_type);
    if (err) {
        return err;
    }
    
    err = register_filesystem(&rootfs_fs_type);
    if (err) {
        return err;
    }
    
    // Mount root filesystem
    printk(KERN_INFO "VFS: Mounting root filesystem...\n");
    err = do_mount(NULL, "/", "rootfs", 0, NULL);
    if (err) {
        printk(KERN_ERR "VFS: Failed to mount root: %d\n", err);
        return err;
    }
    
    printk(KERN_INFO "VFS: Root filesystem mounted\n");
    return 0;
}

/* =========================================================
 * ramfs_create_blob — create a file backed directly by a
 * kernel blob (no copy). The blob must remain valid forever
 * (e.g. in .rodata or BSS). Used for embedding binaries.
 * ========================================================= */
int ramfs_create_blob(const char *path, const void *data, size_t size, u32 mode) {
    /* Open (create) the file */
    extern s64 sys_open(const char *, int, u32);
    extern s64 sys_close(int);

    int fd = (int)sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return fd;

    /* Get the inode and wire up the data pointer directly */
    file_t *f = fget(fd);
    if (!f) { sys_close(fd); return -EBADF; }

    inode_t *inode = f->f_inode;
    if (inode && inode->i_private) {
        ramfs_inode_t *ri = inode->i_private;
        /* Free any existing buffer (no-op for bump allocator, but correct) */
        ri->data     = (void *)data;  /* point directly at blob */
        ri->size     = size;
        ri->capacity = size;
        inode->i_size = (off_t)size;
    }

    fput(f);
    sys_close(fd);
    return 0;
}
