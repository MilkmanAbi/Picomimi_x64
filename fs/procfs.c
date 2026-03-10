/**
 * Picomimi-x64 procfs
 *
 * Provides a read-only virtual filesystem at /proc exposing
 * kernel and process state to userspace.
 *
 * Implemented entries:
 *   /proc/version      — kernel version string
 *   /proc/uptime       — uptime in seconds.hundredths
 *   /proc/meminfo      — memory stats
 *   /proc/cpuinfo      — CPU identification
 *   /proc/stat         — aggregate CPU/context-switch stats
 *   /proc/loadavg      — fake 1/5/15 minute load averages
 *   /proc/mounts       — currently mounted filesystems
 *   /proc/filesystems  — registered filesystem types
 *   /proc/self         — symlink to /proc/<current_pid>
 *   /proc/<pid>/       — per-process directory
 *   /proc/<pid>/status — process status
 *   /proc/<pid>/stat   — scheduler stats (Linux format)
 *   /proc/<pid>/maps   — VMA map
 *   /proc/<pid>/fd/    — open file descriptors
 *   /proc/<pid>/cmdline— argv
 *   /proc/<pid>/exe    — symlink to executable
 */

#include <kernel/types.h>
#include <fs/vfs.h>
#include <fs/procfs.h>
#include <kernel/process.h>
#include <kernel/kernel.h>
#include <kernel/timer.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <mm/pmm.h>

/* =========================================================
 * External symbols
 * ========================================================= */
extern volatile u64 jiffies;
extern kernel_state_t kernel_state;
extern task_struct_t *task_table[];
extern u64 pmm_get_total_memory(void);
extern u64 pmm_get_free_memory(void);
extern u64 pmm_get_used_memory(void);

/* =========================================================
 * procfs inode private_data types
 * ========================================================= */

typedef enum {
    PROC_VERSION    = 1,
    PROC_UPTIME,
    PROC_MEMINFO,
    PROC_CPUINFO,
    PROC_STAT,
    PROC_LOADAVG,
    PROC_MOUNTS,
    PROC_FILESYSTEMS,
    PROC_SELF,
    PROC_PID_STATUS,
    PROC_PID_STAT,
    PROC_PID_MAPS,
    PROC_PID_CMDLINE,
    PROC_PID_FD,
    PROC_PID_EXE,
    PROC_PID_MEM,
    PROC_PID_DIR,
} proc_entry_type_t;

typedef struct proc_inode_info {
    proc_entry_type_t   type;
    pid_t               pid;    /* Valid for PROC_PID_* entries */
} proc_inode_info_t;

/* =========================================================
 * Snprintf-style buffer for proc read
 * ========================================================= */

typedef struct proc_buf {
    char   *data;
    size_t  size;
    size_t  pos;
} proc_buf_t;

static void pbuf_init(proc_buf_t *pb, char *buf, size_t size) {
    pb->data = buf;
    pb->size = size;
    pb->pos  = 0;
}

static void pbuf_printf(proc_buf_t *pb, const char *fmt, ...) {
    if (pb->pos >= pb->size) return;
    size_t avail = pb->size - pb->pos;
    va_list ap;
    va_start(ap, fmt);

    /* Use a temp buffer to format, then copy */
    char tmp[256];
    /* Very minimal vsnprintf — just handle basic %s %d %u %lu %llu */
    int n = 0;
    const char *p = fmt;
    while (*p && n < (int)sizeof(tmp) - 1) {
        if (*p != '%') { tmp[n++] = *p++; continue; }
        p++;  /* skip '%' */
        /* Flags: skip '-', '0', width, length */
        int ljust = 0, zero = 0;
        if (*p == '-') { ljust = 1; p++; }
        if (*p == '0') { zero = 1; p++; }
        (void)ljust; (void)zero;
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p-'0'); p++; }
        int ll = 0;
        if (*p == 'l') { p++; if (*p == 'l') { ll = 1; p++; } else { ll = 1; } }
        if (*p == 'z') { ll = 1; p++; }

        char fmtbuf[64];
        int fi = 0;
        fmtbuf[fi++] = '%';

        char c = *p++;
        switch (c) {
        case 'd': {
            s64 v = ll ? va_arg(ap, s64) : (s64)va_arg(ap, int);
            /* Simple integer to string */
            char ibuf[22]; int ii = 21; ibuf[ii] = 0;
            bool neg = v < 0;
            u64 uv = neg ? (u64)(-v) : (u64)v;
            if (uv == 0) ibuf[--ii] = '0';
            while (uv) { ibuf[--ii] = '0' + uv % 10; uv /= 10; }
            if (neg) ibuf[--ii] = '-';
            const char *s = ibuf + ii;
            while (*s && n < (int)sizeof(tmp)-1) tmp[n++] = *s++;
            break;
        }
        case 'u': {
            u64 v = ll ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            char ibuf[22]; int ii = 21; ibuf[ii] = 0;
            if (v == 0) ibuf[--ii] = '0';
            while (v) { ibuf[--ii] = '0' + v % 10; v /= 10; }
            const char *s = ibuf + ii;
            while (*s && n < (int)sizeof(tmp)-1) tmp[n++] = *s++;
            break;
        }
        case 'x': case 'X': {
            u64 v = ll ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            char ibuf[18]; int ii = 17; ibuf[ii] = 0;
            const char *hex = (c == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            if (v == 0) ibuf[--ii] = '0';
            while (v) { ibuf[--ii] = hex[v & 0xF]; v >>= 4; }
            /* pad with width */
            while (17 - ii < width && ii > 0) ibuf[--ii] = zero ? '0' : ' ';
            const char *s = ibuf + ii;
            while (*s && n < (int)sizeof(tmp)-1) tmp[n++] = *s++;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && n < (int)sizeof(tmp)-1) tmp[n++] = *s++;
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            tmp[n++] = ch;
            break;
        }
        case '%': tmp[n++] = '%'; break;
        default:  tmp[n++] = c; break;
        }
        (void)fi; (void)fmtbuf;
    }
    tmp[n] = 0;

    va_end(ap);

    /* Copy to output buffer */
    size_t len = (size_t)n;
    if (len > avail) len = avail;
    memcpy(pb->data + pb->pos, tmp, len);
    pb->pos += len;
}

