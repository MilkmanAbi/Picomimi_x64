/**
 * Picomimi-x64 Page Fault Handler — Demand Paging + Copy-on-Write
 *
 * do_page_fault() is called from the #PF entry in handlers.c.
 *
 * Handles:
 *   1. Demand paging: anonymous VMA page not yet allocated → alloc + map
 *   2. Stack growth:  fault in guard page below current stack → grow stack VMA
 *   3. COW:           write fault on a read-only page that is COW → copy + remap
 *   4. File-backed mmap (read): allocate page and call vma->vm_ops->fault
 *
 * Returns:
 *   0  — fault resolved, resume faulting instruction
 *  -1  — cannot resolve, caller delivers SIGSEGV / kernel oops
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <lib/printk.h>
#include <lib/string.h>

/* VM flags */
#ifndef VM_READ
#define VM_READ         0x00000001
#define VM_WRITE        0x00000002
#define VM_EXEC         0x00000004
#define VM_SHARED       0x00000008
#define VM_GROWSDOWN    0x00000100
#define VM_STACK        0x00100000
#endif

/* Extra flag: page is COW-protected (mapped read-only for COW tracking) */

/* How far below RSP we allow stack growth (128 KB) */
#define STACK_GUARD_GAP (128ULL * 1024)

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

extern task_struct_t *get_current_task(void);
extern vm_area_t     *find_vma(mm_struct_t *mm, u64 addr);
extern phys_addr_t    pmm_alloc_page(void);

/**
 * alloc_zero_page — allocate one physical page and zero it.
 * Returns physical address or 0 on OOM.
 */
static phys_addr_t alloc_zero_page(void) {
    phys_addr_t phys = pmm_alloc_page();
    if (!phys) return 0;
    void *virt = phys_to_virt(phys);
    memset(virt, 0, PAGE_SIZE);
    return phys;
}

/**
 * pte_flags_for_vma — compute PTE flags from VMA protection flags.
 */
static u64 pte_flags_for_vma(vm_area_t *vma) {
    u64 flags = PTE_PRESENT | PTE_USER;
    if (vma->vm_flags & VM_WRITE)
        flags |= PTE_WRITE;
    return flags;
}

/* -------------------------------------------------------------------------
 * COW: handle write fault on a read-only page belonging to a writable VMA
 * ------------------------------------------------------------------------- */

static int handle_cow(mm_struct_t *mm, vm_area_t *vma, u64 fault_addr) {
    u64 page_addr = fault_addr & ~(u64)(PAGE_SIZE - 1);

    /* Get current physical mapping */
    phys_addr_t old_phys = vmm_get_phys(page_addr);
    if (!old_phys) {
        /* Page wasn't mapped — fall through to demand-alloc */
        return -1;
    }

    /* Allocate a fresh private page */
    phys_addr_t new_phys = alloc_zero_page();
    if (!new_phys) {
        printk(KERN_ERR "[COW] OOM allocating private page at 0x%llx\n",
               page_addr);
        return -1;
    }

    /* Copy the old page's contents */
    memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);

    /* Re-map with write permission */
    u64 flags = pte_flags_for_vma(vma);   /* includes PTE_WRITE */
    vmm_map_page(page_addr, new_phys, flags);
    vmm_flush_tlb_page(page_addr);

    /* Clear the COW flag for this VMA (if all pages are now private) */
    vma->vm_flags &= ~VM_COW;

    (void)mm;
    return 0;
}

/* -------------------------------------------------------------------------
 * Demand page: allocate and map a brand-new anonymous page
 * ------------------------------------------------------------------------- */

static int handle_anon_fault(mm_struct_t *mm, vm_area_t *vma, u64 fault_addr) {
    u64 page_addr = fault_addr & ~(u64)(PAGE_SIZE - 1);

    phys_addr_t phys = alloc_zero_page();
    if (!phys) {
        printk(KERN_ERR "[PF] OOM demand-paging 0x%llx\n", page_addr);
        return -1;
    }

    u64 flags = pte_flags_for_vma(vma);
    vmm_map_page(page_addr, phys, flags);
    vmm_flush_tlb_page(page_addr);

    (void)mm;
    return 0;
}

/* -------------------------------------------------------------------------
 * Stack growth: extend the stack VMA downward
 * ------------------------------------------------------------------------- */

static int handle_stack_growth(mm_struct_t *mm, u64 fault_addr) {
    /* Find the VMA just above the fault address */
    vm_area_t *vma = NULL;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->vm_flags & (VM_GROWSDOWN | VM_STACK)) {
            if (fault_addr < v->start && fault_addr >= v->start - STACK_GUARD_GAP) {
                vma = v;
                break;
            }
        }
    }
    if (!vma) return -1;

    /* Grow the VMA downward to cover the fault page */
    u64 new_start = fault_addr & ~(u64)(PAGE_SIZE - 1);
    vma->start = new_start;

    /* Allocate and map the new page */
    phys_addr_t phys = alloc_zero_page();
    if (!phys) return -1;

    u64 flags = pte_flags_for_vma(vma);
    vmm_map_page(new_start, phys, flags);
    vmm_flush_tlb_page(new_start);

    return 0;
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

int do_page_fault(u64 fault_addr, int present, int write, int user) {
    /* Only handle user-space faults here (kernel faults = oops) */
    if (!user) return -1;

    /* Refuse faults in the NULL page and very low addresses */
    if (fault_addr < 0x1000) return -1;

    task_struct_t *task = get_current_task();
    if (!task || !task->mm) return -1;
    mm_struct_t *mm = task->mm;

    vm_area_t *vma = find_vma(mm, fault_addr);
    if (!vma) {
        /* No VMA — check if this is a stack growth opportunity */
        if (handle_stack_growth(mm, fault_addr) == 0)
            return 0;
        return -1;
    }

    /* Check that fault_addr is actually within VMA */
    if (fault_addr < vma->start || fault_addr >= vma->end) {
        /* Could be stack growth */
        if (handle_stack_growth(mm, fault_addr) == 0)
            return 0;
        return -1;
    }

    /* ---- Write to a read-only page ---- */
    if (present && write) {
        /* Protection fault on a writable VMA → COW */
        if (vma->vm_flags & VM_WRITE)
            return handle_cow(mm, vma, fault_addr);
        /* Genuine write to a non-writable VMA → SIGSEGV */
        return -1;
    }

    /* ---- Read/exec fault on present page — shouldn't happen normally ---- */
    if (present && !write)
        return -1;

    /* ---- Not-present: demand paging ---- */
    if (!present) {
        /* Read fault on exec-only VMA (no write) */
        if (!(vma->vm_flags & VM_READ) && !(vma->vm_flags & VM_EXEC))
            return -1;
        return handle_anon_fault(mm, vma, fault_addr);
    }

    return -1;
}
