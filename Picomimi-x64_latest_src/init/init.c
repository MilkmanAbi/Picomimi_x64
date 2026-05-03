/**
 * init.c — Picomimi PID 1 (kernel-space init)
 *
 * Boot sequence:
 *   1. Mount filesystems, write /etc
 *   2. Install userspace (busybox into ramfs)
 *   3. Spawn a kernel thread that execs /bin/sh
 *   4. Enter zombie reaper loop forever (PID 1 must never die)
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/cred.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>

/* ------------------------------------------------------------------ */
static void mount_early_filesystems(void)
{
    printk(KERN_INFO "[init] Mounting filesystems...\n");
    /* procfs and sysfs are already mounted by kernel_init. Skip re-init. */

    extern int vfs_mkdir_path(const char *path, u32 mode);
    /* Note: /dev is created by devfs, /proc by procfs, /sys by sysfs */
    /* Don't re-create them here - that would shadow the real ones */
    vfs_mkdir_path("/tmp",      0777);
    vfs_mkdir_path("/home",     0755);
    vfs_mkdir_path("/root",     0700);
    vfs_mkdir_path("/sbin",     0755);
    vfs_mkdir_path("/bin",      0755);
    vfs_mkdir_path("/lib",      0755);
    vfs_mkdir_path("/etc",      0755);
    vfs_mkdir_path("/var",      0755);
    vfs_mkdir_path("/var/log",  0755);
    vfs_mkdir_path("/var/run",  0755);

    printk(KERN_INFO "[init] Filesystems mounted\n");
}

static void write_etc(void)
{
    extern s64 sys_open(const char *, int, u32);
    extern s64 sys_write(int, const void *, size_t);
    extern s64 sys_close(int);
    int fd;

#define WRITE_FILE(path, mode, ...) do { \
    const char *_buf = __VA_ARGS__; \
    fd = (int)sys_open(path, O_WRONLY|O_CREAT|O_TRUNC, mode); \
    if (fd >= 0) { sys_write(fd, _buf, strlen(_buf)); sys_close(fd); } \
} while(0)

    WRITE_FILE("/etc/hostname", 0644, "picomimi\n");
    WRITE_FILE("/etc/passwd",   0644,
        "root:x:0:0:root:/root:/bin/sh\n"
        "nobody:x:65534:65534:nobody:/:/sbin/nologin\n");
    WRITE_FILE("/etc/group",    0644,
        "root:x:0:\nnogroup:x:65534:\n");
    WRITE_FILE("/etc/os-release", 0644,
        "NAME=\"Picomimi\"\nVERSION=\"1.0.0\"\nID=picomimi\n"
        "PRETTY_NAME=\"Picomimi 1.0.0\"\n");
    WRITE_FILE("/etc/profile",  0644,
        "export PATH=/bin:/sbin:/usr/bin:/usr/sbin\n"
        "export HOME=/root\n"
        "export TERM=linux\n"
        "export PS1='\\[\\033[1;32m\\]\\u@picomimi\\[\\033[0m\\]:\\[\\033[1;34m\\]\\w\\[\\033[0m\\]# '\n");

    printk(KERN_INFO "[init] /etc populated\n");
}

static int shell_pid = -1;  /* PID of the active shell */
static void shell_launcher(void *arg);  /* forward decl */

static void spawn_shell(void) {
    extern task_struct_t *task_create(const char *name, void (*entry)(void *),
                                       void *arg, unsigned int flags);
    extern void wake_up_process(task_struct_t *task);
    task_struct_t *t = task_create("shell", shell_launcher, NULL, 0);
    if (t) {
        shell_pid = t->pid;
        wake_up_process(t);
        printk(KERN_INFO "[init] shell respawned as PID %d\n", shell_pid);
    }
}

static void zombie_reaper_loop(void)
{
    printk(KERN_INFO "[init] PID 1 zombie reaper running\n");
    while (1) {
        int status;
        s64 pid = sys_wait4(-1, &status, WNOHANG, NULL);
        if (pid > 0) {
            printk(KERN_INFO "[init] reaped zombie PID %lld (status=%d)\n",
                   (long long)pid, status);
            if ((int)pid == shell_pid) {
                printk(KERN_INFO "[init] shell exited — respawning\n");
                shell_pid = -1;
                spawn_shell();
            }
        }
        schedule();
    }
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */

static void shell_launcher(void *arg)
{
    (void)arg;
    printk(KERN_INFO "[shell] launcher: pid=%d, setting up stdio\n",
           (int)sys_getpid());

    /* Verify /dev/console exists before opening */
    {
        extern s64 sys_access(const char *path, int mode);
        s64 r_dev     = sys_access("/dev", 0);
        s64 r_console = sys_access("/dev/console", 0);
        printk(KERN_INFO "[shell] access: /dev=%lld /dev/console=%lld\n",
               (long long)r_dev, (long long)r_console);
    }

    /* Open /dev/console for stdin (fd 0), stdout (fd 1), stderr (fd 2) */
    s64 fd0 = sys_open("/dev/console", O_RDWR, 0);
    if (fd0 != 0) {
        /* If it didn't land on fd 0, something is wrong; try to dup2 */
        if (fd0 > 0) {
            extern s64 sys_dup2(int oldfd, int newfd);
            sys_dup2((int)fd0, 0);
            sys_close((int)fd0);
        }
    }
    s64 fd1 = sys_open("/dev/console", O_RDWR, 0);
    if (fd1 != 1) {
        if (fd1 > 0) {
            extern s64 sys_dup2(int oldfd, int newfd);
            sys_dup2((int)fd1, 1);
            sys_close((int)fd1);
        }
    }
    s64 fd2 = sys_open("/dev/console", O_RDWR, 0);
    if (fd2 != 2) {
        if (fd2 > 0) {
            extern s64 sys_dup2(int oldfd, int newfd);
            sys_dup2((int)fd2, 2);
            sys_close((int)fd2);
        }
    }
    printk(KERN_INFO "[shell] stdio: fd0=%lld fd1=%lld fd2=%lld\n", fd0, fd1, fd2);

    extern int do_execve(const char *filename, char *const argv[],
                         char *const envp[]);

    char *argv[] = { "/bin/sh", NULL };
    char *envp[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "HOME=/root",
        "TERM=linux",
        "PS1=picomimi# ",
        "USER=root",
        "LOGNAME=root",
        "SHELL=/bin/sh",
        NULL
    };

    int r = do_execve("/bin/sh", argv, envp);

    /* execve failed — fall back to kernel shell */
    printk(KERN_ERR "[shell] execve failed: %d, falling back to ksh\n", r);
    extern void ksh_init(void);
    ksh_init();
    sys_exit(1);
}

/* ------------------------------------------------------------------ */
void init_main(void)
{
    printk(KERN_INFO "\n");
    printk(KERN_INFO "[init] *** Picomimi Init (PID 1) ***\n");
    printk(KERN_INFO "[init] Kernel version: Picomimi-x64 1.0.0\n");
    printk(KERN_INFO "\n");

    mount_early_filesystems();
    write_etc();

    extern void ramfs_populate_userspace(void);
    ramfs_populate_userspace();

    /* Spawn shell */
    printk(KERN_INFO "[init] Spawning shell...\n");
    spawn_shell();
    if (shell_pid < 0) {
        printk(KERN_ERR "[init] Failed to create shell task, running ksh\n");
        extern void ksh_init(void);
        ksh_init();
    }

    zombie_reaper_loop();
}
