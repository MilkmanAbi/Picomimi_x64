/**
 * Picomimi-x64 Physical Memory Manager Header
 * 
 * Buddy allocator for physical page management
 * Inspired by Picomimi's memory management
 */
#ifndef _MM_PMM_H
#define _MM_PMM_H

#include <kernel/types.h>

// Maximum order for buddy allocator (2^10 = 1024 pages = 4MB)
#define MAX_ORDER           11

// Memory region structure
typedef struct {
    phys_addr_t base;
    size_t size;
    bool available;
} mem_region_t;

// Free page list for buddy allocator
typedef struct free_area {
    struct list_head free_list;
    u64 nr_free;
} free_area_t;

// Memory zone
typedef struct zone {
    const char *name;
    phys_addr_t start;
    phys_addr_t end;
    u64 total_pages;
    u64 free_pages;
    free_area_t free_area[MAX_ORDER];
    spinlock_t lock;
} zone_t;

// PMM functions
void pmm_init(void);
void pmm_add_region(phys_addr_t addr, size_t size);
void pmm_reserve_region(phys_addr_t addr, size_t size);

// Page allocation
phys_addr_t pmm_alloc_page(void);
phys_addr_t pmm_alloc_pages(u32 order);
void pmm_free_page(phys_addr_t addr);
void pmm_free_pages(phys_addr_t addr, u32 order);

// Statistics
u64 pmm_get_total_memory(void);
u64 pmm_get_free_memory(void);
u64 pmm_get_used_memory(void);
mem_pressure_t pmm_get_pressure(void);

// Page to physical address conversion
#define page_to_phys(page)  ((phys_addr_t)((page) << PAGE_SHIFT))
#define phys_to_page(phys)  ((phys) >> PAGE_SHIFT)

#endif // _MM_PMM_H
