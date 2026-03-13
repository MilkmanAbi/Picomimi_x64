/**
 * Picomimi-x64 execve implementation
 * kernel/exec.c
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/elf.h>
#include <kernel/macho.h>
#include <kernel/xnu_compat.h>
#include <kernel/syscall.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <arch/gdt.h>

/* macho_loader.c */
extern bool macho_probe_file(file_t *file);
extern int  macho_load_file(file_t *file, mm_struct_t *mm,
                             macho_load_info_t *info);

/* elf_loader.c exports */
extern int  elf_load_segment(file_t *file, const Elf64_Phdr *phdr,
                              u64 load_bias, mm_struct_t *mm);
extern u64  build_user_stack(mm_struct_t *mm,
                              char *const argv[], char *const envp[],
                              const Elf64_Ehdr *ehdr,
                              const Elf64_Phdr *phdrs,
                              u64 load_bias, u64 interp_base,
                              u64 *out_rsp);
extern u64 *kernel_pml4;

/* ------------------------------------------------------------------ */

static char **copy_strarray(char *const src[], int *out_count) {
    if (!src) { if (out_count) *out_count = 0; return NULL; }
    int n = 0;
    while (src[n]) n++;
    if (out_count) *out_count = n;
    char **dst = kmalloc((size_t)(n + 1) * sizeof(char *), GFP_KERNEL);
    if (!dst) return NULL;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(src[i]) + 1;
        dst[i] = kmalloc(len, GFP_KERNEL);
        if (!dst[i]) {
            for (int j = 0; j < i; j++) kfree(dst[j]);
            kfree(dst);
            return NULL;
        }
        memcpy(dst[i], src[i], len);
    }
    dst[n] = NULL;
    return dst;
}

static void free_strarray(char **arr, int n) {
    if (!arr) return;
    for (int i = 0; i < n; i++) kfree(arr[i]);
    kfree(arr);
}

static void switch_mm_to(mm_struct_t *mm) {
    /* mm->pgd is already a physical address (from pmm_alloc_page) */
    write_cr3((u64)mm->pgd);
}

static void restore_kernel_mm(void) {
    /* kernel_pml4 = read_cr3() at boot = already physical */
    write_cr3((u64)kernel_pml4);
}

/* ------------------------------------------------------------------ */

