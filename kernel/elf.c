/**
 * Picomimi-x64 ELF Loader
 * 
 * Load and execute ELF64 binaries
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>

// External spinlock functions
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);

// Align macros
#ifndef ALIGN_UP
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

// PTE flags (some may not be defined in vmm.h)
#ifndef PTE_WRITABLE
#define PTE_WRITABLE    PTE_WRITE
#endif
#ifndef PTE_NO_EXECUTE
#define PTE_NO_EXECUTE  0x8000000000000000ULL
#endif

// ============================================================================
// ELF DEFINITIONS
// ============================================================================

#define EI_NIDENT       16

#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8

#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASS64      2
#define ELFDATA2LSB     1

#define ET_EXEC         2
#define ET_DYN          3

#define EM_X86_64       62

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_SHLIB       10
#define SHT_DYNSYM      11

#define AT_NULL         0
#define AT_IGNORE       1
#define AT_EXECFD       2
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_NOTELF       10
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_PLATFORM     15
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM       25
#define AT_EXECFN       31

// ============================================================================
// ELF STRUCTURES
// ============================================================================

// ELF64 Header
typedef struct {
    unsigned char   e_ident[EI_NIDENT];
    u16             e_type;
    u16             e_machine;
    u32             e_version;
    u64             e_entry;
    u64             e_phoff;
    u64             e_shoff;
    u32             e_flags;
    u16             e_ehsize;
    u16             e_phentsize;
    u16             e_phnum;
    u16             e_shentsize;
    u16             e_shnum;
    u16             e_shstrndx;
} __packed Elf64_Ehdr;

// ELF64 Program Header
typedef struct {
    u32             p_type;
    u32             p_flags;
    u64             p_offset;
    u64             p_vaddr;
    u64             p_paddr;
    u64             p_filesz;
    u64             p_memsz;
    u64             p_align;
} __packed Elf64_Phdr;

// ELF64 Section Header
typedef struct {
    u32             sh_name;
    u32             sh_type;
    u64             sh_flags;
    u64             sh_addr;
    u64             sh_offset;
    u64             sh_size;
    u32             sh_link;
    u32             sh_info;
    u64             sh_addralign;
    u64             sh_entsize;
} __packed Elf64_Shdr;

// Auxiliary vector entry
typedef struct {
    u64             a_type;
    u64             a_val;
} Elf64_auxv_t;

// ============================================================================
// ELF VALIDATION
// ============================================================================

static int elf_check_header(const Elf64_Ehdr *hdr) {
    // Check magic number
    if (hdr->e_ident[EI_MAG0] != ELFMAG0 ||
        hdr->e_ident[EI_MAG1] != ELFMAG1 ||
        hdr->e_ident[EI_MAG2] != ELFMAG2 ||
        hdr->e_ident[EI_MAG3] != ELFMAG3) {
        printk(KERN_ERR "ELF: Invalid magic number\n");
        return -ENOEXEC;
    }
    
    // Check class (64-bit)
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64) {
        printk(KERN_ERR "ELF: Not a 64-bit ELF\n");
        return -ENOEXEC;
    }
    
    // Check endianness (little endian)
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        printk(KERN_ERR "ELF: Not little-endian\n");
        return -ENOEXEC;
    }
    
    // Check machine type (x86_64)
    if (hdr->e_machine != EM_X86_64) {
        printk(KERN_ERR "ELF: Not x86_64 binary\n");
        return -ENOEXEC;
    }
    
    // Check type (executable or shared object)
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        printk(KERN_ERR "ELF: Not executable or shared object\n");
        return -ENOEXEC;
    }
    
    return 0;
}

// ============================================================================
// SEGMENT LOADING
// ============================================================================

static int prot_to_vm_flags(u32 p_flags) {
    int flags = 0;
    if (p_flags & PF_R) flags |= VM_READ;
    if (p_flags & PF_W) flags |= VM_WRITE;
    if (p_flags & PF_X) flags |= VM_EXEC;
    return flags;
}

static int load_segment(mm_struct_t *mm, const u8 *elf_data, const Elf64_Phdr *phdr) {
    if (phdr->p_type != PT_LOAD) {
        return 0;  // Skip non-loadable segments
    }
    
    u64 vaddr = phdr->p_vaddr;
    u64 memsz = phdr->p_memsz;
    u64 filesz = phdr->p_filesz;
    u64 offset = phdr->p_offset;
    
    // Align to page boundaries
    u64 page_offset = vaddr & (PAGE_SIZE - 1);
    u64 aligned_vaddr = vaddr & ~(PAGE_SIZE - 1);
    u64 aligned_memsz = ALIGN_UP(memsz + page_offset, PAGE_SIZE);
    
    printk(KERN_DEBUG "ELF: Loading segment: vaddr=0x%lx memsz=0x%lx filesz=0x%lx\n",
           vaddr, memsz, filesz);
    
    // Create VMA for this segment
    vm_area_t *vma = kmalloc(sizeof(vm_area_t), GFP_KERNEL);
    if (!vma) {
        return -ENOMEM;
    }
    
    memset(vma, 0, sizeof(vm_area_t));
    vma->start = aligned_vaddr;
    vma->end = aligned_vaddr + aligned_memsz;
    vma->flags = prot_to_vm_flags(phdr->p_flags);
    vma->file = NULL;
    vma->offset = 0;
    
    // Add to MM
    spin_lock(&mm->mmap_lock);
    if (!mm->mmap) {
        mm->mmap = vma;
    } else {
        vm_area_t *last = mm->mmap;
        while (last->next) last = last->next;
        last->next = vma;
        vma->prev = last;
    }
    spin_unlock(&mm->mmap_lock);
    
    // Allocate physical pages and map them
    u64 num_pages = aligned_memsz / PAGE_SIZE;
    for (u64 i = 0; i < num_pages; i++) {
        u64 page_vaddr = aligned_vaddr + (i * PAGE_SIZE);
        
        // Allocate physical page
        phys_addr_t paddr = pmm_alloc_page();
        if (!paddr) {
            printk(KERN_ERR "ELF: Out of memory allocating page\n");
            return -ENOMEM;
        }
        
        // Clear the page
        void *page_ptr = (void *)phys_to_virt(paddr);
        memset(page_ptr, 0, PAGE_SIZE);
        
        // Copy file data if any
        u64 copy_size = 0;
        
        if (i * PAGE_SIZE < filesz + page_offset) {
            if (i == 0) {
                // First page - copy from page_offset
                copy_size = PAGE_SIZE - page_offset;
                if (copy_size > filesz) copy_size = filesz;
                memcpy((u8 *)page_ptr + page_offset, elf_data + offset, copy_size);
            } else {
                // Subsequent pages
                u64 file_pos = (i * PAGE_SIZE) - page_offset;
                if (file_pos < filesz) {
                    copy_size = PAGE_SIZE;
                    if (file_pos + copy_size > filesz) {
                        copy_size = filesz - file_pos;
                    }
                    memcpy(page_ptr, elf_data + offset + file_pos, copy_size);
                }
            }
        }
        
        // Map the page using kernel VMM (TODO: use user page tables)
        u64 pte_flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W) pte_flags |= PTE_WRITABLE;
        if (!(phdr->p_flags & PF_X)) pte_flags |= PTE_NO_EXECUTE;
        
        // TODO: Map in user address space properly
        // For now just record the mapping in VMA
        (void)page_vaddr;
        (void)pte_flags;
    }
    
    return 0;
}

// ============================================================================
// STACK SETUP
// ============================================================================

#define USER_STACK_TOP      0x7FFFFFFFE000ULL
#define USER_STACK_PAGES    32  // 128KB initial stack

static int setup_user_stack(mm_struct_t *mm, const char *const argv[], 
                            const char *const envp[], const Elf64_Ehdr *ehdr,
                            u64 *sp_out) {
    u64 stack_top = USER_STACK_TOP;
    u64 stack_bottom = stack_top - (USER_STACK_PAGES * PAGE_SIZE);
    
    // Create stack VMA
    vm_area_t *vma = kmalloc(sizeof(vm_area_t), GFP_KERNEL);
    if (!vma) return -ENOMEM;
    
    memset(vma, 0, sizeof(vm_area_t));
    vma->start = stack_bottom;
    vma->end = stack_top;
    vma->flags = VM_READ | VM_WRITE | VM_GROWSDOWN;
    
    spin_lock(&mm->mmap_lock);
    if (!mm->mmap) {
        mm->mmap = vma;
    } else {
        vm_area_t *last = mm->mmap;
        while (last->next) last = last->next;
        last->next = vma;
        vma->prev = last;
    }
    mm->start_stack = stack_top;
    spin_unlock(&mm->mmap_lock);
    
    // Allocate and map stack pages
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        u64 page_vaddr = stack_bottom + (i * PAGE_SIZE);
        phys_addr_t paddr = pmm_alloc_page();
        if (!paddr) return -ENOMEM;
        
        memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
        // TODO: Map in user page tables properly
        (void)page_vaddr;
    }
    
    // Build stack contents (grows down)
    u64 sp = stack_top;
    
    // Count arguments and environment
    int argc = 0, envc = 0;
    if (argv) while (argv[argc]) argc++;
    if (envp) while (envp[envc]) envc++;
    
    // Copy strings to stack (at top)
    u64 string_area = sp - 4096;  // Reserve 4KB for strings
    u64 str_ptr = string_area;
    
    // Copy argv strings (just record positions for now)
    for (int i = 0; i < argc && i < 255; i++) {
        size_t len = strlen(argv[i]) + 1;
        // TODO: Actually copy to user space
        str_ptr += len;
    }
    
    // Copy envp strings
    for (int i = 0; i < envc && i < 255; i++) {
        size_t len = strlen(envp[i]) + 1;
        str_ptr += len;
    }
    
    // Align stack pointer
    sp = string_area;
    sp = sp & ~15;  // 16-byte alignment
    
    // Build auxiliary vector
    Elf64_auxv_t auxv[] = {
        { AT_PAGESZ, PAGE_SIZE },
        { AT_PHDR, ehdr->e_phoff },  // TODO: actual phdr address
        { AT_PHENT, ehdr->e_phentsize },
        { AT_PHNUM, ehdr->e_phnum },
        { AT_ENTRY, ehdr->e_entry },
        { AT_UID, 0 },
        { AT_EUID, 0 },
        { AT_GID, 0 },
        { AT_EGID, 0 },
        { AT_SECURE, 0 },
        { AT_NULL, 0 },
    };
    
    int auxv_count = sizeof(auxv) / sizeof(auxv[0]);
    
    // Push auxiliary vector
    sp -= auxv_count * sizeof(Elf64_auxv_t);
    // TODO: copy auxv to user stack
    
    // Push environment pointers
    sp -= (envc + 1) * sizeof(u64);
    // TODO: copy envp_ptrs
    
    // Push argument pointers
    sp -= (argc + 1) * sizeof(u64);
    // TODO: copy argv_ptrs
    
    // Push argc
    sp -= sizeof(u64);
    // TODO: write argc
    
    *sp_out = sp;
    return 0;
}

// ============================================================================
// MAIN LOAD FUNCTION
// ============================================================================

int elf_load(const u8 *elf_data, size_t elf_size, task_struct_t *task) {
    if (!elf_data || elf_size < sizeof(Elf64_Ehdr)) {
        return -EINVAL;
    }
    
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    
    // Validate header
    int ret = elf_check_header(ehdr);
    if (ret < 0) {
        return ret;
    }
    
    printk(KERN_INFO "ELF: Loading %s binary, entry=0x%lx\n",
           ehdr->e_type == ET_EXEC ? "executable" : "shared object",
           ehdr->e_entry);
    
    // Create new address space
    mm_struct_t *mm = mm_alloc();
    if (!mm) {
        return -ENOMEM;
    }
    
    // Load program segments
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)(elf_data + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        ret = load_segment(mm, elf_data, &phdr[i]);
        if (ret < 0) {
            mm_free(mm);
            return ret;
        }
        
        // Track code/data regions
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_flags & PF_X) {
                if (!mm->start_code || phdr[i].p_vaddr < mm->start_code) {
                    mm->start_code = phdr[i].p_vaddr;
                }
                if (phdr[i].p_vaddr + phdr[i].p_memsz > mm->end_code) {
                    mm->end_code = phdr[i].p_vaddr + phdr[i].p_memsz;
                }
            } else {
                if (!mm->start_data || phdr[i].p_vaddr < mm->start_data) {
                    mm->start_data = phdr[i].p_vaddr;
                }
                if (phdr[i].p_vaddr + phdr[i].p_memsz > mm->end_data) {
                    mm->end_data = phdr[i].p_vaddr + phdr[i].p_memsz;
                }
            }
        }
    }
    
    // Setup heap (brk)
    mm->start_brk = ALIGN_UP(mm->end_data, PAGE_SIZE);
    mm->brk = mm->start_brk;
    
    // Setup stack
    u64 user_sp;
    ret = setup_user_stack(mm, NULL, NULL, ehdr, &user_sp);
    if (ret < 0) {
        mm_free(mm);
        return ret;
    }
    
    // Assign to task
    if (task->mm) {
        mm_free(task->mm);
    }
    task->mm = mm;
    task->active_mm = mm;
    
    // Setup entry point
    task->context.rip = ehdr->e_entry;
    task->context.rsp = user_sp;
    task->context.cs = 0x23;  // User code segment
    task->context.ss = 0x1b;  // User data segment
    task->context.rflags = 0x200;  // Interrupts enabled
    
    printk(KERN_INFO "ELF: Binary loaded, entry=0x%lx, sp=0x%lx\n",
           ehdr->e_entry, user_sp);
    
    return 0;
}

// ============================================================================
// EXECVE IMPLEMENTATION
// ============================================================================

int do_execve(const char *filename, char *const argv[], char *const envp[]) {
    // TODO: Read file from filesystem
    // For now, return error
    (void)filename;
    (void)argv;
    (void)envp;
    
    printk(KERN_INFO "execve: %s (not implemented yet)\n", filename);
    return -ENOENT;
}
