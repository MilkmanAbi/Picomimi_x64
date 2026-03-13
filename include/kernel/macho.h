/**
 * Picomimi-x64 — Mach-O Binary Format Definitions
 *
 * Covers:
 *   - Fat (universal) binary header  (magic 0xCAFEBABE)
 *   - 64-bit Mach-O header           (magic 0xFEEDFACF, cigam 0xCFFAEDFE)
 *   - Load commands we care about:
 *       LC_SEGMENT_64, LC_UNIXTHREAD, LC_MAIN, LC_LOAD_DYLINKER,
 *       LC_ID_DYLIB, LC_LOAD_DYLIB, LC_UUID, LC_SOURCE_VERSION,
 *       LC_BUILD_VERSION, LC_CODE_SIGNATURE (recognised, ignored)
 *
 * Reference: XNU osfmk/mach-o/loader.h  (open-source Apple)
 *
 * IMPORTANT: nothing in this file touches Linux/POSIX interfaces.
 * It is pure struct definitions — safe to include anywhere.
 */

#ifndef _KERNEL_MACHO_H
#define _KERNEL_MACHO_H

#include <kernel/types.h>

/* =========================================================
 * Magic numbers
 * ========================================================= */

#define FAT_MAGIC           0xCAFEBABEU     /* fat binary, big-endian magic  */
#define FAT_CIGAM           0xBEBAFECAU     /* fat binary, little-endian     */

#define MH_MAGIC_64         0xFEEDFACFU     /* 64-bit mach-o, native endian  */
#define MH_CIGAM_64         0xCFFAEDFEU     /* 64-bit mach-o, swapped        */

/* We only load x86_64 thin binaries or fat binaries containing one. */

/* =========================================================
 * Fat / Universal binary
 * ========================================================= */

/* Fat header is big-endian on disk — swap when reading */
typedef struct fat_header {
    u32 magic;          /* FAT_MAGIC */
    u32 nfat_arch;      /* number of archs (big-endian) */
} fat_header_t;

typedef struct fat_arch {
    u32 cputype;        /* cpu type    (big-endian) */
    u32 cpusubtype;     /* cpu subtype (big-endian) */
    u32 offset;         /* file offset (big-endian) */
    u32 size;           /* size        (big-endian) */
    u32 align;          /* alignment   (big-endian, 2^n) */
} fat_arch_t;

/* CPU types we care about */
#define CPU_TYPE_X86_64     ((u32)0x01000007)
#define CPU_TYPE_ARM64      ((u32)0x0100000C)
#define CPU_SUBTYPE_ALL     ((u32)0x00000003)

static inline u32 bswap32(u32 v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0xFF000000u) >> 24);
}

/* =========================================================
 * Mach-O 64-bit header
 * ========================================================= */

typedef struct mach_header_64 {
    u32 magic;          /* MH_MAGIC_64 */
    u32 cputype;        /* CPU_TYPE_X86_64 */
    u32 cpusubtype;     /* CPU_SUBTYPE_ALL */
    u32 filetype;       /* MH_EXECUTE etc. */
    u32 ncmds;          /* number of load commands */
    u32 sizeofcmds;     /* total size of load commands */
    u32 flags;          /* MH_* flags */
    u32 reserved;       /* (padding) */
} mach_header_64_t;

/* filetypes */
#define MH_OBJECT       0x1     /* relocatable object */
#define MH_EXECUTE      0x2     /* demand-paged executable */
#define MH_DYLIB        0x6     /* dynamic library */
#define MH_DYLINKER     0x7     /* dynamic linker */
#define MH_BUNDLE       0x8     /* bundle */

/* header flags (we honour PIE) */
#define MH_NOUNDEFS         0x00000001
#define MH_PIE              0x00200000  /* ASLR-eligible; set for most modern bins */
#define MH_TWOLEVEL         0x00000080

/* =========================================================
 * Load command header
 * ========================================================= */

typedef struct load_command {
    u32 cmd;
    u32 cmdsize;
} load_command_t;

/* Load command identifiers */
#define LC_SEGMENT_64       0x19
#define LC_UNIXTHREAD       0x5
#define LC_MAIN             0x80000028U
#define LC_LOAD_DYLINKER    0xE
#define LC_ID_DYLINKER      0xF
#define LC_LOAD_DYLIB       0xC
#define LC_ID_DYLIB         0xD
#define LC_UUID             0x1B
#define LC_SOURCE_VERSION   0x2A
#define LC_BUILD_VERSION    0x32
#define LC_CODE_SIGNATURE   0x1D
#define LC_DYLD_INFO        0x22
#define LC_DYLD_INFO_ONLY   0x80000022U
#define LC_SYMTAB           0x2
#define LC_DYSYMTAB         0xB
#define LC_FUNCTION_STARTS  0x26
#define LC_DATA_IN_CODE     0x29

