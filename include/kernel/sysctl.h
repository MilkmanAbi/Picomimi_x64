#pragma once
#include <kernel/types.h>

typedef enum {
    SYSCTL_U64    = 0,
    SYSCTL_STRING = 1,
    SYSCTL_BOOL   = 2,
} sysctl_type_t;

#define SYSCTL_RDONLY  (1 << 0)
#define SYSCTL_RW      (0)

typedef struct sysctl_entry {
    char            path[128];
    sysctl_type_t   type;
    void           *data;
    size_t          size;
    int             flags;
} sysctl_entry_t;

void            sysctl_init(void);
int             sysctl_register(const char *path, sysctl_type_t type,
                                void *data, size_t size, int flags);
sysctl_entry_t *sysctl_find(const char *path);
int             sysctl_read(const char *path, void *buf, size_t *size);
int             sysctl_write(const char *path, const void *buf, size_t size);
int             sysctl_enumerate(const char *prefix,
                                 int (*cb)(const sysctl_entry_t *e, void *arg),
                                 void *arg);