/* =========================================================
 * Content generators
 * ========================================================= */

static size_t proc_gen_version(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    pbuf_printf(&pb,
        "Linux version 6.1.0-picomimi-x64 (root@picomimi) "
        "(gcc 13.2.0) #1 SMP %s\n",
        "Mon Jan  1 00:00:00 UTC 2025");
    return pb.pos;
}

static size_t proc_gen_uptime(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    u64 up = get_uptime_seconds();
    /* Format: "uptime_secs.hundredths idle_secs.hundredths\n" */
    pbuf_printf(&pb, "%llu.%02llu %llu.%02llu\n",
        up, (u64)0, up / 2, (u64)0);
    return pb.pos;
}

static size_t proc_gen_meminfo(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);

    u64 total_kb = pmm_get_total_memory() / 1024;
    u64 free_kb  = pmm_get_free_memory()  / 1024;
    u64 used_kb  = pmm_get_used_memory()  / 1024;
    u64 avail_kb = free_kb + (free_kb / 4);  /* Approx available */
    u64 cached_kb = used_kb / 8;
    u64 buf_kb    = used_kb / 16;

    pbuf_printf(&pb, "MemTotal:       %8llu kB\n", total_kb);
    pbuf_printf(&pb, "MemFree:        %8llu kB\n", free_kb);
    pbuf_printf(&pb, "MemAvailable:   %8llu kB\n", avail_kb);
    pbuf_printf(&pb, "Buffers:        %8llu kB\n", buf_kb);
    pbuf_printf(&pb, "Cached:         %8llu kB\n", cached_kb);
    pbuf_printf(&pb, "SwapCached:            0 kB\n");
    pbuf_printf(&pb, "Active:         %8llu kB\n", used_kb / 2);
    pbuf_printf(&pb, "Inactive:       %8llu kB\n", used_kb / 4);
    pbuf_printf(&pb, "SwapTotal:             0 kB\n");
    pbuf_printf(&pb, "SwapFree:              0 kB\n");
    pbuf_printf(&pb, "Dirty:                 0 kB\n");
    pbuf_printf(&pb, "Writeback:             0 kB\n");
    pbuf_printf(&pb, "AnonPages:      %8llu kB\n", used_kb / 4);
    pbuf_printf(&pb, "Mapped:         %8llu kB\n", used_kb / 8);
    pbuf_printf(&pb, "Shmem:                 0 kB\n");
    pbuf_printf(&pb, "KReclaimable:          0 kB\n");
    pbuf_printf(&pb, "Slab:           %8llu kB\n", used_kb / 16);
    pbuf_printf(&pb, "PageTables:            4 kB\n");
    pbuf_printf(&pb, "HugePages_Total:       0\n");
    pbuf_printf(&pb, "HugePages_Free:        0\n");
    pbuf_printf(&pb, "HugePages_Rsvd:        0\n");
    pbuf_printf(&pb, "HugePages_Surp:        0\n");
    pbuf_printf(&pb, "Hugepagesize:       2048 kB\n");
    pbuf_printf(&pb, "DirectMap4k:    %8llu kB\n", total_kb);
    return pb.pos;
}

