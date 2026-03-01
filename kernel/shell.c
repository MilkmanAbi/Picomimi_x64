/**
 * Picomimi-x64 Built-in Shell (ksh)
 * 
 * A simple shell built into the kernel for testing and debugging.
 * This runs in kernel mode but demonstrates process/command concepts.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/io.h>

// ============================================================================
// SHELL CONFIGURATION
// ============================================================================

#define KSH_MAX_LINE        256
#define KSH_MAX_ARGS        16
#define KSH_HISTORY_SIZE    32
#define KSH_PROMPT          "picomimi:/# "

// ============================================================================
// KEYBOARD INPUT
// ============================================================================

// Scancode to ASCII mapping (US keyboard layout)
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_to_ascii_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

static int shift_pressed = 0;
static int ctrl_pressed = 0;

// ============================================================================
// SHELL STATE
// ============================================================================

static char line_buffer[KSH_MAX_LINE];
static int line_pos = 0;
static char *history[KSH_HISTORY_SIZE];
static int history_count = 0;
static int history_pos = 0;

// ============================================================================
// TERMINAL OUTPUT HELPERS
// ============================================================================

static void ksh_putchar(char c) {
    char buf[2] = {c, 0};
    printk("%s", buf);
}

static void ksh_puts(const char *s) {
    printk("%s", s);
}

static void ksh_printf(const char *fmt, ...) __attribute__((unused));
static void ksh_printf(const char *fmt, ...) {
    // Simple printf - just use printk
    printk("%s", fmt);
}

static void ksh_clear_line(void) {
    // Move to start of line and print spaces
    ksh_puts("\r");
    for (int i = 0; i < 79; i++) ksh_putchar(' ');
    ksh_puts("\r");
}

static void ksh_redraw_line(void) {
    ksh_clear_line();
    ksh_puts(KSH_PROMPT);
    ksh_puts(line_buffer);
}

// ============================================================================
// COMMAND: help
// ============================================================================

static void cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\n");
    ksh_puts("=== Picomimi-x64 Kernel Shell ===\n\n");
    ksh_puts("Available commands:\n");
    ksh_puts("  help           - Show this help\n");
    ksh_puts("  clear          - Clear screen\n");
    ksh_puts("  uname          - Show system information\n");
    ksh_puts("  uptime         - Show system uptime\n");
    ksh_puts("  meminfo        - Show memory information\n");
    ksh_puts("  cpuinfo        - Show CPU information\n");
    ksh_puts("  ps             - List processes\n");
    ksh_puts("  lspci          - List PCI devices\n");
    ksh_puts("  lsdev          - List /dev entries\n");
    ksh_puts("  sched          - Show scheduler info\n");
    ksh_puts("  domains        - Show scheduler domains\n");
    ksh_puts("  test <name>    - Run kernel test\n");
    ksh_puts("  echo <text>    - Echo text\n");
    ksh_puts("  color          - Show color test\n");
    ksh_puts("  reboot         - Reboot system\n");
    ksh_puts("  halt           - Halt system\n");
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: clear
// ============================================================================

static void cmd_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Clear screen using VGA driver directly
    extern void vga_clear(void);
    vga_clear();
}

// ============================================================================
// COMMAND: uname
// ============================================================================

static void cmd_uname(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("Picomimi-x64 ");
    ksh_puts("picomimi ");
    ksh_puts("1.0.0-picomimi-x64 ");
    ksh_puts("#1 SMP PREEMPT ");
    ksh_puts("x86_64 ");
    ksh_puts("GNU/Picomimi\n");
}

// ============================================================================
// COMMAND: uptime
// ============================================================================

extern volatile u64 system_ticks;

static void cmd_uptime(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    u64 ticks = system_ticks;
    u64 seconds = ticks / 100;  // 100 Hz timer
    u64 minutes = seconds / 60;
    u64 hours = minutes / 60;
    u64 days = hours / 24;
    
    ksh_puts(" up ");
    
    if (days > 0) {
        printk("%lu day%s, ", days, days > 1 ? "s" : "");
    }
    
    printk("%02lu:%02lu:%02lu\n", hours % 24, minutes % 60, seconds % 60);
}

// ============================================================================
// COMMAND: meminfo
// ============================================================================

extern u64 pmm_get_total_memory(void);
extern u64 pmm_get_free_memory(void);

static void cmd_meminfo(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    u64 total_kb = pmm_get_total_memory() / 1024;
    u64 free_kb = pmm_get_free_memory() / 1024;
    
    ksh_puts("\nMemory Information:\n");
    printk("  Total:     %lu MB\n", total_kb / 1024);
    printk("  Free:      %lu MB\n", free_kb / 1024);
    printk("  Used:      %lu MB\n", (total_kb - free_kb) / 1024);
    if (total_kb > 0) {
        printk("  Usage:     %lu%%\n", ((total_kb - free_kb) * 100) / total_kb);
    }
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: cpuinfo
// ============================================================================

static void cmd_cpuinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Read CPUID
    u32 eax, ebx, ecx, edx;
    char vendor[13] = {0};
    
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(u32 *)&vendor[0] = ebx;
    *(u32 *)&vendor[4] = edx;
    *(u32 *)&vendor[8] = ecx;
    
    ksh_puts("\nCPU Information:\n");
    printk("  Vendor:    %s\n", vendor);
    
    // Get processor brand string
    char brand[49] = {0};
    __asm__ volatile("cpuid" : "=a"(eax) : "a"(0x80000000));
    
    if (eax >= 0x80000004) {
        u32 *b = (u32 *)brand;
        __asm__ volatile("cpuid" : "=a"(b[0]), "=b"(b[1]), "=c"(b[2]), "=d"(b[3]) : "a"(0x80000002));
        __asm__ volatile("cpuid" : "=a"(b[4]), "=b"(b[5]), "=c"(b[6]), "=d"(b[7]) : "a"(0x80000003));
        __asm__ volatile("cpuid" : "=a"(b[8]), "=b"(b[9]), "=c"(b[10]), "=d"(b[11]) : "a"(0x80000004));
        printk("  Model:     %s\n", brand);
    }
    
    // Get features
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    ksh_puts("  Features:  ");
    if (edx & (1 << 0)) ksh_puts("fpu ");
    if (edx & (1 << 4)) ksh_puts("tsc ");
    if (edx & (1 << 5)) ksh_puts("msr ");
    if (edx & (1 << 6)) ksh_puts("pae ");
    if (edx & (1 << 9)) ksh_puts("apic ");
    if (edx & (1 << 15)) ksh_puts("cmov ");
    if (edx & (1 << 23)) ksh_puts("mmx ");
    if (edx & (1 << 25)) ksh_puts("sse ");
    if (edx & (1 << 26)) ksh_puts("sse2 ");
    if (ecx & (1 << 0)) ksh_puts("sse3 ");
    if (ecx & (1 << 9)) ksh_puts("ssse3 ");
    if (ecx & (1 << 19)) ksh_puts("sse4.1 ");
    if (ecx & (1 << 20)) ksh_puts("sse4.2 ");
    if (ecx & (1 << 28)) ksh_puts("avx ");
    ksh_puts("\n\n");
}

// ============================================================================
// COMMAND: ps
// ============================================================================

extern task_struct_t *task_table[];
extern spinlock_t task_table_lock;

static const char *task_state_names[] = {
    "R", "S", "D", "T", "Z", "X"
};

static void cmd_ps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\n  PID TTY      STAT   TIME COMMAND\n");
    
    extern void spin_lock(spinlock_t *lock);
    extern void spin_unlock(spinlock_t *lock);
    
    spin_lock(&task_table_lock);
    
    for (int i = 0; i < MAX_THREADS; i++) {
        task_struct_t *task = task_table[i];
        if (task) {
            const char *state = task->state < 6 ? task_state_names[task->state] : "?";
            printk("%5d tty0     %s      0:00 %s\n", 
                   task->pid, state, task->comm);
        }
    }
    
    spin_unlock(&task_table_lock);
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: lspci
// ============================================================================

// PCI info from pci.c
extern int pci_get_device_count(void);
extern void pci_list_devices(void);

static void cmd_lspci(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\nPCI Devices:\n");
    pci_list_devices();
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: lsdev
// ============================================================================

static void cmd_lsdev(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\n/dev entries:\n");
    ksh_puts("  null     (1:3)   - Null device\n");
    ksh_puts("  zero     (1:5)   - Zero device\n");
    ksh_puts("  full     (1:7)   - Full device\n");
    ksh_puts("  random   (1:8)   - Random device\n");
    ksh_puts("  urandom  (1:9)   - Urandom device\n");
    ksh_puts("  tty      (5:0)   - TTY device\n");
    ksh_puts("  console  (5:1)   - Console device\n");
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: sched
// ============================================================================

extern void sched_hypervisor_stats(void);

static void cmd_sched(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    sched_hypervisor_stats();
}

// ============================================================================
// COMMAND: domains
// ============================================================================

static void cmd_domains(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\nScheduler Domains:\n");
    ksh_puts("  ID  Name       Class        Priority  Running\n");
    ksh_puts("  --  ----       -----        --------  -------\n");
    ksh_puts("   0  realtime   EDF            0         0\n");
    ksh_puts("   1  normal     CFS          100         1\n");
    ksh_puts("   2  batch      batch        120         0\n");
    ksh_puts("   3  idle       idle         140         1\n");
    ksh_puts("\n");
}

// ============================================================================
// COMMAND: test
// ============================================================================

static void test_colors(void) {
    ksh_puts("\nColor Test:\n");
    ksh_puts("  (ANSI colors disabled in this build)\n");
    ksh_puts("\n");
}

static void test_scheduler(void) {
    ksh_puts("\nScheduler Test:\n");
    ksh_puts("  Creating test fiber domain...\n");
    
    extern void *sched_create_fiber_domain(const char *name);
    void *domain = sched_create_fiber_domain("test-fibers");
    
    if (domain) {
        ksh_puts("  Success! Fiber domain created\n");
    } else {
        ksh_puts("  Failed! Could not create domain\n");
    }
    ksh_puts("\n");
}

static void test_syscall(void) {
    ksh_puts("\nSyscall Test:\n");
    
    // Test getpid via syscall
    long pid;
    __asm__ volatile(
        "mov $39, %%rax\n"  // __NR_getpid
        "syscall\n"
        : "=a"(pid)
        :
        : "rcx", "r11", "memory"
    );
    
    printk("  getpid() = %ld\n", pid);
    
    // Test uname
    struct {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    } uts = {0};
    
    long ret;
    __asm__ volatile(
        "mov $63, %%rax\n"  // __NR_uname
        "mov %1, %%rdi\n"
        "syscall\n"
        : "=a"(ret)
        : "r"(&uts)
        : "rdi", "rcx", "r11", "memory"
    );
    
    printk("  uname() = %ld\n", ret);
    printk("    sysname:  %s\n", uts.sysname);
    printk("    nodename: %s\n", uts.nodename);
    printk("    release:  %s\n", uts.release);
    printk("    machine:  %s\n", uts.machine);
    
    ksh_puts("\n");
}

static void cmd_test(int argc, char **argv) {
    if (argc < 2) {
        ksh_puts("Usage: test <name>\n");
        ksh_puts("  colors    - Test terminal colors\n");
        ksh_puts("  sched     - Test scheduler hypervisor\n");
        ksh_puts("  syscall   - Test system calls\n");
        return;
    }
    
    if (strcmp(argv[1], "colors") == 0) {
        test_colors();
    } else if (strcmp(argv[1], "sched") == 0) {
        test_scheduler();
    } else if (strcmp(argv[1], "syscall") == 0) {
        test_syscall();
    } else {
        printk("Unknown test: %s\n", argv[1]);
    }
}

// ============================================================================
// COMMAND: echo
// ============================================================================

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) ksh_putchar(' ');
        ksh_puts(argv[i]);
    }
    ksh_putchar('\n');
}

// ============================================================================
// COMMAND: color
// ============================================================================

static void cmd_color(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\n+------------------------------------------+\n");
    ksh_puts("|  Picomimi-x64 Kernel                     |\n");
    ksh_puts("|  (Color test disabled in this build)    |\n");
    ksh_puts("+------------------------------------------+\n\n");
}

// ============================================================================
// COMMAND: reboot
// ============================================================================

static void cmd_reboot(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\nRebooting...\n");
    
    // Triple fault to reboot
    __asm__ volatile(
        "cli\n"
        "lidt 0\n"
        "int $3\n"
    );
}

// ============================================================================
// COMMAND: halt
// ============================================================================

static void cmd_halt(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    ksh_puts("\nSystem halted.\n");
    
    __asm__ volatile(
        "cli\n"
        "1: hlt\n"
        "jmp 1b\n"
    );
}

// ============================================================================
// COMMAND TABLE
// ============================================================================

typedef struct {
    const char *name;
    void (*func)(int argc, char **argv);
    const char *desc;
} command_t;

static command_t commands[] = {
    {"help",    cmd_help,    "Show help"},
    {"clear",   cmd_clear,   "Clear screen"},
    {"uname",   cmd_uname,   "System info"},
    {"uptime",  cmd_uptime,  "Show uptime"},
    {"meminfo", cmd_meminfo, "Memory info"},
    {"cpuinfo", cmd_cpuinfo, "CPU info"},
    {"ps",      cmd_ps,      "List processes"},
    {"lspci",   cmd_lspci,   "List PCI devices"},
    {"lsdev",   cmd_lsdev,   "List /dev"},
    {"sched",   cmd_sched,   "Scheduler stats"},
    {"domains", cmd_domains, "Scheduler domains"},
    {"test",    cmd_test,    "Run tests"},
    {"echo",    cmd_echo,    "Echo text"},
    {"color",   cmd_color,   "Color test"},
    {"reboot",  cmd_reboot,  "Reboot system"},
    {"halt",    cmd_halt,    "Halt system"},
    {NULL, NULL, NULL}
};

// ============================================================================
// COMMAND EXECUTION
// ============================================================================

static void ksh_execute(void) {
    // Skip empty lines
    if (line_pos == 0) return;
    
    // Add to history
    if (history_count < KSH_HISTORY_SIZE) {
        history[history_count] = kmalloc(line_pos + 1, GFP_KERNEL);
        if (history[history_count]) {
            strcpy(history[history_count], line_buffer);
            history_count++;
        }
    }
    
    // Parse command
    char *args[KSH_MAX_ARGS];
    int argc = 0;
    
    char *p = line_buffer;
    while (*p && argc < KSH_MAX_ARGS) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        
        args[argc++] = p;
        
        // Find end of argument
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    
    if (argc == 0) return;
    
    // Find command
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(args[0], commands[i].name) == 0) {
            commands[i].func(argc, args);
            return;
        }
    }
    
    printk("%s: command not found\n", args[0]);
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

void ksh_handle_key(u8 scancode) {
    // Handle key release
    if (scancode & 0x80) {
        u8 released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {  // Shift
            shift_pressed = 0;
        } else if (released == 0x1D) {  // Ctrl
            ctrl_pressed = 0;
        }
        return;
    }
    
    // Handle special keys
    switch (scancode) {
    case 0x2A:  // Left Shift
    case 0x36:  // Right Shift
        shift_pressed = 1;
        return;
    case 0x1D:  // Ctrl
        ctrl_pressed = 1;
        return;
    case 0x48:  // Up arrow - history
        if (history_pos < history_count) {
            history_pos++;
            int idx = history_count - history_pos;
            strcpy(line_buffer, history[idx]);
            line_pos = strlen(line_buffer);
            ksh_redraw_line();
        }
        return;
    case 0x50:  // Down arrow - history
        if (history_pos > 0) {
            history_pos--;
            if (history_pos == 0) {
                line_buffer[0] = '\0';
                line_pos = 0;
            } else {
                int idx = history_count - history_pos;
                strcpy(line_buffer, history[idx]);
                line_pos = strlen(line_buffer);
            }
            ksh_redraw_line();
        }
        return;
    case 0x4B:  // Left arrow
        // TODO: cursor movement
        return;
    case 0x4D:  // Right arrow
        // TODO: cursor movement
        return;
    }
    
    // Handle Ctrl+C
    if (ctrl_pressed && scancode == 0x2E) {  // C
        ksh_puts("^C\n");
        line_buffer[0] = '\0';
        line_pos = 0;
        ksh_puts(KSH_PROMPT);
        return;
    }
    
    // Handle Ctrl+L (clear)
    if (ctrl_pressed && scancode == 0x26) {  // L
        cmd_clear(0, NULL);
        ksh_puts(KSH_PROMPT);
        ksh_puts(line_buffer);
        return;
    }
    
    // Convert scancode to ASCII
    if (scancode >= sizeof(scancode_to_ascii)) return;
    
    char c = shift_pressed ? scancode_to_ascii_shift[scancode] 
                           : scancode_to_ascii[scancode];
    
    if (c == 0) return;
    
    // Handle special characters
    if (c == '\n') {
        ksh_putchar('\n');
        line_buffer[line_pos] = '\0';
        ksh_execute();
        line_buffer[0] = '\0';
        line_pos = 0;
        history_pos = 0;
        ksh_puts(KSH_PROMPT);
        return;
    }
    
    if (c == '\b') {
        if (line_pos > 0) {
            line_pos--;
            line_buffer[line_pos] = '\0';
            ksh_puts("\b \b");
        }
        return;
    }
    
    if (c == '\t') {
        // TODO: Tab completion
        return;
    }
    
    // Regular character
    if (line_pos < KSH_MAX_LINE - 1) {
        line_buffer[line_pos++] = c;
        line_buffer[line_pos] = '\0';
        ksh_putchar(c);
    }
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void ksh_init(void) {
    // Clear buffer
    line_buffer[0] = '\0';
    line_pos = 0;
    
    // Print simple banner
    ksh_puts("\n");
    ksh_puts("+--------------------------------------------------------------+\n");
    ksh_puts("|     ____  _                      _           _               |\n");
    ksh_puts("|    |  _ \\(_) ___ ___  _ __ ___ (_)_ __ ___ (_)              |\n");
    ksh_puts("|    | |_) | |/ __/ _ \\| '_ ` _ \\| | '_ ` _ \\| |              |\n");
    ksh_puts("|    |  __/| | (_| (_) | | | | | | | | | | | | |              |\n");
    ksh_puts("|    |_|   |_|\\___\\___/|_| |_| |_|_|_| |_| |_|_|              |\n");
    ksh_puts("|                                                              |\n");
    ksh_puts("|    Picomimi-x64 Kernel v1.0.0                                |\n");
    ksh_puts("|    A Linux-compatible x86_64 kernel                          |\n");
    ksh_puts("|    Type 'help' for available commands                        |\n");
    ksh_puts("+--------------------------------------------------------------+\n");
    ksh_puts("\n");
    
    ksh_puts("picomimi:/# ");
    
    printk(KERN_INFO "KSH: Kernel shell initialized\n");
}
