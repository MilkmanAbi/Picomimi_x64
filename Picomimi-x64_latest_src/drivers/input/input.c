/**
 * Picomimi-x64 Input Event Subsystem
 *
 * Provides a Linux-compatible /dev/input/eventN interface.
 *   /dev/input/event0  →  keyboard
 *   /dev/input/event1  →  mouse / pointer
 *
 * Wire-up:
 *   keyboard driver calls  input_push_key(keycode, pressed)
 *   mouse driver calls     input_push_rel(dx, dy, wheel)  /  input_push_btn(btn, pressed)
 *   PaperDE reads          /dev/input/event0  or  /dev/input/event1
 *
 * On-wire format matches Linux struct input_event (uapi/linux/input.h):
 *   u64  tv_sec
 *   u64  tv_usec
 *   u16  type
 *   u16  code
 *   s32  value
 *   → 24 bytes total
 */

#include <kernel/types.h>
#include <kernel/process.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <mm/slab.h>
#include <drivers/chrdev.h>

/* ------------------------------------------------------------------ */
/* input_event layout                                                   */
/* ------------------------------------------------------------------ */

struct input_event {
    u64  tv_sec;
    u64  tv_usec;
    u16  type;
    u16  code;
    s32  value;
} __packed;

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04

#define SYN_REPORT      0
#define SYN_CONFIG      1

/* Relative axes */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_HWHEEL      0x06
#define REL_WHEEL       0x08

/* Mouse buttons (BTN_LEFT etc live in the KEY space) */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112

/* ------------------------------------------------------------------ */
/* Per-device ring buffer                                               */
/* ------------------------------------------------------------------ */

#define INPUT_QUEUE_LEN  512   /* events — power of 2 */

typedef struct {
    struct input_event  ring[INPUT_QUEUE_LEN];
    volatile u32        head;       /* consumer index */
    volatile u32        tail;       /* producer index */
    spinlock_t          lock;
    const char         *name;
    int                 minor;
} input_dev_t;

#define INPUT_NDEVS   2
static input_dev_t input_devs[INPUT_NDEVS];

/* ---- jiffies timestamp shim ---- */
extern volatile u64 jiffies;
static void ev_timestamp(struct input_event *ev) {
    ev->tv_sec  = jiffies / 100;          /* 100 Hz timer tick → seconds */
    ev->tv_usec = (jiffies % 100) * 10000;
}

/* ---- ring push (called from IRQ context) ---- */
static void dev_push(input_dev_t *d, u16 type, u16 code, s32 value) {
    u32 next = (d->tail + 1) & (INPUT_QUEUE_LEN - 1);
    if (next == d->head) return;   /* full — drop oldest would be complex; just drop new */
    struct input_event *ev = &d->ring[d->tail];
    ev_timestamp(ev);
    ev->type  = type;
    ev->code  = code;
    ev->value = value;
    d->tail = next;               /* atomic publish — single-producer assumed */
}

static int dev_pop(input_dev_t *d, struct input_event *out) {
    if (d->head == d->tail) return -1;
    *out = d->ring[d->head];
    d->head = (d->head + 1) & (INPUT_QUEUE_LEN - 1);
    return 0;
}

static int dev_avail(const input_dev_t *d) {
    return (int)((d->tail - d->head) & (INPUT_QUEUE_LEN - 1));
}

/* ------------------------------------------------------------------ */
/* Public push API — called by keyboard.c and mouse.c                  */
/* ------------------------------------------------------------------ */

void input_push_key(u16 keycode, int pressed) {
    dev_push(&input_devs[0], EV_KEY, keycode, pressed ? 1 : 0);
    dev_push(&input_devs[0], EV_SYN, SYN_REPORT, 0);
}

void input_push_rel(s32 dx, s32 dy, s32 wheel) {
    if (dx)    dev_push(&input_devs[1], EV_REL, REL_X,     dx);
    if (dy)    dev_push(&input_devs[1], EV_REL, REL_Y,     dy);
    if (wheel) dev_push(&input_devs[1], EV_REL, REL_WHEEL, wheel);
    dev_push(&input_devs[1], EV_SYN, SYN_REPORT, 0);
}

void input_push_btn(u16 btn, int pressed) {
    dev_push(&input_devs[1], EV_KEY, btn, pressed ? 1 : 0);
    dev_push(&input_devs[1], EV_SYN, SYN_REPORT, 0);
}