static size_t proc_gen_cpuinfo(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    u32 ncpus = kernel_state.num_cpus ? kernel_state.num_cpus : 1;

    for (u32 cpu = 0; cpu < ncpus; cpu++) {
        pbuf_printf(&pb, "processor\t: %u\n", cpu);
        pbuf_printf(&pb, "vendor_id\t: GenuineIntel\n");
        pbuf_printf(&pb, "cpu family\t: 6\n");
        pbuf_printf(&pb, "model\t\t: 142\n");
        pbuf_printf(&pb, "model name\t: Picomimi Virtual CPU @ 2.40GHz\n");
        pbuf_printf(&pb, "stepping\t: 10\n");
        pbuf_printf(&pb, "microcode\t: 0xea\n");
        pbuf_printf(&pb, "cpu MHz\t\t: 2400.000\n");
        pbuf_printf(&pb, "cache size\t: 6144 KB\n");
        pbuf_printf(&pb, "physical id\t: 0\n");
        pbuf_printf(&pb, "siblings\t: %u\n", ncpus);
        pbuf_printf(&pb, "core id\t\t: %u\n", cpu);
        pbuf_printf(&pb, "cpu cores\t: %u\n", ncpus);
        pbuf_printf(&pb, "apicid\t\t: %u\n", cpu * 2);
        pbuf_printf(&pb, "fpu\t\t: yes\n");
        pbuf_printf(&pb, "fpu_exception\t: yes\n");
        pbuf_printf(&pb, "cpuid level\t: 22\n");
        pbuf_printf(&pb, "wp\t\t: yes\n");
        pbuf_printf(&pb, "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic "
                          "sep mtrr pge mca cmov pat pse36 clflush mmx fxsr "
                          "sse sse2 ht syscall nx lm\n");
        pbuf_printf(&pb, "bogomips\t: 4800.00\n");
        pbuf_printf(&pb, "clflush size\t: 64\n");
        pbuf_printf(&pb, "cache_alignment\t: 64\n");
        pbuf_printf(&pb, "address sizes\t: 39 bits physical, 48 bits virtual\n");
        pbuf_printf(&pb, "\n");
    }
    return pb.pos;
}

static size_t proc_gen_stat(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    u64 j = jiffies;
    /* cpu  user nice system idle iowait irq softirq steal guest guest_nice */
    pbuf_printf(&pb, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",
        j / 4, j / 8, j / 2);
    pbuf_printf(&pb, "cpu0 %llu 0 %llu %llu 0 0 0 0 0 0\n",
        j / 4, j / 8, j / 2);
    pbuf_printf(&pb, "intr %llu\n", j * 100);
    pbuf_printf(&pb, "ctxt %llu\n", kernel_state.context_switches);
    pbuf_printf(&pb, "btime %llu\n", (u64)1735689600ULL);
    pbuf_printf(&pb, "processes %u\n", kernel_state.total_tasks);
    pbuf_printf(&pb, "procs_running %u\n", kernel_state.running_tasks);
    pbuf_printf(&pb, "procs_blocked 0\n");
    return pb.pos;
}

static size_t proc_gen_loadavg(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    u32 running = kernel_state.running_tasks;
    u32 total   = kernel_state.total_tasks;
    if (total == 0) total = 1;
    /* Fake load average: running / total */
    pbuf_printf(&pb, "0.%02u 0.%02u 0.%02u %u/%u %u\n",
        running, running, running,
        running, total,
        current ? (u32)current->pid : 1);
    return pb.pos;
}

static size_t proc_gen_mounts(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    pbuf_printf(&pb, "rootfs / rootfs rw 0 0\n");
    pbuf_printf(&pb, "devfs /dev devfs rw,nosuid 0 0\n");
    pbuf_printf(&pb, "proc /proc proc rw,nosuid,nodev,noexec 0 0\n");
    return pb.pos;
}

static size_t proc_gen_filesystems(char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);
    pbuf_printf(&pb, "nodev\tsysfs\n");
    pbuf_printf(&pb, "nodev\ttmpfs\n");
    pbuf_printf(&pb, "nodev\tdevtmpfs\n");
    pbuf_printf(&pb, "nodev\tdevfs\n");
    pbuf_printf(&pb, "nodev\tproc\n");
    pbuf_printf(&pb, "nodev\tpipefs\n");
    pbuf_printf(&pb, "\tramfs\n");
    return pb.pos;
}

