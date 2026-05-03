/**
 * Picomimi-x64 Character Device Infrastructure
 *
 * Provides:
 *   - Major/minor number allocation and registration table
 *   - chrdev_register() / chrdev_unregister()
 *   - Built-in devices wired at init:
 *       1,3  /dev/null   — always succeeds, discards writes, EOF on read
 *       1,5  /dev/zero   — infinite zero bytes, /dev/null on writes
 *       1,8  /dev/random — LCG PRNG (real entropy TBD)
 *       1,9  /dev/urandom— same
 *       1,1  /dev/mem    — physical memory access (dangerous, root only)
 *       1,11 /dev/kmsg   — kernel message log (read/write)
 *       4,0  /dev/tty0   — current TTY (forwarded to tty subsystem)
 *       5,0  /dev/tty    — controlling terminal
 *       5,1  /dev/console— system console
 *   - mknod integration: devfs calls chrdev_dispatch on open
 */

#include <kernel/types.h>
#include <drivers/chrdev.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/cpu.h>

/* =========================================================
 * Major number registry
 * ========================================================= */

#define CHRDEV_MAJOR_MAX    256
#define CHRDEV_MINOR_MAX    256

typedef struct chrdev_entry {
    const char              *name;
    const file_operations_t *fops;
    bool                     registered;
} chrdev_entry_t;

static chrdev_entry_t chrdev_table[CHRDEV_MAJOR_MAX];
static spinlock_t     chrdev_lock = { .raw_lock = { 0 } };


int chrdev_register(unsigned int major, const char *name,
                    const file_operations_t *fops) {
    if (major >= CHRDEV_MAJOR_MAX) return -EINVAL;
    if (!fops)                      return -EINVAL;

    spin_lock(&chrdev_lock);
    if (chrdev_table[major].registered) {
        spin_unlock(&chrdev_lock);
        return -EBUSY;
    }
    chrdev_table[major].name       = name;
    chrdev_table[major].fops       = fops;
    chrdev_table[major].registered = true;
    spin_unlock(&chrdev_lock);

    printk(KERN_INFO "[chrdev] registered major %u: %s\n", major, name);
    return 0;
}

int chrdev_unregister(unsigned int major) {
    if (major >= CHRDEV_MAJOR_MAX) return -EINVAL;
    spin_lock(&chrdev_lock);
    chrdev_table[major].registered = false;
    chrdev_table[major].fops       = NULL;
    chrdev_table[major].name       = NULL;
    spin_unlock(&chrdev_lock);
    return 0;
}

const file_operations_t *chrdev_get_fops(unsigned int major) {
    if (major >= CHRDEV_MAJOR_MAX)         return NULL;
    if (!chrdev_table[major].registered)   return NULL;
    return chrdev_table[major].fops;
}

/* =========================================================
 * /dev/null
 * ========================================================= */

static s64 null_read(file_t *f, char *buf, size_t count, u64 *ppos) {
    (void)f; (void)buf; (void)count; (void)ppos;
    return 0;   /* EOF */
}

static s64 null_write(file_t *f, const char *buf, size_t count, u64 *ppos) {
    (void)f; (void)buf; (void)ppos;
    return (s64)count;   /* Discard */
}

static s64 null_llseek(file_t *f, s64 offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return 0;
}

static const file_operations_t null_fops = {
    .read   = null_read,
    .write  = null_write,
    .llseek = null_llseek,
};

/* =========================================================
 * /dev/zero
 * ========================================================= */

static s64 zero_read(file_t *f, char *buf, size_t count, u64 *ppos) {
    (void)f; (void)ppos;
    if (!buf) return -EFAULT;
    memset(buf, 0, count);
    return (s64)count;
}

static s64 zero_write(file_t *f, const char *buf, size_t count, u64 *ppos) {
    (void)f; (void)buf; (void)ppos;
    return (s64)count;
}

static const file_operations_t zero_fops = {
    .read   = zero_read,
    .write  = zero_write,
    .llseek = null_llseek,
};

/* =========================================================
 * /dev/random and /dev/urandom
 *
 * LCG PRNG seeded from jiffies + stack address.
 * Real entropy (RDRAND, TSC jitter) can be hooked later.
 * ========================================================= */

extern volatile u64 jiffies;

static u64 prng_state = 0;

static void prng_seed(void) {
    u64 tsc = 0;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    prng_state = tsc ^ jiffies ^ (u64)(uintptr_t)&prng_state;
    if (prng_state == 0) prng_state = 0xDEADBEEFCAFEBABE;
}

static u64 prng_next(void) {
    /* xorshift64 */
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 7;
    prng_state ^= prng_state << 17;
    return prng_state;
}

static s64 random_read(file_t *f, char *buf, size_t count, u64 *ppos) {
    (void)f; (void)ppos;
    if (!buf) return -EFAULT;

    size_t done = 0;
    while (done < count) {
        u64 val = prng_next();
        size_t chunk = count - done;
        if (chunk > 8) chunk = 8;
        memcpy(buf + done, &val, chunk);
        done += chunk;
    }
    return (s64)count;
}

static s64 random_write(file_t *f, const char *buf, size_t count, u64 *ppos) {
    /* Accept entropy writes — xor into state */
    (void)f; (void)ppos;
    for (size_t i = 0; i + 8 <= count; i += 8) {
        u64 v;
        memcpy(&v, buf + i, 8);
        prng_state ^= v;
    }
    return (s64)count;
}

