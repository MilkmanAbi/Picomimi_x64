/**
 * Picomimi-x64 ELF Loader
 *
 * Loads ELF64 executables for execve(2):
 *   - Validates ELF header (magic, class=64, type=ET_EXEC/ET_DYN)
 *   - Processes PT_LOAD segments (mmap into new mm_struct)
 *   - PT_INTERP: loads dynamic linker, sets up handoff
 *   - PT_GNU_STACK: controls stack exec permissions
 *   - Builds initial user stack:
 *       [argc] [argv ptrs] [NULL] [envp ptrs] [NULL] [auxv] [data]
 *   - Sets up AT_* auxiliary vector (PHDR, PHNUM, ENTRY, PAGESZ, etc.)
 *   - Clears BSS
 *   - Supports static and PIE executables
 *   - Position-independent executables (ET_DYN) loaded at 0x400000
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/elf.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>





/* =========================================================
 * VMM interface (from mm/advanced_mm.c / vmm.c)
 * ========================================================= */
extern void *vmm_map_pages(u64 virt, u64 phys, u64 size, u32 flags);
extern u64   vmm_alloc_pages(u64 virt, u64 size, u32 flags);
extern void  vmm_free_pages(u64 virt, u64 size);

/* Page protection flags for vmm */
#define VMM_PROT_READ    0x01
#define VMM_PROT_WRITE   0x02
#define VMM_PROT_EXEC    0x04
#define VMM_PROT_USER    0x08

/* =========================================================
 * ELF validation
 * ========================================================= */

static int elf_check_header(const Elf64_Ehdr *ehdr) {
    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3)
        return -ENOEXEC;

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        return -ENOEXEC;   /* Only 64-bit */

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -ENOEXEC;   /* Only little-endian */

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -ENOEXEC;

    if (ehdr->e_machine != EM_X86_64)
        return -ENOEXEC;

    if (ehdr->e_version != EV_CURRENT)
        return -ENOEXEC;

    if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
        return -ENOEXEC;

    if (ehdr->e_phnum == 0 )
        return -ENOEXEC;

    return 0;
}

/* =========================================================
 * Load a single PT_LOAD segment
 * ========================================================= */

int elf_load_segment(file_t *file, const Elf64_Phdr *phdr,
                              u64 load_bias, mm_struct_t *mm) {
    u64 vaddr = phdr->p_vaddr + load_bias;
    u64 filesz = phdr->p_filesz;
    u64 memsz  = phdr->p_memsz;
    u64 offset = phdr->p_offset;

    if (memsz == 0) return 0;

    /* Page-align the mapping */
    u64 map_start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    u64 map_end   = PAGE_ALIGN(vaddr + memsz);
    u64 map_size  = map_end - map_start;

    /* Build protection flags */
    u32 prot = VMM_PROT_USER;
    if (phdr->p_flags & PF_R) prot |= VMM_PROT_READ;
    if (phdr->p_flags & PF_W) prot |= VMM_PROT_WRITE;
    if (phdr->p_flags & PF_X) prot |= VMM_PROT_EXEC;

    /* Allocate physical pages and map them */
    u64 result = vmm_alloc_pages(map_start, map_size, prot);
    if (!result) {
        printk(KERN_ERR "[elf] failed to allocate segment at %016llx size %llx\n",
               map_start, map_size);
        return -ENOMEM;
    }

    /* Zero and populate pages via physical addresses to avoid the
     * identity-map vs user-PML4 aliasing problem.
     * 
     * The kernel identity maps 0..1GB: writing to vaddr directly would
     * hit phys==vaddr (identity), not the newly allocated physical page.
     * We must use phys_to_virt(vmm_virt_to_phys(vaddr)) to write through
     * the KERNEL HALF alias of the correct physical page.
     */
    {
        extern u64 vmm_user_virt_to_phys(u64 virt);
        #define vmm_virt_to_phys vmm_user_virt_to_phys
        #define PHYS_TO_VIRT_ELF(p) ((void *)((u64)(p) + 0xFFFFFFFF80000000ULL))

        /* Zero pages via phys alias */
        for (u64 pg = map_start; pg < map_end; pg += PAGE_SIZE) {
            u64 phys = vmm_virt_to_phys(pg);
            if (phys) memset(PHYS_TO_VIRT_ELF(phys), 0, PAGE_SIZE);
        }

        /* Copy file data page by page via phys alias */
        if (filesz > 0) {
            u64 copy_vaddr = vaddr;       /* destination user vaddr */
            u64 remaining  = filesz;
            u64 file_pos   = offset;

            while (remaining > 0) {
                /* How much to copy in this page? */
                u64 pg_base   = copy_vaddr & ~(u64)(PAGE_SIZE - 1);
                u64 pg_off    = copy_vaddr - pg_base;
                u64 pg_space  = PAGE_SIZE - pg_off;
                u64 chunk     = remaining < pg_space ? remaining : pg_space;

                u64 phys = vmm_virt_to_phys(pg_base);
                if (!phys) { printk(KERN_ERR "[elf] no phys for 0x%llx\n", pg_base); return -EFAULT; }

                void *dest = (char *)PHYS_TO_VIRT_ELF(phys) + pg_off;
                s64 n = vfs_read(file, dest, (size_t)chunk, &file_pos);
                if (n <= 0) {
                    printk(KERN_ERR "[elf] read error at vaddr 0x%llx: %lld\n", copy_vaddr, n);
                    return n < 0 ? (int)n : -EIO;
                }
                copy_vaddr += (u64)n;
                remaining  -= (u64)n;
            }
        }
        #undef PHYS_TO_VIRT_ELF
        #undef vmm_virt_to_phys
    }

    /* Register VMA */
    if (mm) {
        vm_area_t *vma = vma_alloc();
        if (vma) {
            vma->start    = map_start;
            vma->end      = map_end;
            vma->flags    = 0;
            if (phdr->p_flags & PF_R) vma->flags |= VM_READ;
            if (phdr->p_flags & PF_W) vma->flags |= VM_WRITE;
            if (phdr->p_flags & PF_X) vma->flags |= VM_EXEC;
            vma->vm_flags = 0;
            vma->file     = NULL;
            vma->offset   = 0;
            insert_vma(mm, vma);
            mm->total_vm += map_size / PAGE_SIZE;
        }
    }

    return 0;
}

