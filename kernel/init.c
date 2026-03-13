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

/* =========================================================
 * Full 64-bit GDT
 * Matches the selector constants in include/arch/gdt.h:
 *   0x00 Null
 *   0x08 32-bit compat code (unused, fills slot)
 *   0x10 32-bit compat data (unused, fills slot)
 *   0x18 64-bit kernel code  ← GDT_CODE64 / KERNEL_CS
 *   0x20 64-bit kernel data  ← GDT_DATA64 / KERNEL_DS
 *   0x28 64-bit user code    ← GDT_USER_CODE / USER_CS
 *   0x30 64-bit user data    ← GDT_USER_DATA / USER_DS
 *   0x38 TSS descriptor low  (16-byte system descriptor)
 *   0x40 TSS descriptor high
 * ========================================================= */
static u64 runtime_gdt[9] __aligned(16);

typedef struct __packed {
    u16 limit;
    u64 base;
} gdtr_t;

static void gdt_set_entry(int idx, u64 descriptor) {
    runtime_gdt[idx] = descriptor;
}

/* Build a 64-bit code/data descriptor.
 * For 64-bit mode, most fields are ignored; only L, DPL, P, S, type matter. */
static u64 gdt_desc(int dpl, int is_code) {
    u64 d = 0;
    d |= (1ULL << 47);           /* P = present */
    d |= (1ULL << 44);           /* S = code/data (not system) */
    d |= ((u64)(dpl & 3) << 45); /* DPL */
    if (is_code) {
        d |= (1ULL << 43);       /* Code segment: executable */
        d |= (1ULL << 41);       /* Readable */
        d |= (1ULL << 53);       /* L = 64-bit code */
    } else {
        d |= (1ULL << 41);       /* Data segment: writable */
    }
    return d;
}

void gdt_init(void) {
    /* Initialise TSS */
    __builtin_memset(&tss, 0, sizeof(tss));

    extern char kernel_stack_top[];
    tss.rsp0 = (u64)kernel_stack_top;
    tss.iopb_offset = sizeof(tss);

    /* Build full GDT */
    runtime_gdt[0] = 0;                            /* 0x00 null */
    runtime_gdt[1] = 0x00CF9A000000FFFFULL;         /* 0x08 32-bit compat code */
    runtime_gdt[2] = 0x00CF92000000FFFFULL;         /* 0x10 32-bit compat data */
    runtime_gdt[3] = gdt_desc(0, 1);               /* 0x18 64-bit kernel code */
    runtime_gdt[4] = gdt_desc(0, 0);               /* 0x20 64-bit kernel data */
    runtime_gdt[5] = gdt_desc(3, 1);               /* 0x28 64-bit user code   */
    runtime_gdt[6] = gdt_desc(3, 0);               /* 0x30 64-bit user data   */

    /* TSS descriptor: 16-byte system descriptor (occupies slots 7+8) */
    u64 tss_base  = (u64)&tss;
    u64 tss_limit = sizeof(tss) - 1;

    /* Low 64 bits of TSS descriptor */
    u64 tss_lo = 0;
    tss_lo |= (tss_limit & 0xFFFF);               /* limit[15:0] */
    tss_lo |= ((tss_limit >> 16 & 0xF) << 48);    /* limit[19:16] */
    tss_lo |= ((tss_base  & 0xFFFFFF) << 16);     /* base[23:0]  */
    tss_lo |= ((tss_base >> 24 & 0xFF) << 56);    /* base[31:24] */
    tss_lo |= (1ULL << 47);                        /* P = present */
    tss_lo |= (0x9ULL << 40);                      /* type = 64-bit TSS available */
    runtime_gdt[7] = tss_lo;

    /* High 64 bits of TSS descriptor: base[63:32] in bits[31:0] */
    runtime_gdt[8] = (tss_base >> 32);

    /* Load the new GDT */
    gdtr_t gdtr = {
        .limit = (u16)(sizeof(runtime_gdt) - 1),
        .base  = (u64)runtime_gdt,
    };
    __asm__ volatile("lgdt %0" :: "m"(gdtr) : "memory");

    /* Reload all segment registers with the new selectors */
    __asm__ volatile(
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %w1, %%ds\n\t"
        "movw %w1, %%es\n\t"
        "movw %w1, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "i"((u64)0x18), "r"((u16)0x20)
        : "rax", "memory"
    );

    /* Load TR with the TSS selector */
    __asm__ volatile("ltr %w0" :: "r"((u16)0x38) : "memory");

    printk(KERN_INFO "  GDT loaded (9 entries), TSS at 0x%lx, TR=0x38\n",
           (u64)&tss);
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
        idt_set_gate(i, isr_table[i], 0x18, 0x8E);  // Present, DPL=0, Interrupt Gate
    }
    
    // Set up IRQ handlers (32-47)
    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_table[i], 0x18, 0x8E);
    }
    
    // Set up syscall entry (INT 0x80 for compatibility)
    idt_set_gate(0x80, (u64)syscall_entry, 0x18, 0xEE);  // Present, DPL=3
    
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
        
        /* Always set USER on intermediate entries: the leaf PTE controls
         * the actual permission. Without U on every level, user-mode
         * accesses fault even if the leaf has PTE_USER. */
        table[index] = new_table | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        /* Ensure USER bit is set on existing intermediate entries too */
        table[index] |= PTE_USER;
    }
    
    return (u64 *)((table[index] & ~0xFFFULL) + 0xFFFFFFFF80000000ULL);
}