/* Per-process /proc/<pid>/status */
static size_t proc_gen_pid_status(pid_t pid, char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);

    task_struct_t *t = find_task_by_pid(pid);
    if (!t) return 0;

    const char *state_str;
    switch (t->state) {
    case TASK_RUNNING:          state_str = "R (running)"; break;
    case TASK_INTERRUPTIBLE:    state_str = "S (sleeping)"; break;
    case TASK_UNINTERRUPTIBLE:  state_str = "D (disk sleep)"; break;
    case TASK_STOPPED:          state_str = "T (stopped)"; break;
    case TASK_ZOMBIE:           state_str = "Z (zombie)"; break;
    default:                    state_str = "X (dead)"; break;
    }

    pbuf_printf(&pb, "Name:\t%s\n",     t->comm);
    pbuf_printf(&pb, "Umask:\t0022\n");
    pbuf_printf(&pb, "State:\t%s\n",    state_str);
    pbuf_printf(&pb, "Tgid:\t%d\n",     t->tgid);
    pbuf_printf(&pb, "Ngid:\t0\n");
    pbuf_printf(&pb, "Pid:\t%d\n",      t->pid);
    pbuf_printf(&pb, "PPid:\t%d\n",
        t->real_parent ? t->real_parent->pid : 0);
    pbuf_printf(&pb, "TracerPid:\t0\n");
    pbuf_printf(&pb, "Uid:\t%u\t%u\t%u\t%u\n",
        t->cred ? t->cred->uid  : 0,
        t->cred ? t->cred->euid : 0,
        t->cred ? t->cred->suid : 0,
        t->cred ? t->cred->fsuid: 0);
    pbuf_printf(&pb, "Gid:\t%u\t%u\t%u\t%u\n",
        t->cred ? t->cred->gid  : 0,
        t->cred ? t->cred->egid : 0,
        t->cred ? t->cred->sgid : 0,
        t->cred ? t->cred->fsgid: 0);
    pbuf_printf(&pb, "FDSize:\t64\n");
    pbuf_printf(&pb, "Groups:\n");

    if (t->mm) {
        pbuf_printf(&pb, "VmPeak:\t%8llu kB\n", t->mm->total_vm * 4);
        pbuf_printf(&pb, "VmSize:\t%8llu kB\n", t->mm->total_vm * 4);
        pbuf_printf(&pb, "VmLck:\t%8llu kB\n",  t->mm->locked_vm * 4);
        pbuf_printf(&pb, "VmPin:\t%8llu kB\n",  t->mm->pinned_vm * 4);
        pbuf_printf(&pb, "VmHWM:\t%8llu kB\n",  t->mm->total_vm * 4);
        pbuf_printf(&pb, "VmRSS:\t%8llu kB\n",  t->mm->total_vm * 2);
        pbuf_printf(&pb, "VmData:\t%8llu kB\n", t->mm->data_vm * 4);
        pbuf_printf(&pb, "VmStk:\t%8llu kB\n",  t->mm->stack_vm * 4);
        pbuf_printf(&pb, "VmExe:\t%8llu kB\n",  t->mm->exec_vm * 4);
        pbuf_printf(&pb, "VmLib:\t       0 kB\n");
        pbuf_printf(&pb, "VmPTE:\t       4 kB\n");
        pbuf_printf(&pb, "VmSwap:\t       0 kB\n");
    }

    pbuf_printf(&pb, "Threads:\t1\n");
    pbuf_printf(&pb, "SigPnd:\t%016llx\n", t->pending.signal.sig[0]);
    pbuf_printf(&pb, "SigBlk:\t%016llx\n", t->blocked.sig[0]);
    pbuf_printf(&pb, "SigIgn:\t0000000000000000\n");
    pbuf_printf(&pb, "SigCgt:\t0000000000000000\n");
    pbuf_printf(&pb, "CapInh:\t0000000000000000\n");
    pbuf_printf(&pb, "CapPrm:\t0000000000003fff\n");
    pbuf_printf(&pb, "CapEff:\t0000000000003fff\n");
    pbuf_printf(&pb, "CapBnd:\t000000ffffffffff\n");
    pbuf_printf(&pb, "CapAmb:\t0000000000000000\n");
    pbuf_printf(&pb, "NoNewPrivs:\t0\n");
    pbuf_printf(&pb, "Seccomp:\t0\n");
    pbuf_printf(&pb, "voluntary_ctxt_switches:\t%llu\n", t->context.rax);
    pbuf_printf(&pb, "nonvoluntary_ctxt_switches:\t0\n");

    return pb.pos;
}

/* /proc/<pid>/stat — Linux scheduler stat format */
static size_t proc_gen_pid_stat(pid_t pid, char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);

    task_struct_t *t = find_task_by_pid(pid);
    if (!t) return 0;

    char state = 'R';
    switch (t->state) {
    case TASK_RUNNING:         state = 'R'; break;
    case TASK_INTERRUPTIBLE:   state = 'S'; break;
    case TASK_UNINTERRUPTIBLE: state = 'D'; break;
    case TASK_STOPPED:         state = 'T'; break;
    case TASK_ZOMBIE:          state = 'Z'; break;
    default:                   state = 'X'; break;
    }

    pbuf_printf(&pb,
        "%d (%s) %c %d %d %d 0 -1 4194304 0 0 0 0 "
        "%llu %llu 0 0 20 0 1 0 %llu "
        "%llu %llu 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0\n",
        t->pid, t->comm, state,
        t->real_parent ? t->real_parent->pid : 0,
        t->signal ? (int)t->signal->pgrp : (int)t->tgid,
        t->signal ? (int)t->signal->session : (int)t->tgid,
        t->utime / 10000000ULL,   /* in jiffies */
        t->stime / 10000000ULL,
        t->start_time / 10000000ULL,
        t->mm ? (u64)t->mm->total_vm * 4096 : 0ULL,
        t->mm ? (u64)t->mm->total_vm / 2 : 0ULL);

    return pb.pos;
}

