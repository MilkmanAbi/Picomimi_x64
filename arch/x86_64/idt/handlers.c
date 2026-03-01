/**
 * Picomimi-x64 Interrupt Handlers
 * 
 * Exception and IRQ handling
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <arch/apic.h>
#include <arch/io.h>

// Register state pushed by interrupt stubs
typedef struct {
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} __packed regs_t;

// Exception names
static const char *exception_names[] = {
    "Divide Error",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point",
    "Virtualization",
    "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",
    "VMM Communication",
    "Security Exception",
    "Reserved"
};

// IRQ handlers array
static void (*irq_handlers[16])(regs_t *) = {0};

// PIC ports
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

// Forward declarations
void timer_handler(regs_t *regs);
void keyboard_handler(regs_t *regs);

// Tick counter
volatile u64 system_ticks = 0;

/**
 * Initialize the 8259 PIC (remap IRQs to 32-47)
 */
void pic_init(void) {
    // ICW1: Initialize + ICW4 needed
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    
    // ICW2: Vector offsets
    outb(PIC1_DATA, 0x20);  // IRQ 0-7  -> INT 32-39
    outb(PIC2_DATA, 0x28);  // IRQ 8-15 -> INT 40-47
    
    // ICW3: Cascade
    outb(PIC1_DATA, 0x04);  // IRQ2 has slave
    outb(PIC2_DATA, 0x02);  // Slave ID 2
    
    // ICW4: 8086 mode
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    // Mask all IRQs except timer and keyboard
    outb(PIC1_DATA, 0xFC);  // Enable IRQ0 (timer) and IRQ1 (keyboard)
    outb(PIC2_DATA, 0xFF);  // Mask all on slave
    
    printk(KERN_INFO "[IRQ] PIC initialized, remapped to 32-47\n");
}

/**
 * Send EOI to PIC
 */
static void pic_eoi(u32 irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

/**
 * Initialize PIT timer (Channel 0, Mode 3, ~100Hz)
 */
void pit_init(u32 frequency) {
    u32 divisor = 1193182 / frequency;
    
    outb(0x43, 0x36);  // Channel 0, lobyte/hibyte, mode 3
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    printk(KERN_INFO "[TIMER] PIT initialized at %u Hz\n", frequency);
}

/**
 * Register an IRQ handler
 */
void irq_register(u8 irq, void (*handler)(regs_t *)) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

/**
 * Exception handler (called from assembly)
 */
void exception_handler(regs_t *regs) {
    u64 vector = regs->vector;
    
    if (vector < 32) {
        printk(KERN_EMERG "\n*** EXCEPTION: %s (#%lu) ***\n", 
               exception_names[vector], vector);
        printk(KERN_EMERG "Error code: 0x%lx\n", regs->error_code);
        printk(KERN_EMERG "RIP: 0x%lx  RSP: 0x%lx\n", regs->rip, regs->rsp);
        printk(KERN_EMERG "RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx\n", 
               regs->rax, regs->rbx, regs->rcx);
        printk(KERN_EMERG "RDX: 0x%lx  RSI: 0x%lx  RDI: 0x%lx\n",
               regs->rdx, regs->rsi, regs->rdi);
        printk(KERN_EMERG "RBP: 0x%lx  R8:  0x%lx  R9:  0x%lx\n",
               regs->rbp, regs->r8, regs->r9);
        printk(KERN_EMERG "CS: 0x%lx  SS: 0x%lx  RFLAGS: 0x%lx\n",
               regs->cs, regs->ss, regs->rflags);
        
        if (vector == 14) {
            // Page fault - show CR2
            u64 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            printk(KERN_EMERG "CR2 (fault address): 0x%lx\n", cr2);
            printk(KERN_EMERG "Page fault flags: %s%s%s%s\n",
                   (regs->error_code & 1) ? "P " : "NP ",
                   (regs->error_code & 2) ? "W " : "R ",
                   (regs->error_code & 4) ? "U " : "S ",
                   (regs->error_code & 16) ? "I" : "D");
        }
        
        // Halt on unrecoverable exceptions
        if (vector == 8 || vector == 13 || vector == 14) {
            printk(KERN_EMERG "\nSystem halted.\n");
            cli();
            for (;;) { __asm__ volatile("hlt"); }
        }
    }
}

/**
 * IRQ handler (called from assembly)
 */
void irq_handler(regs_t *regs) {
    u32 irq = regs->vector - 32;
    
    if (irq < 16) {
        // Call registered handler
        if (irq_handlers[irq]) {
            irq_handlers[irq](regs);
        }
        
        // Send EOI
        pic_eoi(irq);
    }
}

/**
 * Timer IRQ handler
 */
void timer_handler(regs_t *regs) {
    (void)regs;  // Unused for now
    system_ticks++;
    
    // Print tick every 100 ticks (1 second at 100Hz)
    if (system_ticks % 100 == 0) {
        // Just increment, scheduler will use this
    }
}

/**
 * Keyboard IRQ handler
 */
void keyboard_handler(regs_t *regs) {
    (void)regs;
    u8 scancode = inb(0x60);
    
    // Forward to kernel shell
    extern void ksh_handle_key(u8 scancode);
    ksh_handle_key(scancode);
}

/**
 * Syscall dispatcher (placeholder)
 */
void syscall_dispatch(regs_t *regs) {
    u64 syscall_num = regs->rax;
    
    printk(KERN_DEBUG "Syscall %lu from RIP 0x%lx\n", syscall_num, regs->rip);
    
    // TODO: Implement actual syscalls
    regs->rax = -1;  // ENOSYS
}

/**
 * Initialize interrupts
 */
void interrupts_init(void) {
    // Initialize PIC
    pic_init();
    
    // Initialize PIT at 100Hz
    pit_init(100);
    
    // Register handlers
    irq_register(0, timer_handler);
    irq_register(1, keyboard_handler);
    
    printk(KERN_INFO "[IRQ] Interrupt handlers registered\n");
}
