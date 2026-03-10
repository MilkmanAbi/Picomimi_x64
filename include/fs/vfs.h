/**
 * Picomimi-x64 Virtual File System (VFS)
 * 
 * POSIX-compliant VFS layer supporting multiple filesystems
 */
#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <kernel/types.h>

// ============================================================================
// LIMITS
// ============================================================================

#define NAME_MAX            255
#define PATH_MAX            4096
#define MAX_SYMLINK_DEPTH   8
#define MAX_MOUNTS          256
#define MAX_FILESYSTEMS     32

// File mode for file struct
#ifndef FMODE_READ
#define FMODE_READ          0x1
#define FMODE_WRITE         0x2
#define FMODE_EXEC          0x4
#endif

// File type and mode bits (POSIX)
#define S_IFMT          0170000
#define S_IFSOCK        0140000
#define S_IFLNK         0120000
#define S_IFREG         0100000
#define S_IFBLK         0060000
#define S_IFDIR         0040000
#define S_IFCHR         0020000
#define S_IFIFO         0010000
#define S_ISUID         0004000
#define S_ISGID         0002000
#define S_ISVTX         0001000

#define S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)      (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)      (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)     (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)      (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m)     (((m) & S_IFMT) == S_IFSOCK)

// Directory entry types
#define DT_UNKNOWN      0
#define DT_FIFO         1
#define DT_CHR          2
#define DT_DIR          4
#define DT_BLK          6
#define DT_REG          8
#define DT_LNK          10
#define DT_SOCK         12

// Seek constants
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

// Open flags
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_NOCTTY        0x0100
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_CLOEXEC       0x80000

// Forward declare qstr before use
typedef struct qstr {
    union {
        struct {
            u32 hash;
            u32 len;
        };
        u64 hash_len;
    };
    const unsigned char *name;
} qstr_t;

// Forward declare stat (use linux_stat to avoid conflicts)
struct linux_stat;

// Atomic and spinlock operations are provided by <kernel/types.h>

// ============================================================================
// FILE STRUCTURE (Open file)
// ============================================================================

// Forward declarations for file operations
struct file;
struct inode;
struct dentry;

// File operations
#ifndef _STRUCT_FILE_OPERATIONS
#define _STRUCT_FILE_OPERATIONS
typedef struct file_operations {
    s64 (*read)(struct file *, char *, size_t, u64 *);
    s64 (*write)(struct file *, const char *, size_t, u64 *);
    s64 (*llseek)(struct file *, s64, int);
    s64 (*ioctl)(struct file *, unsigned int, unsigned long);
    s64 (*mmap)(struct file *, void *);
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    int (*fsync)(struct file *);
    s64 (*readdir)(struct file *, void *, int (*)(void *, const char *, int, u64, unsigned int));
    unsigned int (*poll)(struct file *, void *);
} file_operations_t;
#endif

// Open file
#ifndef _STRUCT_FILE
#define _STRUCT_FILE
typedef struct file {
    struct dentry       *f_dentry;      // Associated dentry
    struct inode        *f_inode;       // Cached inode
    const file_operations_t *f_op;      // Operations
    
    u32                 f_flags;        // Open flags
    u32                 f_mode;         // File mode
    u64                 f_pos;          // Current position
    
    atomic_t            f_count;        // Reference count
    spinlock_t          f_lock;
    
    void                *private_data;  // Driver private data
} file_t;
#endif

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

struct super_block;
struct vfsmount;
struct path;
struct nameidata;

// ============================================================================
// SUPERBLOCK
// ============================================================================

// Superblock operations
typedef struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *);
    void (*free_inode)(struct inode *);
    
    void (*dirty_inode)(struct inode *, int flags);
    int (*write_inode)(struct inode *, int sync);
    void (*drop_inode)(struct inode *);
    void (*evict_inode)(struct inode *);
    
    void (*put_super)(struct super_block *);
    int (*sync_fs)(struct super_block *sb, int wait);
    int (*freeze_fs)(struct super_block *);
    int (*unfreeze_fs)(struct super_block *);
    int (*statfs)(struct dentry *, struct statfs *);
    int (*remount_fs)(struct super_block *, int *, char *);
    void (*umount_begin)(struct super_block *);
    
    int (*show_options)(void *, struct dentry *);
} super_operations_t;

