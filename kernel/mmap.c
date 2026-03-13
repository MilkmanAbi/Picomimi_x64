/**
 * Picomimi-x64 brk / mmap / munmap / mprotect
 *
 * Provides the full userspace memory allocation interface:
 *
 *   brk / sbrk  — heap extension via program break
 *   mmap        — anonymous and file-backed mappings
 *   munmap      — unmap regions
 *   mprotect    — change protection flags
 *
 * All mappings are tracked in the process mm_struct's VMA list.
 * Anonymous pages are allocated on demand via pmm_alloc_page().
 * File-backed pages load from vfs_read() at fault time (demand paging stub).
 *
 * mmap address hint selection:
 *   addr == NULL → allocate from mmap_base (grows down from 0x7FFF_0000_0000)
 *   addr != NULL → try that address, fall back to hint search
 *
 * Protection flags are translated to VMM pte flags and vmm_map_page() called.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <arch/cpu.h>

extern void vmm_map_page(u64 virt, u64 phys, u64 flags);

/* =========================================================
 * VMA helpers
 * ========================================================= */

/* Find a free address range of `size` bytes starting above `hint`. */
static u64 find_free_vma(mm_struct_t *mm, u64 hint, u64 size) {
    if (!mm) return 0;

    /* Default hint: below stack, grows down */
    if (hint == 0) hint = 0x00007FFF00000000ULL - size;
    hint = ALIGN_DOWN(hint, PAGE_SIZE);

    /* Simple linear scan: find gap */
    for (int tries = 0; tries < 512; tries++) {
        bool collision = false;

        vm_area_t *vma;
        for (vma = mm->mmap; vma; vma = vma->next) {
            if (hint < vma->end && hint + size > vma->start) {
                hint = PAGE_ALIGN(vma->end);
                collision = true;
                break;
            }
        }

        if (!collision) return hint;
    }

    return 0;  /* No free range found */
}

/* Allocate and map `size` bytes of anonymous pages at `vaddr`. */
static int mmap_anon_pages(mm_struct_t *mm, u64 vaddr, u64 size, u32 prot,
                            u32 vma_flags) {
    (void)mm;

    u64 pte_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE)  pte_flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) pte_flags |= PTE_NX;

    /* Zero-fill the mapping */
    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;

        /* Zero the page through the kernel mapping */
        memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);

        vmm_map_page(vaddr + off, phys, pte_flags);
    }

    return 0;
}

/* =========================================================
 * brk — set program break (heap end)
 * ========================================================= */

s64 sys_brk(void *addr_ptr) {
    u64 addr = (u64)(uintptr_t)addr_ptr;

    task_struct_t *t = current;
    if (!t || !t->mm) {
        /* Kernel context: return a dummy heap area */
        return 0x400000;
    }

    mm_struct_t *mm = t->mm;

    /* addr == 0: just return current brk */
    if (addr == 0) return (s64)mm->brk;

    /* Validate: must not go below start_brk, must stay below stack */
    if (addr < mm->start_brk) return (s64)mm->brk;
    if (addr > mm->start_stack - (128 * 1024)) return (s64)mm->brk; /* safety margin */

    u64 old_brk = mm->brk;
    u64 new_brk  = PAGE_ALIGN(addr);

    if (new_brk == old_brk) {
        mm->brk = addr;  /* Allow unaligned brk within current page */
        return (s64)addr;
    }

    if (new_brk > old_brk) {
        /* Grow: allocate new pages */
        u64 grow_start = PAGE_ALIGN(old_brk);
        u64 grow_size  = new_brk - grow_start;

        int r = mmap_anon_pages(mm, grow_start, grow_size,
                                 PROT_READ | PROT_WRITE, VM_READ | VM_WRITE);
        if (r < 0) return (s64)mm->brk;

        /* Extend or create heap VMA */
        bool extended = false;
        vm_area_t *vma;
        for (vma = mm->mmap; vma; vma = vma->next) {
            if (vma->flags & VM_HEAP && vma->end == grow_start) {
                vma->end = new_brk;
                extended = true;
                break;
            }
        }

        if (!extended) {
            vm_area_t *vma = vma_alloc();
            if (vma) {
                vma->start    = grow_start;
                vma->end      = new_brk;
                vma->flags    = VM_READ | VM_WRITE | VM_HEAP;
                vma->vm_flags = VM_HEAP;
                insert_vma(mm, vma);
                mm->total_vm += grow_size / PAGE_SIZE;
            }
        }
    } else {
        /* Shrink: free pages */
        u64 shrink_start = PAGE_ALIGN(addr);
        u64 shrink_end   = old_brk;

        /* Unmap pages — VMM will reclaim physical pages */
        for (u64 pg = shrink_start; pg < shrink_end; pg += PAGE_SIZE) {
            extern void vmm_unmap_page(u64 virt);
            vmm_unmap_page(pg);
        }

        /* Trim VMA */
        vm_area_t *vma;
        for (vma = mm->mmap; vma; vma = vma->next) {
            if (vma->flags & VM_HEAP && vma->end > shrink_start) {
                vma->end = shrink_start;
                if (vma->start >= vma->end) {
                                        vma_free(vma);
                }
                break;
            }
        }
    }

    mm->brk = addr;
    return (s64)addr;
}