/* /proc/<pid>/maps — VMA listing */
static size_t proc_gen_pid_maps(pid_t pid, char *buf, size_t size) {
    proc_buf_t pb; pbuf_init(&pb, buf, size);

    task_struct_t *t = find_task_by_pid(pid);
    if (!t || !t->mm) return 0;

    vm_area_t *vma = t->mm->mmap;
    while (vma) {
        char perms[5];
        perms[0] = (vma->flags & VM_READ)   ? 'r' : '-';
        perms[1] = (vma->flags & VM_WRITE)  ? 'w' : '-';
        perms[2] = (vma->flags & VM_EXEC)   ? 'x' : '-';
        perms[3] = (vma->vm_flags & 0x0008) ? 's' : 'p';
        perms[4] = 0;

        pbuf_printf(&pb, "%016llx-%016llx %s 00000000 00:00 0",
            vma->start, vma->end, perms);

        if (vma->vm_flags & 0x4000) {
            pbuf_printf(&pb, "                   [stack]");
        } else if (vma->vm_flags & 0x2000) {
            pbuf_printf(&pb, "                   [heap]");
        }
        pbuf_printf(&pb, "\n");
        vma = vma->next;
    }

    return pb.pos;
}

/* /proc/<pid>/cmdline */
static size_t proc_gen_pid_cmdline(pid_t pid, char *buf, size_t size) {
    task_struct_t *t = find_task_by_pid(pid);
    if (!t) return 0;

    /* Copy comm as argv[0] with null separator */
    size_t len = strlen(t->comm);
    if (len >= size) len = size - 1;
    memcpy(buf, t->comm, len);
    buf[len] = 0;
    return len + 1;
}

/* =========================================================
 * procfs file operations
 * ========================================================= */

static s64 proc_read(file_t *file, char *buf, size_t count, u64 *ppos) {
    if (!file || !buf) return -EFAULT;

    inode_t *inode = file->f_inode;
    if (!inode || !inode->i_private) return -EINVAL;

    proc_inode_info_t *info = (proc_inode_info_t *)inode->i_private;

    /* Generate content into a temp buffer */
    char *tmp = kmalloc(65536, GFP_KERNEL);
    if (!tmp) return -ENOMEM;

    size_t total = 0;
    switch (info->type) {
    case PROC_VERSION:      total = proc_gen_version(tmp, 65536);    break;
    case PROC_UPTIME:       total = proc_gen_uptime(tmp, 65536);     break;
    case PROC_MEMINFO:      total = proc_gen_meminfo(tmp, 65536);    break;
    case PROC_CPUINFO:      total = proc_gen_cpuinfo(tmp, 65536);    break;
    case PROC_STAT:         total = proc_gen_stat(tmp, 65536);       break;
    case PROC_LOADAVG:      total = proc_gen_loadavg(tmp, 65536);    break;
    case PROC_MOUNTS:       total = proc_gen_mounts(tmp, 65536);     break;
    case PROC_FILESYSTEMS:  total = proc_gen_filesystems(tmp, 65536);break;
    case PROC_PID_STATUS:   total = proc_gen_pid_status(info->pid, tmp, 65536); break;
    case PROC_PID_STAT:     total = proc_gen_pid_stat(info->pid, tmp, 65536);   break;
    case PROC_PID_MAPS:     total = proc_gen_pid_maps(info->pid, tmp, 65536);   break;
    case PROC_PID_CMDLINE:  total = proc_gen_pid_cmdline(info->pid, tmp, 65536);break;
    default:
        kfree(tmp);
        return 0;
    }

    /* Handle ppos offset */
    u64 pos = ppos ? *ppos : 0;
    s64 ret = 0;

    if (pos < total) {
        size_t avail = total - (size_t)pos;
        size_t n = (avail < count) ? avail : count;
        memcpy(buf, tmp + pos, n);
        if (ppos) *ppos = pos + n;
        ret = (s64)n;
    }

    kfree(tmp);
    return ret;
}

static const file_operations_t proc_file_fops = {
    .read  = proc_read,
};

/* =========================================================
 * procfs inode allocator
 * ========================================================= */

