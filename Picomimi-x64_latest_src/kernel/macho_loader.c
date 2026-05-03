/**
 * Picomimi-x64 — Mach-O Loader
 * kernel/macho_loader.c
 *
 * Loads 64-bit Mach-O executables (MH_EXECUTE, MH_PIE).
 * Supports:
 *   - Thin x86_64 binaries  (magic 0xFEEDFACF)
 *   - Fat/universal binaries (magic 0xCAFEBABE) — extracts x86_64 slice
 *   - LC_SEGMENT_64  — segment mapping
 *   - LC_MAIN        — modern entry point
 *   - LC_UNIXTHREAD  — legacy entry point (rip from x86_thread_state64)
 *   - LC_LOAD_DYLINKER — records dyld path (future use)
 *
 * Re-uses the same physical page allocation / VMA infrastructure as
 * the ELF loader so nothing in mm/ needs to change.
 *
 * Called from exec.c:do_execve() after magic detection.
 * Returns 0 on success and fills *info; negative errno on failure.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/macho.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <lib/string.h>

/* mm helpers */
extern void *vmm_map_pages(u64 virt, u64 phys, u64 size, u32 flags);
extern u64   vmm_alloc_pages(u64 virt, u64 size, u32 flags);
extern void  vmm_free_pages(u64 virt, u64 size);
extern u64   vmm_user_virt_to_phys(u64 virt);
extern vm_area_t *vma_alloc(void);
extern void insert_vma(mm_struct_t *mm, vm_area_t *vma);

