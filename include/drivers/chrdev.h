/**
 * Picomimi-x64 Character Device Header
 */
#ifndef _DRIVERS_CHRDEV_H
#define _DRIVERS_CHRDEV_H

#include <kernel/types.h>
#include <fs/vfs.h>

/* Major number allocation conventions (Linux-compatible) */
#define MEM_MAJOR           1
#define TTY_MAJOR           4
#define MISC_MAJOR          10
#define INPUT_MAJOR         13
#define FB_MAJOR            29
#define RANDOM_MAJOR        1

/* MKDEV / MAJOR / MINOR */
#define MKDEV(ma, mi)   (((dev_t)(ma) << 8) | ((dev_t)(mi)))
#define MAJOR(dev)      ((unsigned int)((dev) >> 8))
#define MINOR(dev)      ((unsigned int)((dev) & 0xFF))

/* Registration */
int chrdev_register(unsigned int major, const char *name,
                    const file_operations_t *fops);
int chrdev_unregister(unsigned int major);
const file_operations_t *chrdev_get_fops(unsigned int major);
const file_operations_t *chrdev_lookup_by_name(const char *name, dev_t *devno);

/* Initialisation */
void chrdev_init(void);

/* Syscall */
s64 sys_getrandom(void *buf, size_t buflen, unsigned int flags);

#endif /* _DRIVERS_CHRDEV_H */
