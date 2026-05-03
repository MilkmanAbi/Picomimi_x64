/**
 * Picomimi-x64 sysfs
 *
 * A simplified sysfs providing:
 *   /sys/kernel/hostname
 *   /sys/kernel/ostype
 *   /sys/kernel/osrelease
 *   /sys/kernel/version
 *   /sys/kernel/dmesg_restrict
 *   /sys/devices/  (enumerated by drivers via sysfs_create_file)
 *   /sys/class/tty/
 *   /sys/class/block/
 *   /sys/class/input/
 *   /sys/block/
 *
 * kobject model:
 *   kobject → kset → sysfs directory
 *   Each kobject maps to one sysfs directory.
 *   Attributes (kobj_attribute) map to files within.
 *
 * Attribute files:
 *   Drivers call sysfs_create_file(kobj, attr) to expose a file.
 *   Read/write dispatched through attr->show / attr->store.
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <fs/sysfs.h>
#include <kernel/process.h>
#include <kernel/kernel.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

extern kernel_state_t kernel_state;

/* =========================================================
 * kobject
 * ========================================================= */

void kobject_init(kobject_t *kobj, const char *name, kset_t *parent) {
    if (!kobj) return;
    memset(kobj, 0, sizeof(*kobj));
    kobj->name   = name;
    kobj->parent = parent ? &parent->kobj : NULL;
    atomic_set(&kobj->refcount, 1);
    INIT_LIST_HEAD(&kobj->entry);
    INIT_LIST_HEAD(&kobj->attrs);
    spin_lock_init(&kobj->lock);
}

kobject_t *kobject_create(const char *name, kset_t *parent) {
    kobject_t *kobj = kzalloc(sizeof(kobject_t), GFP_KERNEL);
    if (!kobj) return NULL;
    kobject_init(kobj, name, parent);
    kobj->dynamic = true;

    if (parent) {
        spin_lock(&parent->list_lock);
        list_add_tail(&kobj->entry, &parent->list);
        spin_unlock(&parent->list_lock);
    }
    return kobj;
}

void kobject_put(kobject_t *kobj) {
    if (!kobj) return;
    if (atomic_dec_return(&kobj->refcount) == 0 && kobj->dynamic)
        kfree(kobj);
}

/* =========================================================
 * kset
 * ========================================================= */

kset_t *kset_create(const char *name, kset_t *parent) {
    kset_t *ks = kzalloc(sizeof(kset_t), GFP_KERNEL);
    if (!ks) return NULL;
    kobject_init(&ks->kobj, name, parent);
    INIT_LIST_HEAD(&ks->list);
    spin_lock_init(&ks->list_lock);
    ks->kobj.dynamic = true;

    if (parent) {
        spin_lock(&parent->list_lock);
        list_add_tail(&ks->kobj.entry, &parent->list);
        spin_unlock(&parent->list_lock);
    }
    return ks;
}

/* =========================================================
 * Attribute management
 * ========================================================= */

int sysfs_create_file(kobject_t *kobj, kobj_attribute_t *attr) {
    if (!kobj || !attr) return -EINVAL;
    spin_lock(&kobj->lock);
    list_add_tail(&attr->list, &kobj->attrs);
    spin_unlock(&kobj->lock);
    return 0;
}

void sysfs_remove_file(kobject_t *kobj, kobj_attribute_t *attr) {
    if (!kobj || !attr) return;
    spin_lock(&kobj->lock);
    list_del(&attr->list);
    spin_unlock(&kobj->lock);
}

/* =========================================================
 * sysfs inode private data
 * ========================================================= */

typedef enum {
    SYSFS_DIR,
    SYSFS_ATTR,
    SYSFS_SYMLINK,
} sysfs_entry_type_t;

typedef struct sysfs_inode_info {
    sysfs_entry_type_t  type;
    kobject_t           *kobj;
    kobj_attribute_t    *attr;
    char                symlink_target[256];
} sysfs_inode_info_t;

/* =========================================================
 * sysfs file operations
 * ========================================================= */