/* PS/2 scan-code → Linux keycode table (set 1, subset) */
static const u16 ps2_to_linux[128] = {
    [0x01]=1,   /* ESC */
    [0x02]=2,   [0x03]=3,   [0x04]=4,   [0x05]=5,   [0x06]=6,
    [0x07]=7,   [0x08]=8,   [0x09]=9,   [0x0A]=10,  [0x0B]=11,
    [0x0C]=12,  [0x0D]=13,
    [0x0E]=14,  /* Backspace */
    [0x0F]=15,  /* Tab */
    [0x10]=16,  [0x11]=17,  [0x12]=18,  [0x13]=19,  [0x14]=20,
    [0x15]=21,  [0x16]=22,  [0x17]=23,  [0x18]=24,  [0x19]=25,
    [0x1A]=26,  [0x1B]=27,
    [0x1C]=28,  /* Enter */
    [0x1D]=29,  /* Left Ctrl */
    [0x1E]=30,  [0x1F]=31,  [0x20]=32,  [0x21]=33,  [0x22]=34,
    [0x23]=35,  [0x24]=36,  [0x25]=37,  [0x26]=38,
    [0x27]=39,  /* ; */
    [0x28]=40,  /* ' */
    [0x29]=41,  /* ` */
    [0x2A]=42,  /* Left Shift */
    [0x2B]=43,
    [0x2C]=44,  [0x2D]=45,  [0x2E]=46,  [0x2F]=47,  [0x30]=48,
    [0x31]=49,  [0x32]=50,
    [0x33]=51,  [0x34]=52,  [0x35]=53,
    [0x36]=54,  /* Right Shift */
    [0x38]=56,  /* Left Alt */
    [0x39]=57,  /* Space */
    [0x3A]=58,  /* Caps Lock */
    [0x3B]=59,  [0x3C]=60,  [0x3D]=61,  [0x3E]=62,
    [0x3F]=63,  [0x40]=64,  [0x41]=65,  [0x42]=66,  [0x43]=67,
    [0x44]=68,  /* F10 */
    [0x57]=87,  [0x58]=88,  /* F11, F12 */
    [0x48]=103, /* Up */
    [0x4B]=105, /* Left */
    [0x4D]=106, /* Right */
    [0x50]=108, /* Down */
    [0x47]=102, /* Home */
    [0x4F]=107, /* End */
    [0x49]=104, /* PgUp */
    [0x51]=109, /* PgDn */
    [0x52]=110, /* Insert */
    [0x53]=111, /* Delete */
};

void input_push_scancode(u8 scancode) {
    int release = (scancode & 0x80) != 0;
    u8  code    = scancode & 0x7F;
    u16 linux_key = (code < 128) ? ps2_to_linux[code] : 0;
    if (!linux_key) return;
    input_push_key(linux_key, !release);
}

/* ------------------------------------------------------------------ */
/* VFS file_operations for /dev/input/eventN                           */
/* ------------------------------------------------------------------ */

static s64 input_fops_read(file_t *file, char *buf, size_t count, u64 *pos) {
    (void)pos;
    int idx = (int)(long)file->private_data;
    if (idx < 0 || idx >= INPUT_NDEVS) return -ENODEV;
    input_dev_t *d = &input_devs[idx];

    size_t evsize  = sizeof(struct input_event);
    size_t max_ev  = count / evsize;
    if (max_ev == 0) return -EINVAL;

    /* Non-blocking: return what's available */
    if (file->f_flags & O_NONBLOCK) {
        size_t n = 0;
        while (n < max_ev) {
            struct input_event ev;
            if (dev_pop(d, &ev) < 0) break;
            memcpy(buf + n * evsize, &ev, evsize);
            n++;
        }
        return n ? (s64)(n * evsize) : -EAGAIN;
    }

    /* Blocking: wait for at least one event */
    while (dev_avail(d) == 0) {
        extern void schedule(void);
        schedule();
    }
    size_t n = 0;
    while (n < max_ev) {
        struct input_event ev;
        if (dev_pop(d, &ev) < 0) break;
        memcpy(buf + n * evsize, &ev, evsize);
        n++;
    }
    return (s64)(n * evsize);
}

static s64 input_fops_write(file_t *f, const char *b, size_t n, u64 *p) {
    (void)f; (void)b; (void)n; (void)p;
    return (s64)n;   /* silently accept, ignore */
}

static int input_fops_open(inode_t *inode, file_t *file) {
    int minor = (int)(inode->i_rdev & 0xFF);
    file->private_data = (void *)(long)minor;
    return 0;
}

static const file_operations_t input_fops = {
    .read  = input_fops_read,
    .write = input_fops_write,
    .open  = input_fops_open,
};

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void input_init(void) {
    for (int i = 0; i < INPUT_NDEVS; i++) {
        memset(&input_devs[i], 0, sizeof(input_devs[i]));
        spin_lock_init(&input_devs[i].lock);
        input_devs[i].minor = i;
    }
    input_devs[0].name = "keyboard";
    input_devs[1].name = "mouse";

    /* Register char device major 13 (Linux evdev input devices) */
    chrdev_register(INPUT_MAJOR, "input", &input_fops);

    printk(KERN_INFO "[input] subsystem ready — event0=kbd event1=mouse\n");
}