// Superblock (mounted filesystem)
typedef struct super_block {
    struct list_head    s_list;         // Keep this first
    dev_t               s_dev;          // Device identifier
    unsigned char       s_blocksize_bits;
    unsigned long       s_blocksize;
    u64                 s_maxbytes;     // Max file size
    
    struct file_system_type *s_type;
    const super_operations_t *s_op;
    
    unsigned long       s_flags;        // Mount flags
    unsigned long       s_magic;        // Filesystem magic number
    struct dentry       *s_root;        // Root dentry
    
    struct rw_semaphore s_umount;
    int                 s_count;
    atomic_t            s_active;
    
    // Filesystem specific
    void                *s_fs_info;
    
    // Dirty inodes
    struct list_head    s_inodes;       // All inodes
    struct list_head    s_dirty;        // Dirty inodes
    struct list_head    s_files;        // Open files
    
    // Mountpoint info
    struct list_head    s_mounts;       // List of mounts
    
    char                s_id[32];       // Informational name
    u8                  s_uuid[16];     // UUID
    
    // Superblock lock
    spinlock_t          s_lock;
} super_block_t;

// ============================================================================
// INODE
// ============================================================================

// Inode operations
typedef struct inode_operations {
    struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
    const char *(*get_link)(struct dentry *, struct inode *, void **);
    
    int (*permission)(struct inode *, int);
    
    int (*create)(struct inode *, struct dentry *, u32, bool);
    int (*link)(struct dentry *, struct inode *, struct dentry *);
    int (*unlink)(struct inode *, struct dentry *);
    int (*readlink)(struct dentry *, char *, int);
    int (*symlink)(struct inode *, struct dentry *, const char *);
    int (*mkdir)(struct inode *, struct dentry *, u32);
    int (*rmdir)(struct inode *, struct dentry *);
    int (*mknod)(struct inode *, struct dentry *, u32, dev_t);
    int (*rename)(struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned int);
    
    int (*setattr)(struct dentry *, struct iattr *);
    int (*getattr)(const struct path *, void *, u32, unsigned int);
    
    s64 (*listxattr)(struct dentry *, char *, size_t);
    
    int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start, u64 len);
    int (*update_time)(struct inode *, struct timespec64 *, int);
    int (*atomic_open)(struct inode *, struct dentry *, struct file *, unsigned open_flag, u32 create_mode);
    int (*tmpfile)(struct inode *, struct dentry *, u32);
} inode_operations_t;

// Inode (in-memory file/directory representation)
typedef struct inode {
    u32                 i_mode;         // File mode
    u16                 i_opflags;
    uid_t               i_uid;
    gid_t               i_gid;
    unsigned int        i_flags;
    
    const inode_operations_t *i_op;
    super_block_t       *i_sb;
    
    // File data mapping
    // struct address_space *i_mapping;
    // struct address_space i_data;
    
    unsigned long       i_ino;          // Inode number
    
    union {
        const unsigned int i_nlink;     // Hard link count
        unsigned int __i_nlink;
    };
    
    dev_t               i_rdev;         // Device (if special)
    s64                 i_size;         // Size in bytes
    
    struct timespec64   i_atime;        // Access time
    struct timespec64   i_mtime;        // Modify time
    struct timespec64   i_ctime;        // Change time
    
    spinlock_t          i_lock;
    
    unsigned short      i_bytes;        // Bytes in last block
    u8                  i_blkbits;      // Block size bits
    blkcnt_t            i_blocks;       // Block count
    
    unsigned long       i_state;        // State flags
    struct rw_semaphore i_rwsem;
    
    struct list_head    i_lru;          // LRU list
    struct list_head    i_sb_list;      // Per-superblock list
    
    union {
        struct list_head i_dentry;      // Dentries pointing to this
        // struct rcu_head i_rcu;
    };
    
    atomic64_t          i_version;
    atomic_t            i_count;        // Reference count
    atomic_t            i_writecount;   // Write count
    
    // File operations (for regular files)
    const struct file_operations *i_fop;
    
    // Private data
    void                *i_private;
} inode_t;

