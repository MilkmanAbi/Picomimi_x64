/**
 * sysctl.c — Kernel parameter store
 *
 * A simple tree of named u64/string parameters exposed through
 * /proc/sys. The kernel registers params at boot; userspace reads
 * and writes them with sysctl(2) or through the procfs interface.
 *
 * Layout mirrors Linux:
 *   /proc/sys/kernel/hostname
 *   /proc/sys/kernel/pid_max
 *   /proc/sys/kernel/panic
 *   /proc/sys/vm/overcommit_memory
 *   /proc/sys/net/ipv4/ip_forward
 */

#include <kernel/types.h>
#include <kernel/sysctl.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <mm/slab.h>

/* ------------------------------------------------------------------ */
/*  Internal tree                                                        */
/* ------------------------------------------------------------------ */

#define SYSCTL_MAX   128

static sysctl_entry_t sysctl_table[SYSCTL_MAX];
static int            sysctl_count = 0;

/* Built-in kernel parameters */
static u64  param_pid_max       = 32768;
static u64  param_panic_timeout = 0;        /* 0 = don't reboot on panic */
static u64  param_printk_ratelimit = 5;
static u64  param_overcommit    = 0;        /* 0 = heuristic, 1 = always, 2 = never */
static u64  param_dirty_ratio   = 20;
static u64  param_ip_forward    = 0;
static char param_hostname[64]  = "picomimi";
static char param_domainname[64] = "(none)";
static char param_ostype[32]    = "Picomimi";
static char param_osrelease[32] = "1.0.0";
static char param_version[64]   = "#1 Picomimi-x64";

/* ------------------------------------------------------------------ */
/*  Registration API                                                     */
/* ------------------------------------------------------------------ */

int sysctl_register(const char *path, sysctl_type_t type,
                    void *data, size_t size, int flags)
{
    if (sysctl_count >= SYSCTL_MAX) {
        printk(KERN_WARNING "sysctl: table full, cannot register %s\n", path);
        return -ENOMEM;
    }
    sysctl_entry_t *e = &sysctl_table[sysctl_count++];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->type  = type;
    e->data  = data;
    e->size  = size;
    e->flags = flags;
    return 0;
}

sysctl_entry_t *sysctl_find(const char *path)
{
    for (int i = 0; i < sysctl_count; i++)
        if (strcmp(sysctl_table[i].path, path) == 0)
            return &sysctl_table[i];
    return NULL;
}

int sysctl_read(const char *path, void *buf, size_t *size)
{
    sysctl_entry_t *e = sysctl_find(path);
    if (!e) return -ENOENT;
    size_t n = (*size < e->size) ? *size : e->size;
    memcpy(buf, e->data, n);
    *size = n;
    return 0;
}

int sysctl_write(const char *path, const void *buf, size_t size)
{
    sysctl_entry_t *e = sysctl_find(path);
    if (!e) return -ENOENT;
    if (e->flags & SYSCTL_RDONLY) return -EPERM;
    size_t n = (size < e->size) ? size : e->size;
    memcpy(e->data, buf, n);
    return 0;
}

/* Iterate over all entries under a prefix */
int sysctl_enumerate(const char *prefix,
                     int (*cb)(const sysctl_entry_t *e, void *arg),
                     void *arg)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < sysctl_count; i++) {
        if (strncmp(sysctl_table[i].path, prefix, plen) == 0)
            if (cb(&sysctl_table[i], arg) != 0) break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Init: register built-in params                                       */
/* ------------------------------------------------------------------ */

void sysctl_init(void)
{
    /* kernel.* */
    sysctl_register("kernel/hostname",     SYSCTL_STRING, param_hostname,         64, SYSCTL_RW);
    sysctl_register("kernel/domainname",   SYSCTL_STRING, param_domainname,       64, SYSCTL_RW);
    sysctl_register("kernel/ostype",       SYSCTL_STRING, param_ostype,           32, SYSCTL_RDONLY);
    sysctl_register("kernel/osrelease",    SYSCTL_STRING, param_osrelease,        32, SYSCTL_RDONLY);
    sysctl_register("kernel/version",      SYSCTL_STRING, param_version,          64, SYSCTL_RDONLY);
    sysctl_register("kernel/pid_max",      SYSCTL_U64,    &param_pid_max,          8, SYSCTL_RW);
    sysctl_register("kernel/panic",        SYSCTL_U64,    &param_panic_timeout,    8, SYSCTL_RW);
    sysctl_register("kernel/printk_ratelimit", SYSCTL_U64, &param_printk_ratelimit, 8, SYSCTL_RW);

    /* vm.* */
    sysctl_register("vm/overcommit_memory",SYSCTL_U64,    &param_overcommit,       8, SYSCTL_RW);
    sysctl_register("vm/dirty_ratio",      SYSCTL_U64,    &param_dirty_ratio,      8, SYSCTL_RW);

    /* net.* */
    sysctl_register("net/ipv4/ip_forward", SYSCTL_U64,    &param_ip_forward,       8, SYSCTL_RW);

    printk(KERN_INFO "sysctl: %d parameters registered\n", sysctl_count);
}