static s64 sysfs_read(file_t *file, char *buf, size_t count, u64 *ppos) {
    if (!file || !buf) return -EFAULT;
    inode_t *inode = file->f_inode;
    if (!inode || !inode->i_private) return -EINVAL;

    sysfs_inode_info_t *info = (sysfs_inode_info_t *)inode->i_private;
    if (info->type != SYSFS_ATTR || !info->attr) return -EINVAL;
    if (!info->attr->show) return -EPERM;

    /* Generate into temp buffer */
    char *tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!tmp) return -ENOMEM;

    ssize_t total = info->attr->show(info->kobj, info->attr, tmp);
    if (total < 0) { kfree(tmp); return total; }

    u64 pos  = ppos ? *ppos : 0;
    s64 ret  = 0;
    if (pos < (u64)total) {
        size_t avail = (size_t)total - (size_t)pos;
        size_t n = avail < count ? avail : count;
        memcpy(buf, tmp + pos, n);
        if (ppos) *ppos = pos + n;
        ret = (s64)n;
    }
    kfree(tmp);
    return ret;
}

static s64 sysfs_write(file_t *file, const char *buf, size_t count, u64 *ppos) {
    (void)ppos;
    if (!file || !buf) return -EFAULT;
    inode_t *inode = file->f_inode;
    if (!inode || !inode->i_private) return -EINVAL;

    sysfs_inode_info_t *info = (sysfs_inode_info_t *)inode->i_private;
    if (info->type != SYSFS_ATTR || !info->attr) return -EINVAL;
    if (!info->attr->store) return -EPERM;

    return info->attr->store(info->kobj, info->attr, buf, count);
}

static const file_operations_t sysfs_attr_fops = {
    .read  = sysfs_read,
    .write = sysfs_write,
};

/* =========================================================
 * Built-in /sys/kernel attributes
 * ========================================================= */

/* Hostname — default "picomimi" */
static char sysfs_hostname[256] = "picomimi";

static ssize_t hostname_show(kobject_t *kobj, kobj_attribute_t *attr, char *buf) {
    (void)kobj; (void)attr;
    size_t n = strlen(sysfs_hostname);
    memcpy(buf, sysfs_hostname, n);
    buf[n] = '\n';
    return (ssize_t)(n + 1);
}

static ssize_t hostname_store(kobject_t *kobj, kobj_attribute_t *attr,
                               const char *buf, size_t count) {
    (void)kobj; (void)attr;
    size_t n = count < 255 ? count : 255;
    memcpy(sysfs_hostname, buf, n);
    /* Strip trailing newline */
    while (n > 0 && (sysfs_hostname[n-1] == '\n' || sysfs_hostname[n-1] == '\r'))
        n--;
    sysfs_hostname[n] = 0;
    return (ssize_t)count;
}

static ssize_t ostype_show(kobject_t *k, kobj_attribute_t *a, char *buf) {
    (void)k; (void)a;
    const char *s = "Linux\n";  /* We report Linux for compatibility */
    size_t n = strlen(s);
    memcpy(buf, s, n);
    return (ssize_t)n;
}

static ssize_t osrelease_show(kobject_t *k, kobj_attribute_t *a, char *buf) {
    (void)k; (void)a;
    const char *s = "6.1.0-picomimi\n";
    size_t n = strlen(s);
    memcpy(buf, s, n);
    return (ssize_t)n;
}

static ssize_t version_show(kobject_t *k, kobj_attribute_t *a, char *buf) {
    (void)k; (void)a;
    const char *s = "#1 SMP Picomimi-x64 kernel\n";
    size_t n = strlen(s);
    memcpy(buf, s, n);
    return (ssize_t)n;
}

static ssize_t dmesg_restrict_show(kobject_t *k, kobj_attribute_t *a, char *buf) {
    (void)k; (void)a;
    buf[0] = '0'; buf[1] = '\n';
    return 2;
}

static kobj_attribute_t hostname_attr  = { .name = "hostname",       .show = hostname_show,       .store = hostname_store };
static kobj_attribute_t ostype_attr    = { .name = "ostype",         .show = ostype_show,         .store = NULL };
static kobj_attribute_t osrelease_attr = { .name = "osrelease",      .show = osrelease_show,      .store = NULL };
static kobj_attribute_t version_attr   = { .name = "version",        .show = version_show,        .store = NULL };
static kobj_attribute_t dmesg_attr     = { .name = "dmesg_restrict", .show = dmesg_restrict_show, .store = NULL };

/* =========================================================
 * sysfs superblock + VFS integration
 * ========================================================= */

static super_block_t *sysfs_sb = NULL;

