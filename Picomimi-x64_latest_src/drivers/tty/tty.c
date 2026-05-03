/**
 * Picomimi-x64 TTY Subsystem
 * 
 * Terminal device abstraction layer
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>

// ============================================================================
// TTY CONSTANTS
// ============================================================================

#define TTY_BUF_SIZE        4096
#define TTY_MAX_TTYS        64
#define TTY_NAME_LEN        32

// Input/Output modes
#define ICANON              0x0002  // Canonical mode (line buffered)
#define ECHO                0x0008  // Echo input
#define ECHOE               0x0010  // Echo erase as backspace
#define ECHOK               0x0020  // Echo newline after kill
#define ECHONL              0x0040  // Echo newline
#define ISIG                0x0001  // Enable signals
#define IEXTEN              0x8000  // Extended input processing

// Output modes
#define OPOST               0x0001  // Post-process output
#define ONLCR               0x0004  // Map NL to CR-NL

// Control modes
#define CREAD               0x0080  // Enable receiver
#define CS8                 0x0030  // 8-bit chars

// Local modes
#define TOSTOP              0x0100  // Stop background jobs

// Special characters
#define VINTR       0   // Interrupt (^C)
#define VQUIT       1   // Quit (^\)
#define VERASE      2   // Erase (backspace)
#define VKILL       3   // Kill line (^U)
#define VEOF        4   // End of file (^D)
#define VTIME       5   // Timeout
#define VMIN        6   // Minimum chars
#define VSTART      8   // Start output (^Q)
#define VSTOP       9   // Stop output (^S)
#define VSUSP       10  // Suspend (^Z)
#define VEOL        11  // End of line
#define VREPRINT    12  // Reprint (^R)
#define VDISCARD    13  // Discard (^O)
#define VWERASE     14  // Word erase (^W)
#define VLNEXT      15  // Literal next (^V)
#define VEOL2       16  // Second EOL
#define NCCS        17  // Number of control chars

// ============================================================================
// TTY STRUCTURES
// ============================================================================

// Terminal I/O settings
typedef struct termios {
    u32     c_iflag;        // Input flags
    u32     c_oflag;        // Output flags
    u32     c_cflag;        // Control flags
    u32     c_lflag;        // Local flags
    u8      c_line;         // Line discipline
    u8      c_cc[NCCS];     // Control characters
    u32     c_ispeed;       // Input speed
    u32     c_ospeed;       // Output speed
} termios_t;

// Window size
typedef struct winsize {
    u16     ws_row;
    u16     ws_col;
    u16     ws_xpixel;
    u16     ws_ypixel;
} winsize_t;

// Ring buffer for TTY
typedef struct tty_buffer {
    u8      data[TTY_BUF_SIZE];
    size_t  head;
    size_t  tail;
    size_t  count;
} tty_buffer_t;

// TTY device structure
typedef struct tty_struct {
    int             index;              // TTY number
    char            name[TTY_NAME_LEN]; // Device name
    
    // Terminal settings
    termios_t       termios;
    winsize_t       winsize;
    
    // Buffers
    tty_buffer_t    read_buf;           // Input buffer
    tty_buffer_t    write_buf;          // Output buffer
    tty_buffer_t    canon_buf;          // Canonical mode buffer
    
    // Line buffer for canonical mode
    u8              line_buf[TTY_BUF_SIZE];
    size_t          line_pos;
    
    // Session/process group
    pid_t           session;            // Controlling session
    pid_t           pgrp;               // Foreground process group
    
    // Driver operations
    const struct tty_operations *ops;
    void            *driver_data;
    
    // State
    int             count;              // Open count
    int             stopped;            // Output stopped (^S)
    int             hw_stopped;         // Hardware flow control
    
    // Wait queues
    wait_queue_head_t read_wait;
    wait_queue_head_t write_wait;
    
    spinlock_t      lock;
} tty_struct_t;

// TTY operations
typedef struct tty_operations {
    int (*open)(tty_struct_t *tty);
    void (*close)(tty_struct_t *tty);
    int (*write)(tty_struct_t *tty, const u8 *buf, size_t count);
    int (*write_room)(tty_struct_t *tty);
    void (*flush_buffer)(tty_struct_t *tty);
    int (*ioctl)(tty_struct_t *tty, u32 cmd, unsigned long arg);
    void (*set_termios)(tty_struct_t *tty, termios_t *old);
    void (*throttle)(tty_struct_t *tty);
    void (*unthrottle)(tty_struct_t *tty);
    void (*stop)(tty_struct_t *tty);
    void (*start)(tty_struct_t *tty);
    void (*hangup)(tty_struct_t *tty);
} tty_operations_t;

// ============================================================================
// GLOBAL STATE
// ============================================================================

static tty_struct_t *tty_table[TTY_MAX_TTYS];
static spinlock_t tty_table_lock __attribute__((unused)) = { .raw_lock = { 0 } };

// Default terminal settings
static const termios_t default_termios = {
    .c_iflag = 0,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_line = 0,
    .c_cc = {
        [VINTR]     = 0x03,     // ^C
        [VQUIT]     = 0x1c,     // backslash
        [VERASE]    = 0x7f,     // DEL
        [VKILL]     = 0x15,     // ^U
        [VEOF]      = 0x04,     // ^D
        [VTIME]     = 0,
        [VMIN]      = 1,
        [VSTART]    = 0x11,     // ^Q
        [VSTOP]     = 0x13,     // ^S
        [VSUSP]     = 0x1a,     // ^Z
        [VEOL]      = 0,
        [VREPRINT]  = 0x12,     // ^R
        [VDISCARD]  = 0x0f,     // ^O
        [VWERASE]   = 0x17,     // ^W
        [VLNEXT]    = 0x16,     // ^V
        [VEOL2]     = 0,
    },
    .c_ispeed = 38400,
    .c_ospeed = 38400,
};

// ============================================================================
// BUFFER OPERATIONS
// ============================================================================

static inline void spin_lock_impl(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->raw_lock.lock, 1, __ATOMIC_ACQUIRE)) {
        while (lock->raw_lock.lock) {
            __asm__ volatile("pause");
        }
    }
}

static inline void spin_unlock_impl(spinlock_t *lock) {
    __atomic_store_n(&lock->raw_lock.lock, 0, __ATOMIC_RELEASE);
}

static void buf_init(tty_buffer_t *buf) {
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
}

static int buf_empty(tty_buffer_t *buf) {
    return buf->count == 0;
}

static int buf_full(tty_buffer_t *buf) {
    return buf->count == TTY_BUF_SIZE;
}

static int buf_put(tty_buffer_t *buf, u8 c) {
    if (buf_full(buf)) return -1;
    buf->data[buf->head] = c;
    buf->head = (buf->head + 1) % TTY_BUF_SIZE;
    buf->count++;
    return 0;
}

static int buf_get(tty_buffer_t *buf, u8 *c) {
    if (buf_empty(buf)) return -1;
    *c = buf->data[buf->tail];
    buf->tail = (buf->tail + 1) % TTY_BUF_SIZE;
    buf->count--;
    return 0;
}

// ============================================================================
// TTY ALLOCATION
// ============================================================================

tty_struct_t *tty_alloc(int index) {
    tty_struct_t *tty = kmalloc(sizeof(tty_struct_t), GFP_KERNEL);
    if (!tty) return NULL;
    
    memset(tty, 0, sizeof(tty_struct_t));
    
    tty->index = index;
    snprintf(tty->name, TTY_NAME_LEN, "tty%d", index);
    
    // Default settings
    tty->termios = default_termios;
    tty->winsize.ws_row = 25;
    tty->winsize.ws_col = 80;
    
    // Initialize buffers
    buf_init(&tty->read_buf);
    buf_init(&tty->write_buf);
    buf_init(&tty->canon_buf);
    tty->line_pos = 0;
    
    // Initialize lock
    tty->lock.raw_lock.lock = 0;
    
    // Initialize wait queues
    INIT_LIST_HEAD(&tty->read_wait.head);
    INIT_LIST_HEAD(&tty->write_wait.head);
    tty->read_wait.lock.raw_lock.lock = 0;
    tty->write_wait.lock.raw_lock.lock = 0;
    
    return tty;
}

void tty_free(tty_struct_t *tty) {
    if (tty) {
        kfree(tty);
    }
}

// ============================================================================
// CHARACTER PROCESSING
// ============================================================================

// Process special characters in cooked mode
static int tty_handle_special(tty_struct_t *tty, u8 c) {
    termios_t *t = &tty->termios;
    
    if (!(t->c_lflag & ISIG)) {
        return 0;  // Signals disabled
    }
    
    if (c == t->c_cc[VINTR]) {
        // Send SIGINT to foreground process group
        if (tty->pgrp) {
            // send_signal(-tty->pgrp, SIGINT);
        }
        return 1;
    }
    
    if (c == t->c_cc[VQUIT]) {
        // Send SIGQUIT
        if (tty->pgrp) {
            // send_signal(-tty->pgrp, SIGQUIT);
        }
        return 1;
    }
    
    if (c == t->c_cc[VSUSP]) {
        // Send SIGTSTP
        if (tty->pgrp) {
            // send_signal(-tty->pgrp, SIGTSTP);
        }
        return 1;
    }
    
    return 0;
}

// Echo character to output
static void tty_echo(tty_struct_t *tty, u8 c) {
    if (!(tty->termios.c_lflag & ECHO)) {
        return;
    }
    
    if (c < 32 && c != '\n' && c != '\t' && c != '\r') {
        // Echo control chars as ^X
        if (tty->ops && tty->ops->write) {
            u8 buf[2] = { '^', c + '@' };
            tty->ops->write(tty, buf, 2);
        }
    } else {
        if (tty->ops && tty->ops->write) {
            tty->ops->write(tty, &c, 1);
        }
    }
}

// Process input character
void tty_receive_char(tty_struct_t *tty, u8 c) {
    spin_lock_impl(&tty->lock);
    
    // Handle special characters
    if (tty_handle_special(tty, c)) {
        spin_unlock_impl(&tty->lock);
        return;
    }
    
    // Handle flow control
    if (c == tty->termios.c_cc[VSTOP]) {
        tty->stopped = 1;
        spin_unlock_impl(&tty->lock);
        return;
    }
    if (c == tty->termios.c_cc[VSTART]) {
        tty->stopped = 0;
        // Wake up writers
        spin_unlock_impl(&tty->lock);
        return;
    }
    
    // Canonical mode processing
    if (tty->termios.c_lflag & ICANON) {
        // Handle erase
        if (c == tty->termios.c_cc[VERASE]) {
            if (tty->line_pos > 0) {
                tty->line_pos--;
                if (tty->termios.c_lflag & ECHOE) {
                    // Echo backspace-space-backspace
                    if (tty->ops && tty->ops->write) {
                        u8 bs[] = { '\b', ' ', '\b' };
                        tty->ops->write(tty, bs, 3);
                    }
                }
            }
            spin_unlock_impl(&tty->lock);
            return;
        }
        
        // Handle kill
        if (c == tty->termios.c_cc[VKILL]) {
            while (tty->line_pos > 0) {
                tty->line_pos--;
                if (tty->termios.c_lflag & ECHOK) {
                    if (tty->ops && tty->ops->write) {
                        u8 bs[] = { '\b', ' ', '\b' };
                        tty->ops->write(tty, bs, 3);
                    }
                }
            }
            spin_unlock_impl(&tty->lock);
            return;
        }
        
        // Add to line buffer
        if (tty->line_pos < TTY_BUF_SIZE - 1) {
            tty->line_buf[tty->line_pos++] = c;
        }
        
        // Echo
        tty_echo(tty, c);
        
        // Check for line completion
        if (c == '\n' || c == tty->termios.c_cc[VEOL] || 
            c == tty->termios.c_cc[VEOF]) {
            // Move line to read buffer
            for (size_t i = 0; i < tty->line_pos; i++) {
                if (tty->line_buf[i] != tty->termios.c_cc[VEOF]) {
                    buf_put(&tty->read_buf, tty->line_buf[i]);
                }
            }
            tty->line_pos = 0;
            
            // Wake up readers
            // wake_up(&tty->read_wait);
        }
    } else {
        // Raw mode - put directly in buffer
        buf_put(&tty->read_buf, c);
        tty_echo(tty, c);
        
        // Wake up readers
        // wake_up(&tty->read_wait);
    }
    
    spin_unlock_impl(&tty->lock);
}

// ============================================================================
// READ/WRITE
// ============================================================================

ssize_t tty_read(tty_struct_t *tty, u8 *buf, size_t count) {
    if (!tty || !buf) return -EINVAL;
    
    size_t read = 0;
    
    spin_lock_impl(&tty->lock);
    
    while (read < count) {
        u8 c;
        if (buf_get(&tty->read_buf, &c) < 0) {
            break;  // No more data
        }
        buf[read++] = c;
        
        // In canonical mode, stop at newline
        if ((tty->termios.c_lflag & ICANON) && c == '\n') {
            break;
        }
    }
    
    spin_unlock_impl(&tty->lock);
    
    // If no data and non-blocking would be set, return -EAGAIN
    // For now, just return what we have
    return read;
}

ssize_t tty_write(tty_struct_t *tty, const u8 *buf, size_t count) {
    if (!tty || !buf) return -EINVAL;
    
    spin_lock_impl(&tty->lock);
    
    // Check if stopped
    while (tty->stopped) {
        spin_unlock_impl(&tty->lock);
        // TODO: Sleep
        spin_lock_impl(&tty->lock);
    }
    
    size_t written = 0;
    
    // Post-process output if OPOST is set
    if (tty->termios.c_oflag & OPOST) {
        for (size_t i = 0; i < count; i++) {
            u8 c = buf[i];
            
            // NL -> CR-NL mapping
            if (c == '\n' && (tty->termios.c_oflag & ONLCR)) {
                if (tty->ops && tty->ops->write) {
                    u8 crnl[] = { '\r', '\n' };
                    tty->ops->write(tty, crnl, 2);
                }
            } else {
                if (tty->ops && tty->ops->write) {
                    tty->ops->write(tty, &c, 1);
                }
            }
            written++;
        }
    } else {
        // Raw output
        if (tty->ops && tty->ops->write) {
            written = tty->ops->write(tty, buf, count);
        }
    }
    
    spin_unlock_impl(&tty->lock);
    return written;
}

// ============================================================================
// IOCTL
// ============================================================================

// IOCTL numbers
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCSCTTY       0x540E

int tty_ioctl(tty_struct_t *tty, u32 cmd, unsigned long arg) {
    int ret = 0;
    
    spin_lock_impl(&tty->lock);
    
    switch (cmd) {
    case TCGETS:
        // Get terminal attributes
        memcpy((void *)arg, &tty->termios, sizeof(termios_t));
        break;
        
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        // Set terminal attributes
        memcpy(&tty->termios, (void *)arg, sizeof(termios_t));
        if (tty->ops && tty->ops->set_termios) {
            tty->ops->set_termios(tty, NULL);
        }
        break;
        
    case TIOCGWINSZ:
        // Get window size
        memcpy((void *)arg, &tty->winsize, sizeof(winsize_t));
        break;
        
    case TIOCSWINSZ:
        // Set window size
        memcpy(&tty->winsize, (void *)arg, sizeof(winsize_t));
        // Send SIGWINCH to process group
        break;
        
    case TIOCGPGRP:
        // Get foreground process group
        *(pid_t *)arg = tty->pgrp;
        break;
        
    case TIOCSPGRP:
        // Set foreground process group
        tty->pgrp = *(pid_t *)arg;
        break;
        
    case TIOCSCTTY:
        // Set controlling terminal
        // TODO: Implement properly
        break;
        
    default:
        ret = -EINVAL;
        break;
    }
    
    spin_unlock_impl(&tty->lock);
    return ret;
}

// ============================================================================
// CONSOLE TTY (built-in)
// ============================================================================

// External VGA functions
extern void vga_putc(char c);
extern void vga_clear(void);

static int console_write(tty_struct_t *tty, const u8 *buf, size_t count) {
    (void)tty;
    for (size_t i = 0; i < count; i++) {
        vga_putc(buf[i]);
    }
    return count;
}

static const tty_operations_t console_ops = {
    .open = NULL,
    .close = NULL,
    .write = console_write,
    .write_room = NULL,
    .flush_buffer = NULL,
    .ioctl = NULL,
    .set_termios = NULL,
    .throttle = NULL,
    .unthrottle = NULL,
    .stop = NULL,
    .start = NULL,
    .hangup = NULL,
};

// ============================================================================
// INITIALIZATION
// ============================================================================

static tty_struct_t *console_tty;

void tty_init(void) {
    printk(KERN_INFO "TTY: Initializing terminal subsystem...\n");
    
    // Initialize console TTY
    console_tty = tty_alloc(0);
    if (console_tty) {
        console_tty->ops = &console_ops;
        tty_table[0] = console_tty;
        printk(KERN_INFO "TTY: Console initialized (tty0)\n");
    }
}

tty_struct_t *tty_get_console(void) {
    return console_tty;
}

// Feed character from keyboard to TTY
void tty_keyboard_input(u8 c) {
    if (console_tty) {
        tty_receive_char(console_tty, c);
    }
}