static inode_t *proc_alloc_inode(super_block_t *sb,
                                  proc_entry_type_t type, pid_t pid,
                                  u32 mode) {
    inode_t *inode = new_inode(sb);
    if (!inode) return NULL;

    proc_inode_info_t *info = kmalloc(sizeof(proc_inode_info_t), GFP_KERNEL);
    if (!info) { kfree(inode); return NULL; }
    info->type = type;
    info->pid  = pid;

    inode->i_mode    = mode;
    inode->i_private = info;
    inode->i_fop     = &proc_file_fops;
    inode->i_uid     = 0;
    inode->i_gid     = 0;
    inode->i_size    = 0;    /* Dynamic */
    inode->i_blocks  = 0;

    return inode;
}

/* =========================================================
 * procfs root directory
 * ========================================================= */

/* Forward declared — inode_operations lookup will build /proc/<pid> on demand */
static dentry_t *proc_root_lookup(inode_t *dir, dentry_t *dentry,
                                   unsigned int flags);

/* Enumerate /proc: static entries + one dir per live PID */
static s64 proc_root_readdir(struct file *file, void *ctx,
    int (*filldir)(void *, const char *, int, u64, unsigned int))
{
    /* Static entries */
    static const struct { const char *name; u32 mode; } statics[] = {
        { "version",     S_IFREG | 0444 },
        { "uptime",      S_IFREG | 0444 },
        { "meminfo",     S_IFREG | 0444 },
        { "cpuinfo",     S_IFREG | 0444 },
        { "stat",        S_IFREG | 0444 },
        { "loadavg",     S_IFREG | 0444 },
        { "mounts",      S_IFREG | 0444 },
        { "filesystems", S_IFREG | 0444 },
        { "self",        S_IFLNK | 0777 },
    };
    const int NSTATIC = (int)(sizeof(statics)/sizeof(statics[0]));

    s64 pos = file->f_pos;

    if (pos == 0) { if (filldir(ctx,".",1,1,DT_DIR)<0) return 0; file->f_pos=pos=1; }
    if (pos == 1) { if (filldir(ctx,"..",2,1,DT_DIR)<0) return 0; file->f_pos=pos=2; }

    while (pos >= 2 && pos < 2 + NSTATIC) {
        int i = (int)(pos - 2);
        unsigned int dtype = S_ISLNK(statics[i].mode) ? DT_LNK : DT_REG;
        if (filldir(ctx, statics[i].name, (int)strlen(statics[i].name),
                    (u64)(i + 10), dtype) < 0) return 0;
        file->f_pos = ++pos;
    }

    /* Per-PID directories */
    extern task_struct_t *task_table[];
    int scan_start = (int)(pos - (2 + NSTATIC));
    if (scan_start < 0) scan_start = 0;
    for (int i = scan_start; i < 512; i++) {
        task_struct_t *t = task_table[i];
        file->f_pos = (s64)(2 + NSTATIC + i + 1);
        if (!t || t->pid <= 0) continue;
        char pidbuf[16]; int plen = 0;
        pid_t p = t->pid;
        char tmp2[12]; int tl = 0;
        while (p > 0) { tmp2[tl++] = '0' + (p % 10); p /= 10; }
        for (int j = tl - 1; j >= 0; j--) pidbuf[plen++] = tmp2[j];
        if (filldir(ctx, pidbuf, plen, (u64)(1000 + t->pid), DT_DIR) < 0) return 0;
    }

    return 0;
}

static s64 proc_pid_dir_readdir(struct file *file, void *ctx,
    int (*filldir)(void *, const char *, int, u64, unsigned int))
{
    dentry_t *dentry = file->f_dentry;
    if (!dentry) return -ENOENT;

    s64 pos = file->f_pos;

    if (pos == 0) {
        u64 self_ino = dentry->d_inode ? dentry->d_inode->i_ino : 2;
        if (filldir(ctx, ".", 1, self_ino, DT_DIR) < 0) return 0;
        file->f_pos = pos = 1;
    }
    if (pos == 1) {
        u64 par_ino = (dentry->d_parent && dentry->d_parent->d_inode) ?
                       dentry->d_parent->d_inode->i_ino : 1;
        if (filldir(ctx, "..", 2, par_ino, DT_DIR) < 0) return 0;
        file->f_pos = pos = 2;
    }

    /* Walk d_subdirs for the rest */
    long skip = pos - 2;
    long idx = 0;
    struct list_head *p;
    list_for_each(p, &dentry->d_subdirs) {
        dentry_t *child = list_entry(p, dentry_t, d_child);
        if (!child->d_inode) continue;
        if (idx < skip) { idx++; continue; }
        unsigned int dtype = DT_REG;
        u32 mode = child->d_inode->i_mode;
        if (S_ISDIR(mode)) dtype = DT_DIR;
        else if (S_ISLNK(mode)) dtype = DT_LNK;
        else if (S_ISCHR(mode)) dtype = DT_CHR;
        if (filldir(ctx, (const char *)child->d_name.name,
                    (int)child->d_name.len,
                    child->d_inode->i_ino, dtype) < 0) return 0;
        file->f_pos = 2 + idx + 1;
        idx++;
    }
    return 0;
}