// Inode state flags
#define I_DIRTY_SYNC        (1 << 0)
#define I_DIRTY_DATASYNC    (1 << 1)
#define I_DIRTY_PAGES       (1 << 2)
#define I_NEW               (1 << 3)
#define I_WILL_FREE         (1 << 4)
#define I_FREEING           (1 << 5)
#define I_CLEAR             (1 << 6)
#define I_SYNC              (1 << 7)
#define I_REFERENCED        (1 << 8)

#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)

// ============================================================================
// DENTRY (Directory Entry Cache)
// ============================================================================

// Dentry operations
typedef struct dentry_operations {
    int (*d_revalidate)(struct dentry *, unsigned int);
    int (*d_weak_revalidate)(struct dentry *, unsigned int);
    int (*d_hash)(const struct dentry *, struct qstr *);
    int (*d_compare)(const struct dentry *, unsigned int, const char *, const struct qstr *);
    int (*d_delete)(const struct dentry *);
    int (*d_init)(struct dentry *);
    void (*d_release)(struct dentry *);
    void (*d_prune)(struct dentry *);
    void (*d_iput)(struct dentry *, struct inode *);
    char *(*d_dname)(struct dentry *, char *, int);
    // struct vfsmount *(*d_automount)(struct path *);
    int (*d_manage)(const struct path *, bool);
    struct dentry *(*d_real)(struct dentry *, const struct inode *);
} dentry_operations_t;

// Dentry (cached directory entry)
typedef struct dentry {
    unsigned int        d_flags;
    // seqcount_spinlock_t d_seq;
    struct hlist_node   d_hash;         // Lookup hash list
    struct dentry       *d_parent;      // Parent directory
    qstr_t              d_name;         // Name
    struct inode        *d_inode;       // Associated inode
    unsigned char       d_iname[32];    // Short name inline
    
    spinlock_t          d_lock;
    const dentry_operations_t *d_op;
    super_block_t       *d_sb;
    unsigned long       d_time;
    void                *d_fsdata;
    
    struct lockref      d_lockref;      // Per-dentry lock and refcount
    
    union {
        struct list_head d_lru;         // LRU list
        // wait_queue_head_t *d_wait;
    };
    
    struct list_head    d_child;        // Child of parent list
    struct list_head    d_subdirs;      // Our children
    
    union {
        struct hlist_node d_alias;      // Inode alias list
        struct hlist_head d_children;   // For lookup hashing
        // struct rcu_head d_rcu;
    };
} dentry_t;

// Dentry flags
#define DCACHE_OP_HASH          0x00000001
#define DCACHE_OP_COMPARE       0x00000002
#define DCACHE_OP_REVALIDATE    0x00000004
#define DCACHE_OP_DELETE        0x00000008
#define DCACHE_OP_PRUNE         0x00000010
#define DCACHE_DISCONNECTED     0x00000020
#define DCACHE_REFERENCED       0x00000040
#define DCACHE_RCUACCESS        0x00000080
#define DCACHE_CANT_MOUNT       0x00000100
#define DCACHE_GENOCIDE         0x00000200
#define DCACHE_SHRINK_LIST      0x00000400
#define DCACHE_OP_WEAK_REVALIDATE   0x00000800
#define DCACHE_NFSFS_RENAMED    0x00001000
#define DCACHE_COOKIE           0x00002000
#define DCACHE_FSNOTIFY_PARENT_WATCHED  0x00004000
#define DCACHE_DENTRY_KILLED    0x00008000
#define DCACHE_MOUNTED          0x00010000
#define DCACHE_NEED_AUTOMOUNT   0x00020000
#define DCACHE_MANAGE_TRANSIT   0x00040000
#define DCACHE_MANAGED_DENTRY   (DCACHE_MOUNTED|DCACHE_NEED_AUTOMOUNT|DCACHE_MANAGE_TRANSIT)
#define DCACHE_LRU_LIST         0x00080000
#define DCACHE_ENTRY_TYPE       0x00700000
#define DCACHE_MISS_TYPE        0x00000000
#define DCACHE_WHITEOUT_TYPE    0x00100000
#define DCACHE_DIRECTORY_TYPE   0x00200000
#define DCACHE_AUTODIR_TYPE     0x00300000
#define DCACHE_REGULAR_TYPE     0x00400000
#define DCACHE_SPECIAL_TYPE     0x00500000
#define DCACHE_SYMLINK_TYPE     0x00600000
#define DCACHE_MAY_FREE         0x00800000
#define DCACHE_FALLTHRU         0x01000000
#define DCACHE_NOKEY_NAME       0x02000000
#define DCACHE_OP_REAL          0x04000000
#define DCACHE_PAR_LOOKUP       0x10000000
#define DCACHE_DENTRY_CURSOR    0x20000000
#define DCACHE_NORCU            0x40000000

