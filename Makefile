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
    arch/x86_64/smp/trampoline.S \
    arch/x86_64/idt/interrupts.S \
    arch/x86_64/context.S

C_SOURCES := \
    kernel/kernel.c \
    kernel/init.c \
    kernel/process.c \
    kernel/syscall.c \
	kernel/exec.c \
    kernel/syscall_table.c \
    kernel/syscall_stubs.c \
    kernel/sched.c \
    kernel/sched_hyper.c \
    kernel/elf.c \
    kernel/elf_loader.c \
    kernel/signal.c \
    kernel/timer.c \
    kernel/wait.c \
    kernel/mmap.c \
    kernel/fs_syscalls.c \
    kernel/at_syscalls.c \
    kernel/shell.c \
    kernel/sysctl.c \
    kernel/cred.c \
    init/init.c \
    kernel/ramfs_populate.c \
    arch/x86_64/idt/handlers.c \
    drivers/serial/serial.c \
    drivers/vga/vga.c \
    drivers/tty/tty.c \
    drivers/tty/tty_ldisc.c \
    drivers/tty/terminal.c \
    drivers/pci/pci.c \
    drivers/net/e1000.c \
    drivers/fb/fb.c \
    drivers/fb/fbcon.c \
    arch/x86_64/smp/smp.c \
    drivers/keyboard/keyboard.c \
    drivers/mouse/mouse.c \
    drivers/input/input.c \
    drivers/chrdev.c \
    net/net_core.c \
    net/tcp.c \
    net/socket_syscalls.c \
    fs/vfs.c \
    fs/vfs_ops.c \
    fs/ramfs.c \
    fs/devfs.c \
    fs/procfs.c \
    fs/sysfs.c \
    fs/pipe.c \
    lib/printk.c \
    lib/string.c \

# Object files
ASM_OBJECTS := $(ASM_SOURCES:%.S=$(OBJDIR)/%.o)
C_OBJECTS := $(C_SOURCES:%.c=$(OBJDIR)/%.o)
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

# Compiler flags
CFLAGS := -Wall -Wextra -Werror
CFLAGS += -Wno-error=cast-function-type
CFLAGS += -Wno-error=format
CFLAGS += -Wno-error=unused-function
CFLAGS += -Wno-error=unused-parameter
CFLAGS += -Wno-unused-function
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-pointer-sign
CFLAGS += -Wno-sign-compare
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
	@mkdir -p $(OBJDIR)/drivers/tty
	@mkdir -p $(OBJDIR)/drivers/pci
	@mkdir -p $(OBJDIR)/drivers/net
	@mkdir -p $(OBJDIR)/drivers/fb
	@mkdir -p $(OBJDIR)/arch/x86_64/smp
	@mkdir -p $(OBJDIR)/drivers/keyboard
	@mkdir -p $(OBJDIR)/drivers/mouse
	@mkdir -p $(OBJDIR)/drivers/input
	@mkdir -p $(OBJDIR)/drivers
	@mkdir -p $(OBJDIR)/fs
	@mkdir -p $(OBJDIR)/lib
	@mkdir -p $(OBJDIR)/mm
	@mkdir -p $(OBJDIR)/sched
	@mkdir -p $(OBJDIR)/net

# Compile assembly
$(OBJDIR)/%.o: %.S | dirs
	@echo "  AS    $<"
	@$(AS) $(ASFLAGS) -c $< -o $@

# Compile C
$(OBJDIR)/%.o: %.c | dirs
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Link kernel

# Embed userspace /bin/busybox as a binary blob
# busybox is a pre-built static binary (from package busybox-static)
# To update: cp /usr/bin/busybox iso/boot/userspace/bin/busybox
build/sh_blob.o: iso/boot/userspace/bin/busybox | build
	@echo "  BLOB  $< ($(shell stat -c%s $<) bytes)"
	@x86_64-linux-gnu-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

iso/boot/userspace/bin/busybox:
	@echo "  ERROR: Missing iso/boot/userspace/bin/busybox"
	@echo "         Run: cp /usr/bin/busybox iso/boot/userspace/bin/busybox"
	@exit 1

# Build userspace binaries (must run before kernel link)
iso/boot/userspace/bin/sh:
	@$(MAKE) -C userspace
$(OBJDIR)/$(KERNEL).elf: $(OBJECTS) build/sh_blob.o
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS) build/sh_blob.o
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