static const file_operations_t proc_pid_dir_fops = {
    .readdir = proc_pid_dir_readdir,
};

static const inode_operations_t proc_root_inode_ops = {
    .lookup = proc_root_lookup,
};

static const file_operations_t proc_dir_fops = {
    .readdir = proc_root_readdir,
};

static super_block_t *proc_sb = NULL;

static dentry_t *proc_make_entry(dentry_t *parent, const char *name,
                                  proc_entry_type_t type, pid_t pid,
                                  u32 mode) {
    qstr_t qname;
    qname.name = (const unsigned char *)name;
    qname.len  = (u32)strlen(name);
    qname.hash = 0;

    dentry_t *d = d_alloc(parent, &qname);
    if (!d) return NULL;

    inode_t *inode = proc_alloc_inode(proc_sb, type, pid, mode);
    if (!inode) { kfree(d); return NULL; }

    d_add(d, inode);
    return d;
}

/* Build /proc/<pid> directory tree */
static dentry_t *proc_build_pid_dir(dentry_t *parent, pid_t pid) {
    char name[16];
    int n = 0;
    pid_t tmp = pid;
    char rev[16]; int ri = 0;
    if (tmp == 0) { rev[ri++] = '0'; }
    while (tmp > 0) { rev[ri++] = '0' + tmp % 10; tmp /= 10; }
    while (ri > 0) name[n++] = rev[--ri];
    name[n] = 0;

    qstr_t qname;
    qname.name = (const unsigned char *)name;
    qname.len  = (u32)n;
    qname.hash = 0;

    dentry_t *pid_dir = d_alloc(parent, &qname);
    if (!pid_dir) return NULL;

    inode_t *dir_inode = proc_alloc_inode(proc_sb, PROC_PID_DIR, pid,
                                            S_IFDIR | 0555);
    if (!dir_inode) { kfree(pid_dir); return NULL; }

    d_add(pid_dir, dir_inode);

    /* Create entries inside */
    proc_make_entry(pid_dir, "status",  PROC_PID_STATUS,  pid, S_IFREG | 0444);
    proc_make_entry(pid_dir, "stat",    PROC_PID_STAT,    pid, S_IFREG | 0444);
    proc_make_entry(pid_dir, "maps",    PROC_PID_MAPS,    pid, S_IFREG | 0444);
    proc_make_entry(pid_dir, "cmdline", PROC_PID_CMDLINE, pid, S_IFREG | 0444);

    return pid_dir;
}

static dentry_t *proc_root_lookup(inode_t *dir, dentry_t *dentry,
                                   unsigned int flags) {
    (void)dir; (void)flags;
    const char *name = (const char *)dentry->d_name.name;

    /* Static proc entries */
    struct { const char *n; proc_entry_type_t t; u32 mode; } static_entries[] = {
        { "version",     PROC_VERSION,     S_IFREG | 0444 },
        { "uptime",      PROC_UPTIME,      S_IFREG | 0444 },
        { "meminfo",     PROC_MEMINFO,     S_IFREG | 0444 },
        { "cpuinfo",     PROC_CPUINFO,     S_IFREG | 0444 },
        { "stat",        PROC_STAT,        S_IFREG | 0444 },
        { "loadavg",     PROC_LOADAVG,     S_IFREG | 0444 },
        { "mounts",      PROC_MOUNTS,      S_IFREG | 0444 },
        { "filesystems", PROC_FILESYSTEMS, S_IFREG | 0444 },
        { NULL, 0, 0 },
    };

    for (int i = 0; static_entries[i].n; i++) {
        if (strcmp(name, static_entries[i].n) == 0) {
            inode_t *inode = proc_alloc_inode(proc_sb,
                                               static_entries[i].t, 0,
                                               static_entries[i].mode);
            if (!inode) return ERR_PTR(-ENOMEM);
            d_add(dentry, inode);
            return NULL;  /* NULL = success in lookup */
        }
    }

    /* "self" -> /proc/<current_pid> */
    if (strcmp(name, "self") == 0) {
        pid_t cpid = current ? current->pid : 1;
        inode_t *dir_inode = proc_alloc_inode(proc_sb, PROC_PID_DIR, cpid,
                                               S_IFDIR | 0555);
        if (!dir_inode) return ERR_PTR(-ENOMEM);
        dir_inode->i_fop = &proc_pid_dir_fops;
        d_add(dentry, dir_inode);
        proc_make_entry(dentry, "status",  PROC_PID_STATUS,  cpid, S_IFREG | 0444);
        proc_make_entry(dentry, "stat",    PROC_PID_STAT,    cpid, S_IFREG | 0444);
        proc_make_entry(dentry, "maps",    PROC_PID_MAPS,    cpid, S_IFREG | 0444);
        proc_make_entry(dentry, "cmdline", PROC_PID_CMDLINE, cpid, S_IFREG | 0444);
        return NULL;
    }

    /* Numeric PID lookup */
    pid_t pid = 0;
    bool numeric = true;
    for (u32 i = 0; i < dentry->d_name.len; i++) {
        char c = name[i];
        if (c < '0' || c > '9') { numeric = false; break; }
        pid = pid * 10 + (c - '0');
    }

    if (numeric && pid > 0) {
        task_struct_t *t = find_task_by_pid(pid);
        if (!t) return ERR_PTR(-ENOENT);

        inode_t *dir_inode = proc_alloc_inode(proc_sb, PROC_PID_DIR, pid,
                                               S_IFDIR | 0555);
        if (!dir_inode) return ERR_PTR(-ENOMEM);
        dir_inode->i_fop = &proc_pid_dir_fops;
        d_add(dentry, dir_inode);

        /* Add per-PID file entries as children of THIS dentry */
        proc_make_entry(dentry, "status",  PROC_PID_STATUS,  pid, S_IFREG | 0444);
        proc_make_entry(dentry, "stat",    PROC_PID_STAT,    pid, S_IFREG | 0444);
        proc_make_entry(dentry, "maps",    PROC_PID_MAPS,    pid, S_IFREG | 0444);
        proc_make_entry(dentry, "cmdline", PROC_PID_CMDLINE, pid, S_IFREG | 0444);

        return NULL;
    }

    return ERR_PTR(-ENOENT);
}

