/**
 * cred.c — Process credential management
 *
 * Each process carries a set of credentials (uid/gid/groups/caps).
 * This implements the Linux-compatible credential model:
 *   - Real UID/GID (ruid/rgid)   — inherited from parent
 *   - Effective UID/GID (euid/egid) — used for permission checks
 *   - Saved UID/GID (suid/sgid)  — used by set-user-ID programs
 *   - Supplementary groups (up to NGROUPS_MAX)
 *   - Capability bitmask (Linux-compatible 64-bit caps)
 *
 * Credentials are copy-on-write: tasks share a cred_t until one
 * needs to change them (e.g. setuid), at which point prepare_cred()
 * makes a private copy.
 */

#include <kernel/types.h>
#include <kernel/cred.h>
#include <kernel/process.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printk.h>

/* Initial credentials — root, all capabilities */
static cred_t init_cred = {
    .usage   = { .counter = 1 },
    .uid     = 0, .gid     = 0,
    .euid    = 0, .egid    = 0,
    .suid    = 0, .sgid    = 0,
    .fsuid   = 0, .fsgid   = 0,
    .ngroups = 0,
    .cap_permitted   = ~0ULL,
    .cap_effective   = ~0ULL,
    .cap_inheritable = 0ULL,
    .cap_bset        = ~0ULL,
};

/* ------------------------------------------------------------------ */

cred_t *cred_alloc(void)
{
    cred_t *c = kmalloc(sizeof(cred_t), GFP_KERNEL);
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    atomic_set(&c->usage, 1);
    return c;
}

cred_t *cred_get(cred_t *c)
{
    if (c) atomic_inc(&c->usage);
    return c;
}

void cred_put(cred_t *c)
{
    if (!c) return;
    if (atomic_dec_and_test(&c->usage))
        kfree(c);
}

/**
 * prepare_cred — allocate a mutable copy of the current credentials.
 * Call commit_cred() to install it on the current task.
 */
cred_t *prepare_cred(void)
{
    task_struct_t *t = get_current_task();
    if (!t || !t->cred) return NULL;

    cred_t *new = cred_alloc();
    if (!new) return NULL;
    *new = *t->cred;
    atomic_set(&new->usage, 1);
    return new;
}

int commit_cred(cred_t *new)
{
    task_struct_t *t = get_current_task();
    if (!t) return -ESRCH;

    cred_t *old = t->cred;
    t->cred = new;
    cred_put(old);
    return 0;
}

cred_t *get_init_cred(void)
{
    return &init_cred;
}

/* ------------------------------------------------------------------ */
/*  Syscall implementations                                              */
/* ------------------------------------------------------------------ */

s64 sys_getuid(void)  { task_struct_t *t = get_current_task(); return t ? t->cred->uid  : 0; }
s64 sys_geteuid(void) { task_struct_t *t = get_current_task(); return t ? t->cred->euid : 0; }
s64 sys_getgid(void)  { task_struct_t *t = get_current_task(); return t ? t->cred->gid  : 0; }
s64 sys_getegid(void) { task_struct_t *t = get_current_task(); return t ? t->cred->egid : 0; }

s64 sys_setuid(uid_t uid)
{
    cred_t *new = prepare_cred();
    if (!new) return -ENOMEM;
    /* If CAP_SETUID: set all three; else only effective if ruid/suid match */
    if (new->cap_effective & CAP_SETUID) {
        new->uid = new->euid = new->suid = new->fsuid = uid;
    } else {
        if (uid != new->uid && uid != new->suid) { kfree(new); return -EPERM; }
        new->euid = new->fsuid = uid;
    }
    return commit_cred(new);
}

s64 sys_setgid(gid_t gid)
{
    cred_t *new = prepare_cred();
    if (!new) return -ENOMEM;
    if (new->cap_effective & CAP_SETGID) {
        new->gid = new->egid = new->sgid = new->fsgid = gid;
    } else {
        if (gid != new->gid && gid != new->sgid) { kfree(new); return -EPERM; }
        new->egid = new->fsgid = gid;
    }
    return commit_cred(new);
}

s64 sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    cred_t *new = prepare_cred();
    if (!new) return -ENOMEM;
    if (!(new->cap_effective & CAP_SETUID)) {
        /* Unprivileged: each must be current ruid/euid/suid or -1 */
        if ((ruid != (uid_t)-1 && ruid != new->uid  && ruid != new->euid && ruid != new->suid) ||
            (euid != (uid_t)-1 && euid != new->uid  && euid != new->euid && euid != new->suid) ||
            (suid != (uid_t)-1 && suid != new->uid  && suid != new->euid && suid != new->suid)) {
            kfree(new); return -EPERM;
        }
    }
    if (ruid != (uid_t)-1) new->uid  = ruid;
    if (euid != (uid_t)-1) new->euid = new->fsuid = euid;
    if (suid != (uid_t)-1) new->suid = suid;
    return commit_cred(new);
}

s64 sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
    task_struct_t *t = get_current_task();
    if (!t) return -ESRCH;
    if (ruid) *ruid = t->cred->uid;
    if (euid) *euid = t->cred->euid;
    if (suid) *suid = t->cred->suid;
    return 0;
}

s64 sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
    task_struct_t *t = get_current_task();
    if (!t) return -ESRCH;
    if (rgid) *rgid = t->cred->gid;
    if (egid) *egid = t->cred->egid;
    if (sgid) *sgid = t->cred->sgid;
    return 0;
}

s64 sys_getgroups(int size, gid_t *list)
{
    task_struct_t *t = get_current_task();
    if (!t) return -ESRCH;
    if (size == 0) return t->cred->ngroups;
    if (size < t->cred->ngroups) return -EINVAL;
    for (int i = 0; i < t->cred->ngroups; i++)
        list[i] = t->cred->groups[i];
    return t->cred->ngroups;
}

s64 sys_setgroups(int size, const gid_t *list)
{
    if (size < 0 || size > NGROUPS_MAX) return -EINVAL;
    cred_t *new = prepare_cred();
    if (!new) return -ENOMEM;
    if (!(new->cap_effective & CAP_SETGID)) { kfree(new); return -EPERM; }
    for (int i = 0; i < size; i++) new->groups[i] = list[i];
    new->ngroups = size;
    return commit_cred(new);
}

cred_t *cred_copy(const cred_t *old)
{
    cred_t *new = cred_alloc();
    if (!new) return NULL;
    *new = *old;
    atomic_set(&new->usage, 1);
    return new;
}

void cred_free(cred_t *c)
{
    cred_put(c);
}

s64 sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    cred_t *new = prepare_cred();
    if (!new) return -ENOMEM;
    if (!(new->cap_effective & CAP_SETGID)) {
        if ((rgid != (gid_t)-1 && rgid != new->gid  && rgid != new->egid && rgid != new->sgid) ||
            (egid != (gid_t)-1 && egid != new->gid  && egid != new->egid && egid != new->sgid) ||
            (sgid != (gid_t)-1 && sgid != new->gid  && sgid != new->egid && sgid != new->sgid)) {
            kfree(new); return -EPERM;
        }
    }
    if (rgid != (gid_t)-1) new->gid  = rgid;
    if (egid != (gid_t)-1) new->egid = new->fsgid = egid;
    if (sgid != (gid_t)-1) new->sgid = sgid;
    return commit_cred(new);
}
