# Picomimi-x64 Kernel

A x86_64 kernel inspired by [Picomimi-AxisOS](https://github.com/MilkmanAbi/picomimi-axisos), 
the microkernel OS for ARM Cortex-M microcontrollers. (Based off unreleased internal Picomimi dev releases, v16 and v17.8). Picomimi-x64 IS NOT BASED OFF LINUX, it is a custom hobbyist kernel, it borrows syscall and API names from Linux but it is coded from the ground up.

>**Note:** I am passionate about systems engineering, kernel development, and the ARM and RISC-V ISAs. I enjoy working with basic assembly, but I have no interest in x64, absolutely none. Picomimi-x64 was created purely for fun and learning, with extensive AI assistance to get it running on x64. It is a fork of my original project, Picomimi, which I developed for ARM Cortex-M MCUs. Picomimi features fully custom schedulers, dynamic memory management, syscalls, APIs, and many other components I built from scratch. I use AI not to replace my work but because I simply don’t care for x86 and don’t have the time or motivation to master it. I have school, I simply don't care enough to fully master x86 in between juggling assignments when x86 isn't even my passion, I'm open about it. This project is a fun little hobby project, will NEVER be proffessional nor be used anywhere whatsoever, most hobbyist OSes don't, I know this is a fun little project and I treat it as such, I use AI to help get x64 working while I write actual Kernel code.

```
================================================
  Picomimi-x64 Kernel 1.0.0
  A partially Linux-compatible x86_64 kernel
  Inspired by Picomimi-AxisOS for RP2040/RP2350
================================================
```

## Features

### Core Architecture
- **64-bit Long Mode** - Full x86_64 support with 4-level paging
- **Higher-Half Kernel** - Kernel mapped at 0xFFFFFFFF80000000
- **Multiboot2 Compliant** - Boots with GRUB2 and other Multiboot2 loaders
- **Multi-Hypervisor Support** - Tested on QEMU, VirtualBox, and VMware

### Memory Management
- **Physical Memory Manager (PMM)** - Bitmap-based page allocator
- **Virtual Memory Manager (VMM)** - 4-level page table management
- **Slab Allocator** - Fast kernel object allocation (kmalloc/kfree)
- **Memory-Mapped I/O (MMIO)** - ioremap/iounmap for device memory

### Process Management
- **Full Process Model** - task_struct compatible with Linux design
- **PID Allocation** - Bitmap-based PID allocator (up to 32768 PIDs)
- **Fork/Clone/Exit** - Process creation and termination
- **Credentials System** - UID/GID/EUID/EGID support
- **File Descriptor Tables** - Per-process FD management with reference counting

### Scheduler Hypervisor
Hierarchical scheduler allowing multiple scheduling policies to coexist!

```
┌─────────────────────────────────────────────────────────┐
│              Main Preemptive Scheduler                  │
│  (O(1) bitmap, handles domain selection & preemption)   │
└────────┬──────────┬──────────┬──────────┬───────────────┘
         │          │          │          │
    ┌────▼────┐┌────▼─────┐┌────▼────┐┌────▼─────┐
    │  COOP   ││REALTIM E ││  BATCH  ││  IDLE    │
    │ Domain  ││ Domain   ││ Domain  ││ Domain   │
    │(fibers) ││(deadline)││(low pri)││(bg jobs) │
    └─────────┘└──────────┘└─────────┘└──────────┘
```

**Pluggable Scheduler Classes:**
- **Cooperative** - Fiber/coroutine scheduling (voluntary yield)
- **Realtime (EDF)** - Earliest Deadline First scheduling
- **Fair (CFS-like)** - Completely Fair Scheduler with vruntime
- **Batch** - Throughput-oriented for background jobs
- **Idle** - Lowest priority background tasks

### Terminal Emulator
- **ANSI/VT100 Escape Sequences** - Full color and cursor control
- **16 Colors** - Standard + bright colors
- **1000-line Scrollback Buffer** - Browse history with Page Up/Down
- **Line Editing** - Backspace, history navigation
- **Special Keys** - Ctrl+C, Ctrl+L support

### Built-in Kernel Shell (ksh)
Interactive shell for kernel debugging and testing:

```
╔══════════════════════════════════════════════════════════════╗
║     Picomimi-x64 Kernel v1.0.0                               ║
║     A Linux-compatible x86_64 kernel                         ║
║     Inspired by Picomimi-AxisOS for RP2040/RP2350            ║
╚══════════════════════════════════════════════════════════════╝

picomimi:/# help
```

**Commands:**
- `help` - Show available commands
- `clear` - Clear screen (Ctrl+L)
- `uname` - System information
- `uptime` - System uptime
- `meminfo` - Memory statistics
- `cpuinfo` - CPU information with CPUID
- `ps` - Process list
- `lspci` - PCI device list
- `lsdev` - /dev entries
- `sched` - Scheduler hypervisor stats
- `domains` - Scheduler domains
- `test colors` - Terminal color test
- `test syscall` - System call test
- `color` - Colorful banner
- `reboot` / `halt` - System control

### O(1) Scheduler
- **Priority Bitmap** - O(1) task selection inspired by Picomimi-AxisOS
- **140 Priority Levels** - 0-99 RT, 100-139 normal (nice -20 to +19)
- **Per-CPU Run Queues** - SMP-ready design
- **Active/Expired Arrays** - Round-robin within priority

### System Calls
- **512 Syscall Table** - Linux x86_64 ABI compatible
- **SYSCALL/SYSRET** - Fast syscall mechanism via MSRs
- **INT 0x80 Fallback** - Legacy syscall support

#### Implemented Syscalls
| Category | Syscalls |
|----------|----------|
| Process ID | getpid, gettid, getppid, getuid, geteuid, getgid, getegid |
| Process Control | fork, vfork, clone, exit, exit_group, wait4, kill, tkill, tgkill |
| File Operations | read, write, open, close, lseek, dup, dup2, dup3, pipe, pipe2, fcntl, ioctl |
| Memory | brk, mmap, munmap, mprotect |
| Filesystem | getcwd, chdir, fchdir, mkdir, rmdir, unlink, rename, chmod, chown, umask |
| Time | gettimeofday, clock_gettime, nanosleep |
| System Info | uname, sysinfo |
| Scheduling | sched_yield |
| Signals | rt_sigaction, rt_sigprocmask, pause |
| Misc | set_tid_address, arch_prctl, getrandom, prctl |

### Virtual File System (VFS)
- **Inode/Dentry/Superblock** - Full VFS abstraction layer
- **Multiple Filesystem Support** - Pluggable filesystem architecture
- **Mount Table** - Hierarchical mount management

#### Filesystems
- **rootfs** - Root RAM filesystem
- **ramfs** - In-memory filesystem
- **devfs** - Device filesystem (/dev)

#### Device Nodes
```
/dev/null    (1:3)   - Null device
/dev/zero    (1:5)   - Zero device
/dev/full    (1:7)   - Full device
/dev/random  (1:8)   - Random device
/dev/urandom (1:9)   - Urandom device
/dev/tty     (5:0)   - TTY device
/dev/console (5:1)   - Console device
```

### Interrupts & Exceptions
- **IDT** - 256-entry Interrupt Descriptor Table
- **Exception Handlers** - All 32 CPU exceptions handled
- **PIC** - 8259 PIC driver (IRQ 0-15)
- **Timer** - PIT at 100 Hz
- **Keyboard** - PS/2 keyboard support

### PCI Bus
- **Full Enumeration** - Scans all buses, slots, and functions
- **BAR Detection** - Memory and I/O BAR support (32-bit and 64-bit)
- **Driver Framework** - PCI driver registration and matching
- **Device/Vendor IDs** - Class code detection

### Network
- **Intel e1000 Driver** - 82540EM Gigabit Ethernet support
  - MMIO register access
  - EEPROM MAC address reading
  - Device reset and initialization
  - Multiple NIC support

### ELF Loader
- **ELF64 Support** - Load and execute ELF binaries
- **Program Header Parsing** - PT_LOAD segment handling
- **Entry Point Detection** - Proper process entry setup

### Console
- **VGA Text Mode** - 80x25 text output
- **Serial Console** - COM1 @ 115200 baud
- **printk** - Kernel logging with severity levels

## Building

### Requirements
- GCC cross-compiler (x86_64-elf-gcc) or native GCC
- NASM assembler
- GNU Make
- GRUB2 (for ISO creation)
- xorriso (for ISO creation)

### Build Commands
```bash
make            # Build kernel ELF
make iso        # Create bootable ISO
make clean      # Clean build artifacts
make run        # Run in QEMU
make debug      # Run in QEMU with GDB server
```

### Testing
```bash
# QEMU
qemu-system-x86_64 -cdrom picomimi-x64.iso -serial stdio -m 256M

# With networking
qemu-system-x86_64 -cdrom picomimi-x64.iso -serial stdio -m 256M \
    -device e1000,netdev=net0 -netdev user,id=net0

# VirtualBox (create VM with IDE CD-ROM)
VBoxManage createvm --name picomimi-x64 --ostype Linux_64 --register
VBoxManage modifyvm picomimi-x64 --memory 256 --boot1 dvd
VBoxManage storagectl picomimi-x64 --name "IDE" --add ide
VBoxManage storageattach picomimi-x64 --storagectl "IDE" --port 0 --device 0 \
    --type dvddrive --medium picomimi-x64.iso
```

## Project Structure

```
Picomimi-x64/
├── arch/x86_64/
│   ├── boot/           # Boot code (Multiboot2, GDT, long mode)
│   ├── idt/            # IDT, interrupt handlers, PIC/PIT
│   └── context.S       # Context switching
├── drivers/
│   ├── pci/            # PCI bus driver
│   ├── net/            # Network drivers (e1000)
│   ├── serial/         # Serial port driver
│   ├── tty/            # TTY driver + terminal emulator
│   └── vga/            # VGA text mode driver
├── fs/
│   ├── vfs.c           # Virtual File System
│   ├── ramfs.c         # RAM filesystem
│   └── devfs.c         # Device filesystem
├── include/
│   ├── arch/           # Architecture headers
│   ├── drivers/        # Driver headers
│   ├── fs/             # Filesystem headers
│   ├── kernel/         # Kernel headers
│   ├── lib/            # Library headers
│   └── mm/             # Memory management headers
├── kernel/
│   ├── kernel.c        # Main kernel entry
│   ├── init.c          # Kernel initialization
│   ├── process.c       # Process management
│   ├── syscall.c       # System call handlers
│   ├── sched.c         # O(1) scheduler
│   ├── sched_hyper.c   # Scheduler Hypervisor
│   ├── shell.c         # Built-in kernel shell
│   └── elf.c           # ELF loader
├── mm/
│   └── advanced_mm.c   # Advanced memory management
├── lib/
│   ├── printk.c        # Kernel printing
│   └── string.c        # String functions
├── Makefile
├── linker.ld           # Kernel linker script
└── grub.cfg            # GRUB configuration
```

## Boot Output

```
================================================
  Picomimi-x64 Kernel 1.0.0-picomimi-x64
  A Linux-compatible x86_64 kernel
  Inspired by Picomimi-AxisOS for RP2040/RP2350
================================================

[INIT] Scheduler Hypervisor...
  Registered scheduler classes:
    - cooperative (fibers/coroutines)
    - realtime (EDF deadline)
    - fair (CFS-like)
    - batch (throughput)
    - idle (background)
  RT: Domain 'realtime' initialized (deadline scheduler)
  FAIR: Domain 'normal' initialized (CFS-like)
  BATCH: Domain 'batch' initialized
  IDLE: Domain 'idle' initialized
  Scheduler Hypervisor initialized!

[INIT] Terminal emulator...
Terminal initialized (80x25, 1000 lines scrollback)

[INIT] PCI bus...
PCI: Found 6 devices:
  00:03.0 [8086:100e] Network (02:00)
  00:02.0 [1234:1111] Display (03:00)
  ...

[INIT] Network drivers...
e1000: MAC 52:54:00:12:34:56
e1000: Device initialized successfully

[INIT] Starting kernel shell...
╔══════════════════════════════════════════════════════════════╗
║     Picomimi-x64 Kernel v1.0.0                               ║
║     A Linux-compatible x86_64 kernel                         ║
║     Inspired by Picomimi-AxisOS for RP2040/RP2350            ║
║                                                              ║
║     Type 'help' for available commands                       ║
╚══════════════════════════════════════════════════════════════╝

picomimi:/# 
```

## Comparison with Picomimi-AxisOS

| Feature | Picomimi (ARM-32) | Picomimi-x64 |
|---------|--------------------------|--------------|
| Architecture | ARM Cortex-M | x86_64 |
| Word Size | 32-bit | 64-bit |
| Memory Model | Flat | Virtual (4-level paging) |
| Scheduler | O(1) Bitmap + Hypervisor | O(1) Bitmap + Hypervisor |
| Scheduler Domains | Piority Cooperative | RT, Fair, Batch, Idle, Coop |
| Syscalls | Custom Picomimi | Linux ABI (512 syscalls) |
| Cores | 2, SMP (expansable) | SMP-ready |
| Max RAM | 512KB (PSRAM expandable) | x64 limit |
| Boot | MimiBoot/Direct | GRUB/Multiboot2 |
| Terminal | Basic | Full ANSI/VT100 |
| Shell | MimiShell | Built-in (ksh) |

## Future Plans

- [x] Scheduler Hypervisor with pluggable classes
- [x] Terminal emulator with ANSI colors
- [x] Built-in kernel shell
- [ ] Full SMP support (AP startup)
- [ ] Complete e1000 RX/TX with network stack
- [ ] TCP/IP stack
- [ ] More filesystems (ext2, FAT32)
- [ ] ATA/AHCI storage driver
- [ ] User-space init process
- [ ] Port busybox/toybox
- [ ] POSIX compliance testing
- [ ] Copy-on-Write fork
- [ ] Demand paging

## License

MIT License - See LICENSE file

## Acknowledgments

- Picomimi-AxisOS for the O(1) scheduler design inspiration
- OSDev Wiki for x86_64 documentation
- Linux kernel for syscall ABI reference
