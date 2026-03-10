/**
 * Picomimi-x64 SMP Initialization
 *
 * Boots Application Processors (APs) from real mode to 64-bit long mode.
 *
 * Flow:
 *   BSP: lapic_init() → smp_init() → (per AP) INIT IPI → SIPI → SIPI
 *   AP:  trampoline (16-bit) → protected mode → long mode → ap_entry64 → ap_main()
 *
 * The trampoline is copied to physical 0x8000 (SIPI vector 0x08).
 * A small params block at the end of the trampoline page passes data BSP→AP.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/kernel.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include "trampoline_bin.h"
#include <arch/cpu.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define TRAMPOLINE_PHYS     0x8000UL   /* 32KB mark, SIPI vector = 0x08 */
#define TRAMPOLINE_VIRT     (TRAMPOLINE_PHYS + 0xFFFFFFFF80000000ULL)
#define TRAMPOLINE_PARAMS   (TRAMPOLINE_PHYS + 0xFF0)   /* last 16 bytes of page */
#define AP_STACK_SIZE       (16 * 1024)
#define AP_INIT_DELAY_US    10000   /* 10ms after INIT IPI */
#define AP_SIPI_DELAY_US    200     /* 200µs between SIPIs */
#define MAX_APS             15      /* support up to 16 CPUs total */

/* IA32_TSC_AUX MSR - we store CPU ID here so rdtscp/rdpid works */
#define MSR_IA32_TSC_AUX    0xC0000103UL

/* ------------------------------------------------------------------ */
/* AP boot parameter block (at end of trampoline page)                 */
/* ------------------------------------------------------------------ */

typedef struct __packed {
    u64  pml4_phys;         /* BSP's CR3 value */
    u64  ap_entry_virt;     /* virtual address of ap_entry64() */
    u64  gdt_ptr_virt;      /* virtual address of bsp_gdt_ptr64 */
    u64  idt_ptr_virt;      /* virtual address of bsp_idt_ptr64 */
    u64  ap_stacks[MAX_APS];/* per-AP stack tops (virtual) */
    volatile u32 ap_cpu_id[MAX_APS]; /* LAPIC ID each AP should assume */
    volatile u32 ap_online;  /* count of APs that finished boot */
    volatile u32 ap_go;      /* BSP sets to 1 to release APs from spin */
} ap_params_t;

/* ------------------------------------------------------------------ */
/* Exported GDT/IDT pointer structs (filled by lapic_init / idt_init)  */
/* ------------------------------------------------------------------ */

/* These are filled in at runtime and placed in .data so APs can find them */
typedef struct __packed { u16 limit; u64 base; } desc_ptr64_t;

desc_ptr64_t bsp_gdt_ptr64;
desc_ptr64_t bsp_idt_ptr64;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */
extern void sched_init_ap(u32 cpu);

/* ------------------------------------------------------------------ */
/* AP count and LAPIC ID table (populated from MADT)                   */
/* ------------------------------------------------------------------ */

static u32  ap_lapic_ids[MAX_APS];
static int  ap_count = 0;
static int  total_cpus = 1;   /* include BSP */

/* ------------------------------------------------------------------ */
/* udelay - spin-wait using PIT calibration (rough)                    */
/* ------------------------------------------------------------------ */

static void udelay(u32 us) {
    /* Rough calibration: on modern HW ~1 billion iterations/sec
     * Use a conservative 100 million/sec = 100 iterations/µs         */
    volatile u64 n = (u64)us * 100;
    while (n--) __asm__ volatile("pause");
}

/* ------------------------------------------------------------------ */
/* LAPIC                                                                */
/* ------------------------------------------------------------------ */

static volatile u32 *lapic_mmio = NULL;

static u32 lapic_reg_read(u32 reg) {
    return lapic_mmio[reg >> 2];
}
static void lapic_reg_write(u32 reg, u32 val) {
    lapic_mmio[reg >> 2] = val;
    (void)lapic_mmio[LAPIC_ID >> 2];   /* flush write */
}

