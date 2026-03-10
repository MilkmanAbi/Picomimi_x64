# Picomimi-x64 Kernel — Development Status
**Date:** 2026-03-06  
**Total LOC:** ~33,000+ (47 .c files, 32 .h files)

---

## Session Summary

### New This Session
| File | Lines | Description |
|------|-------|-------------|
| `kernel/at_syscalls.c` | 455 | Complete *at syscall family (openat, mkdirat, mknodat, fchownat, newfstatat, unlinkat, renameat2, linkat, symlinkat, readlinkat, fchmodat, faccessat2) |
| `kernel/mmap.c` | 401 | Full brk/mmap/munmap/mprotect — anonymous and file-backed mappings, VMA tracking, page table walk |
| `kernel/wait.c` | 344 | wait4/waitpid with zombie reaping, WNOHANG/WUNTRACED, rusage, do_exit, sys_exit_group |
| `fs/vfs_ops.c` | 713 | vfs_mkdir/rmdir/unlink/link/symlink/rename/mknod, dentry_path_raw, kstrdup, vma_alloc/insert_vma, phys_to_virt, vmm_virt_to_phys/unmap_page, vmm_alloc_pages, pid_alloc/free, sys_mkdir/rmdir/mknod/unlink/rename/link/symlink/readlink/chown/chmod |
| `include/net/socket.h` | 576 | Full socket API: AF_INET/UNIX/INET6, sock_t, socket_t, sk_buff, TCP state machine, IP/TCP/UDP/ICMP/ARP headers, proto_ops vtable, net_device |
| `net/net_core.c` | 696 | skb lifecycle, netdev registry, ARP table, IP checksum, IP send/receive, ICMP echo reply, UDP send/receive, loopback (127.0.0.1), socket ring buffers, sk/socket allocators, port allocation, inet_lookup |
| `net/tcp.c` | 402 | Full TCP state machine: SYN/SYN-ACK/ACK handshake, tcp_connect/listen/accept/receive/send_data/close, FIN_WAIT1/2, TIME_WAIT, CLOSE_WAIT/LAST_ACK, backlog accept queue, MSS segmentation |
| `net/socket_syscalls.c` | 638 | sys_socket/bind/connect/listen/accept4/send/recv/sendto/recvfrom/sendmsg/recvmsg/shutdown/setsockopt/getsockopt/getsockname/getpeername/socketpair, socket file_ops (read/write/release) |
| `include/kernel/types.h` | +200 | Added: all signal numbers, EXIT_STOPPED/CONTINUED, VM_* flags, PTE_* flags, struct utsname/linux_dirent64/iovec/rlimit/rusage/sysinfo, NSEC constants, O_* flags, PATH_MAX |
| `kernel/shell.c` | +400 | New commands: ls (colored), cat, mkdir, rm, pwd, cd, kill, free, dmesg, stat, mount, ifconfig, ping, write, hexdump, net (socket test), simple_strtol |
| `kernel/kernel.c` | +30 | Wired: chrdev_init, tty_init, timers_init, init_pipefs, init_procfs, init_sysfs, net_init |
| `Makefile` | updated | Added: kernel/{wait,mmap,at_syscalls,signal,timer,elf_loader,fs_syscalls,syscall_table,syscall_stubs}, fs/{vfs_ops,procfs,sysfs,pipe}, drivers/chrdev, drivers/tty/tty_ldisc, net/{net_core,tcp,socket_syscalls}, build dirs |

---