// ============================================================================
// PATH & NAMEIDATA (Path Resolution)
// ============================================================================

typedef struct path {
    struct vfsmount     *mnt;
    dentry_t            *dentry;
} path_t;

typedef struct nameidata {
    path_t              path;
    qstr_t              last;
    path_t              root;
    inode_t             *inode;
    unsigned int        flags;
    unsigned int        seq;
    unsigned int        m_seq;
    int                 last_type;
    unsigned int        depth;
    int                 total_link_count;
    struct saved {
        path_t          link;
        const char      *name;
        unsigned int    seq;
    } *stack, internal[2];
    // struct filename   *name;
    // struct nameidata  *saved;
    // struct inode      *link_inode;
    unsigned int        root_seq;
    int                 dfd;
} nameidata_t;

// Nameidata flags
#define LOOKUP_FOLLOW       0x0001
#define LOOKUP_DIRECTORY    0x0002
#define LOOKUP_AUTOMOUNT    0x0004
#define LOOKUP_PARENT       0x0010
#define LOOKUP_REVAL        0x0020
#define LOOKUP_RCU          0x0040
#define LOOKUP_NO_REVAL     0x0080
#define LOOKUP_OPEN         0x0100
#define LOOKUP_CREATE       0x0200
#define LOOKUP_EXCL         0x0400
#define LOOKUP_RENAME_TARGET    0x0800
#define LOOKUP_JUMPED       0x1000
#define LOOKUP_ROOT         0x2000
#define LOOKUP_EMPTY        0x4000
#define LOOKUP_DOWN         0x8000
#define LOOKUP_MOUNTPOINT   0x0080

// ============================================================================
// VFSMOUNT (Mount Point)
// ============================================================================

typedef struct vfsmount {
    dentry_t            *mnt_root;      // Root of mounted tree
    super_block_t       *mnt_sb;        // Pointer to superblock
    int                 mnt_flags;      // Mount flags
    // void              *mnt_data;
    
    // For mount tree
    struct list_head    mnt_mounts;     // List of mounts
    struct list_head    mnt_child;      // Child mount
    struct list_head    mnt_instance;   // Mount instance list
    
    const char          *mnt_devname;   // Device name
    struct vfsmount     *mnt_parent;    // Parent mount
    dentry_t            *mnt_mountpoint; // Where mounted
    
    atomic_t            mnt_count;      // Reference count
} vfsmount_t;

// Mount flags
#define MNT_NOSUID      0x01
#define MNT_NODEV       0x02
#define MNT_NOEXEC      0x04
#define MNT_NOATIME     0x08
#define MNT_NODIRATIME  0x10
#define MNT_RELATIME    0x20
#define MNT_READONLY    0x40
#define MNT_SHRINKABLE  0x100
#define MNT_WRITE_HOLD  0x200
#define MNT_SHARED      0x1000
#define MNT_UNBINDABLE  0x2000

