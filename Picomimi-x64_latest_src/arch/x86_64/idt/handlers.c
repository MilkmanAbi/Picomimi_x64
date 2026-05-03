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
    
    // Enable IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade to slave)
    // 0xF8 = 11111000 — bits 0,1,2 clear = enabled
    outb(PIC1_DATA, 0xF8);  // Enable IRQ0+IRQ1+IRQ2(cascade)
    outb(PIC2_DATA, 0xEF);  // Enable IRQ12 (PS/2 mouse) on slave; mask rest
    
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
            /* ============================================================
             * #PF handler — demand paging + Copy-on-Write
             *
             * Error code bits:
             *   bit 0 (P):  0=not-present  1=protection violation
             *   bit 1 (W):  0=read         1=write
             *   bit 2 (U):  0=supervisor   1=user
             *   bit 4 (I):  instruction fetch
             * ============================================================ */
            u64 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            int fault_present = (regs->error_code & 1);
            int fault_write   = (regs->error_code & 2);
            int fault_user    = (regs->error_code & 4);

            /* Ask the kernel memory subsystem to handle it */
            extern int do_page_fault(u64 fault_addr, int present,
                                     int write, int user);
            int handled = do_page_fault(cr2, fault_present, fault_write, fault_user);

            if (handled == 0)
                return;  /* Fault resolved — resume faulting instruction */

            /* Unhandled fault */
            if (fault_user) {
                /* User-space SIGSEGV */
                printk(KERN_ERR
                    "[#PF] user SIGSEGV: CR2=0x%llx RIP=0x%lx err=0x%lx\n",
                    cr2, regs->rip, regs->error_code);
                if (regs->rip == 0 && regs->rsp >= 0x400000ULL &&
                    regs->rsp < 0x800000000000ULL) {
                    u64 *usp = (u64 *)regs->rsp;
                    printk(KERN_ERR "  RSP[0..2]: 0x%llx 0x%llx 0x%llx\n",
                           usp[0], usp[1], usp[2]);
                }
                extern void do_exit(int code);
                extern void schedule(void);
                do_exit(11);  /* SIGSEGV */
                schedule();
                return;
            } else {
                /* Kernel oops */
                printk(KERN_EMERG
                    "[#PF] KERNEL OOPS: CR2=0x%llx RIP=0x%lx err=0x%lx\n",
                    cr2, regs->rip, regs->error_code);
                printk(KERN_EMERG "Page fault flags: %s%s%s%s\n",
                       fault_present ? "P " : "NP ",
                       fault_write   ? "W " : "R ",
                       fault_user    ? "U " : "S ",
                       (regs->error_code & 16) ? "I" : "D");
                printk(KERN_EMERG "System halted.\n");
                __asm__ volatile("cli");
                for (;;) { __asm__ volatile("hlt"); }
            }
        }


        // Halt on unrecoverable kernel exceptions
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
    (void)regs;
    system_ticks++;
    extern volatile u64 jiffies;
    jiffies++;
    /* Drive the O(1) scheduler on each tick */
    extern void scheduler_tick(void);
    scheduler_tick();

    /* Poll e1000 RX every 10 ticks (~10ms) to drain received packets */
    if ((system_ticks % 10) == 0) {
        extern void e1000_rx_poll(void);
        e1000_rx_poll();
    }

    /* Preempt if current task timeslice expired or a higher-prio task is ready */
    extern void schedule(void);
    schedule();
}

/**
 * Keyboard IRQ handler
 */
void keyboard_handler(regs_t *regs) {
    (void)regs;
    /* Full PS/2 driver: scancode translation, modifier tracking,
       event queuing, WM callback dispatch */
    extern void kbd_handle_irq(void);
    kbd_handle_irq();
}

/**
 * Syscall dispatcher (placeholder)
 */
/* interrupts_init - called during kernel boot to set up PIC and PIT */
void interrupts_init(void) {
    pic_init();
    pit_init(1000);   /* 1000 Hz tick */
}
