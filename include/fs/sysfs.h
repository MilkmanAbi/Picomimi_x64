/**
 * Picomimi-x64 sysfs Header
 */
#ifndef _FS_SYSFS_H
#define _FS_SYSFS_H

#include <kernel/types.h>
#include <fs/vfs.h>

/* Forward declarations */
struct kobject;
struct kset;
struct kobj_attribute;

/* =========================================================
 * kobj_attribute — an individual file in a sysfs directory
 * ========================================================= */
typedef u32 umode_t;
typedef struct kobj_attribute {
    const char  *name;
    umode_t      mode;
    struct list_head list;      /* Linked into kobject->attrs */

    ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
                    char *buf);
    ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
                     const char *buf, size_t count);
} kobj_attribute_t;


/* =========================================================
 * kobject — base object for the sysfs device model
 * ========================================================= */
typedef struct kobject {
    const char          *name;
    struct kobject      *parent;        /* Parent kobject */
    struct kset         *kset;          /* Containing kset */
    struct list_head     entry;         /* Entry in parent kset->list */
    struct list_head     attrs;         /* List of kobj_attribute */
    atomic_t             refcount;
    spinlock_t           lock;
    bool                 dynamic;       /* Free on last put */
} kobject_t;

/* =========================================================
 * kset — a collection of kobjects
 * ========================================================= */
typedef struct kset {
    kobject_t            kobj;          /* Embedded kobject for this kset */
    struct list_head     list;          /* List of member kobjects */
    spinlock_t           list_lock;
} kset_t;

/* =========================================================
 * API
 * ========================================================= */

/* kobject */
void        kobject_init(kobject_t *kobj, const char *name, kset_t *parent);
kobject_t  *kobject_create(const char *name, kset_t *parent);
void        kobject_put(kobject_t *kobj);

/* kset */
kset_t     *kset_create(const char *name, kset_t *parent);

/* Attribute management */
int  sysfs_create_file(kobject_t *kobj, kobj_attribute_t *attr);
void sysfs_remove_file(kobject_t *kobj, kobj_attribute_t *attr);

/* Root kset accessors (for drivers) */
kset_t *sysfs_devices_kset(void);
kset_t *sysfs_class_kset(void);
kset_t *sysfs_block_kset(void);
kset_t *sysfs_bus_kset(void);
kset_t *sysfs_kernel_kset(void);

/* Init */
int init_sysfs(void);

/* Syscalls */
s64 sys_sethostname(const char *name, size_t len);
s64 sys_gethostname(char *name, size_t len);
s64 sys_uname(struct utsname *buf);

extern file_system_type_t sysfs_fs_type;

#endif /* _FS_SYSFS_H */