// Map a single page
// - kernel addresses (bit 63 set): always use kernel_pml4
// - user addresses: use current CR3 (which is the active process PML4)
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, u64 flags) {
    if (virt == 0) {
        printk(KERN_WARNING "[vmm] WARNING: mapping virt=0 phys=0x%llx flags=0x%llx\n",
               (u64)phys, flags);
        return; /* never map the null page */
    }
    // Calculate indices
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx = (virt >> 21) & 0x1FF;
    int pt_idx = (virt >> 12) & 0x1FF;

    // Choose PML4: kernel space uses kernel_pml4, user space uses active CR3
    phys_addr_t pgd_phys;
    if (virt & (1ULL << 63)) {
        pgd_phys = (phys_addr_t)kernel_pml4;   /* kernel_pml4 stored as phys */
    } else {
        pgd_phys = read_cr3() & ~0xFFFULL;     /* current process PML4 */
    }
    u64 *pml4_virt = (u64 *)(pgd_phys + 0xFFFFFFFF80000000ULL);
    
    // Walk/create page tables
    u64 *pdpt = vmm_get_or_create_table(pml4_virt, pml4_idx);
    if (!pdpt) return;
    
    u64 *pd = vmm_get_or_create_table(pdpt, pdpt_idx);
    if (!pd) return;
    
    u64 *pt = vmm_get_or_create_table(pd, pd_idx);
    if (!pt) return;
    
    // Set page table entry
    // flags & 0x0FFF captures bits 0-11 (present, write, user, etc.)
    // flags & PTE_NX  captures bit 63 (no-execute)
    pt[pt_idx] = (phys & ~0xFFFULL) | (flags & 0x0FFFULL) | (flags & PTE_NX) | PTE_PRESENT;
    
    // Flush TLB for this page
    vmm_flush_tlb_page(virt);
}

/* Map a page into a specific PGD (physical address) rather than current CR3.
 * Used by mm_copy to populate child page tables without switching CR3. */
void vmm_map_page_in_pgd(u64 pgd_phys, u64 virt, u64 phys, u64 flags) {
    if (!virt) return;
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    u64 *pml4_virt = (u64 *)(pgd_phys + 0xFFFFFFFF80000000ULL);
    u64 *pdpt = vmm_get_or_create_table(pml4_virt, pml4_idx);
    if (!pdpt) return;
    u64 *pd = vmm_get_or_create_table(pdpt, pdpt_idx);
    if (!pd) return;
    u64 *pt = vmm_get_or_create_table(pd, pd_idx);
    if (!pt) return;
    pt[pt_idx] = (phys & ~0xFFFULL) | (flags & 0x0FFFULL) | (flags & PTE_NX) | PTE_PRESENT;
}


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

/* ============================================================================
 * Real free-list slab allocator — replaces bump allocator
 *
 * Design:
 *   - 64 MB static heap (same size as before, keeps large FB back-buffers)
 *   - Every allocation is prefixed by an 8-byte header:
 *       [size:u32][magic:u32]  (ALLOC_MAGIC or FREE_MAGIC)
 *   - kfree() marks the header FREE and merges adjacent free blocks
 *     (first-fit coalesce on free)
 *   - kmalloc() uses first-fit search in the free list; falls back to bump
 *     if no free block is large enough
 *   - Thread safety: simple spinlock (cli/sti pair) — same as rest of kernel
 * ========================================================================== */