#define VMM_PROT_READ    0x01
#define VMM_PROT_WRITE   0x02
#define VMM_PROT_EXEC    0x04
#define VMM_PROT_USER    0x08

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096ULL
#endif
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(x)   (((u64)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#endif
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x,a) ((u64)(x) & ~((u64)(a)-1))
#endif

#define PHYS_KVIRT(p)   ((void *)((u64)(p) + 0xFFFFFFFF80000000ULL))

/* =========================================================
 * Probe helpers
 * ========================================================= */

/**
 * macho_probe_file - returns true if the file looks like a Mach-O or fat binary
 * Does NOT consume the file position permanently (resets to 0).
 */
bool macho_probe_file(file_t *file) {
    if (!file) return false;
    u32 magic = 0;
    u64 pos   = 0;
    s64 n = vfs_read(file, (char *)&magic, 4, &pos);
    if (n != 4) return false;
    return (magic == MH_MAGIC_64  || magic == MH_CIGAM_64 ||
            magic == FAT_MAGIC    || magic == FAT_CIGAM);
}

/* =========================================================
 * Fat binary: find the x86_64 slice offset+size
 * ========================================================= */

static int fat_find_x86_64(file_t *file, u64 *out_offset, u64 *out_size) {
    fat_header_t fh;
    u64 pos = 0;
    if (vfs_read(file, (char *)&fh, sizeof(fh), &pos) != (s64)sizeof(fh))
        return -ENOEXEC;

    /* fat header is big-endian */
    u32 narch = bswap32(fh.nfat_arch);
    if (narch == 0 || narch > 32) return -ENOEXEC;

    for (u32 i = 0; i < narch; i++) {
        fat_arch_t fa;
        if (vfs_read(file, (char *)&fa, sizeof(fa), &pos) != (s64)sizeof(fa))
            return -ENOEXEC;
        u32 cputype = bswap32(fa.cputype);
        if (cputype == CPU_TYPE_X86_64) {
            *out_offset = (u64)bswap32(fa.offset);
            *out_size   = (u64)bswap32(fa.size);
            return 0;
        }
    }
    return -ENOEXEC; /* no x86_64 slice */
}

/* =========================================================
 * Load a single LC_SEGMENT_64 into the new address space
 * ========================================================= */

static int macho_load_segment64(file_t *file,
                                 const segment_command_64_t *seg,
                                 u64 load_bias,
                                 u64 file_base,   /* offset of Mach-O start in file */
                                 mm_struct_t *mm)
{
    /* Skip __PAGEZERO — it's a zero-size guard mapping at vaddr 0 */
    if (seg->vmsize == 0) return 0;
    if (strncmp(seg->segname, "__PAGEZERO", MACHO_SEG_NAMELEN) == 0) return 0;

    u64 vaddr   = seg->vmaddr + load_bias;
    u64 memsz   = seg->vmsize;
    u64 filesz  = seg->filesize;
    u64 fileoff = file_base + seg->fileoff;

    u64 map_start = ALIGN_DOWN(vaddr, PAGE_SIZE);
    u64 map_end   = PAGE_ALIGN(vaddr + memsz);
    u64 map_size  = map_end - map_start;

    u32 prot = VMM_PROT_USER;
    if (seg->initprot & VM_PROT_READ)    prot |= VMM_PROT_READ;
    if (seg->initprot & VM_PROT_WRITE)   prot |= VMM_PROT_WRITE;
    if (seg->initprot & VM_PROT_EXECUTE) prot |= VMM_PROT_EXEC;

    /* Ensure at least readable so we can copy data in */
    u32 alloc_prot = prot | VMM_PROT_READ | VMM_PROT_WRITE;

    u64 result = vmm_alloc_pages(map_start, map_size, alloc_prot);
    if (!result) {
        printk(KERN_ERR "[macho] failed to alloc segment %.16s at 0x%llx\n",
               seg->segname, map_start);
        return -ENOMEM;
    }

    /* Zero via phys alias */
    for (u64 pg = map_start; pg < map_end; pg += PAGE_SIZE) {
        u64 phys = vmm_user_virt_to_phys(pg);
        if (phys) memset(PHYS_KVIRT(phys), 0, PAGE_SIZE);
    }

    /* Copy file data */
    if (filesz > 0) {
        u64 dst  = vaddr;
        u64 rem  = filesz;
        u64 fpos = fileoff;

        while (rem > 0) {
            u64 pg_base  = dst & ~(PAGE_SIZE - 1);
            u64 pg_off   = dst - pg_base;
            u64 pg_space = PAGE_SIZE - pg_off;
            u64 chunk    = rem < pg_space ? rem : pg_space;

            u64 phys = vmm_user_virt_to_phys(pg_base);
            if (!phys) {
                printk(KERN_ERR "[macho] no phys for 0x%llx\n", pg_base);
                return -EFAULT;
            }
            void *dest = (char *)PHYS_KVIRT(phys) + pg_off;
            s64 n = vfs_read(file, dest, (size_t)chunk, &fpos);
            if (n <= 0) {
                printk(KERN_ERR "[macho] read error at 0x%llx\n", dst);
                return n < 0 ? (int)n : -EIO;
            }
            dst += (u64)n;
            rem -= (u64)n;
        }
    }

    /* Register VMA */
    if (mm) {
        vm_area_t *vma = vma_alloc();
        if (vma) {
            vma->start    = map_start;
            vma->end      = map_end;
            vma->flags    = 0;
            if (seg->initprot & VM_PROT_READ)    vma->flags |= VM_READ;
            if (seg->initprot & VM_PROT_WRITE)   vma->flags |= VM_WRITE;
            if (seg->initprot & VM_PROT_EXECUTE) vma->flags |= VM_EXEC;
            vma->vm_flags = 0;
            vma->file     = NULL;
            vma->offset   = 0;
            insert_vma(mm, vma);
            mm->total_vm += map_size / PAGE_SIZE;
        }
    }

    return 0;
}

/* =========================================================
 * Main loader: parse load commands, map segments, find entry
 * ========================================================= */

int macho_load(file_t *file, u64 file_base, mm_struct_t *mm,
               macho_load_info_t *info)
{
    int ret = -ENOEXEC;
    memset(info, 0, sizeof(*info));

    /* Read Mach-O header */
    mach_header_64_t mh;
    u64 pos = file_base;
    if (vfs_read(file, (char *)&mh, sizeof(mh), &pos) != (s64)sizeof(mh))
        return -ENOEXEC;

    /* Validate */
    if (mh.magic != MH_MAGIC_64)          return -ENOEXEC;
    if (mh.cputype != CPU_TYPE_X86_64)    return -ENOEXEC;
    if (mh.filetype != MH_EXECUTE)        return -ENOEXEC;
    if (mh.ncmds == 0 || mh.ncmds > 512) return -ENOEXEC;

    info->is_pie = (mh.flags & MH_PIE) != 0;

    /*
     * ASLR slide: for PIE binaries we apply a fixed slide so the binary
     * doesn't land at vaddr 0 (where __PAGEZERO lives).
     * Use 0x100000000 (4GB) as the canonical Mach-O PIE base —
     * this matches what macOS typically does and avoids collision with
     * the ELF region (0x400000) used by Linux binaries.
     */
    u64 load_bias = info->is_pie ? 0x100000000ULL : 0ULL;
    info->load_bias = load_bias;

    /* Read all load commands in one shot */
    u8 *lcbuf = kmalloc(mh.sizeofcmds, GFP_KERNEL);
    if (!lcbuf) return -ENOMEM;

    pos = file_base + sizeof(mach_header_64_t);
    if (vfs_read(file, (char *)lcbuf, mh.sizeofcmds, &pos)
            != (s64)mh.sizeofcmds) {
        kfree(lcbuf);
        return -ENOEXEC;
    }

    u64  entry       = 0;
    bool have_entry  = false;
    u64  seg_end_max = 0;
    info->mh_vaddr   = load_bias; /* __TEXT vmaddr is typically 0x100000000 for PIE */

    u8  *lcp     = lcbuf;
    u8  *lc_end  = lcbuf + mh.sizeofcmds;

    for (u32 i = 0; i < mh.ncmds && lcp + sizeof(load_command_t) <= lc_end; i++) {
        load_command_t *lc = (load_command_t *)lcp;

        if (lc->cmdsize == 0 || lcp + lc->cmdsize > lc_end) break;

        switch (lc->cmd) {

        /* ---- LC_SEGMENT_64 ---- */
        case LC_SEGMENT_64: {
            if (lc->cmdsize < sizeof(segment_command_64_t)) break;
            segment_command_64_t *seg = (segment_command_64_t *)lcp;

            /* Track mach_header virtual address from __TEXT */
            if (strncmp(seg->segname, "__TEXT", MACHO_SEG_NAMELEN) == 0)
                info->mh_vaddr = seg->vmaddr + load_bias;

            ret = macho_load_segment64(file, seg, load_bias, file_base, mm);
            if (ret < 0) {
                printk(KERN_ERR "[macho] segment load failed: %d\n", ret);
                goto out;
            }

            /* Track highest mapped address for brk */
            u64 seg_hi = seg->vmaddr + seg->vmsize + load_bias;
            if (seg_hi > seg_end_max) seg_end_max = seg_hi;
            break;
        }

        /* ---- LC_MAIN (modern) ---- */
        case LC_MAIN: {
            if (lc->cmdsize < sizeof(entry_point_command_t)) break;
            entry_point_command_t *ep = (entry_point_command_t *)lcp;
            /*
             * entryoff is a file offset relative to Mach-O header start.
             * Convert to virtual address: find __TEXT vmaddr + entryoff.
             * For now we use load_bias + entryoff which is correct when
             * __TEXT.vmaddr == load_bias (non-PIE) or when the binary has
             * a standard __TEXT base (PIE at 0x100000000).
             * A second pass will fix this up using __TEXT.vmaddr if needed.
             */
            entry = load_bias + ep->entryoff;
            info->stack_size = ep->stacksize;
            have_entry = true;
            break;
        }

        /* ---- LC_UNIXTHREAD (legacy) ---- */
        case LC_UNIXTHREAD: {
            if (lc->cmdsize < sizeof(thread_command_t)) break;
            thread_command_t *tc = (thread_command_t *)lcp;
            if (tc->flavor == x86_THREAD_STATE64) {
                entry = tc->state.rip + load_bias;
                have_entry = true;
            }
            break;
        }

        /* ---- LC_LOAD_DYLINKER ---- */
        case LC_LOAD_DYLINKER: {
            if (lc->cmdsize < sizeof(dylinker_command_t)) break;
            dylinker_command_t *dl = (dylinker_command_t *)lcp;
            u32 off = dl->name_offset;
            if (off < lc->cmdsize) {
                const char *path = (const char *)lcp + off;
                size_t plen = lc->cmdsize - off;
                if (plen > 255) plen = 255;
                memcpy(info->dylinker, path, plen);
                info->dylinker[plen] = 0;
                info->has_dylinker = true;
            }
            break;
        }

        /* Everything else: recognised and ignored */
        default:
            break;
        }

        lcp += lc->cmdsize;
    }

    kfree(lcbuf);

    if (!have_entry) {
        printk(KERN_ERR "[macho] no entry point found\n");
        return -ENOEXEC;
    }
    if (seg_end_max == 0) {
        printk(KERN_ERR "[macho] no segments loaded\n");
        return -ENOEXEC;
    }

    info->entry   = entry;
    info->seg_end = seg_end_max;

    printk(KERN_INFO "[macho] loaded: entry=0x%llx bias=0x%llx pie=%d dylinker=%s\n",
           entry, load_bias, info->is_pie,
           info->has_dylinker ? info->dylinker : "(none)");

    return 0;

out:
    kfree(lcbuf);
    return ret;
}

/* =========================================================
 * Top-level entry: handle fat or thin, called from do_execve
 * ========================================================= */

int macho_load_file(file_t *file, mm_struct_t *mm, macho_load_info_t *info)
{
    /* Read magic */
    u32 magic = 0;
    u64 pos   = 0;
    if (vfs_read(file, (char *)&magic, 4, &pos) != 4) return -ENOEXEC;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        /* Fat binary — find x86_64 slice */
        u64 slice_off = 0, slice_sz = 0;
        int r = fat_find_x86_64(file, &slice_off, &slice_sz);
        if (r < 0) {
            printk(KERN_ERR "[macho] fat binary has no x86_64 slice\n");
            return r;
        }
        printk(KERN_INFO "[macho] fat binary: x86_64 slice at 0x%llx size 0x%llx\n",
               slice_off, slice_sz);
        return macho_load(file, slice_off, mm, info);
    }

    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        return macho_load(file, 0, mm, info);
    }

    return -ENOEXEC;
}
