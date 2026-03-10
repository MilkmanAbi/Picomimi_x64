#pragma once
/* cred_t is defined in process.h — include that first */
#include <kernel/types.h>

/* Forward decl */
struct cred;
typedef struct cred cred_t;

/* API */
cred_t *cred_alloc(void);
cred_t *cred_get(cred_t *c);
void    cred_put(cred_t *c);
cred_t *prepare_cred(void);
int     commit_cred(cred_t *new);
cred_t *get_init_cred(void);

/* Syscalls implemented in cred.c */
s64 sys_setuid(uid_t uid);
s64 sys_setgid(gid_t gid);
s64 sys_setresuid(uid_t ruid, uid_t euid, uid_t suid);
s64 sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
s64 sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
s64 sys_getgroups(int size, gid_t *list);
s64 sys_setgroups(int size, const gid_t *list);