#define SLAB_MAGIC_ALLOC  0xA110CA7EU
#define SLAB_MAGIC_FREE   0xFEE1DEAD

typedef struct slab_hdr {
    u32 size;        /* payload bytes (not including this header) */
    u32 magic;       /* SLAB_MAGIC_ALLOC or SLAB_MAGIC_FREE       */
} __attribute__((packed)) slab_hdr_t;

#define SLAB_HDR_SZ  sizeof(slab_hdr_t)   /* 8 bytes */

static u8 heap[64 * 1024 * 1024] __aligned(PAGE_SIZE);
static size_t heap_bump = 0;          /* bump pointer for virgin territory */
static size_t heap_used = 0;          /* live allocated bytes (approx)     */
static size_t heap_freed = 0;         /* bytes returned via kfree           */

/* Spinlock: 0=free, 1=held */
static volatile int slab_lock = 0;

static inline void slab_acquire(void) {
    while (__sync_lock_test_and_set(&slab_lock, 1)) {
        __asm__ volatile("pause");
    }
}
static inline void slab_release(void) {
    __sync_lock_release(&slab_lock);
}

void slab_init(void) {
    heap_bump = 0;
    heap_used = 0;
    heap_freed = 0;
    printk(KERN_INFO "  Slab allocator initialized (64MB heap, free-list enabled)\n");
}

/* Internal: carve a new block from the virgin region. */
static void *slab_bump(size_t payload) {
    size_t total = SLAB_HDR_SZ + payload;
    if (heap_bump + total > sizeof(heap))
        return NULL;
    slab_hdr_t *hdr = (slab_hdr_t *)&heap[heap_bump];
    hdr->size  = (u32)payload;
    hdr->magic = SLAB_MAGIC_ALLOC;
    heap_bump += total;
    heap_used += payload;
    return (u8 *)hdr + SLAB_HDR_SZ;
}

void *kmalloc(size_t size, gfp_t flags) {
    if (!size) return NULL;
    size = ALIGN(size, 16);          /* 16-byte alignment */

    slab_acquire();

    /* First-fit search through the already-bumped region for a free block. */
    if (heap_freed > 0) {
        size_t off = 0;
        while (off + SLAB_HDR_SZ <= heap_bump) {
            slab_hdr_t *hdr = (slab_hdr_t *)&heap[off];
            if (hdr->magic == SLAB_MAGIC_FREE && hdr->size >= size) {
                /* Found a big enough free block. */
                if (hdr->size >= size + SLAB_HDR_SZ + 16) {
                    /* Split: carve off what we need; leave the remainder free. */
                    size_t leftover = hdr->size - size - SLAB_HDR_SZ;
                    slab_hdr_t *tail = (slab_hdr_t *)((u8 *)hdr + SLAB_HDR_SZ + size);
                    tail->size  = (u32)leftover;
                    tail->magic = SLAB_MAGIC_FREE;
                    hdr->size   = (u32)size;
                } else {
                    heap_freed -= hdr->size;   /* entire block consumed */
                }
                hdr->magic  = SLAB_MAGIC_ALLOC;
                heap_used  += hdr->size;
                void *ptr   = (u8 *)hdr + SLAB_HDR_SZ;
                slab_release();
                if (flags & __GFP_ZERO) __builtin_memset(ptr, 0, size);
                return ptr;
            }
            off += SLAB_HDR_SZ + hdr->size;
        }
    }

    /* No suitable free block — bump-allocate from virgin region. */
    void *ptr = slab_bump(size);
    slab_release();

    if (!ptr) {
        printk(KERN_ERR "[kmalloc] OOM: size=%zu bump=%zu/%zu freed=%zu\n",
               size, heap_bump, sizeof(heap), heap_freed);
        return NULL;
    }
    if (flags & __GFP_ZERO) __builtin_memset(ptr, 0, size);
    return ptr;
}

size_t kmalloc_used(void)  { return heap_used;  }
size_t kmalloc_total(void) { return sizeof(heap); }

void *kzalloc(size_t size, gfp_t flags) {
    return kmalloc(size, flags | __GFP_ZERO);
}

