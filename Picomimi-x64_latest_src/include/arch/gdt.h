/**
 * Picomimi-x64 GDT Header
 */
#ifndef _ARCH_GDT_H
#define _ARCH_GDT_H

#include <kernel/types.h>

// GDT segment selectors
#define GDT_NULL        0x00
#define GDT_CODE32      0x08
#define GDT_DATA32      0x10
#define GDT_CODE64      0x18
#define GDT_DATA64      0x20
#define GDT_USER_CODE   0x28
#define GDT_USER_DATA   0x30
#define GDT_TSS         0x38

// Ring levels
#define KERNEL_CS       (GDT_CODE64 | 0)
#define KERNEL_DS       (GDT_DATA64 | 0)
#define USER_CS         (GDT_USER_CODE | 3)
#define USER_DS         (GDT_USER_DATA | 3)

// TSS structure
typedef struct __packed {
    u32 reserved0;
    u64 rsp0;           // Stack pointer for ring 0
    u64 rsp1;           // Stack pointer for ring 1
    u64 rsp2;           // Stack pointer for ring 2
    u64 reserved1;
    u64 ist1;           // Interrupt stack table 1
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;    // I/O permission bitmap offset
} tss_t;

void gdt_init(void);
void gdt_load(void);
void tss_set_rsp0(u64 rsp0);

#endif // _ARCH_GDT_H
