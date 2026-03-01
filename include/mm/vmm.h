/**
 * Picomimi-x64 Virtual Memory Manager Header
 */
#ifndef _MM_VMM_H
#define _MM_VMM_H

#include <kernel/types.h>

// Page table entry flags
#define PTE_PRESENT     (1UL << 0)
#define PTE_WRITE       (1UL << 1)
#define PTE_USER        (1UL << 2)
#define PTE_PWT         (1UL << 3)  // Write-through
#define PTE_PCD         (1UL << 4)  // Cache disable
#define PTE_ACCESSED    (1UL << 5)
#define PTE_DIRTY       (1UL << 6)
#define PTE_HUGE        (1UL << 7)  // 2MB/1GB page
#define PTE_GLOBAL      (1UL << 8)
#define PTE_NX          (1UL << 63) // No-execute

// Page table index extraction macros (4-level paging)
#define PML4_INDEX(va)  (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)  (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)    (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)    (((va) >> 12) & 0x1FF)

// Canonical address check
#define IS_CANONICAL(addr)  (((addr) >> 47) == 0 || ((addr) >> 47) == 0x1FFFF)

// Higher-half kernel offset
#define KERNEL_OFFSET   0xFFFFFFFF80000000UL
#define PHYS_OFFSET     0xFFFF888000000000UL  // Physical memory mapping base

// Address conversion
#define phys_to_virt(p) ((void *)((phys_addr_t)(p) + KERNEL_OFFSET))
#define virt_to_phys(v) ((phys_addr_t)(v) - KERNEL_OFFSET)

// VMM functions
void vmm_init(void);
void vmm_map_page(virt_addr_t virt, phys_addr_t phys, u64 flags);
void vmm_unmap_page(virt_addr_t virt);
phys_addr_t vmm_get_phys(virt_addr_t virt);

// Kernel page table
extern u64 *kernel_pml4;

// TLB management
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(virt_addr_t addr);
void vmm_flush_tlb_range(virt_addr_t start, virt_addr_t end);

#endif // _MM_VMM_H