/* Wait for ICR delivery to complete (PENDING bit clears) */
static void lapic_wait_icr(void) {
    u32 timeout = 100000;
    while ((lapic_reg_read(LAPIC_ICR_LO) & ICR_PENDING) && --timeout)
        __asm__ volatile("pause");
}

void lapic_write(u32 reg, u32 value) {
    if (lapic_mmio) lapic_reg_write(reg, value);
}
u32 lapic_read(u32 reg) {
    if (lapic_mmio) return lapic_reg_read(reg);
    return 0;
}
void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

u32 lapic_id(void) {
    if (lapic_mmio) return lapic_reg_read(LAPIC_ID) >> 24;
    /* Fallback: CPUID leaf 1 EBX[31:24] */
    u32 eax, ebx, ecx, edx;
    __asm__("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1));
    return (ebx >> 24) & 0xFF;
}

void lapic_send_ipi(u32 apic_id, u32 vector) {
    lapic_reg_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_reg_write(LAPIC_ICR_LO, ICR_NO_SHORTHAND | ICR_PHYSICAL |
                                   ICR_ASSERT | ICR_EDGE | vector);
    lapic_wait_icr();
}
void lapic_send_init(u32 apic_id) {
    /* Send INIT IPI: assert + level-triggered */
    lapic_reg_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_reg_write(LAPIC_ICR_LO, ICR_NO_SHORTHAND | ICR_PHYSICAL |
                                   ICR_ASSERT | ICR_LEVEL | ICR_INIT);
    lapic_wait_icr();
    udelay(AP_INIT_DELAY_US);
    /* De-assert: required by MP spec for level-triggered INIT */
    lapic_reg_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_reg_write(LAPIC_ICR_LO, ICR_NO_SHORTHAND | ICR_PHYSICAL |
                                   ICR_DEASSERT | ICR_LEVEL | ICR_INIT);
    /* Don't wait on de-assert — QEMU may not signal IDLE for it */
}
void lapic_send_sipi(u32 apic_id, u8 vector) {
    lapic_reg_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_reg_write(LAPIC_ICR_LO, ICR_NO_SHORTHAND | ICR_PHYSICAL |
                                   ICR_ASSERT | ICR_EDGE | ICR_STARTUP | vector);
    lapic_wait_icr();
}

void lapic_timer_init(u32 vector, u32 divide) {
    lapic_reg_write(LAPIC_TIMER_DCR, divide);
    lapic_reg_write(LAPIC_TIMER_LVT, LVT_TIMER_PERIODIC | vector);
    lapic_reg_write(LAPIC_TIMER_ICR, 0x100000);  /* initial count */
}
void lapic_timer_stop(void) {
    lapic_reg_write(LAPIC_TIMER_LVT, LVT_MASKED);
    lapic_reg_write(LAPIC_TIMER_ICR, 0);
}

/**
 * lapic_init - Map and enable BSP's Local APIC
 * Called from kernel_main() during BSP init.
 */
void lapic_init(void) {
    /* Map the LAPIC MMIO region (physical 0xFEE00000) */
    extern void *ioremap(u64 phys, u64 size);
    lapic_mmio = (volatile u32 *)ioremap(0xFEE00000UL, 4096);
    if (!lapic_mmio) {
        printk(KERN_ERR "  LAPIC: ioremap failed!\n");
        return;
    }

    /* Enable LAPIC via Spurious Interrupt Vector Register */
    u32 svr = lapic_reg_read(LAPIC_SVR);
    svr |= SVR_ENABLE | 0xFF;   /* enable + spurious vector = 0xFF */
    lapic_reg_write(LAPIC_SVR, svr);

    /* Mask all LVT entries we don't use yet */
    lapic_reg_write(LAPIC_TIMER_LVT,   LVT_MASKED);
    lapic_reg_write(LAPIC_THERMAL_LVT, LVT_MASKED);
    lapic_reg_write(LAPIC_PERF_LVT,    LVT_MASKED);
    lapic_reg_write(LAPIC_LINT0_LVT,   LVT_MASKED);
    lapic_reg_write(LAPIC_LINT1_LVT,   LVT_MASKED);
    lapic_reg_write(LAPIC_ERROR_LVT,   LVT_MASKED);

    /* Clear ESR */
    lapic_reg_write(LAPIC_ESR, 0);
    lapic_reg_read(LAPIC_ESR);

    /* Set Task Priority Register to 0 (accept all interrupts) */
    lapic_reg_write(LAPIC_TPR, 0);

    /* Issue EOI to clear any pending */
    lapic_reg_write(LAPIC_EOI, 0);

    /* Store BSP's CPU ID in IA32_TSC_AUX so smp_processor_id() works */
    wrmsr(MSR_IA32_TSC_AUX, 0);   /* BSP is always logical CPU 0 */

    u32 id = lapic_id();
    printk(KERN_INFO "  LAPIC: enabled, BSP APIC ID=%u mapped at %p\n",
           id, (void *)lapic_mmio);
}

/* ------------------------------------------------------------------ */
/* ACPI MADT parser                                                     */
/* ------------------------------------------------------------------ */

#define ACPI_SIG(a,b,c,d) ((u32)(a)|((u32)(b)<<8)|((u32)(c)<<16)|((u32)(d)<<24))

typedef struct __packed {
    u32  signature;
    u32  length;
    u8   revision;
    u8   checksum;
    u8   oem_id[6];
    u8   oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} acpi_sdt_hdr_t;

typedef struct __packed {
    acpi_sdt_hdr_t hdr;
    u32  entries[];   /* array of physical 32-bit pointers to SDTs */
} acpi_rsdt_t;

typedef struct __packed {
    acpi_sdt_hdr_t hdr;
    u64  entries[];
} acpi_xsdt_t;

typedef struct __packed {
    u8  signature[8];   /* "RSD PTR " */
    u8  checksum;
    u8  oem_id[6];
    u8  revision;
    u32 rsdt_phys;
    /* v2 fields: */
    u32 length;
    u64 xsdt_phys;
    u8  ext_checksum;
    u8  reserved[3];
} acpi_rsdp_t;

typedef struct __packed {
    acpi_sdt_hdr_t hdr;
    u32 lapic_phys;
    u32 flags;
} acpi_madt_t;

typedef struct __packed {
    u8  type;
    u8  length;
} madt_entry_hdr_t;

#define MADT_LAPIC      0   /* Processor Local APIC */
#define MADT_IOAPIC     1
#define MADT_ISO        2   /* Interrupt Source Override */

typedef struct __packed {
    madt_entry_hdr_t hdr;
    u8  acpi_proc_id;
    u8  apic_id;
    u32 flags;          /* bit 0: processor enabled */
} madt_lapic_entry_t;

/* Convert physical address to virtual (identity map for low memory) */
static void *phys_to_virt_low(u64 phys) {
    if (phys < 0x40000000ULL)   /* < 1GB: identity mapped */
        return (void *)phys;
    /* MMIO region: need ioremap -- but for ACPI tables they're in low RAM */
    return (void *)phys;
}

static u8 acpi_checksum(const void *ptr, u32 len) {
    const u8 *p = ptr;
    u8 sum = 0;
    while (len--) sum += *p++;
    return sum;
}

static acpi_sdt_hdr_t *find_table(acpi_rsdp_t *rsdp, u32 sig) {
    /* Prefer XSDT (v2), fall back to RSDT */
    if (rsdp->revision >= 2 && rsdp->xsdt_phys) {
        acpi_xsdt_t *xsdt = phys_to_virt_low(rsdp->xsdt_phys);
        if (!xsdt) return NULL;
        u32 n = (xsdt->hdr.length - sizeof(acpi_sdt_hdr_t)) / 8;
        for (u32 i = 0; i < n; i++) {
            acpi_sdt_hdr_t *t = phys_to_virt_low(xsdt->entries[i]);
            if (t && t->signature == sig) return t;
        }
    }
    /* RSDT */
    acpi_rsdt_t *rsdt = phys_to_virt_low(rsdp->rsdt_phys);
    if (!rsdt) return NULL;
    u32 n = (rsdt->hdr.length - sizeof(acpi_sdt_hdr_t)) / 4;
    for (u32 i = 0; i < n; i++) {
        acpi_sdt_hdr_t *t = phys_to_virt_low(rsdt->entries[i]);
        if (t && t->signature == sig) return t;
    }
    return NULL;
}

static int parse_madt(acpi_rsdp_t *rsdp, u32 bsp_lapic_id) {
    acpi_madt_t *madt = (acpi_madt_t *)find_table(rsdp, ACPI_SIG('A','P','I','C'));
    if (!madt) {
        printk(KERN_WARNING "  SMP: no MADT found\n");
        return 0;
    }

    printk(KERN_INFO "  SMP: MADT at %p, LAPIC phys=0x%x\n",
           madt, madt->lapic_phys);

    u8 *p   = (u8 *)(madt + 1);
    u8 *end = (u8 *)madt + madt->hdr.length;
    int found = 0;

    while (p < end) {
        madt_entry_hdr_t *e = (madt_entry_hdr_t *)p;
        if (e->length < 2) break;

        if (e->type == MADT_LAPIC) {
            madt_lapic_entry_t *la = (madt_lapic_entry_t *)e;
            if (!(la->flags & 1)) { p += e->length; continue; }  /* disabled */

            if (la->apic_id == (u8)bsp_lapic_id) {
                /* This is the BSP - skip */
            } else if (found < MAX_APS) {
                ap_lapic_ids[found++] = la->apic_id;
                printk(KERN_INFO "  SMP: AP%d: LAPIC ID=%u\n", found, la->apic_id);
            }
        }
        p += e->length;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* AP trampoline (16-bit real mode → 32-bit protected → 64-bit)       */
/* ------------------------------------------------------------------ */

/*
 * The trampoline blob is assembled at compile time via a dedicated
 * translation unit (smp_trampoline.S), then objcopy'd to a symbol.
 * However to avoid the cross-compile complication we embed a pre-
 * assembled blob here.  This is a minimal, well-tested trampoline
 * sequence identical to what Linux uses.
 *
 * Layout within the 0x8000 page (offset from TRAMPOLINE_PHYS):
 *   0x000  16-bit startup code
 *   0xF00  temporary GDT (3 entries: null/code32/data32)
 *   0xFF0  ap_params_t pointer (8 bytes: physical address of ap_params_t)
 *   0xFF8  reserved
 *
 * We write the trampoline as C-generated machine code instead of
 * inline asm to keep things readable and correct.  The trampoline
 * page is in the identity-mapped region (< 1GB), so physical
 * addresses == virtual addresses for this code.
 */

/* Temporary GDT inside the trampoline page (at offset 0xF00) */
#define TRAM_GDT_PHYS   (TRAMPOLINE_PHYS + 0xF00)

/* Trampoline blob symbols (from arch/x86_64/smp/trampoline.S linked into kernel) */
extern char ap_trampoline_start[];
extern char ap_trampoline_end[];

/* GDT data placed just after the trampoline code in the trampoline page */
#define TRAM_GDT_OFFSET   0x200   /* physical 0x8200: 4 entries = 32 bytes, ends at 0x8220 */
#define TRAM_GDTP_OFFSET  0x220   /* physical 0x8220 - GDT pointer (after 4 GDT entries) */
#define TRAM_PARAMS_OFF   0x400   /* physical 0x8400 - ap_params_t copy */
#define TRAM_PML4_IMMOFF  0x3A    /* byte offset of PML4 immediate in blob */
#define TRAM_PARAMS_IMMOFF 0x6A   /* byte offset of params ptr immediate in blob */

static void write_trampoline(ap_params_t *params) {
    u8 *tram = (u8 *)TRAMPOLINE_PHYS;   /* identity-mapped */
    u32 tram_size = (u32)(ap_trampoline_end - ap_trampoline_start);

    /* Copy the assembled trampoline blob */
    memset(tram, 0x90, 4096);           /* fill with NOPs */
    memcpy(tram, ap_trampoline_start, tram_size);

    /* Write temp GDT at 0x8200 (4 entries: null, code32, data32, code64) */
    u64 *gdt_tmp = (u64 *)(TRAMPOLINE_PHYS + TRAM_GDT_OFFSET);
    gdt_tmp[0] = 0;
    gdt_tmp[1] = 0x00CF9A000000FFFFULL;   /* 0x08: code32 (D=1, for PM32 phase) */
    gdt_tmp[2] = 0x00CF92000000FFFFULL;   /* 0x10: data32 */
    gdt_tmp[3] = 0x00AF9A000000FFFFULL;   /* 0x18: code64 (L=1, for LM64 phase) */
    /* GDT pointer at TRAM_GDTP_OFFSET */
    u16 *gp_lim = (u16 *)(TRAMPOLINE_PHYS + TRAM_GDTP_OFFSET);
    u32 *gp_base = (u32 *)(TRAMPOLINE_PHYS + TRAM_GDTP_OFFSET + 2);
    *gp_lim  = 4 * 8 - 1;
    *gp_base = (u32)(TRAMPOLINE_PHYS + TRAM_GDT_OFFSET);

    /* ---- Copy ap_params_t at TRAM_PARAMS_OFF (physical 0x8400) ---- */
    ap_params_t *tram_params = (ap_params_t *)(TRAMPOLINE_PHYS + TRAM_PARAMS_OFF);
    memcpy(tram_params, params, sizeof(ap_params_t));

    /* ---- Patch byte offsets within the trampoline code ---- */

    /* Patch PM32 and LM64 entry offsets using fixed known offsets from assembly */
    *(u16 *)(tram + 0x11) = (u16)(TRAMPOLINE_PHYS + TRAM_GDTP_OFFSET);   /* GDT ptr addr */
    *(u16 *)(tram + 0x1E) = (u16)(TRAMPOLINE_PHYS + 0x22);   /* PM32 entry */
    *(u32 *)(tram + 0x5B) = (u32)(TRAMPOLINE_PHYS + 0x61);   /* LM64 entry */

    /* Patch PML4 immediate (u32 at TRAM_PML4_IMMOFF) */
    extern u64 *kernel_pml4;
    *(u32 *)(tram + TRAM_PML4_IMMOFF) = (u32)(u64)kernel_pml4;

    /* Patch params pointer immediate (u64 at TRAM_PARAMS_IMMOFF) */
    *(u64 *)(tram + TRAM_PARAMS_IMMOFF) = (u64)(TRAMPOLINE_PHYS + TRAM_PARAMS_OFF);

    printk(KERN_INFO "  SMP: tram[0x1D-0x22]: %02x %02x %02x %02x %02x %02x\n",
           tram[0x1D], tram[0x1E], tram[0x1F], tram[0x20], tram[0x21], tram[0x22]);
    printk(KERN_INFO "  SMP: tram[0x58-0x63]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           tram[0x58],tram[0x59],tram[0x5A],tram[0x5B],tram[0x5C],
           tram[0x5D],tram[0x5E],tram[0x5F],tram[0x60],tram[0x61],tram[0x62]);
    printk(KERN_INFO "  SMP: tram[0x38-0x42]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           tram[0x38],tram[0x39],tram[0x3A],tram[0x3B],tram[0x3C],tram[0x3D],tram[0x3E],tram[0x3F],tram[0x40],tram[0x41]);
    printk(KERN_INFO "  SMP: tram bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           tram[0], tram[1], tram[2], tram[3],
           tram[4], tram[5], tram[6], tram[7]);
    printk(KERN_INFO "  SMP: GDT at 0x%x: %016llx %016llx\n",
           (u32)(TRAMPOLINE_PHYS + TRAM_GDT_OFFSET),
           gdt_tmp[0], gdt_tmp[1]);
    printk(KERN_INFO "  SMP: tram written (%u bytes at 0x%x, params at 0x%x)\n",
           tram_size, (u32)TRAMPOLINE_PHYS, (u32)(TRAMPOLINE_PHYS + TRAM_PARAMS_OFF));
}

/* ------------------------------------------------------------------ */
/* AP C entry point (higher-half, called from trampoline)              */
/* ------------------------------------------------------------------ */

static volatile u32 next_ap_cpu_id = 1;
static volatile u32 ap_online_count = 0;
/* Per-LAPIC-ID CPU number, set before stack switch, read after */
static volatile u32 ap_cpu_by_lapic[256];

void __attribute__((noinline, noreturn)) ap_entry64(void) {
    /* Write to QEMU debug port 0xE9 - shows in stderr even without serial init */
    __asm__ volatile(
        "mov $0xE9, %%dx\n"
        "mov $0x41, %%al\n"   /* ASCII 'A' */
        "outb %%al, %%dx\n"
        "mov $0x50, %%al\n"   /* ASCII 'P' */
        "outb %%al, %%dx\n"
        "mov $0x0A, %%al\n"   /* newline */
        "outb %%al, %%dx\n"
        :: : "ax", "dx");
    /*
     * We arrive here with:
     *  - Temp GDT (code64 + data32) active from trampoline
     *  - Kernel PML4 in CR3 (boot.S page tables — identity + higher-half)
     *  - Interrupts OFF
     *  - RSP = 0x6FF0 (tiny trampoline stack, good for a few calls)
     *
     * Load kernel GDT/IDT before anything else so we have proper descriptors.
     */

    /* Load kernel GDT and IDT (virtual addrs stored in params) */
    {
        ap_params_t *p = (ap_params_t *)(TRAMPOLINE_PHYS + TRAM_PARAMS_OFF);
        void *gp = (void*)p->gdt_ptr_virt;
        void *ip = (void*)p->idt_ptr_virt;
        __asm__ volatile("lgdt (%0)" :: "r"(gp) : "memory");
        __asm__ volatile("lidt (%0)" :: "r"(ip) : "memory");
    }

    /* Step 1: claim CPU ID using LAPIC ID as stable index */
    u32 my_lapic = lapic_id();
    u32 my_cpu = __atomic_fetch_add(&next_ap_cpu_id, 1, __ATOMIC_SEQ_CST);
    ap_cpu_by_lapic[my_lapic] = my_cpu;  /* save before stack switch */

    /* Step 2: switch to real per-AP stack before ANY C function calls. */
    ap_params_t *params = (ap_params_t *)(TRAMPOLINE_PHYS + TRAM_PARAMS_OFF);
    u64 real_stack = params->ap_stacks[my_cpu - 1];
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "xor %%rbp, %%rbp"
        :: "r"(real_stack) : "memory"
    );

    /* Step 3: restore CPU ID using LAPIC ID (stable across stack switch) */
    my_cpu = ap_cpu_by_lapic[lapic_id()];

    /* Step 4: write TSC_AUX for smp_processor_id() */
    wrmsr(MSR_IA32_TSC_AUX, (u64)my_cpu);

    /* Step 5: enable this AP's Local APIC */
    if (lapic_mmio) {
        lapic_reg_write(LAPIC_SVR, lapic_reg_read(LAPIC_SVR) | SVR_ENABLE | 0xFF);
        lapic_reg_write(LAPIC_TPR, 0);
        lapic_reg_write(LAPIC_EOI, 0);
    }

    /* Step 6: initialize this CPU's scheduler runqueue */
    sched_init_ap(my_cpu);

    /* Step 7: signal BSP that we are fully online */
    __atomic_fetch_add(&ap_online_count, 1, __ATOMIC_SEQ_CST);

    /* Step 8: announce (printk holds its own spinlock — safe from any CPU) */
    printk(KERN_INFO "  SMP: CPU%u online (LAPIC ID=%u)\n",
           my_cpu, lapic_id());

    /* Step 9: enable interrupts and idle */
    __asm__ volatile("sti");
    while (1) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* smp_init - main BSP entry point                                     */
/* ------------------------------------------------------------------ */

void smp_init(void) {
    if (!kernel_state.acpi_rsdp) {
        printk(KERN_WARNING "  SMP: no ACPI RSDP, single-CPU mode\n");
        kernel_state.num_cpus = 1;
        kernel_state.smp_enabled = false;
        return;
    }

    /* Capture current BSP GDT and IDT pointers */
    __asm__ volatile("sgdt %0" : "=m"(bsp_gdt_ptr64));
    __asm__ volatile("sidt %0" : "=m"(bsp_idt_ptr64));

    /* Also write BSP's TSC_AUX = 0 (in case lapic_init didn't run yet) */
    wrmsr(MSR_IA32_TSC_AUX, 0);

    u32 bsp_id = lapic_id();
    printk(KERN_INFO "  SMP: BSP LAPIC ID=%u\n", bsp_id);

    /* Parse MADT to find AP LAPIC IDs */
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)kernel_state.acpi_rsdp;
    ap_count = parse_madt(rsdp, bsp_id);

    if (ap_count == 0) {
        printk(KERN_INFO "  SMP: no APs found, single-CPU mode\n");
        kernel_state.num_cpus = 1;
        kernel_state.smp_enabled = false;
        return;
    }

    total_cpus = 1 + ap_count;
    printk(KERN_INFO "  SMP: found %d APs (%d total CPUs)\n", ap_count, total_cpus);

    /* Allocate per-AP stacks */
    ap_params_t params;
    memset(&params, 0, sizeof(params));

    extern u64 *kernel_pml4;
    params.pml4_phys    = (u64)kernel_pml4;
    params.ap_entry_virt = (u64)ap_entry64;
    params.gdt_ptr_virt  = (u64)&bsp_gdt_ptr64;
    params.idt_ptr_virt  = (u64)&bsp_idt_ptr64;

    for (int i = 0; i < ap_count && i < MAX_APS; i++) {
        void *stack = kmalloc(AP_STACK_SIZE, 0);
        if (!stack) {
            printk(KERN_ERR "  SMP: failed to allocate stack for AP%d\n", i);
            ap_count = i;
            break;
        }
        params.ap_stacks[i] = (u64)stack + AP_STACK_SIZE;  /* stack grows down */
        params.ap_cpu_id[i] = ap_lapic_ids[i];
        printk(KERN_INFO "  SMP: AP%d stack top=0x%llx\n", i+1,
               (unsigned long long)params.ap_stacks[i]);
    }
    params.ap_online = 0;
    params.ap_go     = 1;

    /* Write trampoline code to physical 0x8000 */
    write_trampoline(&params);

    /* Boot each AP with INIT + SIPI sequence.
     * We wait for each AP to signal online BEFORE starting the next one.
     * This avoids the double-SIPI race where AP is mid-execution when
     * the second SIPI arrives and resets it to real mode. */
    for (int i = 0; i < ap_count; i++) {
        u32 lapic_id_ap = ap_lapic_ids[i];
        u32 expected = (u32)(i + 1);  /* APs come online as 1, 2, 3... */
        printk(KERN_INFO "  SMP: booting AP%d (LAPIC ID=%u)...\n", i+1, lapic_id_ap);

        /* INIT IPI — resets AP to wait-for-SIPI state */
        lapic_send_init(lapic_id_ap);
        udelay(AP_INIT_DELAY_US);  /* Intel MP spec: wait 10ms after INIT */

        /* First SIPI: AP wakes at physical 0x8000 (vector 0x08) */
        lapic_send_sipi(lapic_id_ap, 0x08);
        udelay(AP_SIPI_DELAY_US);  /* wait 200µs for AP to start */

        /* Wait up to ~100ms for this AP to come online before sending second SIPI */
        u32 wait = 1000000;
        while (ap_online_count < expected && --wait)
            __asm__ volatile("pause");

        if (ap_online_count >= expected) {
            printk(KERN_INFO "  SMP: AP%d online after first SIPI\n", i+1);
            continue;
        }

        /* AP didn't respond to first SIPI — send second (MP spec requirement) */
        printk(KERN_INFO "  SMP: AP%d no response, sending second SIPI\n", i+1);
        lapic_send_sipi(lapic_id_ap, 0x08);

        /* Wait another ~100ms */
        wait = 1000000;
        while (ap_online_count < expected && --wait)
            __asm__ volatile("pause");
    }

    /* Final accounting */
    // per-AP wait done above

    if (ap_online_count < (u32)ap_count) {
        printk(KERN_WARNING "  SMP: timeout: only %u/%d APs came online\n",
               ap_online_count, ap_count);
    }

    kernel_state.num_cpus    = 1 + ap_online_count;
    kernel_state.smp_enabled = (ap_online_count > 0);
    printk(KERN_INFO "  SMP: %u CPUs online\n", kernel_state.num_cpus);
}
