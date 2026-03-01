/**
 * Picomimi-x64 Kernel Header
 */
#ifndef _KERNEL_KERNEL_H
#define _KERNEL_KERNEL_H

#include <kernel/types.h>

// ============================================================================
// KERNEL STATE STRUCTURE (inspired by Picomimi)
// ============================================================================

typedef struct kernel_state {
    // Boot info
    u64 boot_time_ns;
    u64 total_memory;
    u64 free_memory;
    void *acpi_rsdp;
    
    // CPU info
    u32 num_cpus;
    u32 bsp_cpu_id;
    cpumask_t online_cpus;
    cpumask_t present_cpus;
    
    // Memory stats
    u64 total_pages;
    u64 free_pages;
    u64 used_pages;
    u64 kernel_pages;
    
    // Task stats
    u32 total_tasks;
    u32 running_tasks;
    u32 sleeping_tasks;
    u32 zombie_tasks;
    
    // Scheduling stats
    u64 context_switches;
    u64 preemptions;
    
    // State flags
    bool initialized;
    bool running;
    bool panic_mode;
    bool smp_enabled;
} kernel_state_t;

extern kernel_state_t kernel_state;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

void __noreturn kernel_main(u32 magic, u64 multiboot_info);
void __noreturn kernel_panic(const char *fmt, ...);

void cli(void);
void sti(void);
u64 read_flags(void);
void write_flags(u64 flags);
irqflags_t local_irq_save(void);
void local_irq_restore(irqflags_t flags);

// ============================================================================
// STUB DECLARATIONS (to be implemented)
// ============================================================================

void cpu_init(void);
void gdt_init(void);
void idt_init(void);
void pmm_init(void);
void pmm_add_region(u64 addr, u64 size);
void pmm_reserve_region(u64 addr, u64 size);
u64 pmm_get_free_memory(void);
void vmm_init(void);
void slab_init(void);
void lapic_init(void);
void ioapic_init(void);
void sched_init(void);
void smp_init(void);
void syscall_init(void);

#endif // _KERNEL_KERNEL_H
