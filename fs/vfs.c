/**
 * Picomimi-x64 Virtual File System Core
 * 
 * POSIX-compliant VFS layer
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// External spinlock functions
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);
extern void spin_lock_init(spinlock_t *lock);

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Registered filesystems
static file_system_type_t *file_systems = NULL;
static spinlock_t file_systems_lock = { .raw_lock = { 0 } };

// Dentry cache
struct list_head dentry_unused;
spinlock_t dcache_lock = { .raw_lock = { 0 } };
static u32 nr_dentry = 0;

// Inode cache
struct list_head inode_unused;
struct list_head inode_in_use;
spinlock_t inode_lock = { .raw_lock = { 0 } };
static u32 nr_inodes = 0;

// Mount table
static vfsmount_t *mount_table[MAX_MOUNTS];
static int nr_mounts = 0;
static spinlock_t mount_lock = { .raw_lock = { 0 } };

// Root filesystem
vfsmount_t *root_mnt = NULL;
dentry_t *root_dentry = NULL;

// ============================================================================
// FILESYSTEM REGISTRATION
// ============================================================================

int register_filesystem(file_system_type_t *fs) {
    if (!fs || !fs->name) {
        return -EINVAL;
    }
    
    spin_lock(&file_systems_lock);
    
    // Check for duplicate
    file_system_type_t *p = file_systems;
    while (p) {
        if (strcmp(p->name, fs->name) == 0) {
            spin_unlock(&file_systems_lock);
            return -EBUSY;
        }
        p = p->next;
    }
    
    // Add to list
    fs->next = file_systems;
    file_systems = fs;
    INIT_LIST_HEAD(&fs->fs_supers);
    
    spin_unlock(&file_systems_lock);
    
    printk(KERN_INFO "VFS: Registered filesystem '%s'\n", fs->name);
    return 0;
}

int unregister_filesystem(file_system_type_t *fs) {
    spin_lock(&file_systems_lock);
    
    file_system_type_t **p = &file_systems;
    while (*p) {
        if (*p == fs) {
            *p = fs->next;
            spin_unlock(&file_systems_lock);
            return 0;
        }
        p = &(*p)->next;
    }
    
    spin_unlock(&file_systems_lock);
    return -EINVAL;
}

file_system_type_t *get_fs_type(const char *name) {
    spin_lock(&file_systems_lock);
    
    file_system_type_t *fs = file_systems;
    while (fs) {
        if (strcmp(fs->name, name) == 0) {
            spin_unlock(&file_systems_lock);
            return fs;
        }
        fs = fs->next;
    }
    
    spin_unlock(&file_systems_lock);
    return NULL;
}

// ============================================================================
// INODE OPERATIONS
// ============================================================================

inode_t *new_inode(super_block_t *sb) {
    inode_t *inode = kmalloc(sizeof(inode_t), GFP_KERNEL);
    if (!inode) {
        return NULL;
    }
    
    memset(inode, 0, sizeof(inode_t));
    
    inode->i_sb = sb;
    atomic_set(&inode->i_count, 1);
    INIT_LIST_HEAD(&inode->i_lru);
    INIT_LIST_HEAD(&inode->i_sb_list);
    INIT_LIST_HEAD(&inode->i_dentry);
    spin_lock_init(&inode->i_lock);
    
    inode->i_state = I_NEW;
    
    // Add to superblock's inode list
    if (sb) {
        spin_lock(&sb->s_lock);
        list_add(&inode->i_sb_list, &sb->s_inodes);
        spin_unlock(&sb->s_lock);
    }
    
    spin_lock(&inode_lock);
    nr_inodes++;
    list_add(&inode->i_lru, &inode_in_use);
    spin_unlock(&inode_lock);
    
    return inode;
}

void ihold(inode_t *inode) {
    atomic_inc(&inode->i_count);
}

void iput(inode_t *inode) {
    if (!inode) return;
    
    if (atomic_dec_return(&inode->i_count) == 0) {
        // Move to unused list or free
        spin_lock(&inode_lock);
        list_move(&inode->i_lru, &inode_unused);
        spin_unlock(&inode_lock);
        
        // TODO: writeback if dirty
        // TODO: Actually free if no dentries reference it
    }
}

void unlock_new_inode(inode_t *inode) {
    inode->i_state &= ~I_NEW;
    // Wake up waiters
}

void clear_inode(inode_t *inode) {
    inode->i_state = I_CLEAR;
}

void set_nlink(inode_t *inode, unsigned int nlink) {
    inode->__i_nlink = nlink;
}

void inc_nlink(inode_t *inode) {
    inode->__i_nlink++;
}

void drop_nlink(inode_t *inode) {
    if (inode->__i_nlink > 0) {
        inode->__i_nlink--;
    }
}

void clear_nlink(inode_t *inode) {
    inode->__i_nlink = 0;
}

void mark_inode_dirty(inode_t *inode) {
    inode->i_state |= I_DIRTY;
}

void mark_inode_dirty_sync(inode_t *inode) {
    inode->i_state |= I_DIRTY_SYNC;
}

// ============================================================================
// DENTRY OPERATIONS
// ============================================================================

static unsigned int d_hash(const dentry_t *parent, const qstr_t *name) {
    unsigned int hash = 0;
    const unsigned char *p = name->name;
    
    while (*p) {
        hash = (hash << 5) + hash + *p++;
    }
    
    return hash ^ (unsigned long)parent;
}

dentry_t *d_alloc(dentry_t *parent, const qstr_t *name) {
    dentry_t *dentry = kmalloc(sizeof(dentry_t), GFP_KERNEL);
    if (!dentry) {
        return NULL;
    }
    
    memset(dentry, 0, sizeof(dentry_t));
    
    // Copy name
    if (name->len < sizeof(dentry->d_iname)) {
        memcpy(dentry->d_iname, name->name, name->len);
        dentry->d_iname[name->len] = '\0';
        dentry->d_name.name = dentry->d_iname;
    } else {
        char *newname = kmalloc(name->len + 1, GFP_KERNEL);
        if (!newname) {
            kfree(dentry);
            return NULL;
        }
        memcpy(newname, name->name, name->len);
        newname[name->len] = '\0';
        dentry->d_name.name = (const unsigned char *)newname;
    }
    
    dentry->d_name.len = name->len;
    dentry->d_name.hash = d_hash(parent, name);
    
    spin_lock_init(&dentry->d_lock);
    dentry->d_lockref.count = 1;
    
    INIT_LIST_HEAD(&dentry->d_lru);
    INIT_LIST_HEAD(&dentry->d_child);
    INIT_LIST_HEAD(&dentry->d_subdirs);
    
    dentry->d_parent = parent ? dget(parent) : dentry;
    
    if (parent) {
        dentry->d_sb = parent->d_sb;
        list_add(&dentry->d_child, &parent->d_subdirs);
    }
    
    spin_lock(&dcache_lock);
    nr_dentry++;
    spin_unlock(&dcache_lock);
    
    return dentry;
}

dentry_t *d_alloc_anon(super_block_t *sb) {
    qstr_t name = { .name = (unsigned char *)"/", .len = 1 };
    dentry_t *dentry = d_alloc(NULL, &name);
    if (dentry) {
        dentry->d_sb = sb;
    }
    return dentry;
}

dentry_t *d_make_root(inode_t *root_inode) {
    dentry_t *dentry = NULL;
    
    if (root_inode) {
        dentry = d_alloc_anon(root_inode->i_sb);
        if (dentry) {
            d_instantiate(dentry, root_inode);
        } else {
            iput(root_inode);
        }
    }
    
    return dentry;
}

void d_instantiate(dentry_t *dentry, inode_t *inode) {
    spin_lock(&dentry->d_lock);
    dentry->d_inode = inode;
    if (inode) {
        hlist_add_head(&dentry->d_alias, (struct hlist_head *)&inode->i_dentry);
    }
    spin_unlock(&dentry->d_lock);
}

void d_add(dentry_t *dentry, inode_t *inode) {
    d_instantiate(dentry, inode);
    d_rehash(dentry);
}

void d_rehash(dentry_t *dentry) {
    spin_lock(&dcache_lock);
    dentry->d_flags &= ~DCACHE_DENTRY_KILLED;
    // Add to hash table - simplified
    spin_unlock(&dcache_lock);
}

void d_drop(dentry_t *dentry) {
    spin_lock(&dcache_lock);
    dentry->d_flags |= DCACHE_DENTRY_KILLED;
    // Remove from hash
    spin_unlock(&dcache_lock);
}

void d_delete(dentry_t *dentry) {
    if (dentry->d_lockref.count == 1) {
        d_drop(dentry);
    }
}

dentry_t *dget(dentry_t *dentry) {
    if (dentry) {
        dentry->d_lockref.count++;
    }
    return dentry;
}

void dput(dentry_t *dentry) {
    if (!dentry) return;
    
    if (--dentry->d_lockref.count == 0) {
        // Move to unused list
        spin_lock(&dcache_lock);
        list_add(&dentry->d_lru, &dentry_unused);
        spin_unlock(&dcache_lock);
    }
}

dentry_t *d_lookup(const dentry_t *parent, const qstr_t *name) {
    struct list_head *pos;
    
    spin_lock(&((dentry_t *)parent)->d_lock);
    
    list_for_each(pos, &parent->d_subdirs) {
        dentry_t *child = list_entry(pos, dentry_t, d_child);
        
        if (child->d_name.len == name->len &&
            memcmp(child->d_name.name, name->name, name->len) == 0) {
            dget(child);
            spin_unlock(&((dentry_t *)parent)->d_lock);
            return child;
        }
    }
    
    spin_unlock(&((dentry_t *)parent)->d_lock);
    return NULL;
}

// Build path from dentry
char *d_path(const path_t *path, char *buf, int buflen) {
    char *end = buf + buflen;
    char *start = end;
    dentry_t *dentry = path->dentry;
    
    *--start = '\0';
    
    while (dentry != dentry->d_parent) {
        int len = dentry->d_name.len;
        
        start -= len;
        if (start < buf) {
            return NULL;  // Buffer too small
        }
        
        memcpy(start, dentry->d_name.name, len);
        *--start = '/';
        
        dentry = dentry->d_parent;
    }
    
    if (start == end - 1) {
        *--start = '/';
    }
    
    return start;
}

// ============================================================================
// PATH RESOLUTION
// ============================================================================

// Simple path lookup
static dentry_t *lookup_one(dentry_t *dir, const char *name, int len) {
    qstr_t qname;
    qname.name = (const unsigned char *)name;
    qname.len = len;
    qname.hash = 0;
    
    // Check dcache first
    dentry_t *dentry = d_lookup(dir, &qname);
    if (dentry) {
        return dentry;
    }
    
    // Create new dentry
    dentry = d_alloc(dir, &qname);
    if (!dentry) {
        return ERR_PTR(-ENOMEM);
    }
    
    // Lookup via filesystem
    if (dir->d_inode && dir->d_inode->i_op && dir->d_inode->i_op->lookup) {
        dentry_t *result = dir->d_inode->i_op->lookup(dir->d_inode, dentry, 0);
        if (IS_ERR(result)) {
            dput(dentry);
            return result;
        }
        if (result) {
            dput(dentry);
            return result;
        }
    }
    
    // Negative dentry (file doesn't exist)
    if (!dentry->d_inode) {
        return ERR_PTR(-ENOENT);
    }
    
    return dentry;
}

int kern_path(const char *name, unsigned int flags, path_t *path) {
    if (!name || !path || !root_mnt) {
        return -EINVAL;
    }
    
    dentry_t *dentry;
    
    // Start from root or cwd
    if (name[0] == '/') {
        dentry = dget(root_dentry);
        name++;
    } else {
        // TODO: Use current->fs->pwd
        dentry = dget(root_dentry);
    }
    
    // Walk path components
    while (*name) {
        // Skip slashes
        while (*name == '/') name++;
        if (!*name) break;
        
        // Find end of component
        const char *next = name;
        while (*next && *next != '/') next++;
        int len = next - name;
        
        // Handle . and ..
        if (len == 1 && name[0] == '.') {
            name = next;
            continue;
        }
        
        if (len == 2 && name[0] == '.' && name[1] == '.') {
            if (dentry != root_dentry) {
                dentry_t *parent = dget(dentry->d_parent);
                dput(dentry);
                dentry = parent;
            }
            name = next;
            continue;
        }
        
        // Lookup component
        dentry_t *next_dentry = lookup_one(dentry, name, len);
        dput(dentry);
        
        if (IS_ERR(next_dentry)) {
            return PTR_ERR(next_dentry);
        }
        
        dentry = next_dentry;
        name = next;
    }
    
    path->dentry = dentry;
    path->mnt = root_mnt;
    
    // Handle LOOKUP_FOLLOW for symlinks
    if ((flags & LOOKUP_FOLLOW) && dentry->d_inode && 
        S_ISLNK(dentry->d_inode->i_mode)) {
        // TODO: Follow symlink
    }
    
    return 0;
}

void path_get(const path_t *path) {
    if (path->dentry) {
        dget(path->dentry);
    }
    // TODO: mntget
}

void path_put(const path_t *path) {
    if (path->dentry) {
        dput(path->dentry);
    }
    // TODO: mntput
}

// ============================================================================
// FILE OPERATIONS
// ============================================================================

file_t *get_empty_filp(void) {
    file_t *f = kmalloc(sizeof(file_t), GFP_KERNEL);
    if (!f) {
        return NULL;
    }
    
    memset(f, 0, sizeof(file_t));
    atomic_set(&f->f_count, 1);
    spin_lock_init(&f->f_lock);
    
    return f;
}

file_t *filp_open(const char *filename, int flags, u32 mode) {
    path_t path;
    int err;
    
    // Lookup path
    err = kern_path(filename, LOOKUP_FOLLOW, &path);
    
    // Handle O_CREAT
    if (err == -ENOENT && (flags & O_CREAT)) {
        // TODO: Create file
        return ERR_PTR(-ENOSYS);
    }
    
    if (err) {
        return ERR_PTR(err);
    }
    
    // Check permissions
    inode_t *inode = path.dentry->d_inode;
    if (!inode) {
        path_put(&path);
        return ERR_PTR(-ENOENT);
    }
    
    // Check if it's a directory and O_DIRECTORY wasn't specified
    if (S_ISDIR(inode->i_mode) && !(flags & O_DIRECTORY)) {
        if ((flags & O_ACCMODE) != O_RDONLY) {
            path_put(&path);
            return ERR_PTR(-EISDIR);
        }
    }
    
    // Allocate file structure
    file_t *f = get_empty_filp();
    if (!f) {
        path_put(&path);
        return ERR_PTR(-ENOMEM);
    }
    
    f->f_dentry = path.dentry;
    f->f_inode = inode;
    f->f_flags = flags;
    f->f_mode = mode;
    f->f_pos = 0;
    f->f_op = inode->i_fop;
    
    // Call open if defined
    if (f->f_op && f->f_op->open) {
        err = f->f_op->open(inode, f);
        if (err) {
            kfree(f);
            path_put(&path);
            return ERR_PTR(err);
        }
    }
    
    return f;
}

void filp_close(file_t *filp, void *id) {
    (void)id;
    
    if (!filp) return;
    
    if (filp->f_op && filp->f_op->release) {
        filp->f_op->release(filp->f_inode, filp);
    }
    
    if (filp->f_dentry) {
        dput(filp->f_dentry);
    }
    
    kfree(filp);
}

s64 vfs_read(file_t *file, char *buf, size_t count, s64 *pos) {
    if (!file || !file->f_op || !file->f_op->read) {
        return -EINVAL;
    }
    
    if (!(file->f_mode & FMODE_READ)) {
        return -EBADF;
    }
    
    return file->f_op->read(file, buf, count, (u64 *)pos);
}

s64 vfs_write(file_t *file, const char *buf, size_t count, s64 *pos) {
    if (!file || !file->f_op || !file->f_op->write) {
        return -EINVAL;
    }
    
    if (!(file->f_mode & FMODE_WRITE)) {
        return -EBADF;
    }
    
    return file->f_op->write(file, buf, count, (u64 *)pos);
}

s64 vfs_llseek(file_t *file, s64 offset, int whence) {
    if (!file) {
        return -EBADF;
    }
    
    if (file->f_op && file->f_op->llseek) {
        return file->f_op->llseek(file, offset, whence);
    }
    
    // Default implementation
    s64 new_pos;
    
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        if (file->f_inode) {
            new_pos = file->f_inode->i_size + offset;
        } else {
            return -EINVAL;
        }
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

// ============================================================================
// MOUNT OPERATIONS
// ============================================================================

int do_mount(const char *dev_name, const char *dir_name, const char *type_page,
             unsigned long flags, void *data) {
    file_system_type_t *fs_type;
    dentry_t *mountpoint;
    
    // Find filesystem type
    fs_type = get_fs_type(type_page);
    if (!fs_type) {
        printk(KERN_ERR "VFS: Unknown filesystem type '%s'\n", type_page);
        return -ENODEV;
    }
    
    // Lookup mount point
    if (root_mnt) {
        path_t path;
        int err = kern_path(dir_name, LOOKUP_FOLLOW, &path);
        if (err) {
            return err;
        }
        mountpoint = path.dentry;
    } else {
        mountpoint = NULL;
    }
    
    // Call filesystem mount
    dentry_t *root = fs_type->mount(fs_type, flags, dev_name, data);
    if (IS_ERR(root)) {
        if (mountpoint) dput(mountpoint);
        return PTR_ERR(root);
    }
    
    // Create mount structure
    vfsmount_t *mnt = kmalloc(sizeof(vfsmount_t), GFP_KERNEL);
    if (!mnt) {
        if (mountpoint) dput(mountpoint);
        return -ENOMEM;
    }
    
    memset(mnt, 0, sizeof(vfsmount_t));
    mnt->mnt_root = root;
    mnt->mnt_sb = root->d_sb;
    mnt->mnt_flags = flags;
    mnt->mnt_devname = dev_name;
    mnt->mnt_mountpoint = mountpoint;
    atomic_set(&mnt->mnt_count, 1);
    INIT_LIST_HEAD(&mnt->mnt_mounts);
    INIT_LIST_HEAD(&mnt->mnt_child);
    INIT_LIST_HEAD(&mnt->mnt_instance);
    
    // Add to mount table
    spin_lock(&mount_lock);
    if (nr_mounts < MAX_MOUNTS) {
        mount_table[nr_mounts++] = mnt;
    }
    spin_unlock(&mount_lock);
    
    // Set as root if first mount
    if (!root_mnt) {
        root_mnt = mnt;
        root_dentry = root;
    }
    
    printk(KERN_INFO "VFS: Mounted '%s' on '%s' type %s\n", 
           dev_name ? dev_name : "none", dir_name, type_page);
    
    return 0;
}

// ============================================================================
// VFS INITIALIZATION
// ============================================================================

void vfs_init(void) {
    printk(KERN_INFO "VFS: Initializing virtual filesystem...\n");
    
    // Initialize caches
    INIT_LIST_HEAD(&dentry_unused);
    INIT_LIST_HEAD(&inode_unused);
    INIT_LIST_HEAD(&inode_in_use);
    
    spin_lock_init(&dcache_lock);
    spin_lock_init(&inode_lock);
    spin_lock_init(&mount_lock);
    spin_lock_init(&file_systems_lock);
    
    printk(KERN_INFO "VFS: Initialized\n");
}
