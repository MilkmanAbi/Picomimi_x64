/**
 * Picomimi-x64 Device Filesystem
 * 
 * /dev filesystem with standard device nodes
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// ============================================================================
// DEVICE NUMBERS
// ============================================================================

#define MKDEV(ma, mi)   (((ma) << 20) | (mi))
#define MAJOR(dev)      ((dev) >> 20)
#define MINOR(dev)      ((dev) & 0xFFFFF)

// Major device numbers
#define MEM_MAJOR       1       // /dev/null, /dev/zero, /dev/full, etc.
#define TTY_MAJOR       5       // /dev/tty, /dev/console
#define MISC_MAJOR      10

// Minor numbers for MEM_MAJOR
#define NULL_MINOR      3
#define ZERO_MINOR      5
#define FULL_MINOR      7
#define RANDOM_MINOR    8
#define URANDOM_MINOR   9

// Minor numbers for TTY_MAJOR
#define TTY_MINOR       0
#define CONSOLE_MINOR   1

// ============================================================================
// DEVICE OPERATIONS: /dev/null
// ============================================================================

static s64 null_read(struct file *file, char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)buf;
    (void)count;
    (void)ppos;
    return 0;  // EOF immediately
}

static s64 null_write(struct file *file, const char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)buf;
    (void)ppos;
    return count;  // Swallow everything
}

static const file_operations_t null_fops = {
    .read   = null_read,
    .write  = null_write,
};

// ============================================================================
// DEVICE OPERATIONS: /dev/zero
// ============================================================================

static s64 zero_read(struct file *file, char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)ppos;
    memset(buf, 0, count);
    return count;
}

static const file_operations_t zero_fops = {
    .read   = zero_read,
    .write  = null_write,  // Same as null
};

// ============================================================================
// DEVICE OPERATIONS: /dev/full
// ============================================================================

static s64 full_write(struct file *file, const char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)buf;
    (void)count;
    (void)ppos;
    return -ENOSPC;  // Disk full
}

static const file_operations_t full_fops = {
    .read   = zero_read,
    .write  = full_write,
};

// ============================================================================
// DEVICE OPERATIONS: /dev/random, /dev/urandom
// ============================================================================

static u64 random_seed = 0x123456789ABCDEF0ULL;

static s64 random_read(struct file *file, char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)ppos;
    
    u8 *p = (u8 *)buf;
    for (size_t i = 0; i < count; i++) {
        random_seed = random_seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (random_seed >> 32) & 0xFF;
    }
    
    return count;
}

static const file_operations_t random_fops = {
    .read   = random_read,
    .write  = null_write,
};

// ============================================================================
// DEVICE OPERATIONS: /dev/console, /dev/tty
// ============================================================================

// Serial port functions from serial.c
extern void serial_putc(u16 port, char c);
extern void serial_puts(u16 port, const char *str);
extern int serial_getc(u16 port);

#define COM1 0x3F8

static s64 console_read(struct file *file, char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)ppos;
    
    // TODO: Implement proper keyboard/serial input buffering
    // For now, just return 0 (no input available)
    (void)buf;
    (void)count;
    return 0;
}

static s64 console_write(struct file *file, const char *buf, size_t count, u64 *ppos) {
    (void)file;
    (void)ppos;
    
    for (size_t i = 0; i < count; i++) {
        serial_putc(COM1, buf[i]);
        // Also write to VGA
        extern void vga_putc(char c);
        vga_putc(buf[i]);
    }
    
    return count;
}

static const file_operations_t console_fops = {
    .read   = console_read,
    .write  = console_write,
};

// ============================================================================
// DEVICE LOOKUP
// ============================================================================

static const file_operations_t *get_chrdev_fops(dev_t dev) {
    unsigned int major = MAJOR(dev);
    unsigned int minor = MINOR(dev);
    
    if (major == MEM_MAJOR) {
        switch (minor) {
        case NULL_MINOR:    return &null_fops;
        case ZERO_MINOR:    return &zero_fops;
        case FULL_MINOR:    return &full_fops;
        case RANDOM_MINOR:
        case URANDOM_MINOR: return &random_fops;
        }
    }
    
    if (major == TTY_MAJOR) {
        switch (minor) {
        case TTY_MINOR:
        case CONSOLE_MINOR: return &console_fops;
        }
    }
    
    return NULL;
}

// ============================================================================
// DEVFS INODE OPERATIONS
// ============================================================================

static int devfs_open(struct inode *inode, struct file *file) {
    // Lookup device by dev_t
    const file_operations_t *fops = get_chrdev_fops(inode->i_rdev);
    if (fops) {
        file->f_op = fops;
    }
    return 0;
}

static const file_operations_t devfs_chrdev_fops = {
    .open = devfs_open,
};

// ============================================================================
// POPULATE /dev
// ============================================================================

static int devfs_mknod(dentry_t *parent, const char *name, u32 mode, dev_t dev) {
    qstr_t qname = {
        .name = (const unsigned char *)name,
        .len = strlen(name),
    };
    
    dentry_t *dentry = d_alloc(parent, &qname);
    if (!dentry) {
        return -ENOMEM;
    }
    
    inode_t *inode = new_inode(parent->d_sb);
    if (!inode) {
        dput(dentry);
        return -ENOMEM;
    }
    
    static unsigned long next_ino = 100;
    inode->i_ino = next_ino++;
    inode->i_mode = mode;
    inode->i_rdev = dev;
    inode->i_uid = 0;
    inode->i_gid = 0;
    set_nlink(inode, 1);
    inode->i_fop = &devfs_chrdev_fops;
    
    d_instantiate(dentry, inode);
    
    printk(KERN_INFO "  Created /dev/%s (mode=%o, dev=%u:%u)\n", 
           name, mode & 0777, MAJOR(dev), MINOR(dev));
    
    return 0;
}

// ============================================================================
// INIT
// ============================================================================

int init_devfs(void) {
    extern vfsmount_t *root_mnt;
    extern dentry_t *root_dentry;
    
    if (!root_mnt || !root_dentry) {
        printk(KERN_ERR "devfs: Root not mounted\n");
        return -EINVAL;
    }
    
    printk(KERN_INFO "VFS: Creating /dev...\n");
    
    // Create /dev directory
    qstr_t devname = { .name = (const unsigned char *)"dev", .len = 3 };
    dentry_t *dev_dentry = d_alloc(root_dentry, &devname);
    if (!dev_dentry) {
        return -ENOMEM;
    }
    
    inode_t *dev_inode = new_inode(root_dentry->d_sb);
    if (!dev_inode) {
        dput(dev_dentry);
        return -ENOMEM;
    }
    
    dev_inode->i_ino = 2;
    dev_inode->i_mode = S_IFDIR | 0755;
    set_nlink(dev_inode, 2);
    
    extern const inode_operations_t ramfs_dir_inode_ops;
    extern const file_operations_t ramfs_dir_ops;
    dev_inode->i_op = &ramfs_dir_inode_ops;
    dev_inode->i_fop = &ramfs_dir_ops;
    
    d_instantiate(dev_dentry, dev_inode);
    
    // Create device nodes
    devfs_mknod(dev_dentry, "null",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, NULL_MINOR));
    devfs_mknod(dev_dentry, "zero",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, ZERO_MINOR));
    devfs_mknod(dev_dentry, "full",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, FULL_MINOR));
    devfs_mknod(dev_dentry, "random",  S_IFCHR | 0666, MKDEV(MEM_MAJOR, RANDOM_MINOR));
    devfs_mknod(dev_dentry, "urandom", S_IFCHR | 0444, MKDEV(MEM_MAJOR, URANDOM_MINOR));
    devfs_mknod(dev_dentry, "tty",     S_IFCHR | 0666, MKDEV(TTY_MAJOR, TTY_MINOR));
    devfs_mknod(dev_dentry, "console", S_IFCHR | 0600, MKDEV(TTY_MAJOR, CONSOLE_MINOR));
    
    // Create /dev/fd -> /proc/self/fd symlink (placeholder)
    // devfs_symlink(dev_dentry, "fd", "/proc/self/fd");
    
    // Create /dev/stdin, /dev/stdout, /dev/stderr symlinks
    // devfs_symlink(dev_dentry, "stdin",  "/proc/self/fd/0");
    // devfs_symlink(dev_dentry, "stdout", "/proc/self/fd/1");
    // devfs_symlink(dev_dentry, "stderr", "/proc/self/fd/2");
    
    printk(KERN_INFO "VFS: /dev created with %d devices\n", 7);
    
    return 0;
}