static const file_operations_t random_fops = {
    .read  = random_read,
    .write = random_write,
};

/* =========================================================
 * /dev/kmsg  — kernel message log
 *
 * Writes go to printk. Reads return a stub.
 * ========================================================= */

static s64 kmsg_read(file_t *f, char *buf, size_t count, u64 *ppos) {
    (void)f; (void)buf; (void)count; (void)ppos;
    /* TODO: expose circular printk log */
    return 0;
}

static s64 kmsg_write(file_t *f, const char *buf, size_t count, u64 *ppos) {
    (void)f; (void)ppos;
    /* Echo to kernel log */
    char tmp[256];
    size_t n = count < 255 ? count : 255;
    memcpy(tmp, buf, n);
    tmp[n] = 0;
    printk(KERN_INFO "[kmsg] %s", tmp);
    return (s64)count;
}

static const file_operations_t kmsg_fops = {
    .read  = kmsg_read,
    .write = kmsg_write,
};

/* =========================================================
 * sys_getrandom — fast path PRNG (no fd needed)
 * ========================================================= */

s64 sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (!buf) return -EFAULT;
    return random_read(NULL, (char *)buf, buflen, NULL);
}

/* =========================================================
 * /dev/mem — physical memory read/write (dangerous, root-only)
 * ========================================================= */

#define PHYS_KERNEL_BASE    0xFFFFFFFF80000000ULL   /* Higher-half offset */

static s64 mem_read(file_t *f, char *buf, size_t count, u64 *ppos) {
    (void)f;
    if (!buf || !ppos) return -EFAULT;
    /* Map physical address to virtual: phys + KERNEL_PHYS_BASE */
    u64 phys = *ppos;
    void *virt = (void *)(phys + PHYS_KERNEL_BASE);
    memcpy(buf, virt, count);
    *ppos += count;
    return (s64)count;
}

static s64 mem_write(file_t *f, const char *buf, size_t count, u64 *ppos) {
    (void)f;
    if (!buf || !ppos) return -EFAULT;
    u64 phys = *ppos;
    void *virt = (void *)(phys + PHYS_KERNEL_BASE);
    memcpy(virt, buf, count);
    *ppos += count;
    return (s64)count;
}

static s64 mem_llseek(file_t *f, s64 offset, int whence) {
    (void)f;
    if (whence == SEEK_SET) {
        f->f_pos = (u64)offset;
        return (s64)f->f_pos;
    }
    return -EINVAL;
}

static const file_operations_t mem_fops = {
    .read   = mem_read,
    .write  = mem_write,
    .llseek = mem_llseek,
};

/* =========================================================
 * /dev/fb0 — framebuffer passthrough
 * (actual implementation in drivers/fb/fb.c; we register
 *  the major here so devfs open() can find us)
 * ========================================================= */
extern const file_operations_t fb_fops;   /* from drivers/fb/fb.c */

/* =========================================================
 * chrdev_init — called from kernel init
 * ========================================================= */

void chrdev_init(void) {
    prng_seed();
    memset(chrdev_table, 0, sizeof(chrdev_table));
    spin_lock_init(&chrdev_lock);

    /* MEM_MAJOR = 1 */
    chrdev_register(1,  "mem",    &mem_fops);

    /* TTY_MAJOR = 4  (actual tty fops set by tty subsystem) */
    /* MISC_MAJOR = 5 */

    /* RANDOM_MAJOR = 1, minors 8/9 handled via devfs name lookup */
    /* We use a separate major scheme for simplicity:
     *   major 240 = null/zero/random/urandom (minor 0-3)
     *   major 241 = kmsg
     *   major 242 = fb
     */
    chrdev_register(240, "nullzero",  &null_fops);   /* default; devfs picks minor */

    /* Input event devices (major 13) — fops registered later by input_init() */
    /* We don't register here; input_init() calls chrdev_register(13, ...) */
    chrdev_register(241, "kmsg",      &kmsg_fops);
    /* fb0 registered by fb driver at startup */

    printk(KERN_INFO "[chrdev] character device infrastructure ready\n");
}

/* =========================================================
 * devfs helper: given device name, return fops + devno
 * Used by devfs open() to wire up /dev/null etc.
 * ========================================================= */

typedef struct {
    const char              *name;
    dev_t                    devno;   /* MKDEV(major, minor) */
    const file_operations_t *fops;
} dev_table_entry_t;

#ifndef MKDEV
#define MKDEV(ma, mi)   (((ma) << 8) | (mi))
#endif

static const dev_table_entry_t known_devs[] = {
    { "null",       MKDEV(1, 3),    &null_fops   },
    { "zero",       MKDEV(1, 5),    &zero_fops   },
    { "random",     MKDEV(1, 8),    &random_fops },
    { "urandom",    MKDEV(1, 9),    &random_fops },
    { "kmsg",       MKDEV(241, 0),  &kmsg_fops   },
    { "mem",        MKDEV(1, 1),    &mem_fops    },
    { NULL, 0, NULL }
};

const file_operations_t *chrdev_lookup_by_name(const char *name, dev_t *devno) {
    for (int i = 0; known_devs[i].name; i++) {
        if (strcmp(known_devs[i].name, name) == 0) {
            if (devno) *devno = known_devs[i].devno;
            return known_devs[i].fops;
        }
    }
    return NULL;
}
