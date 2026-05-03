/**
 * Picomimi-x64 procfs Header
 */
#ifndef _FS_PROCFS_H
#define _FS_PROCFS_H

#include <kernel/types.h>
#include <fs/vfs.h>

extern file_system_type_t procfs_fs_type;
int init_procfs(void);

#endif /* _FS_PROCFS_H */
