/**
 * Picomimi-x64 Kernel Main
 * 
 * Main kernel entry point after boot assembly
 * Initializes all subsystems and starts the scheduler
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include <lib/printk.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/apic.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <sched/sched.h>
#include <drivers/serial.h>
#include <drivers/vga.h>
#include <lib/printk.h>

// ============================================================================
// KERNEL STATE (inspired by Picomimi's pm_kernel_state_t)
// ============================================================================

kernel_state_t kernel_state;

// Global jiffies counter
volatile u64 jiffies = 0;
u64 jiffies_64 = 0;

// Per-CPU offset table
u64 __per_cpu_offset[NR_CPUS] = {0};

// BSS symbols from linker
extern u64 __bss_start;
extern u64 __bss_end;
extern u64 __kernel_end;
extern u64 __kernel_end_phys;

// ============================================================================
// MULTIBOOT2 PARSING
// ============================================================================

#define MULTIBOOT2_BOOTLOADER_MAGIC     0x36D76289

#define MULTIBOOT2_TAG_TYPE_END             0
#define MULTIBOOT2_TAG_TYPE_CMDLINE         1
#define MULTIBOOT2_TAG_TYPE_BOOT_LOADER     2
#define MULTIBOOT2_TAG_TYPE_MODULE          3
#define MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO   4
#define MULTIBOOT2_TAG_TYPE_BOOTDEV         5
#define MULTIBOOT2_TAG_TYPE_MMAP            6
#define MULTIBOOT2_TAG_TYPE_VBE             7
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER     8
#define MULTIBOOT2_TAG_TYPE_ELF_SECTIONS    9
#define MULTIBOOT2_TAG_TYPE_APM             10
#define MULTIBOOT2_TAG_TYPE_EFI32           11
#define MULTIBOOT2_TAG_TYPE_EFI64           12
#define MULTIBOOT2_TAG_TYPE_SMBIOS          13
#define MULTIBOOT2_TAG_TYPE_ACPI_OLD        14
#define MULTIBOOT2_TAG_TYPE_ACPI_NEW        15
#define MULTIBOOT2_TAG_TYPE_NETWORK         16
#define MULTIBOOT2_TAG_TYPE_EFI_MMAP        17
#define MULTIBOOT2_TAG_TYPE_EFI_BS          18
#define MULTIBOOT2_TAG_TYPE_EFI32_IH        19
#define MULTIBOOT2_TAG_TYPE_EFI64_IH        20
#define MULTIBOOT2_TAG_TYPE_LOAD_BASE_ADDR  21

#define MULTIBOOT2_MEMORY_AVAILABLE         1
#define MULTIBOOT2_MEMORY_RESERVED          2
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE  3
#define MULTIBOOT2_MEMORY_NVS               4
#define MULTIBOOT2_MEMORY_BADRAM            5

struct multiboot2_tag {
    u32 type;
    u32 size;
} __packed;

struct multiboot2_tag_string {
    u32 type;
    u32 size;
    char string[];
} __packed;

struct multiboot2_tag_mmap {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
} __packed;

struct multiboot2_mmap_entry {
    u64 addr;
    u64 len;
    u32 type;
    u32 reserved;
} __packed;

struct multiboot2_tag_basic_meminfo {
    u32 type;
    u32 size;
    u32 mem_lower;
    u32 mem_upper;
} __packed;

struct multiboot2_tag_framebuffer {
    u32 type;
    u32 size;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8  framebuffer_bpp;
    u8  framebuffer_type;
    u16 reserved;
} __packed;

// ============================================================================
// EARLY INITIALIZATION
// ============================================================================

static void early_serial_init(void) {
    serial_init(SERIAL_COM1);
}

static void early_vga_init(void) {
    vga_init();
    vga_clear();
}

// ============================================================================
// MULTIBOOT2 INFO PARSING
// ============================================================================

static void parse_multiboot2(u32 magic, u64 mbi_addr) {
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        printk(KERN_ERR "Invalid multiboot2 magic: 0x%x\n", magic);
        return;
    }

    // MBI is at physical address, we need to use higher-half mapping
    // For now, identity mapping is still active so we can access it
    struct multiboot2_tag *tag;
    u64 addr = mbi_addr;
    
    // Skip the total size field
    u32 total_size = *(u32 *)addr;
    addr += 8;  // Skip size and reserved

    printk(KERN_INFO "Multiboot2 info at 0x%lx, size %u bytes\n", mbi_addr, total_size);

    for (tag = (struct multiboot2_tag *)addr;
         tag->type != MULTIBOOT2_TAG_TYPE_END;
         tag = (struct multiboot2_tag *)((u8 *)tag + ((tag->size + 7) & ~7))) {
        
        switch (tag->type) {
            case MULTIBOOT2_TAG_TYPE_CMDLINE: {
                struct multiboot2_tag_string *cmdline = (void *)tag;
                printk(KERN_INFO "Command line: %s\n", cmdline->string);
                break;
            }

            case MULTIBOOT2_TAG_TYPE_BOOT_LOADER: {
                struct multiboot2_tag_string *bootloader = (void *)tag;
                printk(KERN_INFO "Boot loader: %s\n", bootloader->string);
                break;
            }

            case MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO: {
                struct multiboot2_tag_basic_meminfo *meminfo = (void *)tag;
                printk(KERN_INFO "Memory: lower=%uKB, upper=%uKB\n", 
                       meminfo->mem_lower, meminfo->mem_upper);
                kernel_state.total_memory = ((u64)meminfo->mem_upper + 1024) * 1024;
                break;
            }

            case MULTIBOOT2_TAG_TYPE_MMAP: {
                struct multiboot2_tag_mmap *mmap_tag = (void *)tag;
                struct multiboot2_mmap_entry *entry;
                u64 end_addr = (u64)tag + tag->size;
                
                printk(KERN_INFO "Memory map:\n");
                
                for (entry = (void *)((u8 *)tag + 16);
                     (u64)entry < end_addr;
                     entry = (void *)((u8 *)entry + mmap_tag->entry_size)) {
                    
                    const char *type_str;
                    switch (entry->type) {
                        case MULTIBOOT2_MEMORY_AVAILABLE:
                            type_str = "available";
                            // Pass to physical memory manager
                            pmm_add_region(entry->addr, entry->len);
                            break;
                        case MULTIBOOT2_MEMORY_RESERVED:
                            type_str = "reserved";
                            break;
                        case MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE:
                            type_str = "ACPI reclaimable";
                            break;
                        case MULTIBOOT2_MEMORY_NVS:
                            type_str = "NVS";
                            break;
                        case MULTIBOOT2_MEMORY_BADRAM:
                            type_str = "bad RAM";
                            break;
                        default:
                            type_str = "unknown";
                            break;
                    }
                    
                    printk(KERN_INFO "  0x%016lx - 0x%016lx (%s)\n",
                           entry->addr, entry->addr + entry->len - 1, type_str);
                }
                break;
            }

            case MULTIBOOT2_TAG_TYPE_FRAMEBUFFER: {
                struct multiboot2_tag_framebuffer *fb = (void *)tag;
                printk(KERN_INFO "Framebuffer: %ux%u@%u at 0x%lx\n",
                       fb->framebuffer_width, fb->framebuffer_height,
                       fb->framebuffer_bpp, fb->framebuffer_addr);
                break;
            }

            case MULTIBOOT2_TAG_TYPE_ACPI_OLD: {
                printk(KERN_INFO "ACPI 1.0 RSDP found\n");
                // Store for later ACPI parsing
                kernel_state.acpi_rsdp = (void *)((u8 *)tag + 8);
                break;
            }

            case MULTIBOOT2_TAG_TYPE_ACPI_NEW: {
                printk(KERN_INFO "ACPI 2.0+ RSDP found\n");
                kernel_state.acpi_rsdp = (void *)((u8 *)tag + 8);
                break;
            }
        }
    }
}

// ============================================================================
// KERNEL MAIN
// ============================================================================

void __noreturn kernel_main(u32 magic, u64 multiboot_info) {
    // VERY early debug - write directly to VGA
    volatile u16 *vga = (volatile u16 *)0xFFFFFFFF800B8000;
    vga[0] = 0x0F50; // 'P'
    vga[1] = 0x0F49; // 'I'
    vga[2] = 0x0F43; // 'C'
    vga[3] = 0x0F4F; // 'O'
    
    // VERY early debug - write directly to COM1
    // First init: disable interrupts, set 8N1, enable FIFO
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x00), "Nd"((u16)0x3F9));  // IER = 0
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x80), "Nd"((u16)0x3FB));  // LCR = DLAB
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x01), "Nd"((u16)0x3F8));  // DLL = 1 (115200)
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x00), "Nd"((u16)0x3F9));  // DLH = 0
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x03), "Nd"((u16)0x3FB));  // LCR = 8N1
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0xC7), "Nd"((u16)0x3FA));  // FCR = enable FIFO
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)0x0B), "Nd"((u16)0x3FC));  // MCR = DTR|RTS|OUT2
    
    // Now write "BOOT\r\n" directly
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'B'), "Nd"((u16)0x3F8));
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'O'), "Nd"((u16)0x3F8));
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'O'), "Nd"((u16)0x3F8));
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'T'), "Nd"((u16)0x3F8));
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'\r'), "Nd"((u16)0x3F8));
    __asm__ volatile("outb %b0, %w1" : : "a"((u8)'\n'), "Nd"((u16)0x3F8));
    
    // Initialize kernel state
    kernel_state.initialized = false;
    kernel_state.running = false;
    kernel_state.boot_time_ns = 0;
    kernel_state.num_cpus = 1;
    kernel_state.bsp_cpu_id = 0;

    // Early initialization - serial first for debugging
    early_serial_init();
    early_vga_init();

    // Print banner
    printk(KERN_INFO "\n");
    printk(KERN_INFO "================================================\n");
    printk(KERN_INFO "  Picomimi-x64 Kernel " KERNEL_VERSION_STRING "\n");
    printk(KERN_INFO "  A Linux-compatible x86_64 kernel\n");
    printk(KERN_INFO "  Inspired by Picomimi-AxisOS for RP2040/RP2350\n");
    printk(KERN_INFO "================================================\n");
    printk(KERN_INFO "\n");

    // Initialize CPU
    printk(KERN_INFO "[INIT] CPU initialization...\n");
    cpu_init();
    
    // Initialize GDT with TSS
    printk(KERN_INFO "[INIT] GDT initialization...\n");
    gdt_init();

    // Initialize IDT
    printk(KERN_INFO "[INIT] IDT initialization...\n");
    idt_init();

    // Initialize physical memory manager FIRST
    printk(KERN_INFO "[INIT] Physical memory manager...\n");
    pmm_init();

    // Parse multiboot2 info (adds memory regions to PMM)
    printk(KERN_INFO "[INIT] Parsing multiboot2 info...\n");
    parse_multiboot2(magic, multiboot_info);

    // Reserve kernel memory
    extern u64 __kernel_phys_start;
    pmm_reserve_region(0, 0x100000);  // Reserve first 1MB
    pmm_reserve_region((u64)&__kernel_phys_start, (u64)&__kernel_end_phys - (u64)&__kernel_phys_start);

    // Initialize virtual memory manager
    printk(KERN_INFO "[INIT] Virtual memory manager...\n");
    vmm_init();

    // Initialize slab allocator
    printk(KERN_INFO "[INIT] Slab allocator...\n");
    slab_init();

    // Initialize Local APIC
    printk(KERN_INFO "[INIT] Local APIC...\n");
    lapic_init();

    // Initialize I/O APIC
    printk(KERN_INFO "[INIT] I/O APIC...\n");
    ioapic_init();

    // Initialize scheduler
    printk(KERN_INFO "[INIT] Scheduler...\n");
    sched_init();

    // Initialize Scheduler Hypervisor
    printk(KERN_INFO "[INIT] Scheduler Hypervisor...\n");
    extern void sched_hypervisor_init(void);
    sched_hypervisor_init();

    // Initialize SMP (start other CPUs)
    printk(KERN_INFO "[INIT] SMP initialization...\n");
    smp_init();

    // Terminal emulator disabled - using simple VGA
    // printk(KERN_INFO "[INIT] Terminal emulator...\n");
    // extern void term_init(void);
    // term_init();

    // Initialize PCI bus
    printk(KERN_INFO "[INIT] PCI bus...\n");
    extern void pci_init(void);
    pci_init();
    
    // Initialize network driver
    printk(KERN_INFO "[INIT] Network drivers...\n");
    extern void e1000_init(void);
    e1000_init();

    // Initialize process management
    printk(KERN_INFO "[INIT] Process management...\n");
    extern void process_init(void);
    process_init();
    
    // Initialize syscall table
    printk(KERN_INFO "[INIT] Syscall table...\n");
    extern void syscall_init_table(void);
    syscall_init_table();

    // Initialize syscall interface (MSRs)
    printk(KERN_INFO "[INIT] Syscall interface...\n");
    syscall_init();
    
    // Initialize VFS
    printk(KERN_INFO "[INIT] Virtual filesystem...\n");
    extern void vfs_init(void);
    vfs_init();
    
    // Mount root filesystem
    printk(KERN_INFO "[INIT] Root filesystem...\n");
    extern int init_rootfs(void);
    int err = init_rootfs();
    if (err) {
        printk(KERN_ERR "[INIT] Failed to mount root: %d\n", err);
    }
    
    // Initialize devfs (/dev)
    printk(KERN_INFO "[INIT] Device filesystem...\n");
    extern int init_devfs(void);
    err = init_devfs();
    if (err) {
        printk(KERN_ERR "[INIT] Failed to create /dev: %d\n", err);
    }
    
    // Create init process
    printk(KERN_INFO "[INIT] Creating init task...\n");
    extern task_struct_t *create_init_task(void);
    create_init_task();

    // Mark kernel as initialized
    kernel_state.initialized = true;
    kernel_state.running = true;

    printk(KERN_INFO "\n");
    printk(KERN_INFO "[INIT] Kernel initialization complete!\n");
    printk(KERN_INFO "[INIT] Total memory: %lu MB\n", kernel_state.total_memory / (1024 * 1024));
    printk(KERN_INFO "[INIT] Free memory: %lu MB\n", pmm_get_free_memory() / (1024 * 1024));
    printk(KERN_INFO "[INIT] CPUs online: %u\n", kernel_state.num_cpus);
    printk(KERN_INFO "\n");

    // Enable interrupts
    printk(KERN_INFO "[INIT] Enabling interrupts...\n");
    sti();

    // Initialize kernel shell
    printk(KERN_INFO "[INIT] Starting kernel shell...\n");
    extern void ksh_init(void);
    ksh_init();
    
    // Idle loop
    while (1) {
        cpu_relax();
        __asm__ volatile("hlt");
    }
}

// ============================================================================
// PANIC HANDLER
// ============================================================================

void __noreturn kernel_panic(const char *fmt, ...) {
    // Disable interrupts
    cli();

    printk(KERN_EMERG "\n");
    printk(KERN_EMERG "========================================\n");
    printk(KERN_EMERG "         KERNEL PANIC!\n");
    printk(KERN_EMERG "========================================\n");
    
    // Print the panic message
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    
    printk(KERN_EMERG "\n");
    printk(KERN_EMERG "System halted.\n");

    // Halt all CPUs
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================================
// CPU CONTROL
// ============================================================================

void cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

void sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

u64 read_flags(void) {
    u64 flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return flags;
}

void write_flags(u64 flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

irqflags_t local_irq_save(void) {
    irqflags_t flags = read_flags();
    cli();
    return flags;
}

void local_irq_restore(irqflags_t flags) {
    write_flags(flags);
}
