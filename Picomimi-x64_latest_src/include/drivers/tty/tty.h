/**
 * Picomimi-x64 TTY Subsystem Header
 */
#ifndef _DRIVERS_TTY_TTY_H
#define _DRIVERS_TTY_TTY_H

#include <kernel/types.h>

/* =========================================================
 * Termios flags
 * ========================================================= */

/* c_iflag */
#define IGNBRK      0x000001
#define BRKINT      0x000002
#define IGNPAR      0x000004
#define PARMRK      0x000008
#define INPCK       0x000010
#define ISTRIP      0x000020
#define INLCR       0x000040
#define IGNCR       0x000080
#define ICRNL       0x000100
#define IUCLC       0x000200
#define IXON        0x000400
#define IXANY       0x000800
#define IXOFF       0x001000
#define IMAXBEL     0x002000
#define IUTF8       0x004000

/* c_oflag */
#define OPOST       0x000001
#define OLCUC       0x000002
#define ONLCR       0x000004
#define OCRNL       0x000008
#define ONOCR       0x000010
#define ONLRET      0x000020
#define OFILL       0x000040
#define OFDEL       0x000080

/* c_cflag */
#define CS5         0x000000
#define CS6         0x000010
#define CS7         0x000020
#define CS8         0x000030
#define CSTOPB      0x000040
#define CREAD       0x000080
#define PARENB      0x000100
#define PARODD      0x000200
#define HUPCL       0x000400
#define CLOCAL      0x000800

/* c_lflag */
#define ISIG        0x000001
#define ICANON      0x000002
#define XCASE       0x000004
#define ECHO        0x000008
#define ECHOE       0x000010
#define ECHOK       0x000020
#define ECHONL      0x000040
#define NOFLSH      0x000080
#define TOSTOP      0x000100
#define ECHOCTL     0x000200
#define ECHOPRT     0x000400
#define ECHOKE      0x000800
#define FLUSHO      0x001000
#define PENDIN      0x004000
#define IEXTEN      0x008000
#define EXTPROC     0x010000

/* c_cc indices */
#define VINTR       0
#define VQUIT       1
#define VERASE      2
#define VKILL       3
#define VEOF        4
#define VTIME       5
#define VMIN        6
#define VSWTC       7
#define VSTART      8
#define VSTOP       9
#define VSUSP       10
#define VEOL        11
#define VREPRINT    12
#define VDISCARD    13
#define VWERASE     14
#define VLNEXT      15
#define VEOL2       16
#define NCCS        19

/* =========================================================
 * Structures
 * ========================================================= */

#define TTY_BUF_SIZE        4096
#define TTY_LINEBUF_SIZE    4096
#define TTY_NAME_LEN        32

typedef struct termios {
    u32     c_iflag;
    u32     c_oflag;
    u32     c_cflag;
    u32     c_lflag;
    u8      c_line;
    u8      c_cc[NCCS];
    u32     c_ispeed;
    u32     c_ospeed;
} termios_t;

typedef struct winsize {
    u16     ws_row;
    u16     ws_col;
    u16     ws_xpixel;
    u16     ws_ypixel;
} winsize_t;

typedef struct tty_buffer {
    u8      data[TTY_BUF_SIZE];
    size_t  head;
    size_t  tail;
    size_t  count;
} tty_buffer_t;

struct tty_struct;

typedef struct tty_operations {
    void (*write_char)(struct tty_struct *tty, u8 c);
    int  (*write)(struct tty_struct *tty, const u8 *buf, size_t count);
    int  (*ioctl)(struct tty_struct *tty, unsigned int cmd, unsigned long arg);
    void (*set_termios)(struct tty_struct *tty, termios_t *old);
    void (*start)(struct tty_struct *tty);
    void (*stop)(struct tty_struct *tty);
    void (*hangup)(struct tty_struct *tty);
    int  (*open)(struct tty_struct *tty);
    void (*close)(struct tty_struct *tty);
} tty_operations_t;

typedef struct tty_struct {
    int                     index;
    char                    name[TTY_NAME_LEN];

    termios_t               termios;
    winsize_t               winsize;

    tty_buffer_t            read_buf;
    tty_buffer_t            write_buf;

    u8                      line_buf[TTY_LINEBUF_SIZE];
    size_t                  line_pos;

    pid_t                   session;
    pid_t                   pgrp;

    const tty_operations_t  *ops;
    void                    *driver_data;

    int                     count;
    int                     stopped;

    spinlock_t              lock;
} tty_struct_t;

/* =========================================================
 * API
 * ========================================================= */

void  tty_init(void);
tty_struct_t *tty_alloc(int index, const tty_operations_t *ops);
void  tty_free(tty_struct_t *tty);
tty_struct_t *tty_get(int index);

void  tty_receive_char(tty_struct_t *tty, u8 c);
void  tty_console_write(const char *str, size_t len);
void  snprintf_simple(char *buf, size_t size, const char *fmt, int n);

extern const file_operations_t tty_fops;

/* ioctl numbers */
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414
#define TIOCSCTTY       0x540E
#define TIOCNOTTY       0x5422
#define FIONREAD        0x541B
#define TIOCOUTQ        0x5411

#endif /* _DRIVERS_TTY_TTY_H */
