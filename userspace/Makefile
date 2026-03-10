# Picomimi-x64 Userspace Build System
CC      = x86_64-linux-gnu-gcc
AS      = x86_64-linux-gnu-as
LD      = x86_64-linux-gnu-ld

CFLAGS  = -m64 -std=c11 -O2 -Wall -Wextra \
          -ffreestanding -fno-stack-protector \
          -fno-builtin -nostdinc -nostdlib \
          -I$(CURDIR)/include

LDFLAGS = -static -nostdlib -T linker.ld

OUTDIR  = ../iso/boot/userspace
OBJDIR  = $(OUTDIR)/.obj

LIBC_OBJ = $(OBJDIR)/libc.o
CRT0_OBJ = $(OBJDIR)/crt0.o

.PHONY: all clean

all:
	@mkdir -p $(OUTDIR)/bin $(OUTDIR)/sbin $(OBJDIR)
	@$(MAKE) $(OUTDIR)/bin/sh
	@echo "[USERSPACE] All binaries built"

$(CRT0_OBJ): crt0.S
	$(AS) -o $@ $<

$(LIBC_OBJ): libc/libc.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUTDIR)/bin/sh: sh/sh.c $(LIBC_OBJ) $(CRT0_OBJ) linker.ld
	$(CC) $(CFLAGS) -c -o $(OBJDIR)/sh.o $<
	$(LD) $(LDFLAGS) -o $@ $(CRT0_OBJ) $(OBJDIR)/sh.o $(LIBC_OBJ)
	@echo "[USERSPACE] /bin/sh: $$(size $@ | tail -1)"

clean:
	rm -rf $(OUTDIR)
