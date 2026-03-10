/**
 * Picomimi-x64 Interrupt Entry Points
 * 
 * Assembly stubs for all CPU exceptions and IRQs
 */

.section .text
.code64

// Macro to save all registers
.macro SAVE_REGS
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax
.endm

// Macro to restore all registers
.macro RESTORE_REGS
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
.endm

// Exception without error code
.macro EXCEPTION_NOERR num
.global isr\num
isr\num:
    push $0             // Dummy error code
    push $\num          // Interrupt number
    jmp isr_common
.endm

// Exception with error code
.macro EXCEPTION_ERR num
.global isr\num
isr\num:
    push $\num          // Interrupt number (error code already pushed by CPU)
    jmp isr_common
.endm

// IRQ handler
.macro IRQ num irq_num
.global irq\irq_num
irq\irq_num:
    push $0             // Dummy error code
    push $\num          // Interrupt number (32 + irq_num)
    jmp irq_common
.endm

// ============================================================================
// EXCEPTION HANDLERS (0-31)
// ============================================================================

EXCEPTION_NOERR 0   // Divide by Zero
EXCEPTION_NOERR 1   // Debug
EXCEPTION_NOERR 2   // NMI
EXCEPTION_NOERR 3   // Breakpoint
EXCEPTION_NOERR 4   // Overflow
EXCEPTION_NOERR 5   // Bound Range Exceeded
EXCEPTION_NOERR 6   // Invalid Opcode
EXCEPTION_NOERR 7   // Device Not Available
EXCEPTION_ERR   8   // Double Fault
EXCEPTION_NOERR 9   // Coprocessor Segment Overrun (reserved)
EXCEPTION_ERR   10  // Invalid TSS
EXCEPTION_ERR   11  // Segment Not Present
EXCEPTION_ERR   12  // Stack-Segment Fault
EXCEPTION_ERR   13  // General Protection Fault
EXCEPTION_ERR   14  // Page Fault
EXCEPTION_NOERR 15  // Reserved
EXCEPTION_NOERR 16  // x87 FPU Error
EXCEPTION_ERR   17  // Alignment Check
EXCEPTION_NOERR 18  // Machine Check
EXCEPTION_NOERR 19  // SIMD Floating-Point
EXCEPTION_NOERR 20  // Virtualization Exception
EXCEPTION_ERR   21  // Control Protection Exception
EXCEPTION_NOERR 22  // Reserved
EXCEPTION_NOERR 23  // Reserved
EXCEPTION_NOERR 24  // Reserved
EXCEPTION_NOERR 25  // Reserved
EXCEPTION_NOERR 26  // Reserved
EXCEPTION_NOERR 27  // Reserved
EXCEPTION_NOERR 28  // Hypervisor Injection
EXCEPTION_ERR   29  // VMM Communication Exception
EXCEPTION_ERR   30  // Security Exception
EXCEPTION_NOERR 31  // Reserved

// ============================================================================
// IRQ HANDLERS (32-47)
// ============================================================================

IRQ 32 0    // Timer (PIT or LAPIC)
IRQ 33 1    // Keyboard
IRQ 34 2    // Cascade
IRQ 35 3    // COM2
IRQ 36 4    // COM1
IRQ 37 5    // LPT2
IRQ 38 6    // Floppy
IRQ 39 7    // LPT1 / Spurious
IRQ 40 8    // RTC
IRQ 41 9    // Free
IRQ 42 10   // Free
IRQ 43 11   // Free
IRQ 44 12   // PS/2 Mouse
IRQ 45 13   // FPU
IRQ 46 14   // Primary ATA
IRQ 47 15   // Secondary ATA

// ============================================================================
// SYSCALL HANDLER
// ============================================================================

.global syscall_entry
syscall_entry:
    // SYSCALL instruction:
    // - RCX = return RIP
    // - R11 = return RFLAGS
    // - RAX = syscall number
    // - RDI, RSI, RDX, R10, R8, R9 = arguments

    // Swap to kernel stack (from TSS RSP0)
    swapgs
    mov %rsp, %gs:8           // Save user RSP to per-CPU
    mov %gs:0, %rsp           // Load kernel RSP from per-CPU

    // Build trap frame
    push $0x33                // User SS (GDT_USER_DATA | 3 = 0x33)
    push %gs:8                // User RSP (saved above)
    push %r11                 // RFLAGS
    push $0x2B                // User CS
    push %rcx                 // Return RIP

    push $0                   // Error code
    push $0x80                // Syscall vector

    SAVE_REGS

    // Set up kernel segments (GDT_DATA64 = 0x20)
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es

    // Call C handler: syscall_dispatch(regs)
    mov %rsp, %rdi
    call syscall_dispatch

    RESTORE_REGS
    add $16, %rsp             // Skip error code and vector

    // Restore user state
    swapgs
    iretq

// ============================================================================
// COMMON HANDLERS
// ============================================================================

isr_common:
    SAVE_REGS

    // Set kernel data segments (GDT_DATA64 = 0x20)
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es

    // Conditional swapgs: only when coming from ring 3
    // The saved CS is at rsp + 15*8 (after SAVE_REGS) + 16 (vector+errcode) + 8 (rip) = rsp+144
    // Bits[1:0] of CS = CPL. If CPL==3, we came from user mode → need swapgs.
    testb $3, (15*8 + 16 + 8)(%rsp)    // CS byte, bit1:0 = CPL
    jz 1f
    swapgs
1:
    // Call C handler: exception_handler(regs)
    mov %rsp, %rdi
    call exception_handler

    // Conditional swapgs on return
    testb $3, (15*8 + 16 + 8)(%rsp)
    jz 2f
    swapgs
2:
    RESTORE_REGS
    add $16, %rsp             // Skip error code and vector
    iretq

irq_common:
    SAVE_REGS

    // Set kernel data segments (GDT_DATA64 = 0x20)
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es

    // Conditional swapgs: only when coming from ring 3
    testb $3, (15*8 + 16 + 8)(%rsp)
    jz 1f
    swapgs
1:
    // Call C handler: irq_handler(regs)
    mov %rsp, %rdi
    call irq_handler

    // Conditional swapgs on return
    testb $3, (15*8 + 16 + 8)(%rsp)
    jz 2f
    swapgs
2:
    RESTORE_REGS
    add $16, %rsp             // Skip error code and vector
    iretq

// ============================================================================
// ISR TABLE (for IDT setup)
// ============================================================================

.section .data
.align 8

.global isr_table
isr_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

.global irq_table
irq_table:
    .quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    .quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

.section .note.GNU-stack, "", @progbits