int do_execve(const char *filename, char *const argv[], char *const envp[]) {
    int ret = -ENOENT;
    task_struct_t *proc = get_current_task();

    if (!filename) return -EFAULT;

    /* Copy filename to kernel before address space switch */
    char k_filename[256];
    {
        int i = 0;
        while (i < 255 && filename[i]) { k_filename[i] = filename[i]; i++; }
        k_filename[i] = 0;
    }

    /* 1. Copy argv/envp to kernel before we switch address spaces */
    int argc = 0, envc = 0;
    char **k_argv = copy_strarray(argv, &argc);
    char **k_envp = copy_strarray(envp, &envc);

    /* 2. Open binary file */
    file_t *file = filp_open(k_filename, O_RDONLY, 0);
    if (!file || IS_ERR(file)) { ret = -ENOENT; goto err_free; }

    /* ================================================================
     * Detect binary format: Mach-O (fat or thin) vs ELF
     * ================================================================ */
    if (macho_probe_file(file)) {
        /* ----------- MACH-O PATH ----------- */
        mm_struct_t *new_mm = mm_alloc();
        if (!new_mm) { ret = -ENOMEM; goto err_close; }

        mm_struct_t *old_mm = proc->mm;
        switch_mm_to(new_mm);
        proc->mm        = new_mm;
        proc->active_mm = new_mm;

        macho_load_info_t minfo;
        ret = macho_load_file(file, new_mm, &minfo);
        if (ret < 0) { printk(KERN_ERR "[exec] Mach-O failed: %d\n", ret); goto kill_macho; }

        if (minfo.seg_end) {
            new_mm->end_data  = minfo.seg_end;
            new_mm->start_brk = PAGE_ALIGN(minfo.seg_end);
            new_mm->brk       = new_mm->start_brk;
        }

        /* Build stack — pass a zeroed fake ELF header (no auxv needed for Mach-O) */
        Elf64_Ehdr fake_ehdr; memset(&fake_ehdr, 0, sizeof(fake_ehdr));
        fake_ehdr.e_entry     = minfo.entry;
        fake_ehdr.e_phentsize = sizeof(Elf64_Phdr);
        u64 user_rsp = 0;
        if (!build_user_stack(new_mm, (char *const *)k_argv, (char *const *)k_envp,
                              &fake_ehdr, NULL, 0, 0, &user_rsp)) {
            ret = -ENOMEM; goto kill_macho;
        }

        /* Close O_CLOEXEC fds */
        if (proc->files) {
            files_struct_t *fs = proc->files;
            int max = fs->max_fds < MAX_FDS_PER_PROCESS ? fs->max_fds : MAX_FDS_PER_PROCESS;
            for (int fd = 0; fd < max; fd++) {
                fd_entry_t *fde = &fs->fd_array[fd];
                if (fde->file && (u64)fde->file > 0x1000 && (fde->flags & FD_CLOEXEC)) {
                    filp_close(fde->file, NULL); fde->file = NULL; fde->flags = 0;
                }
            }
        }

        /* Reset signal handlers */
        if (proc->sighand) {
            for (int i = 0; i < NSIG; i++) {
                if (proc->sighand->action[i].sa_handler != SIG_IGN)
                    memset(&proc->sighand->action[i], 0, sizeof(proc->sighand->action[i]));
            }
        }

        /* Tag personality as XNU */
        proc->personality = PERSONALITY_XNU;
        if (proc->mach_ports) { kfree(proc->mach_ports); proc->mach_ports = NULL; }
        proc->mach_task_port = proc->mach_thread_port = 0;
        /* mach_ports_init_task() called lazily on first Mach trap */

        if (old_mm) mm_free(old_mm);

        /* Commit comm name */
        { const char *b = k_filename; for (const char *p = k_filename; *p; p++) if (*p == '/') b = p+1;
          size_t nl = strlen(b); if (nl > 15) nl = 15; memcpy(proc->comm, b, nl); proc->comm[nl] = 0; }

        filp_close(file, NULL);
        free_strarray(k_argv, argc);
        free_strarray(k_envp, envc);

        trap_frame_t frame; memset(&frame, 0, sizeof(frame));
        frame.rip = minfo.entry; frame.cs = USER_CS;
        frame.rflags = 0x202; frame.rsp = user_rsp; frame.ss = USER_DS;
        proc->tls = 0;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100UL), "a"(0), "d"(0));
        switch_to_user(&frame);
        __builtin_unreachable();
    }

    /* ================================================================
     * ELF PATH (original, unchanged)
     * ================================================================ */

    /* 3. Read + validate ELF header */
    Elf64_Ehdr *ehdr  = NULL;
    Elf64_Phdr *phdrs = NULL;
    ehdr = kmalloc(sizeof(Elf64_Ehdr), GFP_KERNEL);
    if (!ehdr) { ret = -ENOMEM; goto err_close; }
    {
        u64 pos = 0;
        if (vfs_read(file, (char *)ehdr, sizeof(*ehdr), (s64*)&pos)
                != (s64)sizeof(*ehdr)) {
            ret = -ENOEXEC; goto err_ehdr;
        }
    }
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS]  != ELFCLASS64  ||
        ehdr->e_ident[EI_DATA]   != ELFDATA2LSB ||
        ehdr->e_machine          != EM_X86_64   ||
        (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0) {
        ret = -ENOEXEC; goto err_ehdr;
    }

    /* 4. Read program headers */
    size_t ph_sz = (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    phdrs = kmalloc(ph_sz, GFP_KERNEL);
    if (!phdrs) { ret = -ENOMEM; goto err_ehdr; }
    {
        u64 pos = ehdr->e_phoff;
        if (vfs_read(file, (char *)phdrs, ph_sz, (s64*)&pos) != (s64)ph_sz) {
            ret = -ENOEXEC; goto err_phdrs;
        }
    }

    /* 5. New address space */
    mm_struct_t *new_mm = mm_alloc();
    if (!new_mm) { ret = -ENOMEM; goto err_phdrs; }

    /* ---- POINT OF NO RETURN ---- */
    mm_struct_t *old_mm = proc->mm;
    switch_mm_to(new_mm);
    proc->mm        = new_mm;
    proc->active_mm = new_mm;

    /* 6. Load bias: PIE at 0x400000, ET_EXEC at 0 */
    u64 load_bias = (ehdr->e_type == ET_DYN) ? 0x400000ULL : 0ULL;

    /* 7. Load PT_LOAD segments */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if ((ret = elf_load_segment(file, &phdrs[i], load_bias, new_mm)) < 0) {
            printk(KERN_ERR "[exec] segment %d load failed: %d\n", i, ret);
            goto kill;
        }
    }

    /* 7b. Set mm->brk to end of loaded data (page-aligned end of last PT_LOAD) */
    {
        u64 highest = 0;
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type != PT_LOAD) continue;
            u64 seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz + load_bias;
            if (seg_end > highest) highest = seg_end;
        }
        if (highest) {
            /* PAGE_ALIGN rounds up to next page */
            new_mm->end_data  = highest;
            new_mm->start_brk = (highest + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
            new_mm->brk       = new_mm->start_brk;
        }
    }

    /* 8. Build user stack */
    u64 user_rsp = 0;
    if (!build_user_stack(new_mm,
                          (char *const *)k_argv, (char *const *)k_envp,
                          ehdr, phdrs, load_bias, 0, &user_rsp)) {
        ret = -ENOMEM; goto kill;
    }

    /* 9. Close O_CLOEXEC fds */
    if (proc->files) {
        files_struct_t *fs = proc->files;
        int max = fs->max_fds < MAX_FDS_PER_PROCESS
                ? fs->max_fds : MAX_FDS_PER_PROCESS;
        for (int fd = 0; fd < max; fd++) {
            fd_entry_t *fde = &fs->fd_array[fd];
            if (fde->file && (u64)fde->file > 0x1000 && (fde->flags & FD_CLOEXEC)) {
                filp_close(fde->file, NULL);
                fde->file  = NULL;
                fde->flags = 0;
            }
        }
    }

    /* 10. Reset signal handlers (keep ignored/blocked as per POSIX) */
    if (!proc->sighand) {
        proc->sighand = kmalloc(sizeof(sighand_struct_t), GFP_KERNEL);
        if (proc->sighand) {
            memset(proc->sighand, 0, sizeof(sighand_struct_t));
            atomic_set(&proc->sighand->count, 1);
        }
    }
    if (!proc->signal) {
        proc->signal = kmalloc(sizeof(signal_struct_t), GFP_KERNEL);
        if (proc->signal) {
            memset(proc->signal, 0, sizeof(signal_struct_t));
            atomic_set(&proc->signal->count, 1);
            proc->signal->pgrp    = proc->pid;
            proc->signal->session = proc->pid;
        }
    }
    if (proc->sighand) {
        for (int i = 0; i < NSIG; i++) {
            void *h = proc->sighand->action[i].sa_handler;
            /* SIG_IGN stays ignored across exec; SIG_DFL already default */
            if (h != SIG_IGN)
                memset(&proc->sighand->action[i], 0,
                       sizeof(proc->sighand->action[i]));
        }
    }

    /* 11. Drop old mm */
    if (old_mm) mm_free(old_mm);

    /* ELF exec: ensure Linux personality (even if previous exec was Mach-O) */
    proc->personality = PERSONALITY_LINUX;
    if (proc->mach_ports) { kfree(proc->mach_ports); proc->mach_ports = NULL; }
    proc->mach_task_port = proc->mach_thread_port = 0;

    kfree(phdrs);
    kfree(ehdr);
    filp_close(file, NULL);
    free_strarray(k_argv, argc);
    free_strarray(k_envp, envc);

    /* 12. Jump to userspace entry via iretq */
    u64 entry = ehdr->e_entry + load_bias;

    trap_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.rip    = entry;
    frame.cs     = USER_CS;
    frame.rflags = 0x202;   /* IF=1 */
    frame.rsp    = user_rsp;
    frame.ss     = USER_DS;

    /* Reset TLS — old address is gone after mm_free. glibc will call
     * arch_prctl(ARCH_SET_FS) during startup to set a fresh TLS. */
    proc->tls = 0;
    __asm__ volatile("wrmsr" :: "c"(0xC0000100UL), "a"(0), "d"(0));

    switch_to_user(&frame);
    __builtin_unreachable();

kill_macho:
    restore_kernel_mm();
    filp_close(file, NULL);
    free_strarray(k_argv, argc);
    free_strarray(k_envp, envc);
    sys_exit(1);
    __builtin_unreachable();

kill:
    restore_kernel_mm();
    kfree(phdrs);
    kfree(ehdr);
    filp_close(file, NULL);
    free_strarray(k_argv, argc);
    free_strarray(k_envp, envc);
    sys_exit(1);
    __builtin_unreachable();

err_phdrs:  kfree(phdrs);
err_ehdr:   kfree(ehdr);
err_close:  filp_close(file, NULL);
err_free:
    free_strarray(k_argv, argc);
    free_strarray(k_envp, envc);
    return ret;
}