/* =========================================================
 * mmap
 * ========================================================= */

/* mmap prot flags */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_GROWSDOWN   0x0100
#ifndef MAP_POPULATE
#define MAP_POPULATE    0x08000
#endif
#define MAP_NONBLOCK    0x10000
#define MAP_STACK       0x20000
#define MAP_HUGETLB     0x40000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED  ((void *)-1)

s64 sys_mmap(void *addr_hint, size_t length, int prot, int flags,
              int fd, s64 offset) {
    if (length == 0) return -EINVAL;
    if (offset & (PAGE_SIZE - 1)) return -EINVAL;

    length = PAGE_ALIGN(length);

    task_struct_t *t = current;
    if (!t || !t->mm) {
        /* Kernel context: simple bump allocator */
        static u64 kmap_ptr = 0xFFFF800100000000ULL;
        u64 ret = kmap_ptr;
        kmap_ptr += length;
        mmap_anon_pages(NULL, ret, length, (u32)prot, 0);
        return (s64)ret;
    }

    mm_struct_t *mm = t->mm;

    /* Determine virtual address */
    u64 vaddr;
    if (flags & MAP_FIXED) {
        vaddr = (u64)(uintptr_t)addr_hint;
        if (vaddr & (PAGE_SIZE - 1)) return -EINVAL;
    } else {
        vaddr = find_free_vma(mm, (u64)(uintptr_t)addr_hint, length);
        if (!vaddr) return (s64)-ENOMEM;
    }

    /* VMA flags */
    u32 vma_flags = 0;
    if (prot & PROT_READ)  vma_flags |= VM_READ;
    if (prot & PROT_WRITE) vma_flags |= VM_WRITE;
    if (prot & PROT_EXEC)  vma_flags |= VM_EXEC;
    if (flags & MAP_SHARED) vma_flags |= VM_SHARED;
    if (flags & MAP_GROWSDOWN) vma_flags |= VM_GROWSDOWN;

    int r = 0;

    if (flags & MAP_ANONYMOUS) {
        /* Anonymous mapping */
        r = mmap_anon_pages(mm, vaddr, length, (u32)prot, vma_flags);
    } else {
        /* File-backed mapping */
        file_t *file = fget((unsigned int)fd);
        if (!file) return -EBADF;

        /* Check if this file supports physical passthrough (e.g. /dev/fb0).
         * We query via FBIO_GET_PHYSADDR ioctl; if it returns a non-zero
         * physical address, map the device memory directly. */
        u64 phys_base = 0;
        if (file->f_op && file->f_op->ioctl) {
            file->f_op->ioctl(file, 0x4690 /* FBIO_GET_PHYSADDR */, (unsigned long)&phys_base);
        }

        if (phys_base) {
            /* Physical device mapping — map fb pages directly */
            u64 pte_flags = PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NX;
            /* Write-combining would be ideal but just use WB for now */
            for (u64 off = 0; off < length; off += PAGE_SIZE) {
                vmm_map_page(vaddr + off, phys_base + (u64)offset + off, pte_flags);
            }
            fput(file);
        } else {
            /* Allocate pages and populate from file */
            for (u64 off = 0; off < length; off += PAGE_SIZE) {
                u64 phys = pmm_alloc_page();
                if (!phys) { fput(file); return -ENOMEM; }

                void *page = (void *)phys_to_virt(phys);
                memset(page, 0, PAGE_SIZE);

                u64 file_off = (u64)offset + off;
                s64 nr = vfs_read(file, (char *)page, PAGE_SIZE, &file_off);
                (void)nr;

                u64 pte_flags = PTE_PRESENT | PTE_USER;
                if (prot & PROT_WRITE)  pte_flags |= PTE_WRITE;
                if (!(prot & PROT_EXEC)) pte_flags |= PTE_NX;
                vmm_map_page(vaddr + off, phys, pte_flags);
            }

            /* Keep a reference for shared mappings */
            if (!(flags & MAP_PRIVATE)) {
                /* shared: writes go back to file — TODO: write-back tracking */
            } else {
                fput(file);
            }
        }
        vma_flags |= VM_FILE;
    }

    if (r < 0) return (s64)r;

    /* Register VMA */
    vm_area_t *vma = vma_alloc();
    if (vma) {
        vma->start    = vaddr;
        vma->end      = vaddr + length;
        vma->flags    = vma_flags;
        vma->vm_flags = vma_flags;
        vma->pgoff    = (u64)offset >> PAGE_SHIFT;
        insert_vma(mm, vma);
        mm->total_vm += length / PAGE_SIZE;
    }

    return (s64)vaddr;
}

/* =========================================================
 * munmap
 * ========================================================= */