/* =========================================================
 * Build user stack
 *
 * Stack layout (grows down from USER_STACK_TOP):
 *   ... strings (argv, envp, platform) ...
 *   AT_NULL pair
 *   auxv[] pairs
 *   NULL (envp terminator)
 *   envp[n-1] ... envp[0]
 *   NULL (argv terminator)
 *   argv[argc-1] ... argv[0]
 *   argc
 *   ← rsp points here
 * ========================================================= */

u64 build_user_stack(mm_struct_t *mm,
                              char *const argv[], char *const envp[],
                              const Elf64_Ehdr *ehdr,
                              const Elf64_Phdr *phdrs,
                              u64 load_bias, u64 interp_base,
                              u64 *out_rsp) {
    /* Allocate stack pages */
    u64 stack_top  = USER_STACK_TOP;
    u64 stack_base = stack_top - USER_STACK_SIZE;

    u64 r = vmm_alloc_pages(stack_base, USER_STACK_SIZE,
                             VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER);
    if (!r) return 0;

    memset((void *)stack_base, 0, USER_STACK_SIZE);

    /* Register stack VMA */
    if (mm) {
        vm_area_t *vma = vma_alloc();
        if (vma) {
            vma->start    = stack_base;
            vma->end      = stack_top;
            vma->flags    = VM_READ | VM_WRITE | VM_GROWSDOWN;
            vma->vm_flags = VM_GROWSDOWN;
            insert_vma(mm, vma);
            mm->start_stack = stack_top;
            mm->stack_vm   += USER_STACK_SIZE / PAGE_SIZE;
        }
    }

    /* Count argv, envp */
    int argc = 0;
    if (argv) { while (argv[argc]) argc++; }

    int envc = 0;
    if (envp) { while (envp[envc]) envc++; }

    /* Copy strings to top of stack, building pointer arrays */
    u64 sp = stack_top;

    /* Helper: push a string onto stack, return pointer */
    #define PUSH_STR(str) ({ \
        size_t _len = strlen(str) + 1; \
        sp -= _len; \
        memcpy((void *)sp, (str), _len); \
        sp; \
    })

    /* platform string */
    sp -= 16;
    sp &= ~0xFULL;  /* Align */
    memcpy((void *)sp, "x86_64", 7);
    u64 platform_ptr = sp;

    /* Random bytes for AT_RANDOM (16 bytes) */
    sp -= 16;
    sp &= ~0xFULL;
    /* Use simple pseudo-random fill */
    extern u64 prng_next(void) __attribute__((weak));
    for (int i = 0; i < 16; i += 8) {
        u64 v = prng_next ? prng_next() : 0xDEADBEEFCAFEBABEULL;
        memcpy((void *)(sp + i), &v, 8);
    }
    u64 random_ptr = sp;

    /* Copy envp strings */
    u64 *envp_ptrs = kmalloc(((size_t)envc + 1) * sizeof(u64), GFP_KERNEL);
    if (!envp_ptrs) return 0;
    for (int i = envc - 1; i >= 0; i--) {
        envp_ptrs[i] = PUSH_STR(envp[i]);
    }
    envp_ptrs[envc] = 0;

    /* Copy argv strings */
    u64 *argv_ptrs = kmalloc(((size_t)argc + 1) * sizeof(u64), GFP_KERNEL);
    if (!argv_ptrs) { kfree(envp_ptrs); return 0; }
    for (int i = argc - 1; i >= 0; i--) {
        argv_ptrs[i] = PUSH_STR(argv[i]);
    }
    argv_ptrs[argc] = 0;

    /* Align sp to 16 bytes */
    sp &= ~0xFULL;

    /* Count auxv entries (each is 2 × u64) */
    int n_auxv = 14; /* We'll push 14 entries + AT_NULL */
    /* Ensure 16-byte alignment after all pushes:
     * total_items = 1 (argc) + (argc+1) + (envc+1) + n_auxv*2 + 2
     * Adjust with padding word if needed */
    int total_words = 1 + (argc + 1) + (envc + 1) + (n_auxv + 1) * 2;
    if (total_words % 2 != 0) {
        sp -= 8;  /* padding */
    }

    /* Push auxiliary vector (AT_NULL last = first push in reverse) */
    #define PUSH64(v) do { sp -= 8; *(u64 *)sp = (u64)(v); } while(0)

    /* AT_NULL */
    PUSH64(0); PUSH64(AT_NULL);

    /* auxv entries (in reverse push order → correct memory order) */
    PUSH64(0);              PUSH64(AT_SECURE);
    PUSH64(platform_ptr);   PUSH64(AT_PLATFORM);
    PUSH64(random_ptr);     PUSH64(AT_RANDOM);
    PUSH64(0);              PUSH64(AT_HWCAP2);
    PUSH64(0x1f8bfbff);     PUSH64(AT_HWCAP);
    PUSH64(100);            PUSH64(AT_CLKTCK);
    PUSH64(0);              PUSH64(AT_FLAGS);
    PUSH64(interp_base);    PUSH64(AT_BASE);
    PUSH64(ehdr->e_entry + load_bias); PUSH64(AT_ENTRY);
    PUSH64(PAGE_SIZE);      PUSH64(AT_PAGESZ);
    PUSH64(ehdr->e_phnum);  PUSH64(AT_PHNUM);
    PUSH64(sizeof(Elf64_Phdr)); PUSH64(AT_PHENT);
    /* AT_PHDR: virtual address where ELF program headers are mapped.
     * For ET_EXEC: find the first PT_LOAD to get its vaddr and offset,
     * then AT_PHDR = first_load_vaddr - first_load_offset + e_phoff + load_bias */
    {
        u64 at_phdr = load_bias + ehdr->e_phoff;  /* fallback */
        for (int _i = 0; _i < ehdr->e_phnum; _i++) {
            if (phdrs[_i].p_type == PT_LOAD) {
                at_phdr = phdrs[_i].p_vaddr - phdrs[_i].p_offset
                          + ehdr->e_phoff + load_bias;
                break;
            }
        }
        PUSH64(at_phdr);
    }
                            PUSH64(AT_PHDR);
    /* AT_EXECFN */
    const char *execfn = argv_ptrs[0] ? (const char *)argv_ptrs[0] : "";
    PUSH64((u64)(uintptr_t)execfn); PUSH64(AT_EXECFN);
    (void)execfn;

    /* NULL terminator for envp */
    PUSH64(0);

    /* envp pointers (reverse: last first) */
    for (int i = envc - 1; i >= 0; i--) PUSH64(envp_ptrs[i]);

    /* NULL terminator for argv */
    PUSH64(0);

    /* argv pointers (reverse) */
    for (int i = argc - 1; i >= 0; i--) PUSH64(argv_ptrs[i]);

    /* argc */
    PUSH64((u64)argc);

    *out_rsp = sp;

    kfree(argv_ptrs);
    kfree(envp_ptrs);

    return sp;

    #undef PUSH_STR
    #undef PUSH64
}

/* =========================================================
 * do_execve — the real execve implementation
 * ========================================================= */


/* =========================================================
 * ELF probe — check if file is an ELF binary
 * ========================================================= */

bool elf_check_file(file_t *file) {
    if (!file) return false;
    unsigned char magic[4];
    u64 pos = 0;
    s64 n = vfs_read(file, (char *)magic, 4, &pos);
    if (n != 4) return false;
    return (magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
            magic[2] == ELFMAG2 && magic[3] == ELFMAG3);
}
