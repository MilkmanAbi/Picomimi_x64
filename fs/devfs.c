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
    if (!count) return 0;

    /* Block until a character is available from COM1 */
    extern int serial_getc_blocking(u16 port);
    int c = serial_getc_blocking(COM1);
    if (c < 0) return -EIO;

    char ch = (char)(c & 0xFF);

    /* CR -> LF (terminal canonical mode: Enter key sends \r, we give ash \n) */
    if (ch == '\r') ch = '\n';

    buf[0] = ch;

    /* NO manual echo here — ash owns echo via termios ECHO flag.
     * Echoing here AND letting ash echo produces double characters.
     * The TTY line discipline (ash in this case) will write the char
     * back to stdout itself when ECHO is set in c_lflag. */

    return 1;
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

/* Fake termios for /dev/console — makes busybox think it's a real TTY */
static s64 console_ioctl(struct file *file, u32 cmd, unsigned long arg) {
    (void)file;

    switch (cmd) {
    case 0x5401: /* TCGETS */ {
        /* Return a sane default termios so busybox enters interactive mode */
        struct fake_termios {
            unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
            unsigned char c_line;
            unsigned char c_cc[19];
        } t = {
            .c_iflag = 0x6d02,   /* ICRNL|IXON|BRKINT|IUTF8 */
            .c_oflag = 0x05,     /* OPOST|ONLCR */
            .c_cflag = 0xbf,     /* CS8|CREAD|HUPCL... */
            .c_lflag = 0x8a3b,   /* ECHO|ECHOE|ECHOK|ICANON|ISIG|IEXTEN */
            .c_line  = 0,
            .c_cc    = {3,28,127,21,4,0,1,0,17,19,26,0,18,15,23,22,0,0,0},
        };
        memcpy((void *)arg, &t, sizeof(t));
        return 0;
    }
    case 0x5413: /* TIOCGWINSZ */ {
        struct fake_winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
        struct fake_winsize ws = { 24, 80, 0, 0 };
        memcpy((void *)arg, &ws, sizeof(ws));
        return 0;
    }
    case 0x5402: /* TCSETS */
    case 0x5403: /* TCSETSW */
    case 0x5404: /* TCSETSF */
    case 0x5414: /* TIOCSWINSZ */
    case 0x540E: /* TIOCSCTTY */
    case 0x5410: /* TIOCSPGRP */
        return 0;  /* accept but ignore */
    case 0x540F: /* TIOCGPGRP */
        *(int *)arg = 2;  /* return busybox's pid as pgrp */
        return 0;
    default:
        return -25; /* ENOTTY */
    }
}

static const file_operations_t console_fops = {
    .read   = console_read,
    .write  = console_write,
    .ioctl  = console_ioctl,
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

    if (major == 29 && minor == 0) {  /* FB_MAJOR=29, minor 0 = fb0 */
        extern const file_operations_t fb_fops;
        return &fb_fops;
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
    extern const inode_operations_t ramfs_dir_inode_ops;
    extern const file_operations_t ramfs_dir_ops;
    
    if (!root_mnt || !root_dentry) {
        printk(KERN_ERR "devfs: Root not mounted\n");
        return -EINVAL;
    }
    
    printk(KERN_INFO "VFS: Creating /dev...\n");

    /* Create /dev directory — check first to avoid duplicate dentry */
    qstr_t devname = { .name = (const unsigned char *)"dev", .len = 3 };
    dentry_t *dev_dentry = d_lookup(root_dentry, &devname);
    if (!dev_dentry) {
        dev_dentry = d_alloc(root_dentry, &devname);
        if (!dev_dentry) return -ENOMEM;

        inode_t *dev_inode = new_inode(root_dentry->d_sb);
        if (!dev_inode) { dput(dev_dentry); return -ENOMEM; }

        dev_inode->i_ino  = 2;
        dev_inode->i_mode = S_IFDIR | 0755;
        set_nlink(dev_inode, 2);

        dev_inode->i_op  = &ramfs_dir_inode_ops;
        dev_inode->i_fop = &ramfs_dir_ops;

        d_instantiate(dev_dentry, dev_inode);
    }
    
    // Create device nodes
    devfs_mknod(dev_dentry, "null",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, NULL_MINOR));
    devfs_mknod(dev_dentry, "zero",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, ZERO_MINOR));
    devfs_mknod(dev_dentry, "full",    S_IFCHR | 0666, MKDEV(MEM_MAJOR, FULL_MINOR));
    devfs_mknod(dev_dentry, "random",  S_IFCHR | 0666, MKDEV(MEM_MAJOR, RANDOM_MINOR));
    devfs_mknod(dev_dentry, "urandom", S_IFCHR | 0444, MKDEV(MEM_MAJOR, URANDOM_MINOR));
    devfs_mknod(dev_dentry, "kmsg",    S_IFCHR | 0660, MKDEV(MEM_MAJOR, 11));
    devfs_mknod(dev_dentry, "mem",     S_IFCHR | 0640, MKDEV(MEM_MAJOR, 1));
    devfs_mknod(dev_dentry, "tty",     S_IFCHR | 0666, MKDEV(TTY_MAJOR, TTY_MINOR));
    devfs_mknod(dev_dentry, "console", S_IFCHR | 0600, MKDEV(TTY_MAJOR, CONSOLE_MINOR));
    devfs_mknod(dev_dentry, "ptmx",    S_IFCHR | 0666, MKDEV(TTY_MAJOR, 2));
    devfs_mknod(dev_dentry, "fb0",     S_IFCHR | 0660, MKDEV(29, 0));

    // TTY nodes tty0..tty7
    char tty_name[8];
    for (int i = 0; i < 8; i++) {
        tty_name[0]='t'; tty_name[1]='t'; tty_name[2]='y';
        tty_name[3]='0'+i; tty_name[4]=0;
        devfs_mknod(dev_dentry, tty_name, S_IFCHR | 0620, MKDEV(4, i));
    }

    /* ---- /dev/input/ subdirectory ---- */
    {
        static const char input_name[] = "input";
        qstr_t iqname = { .name = (const unsigned char *)input_name,
                          .len  = sizeof(input_name) - 1 };
        dentry_t *input_dentry = d_alloc(dev_dentry, &iqname);
        if (input_dentry) {
            inode_t *input_inode = new_inode(root_dentry->d_sb);
            if (input_inode) {
                static unsigned long _ino_ctr = 100;
                input_inode->i_ino  = _ino_ctr++;
                input_inode->i_mode = S_IFDIR | 0755;
                set_nlink(input_inode, 2);
                input_inode->i_op  = &ramfs_dir_inode_ops;
                input_inode->i_fop = &ramfs_dir_ops;
                d_instantiate(input_dentry, input_inode);

                /* event0 = keyboard (major 13, minor 0)
                   event1 = mouse    (major 13, minor 1) */
#define INPUT_MAJOR 13
                devfs_mknod(input_dentry, "event0", S_IFCHR | 0660,
                            MKDEV(INPUT_MAJOR, 0));
                devfs_mknod(input_dentry, "event1", S_IFCHR | 0660,
                            MKDEV(INPUT_MAJOR, 1));
            }
        }
    }

    printk(KERN_INFO "VFS: /dev created with %d devices\n", 17);
    
    return 0;
}