s64 sys_munmap(void *addr, size_t length) {
    if ((u64)(uintptr_t)addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (length == 0) return -EINVAL;

    length = PAGE_ALIGN(length);
    u64 start = (u64)(uintptr_t)addr;
    u64 end   = start + length;

    task_struct_t *t = current;
    if (!t || !t->mm) return 0;

    mm_struct_t *mm = t->mm;

    /* Unmap physical pages */
    extern void vmm_unmap_page(u64 virt);
    for (u64 pg = start; pg < end; pg += PAGE_SIZE)
        vmm_unmap_page(pg);

    /* Remove or trim VMAs in the range */
    vm_area_t *vma, *_next_vma;
    for (vma = mm->mmap; vma; vma = _next_vma) {
        _next_vma = vma->next;
        if (vma->end <= start || vma->start >= end) continue;

        if (vma->start >= start && vma->end <= end) {
            /* Fully covered: remove */
                        mm->total_vm -= (vma->end - vma->start) / PAGE_SIZE;
            vma_free(vma);
        } else if (vma->start < start && vma->end > end) {
            /* Punch a hole: split into two VMAs */
            vm_area_t *tail = vma_alloc();
            if (tail) {
                tail->start    = end;
                tail->end      = vma->end;
                tail->flags    = vma->flags;
                tail->vm_flags = vma->vm_flags;
                insert_vma(mm, tail);
            }
            vma->end = start;
            mm->total_vm -= length / PAGE_SIZE;
        } else if (vma->start < start) {
            /* Trim tail */
            mm->total_vm -= (vma->end - start) / PAGE_SIZE;
            vma->end = start;
        } else {
            /* Trim head */
            mm->total_vm -= (end - vma->start) / PAGE_SIZE;
            vma->start = end;
        }
    }

    return 0;
}

/* =========================================================
 * mprotect — change protection flags on a VMA range
 * ========================================================= */

s64 sys_mprotect(void *addr, size_t len, int prot) {
    if ((u64)(uintptr_t)addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (len == 0) return 0;

    len = PAGE_ALIGN(len);
    u64 start = (u64)(uintptr_t)addr;
    u64 end   = start + len;

    task_struct_t *t = current;
    if (!t || !t->mm) return 0;

    /* Remap pages with new protection */
    u64 pte_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE)   pte_flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC))  pte_flags |= PTE_NX;

    for (u64 pg = start; pg < end; pg += PAGE_SIZE) {
        /* Lookup physical address for this virtual page */
        extern u64 vmm_user_virt_to_phys(u64 virt);
        u64 phys = vmm_user_virt_to_phys(pg);
        if (phys) vmm_map_page(pg, phys, pte_flags);
    }

    /* Update VMA flags */
    u32 new_vma_flags = 0;
    if (prot & PROT_READ)  new_vma_flags |= VM_READ;
    if (prot & PROT_WRITE) new_vma_flags |= VM_WRITE;
    if (prot & PROT_EXEC)  new_vma_flags |= VM_EXEC;

    mm_struct_t *mm = t->mm;
    vm_area_t *vma;
    for (vma = mm->mmap; vma; vma = vma->next) {
        if (vma->end <= start || vma->start >= end) continue;
        vma->flags = (vma->flags & ~(VM_READ | VM_WRITE | VM_EXEC)) | new_vma_flags;
    }

    return 0;
}

/* ============================================================
 * vmm_get_phys — walk page tables to get physical addr for VA.
 * Returns 0 if not mapped.
 * ============================================================ */
phys_addr_t vmm_get_phys(virt_addr_t virt) {
#define KERN_OFF 0xFFFFFFFF80000000ULL
    u64 *pml4 = (u64 *)(read_cr3() + KERN_OFF);
    int l4 = (int)((virt >> 39) & 0x1FF);
    int l3 = (int)((virt >> 30) & 0x1FF);
    int l2 = (int)((virt >> 21) & 0x1FF);
    int l1 = (int)((virt >> 12) & 0x1FF);

    if (!(pml4[l4] & PTE_PRESENT)) return 0;
    u64 *pdpt = (u64 *)((pml4[l4] & ~0xFFFULL) + KERN_OFF);
    if (!(pdpt[l3] & PTE_PRESENT)) return 0;
    if (pdpt[l3] & PTE_HUGE)
        return (pdpt[l3] & ~0x3FFFFFFFULL) | (virt & 0x3FFFFFFF);
    u64 *pd = (u64 *)((pdpt[l3] & ~0xFFFULL) + KERN_OFF);
    if (!(pd[l2] & PTE_PRESENT)) return 0;
    if (pd[l2] & PTE_HUGE)
        return (pd[l2] & ~0x1FFFFFULL) | (virt & 0x1FFFFF);
    u64 *pt = (u64 *)((pd[l2] & ~0xFFFULL) + KERN_OFF);
    if (!(pt[l1] & PTE_PRESENT)) return 0;
    return (pt[l1] & ~0xFFFULL) | (virt & 0xFFF);
#undef KERN_OFF
}

/* ============================================================
 * find_vma — find the VMA containing addr (or next VMA after it).
 * ============================================================ */
vm_area_t *find_vma(mm_struct_t *mm, u64 addr) {
    if (!mm) return NULL;
    vm_area_t *best = NULL;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (addr >= v->start && addr < v->end)
            return v;
        if (v->start > addr) {
            if (!best || v->start < best->start)
                best = v;
        }
    }
    return best;
}
