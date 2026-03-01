/**
 * Picomimi-x64 Linker Script
 * 
 * Physical 0x100000 maps to Virtual 0xFFFFFFFF80100000
 * (Because 2MB huge pages require alignment)
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS = 0x100000;
KERNEL_VIRT = 0xFFFFFFFF80100000;  /* Maps directly to 0x100000 physical */

SECTIONS
{
    . = KERNEL_PHYS;

    /* Multiboot header must be in first 32KB */
    .multiboot ALIGN(8) : {
        KEEP(*(.multiboot))
    }

    /* 32-bit boot code and data (physical addresses) */
    .text.boot ALIGN(4K) : {
        *(.text.boot)
    }

    .rodata.boot ALIGN(16) : {
        *(.rodata.boot)
    }

    .data.boot ALIGN(8) : {
        *(.data.boot)
    }

    .bss.boot ALIGN(4K) : {
        *(.bss.boot)
    }

    /* Switch to virtual addresses */
    . = KERNEL_VIRT + (. - KERNEL_PHYS);

    /* Code section */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT + KERNEL_PHYS) {
        __text_start = .;
        *(.text)
        *(.text.*)
        __text_end = .;
    }

    /* Read-only data */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT + KERNEL_PHYS) {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        __rodata_end = .;
    }

    /* Data section */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT + KERNEL_PHYS) {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    }

    /* BSS section */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT + KERNEL_PHYS) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}

__kernel_end_phys = __kernel_end - KERNEL_VIRT + KERNEL_PHYS;

PROVIDE(__kernel_phys_start = KERNEL_PHYS);
PROVIDE(__kernel_virt_start = KERNEL_VIRT);
