/**
 * Picomimi-x64 TTY Line Discipline (N_TTY)
 *
 * Full POSIX line discipline:
 *   - Canonical (cooked) mode: line-buffered with VINTR/VKILL/VERASE/VEOF
 *   - Raw mode: byte-at-a-time delivery
 *   - ISIG: SIGINT on ^C, SIGQUIT on ^\, SIGTSTP on ^Z
 *   - ECHO / ECHOE / ECHOK / ECHONL
 *   - OPOST / ONLCR output post-processing
 *   - TIOCGWINSZ / TIOCSWINSZ / TCGETS / TCSETS ioctls
 *   - SIGWINCH on window resize
 *   - Foreground process group signal delivery
 *   - Non-blocking read support (O_NONBLOCK)
 *
 * Multiple TTY instances (tty0..tty7, console, serial console).
 * VT switching stub (Alt+Fn) ready for WM integration.
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/io.h>
#include <drivers/tty/tty.h>
#include <fs/vfs.h>

extern int  send_signal(pid_t pid, int sig);

/* =========================================================
 * TTY registry
 * ========================================================= */

#define MAX_TTYS            8
#define TTY_BUF_SIZE        4096
#define TTY_LINEBUF_SIZE    4096
#define NCCS                19

__attribute__((unused)) static tty_struct_t *tty_table[MAX_TTYS];
static tty_struct_t *console_tty = NULL;
__attribute__((unused)) static spinlock_t tty_registry_lock = { .raw_lock = {0} };

/* =========================================================
 * Ring buffer helpers
 * ========================================================= */

static inline int rbuf_empty(const tty_buffer_t *rb) {
    return rb->count == 0;
}

static inline int rbuf_full(const tty_buffer_t *rb) {
    return rb->count == TTY_BUF_SIZE;
}

static inline int rbuf_put(tty_buffer_t *rb, u8 c) {
    if (rbuf_full(rb)) return -1;
    rb->data[rb->head] = c;
    rb->head = (rb->head + 1) & (TTY_BUF_SIZE - 1);
    rb->count++;
    return 0;
}

static inline int rbuf_get(tty_buffer_t *rb, u8 *c) {
    if (rbuf_empty(rb)) return -1;
    *c = rb->data[rb->tail];
    rb->tail = (rb->tail + 1) & (TTY_BUF_SIZE - 1);
    rb->count--;
    return 0;
}

static inline size_t rbuf_count(const tty_buffer_t *rb) {
    return rb->count;
}

/* =========================================================
 * Default termios
 * ========================================================= */

static void tty_set_default_termios(tty_struct_t *tty) {
    termios_t *t = &tty->termios;

    /* Input: enable CR->NL mapping, 8-bit input */
    t->c_iflag = ICRNL | IXON;

    /* Output: post-process, NL->CRNL */
    t->c_oflag = OPOST | ONLCR;

    /* Control: 8N1, enable receiver */
    t->c_cflag = CS8 | CREAD | CLOCAL;

    /* Local: canonical + echo + signals */
    t->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;

    /* Control chars */
    t->c_cc[VINTR]    = 3;    /* ^C */
    t->c_cc[VQUIT]    = 28;   /* ^\ */
    t->c_cc[VERASE]   = 127;  /* DEL / Backspace */
    t->c_cc[VKILL]    = 21;   /* ^U */
    t->c_cc[VEOF]     = 4;    /* ^D */
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSTART]   = 17;   /* ^Q */
    t->c_cc[VSTOP]    = 19;   /* ^S */
    t->c_cc[VSUSP]    = 26;   /* ^Z */
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 18;   /* ^R */
    t->c_cc[VDISCARD] = 15;   /* ^O */
    t->c_cc[VWERASE]  = 23;   /* ^W */
    t->c_cc[VLNEXT]   = 22;   /* ^V */
    t->c_cc[VEOL2]    = 0;

    t->c_ispeed = 38400;
    t->c_ospeed = 38400;

    tty->winsize.ws_row    = 25;
    tty->winsize.ws_col    = 80;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
}

/* =========================================================
 * Output: write to underlying hardware driver
 * ========================================================= */

static void tty_output_char(tty_struct_t *tty, u8 c) {
    /* OPOST processing */
    if (tty->termios.c_oflag & OPOST) {
        if (c == '\n' && (tty->termios.c_oflag & ONLCR)) {
            if (tty->ops && tty->ops->write_char)
                tty->ops->write_char(tty, '\r');
        }
        if (c == '\r' && (tty->termios.c_oflag & OCRNL)) {
            c = '\n';
        }
    }

    if (tty->ops && tty->ops->write_char)
        tty->ops->write_char(tty, c);
}

static void tty_echo_char(tty_struct_t *tty, u8 c) {
    if (!(tty->termios.c_lflag & ECHO)) return;

    if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
        /* Print control chars as ^X */
        tty_output_char(tty, '^');
        tty_output_char(tty, c + 0x40);
    } else {
        tty_output_char(tty, c);
    }
}

/* =========================================================
 * Signal delivery to foreground process group
 * ========================================================= */

static void tty_signal_pgrp(tty_struct_t *tty, int sig) {
    if (tty->pgrp <= 0) return;

    extern task_struct_t *task_table[];
    for (int i = 0; i < 4096; i++) {
        task_struct_t *t = task_table[i];
        if (!t) continue;
        if (!t->signal) continue;
        if (t->signal->pgrp == tty->pgrp) {
            send_signal(t->pid, sig);
        }
    }
}

/* =========================================================
 * N_TTY: receive character from hardware into line discipline
 * Called from keyboard IRQ / serial IRQ
 * ========================================================= */


/* =========================================================
 * Console write (called by printk)
 * ========================================================= */

static void console_write_char_fb(tty_struct_t *tty, u8 c);

__attribute__((unused)) static const tty_operations_t fb_tty_ops = {
    .write_char = console_write_char_fb,
};

/* printk calls tty_console_write for kernel messages */
void tty_console_write(const char *str, size_t len) {
    if (!console_tty) return;
    for (size_t i = 0; i < len; i++)
        tty_output_char(console_tty, (u8)str[i]);
}

/* Simple VGA text mode fallback for console_write_char */
static void console_write_char_fb(tty_struct_t *tty, u8 c) {
    (void)tty;
    /* Forward to VGA driver (always available as fallback) */
    extern void vga_putchar(char c);
    vga_putchar((char)c);

    /* Also forward to framebuffer WM terminal if available */
    extern void wm_terminal_putchar(char c) __attribute__((weak));
    if (wm_terminal_putchar)
        wm_terminal_putchar((char)c);
}

/* =========================================================
 * TTY subsystem init
 * ========================================================= */
