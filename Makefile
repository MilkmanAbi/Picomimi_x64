# =============================================================================
# Picomimi-x64 Kernel Makefile
#
# A Linux-compatible x86_64 kernel inspired by Picomimi-AxisOS
# =============================================================================

# Cross-compiler (use native GCC for now, cross-compile later)
CC := gcc
AS := gcc
LD := ld
OBJCOPY := objcopy

# Kernel name
KERNEL := picomimi-x64
ISO := $(KERNEL).iso

# Directories
SRCDIR := .
OBJDIR := build
ISODIR := iso

# Source files
ASM_SOURCES := \
    arch/x86_64/boot/boot.S \
    arch/x86_64/idt/interrupts.S \
    arch/x86_64/context.S

C_SOURCES := \
    kernel/kernel.c \
    kernel/init.c \
    kernel/process.c \
    kernel/syscall.c \
    kernel/sched.c \
    kernel/elf.c \
    arch/x86_64/idt/handlers.c \
    drivers/serial/serial.c \
    drivers/vga/vga.c \
    drivers/tty/tty.c \
    drivers/pci/pci.c \
    drivers/net/e1000.c \
    drivers/tty/terminal.c \
    kernel/sched_hyper.c \
    kernel/shell.c \
    fs/vfs.c \
    fs/ramfs.c \
    fs/devfs.c \
    lib/printk.c \
    lib/string.c

# Object files
ASM_OBJECTS := $(ASM_SOURCES:%.S=$(OBJDIR)/%.o)
C_OBJECTS := $(C_SOURCES:%.c=$(OBJDIR)/%.o)
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

# Compiler flags
CFLAGS := -Wall -Wextra -Werror
CFLAGS += -std=gnu11
CFLAGS += -ffreestanding
CFLAGS += -fno-builtin
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pic
CFLAGS += -fno-pie
CFLAGS += -mno-red-zone
CFLAGS += -mno-mmx
CFLAGS += -mno-sse
CFLAGS += -mno-sse2
CFLAGS += -mcmodel=kernel
CFLAGS += -m64
CFLAGS += -g
CFLAGS += -O2
CFLAGS += -I$(SRCDIR)/include

# Assembler flags
ASFLAGS := -m64
ASFLAGS += -g
ASFLAGS += -I$(SRCDIR)/include

# Linker flags
LDFLAGS := -nostdlib
LDFLAGS += -z max-page-size=0x1000
LDFLAGS += -T scripts/linker.ld

# QEMU flags
QEMU := qemu-system-x86_64
QEMU_FLAGS := -cdrom $(ISO)
QEMU_FLAGS += -serial stdio
QEMU_FLAGS += -m 256M
QEMU_FLAGS += -smp 4
QEMU_FLAGS += -no-reboot
QEMU_FLAGS += -no-shutdown

# Debug QEMU flags
QEMU_DEBUG_FLAGS := $(QEMU_FLAGS)
QEMU_DEBUG_FLAGS += -s -S
QEMU_DEBUG_FLAGS += -d int,cpu_reset

# =============================================================================
# TARGETS
# =============================================================================

.PHONY: all clean run debug iso dirs

all: $(OBJDIR)/$(KERNEL).elf

# Create directories
dirs:
	@mkdir -p $(OBJDIR)/arch/x86_64/boot
	@mkdir -p $(OBJDIR)/arch/x86_64/idt
	@mkdir -p $(OBJDIR)/kernel
	@mkdir -p $(OBJDIR)/drivers/serial
	@mkdir -p $(OBJDIR)/drivers/vga
	@mkdir -p $(OBJDIR)/lib
	@mkdir -p $(OBJDIR)/mm
	@mkdir -p $(OBJDIR)/sched

# Compile assembly
$(OBJDIR)/%.o: %.S | dirs
	@echo "  AS    $<"
	@$(AS) $(ASFLAGS) -c $< -o $@

# Compile C
$(OBJDIR)/%.o: %.c | dirs
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Link kernel
$(OBJDIR)/$(KERNEL).elf: $(OBJECTS)
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo ""
	@echo "Kernel built successfully!"
	@echo "Size: $$(stat -c%s $@) bytes"

# Create ISO
iso: $(OBJDIR)/$(KERNEL).elf
	@echo "  ISO   $(ISO)"
	@mkdir -p $(ISODIR)/boot/grub
	@cp $(OBJDIR)/$(KERNEL).elf $(ISODIR)/boot/kernel
	@cp scripts/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO) $(ISODIR) 2>/dev/null || \
		(echo "grub-mkrescue not found, trying grub2-mkrescue..." && \
		 grub2-mkrescue -o $(ISO) $(ISODIR) 2>/dev/null) || \
		(echo "ERROR: grub-mkrescue/grub2-mkrescue not found!" && \
		 echo "Install with: apt install grub-pc-bin grub-common xorriso" && \
		 exit 1)
	@echo ""
	@echo "ISO created: $(ISO)"
	@echo "Size: $$(stat -c%s $(ISO)) bytes"

# Run in QEMU
run: iso
	@echo "Starting QEMU..."
	@echo "Press Ctrl+A, X to exit"
	@echo ""
	$(QEMU) $(QEMU_FLAGS)

# Run in QEMU with GDB server
debug: iso
	@echo "Starting QEMU in debug mode..."
	@echo "Connect GDB with: target remote localhost:1234"
	@echo ""
	$(QEMU) $(QEMU_DEBUG_FLAGS)

# Quick run (kernel only, no ISO)
qrun: $(OBJDIR)/$(KERNEL).elf
	$(QEMU) -kernel $(OBJDIR)/$(KERNEL).elf -serial stdio -m 256M

# Clean
clean:
	@echo "  CLEAN"
	@rm -rf $(OBJDIR) $(ISODIR) $(ISO)

# Show info
info:
	@echo "Picomimi-x64 Kernel Build System"
	@echo "================================"
	@echo "Compiler:  $(CC)"
	@echo "Assembler: $(AS)"
	@echo "Linker:    $(LD)"
	@echo ""
	@echo "Targets:"
	@echo "  all    - Build kernel ELF"
	@echo "  iso    - Create bootable ISO"
	@echo "  run    - Run in QEMU"
	@echo "  debug  - Run with GDB server"
	@echo "  clean  - Clean build files"

# Dependencies
-include $(OBJECTS:.o=.d)
