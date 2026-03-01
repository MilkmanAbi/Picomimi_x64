/**
 * Picomimi-x64 Boot Assembly
 * Multiboot2-compliant bootloader for GRUB
 * 
 * Memory Layout:
 * - Physical 0x100000: Kernel loaded here by GRUB
 * - Virtual 0xFFFFFFFF80000000: Kernel virtual base (maps to 0x100000)
 * 
 * Page Table Design:
 * - Identity map first 2MB for boot code
 * - Map 0xFFFFFFFF80000000 -> 0x100000 for kernel
 */

.set MULTIBOOT2_MAGIC,          0xE85250D6
.set MULTIBOOT2_ARCH_I386,      0
.set MULTIBOOT2_HEADER_TAG_END, 0

.set CR0_PE,    (1 << 0)
.set CR0_PG,    (1 << 31)
.set CR4_PAE,   (1 << 5)
.set MSR_EFER,  0xC0000080
.set EFER_LME,  (1 << 8)
.set EFER_NXE,  (1 << 11)
.set EFER_SCE,  (1 << 0)
.set PAGE_PRESENT,  (1 << 0)
.set PAGE_WRITE,    (1 << 1)
.set PAGE_HUGE,     (1 << 7)
.set GDT_CODE64,    0x08
.set KERNEL_STACK_SIZE, 0x10000
.set KERNEL_PHYS_BASE,  0x100000

// ============================================================================
// MULTIBOOT2 HEADER
// ============================================================================

.section .multiboot, "aw"
.align 8

multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH_I386
    .long multiboot_header_end - multiboot_header
    .long -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + (multiboot_header_end - multiboot_header))
    .align 8
    .short MULTIBOOT2_HEADER_TAG_END
    .short 0
    .long 8
multiboot_header_end:

// ============================================================================
// 32-BIT BOOT CODE
// ============================================================================

.section .text.boot, "ax"
.code32

.global _start
_start:
    cli
    cld

    // Save multiboot info
    movl %eax, mb_magic
    movl %ebx, mb_info

    cmp $0x36D76289, %eax
    jne halt32

    mov $boot_stack_top, %esp

    // Check long mode support
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb halt32
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz halt32

    // Setup page tables
    // Clear all page tables first
    mov $pml4, %edi
    xor %eax, %eax
    mov $(6 * 4096 / 4), %ecx  // 6 page tables: pml4, pdpt_low, pdpt_high, pd_low, pd_high
    rep stosl

    // PML4[0] -> pdpt_low (for identity mapping)
    mov $pdpt_low, %eax
    or $(PAGE_PRESENT | PAGE_WRITE), %eax
    mov %eax, pml4

    // PML4[511] -> pdpt_high (for 0xFFFFFFFF80000000)
    mov $pdpt_high, %eax
    or $(PAGE_PRESENT | PAGE_WRITE), %eax
    mov %eax, (pml4 + 511 * 8)

    // PDPT_LOW[0] -> pd_low (identity map first 1GB)
    mov $pd_low, %eax
    or $(PAGE_PRESENT | PAGE_WRITE), %eax
    mov %eax, pdpt_low

    // PDPT_HIGH[510] -> pd_high (map 0xFFFFFFFF80000000)
    mov $pd_high, %eax
    or $(PAGE_PRESENT | PAGE_WRITE), %eax
    mov %eax, (pdpt_high + 510 * 8)

    // Fill pd_low with 2MB huge pages (identity map 0-1GB)
    mov $pd_low, %edi
    mov $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE), %eax  // Start at physical 0
    mov $512, %ecx
fill_pd_low:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop fill_pd_low

    // Fill pd_high with 2MB huge pages starting at physical 0
    // This maps 0xFFFFFFFF80000000 -> 0x0, 0xFFFFFFFF80200000 -> 0x200000, etc.
    // Kernel at physical 0x100000 will be at virtual 0xFFFFFFFF80100000
    mov $pd_high, %edi
    mov $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE), %eax  // Start at physical 0
    mov $512, %ecx
fill_pd_high:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop fill_pd_high

    // Enable paging
    mov $pml4, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $CR4_PAE, %eax
    mov %eax, %cr4

    mov $MSR_EFER, %ecx
    rdmsr
    or $(EFER_LME | EFER_NXE | EFER_SCE), %eax
    wrmsr

    mov %cr0, %eax
    or $(CR0_PG | CR0_PE), %eax
    mov %eax, %cr0

    // Load GDT and jump to 64-bit code
    lgdt gdt_ptr
    ljmp $GDT_CODE64, $entry64

halt32:
    cli
    hlt
    jmp halt32

// ============================================================================
// 64-BIT TRAMPOLINE (still using identity mapping)
// ============================================================================

.code64
entry64:
    // Clear segment registers
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    // Jump to higher-half kernel entry
    movabs $kernel_entry_high, %rax
    jmp *%rax

// ============================================================================
// BOOT DATA (physical addresses)
// ============================================================================

.section .data.boot, "aw"
.align 8

mb_magic:
    .long 0
mb_info:
    .long 0

.align 16
gdt:
    .quad 0                         // Null
    .quad 0x00AF9A000000FFFF        // Code64
    .quad 0x00CF92000000FFFF        // Data64
gdt_end:

gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt

// ============================================================================
// BOOT BSS (physical, page aligned)
// ============================================================================

.section .bss.boot, "aw", @nobits
.align 4096

pml4:
    .skip 4096
pdpt_low:
    .skip 4096
pdpt_high:
    .skip 4096
pd_low:
    .skip 4096
pd_high:
    .skip 4096

.align 16
boot_stack:
    .skip 4096
boot_stack_top:

// ============================================================================
// 64-BIT KERNEL ENTRY (virtual address space - 0xFFFFFFFF80000000)
// ============================================================================

.section .text
.code64

.global kernel_entry_high
kernel_entry_high:
    // Now we're in higher half!
    movabs $kernel_stack_top, %rsp
    xor %rbp, %rbp

    // Clear BSS
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    test %rcx, %rcx
    jz .bss_done
    xor %rax, %rax
    rep stosq
.bss_done:

    // Load multiboot args from boot data (still identity-mapped)
    movabs $mb_magic, %rax
    movl (%rax), %edi
    movabs $mb_info, %rax
    movl (%rax), %esi

    // Store in virtual-space variables
    movabs $multiboot_magic, %rax
    movl %edi, (%rax)
    movabs $multiboot_info_ptr, %rax
    movl %esi, (%rax)

    // Call kernel
    call kernel_main

    // Should never return
    cli
.hang:
    hlt
    jmp .hang

// ============================================================================
// VIRTUAL DATA
// ============================================================================

.section .data
.align 8

.global multiboot_magic
multiboot_magic:
    .long 0

.global multiboot_info_ptr
multiboot_info_ptr:
    .long 0

// ============================================================================
// VIRTUAL BSS
// ============================================================================

.section .bss
.align 16

.global kernel_stack
kernel_stack:
    .skip KERNEL_STACK_SIZE
.global kernel_stack_top
kernel_stack_top:

// Suppress executable stack warning
.section .note.GNU-stack, "", @progbits