/* =========================================================
 * procfs superblock + mount
 * ========================================================= */

static int procfs_fill_super(super_block_t *sb, void *data, int silent) {
    (void)data; (void)silent;

    sb->s_magic     = 0x9FA0;   /* PROC_SUPER_MAGIC */
    sb->s_blocksize = 4096;
    sb->s_maxbytes  = ~0ULL;
    proc_sb = sb;

    /* Root inode */
    inode_t *root = new_inode(sb);
    if (!root) return -ENOMEM;

    root->i_mode    = S_IFDIR | 0555;
    root->i_op      = &proc_root_inode_ops;
    root->i_fop     = &proc_dir_fops;
    root->i_uid     = 0;
    root->i_gid     = 0;

    proc_inode_info_t *info = kmalloc(sizeof(proc_inode_info_t), GFP_KERNEL);
    if (info) { info->type = PROC_VERSION; info->pid = 0; }
    root->i_private = info;

    sb->s_root = d_make_root(root);
    if (!sb->s_root) return -ENOMEM;

    /* Pre-create a few static entries at mount time */
    proc_make_entry(sb->s_root, "version",     PROC_VERSION,     0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "uptime",      PROC_UPTIME,      0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "meminfo",     PROC_MEMINFO,     0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "cpuinfo",     PROC_CPUINFO,     0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "stat",        PROC_STAT,        0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "loadavg",     PROC_LOADAVG,     0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "mounts",      PROC_MOUNTS,      0, S_IFREG | 0444);
    proc_make_entry(sb->s_root, "filesystems", PROC_FILESYSTEMS, 0, S_IFREG | 0444);

    return 0;
}

static struct dentry *procfs_mount(file_system_type_t *fs_type, int flags,
                                    const char *dev_name, void *data) {
    (void)dev_name; (void)flags; (void)fs_type; (void)data;

    super_block_t *sb = kmalloc(sizeof(super_block_t), GFP_KERNEL);
    if (!sb) return ERR_PTR(-ENOMEM);
    memset(sb, 0, sizeof(*sb));

    if (procfs_fill_super(sb, data, 0) != 0) {
        kfree(sb);
        return ERR_PTR(-EINVAL);
    }

    return sb->s_root;
}

static void procfs_kill_sb(super_block_t *sb) {
    (void)sb;
}

file_system_type_t procfs_fs_type = {
    .name    = "proc",
    .fs_flags = 0,
    .mount   = procfs_mount,
    .kill_sb = procfs_kill_sb,
};

int init_procfs(void) {
    printk(KERN_INFO "[procfs] initializing /proc\n");
    register_filesystem(&procfs_fs_type);

    /* Mount at /proc */
    int err = do_mount(NULL, "/proc", "proc", 0, NULL);
    if (err && err != -ENOENT) {
        printk(KERN_WARNING "[procfs] mount failed: %d (creating /proc)\n", err);
        /* Try to create /proc directory first */
        vfs_mkdir(NULL, NULL, 0555);
        do_mount(NULL, "/proc", "proc", 0, NULL);
    }

    printk(KERN_INFO "[procfs] /proc mounted\n");
    return 0;
}
