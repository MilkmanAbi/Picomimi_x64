/*
 * AP trampoline: 16-bit → 32-bit PM → 64-bit LM → ap_entry64()
 * Assembled at physical 0x8000 (SIPI vector 0x08).
 *
 * Temp GDT layout (written by smp.c at 0x8200):
 *   0x00: null
 *   0x08: code32  (D=1, for PM32 phase)
 *   0x10: data32
 *   0x18: code64  (L=1, for LM64 phase)
 *
 * GDT pointer at 0x8220, params block at 0x8400.
 */

.section .trampoline.text, "ax"
.global ap_trampoline_start
.code16
ap_trampoline_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x6FFE, %sp

    /* lgdtl [addr] — addr patched to physical 0x8220 (GDTP) */
    .byte 0x66, 0x0F, 0x01, 0x16
.global ap_patch_gdt_addr
ap_patch_gdt_addr:
    .word 0x0000

    movl %cr0, %eax
    orl  $0x1, %eax
    movl %eax, %cr0

    /* Far jmp to PM32 — target patched at runtime */
    .byte 0xEA
.global ap_patch_pm32_entry
ap_patch_pm32_entry:
    .word 0x0000    /* IP: physical addr of ap_pm32 */
    .word 0x0008    /* CS: code32 selector */

.code32
.global ap_pm32
ap_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* Enable PAE */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    /* Load PML4 — immediate patched to params->pml4_phys */
    movl $0x00000000, %eax
    movl %eax, %cr3

    /* Enable LME in EFER */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x00000101, %eax   /* LME + SCE */
    wrmsr

    /* Enable PG + PE */
    movl %cr0, %eax
    orl  $0x80000001, %eax
    movl %eax, %cr0

    /* Far jmp to LM64 using code64 selector 0x18 — target patched at runtime */
    .byte 0xEA
.global ap_patch_lm64_entry
ap_patch_lm64_entry:
    .long 0x00000000   /* EIP: physical addr of ap_lm64 */
    .word 0x0018       /* CS: code64 selector */

.code64
.global ap_lm64
ap_lm64:
    movq $0x6FF0, %rsp
    /* Load params physical address — immediate patched by write_trampoline() */
    movabsq $0xDEADBEEFDEADBEEF, %rax
    movq 0x08(%rax), %rax   /* params->ap_entry_virt */
    jmp *%rax

.global ap_trampoline_end
ap_trampoline_end:
    cli
    hlt
    jmp ap_trampoline_end
