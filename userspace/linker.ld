/* Picomimi-x64 Userspace Linker Script
 * Standard x86-64 ELF layout, base 0x400000
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text._start)
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : { *(.rodata*) }

    . = ALIGN(4096);
    .data : { *(.data*) }

    . = ALIGN(4096);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
