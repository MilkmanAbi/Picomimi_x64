/**
 * Picomimi-x64 Advanced Memory Management
 * 
 * Features:
 * - Memory zones (DMA, Normal, High)
 * - Buddy allocator for page allocation
 * - SLAB allocator for kernel objects
 * - Copy-on-Write (COW) for fork()
 * - Demand paging with page fault handler
 * - Memory-mapped files
 * - Anonymous memory
 * - Swap support (framework)
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>

// Use zone types from types.h
#define NR_ZONES MAX_NR_ZONES

#define ZONE_DMA_END        (16 * 1024 * 1024)
#define ZONE_DMA32_END      (4ULL * 1024 * 1024 * 1024)

// External atomic operations (from process.c)
extern void atomic_set(atomic_t *v, int i);
extern int atomic_read(atomic_t *v);
extern void atomic_inc(atomic_t *v);
extern void atomic_dec(atomic_t *v);
extern int atomic_dec_return(atomic_t *v);
extern int atomic_inc_return(atomic_t *v);

// ============================================================================
// PAGE FLAGS
// ============================================================================

#define PG_locked       0   // Page is locked
#define PG_referenced   1   // Page was accessed
#define PG_uptodate     2   // Page data is valid
#define PG_dirty        3   // Page needs writeback
#define PG_lru          4   // On LRU list
#define PG_active       5   // On active list (not inactive)
#define PG_slab         6   // Page is used by slab allocator
#define PG_reserved     7   // Reserved (kernel, MMIO)
#define PG_private      8   // Has private data
#define PG_writeback    9   // Page is being written back
#define PG_compound     10  // Part of compound page
#define PG_swapcache    11  // In swap cache
#define PG_mappedtodisk 12  // Has disk blocks allocated
#define PG_reclaim      13  // To be reclaimed
#define PG_swapbacked   14  // Anonymous or swap-backed
#define PG_unevictable  15  // Cannot be evicted

// ============================================================================
// PAGE STRUCTURE
// ============================================================================

typedef struct page {
    u32             flags;          // Page flags
    atomic_t        _refcount;      // Reference count
    atomic_t        _mapcount;      // Number of PTEs mapping this
    
    union {
        struct {
            // LRU list management
            struct list_head lru;
            // Address space this page belongs to
            void *mapping;
            // Offset within file/swap
            u64 index;
        };
        struct {
            // Slab allocator
            void *slab_cache;
            void *freelist;
            u32 objects;
            u32 inuse;
        };
        struct {
            // Compound pages
            struct page *first_page;
            u8 compound_order;
        };
        struct {
            // Page table page
            u64 pt_usage;
        };
    };
    
    // Private data
    void            *private;
    
    // Physical address (implicit from array index)
} page_t;

// ============================================================================
// ZONE STRUCTURE
// ============================================================================

#define MAX_ORDER   11  // Up to 2^10 = 1024 pages = 4MB blocks

typedef struct free_area {
    struct list_head    free_list;
    u64                 nr_free;
} free_area_t;

typedef struct zone {
    // Zone identity
    const char          *name;
    zone_type_t         type;
    
    // Physical range
    phys_addr_t         start_pfn;
    phys_addr_t         end_pfn;
    u64                 present_pages;
    u64                 spanned_pages;
    
    // Buddy allocator
    free_area_t         free_area[MAX_ORDER];
    spinlock_t          lock;
    
    // Statistics
    u64                 pages_free;
    u64                 pages_min;      // Minimum free threshold
    u64                 pages_low;      // Low watermark
    u64                 pages_high;     // High watermark
    u64                 pages_scanned;  // Pages scanned since last reclaim
    
    // LRU lists
    struct list_head    lru_active;     // Active pages
    struct list_head    lru_inactive;   // Inactive pages
    u64                 nr_active;
    u64                 nr_inactive;
    
    // Zone statistics
    u64                 stat[16];
} zone_t;

// ============================================================================
// PAGE TABLE FLAGS (extended)
// ============================================================================

#define _PAGE_PRESENT       (1UL << 0)
#define _PAGE_WRITE         (1UL << 1)
#define _PAGE_USER          (1UL << 2)
#define _PAGE_PWT           (1UL << 3)
#define _PAGE_PCD           (1UL << 4)
#define _PAGE_ACCESSED      (1UL << 5)
#define _PAGE_DIRTY         (1UL << 6)
#define _PAGE_PSE           (1UL << 7)  // Huge page
#define _PAGE_GLOBAL        (1UL << 8)
#define _PAGE_COW           (1UL << 9)  // Copy-on-write (using software bit)
#define _PAGE_NX            (1UL << 63) // No execute

#define PAGE_KERNEL         (_PAGE_PRESENT | _PAGE_WRITE)
#define PAGE_KERNEL_RO      (_PAGE_PRESENT)
#define PAGE_USER           (_PAGE_PRESENT | _PAGE_WRITE | _PAGE_USER)
#define PAGE_USER_RO        (_PAGE_PRESENT | _PAGE_USER)
#define PAGE_COW            (_PAGE_PRESENT | _PAGE_USER | _PAGE_COW)

// ============================================================================
// VMA FLAGS (extended) - base flags in process.h, extras here
// ============================================================================

// Additional VMA flags not in process.h
#define VM_MAYREAD      0x00000010
#define VM_MAYWRITE     0x00000020
#define VM_MAYEXEC      0x00000040
#define VM_MAYSHARE     0x00000080
#define VM_PFNMAP       0x00000400  // Direct physical mapping
#define VM_DONTCOPY     0x00020000
#define VM_DONTEXPAND   0x00040000
#define VM_HUGETLB      0x00400000
#define VM_MIXEDMAP     0x10000000
#define VM_ANON         0x20000000  // Anonymous mapping
#define VM_FILE         0x40000000  // File-backed mapping

// ============================================================================
// GLOBAL STATE
// ============================================================================

static zone_t zones[NR_ZONES];
static page_t *mem_map;             // Page array
static u64 max_pfn;                 // Maximum page frame number
static u64 min_low_pfn;             // First usable PFN
static u64 total_pages;
static u64 free_pages;

extern u64 *kernel_pml4;

// ============================================================================
// PAGE HELPERS
// ============================================================================

static inline u64 page_to_pfn(page_t *page) {
    return page - mem_map;
}

static inline phys_addr_t page_to_phys(page_t *page) {
    return page_to_pfn(page) << PAGE_SHIFT;
}

static inline page_t *pfn_to_page(u64 pfn) {
    return &mem_map[pfn];
}

static inline page_t *phys_to_page(phys_addr_t addr) {
    return pfn_to_page(addr >> PAGE_SHIFT);
}

static inline int page_count(page_t *page) {
    return atomic_read(&page->_refcount);
}

static inline void get_page(page_t *page) {
    atomic_inc(&page->_refcount);
}

static inline void put_page(page_t *page) {
    if (atomic_dec_return(&page->_refcount) == 0) {
        // Free page back to buddy allocator
        extern void __free_page(page_t *page);
        __free_page(page);
    }
}

static inline void set_page_count(page_t *page, int v) {
    atomic_set(&page->_refcount, v);
}

static inline int PageLocked(page_t *page) {
    return page->flags & (1 << PG_locked);
}

static inline void SetPageLocked(page_t *page) {
    page->flags |= (1 << PG_locked);
}

static inline void ClearPageLocked(page_t *page) {
    page->flags &= ~(1 << PG_locked);
}

static inline int PageDirty(page_t *page) {
    return page->flags & (1 << PG_dirty);
}

static inline void SetPageDirty(page_t *page) {
    page->flags |= (1 << PG_dirty);
}

// ============================================================================
// BUDDY ALLOCATOR
// ============================================================================

static inline int get_order(size_t size) {
    int order = 0;
    size = (size - 1) >> PAGE_SHIFT;
    while (size) {
        order++;
        size >>= 1;
    }
    return order;
}

// Find zone for physical address
static zone_t *page_zone(page_t *page) {
    phys_addr_t phys = page_to_phys(page);
    
    if (phys < ZONE_DMA_END) return &zones[ZONE_DMA];
    if (phys < ZONE_DMA32_END) return &zones[ZONE_DMA32];
    return &zones[ZONE_NORMAL];
}

// Allocate 2^order pages from zone
static page_t *__alloc_pages_zone(zone_t *zone, int order) {
    spin_lock(&zone->lock);
    
    // Find smallest available block
    for (int current_order = order; current_order < MAX_ORDER; current_order++) {
        free_area_t *area = &zone->free_area[current_order];
        
        if (list_empty(&area->free_list)) {
            continue;
        }
        
        // Get first free block
        page_t *page = list_first_entry(&area->free_list, page_t, lru);
        list_del(&page->lru);
        area->nr_free--;
        
        // Split block if necessary
        while (current_order > order) {
            current_order--;
            area = &zone->free_area[current_order];
            
            // Add buddy to free list
            page_t *buddy = page + (1 << current_order);
            list_add(&buddy->lru, &area->free_list);
            area->nr_free++;
        }
        
        // Mark page as allocated
        page->flags &= ~(1 << PG_reserved);
        set_page_count(page, 1);
        
        zone->pages_free -= (1 << order);
        free_pages -= (1 << order);
        
        spin_unlock(&zone->lock);
        return page;
    }
    
    spin_unlock(&zone->lock);
    return NULL;
}

// Allocate pages
page_t *alloc_pages(gfp_t gfp_mask, int order) {
    page_t *page = NULL;
    
    // Try zones in order based on GFP flags
    if (gfp_mask & __GFP_DMA) {
        page = __alloc_pages_zone(&zones[ZONE_DMA], order);
    }
    
    if (!page && (gfp_mask & __GFP_DMA32)) {
        page = __alloc_pages_zone(&zones[ZONE_DMA32], order);
    }
    
    if (!page) {
        page = __alloc_pages_zone(&zones[ZONE_NORMAL], order);
    }
    
    if (!page && !(gfp_mask & __GFP_NOWARN)) {
        printk(KERN_WARNING "MM: Failed to allocate %d pages\n", 1 << order);
    }
    
    if (page && (gfp_mask & __GFP_ZERO)) {
        memset((void *)page_to_phys(page), 0, (1 << order) * PAGE_SIZE);
    }
    
    return page;
}

// Free pages back to buddy allocator
void __free_pages(page_t *page, int order) {
    zone_t *zone = page_zone(page);
    u64 pfn = page_to_pfn(page);
    
    spin_lock(&zone->lock);
    
    // Try to coalesce with buddy
    while (order < MAX_ORDER - 1) {
        // Find buddy PFN
        u64 buddy_pfn = pfn ^ (1 << order);
        page_t *buddy = pfn_to_page(buddy_pfn);
        
        // Check if buddy is free and same order
        int buddy_free = 0;
        free_area_t *area = &zone->free_area[order];
        struct list_head *pos;
        
        list_for_each(pos, &area->free_list) {
            if (list_entry(pos, page_t, lru) == buddy) {
                buddy_free = 1;
                break;
            }
        }
        
        if (!buddy_free) break;
        
        // Remove buddy from free list
        list_del(&buddy->lru);
        area->nr_free--;
        
        // Merge: use lower PFN
        if (buddy_pfn < pfn) {
            pfn = buddy_pfn;
            page = buddy;
        }
        
        order++;
    }
    
    // Add to free list
    free_area_t *area = &zone->free_area[order];
    list_add(&page->lru, &area->free_list);
    area->nr_free++;
    
    zone->pages_free += (1 << order);
    free_pages += (1 << order);
    
    spin_unlock(&zone->lock);
}

void __free_page(page_t *page) {
    __free_pages(page, 0);
}

// Convenience functions
page_t *alloc_page(gfp_t gfp_mask) {
    return alloc_pages(gfp_mask, 0);
}

void free_page(page_t *page) {
    __free_pages(page, 0);
}

unsigned long __get_free_pages(gfp_t gfp_mask, int order) {
    page_t *page = alloc_pages(gfp_mask, order);
    if (!page) return 0;
    return page_to_phys(page);
}

unsigned long __get_free_page(gfp_t gfp_mask) {
    return __get_free_pages(gfp_mask, 0);
}

// ============================================================================
// COPY-ON-WRITE (COW)
// ============================================================================

// Mark PTE as copy-on-write
static void pte_mkwrite(u64 *pte) {
    *pte |= _PAGE_WRITE;
    *pte &= ~_PAGE_COW;
}

static void pte_wrprotect(u64 *pte) {
    *pte &= ~_PAGE_WRITE;
}

static void pte_mkcow(u64 *pte) {
    *pte &= ~_PAGE_WRITE;  // Remove write permission
    *pte |= _PAGE_COW;     // Set COW flag
}

static int pte_cow(u64 pte) {
    return (pte & _PAGE_COW) != 0;
}

// Copy page tables with COW for fork()
int copy_page_tables_cow(u64 *dst_pml4, u64 *src_pml4, u64 start, u64 end) {
    // Iterate through address range
    for (u64 addr = start; addr < end; addr += PAGE_SIZE) {
        // Walk source page tables
        u64 pml4_idx = (addr >> 39) & 0x1FF;
        u64 pdpt_idx = (addr >> 30) & 0x1FF;
        u64 pd_idx   = (addr >> 21) & 0x1FF;
        u64 pt_idx   = (addr >> 12) & 0x1FF;
        
        if (!(src_pml4[pml4_idx] & _PAGE_PRESENT)) continue;
        
        u64 *src_pdpt = (u64 *)(src_pml4[pml4_idx] & ~0xFFF);
        if (!(src_pdpt[pdpt_idx] & _PAGE_PRESENT)) continue;
        
        u64 *src_pd = (u64 *)(src_pdpt[pdpt_idx] & ~0xFFF);
        if (!(src_pd[pd_idx] & _PAGE_PRESENT)) continue;
        
        u64 *src_pt = (u64 *)(src_pd[pd_idx] & ~0xFFF);
        if (!(src_pt[pt_idx] & _PAGE_PRESENT)) continue;
        
        u64 pte = src_pt[pt_idx];
        
        // Check if writable
        if (pte & _PAGE_WRITE) {
            // Make both entries COW
            pte_mkcow(&src_pt[pt_idx]);
            pte |= _PAGE_COW;
            pte &= ~_PAGE_WRITE;
            
            // Increment page refcount
            page_t *page = phys_to_page(pte & ~0xFFF);
            get_page(page);
        }
        
        // Create destination page table entries if needed
        if (!(dst_pml4[pml4_idx] & _PAGE_PRESENT)) {
            page_t *new_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
            if (!new_page) return -ENOMEM;
            dst_pml4[pml4_idx] = page_to_phys(new_page) | PAGE_USER;
        }
        
        u64 *dst_pdpt = (u64 *)(dst_pml4[pml4_idx] & ~0xFFF);
        if (!(dst_pdpt[pdpt_idx] & _PAGE_PRESENT)) {
            page_t *new_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
            if (!new_page) return -ENOMEM;
            dst_pdpt[pdpt_idx] = page_to_phys(new_page) | PAGE_USER;
        }
        
        u64 *dst_pd = (u64 *)(dst_pdpt[pdpt_idx] & ~0xFFF);
        if (!(dst_pd[pd_idx] & _PAGE_PRESENT)) {
            page_t *new_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
            if (!new_page) return -ENOMEM;
            dst_pd[pd_idx] = page_to_phys(new_page) | PAGE_USER;
        }
        
        u64 *dst_pt = (u64 *)(dst_pd[pd_idx] & ~0xFFF);
        
        // Copy PTE with COW flag
        dst_pt[pt_idx] = pte;
    }
    
    return 0;
}

// Handle COW page fault
int handle_cow_fault(u64 fault_addr, u64 *pte) {
    u64 old_phys = *pte & ~0xFFF;
    page_t *old_page = phys_to_page(old_phys);
    
    // Check if we're the only reference
    if (page_count(old_page) == 1) {
        // Just make it writable
        pte_mkwrite(pte);
        return 0;
    }
    
    // Allocate new page
    page_t *new_page = alloc_page(GFP_KERNEL);
    if (!new_page) return -ENOMEM;
    
    // Copy content
    memcpy((void *)page_to_phys(new_page), (void *)old_phys, PAGE_SIZE);
    
    // Update PTE
    *pte = page_to_phys(new_page) | (_PAGE_PRESENT | _PAGE_WRITE | _PAGE_USER);
    
    // Release old page
    put_page(old_page);
    
    // Flush TLB for this address
    __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
    
    return 0;
}

// ============================================================================
// DEMAND PAGING
// ============================================================================

// Anonymous page fault - allocate a new zero page
int handle_anonymous_fault(u64 fault_addr, vm_area_t *vma) {
    // Allocate new page
    page_t *page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!page) return -ENOMEM;
    
    // Calculate page table indices
    u64 pml4_idx = (fault_addr >> 39) & 0x1FF;
    u64 pdpt_idx = (fault_addr >> 30) & 0x1FF;
    u64 pd_idx   = (fault_addr >> 21) & 0x1FF;
    u64 pt_idx   = (fault_addr >> 12) & 0x1FF;
    
    // Get current task's PML4
    task_struct_t *current = get_current_task();
    if (!current || !current->mm) return -EFAULT;
    
    u64 *pml4 = current->mm->pgd;
    
    // Ensure page tables exist
    if (!(pml4[pml4_idx] & _PAGE_PRESENT)) {
        page_t *pt_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pt_page) { free_page(page); return -ENOMEM; }
        pml4[pml4_idx] = page_to_phys(pt_page) | PAGE_USER;
    }
    
    u64 *pdpt = (u64 *)(pml4[pml4_idx] & ~0xFFF);
    if (!(pdpt[pdpt_idx] & _PAGE_PRESENT)) {
        page_t *pt_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pt_page) { free_page(page); return -ENOMEM; }
        pdpt[pdpt_idx] = page_to_phys(pt_page) | PAGE_USER;
    }
    
    u64 *pd = (u64 *)(pdpt[pdpt_idx] & ~0xFFF);
    if (!(pd[pd_idx] & _PAGE_PRESENT)) {
        page_t *pt_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pt_page) { free_page(page); return -ENOMEM; }
        pd[pd_idx] = page_to_phys(pt_page) | PAGE_USER;
    }
    
    u64 *pt = (u64 *)(pd[pd_idx] & ~0xFFF);
    
    // Set up PTE with appropriate permissions
    u64 pte_flags = _PAGE_PRESENT | _PAGE_USER;
    if (vma->flags & VM_WRITE) pte_flags |= _PAGE_WRITE;
    if (!(vma->flags & VM_EXEC)) pte_flags |= _PAGE_NX;
    
    pt[pt_idx] = page_to_phys(page) | pte_flags;
    
    // Flush TLB
    __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
    
    return 0;
}

// Main page fault handler
int do_page_fault(u64 fault_addr, u64 error_code) {
    task_struct_t *current = get_current_task();
    
    // Error code bits
    int present = error_code & 0x1;      // Page was present
    int write = error_code & 0x2;        // Write access
    int user = error_code & 0x4;         // User mode access
    int reserved = error_code & 0x8;     // Reserved bit set
    int exec = error_code & 0x10;        // Instruction fetch
    
    (void)exec;
    
    // Reserved bit violation - serious error
    if (reserved) {
        printk(KERN_ERR "MM: Reserved bit violation at 0x%lx\n", fault_addr);
        return -EFAULT;
    }
    
    // Kernel fault in user space?
    if (!user && fault_addr < 0xFFFF800000000000ULL) {
        printk(KERN_ERR "MM: Kernel accessed user space at 0x%lx\n", fault_addr);
        return -EFAULT;
    }
    
    if (!current || !current->mm) {
        printk(KERN_ERR "MM: Page fault with no mm at 0x%lx\n", fault_addr);
        return -EFAULT;
    }
    
    // Find VMA for fault address
    vm_area_t *vma = current->mm->mmap;
    while (vma) {
        if (fault_addr >= vma->start && fault_addr < vma->end) {
            break;
        }
        vma = vma->next;
    }
    
    if (!vma) {
        // Check for stack growth
        if (fault_addr >= current->mm->start_stack - (1024 * PAGE_SIZE) &&
            fault_addr < current->mm->start_stack) {
            // Expand stack
            // TODO: Implement stack growth
        }
        printk(KERN_ERR "MM: Segmentation fault at 0x%lx (no VMA)\n", fault_addr);
        return -EFAULT;
    }
    
    // Check permissions
    if (write && !(vma->flags & VM_WRITE)) {
        // Check for COW
        u64 *pml4 = current->mm->pgd;
        u64 pml4_idx = (fault_addr >> 39) & 0x1FF;
        u64 pdpt_idx = (fault_addr >> 30) & 0x1FF;
        u64 pd_idx   = (fault_addr >> 21) & 0x1FF;
        u64 pt_idx   = (fault_addr >> 12) & 0x1FF;
        
        if (pml4[pml4_idx] & _PAGE_PRESENT) {
            u64 *pdpt = (u64 *)(pml4[pml4_idx] & ~0xFFF);
            if (pdpt[pdpt_idx] & _PAGE_PRESENT) {
                u64 *pd = (u64 *)(pdpt[pdpt_idx] & ~0xFFF);
                if (pd[pd_idx] & _PAGE_PRESENT) {
                    u64 *pt = (u64 *)(pd[pd_idx] & ~0xFFF);
                    if (pte_cow(pt[pt_idx])) {
                        return handle_cow_fault(fault_addr, &pt[pt_idx]);
                    }
                }
            }
        }
        
        printk(KERN_ERR "MM: Write to read-only at 0x%lx\n", fault_addr);
        return -EFAULT;
    }
    
    // Handle based on whether page was present
    if (!present) {
        // Demand paging - page not mapped yet
        if (vma->flags & VM_ANON) {
            return handle_anonymous_fault(fault_addr, vma);
        } else if (vma->flags & VM_FILE) {
            // TODO: File-backed page fault
            printk(KERN_ERR "MM: File-backed fault not implemented\n");
            return -EFAULT;
        }
    }
    
    return -EFAULT;
}

// ============================================================================
// MMAP IMPLEMENTATION
// ============================================================================

// Find free virtual address range
static u64 find_vma_gap(mm_struct_t *mm, u64 len) {
    u64 addr = mm->mmap_base;
    vm_area_t *vma = mm->mmap;
    
    while (vma) {
        if (addr + len <= vma->start) {
            return addr;
        }
        addr = vma->end;
        vma = vma->next;
    }
    
    return addr;
}

// Create new VMA
static vm_area_t *vm_area_alloc(mm_struct_t *mm) {
    vm_area_t *vma = kmalloc(sizeof(vm_area_t), GFP_KERNEL);
    if (!vma) return NULL;
    
    memset(vma, 0, sizeof(*vma));
    vma->mm = mm;
    
    return vma;
}

// Insert VMA into mm (sorted by address)
static void insert_vma(mm_struct_t *mm, vm_area_t *vma) {
    vm_area_t **pp = &mm->mmap;
    vm_area_t *prev = NULL;
    
    while (*pp && (*pp)->start < vma->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    
    vma->next = *pp;
    vma->prev = prev;
    if (*pp) (*pp)->prev = vma;
    *pp = vma;
    
    mm->map_count++;
}

// mmap implementation
void *do_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    task_struct_t *current = get_current_task();
    if (!current || !current->mm) return (void *)-EFAULT;
    
    mm_struct_t *mm = current->mm;
    
    // Align length
    len = ALIGN(len, PAGE_SIZE);
    if (len == 0) return (void *)-EINVAL;
    
    // Determine address
    u64 start;
    if (flags & MAP_FIXED) {
        start = (u64)addr;
        if (start & (PAGE_SIZE - 1)) return (void *)-EINVAL;
    } else {
        start = find_vma_gap(mm, len);
    }
    
    // Create VMA
    vm_area_t *vma = vm_area_alloc(mm);
    if (!vma) return (void *)-ENOMEM;
    
    vma->start = start;
    vma->end = start + len;
    
    // Set flags
    if (prot & PROT_READ) vma->flags |= VM_READ;
    if (prot & PROT_WRITE) vma->flags |= VM_WRITE;
    if (prot & PROT_EXEC) vma->flags |= VM_EXEC;
    if (flags & MAP_SHARED) vma->flags |= VM_SHARED;
    
    if (flags & MAP_ANONYMOUS) {
        vma->flags |= VM_ANON;
    } else {
        vma->flags |= VM_FILE;
        // TODO: Set up file mapping
        (void)fd;
        (void)offset;
    }
    
    // Insert into mm
    spin_lock(&mm->mmap_lock);
    insert_vma(mm, vma);
    mm->total_vm += len >> PAGE_SHIFT;
    spin_unlock(&mm->mmap_lock);
    
    return (void *)start;
}

// munmap implementation
int do_munmap(void *addr, size_t len) {
    task_struct_t *current = get_current_task();
    if (!current || !current->mm) return -EFAULT;
    
    mm_struct_t *mm = current->mm;
    u64 start = (u64)addr;
    u64 end = start + ALIGN(len, PAGE_SIZE);
    
    spin_lock(&mm->mmap_lock);
    
    // Find and remove VMAs in range
    vm_area_t **pp = &mm->mmap;
    while (*pp) {
        vm_area_t *vma = *pp;
        
        if (vma->start >= end) break;
        
        if (vma->end <= start) {
            pp = &vma->next;
            continue;
        }
        
        // VMA overlaps with range
        // TODO: Handle partial unmapping
        
        *pp = vma->next;
        if (vma->next) vma->next->prev = vma->prev;
        
        mm->total_vm -= (vma->end - vma->start) >> PAGE_SHIFT;
        mm->map_count--;
        
        // Unmap pages
        // TODO: Actually unmap and free pages
        
        kfree(vma);
    }
    
    spin_unlock(&mm->mmap_lock);
    
    return 0;
}

// ============================================================================
// LRU AND PAGE RECLAIM
// ============================================================================

// Move page to active list
void lru_cache_activate(page_t *page) {
    zone_t *zone = page_zone(page);
    
    spin_lock(&zone->lock);
    
    if (page->flags & (1 << PG_lru)) {
        if (!(page->flags & (1 << PG_active))) {
            list_del(&page->lru);
            zone->nr_inactive--;
            list_add(&page->lru, &zone->lru_active);
            zone->nr_active++;
            page->flags |= (1 << PG_active);
        }
    }
    
    page->flags |= (1 << PG_referenced);
    
    spin_unlock(&zone->lock);
}

// Add page to inactive list
void lru_cache_add(page_t *page) {
    zone_t *zone = page_zone(page);
    
    spin_lock(&zone->lock);
    
    if (!(page->flags & (1 << PG_lru))) {
        list_add_tail(&page->lru, &zone->lru_inactive);
        zone->nr_inactive++;
        page->flags |= (1 << PG_lru);
    }
    
    spin_unlock(&zone->lock);
}

// Simple page reclaim (shrink inactive list)
int shrink_zone(zone_t *zone, int nr_to_reclaim) {
    int reclaimed = 0;
    
    spin_lock(&zone->lock);
    
    while (reclaimed < nr_to_reclaim && !list_empty(&zone->lru_inactive)) {
        page_t *page = list_last_entry(&zone->lru_inactive, page_t, lru);
        
        // Skip if referenced recently
        if (page->flags & (1 << PG_referenced)) {
            page->flags &= ~(1 << PG_referenced);
            list_move(&page->lru, &zone->lru_inactive);
            continue;
        }
        
        // Skip if locked or dirty
        if (PageLocked(page) || PageDirty(page)) {
            continue;
        }
        
        // Remove from LRU
        list_del(&page->lru);
        zone->nr_inactive--;
        page->flags &= ~(1 << PG_lru);
        
        spin_unlock(&zone->lock);
        
        // Free the page
        __free_page(page);
        reclaimed++;
        
        spin_lock(&zone->lock);
    }
    
    spin_unlock(&zone->lock);
    
    return reclaimed;
}

// ============================================================================
// MEMORY STATISTICS
// ============================================================================

void mm_show_stats(void) {
    printk(KERN_INFO "\n=== Memory Statistics ===\n");
    printk(KERN_INFO "Total pages: %lu (%lu MB)\n", 
           total_pages, (total_pages * PAGE_SIZE) / (1024 * 1024));
    printk(KERN_INFO "Free pages:  %lu (%lu MB)\n",
           free_pages, (free_pages * PAGE_SIZE) / (1024 * 1024));
    
    for (int i = 0; i < NR_ZONES; i++) {
        zone_t *zone = &zones[i];
        if (zone->present_pages == 0) continue;
        
        printk(KERN_INFO "Zone %s: %lu pages, %lu free\n",
               zone->name, zone->present_pages, zone->pages_free);
        
        for (int order = 0; order < MAX_ORDER; order++) {
            if (zone->free_area[order].nr_free > 0) {
                printk(KERN_INFO "  order %d: %lu free (%lu KB each)\n",
                       order, zone->free_area[order].nr_free,
                       (1UL << order) * 4);
            }
        }
    }
    printk(KERN_INFO "=========================\n\n");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void mm_zone_init(phys_addr_t start, phys_addr_t end) {
    // Initialize zones
    zones[ZONE_DMA].name = "DMA";
    zones[ZONE_DMA].type = ZONE_DMA;
    zones[ZONE_DMA32].name = "DMA32";
    zones[ZONE_DMA32].type = ZONE_DMA32;
    zones[ZONE_NORMAL].name = "Normal";
    zones[ZONE_NORMAL].type = ZONE_NORMAL;
    
    for (int i = 0; i < NR_ZONES; i++) {
        zone_t *zone = &zones[i];
        spin_lock_init(&zone->lock);
        INIT_LIST_HEAD(&zone->lru_active);
        INIT_LIST_HEAD(&zone->lru_inactive);
        
        for (int j = 0; j < MAX_ORDER; j++) {
            INIT_LIST_HEAD(&zone->free_area[j].free_list);
            zone->free_area[j].nr_free = 0;
        }
    }
    
    // Set zone ranges
    zones[ZONE_DMA].start_pfn = 0;
    zones[ZONE_DMA].end_pfn = ZONE_DMA_END >> PAGE_SHIFT;
    
    zones[ZONE_DMA32].start_pfn = ZONE_DMA_END >> PAGE_SHIFT;
    zones[ZONE_DMA32].end_pfn = ZONE_DMA32_END >> PAGE_SHIFT;
    
    zones[ZONE_NORMAL].start_pfn = ZONE_DMA32_END >> PAGE_SHIFT;
    zones[ZONE_NORMAL].end_pfn = end >> PAGE_SHIFT;
    
    // Calculate actual available pages per zone
    u64 start_pfn = start >> PAGE_SHIFT;
    u64 end_pfn = end >> PAGE_SHIFT;
    
    for (int i = 0; i < NR_ZONES; i++) {
        zone_t *zone = &zones[i];
        
        u64 zone_start = zone->start_pfn;
        u64 zone_end = zone->end_pfn;
        
        if (zone_end <= start_pfn || zone_start >= end_pfn) {
            zone->present_pages = 0;
            continue;
        }
        
        if (zone_start < start_pfn) zone_start = start_pfn;
        if (zone_end > end_pfn) zone_end = end_pfn;
        
        zone->present_pages = zone_end - zone_start;
        zone->spanned_pages = zone->end_pfn - zone->start_pfn;
        zone->pages_free = zone->present_pages;
        
        total_pages += zone->present_pages;
        free_pages += zone->present_pages;
    }
    
    // Allocate page array (simplified - use static array or bootmem)
    // In a real kernel, this would use bootmem allocator
    static page_t page_array[1024 * 1024];  // Up to 4GB
    mem_map = page_array;
    max_pfn = end_pfn;
    min_low_pfn = start_pfn;
    
    // Initialize page structures
    for (u64 pfn = start_pfn; pfn < end_pfn; pfn++) {
        page_t *page = pfn_to_page(pfn);
        memset(page, 0, sizeof(*page));
        INIT_LIST_HEAD(&page->lru);
        set_page_count(page, 0);
    }
    
    // Add free pages to buddy allocator
    // For simplicity, add one page at a time (real kernel would add larger blocks)
    for (u64 pfn = start_pfn; pfn < end_pfn; pfn++) {
        page_t *page = pfn_to_page(pfn);
        zone_t *zone = page_zone(page);
        
        list_add(&page->lru, &zone->free_area[0].free_list);
        zone->free_area[0].nr_free++;
    }
    
    printk(KERN_INFO "MM: Zone allocator initialized\n");
    printk(KERN_INFO "  DMA zone:    %lu pages\n", zones[ZONE_DMA].present_pages);
    printk(KERN_INFO "  DMA32 zone:  %lu pages\n", zones[ZONE_DMA32].present_pages);
    printk(KERN_INFO "  Normal zone: %lu pages\n", zones[ZONE_NORMAL].present_pages);
}