/* Root ksets */
static kset_t *kernel_kset   = NULL;
static kset_t *devices_kset  = NULL;
static kset_t *class_kset    = NULL;
static kset_t *block_kset    = NULL;
static kset_t *bus_kset      = NULL;
static kset_t *firmware_kset = NULL;
static kset_t *module_kset   = NULL;
static kset_t *power_kset    = NULL;

/* These are exported for drivers */
kset_t *sysfs_devices_kset(void)  { return devices_kset; }
kset_t *sysfs_class_kset(void)    { return class_kset;   }
kset_t *sysfs_block_kset(void)    { return block_kset;   }
kset_t *sysfs_bus_kset(void)      { return bus_kset;     }
kset_t *sysfs_kernel_kset(void)   { return kernel_kset;  }

/* =========================================================
 * sysfs inode creation helpers
 * ========================================================= */

static inode_t *sysfs_make_inode(sysfs_entry_type_t type,
                                   kobject_t *kobj,
                                   kobj_attribute_t *attr,
                                   u32 mode) {
    inode_t *inode = new_inode(sysfs_sb);
    if (!inode) return NULL;

    sysfs_inode_info_t *info = kzalloc(sizeof(sysfs_inode_info_t), GFP_KERNEL);
    if (!info) { kfree(inode); return NULL; }

    info->type  = type;
    info->kobj  = kobj;
    info->attr  = attr;

    inode->i_mode    = mode;
    inode->i_private = info;
    inode->i_uid     = 0;
    inode->i_gid     = 0;
    inode->i_size    = PAGE_SIZE;

    if (type == SYSFS_ATTR)
        inode->i_fop = &sysfs_attr_fops;

    return inode;
}

/* =========================================================
 * sysfs directory lookup — walks kobject tree
 * ========================================================= */

static const inode_operations_t sysfs_dir_inode_ops;  /* forward */
static dentry_t *sysfs_lookup(inode_t *dir, dentry_t *dentry,
                                unsigned int flags) {
    (void)flags;
    sysfs_inode_info_t *dir_info = (sysfs_inode_info_t *)dir->i_private;
    if (!dir_info || dir_info->type != SYSFS_DIR) return ERR_PTR(-ENOTDIR);

    kobject_t *parent_kobj = dir_info->kobj;
    const char *name = (const char *)dentry->d_name.name;

    /* Check if it's an attribute of this kobj */
    if (parent_kobj) {
        struct list_head *p;
        list_for_each(p, &parent_kobj->attrs) {
            kobj_attribute_t *attr = list_entry(p, kobj_attribute_t, list);
            if (strcmp(attr->name, name) == 0) {
                u32 mode = S_IFREG;
                if (attr->show)  mode |= 0444;
                if (attr->store) mode |= 0200;
                inode_t *inode = sysfs_make_inode(SYSFS_ATTR, parent_kobj,
                                                   attr, mode);
                if (!inode) return ERR_PTR(-ENOMEM);
                d_add(dentry, inode);
                return NULL;
            }
        }
    }

    /* Check if it's a child kobject (kset member) */
    /* kset members stored in kset->list, each with a kobject */
    /* We need to find the kset for this directory */
    /* For now: iterate sysfs root ksets */
    kset_t *ksets[] = { kernel_kset, devices_kset, class_kset,
                        block_kset, bus_kset, firmware_kset, NULL };
    for (int i = 0; ksets[i]; i++) {
        kset_t *ks = ksets[i];
        struct list_head *p;
        spin_lock(&ks->list_lock);
        list_for_each(p, &ks->list) {
            kobject_t *child = list_entry(p, kobject_t, entry);
            if (child->name && strcmp(child->name, name) == 0) {
                spin_unlock(&ks->list_lock);
                inode_t *inode = sysfs_make_inode(SYSFS_DIR, child,
                                                   NULL, S_IFDIR | 0555);
                if (!inode) return ERR_PTR(-ENOMEM);
                inode->i_op = &sysfs_dir_inode_ops;
                d_add(dentry, inode);
                return NULL;
            }
        }
        spin_unlock(&ks->list_lock);
    }

    return ERR_PTR(-ENOENT);
}

static const inode_operations_t sysfs_dir_inode_ops = {
    .lookup = sysfs_lookup,
};

/* =========================================================
 * sysfs superblock fill
 * ========================================================= */

