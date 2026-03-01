/**
 * Picomimi-x64 Initialization Stubs
 * 
 * These are placeholder implementations that will be expanded
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/apic.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <sched/sched.h>
#include <lib/printk.h>
#include <lib/string.h>

// ============================================================================
// CPU INITIALIZATION
// ============================================================================

void cpu_init(void) {
    // Enable features in CR4
    u64 cr4 = read_cr4();
    cr4 |= CR4_PAE;     // Physical Address Extension (required for long mode)
    cr4 |= CR4_PGE;     // Page Global Enable
    cr4 |= CR4_OSFXSR;  // Enable SSE
    cr4 |= CR4_OSXMMEXCPT; // Enable SSE exceptions
    write_cr4(cr4);

    // Enable NX and syscall in EFER
    u64 efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;   // No-Execute Enable
    efer |= EFER_SCE;   // Syscall Enable
    wrmsr(MSR_EFER, efer);

    printk(KERN_INFO "  CPU features enabled\n");
}

// ============================================================================
// GDT INITIALIZATION
// ============================================================================

static tss_t tss __aligned(16);

void gdt_init(void) {
    // TSS is already set up in boot.S GDT
    // Just initialize the TSS structure
    __builtin_memset(&tss, 0, sizeof(tss));
    
    // Set kernel stack for syscall/interrupt from ring 3
    extern char kernel_stack_top[];
    tss.rsp0 = (u64)kernel_stack_top;
    
    // Set I/O permission bitmap offset (beyond TSS = no I/O)
    tss.iopb_offset = sizeof(tss);

    // TODO: Update GDT with TSS address and load TR
    
    printk(KERN_INFO "  TSS initialized at 0x%lx\n", (u64)&tss);
}

void tss_set_rsp0(u64 rsp0) {
    tss.rsp0 = rsp0;
}

// ============================================================================
// IDT INITIALIZATION
// ============================================================================

// IDT entry structure
typedef struct __packed {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} idt_entry_t;

static idt_entry_t idt[256] __aligned(16);

typedef struct __packed {
    u16 limit;
    u64 base;
} idtr_t;

static idtr_t idtr;

// Interrupt handlers array
static void (*interrupt_handlers[256])(interrupt_frame_t *);

void idt_set_gate(u8 num, u64 handler, u16 selector, u8 type) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = selector;
    idt[num].ist = 0;
    idt[num].type_attr = type;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void register_interrupt_handler(u8 num, void (*handler)(interrupt_frame_t *)) {
    interrupt_handlers[num] = handler;
}

// Default exception handler
static void __used default_exception_handler(interrupt_frame_t *frame) {
    printk(KERN_ERR "Exception %lu! Error code: 0x%lx\n", 
           frame->int_no, frame->error_code);
    printk(KERN_ERR "  RIP: 0x%016lx  RSP: 0x%016lx\n", frame->rip, frame->rsp);
    printk(KERN_ERR "  CS:  0x%04lx        SS:  0x%04lx\n", frame->cs, frame->ss);
    printk(KERN_ERR "  RFLAGS: 0x%016lx\n", frame->rflags);
    
    if (frame->int_no == 14) { // Page fault
        printk(KERN_ERR "  CR2 (fault address): 0x%016lx\n", read_cr2());
    }
    
    // Halt
    cli();
    while (1) hlt();
}

// External ISR/IRQ tables from assembly
extern u64 isr_table[32];
extern u64 irq_table[16];
extern void syscall_entry(void);
extern void interrupts_init(void);

void idt_init(void) {
    // Clear IDT
    __builtin_memset(idt, 0, sizeof(idt));
    __builtin_memset(interrupt_handlers, 0, sizeof(interrupt_handlers));

    // Set up exception handlers (0-31)
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_table[i], 0x08, 0x8E);  // Present, DPL=0, Interrupt Gate
    }
    
    // Set up IRQ handlers (32-47)
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_table[i], 0x08, 0x8E);
    }
    
    // Set up syscall entry (INT 0x80 for compatibility)
    idt_set_gate(0x80, (u64)syscall_entry, 0x08, 0xEE);  // Present, DPL=3
    
    // Load IDT
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (u64)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));

    printk(KERN_INFO "  IDT loaded at 0x%lx\n", (u64)idt);
    
    // Initialize PIC, PIT, and interrupt handlers
    interrupts_init();
}

// ============================================================================
// PHYSICAL MEMORY MANAGER
// ============================================================================

#define MAX_MEM_REGIONS 64

static struct {
    mem_region_t regions[MAX_MEM_REGIONS];
    u32 num_regions;
    u64 total_memory;
    u64 free_memory;
    u64 used_memory;
    spinlock_t lock;
} pmm_state;

// Simple bitmap allocator for now
#define BITMAP_SIZE (256 * 1024 * 1024 / PAGE_SIZE / 64)  // Up to 256MB
static u64 page_bitmap[BITMAP_SIZE];
static u64 bitmap_base = 0;

void pmm_init(void) {
    __builtin_memset(&pmm_state, 0, sizeof(pmm_state));
    __builtin_memset(page_bitmap, 0xFF, sizeof(page_bitmap));  // All pages free
    printk(KERN_INFO "  PMM initialized (bitmap allocator)\n");
}

void pmm_add_region(phys_addr_t addr, size_t size) {
    if (pmm_state.num_regions >= MAX_MEM_REGIONS) return;
    
    pmm_state.regions[pmm_state.num_regions].base = addr;
    pmm_state.regions[pmm_state.num_regions].size = size;
    pmm_state.regions[pmm_state.num_regions].available = true;
    pmm_state.num_regions++;
    
    pmm_state.total_memory += size;
    pmm_state.free_memory += size;
    
    // Use first usable region as bitmap base
    if (bitmap_base == 0 && addr >= 0x100000) {
        bitmap_base = addr;
    }
}

void pmm_reserve_region(phys_addr_t addr, size_t size) {
    // Mark pages as used in bitmap
    if (bitmap_base == 0) return;
    
    u64 start_page = (addr - bitmap_base) / PAGE_SIZE;
    u64 num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (u64 i = 0; i < num_pages && start_page + i < BITMAP_SIZE * 64; i++) {
        u64 idx = (start_page + i) / 64;
        u64 bit = (start_page + i) % 64;
        page_bitmap[idx] &= ~(1UL << bit);
    }
    
    pmm_state.used_memory += size;
    pmm_state.free_memory -= size;
}

phys_addr_t pmm_alloc_page(void) {
    for (u64 i = 0; i < BITMAP_SIZE; i++) {
        if (page_bitmap[i] != 0) {
            int bit = __builtin_ffsll(page_bitmap[i]) - 1;
            page_bitmap[i] &= ~(1UL << bit);
            pmm_state.free_memory -= PAGE_SIZE;
            pmm_state.used_memory += PAGE_SIZE;
            return bitmap_base + (i * 64 + bit) * PAGE_SIZE;
        }
    }
    return 0; // Out of memory
}

void pmm_free_page(phys_addr_t addr) {
    if (addr < bitmap_base) return;
    
    u64 page = (addr - bitmap_base) / PAGE_SIZE;
    u64 idx = page / 64;
    u64 bit = page % 64;
    
    if (idx < BITMAP_SIZE) {
        page_bitmap[idx] |= (1UL << bit);
        pmm_state.free_memory += PAGE_SIZE;
        pmm_state.used_memory -= PAGE_SIZE;
    }
}

u64 pmm_get_total_memory(void) { return pmm_state.total_memory; }
u64 pmm_get_free_memory(void) { return pmm_state.free_memory; }
u64 pmm_get_used_memory(void) { return pmm_state.used_memory; }

// ============================================================================
// VIRTUAL MEMORY MANAGER
// ============================================================================

u64 *kernel_pml4 = (void *)0;

void vmm_init(void) {
    // Get current PML4 from CR3
    kernel_pml4 = (u64 *)read_cr3();
    printk(KERN_INFO "  VMM initialized (PML4 at 0x%lx)\n", (u64)kernel_pml4);
}

void vmm_flush_tlb(void) {
    write_cr3(read_cr3());
}

void vmm_flush_tlb_page(virt_addr_t addr) {
    invlpg((void *)addr);
}

// Get/allocate page table entry
static u64 *vmm_get_or_create_table(u64 *table, int index) {
    if (!(table[index] & PTE_PRESENT)) {
        // Allocate new page table
        phys_addr_t new_table = pmm_alloc_page();
        if (!new_table) return NULL;
        
        // Zero the new table  
        u64 *new_table_virt = (u64 *)(new_table + 0xFFFFFFFF80000000ULL);
        for (int i = 0; i < 512; i++) {
            new_table_virt[i] = 0;
        }
        
        table[index] = new_table | PTE_PRESENT | PTE_WRITE;
    }
    
    return (u64 *)((table[index] & ~0xFFFULL) + 0xFFFFFFFF80000000ULL);
}

// Map a single page (uses kernel_pml4)
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, u64 flags) {
    // Calculate indices
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx = (virt >> 21) & 0x1FF;
    int pt_idx = (virt >> 12) & 0x1FF;
    
    // Convert PML4 physical to virtual for kernel access
    u64 *pml4_virt = (u64 *)((u64)kernel_pml4 + 0xFFFFFFFF80000000ULL);
    
    // Walk/create page tables
    u64 *pdpt = vmm_get_or_create_table(pml4_virt, pml4_idx);
    if (!pdpt) return;
    
    u64 *pd = vmm_get_or_create_table(pdpt, pdpt_idx);
    if (!pd) return;
    
    u64 *pt = vmm_get_or_create_table(pd, pd_idx);
    if (!pt) return;
    
    // Set page table entry
    pt[pt_idx] = (phys & ~0xFFFULL) | (flags & 0xFFF) | PTE_PRESENT;
    
    // Flush TLB for this page
    vmm_flush_tlb_page(virt);
}

// Map a range of physical memory to virtual (for MMIO)
// MMIO virtual address allocator
static u64 mmio_virt_next = 0xFFFFFFFFC0000000ULL;  // -1GB region for MMIO

void *ioremap(phys_addr_t phys_addr, size_t size) {
    // Align to page boundary
    u64 offset = phys_addr & 0xFFF;
    phys_addr &= ~0xFFFULL;
    size = (size + offset + 0xFFF) & ~0xFFFULL;
    
    // Allocate virtual address space for this MMIO region
    u64 virt = mmio_virt_next;
    mmio_virt_next += size;
    
    // Map with device memory flags (uncached)
    for (u64 off = 0; off < size; off += 0x1000) {
        vmm_map_page(virt + off, phys_addr + off,
                     PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT);
    }
    
    return (void *)(virt + offset);
}

void iounmap(void *addr) {
    // Simple implementation - just leave mapped
    (void)addr;
}

// ============================================================================
// SLAB ALLOCATOR
// ============================================================================

static u8 heap[16 * 1024 * 1024] __aligned(PAGE_SIZE);  // 16MB heap
static size_t heap_used = 0;

void slab_init(void) {
    heap_used = 0;
    printk(KERN_INFO "  Slab allocator initialized (16MB heap)\n");
}

void *kmalloc(size_t size, gfp_t flags) {
    size = ALIGN(size, 16);
    if (heap_used + size > sizeof(heap)) {
        return NULL;
    }
    void *ptr = &heap[heap_used];
    heap_used += size;
    
    if (flags & __GFP_ZERO) {
        __builtin_memset(ptr, 0, size);
    }
    return ptr;
}

void *kzalloc(size_t size, gfp_t flags) {
    return kmalloc(size, flags | __GFP_ZERO);
}

void kfree(void *ptr) {
    // Simple bump allocator - no free support yet
    (void)ptr;
}

// ============================================================================
// LOCAL APIC
// ============================================================================

static volatile u32 *lapic_base = (void *)0xFFFFFFFFFEE00000UL;

void lapic_write(u32 reg, u32 value) {
    lapic_base[reg / 4] = value;
}

u32 lapic_read(u32 reg) {
    return lapic_base[reg / 4];
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

u32 lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_init(void) {
    // Skip LAPIC for now - using PIC instead
    // LAPIC requires mapping 0xFEE00000 which isn't in our 2MB pages
    printk(KERN_INFO "  LAPIC skipped (using PIC)\n");
}

// ============================================================================
// I/O APIC
// ============================================================================

static volatile u32 *ioapic_base = (void *)0xFFFFFFFFFEC00000UL;

void ioapic_write(u32 reg, u32 value) {
    ioapic_base[0] = reg;
    ioapic_base[4] = value;
}

u32 ioapic_read(u32 reg) {
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

void ioapic_init(void) {
    // Skip I/O APIC for now - using PIC
    printk(KERN_INFO "  I/O APIC skipped (using PIC)\n");
}

// ============================================================================
// SCHEDULER
// ============================================================================

static runqueue_t runqueues[NR_CPUS];
static task_struct_t *current_task[NR_CPUS];
static task_struct_t idle_tasks[NR_CPUS];

task_struct_t *get_current(void) {
    return current_task[smp_processor_id()];
}

void sched_init(void) {
    u32 cpu = smp_processor_id();
    
    __builtin_memset(&runqueues[cpu], 0, sizeof(runqueue_t));
    runqueues[cpu].cpu_id = cpu;
    
    // Create idle task
    __builtin_memset(&idle_tasks[cpu], 0, sizeof(task_struct_t));
    idle_tasks[cpu].pid = 0;
    idle_tasks[cpu].state = TASK_RUNNING;
    idle_tasks[cpu].flags = PF_IDLE | PF_KTHREAD;
    idle_tasks[cpu].policy = SCHED_IDLE;
    __builtin_memcpy(idle_tasks[cpu].comm, "swapper", 8);
    
    runqueues[cpu].idle = &idle_tasks[cpu];
    runqueues[cpu].curr = &idle_tasks[cpu];
    current_task[cpu] = &idle_tasks[cpu];
    
    printk(KERN_INFO "  Scheduler initialized for CPU %u\n", cpu);
}

// schedule() is implemented in process.c

void sched_yield(void) {
    extern void schedule(void);
    schedule();
}

// ============================================================================
// SMP
// ============================================================================

void smp_init(void) {
    // TODO: Parse ACPI MADT, send SIPI to APs
    kernel_state.num_cpus = 1;
    printk(KERN_INFO "  SMP: BSP is CPU %u (single CPU mode for now)\n", smp_processor_id());
}

// ============================================================================
// SYSCALL
// ============================================================================

void syscall_init(void) {
    // Set up SYSCALL/SYSRET MSRs
    // STAR: SYSCALL CS/SS in bits 47:32, SYSRET CS/SS in bits 63:48
    wrmsr(MSR_STAR, ((u64)GDT_USER_CODE << 48) | ((u64)GDT_CODE64 << 32));
    
    // LSTAR: SYSCALL entry point
    // wrmsr(MSR_LSTAR, (u64)syscall_entry);
    
    // SFMASK: Flags to clear on SYSCALL
    wrmsr(MSR_SFMASK, 0x200);  // Clear IF
    
    printk(KERN_INFO "  Syscall interface initialized\n");
}