// ============================================================================
// FILE SYSTEM TYPE
// ============================================================================

typedef struct file_system_type {
    const char          *name;
    int                 fs_flags;
    
    // Mount operations
    struct dentry *(*mount)(struct file_system_type *, int, const char *, void *);
    void (*kill_sb)(struct super_block *);
    
    // Module owner
    // struct module *owner;
    
    struct file_system_type *next;
    struct list_head    fs_supers;
    
    // Lockdep stuff
    // struct lock_class_key s_lock_key;
    // struct lock_class_key s_umount_key;
    // struct lock_class_key i_lock_key;
    // struct lock_class_key i_mutex_key;
    // struct lock_class_key i_mutex_dir_key;
} file_system_type_t;

// Filesystem flags
#define FS_REQUIRES_DEV     1
#define FS_BINARY_MOUNTDATA 2
#define FS_HAS_SUBTYPE      4
#define FS_USERNS_MOUNT     8
#define FS_DISALLOW_NOTIFY_PERM 16
#define FS_RENAME_DOES_D_MOVE   32768

// ============================================================================
// VFS OPERATIONS
// ============================================================================

// Filesystem registration
int register_filesystem(file_system_type_t *fs);
int unregister_filesystem(file_system_type_t *fs);
file_system_type_t *get_fs_type(const char *name);

// Superblock operations
super_block_t *sget(file_system_type_t *type, int (*test)(super_block_t *, void *), int (*set)(super_block_t *, void *), int flags, void *data);
void deactivate_super(super_block_t *s);
void drop_super(super_block_t *sb);

// Inode operations
inode_t *new_inode(super_block_t *sb);
inode_t *iget_locked(super_block_t *sb, unsigned long ino);
void iget_failed(inode_t *inode);
void unlock_new_inode(inode_t *inode);
void iput(inode_t *inode);
void ihold(inode_t *inode);
void clear_inode(inode_t *inode);
void set_nlink(inode_t *inode, unsigned int nlink);
void inc_nlink(inode_t *inode);
void drop_nlink(inode_t *inode);
void clear_nlink(inode_t *inode);
void mark_inode_dirty(inode_t *inode);
void mark_inode_dirty_sync(inode_t *inode);

// Dentry operations  
dentry_t *d_alloc(dentry_t *parent, const qstr_t *name);
dentry_t *d_alloc_anon(super_block_t *sb);
dentry_t *d_make_root(inode_t *root_inode);
void d_instantiate(dentry_t *dentry, inode_t *inode);
dentry_t *d_instantiate_unique(dentry_t *entry, inode_t *inode);
dentry_t *d_instantiate_anon(dentry_t *dentry, inode_t *inode);
void d_add(dentry_t *dentry, inode_t *inode);
void d_delete(dentry_t *dentry);
void d_drop(dentry_t *dentry);
void dput(dentry_t *dentry);
dentry_t *dget(dentry_t *dentry);
dentry_t *d_lookup(const dentry_t *parent, const qstr_t *name);
dentry_t *d_hash_and_lookup(dentry_t *dir, qstr_t *name);
void d_rehash(dentry_t *dentry);
void d_move(dentry_t *dentry, dentry_t *target);
void d_invalidate(dentry_t *dentry);
char *d_path(const path_t *path, char *buf, int buflen);
char *dentry_path_raw(dentry_t *dentry, char *buf, int buflen);

// Path resolution
int kern_path(const char *name, unsigned int flags, path_t *path);
int vfs_path_lookup(dentry_t *dentry, vfsmount_t *mnt, const char *name, unsigned int flags, path_t *path);
dentry_t *lookup_one_len(const char *name, dentry_t *base, int len);
dentry_t *lookup_positive_unlocked(const char *name, dentry_t *base, int len);
void path_get(const path_t *path);
void path_put(const path_t *path);

// Mount operations
int do_mount(const char *dev_name, const char *dir_name, const char *type_page, unsigned long flags, void *data_page);
int do_umount(vfsmount_t *mnt, int flags);
vfsmount_t *lookup_mnt(const path_t *path);