static int sysfs_fill_super(super_block_t *sb) {
    sb->s_magic     = 0x62656572;   /* SYSFS_MAGIC */
    sb->s_blocksize = 4096;
    sb->s_maxbytes  = ~0ULL;
    sysfs_sb = sb;

    inode_t *root = new_inode(sb);
    if (!root) return -ENOMEM;

    sysfs_inode_info_t *info = kzalloc(sizeof(sysfs_inode_info_t), GFP_KERNEL);
    if (!info) return -ENOMEM;
    info->type = SYSFS_DIR;
    info->kobj = NULL;  /* Root has no kobject */

    root->i_mode    = S_IFDIR | 0555;
    root->i_op      = &sysfs_dir_inode_ops;
    root->i_private = info;
    root->i_uid     = 0;
    root->i_gid     = 0;

    sb->s_root = d_make_root(root);
    return sb->s_root ? 0 : -ENOMEM;
}

static struct dentry *sysfs_mount(file_system_type_t *fs_type, int flags,
                                   const char *dev_name, void *data) {
    (void)fs_type; (void)flags; (void)dev_name; (void)data;

    super_block_t *sb = kzalloc(sizeof(super_block_t), GFP_KERNEL);
    if (!sb) return ERR_PTR(-ENOMEM);

    if (sysfs_fill_super(sb) != 0) { kfree(sb); return ERR_PTR(-EINVAL); }
    return sb->s_root;
}

static void sysfs_kill_sb(super_block_t *sb) { (void)sb; }

file_system_type_t sysfs_fs_type = {
    .name    = "sysfs",
    .fs_flags = 0,
    .mount   = sysfs_mount,
    .kill_sb = sysfs_kill_sb,
};

/* =========================================================
 * sys_sethostname / sys_gethostname
 * ========================================================= */

s64 sys_sethostname(const char *name, size_t len) {
    if (!name || len > 255) return -EINVAL;
    memcpy(sysfs_hostname, name, len);
    sysfs_hostname[len] = 0;
    return 0;
}

s64 sys_gethostname(char *name, size_t len) {
    if (!name || len == 0) return -EINVAL;
    size_t n = strlen(sysfs_hostname);
    if (n >= len) n = len - 1;
    memcpy(name, sysfs_hostname, n);
    name[n] = 0;
    return 0;
}

s64 sys_uname(struct utsname *buf) {
    if (!buf) return -EFAULT;
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->sysname,  "Linux",             6);
    memcpy(buf->nodename, sysfs_hostname,      strlen(sysfs_hostname) + 1);
    memcpy(buf->release,  "6.1.0-picomimi",    15);
    memcpy(buf->version,  "#1 SMP Picomimi",   15);
    memcpy(buf->machine,  "x86_64",            7);
    memcpy(buf->domainname, "picomimi.local",  14);
    return 0;
}

/* =========================================================
 * sysfs init
 * ========================================================= */

int init_sysfs(void) {
    printk(KERN_INFO "[sysfs] initializing /sys\n");

    /* Create root ksets */
    kernel_kset   = kset_create("kernel",   NULL);
    devices_kset  = kset_create("devices",  NULL);
    class_kset    = kset_create("class",    NULL);
    block_kset    = kset_create("block",    NULL);
    bus_kset      = kset_create("bus",      NULL);
    firmware_kset = kset_create("firmware", NULL);
    module_kset   = kset_create("module",   NULL);
    power_kset    = kset_create("power",    NULL);

    /* Populate /sys/kernel */
    if (kernel_kset) {
        INIT_LIST_HEAD(&hostname_attr.list);
        INIT_LIST_HEAD(&ostype_attr.list);
        INIT_LIST_HEAD(&osrelease_attr.list);
        INIT_LIST_HEAD(&version_attr.list);
        INIT_LIST_HEAD(&dmesg_attr.list);
        sysfs_create_file(&kernel_kset->kobj, &hostname_attr);
        sysfs_create_file(&kernel_kset->kobj, &ostype_attr);
        sysfs_create_file(&kernel_kset->kobj, &osrelease_attr);
        sysfs_create_file(&kernel_kset->kobj, &version_attr);
        sysfs_create_file(&kernel_kset->kobj, &dmesg_attr);
    }

    /* Create tty class */
    kset_t *tty_class = kset_create("tty", class_kset);
    (void)tty_class;

    /* Register and mount */
    register_filesystem(&sysfs_fs_type);
    do_mount(NULL, "/sys", "sysfs", 0, NULL);

    printk(KERN_INFO "[sysfs] /sys mounted\n");
    return 0;
}
