/**
 * Picomimi-x64 Ramfs Population
 *
 * Installs userspace binaries into ramfs at boot:
 *   /bin/busybox  — musl-static busybox (194KB, 272 applets)
 *   /bin/sh       — symlink → busybox (ash)
 *   /bin/ls, /bin/cat, etc. — symlinks → busybox
 *
 * Binary blobs injected via objcopy -I binary in the Makefile.
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

/* Symbols injected by objcopy */
extern const char _binary_iso_boot_userspace_bin_busybox_start[];
extern const char _binary_iso_boot_userspace_bin_busybox_end[];

/* Write a binary blob into a ramfs file */
static int create_binary(const char *path, const char *data, size_t size, u32 mode) {
    extern s64 sys_open(const char *, int, u32);
    extern s64 sys_write(int, const void *, size_t);
    extern s64 sys_close(int);

    int fd = (int)sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        printk(KERN_ERR "[ramfs_pop] Failed to create %s: %d\n", path, fd);
        return fd;
    }
    size_t written = 0;
    while (written < size) {
        size_t chunk = (size - written) > 65536 ? 65536 : (size - written);
        s64 r = sys_write(fd, data + written, chunk);
        if (r <= 0) { sys_close(fd); return (int)r; }
        written += (size_t)r;
    }
    sys_close(fd);
    return 0;
}

/* Create a symlink, silently ignore failures */
static void make_symlink(const char *target, const char *linkpath) {
    extern s64 sys_symlink(const char *target, const char *linkpath);
    sys_symlink(target, linkpath);
}

/* Create directory, ignore if exists */
static void make_dir(const char *path) {
    extern int vfs_mkdir_path(const char *path, u32 mode);
    vfs_mkdir_path(path, 0755);
}

void ramfs_populate_userspace(void) {
    printk(KERN_INFO "[ramfs_pop] Installing userspace...\n");

    /* Directory structure */
    /* NOTE: /dev is owned by devfs, /proc by procfs, /sys by sysfs.
     * Creating them here would shadow the real mounts! Skip them. */
    make_dir("/bin");
    make_dir("/sbin");
    make_dir("/usr");
    make_dir("/usr/bin");
    make_dir("/usr/sbin");
    make_dir("/lib");
    make_dir("/etc");
    make_dir("/tmp");
    make_dir("/var");
    make_dir("/var/log");
    make_dir("/home");
    make_dir("/root");

    /* /bin/busybox — the main binary */
    size_t bb_sz = (size_t)(
        _binary_iso_boot_userspace_bin_busybox_end -
        _binary_iso_boot_userspace_bin_busybox_start);

    extern int ramfs_create_blob(const char *path, const void *data,
                                  size_t size, u32 mode);
    /* Zero-copy install: point ramfs inode directly at the in-kernel blob */
    int r = ramfs_create_blob("/bin/busybox",
        _binary_iso_boot_userspace_bin_busybox_start, bb_sz, 0755);
    if (r < 0) {
        printk(KERN_ERR "[ramfs_pop] FATAL: failed to install busybox: %d\n", r);
        return;
    }
    printk(KERN_INFO "[ramfs_pop] /bin/busybox: %zu bytes (zero-copy)\n", bb_sz);

    /* Symlinks for every busybox applet we care about */
    const char *applets[] = {
        "sh", "ash", "cat", "echo", "ls", "mkdir", "rm", "cp", "mv",
        "ln", "chmod", "chown", "pwd", "head", "tail", "wc", "cut",
        "sort", "uniq", "grep", "egrep", "fgrep", "find", "sed", "awk",
        "env", "basename", "dirname", "sleep", "kill", "ps", "expr",
        "test", "true", "false", "uname", "whoami", "date", "clear",
        "stat", "touch", "which", "xargs", "dd", "tee", "yes", "seq",
        "free", "sync", "hostname", "readlink", "printf",
        NULL
    };

    for (int i = 0; applets[i]; i++) {
        char path[64];
        path[0] = '/'; path[1] = 'b'; path[2] = 'i'; path[3] = 'n';
        path[4] = '/';
        int j = 0;
        while (applets[i][j]) { path[5+j] = applets[i][j]; j++; }
        path[5+j] = '\0';
        make_symlink("/bin/busybox", path);
    }

    /* /etc/passwd and /etc/hostname */
    {
        extern s64 sys_open(const char *, int, u32);
        extern s64 sys_write(int, const void *, size_t);
        extern s64 sys_close(int);
        const char *passwd =
            "root:x:0:0:root:/root:/bin/sh\n"
            "nobody:x:65534:65534:nobody:/:/bin/false\n";
        int fd = (int)sys_open("/etc/passwd", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) {
            sys_write(fd, passwd, strlen(passwd));
            sys_close(fd);
        }
        const char *hostname = "picomimi\n";
        fd = (int)sys_open("/etc/hostname", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) {
            sys_write(fd, hostname, strlen(hostname));
            sys_close(fd);
        }
        /* /etc/profile for shell environment */
        const char *profile =
            "export PATH=/bin:/sbin:/usr/bin:/usr/sbin\n"
            "export HOME=/root\n"
            "export TERM=linux\n"
            "export PS1='\\[\\033[1;32m\\]picomimi\\[\\033[0m\\]:\\[\\033[1;34m\\]\\w\\[\\033[0m\\]# '\n";
        fd = (int)sys_open("/etc/profile", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) {
            sys_write(fd, profile, strlen(profile));
            sys_close(fd);
        }
    }

    printk(KERN_INFO "[ramfs_pop] Userspace ready: busybox + %d symlinks\n", 51);
    printk(KERN_INFO "[ramfs_pop]   /bin/sh /bin/ls /bin/cat /bin/grep ...\n");
}