// File operations
file_t *filp_open(const char *filename, int flags, u32 mode);
file_t *file_open_name(void *name, int flags, u32 mode);
file_t *file_open_root(dentry_t *dentry, vfsmount_t *mnt, const char *filename, int flags, u32 mode);
void filp_close(file_t *filp, void *id);
file_t *fget(unsigned int fd);
file_t *fget_raw(unsigned int fd);
file_t *fget_light(unsigned int fd, int *fput_needed);
void fput(file_t *file);
s64 vfs_read(file_t *file, char *buf, size_t count, s64 *pos);
s64 vfs_write(file_t *file, const char *buf, size_t count, s64 *pos);
s64 vfs_llseek(file_t *file, s64 offset, int whence);
s64 vfs_ioctl(file_t *file, unsigned int cmd, unsigned long arg);
int vfs_fsync(file_t *file, int datasync);
int vfs_fstat(int fd, void *stat);
int vfs_stat(const char *name, void *stat);
int vfs_lstat(const char *name, void *stat);
int vfs_fstatat(int dfd, const char *name, void *stat, int flag);

// Directory operations
int vfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode);
int vfs_rmdir(inode_t *dir, dentry_t *dentry);
int vfs_mknod(inode_t *dir, dentry_t *dentry, u32 mode, dev_t dev);
int vfs_create(inode_t *dir, dentry_t *dentry, u32 mode, bool want_excl);
int vfs_link(dentry_t *old_dentry, inode_t *dir, dentry_t *new_dentry, inode_t **delegated_inode);
int vfs_unlink(inode_t *dir, dentry_t *dentry, inode_t **delegated_inode);
int vfs_symlink(inode_t *dir, dentry_t *dentry, const char *oldname);
int vfs_rename(inode_t *old_dir, dentry_t *old_dentry, inode_t *new_dir, dentry_t *new_dentry, inode_t **delegated_inode, unsigned int flags);
s64 vfs_readlink(dentry_t *dentry, char *buffer, int buflen);
const char *vfs_get_link(dentry_t *dentry, inode_t *inode, void **cookie);

// Permission checking
int inode_permission(inode_t *inode, int mask);
int generic_permission(inode_t *inode, int mask);

// ============================================================================
// BUILT-IN FILESYSTEMS
// ============================================================================

// Root filesystem (ramfs/tmpfs style)
extern file_system_type_t rootfs_fs_type;
int init_rootfs(void);

// Device filesystem (devfs/devtmpfs)
extern file_system_type_t devfs_fs_type;
int init_devfs(void);

// Proc filesystem
extern file_system_type_t procfs_fs_type;
int init_procfs(void);

// Sysfs
extern file_system_type_t sysfs_fs_type;
int init_sysfs(void);

// Pipe filesystem
extern file_system_type_t pipefs_fs_type;
int init_pipefs(void);

// Socket filesystem
extern file_system_type_t sockfs_fs_type;
int init_sockfs(void);

// ============================================================================
// VFS INIT
// ============================================================================

void vfs_init(void);
void mnt_init(void);
void bdev_cache_init(void);
void chrdev_init(void);

// Global dentry cache
extern struct list_head dentry_unused;
extern spinlock_t dcache_lock;

// Global inode cache
extern struct list_head inode_unused;
extern struct list_head inode_in_use;
extern spinlock_t inode_lock;

// Root filesystem mount
extern vfsmount_t *root_mnt;
extern dentry_t *root_dentry;

/* inode link count helpers */
static inline void inode_inc_link_count(inode_t *inode) {
    if (inode) inode->__i_nlink++;
}
static inline void inode_dec_link_count(inode_t *inode) {
    if (inode && inode->__i_nlink > 0) inode->__i_nlink--;
}

/* vfs_root_dentry — get root dentry */
extern dentry_t *vfs_root_dentry(void);
#endif // _FS_VFS_H
int vfs_mkdir_path(const char *path, u32 mode);