void kfree(void *ptr) {
    if (!ptr) return;

    slab_hdr_t *hdr = (slab_hdr_t *)((u8 *)ptr - SLAB_HDR_SZ);

    /* Sanity check — catch double-free and bogus pointers. */
    if (hdr->magic == SLAB_MAGIC_FREE) {
        printk(KERN_WARNING "[kfree] double-free at %p (caller=%p) size=%u\n",
               ptr, __builtin_return_address(0), hdr->size);
        return;
    }
    if (hdr->magic != SLAB_MAGIC_ALLOC) {
        printk(KERN_WARNING "[kfree] corrupt header at %p (magic=0x%x)\n",
               ptr, hdr->magic);
        return;
    }

    slab_acquire();

    size_t payload = hdr->size;
    hdr->magic     = SLAB_MAGIC_FREE;
    heap_used     -= payload;
    heap_freed    += payload;

    /* Forward-coalesce: merge with the next free block if adjacent. */
    size_t off = (size_t)((u8 *)hdr - heap);
    size_t next_off = off + SLAB_HDR_SZ + hdr->size;
    if (next_off + SLAB_HDR_SZ <= heap_bump) {
        slab_hdr_t *next = (slab_hdr_t *)&heap[next_off];
        if (next->magic == SLAB_MAGIC_FREE) {
            hdr->size  += SLAB_HDR_SZ + next->size;
            heap_freed -= SLAB_HDR_SZ; /* the merged header is recovered */
        }
    }

    slab_release();
}

// ============================================================================
// I/O APIC (stub - real LAPIC is in arch/x86_64/smp/smp.c)
// ============================================================================

void ioapic_init(void) {
    printk(KERN_INFO "  I/O APIC skipped (using PIC)\n");
}
void ioapic_write(u32 reg, u32 value) { (void)reg; (void)value; }
u32  ioapic_read(u32 reg)             { (void)reg; return 0; }

// ============================================================================
// SCHEDULER - real implementation in kernel/sched.c and kernel/process.c
// ============================================================================

void sched_yield(void) {
    extern void schedule(void);
    schedule();
}

// ============================================================================
// SMP
// ============================================================================

/* smp_init() is implemented in arch/x86_64/smp/smp.c */

// ============================================================================
// SYSCALL
// ============================================================================

/* =========================================================
 * Per-CPU data structure for syscall entry stack swap.
 * GS_BASE (kernel) must point here.
 * Layout:
 *   [0]  = kernel RSP (for SYSCALL: swapgs loads kernel RSP from here)
 *   [8]  = scratch (used to save user RSP temporarily)
 * ========================================================= */
typedef struct {
    u64 kernel_rsp;    /* offset 0: loaded by syscall_entry after swapgs */
    u64 user_rsp_tmp;  /* offset 8: scratch for saving user RSP           */
} per_cpu_data_t;

static per_cpu_data_t per_cpu_bsp __aligned(16);
u64 per_cpu_bsp_addr = 0;  /* exported for assembly (switch_to_user) */

void syscall_init(void) {
    // Set up SYSCALL/SYSRET MSRs
    // STAR: SYSCALL CS/SS in bits 47:32, SYSRET CS/SS in bits 63:48
    wrmsr(MSR_STAR, ((u64)GDT_USER_CODE << 48) | ((u64)GDT_CODE64 << 32));

    // LSTAR: SYSCALL entry point (was incorrectly commented out)
    extern void syscall_entry(void);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);

    // SFMASK: Flags to clear on SYSCALL (clear IF so we don't take IRQs
    //         while on user stack before we swap to kernel stack)
    wrmsr(MSR_SFMASK, 0x200);  // Clear IF on SYSCALL

    // KERNEL_GS_BASE: points to per-CPU data; swapgs in syscall_entry
    // makes %gs point here, giving access to kernel_rsp at gs:0 and
    // the user_rsp scratch slot at gs:8.
    // Initialize kernel_rsp to the BSP kernel stack top.
    extern char kernel_stack_top[];
    per_cpu_bsp.kernel_rsp = (u64)kernel_stack_top;

    // Write KERNEL_GS_BASE MSR (0xC0000102)
    u64 gsbase = (u64)&per_cpu_bsp;
    per_cpu_bsp_addr = gsbase;
    wrmsr(0xC0000102UL, gsbase);

    printk(KERN_INFO "  Syscall interface initialized (LSTAR=%p GSBASE=%p)\n",
           (void *)syscall_entry, (void *)gsbase);
}

/* Update the kernel RSP in the per-CPU structure.
 * Called by the scheduler whenever it switches to a new task. */
void percpu_set_kernel_rsp(u64 rsp) {
    per_cpu_bsp.kernel_rsp = rsp;
}