/* =========================================================
 * LC_SEGMENT_64
 * ========================================================= */

#define MACHO_SEG_NAMELEN   16

typedef struct segment_command_64 {
    u32  cmd;           /* LC_SEGMENT_64 */
    u32  cmdsize;
    char segname[MACHO_SEG_NAMELEN];
    u64  vmaddr;
    u64  vmsize;
    u64  fileoff;
    u64  filesize;
    u32  maxprot;       /* maximum VM protection */
    u32  initprot;      /* initial VM protection */
    u32  nsects;
    u32  flags;
} segment_command_64_t;

/* Protection bits (match PROT_READ/WRITE/EXEC) */
#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04
#define VM_PROT_ALL     0x07

/* Segment flags */
#define SG_HIGHVM       0x1     /* mapping occupies high part of vmspace */
#define SG_NORELOC      0x4

typedef struct section_64 {
    char  sectname[MACHO_SEG_NAMELEN];
    char  segname[MACHO_SEG_NAMELEN];
    u64   addr;
    u64   size;
    u32   offset;
    u32   align;
    u32   reloff;
    u32   nreloc;
    u32   flags;
    u32   reserved1;
    u32   reserved2;
    u32   reserved3;
} section_64_t;

/* =========================================================
 * LC_UNIXTHREAD  (old-style entry point: x86_64 register state)
 * ========================================================= */

/* thread flavours */
#define x86_THREAD_STATE64      4

typedef struct x86_thread_state64 {
    u64 rax, rbx, rcx, rdx;
    u64 rdi, rsi, rbp, rsp;
    u64 r8,  r9,  r10, r11;
    u64 r12, r13, r14, r15;
    u64 rip, rflags;
    u64 cs,  fs,  gs;
} x86_thread_state64_t;

typedef struct thread_command {
    u32 cmd;            /* LC_UNIXTHREAD */
    u32 cmdsize;
    u32 flavor;         /* x86_THREAD_STATE64 */
    u32 count;          /* number of u32 words in state */
    x86_thread_state64_t state;
} thread_command_t;

/* =========================================================
 * LC_MAIN  (modern entry point)
 * ========================================================= */

typedef struct entry_point_command {
    u32 cmd;            /* LC_MAIN */
    u32 cmdsize;
    u64 entryoff;       /* file offset of entry point */
    u64 stacksize;      /* requested stack size (0 = default) */
} entry_point_command_t;

/* =========================================================
 * LC_LOAD_DYLINKER / LC_ID_DYLINKER
 * ========================================================= */

typedef struct dylinker_command {
    u32 cmd;
    u32 cmdsize;
    u32 name_offset;    /* offset from start of command to name string */
    /* followed by NUL-terminated path */
} dylinker_command_t;

/* =========================================================
 * LC_LOAD_DYLIB / LC_ID_DYLIB
 * ========================================================= */

typedef struct dylib {
    u32 name_offset;
    u32 timestamp;
    u32 current_version;
    u32 compatibility_version;
} dylib_t;

typedef struct dylib_command {
    u32     cmd;
    u32     cmdsize;
    dylib_t dylib;
    /* followed by NUL-terminated path */
} dylib_command_t;

/* =========================================================
 * LC_UUID
 * ========================================================= */

typedef struct uuid_command {
    u32 cmd;
    u32 cmdsize;
    u8  uuid[16];
} uuid_command_t;

/* =========================================================
 * Result of a successful Mach-O parse
 * ========================================================= */

typedef struct macho_load_info {
    u64     entry;          /* resolved entry point (virtual address) */
    u64     load_bias;      /* slide applied (0 for non-PIE) */
    u64     mh_vaddr;       /* virtual address of mach_header_64 after load */
    u64     stack_size;     /* requested stack size from LC_MAIN (0=default) */
    bool    has_dylinker;   /* true if LC_LOAD_DYLINKER present */
    char    dylinker[256];  /* path to dyld (if any) */
    bool    is_pie;         /* MH_PIE flag set */
    /* brk seed: end of last segment */
    u64     seg_end;
} macho_load_info_t;

#endif /* _KERNEL_MACHO_H */