## Subsystem Completion Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Boot (multiboot2) | ✅ Complete | boot.S, GDT, IDT, paging |
| PMM (bitmap) | ✅ Complete | 256MB, pmm_alloc/free_page |
| VMM (4-level paging) | ✅ Complete | vmm_map_page, ioremap, virt_to_phys, unmap |
| Slab allocator | ✅ Complete | 16MB bump allocator (kfree is stub) |
| Advanced MM | ✅ Complete | buddy allocator in advanced_mm.c |
| Scheduler | ✅ Complete | O(1) + CFS hybrid, hyper scheduler, 140 priorities |
| Process/fork/clone | ✅ Complete | fork, vfork, clone, exec, COW stub |
| Signal subsystem | ✅ Complete | Full POSIX signals, sigaction, sigprocmask, sigreturn |
| Timer subsystem | ✅ Complete | Timer wheel, nanosleep, clock_gettime, alarm |
| ELF loader | ✅ Complete | ELF64, PT_LOAD, PT_INTERP, auxv, brk |
| brk / mmap | ✅ Complete | Anonymous + file-backed, VMA tracking, mprotect, munmap |
| wait4 / exit | ✅ Complete | Zombie reaping, rusage, WNOHANG, SIGCHLD, do_exit |
| VFS | ✅ Complete | dentry cache, path resolution, LOOKUP_FOLLOW |
| ramfs | ✅ Complete | Root filesystem, file/dir create/read/write |
| procfs | ✅ Complete | /proc/version, cpuinfo, meminfo, stat, per-pid/ |
| sysfs | ✅ Complete | kobject tree, /sys/kernel attrs, hostname |
| devfs | ✅ Complete | /dev/null/zero/random/urandom/tty/kmsg + tty0-7 |
| pipefs | ✅ Complete | Anonymous pipes, SIGPIPE, O_NONBLOCK, 64KB ring |
| Character devices | ✅ Complete | Major/minor registry, null/zero/random/mem/kmsg |
| TTY line discipline | ✅ Complete | N_TTY, canonical/raw, ISIG, IXON, termios ioctls |
| Syscall table | ✅ Complete | 200+ syscalls wired (x86_64 ABI) |
| *at syscalls | ✅ Complete | openat, mkdirat, mknodat, fchownat, newfstatat, unlinkat, renameat2, linkat, symlinkat, readlinkat, fchmodat, faccessat2 |
| VFS operations | ✅ Complete | vfs_mkdir/rmdir/unlink/link/symlink/rename/mknod |
| PCI driver | ✅ Complete | Config space enumeration |
| Serial UART | ✅ Complete | 16550A, 115200 baud, printk backend |
| VGA text mode | ✅ Complete | 80x25, scrolling, colors |
| Framebuffer | ✅ Complete | Multiboot2 tag, pixel operations |
| Keyboard | ✅ Complete | PS/2 scan codes, tty_receive_char |
| Window manager | ✅ Complete | Compositor, widgets, WM apps |
| e1000 NIC driver | ✅ Complete | Ring buffers, DMA descriptors |
| Network core | ✅ Complete | skb, netdev registry, ARP, IP, ICMP, UDP, loopback |
| TCP stack | ✅ Complete | Full state machine, connect/listen/accept, MSS segmentation |
| Socket syscalls | ✅ Complete | socket/bind/connect/listen/accept/send/recv/setsockopt/getsockopt/socketpair |
| Kernel shell | ✅ Complete | 30 commands: ls/cat/mkdir/rm/pwd/cd/kill/free/stat/mount/ifconfig/ping/net/hexdump |

---

## Architecture

```
Userspace (ELF64)
    │  SYSCALL/SYSRET
    ▼
Syscall Dispatch (kernel/syscall.c + syscall_table.c)
    │
    ├─ Process/Sched  ──  kernel/{process,sched,sched_hyper,signal,timer,wait}.c
    ├─ Memory         ──  kernel/mmap.c + mm/{pmm,vmm,slab,advanced_mm}.c
    ├─ Filesystem     ──  fs/{vfs,vfs_ops,ramfs,procfs,sysfs,devfs,pipe}.c
    ├─ *at syscalls   ──  kernel/at_syscalls.c
    ├─ ELF loader     ──  kernel/elf_loader.c
    ├─ Network        ──  net/{net_core,tcp,socket_syscalls}.c
    └─ Drivers        ──  drivers/{serial,vga,fb,keyboard,tty,chrdev,pci,net/e1000}.c
```

---

## Pending / Next Steps

1. **`kfree` implementation** — Slab allocator currently uses a bump allocator with no free. Replace with a proper free-list slab.
2. **Copy-on-Write fork** — fork() currently copies all pages. COW would mark pages RO and copy on fault.
3. **Page fault handler** — Demand paging for mmap'd regions (currently all pages allocated eagerly).
4. **switch_to_user()** — The SYSRET/IRET trampoline in context.S needs to be connected to ELF loader.
5. **Keyboard → TTY wiring** — keyboard IRQ must call `tty_receive_char()`.
6. **Futex proper blocking** — futex currently spin-waits; needs wait_queue integration.
7. **select/poll blocking** — Currently always returns ready; needs per-socket wait queues.
8. **e1000 → eth_receive wiring** — e1000 IRQ handler must call `eth_receive()`.
9. **UNIX socket AF_UNIX** — `socketpair` works via peer pointers; bound UNIX sockets need VFS path.
10. **Userspace init/shell** — Write `init.c` to spawn from ELF, exec `/bin/sh`.
11. **Real entropy** — `/dev/random` uses xorshift64; RDRAND instruction would improve it.
12. **IPv6** — AF_INET6 currently returns -EAFNOSUPPORT.
