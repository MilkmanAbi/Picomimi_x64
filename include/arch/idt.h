/**
 * Picomimi-x64 IDT Header
 */
#ifndef _ARCH_IDT_H
#define _ARCH_IDT_H

#include <kernel/types.h>

// Exception numbers
#define EXC_DIVIDE_ERROR        0
#define EXC_DEBUG               1
#define EXC_NMI                 2
#define EXC_BREAKPOINT          3
#define EXC_OVERFLOW            4
#define EXC_BOUND_RANGE         5
#define EXC_INVALID_OPCODE      6
#define EXC_DEVICE_NOT_AVAIL    7
#define EXC_DOUBLE_FAULT        8
#define EXC_COPROCESSOR_SEG     9
#define EXC_INVALID_TSS         10
#define EXC_SEGMENT_NOT_PRESENT 11
#define EXC_STACK_FAULT         12
#define EXC_GENERAL_PROTECTION  13
#define EXC_PAGE_FAULT          14
#define EXC_RESERVED_15         15
#define EXC_X87_FPU             16
#define EXC_ALIGNMENT_CHECK     17
#define EXC_MACHINE_CHECK       18
#define EXC_SIMD_FP             19
#define EXC_VIRTUALIZATION      20

// IRQ numbers (remapped to 32+)
#define IRQ_BASE        32
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1
#define IRQ_CASCADE     2
#define IRQ_COM2        3
#define IRQ_COM1        4
#define IRQ_LPT2        5
#define IRQ_FLOPPY      6
#define IRQ_LPT1        7
#define IRQ_RTC         8
#define IRQ_MOUSE       12
#define IRQ_FPU         13
#define IRQ_ATA_PRIMARY 14
#define IRQ_ATA_SECONDARY 15

// System call interrupt
#define INT_SYSCALL     0x80

// IDT gate types
#define IDT_INTERRUPT_GATE  0x8E    // Present, Ring 0, Interrupt Gate
#define IDT_TRAP_GATE       0x8F    // Present, Ring 0, Trap Gate
#define IDT_USER_INT_GATE   0xEE    // Present, Ring 3, Interrupt Gate

// Interrupt frame (pushed by CPU and ISR stub)
typedef struct __packed {
    // Pushed by ISR stub
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 int_no, error_code;
    
    // Pushed by CPU
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} interrupt_frame_t;

void idt_init(void);
void idt_set_gate(u8 num, u64 handler, u16 selector, u8 type);
void register_interrupt_handler(u8 num, void (*handler)(interrupt_frame_t *));

#endif // _ARCH_IDT_H
