#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <kernel/types.h>

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

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25
#define AT_EXECFN       31

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

typedef struct {
    u64             a_type;
    u64             a_val;
} Elf64_auxv_t;


/* Additional AT_ values */
#define AT_SECURE       23
#define AT_BASE_PLATFORM 24
#define AT_HWCAP        16
#define AT_HWCAP2       26
#define AT_CLKTCK       17
#define AT_PLATFORM     15
#define AT_SYSINFO_EHDR 33

#define EV_CURRENT      1

/* User stack layout */
#ifndef USER_STACK_TOP
#define USER_STACK_TOP      0x00007FFFFFFFE000ULL
#endif
#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE     (8 * 1024 * 1024)
#endif
#ifndef ELF_PIE_LOAD_BASE
#define ELF_PIE_LOAD_BASE   0x0000000000400000ULL
#endif

#endif /* _KERNEL_ELF_H */
