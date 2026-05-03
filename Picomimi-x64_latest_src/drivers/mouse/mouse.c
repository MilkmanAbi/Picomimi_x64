/**
 * Picomimi-x64 PS/2 Mouse Driver
 *
 * PS/2 mouse sits on the 8042 keyboard controller aux port, IRQ12.
 * Three-byte packet protocol (standard PS/2).
 *
 * Key QEMU note: QEMU emulates a PS/2 mouse on the 8042. Status bit 5
 * (AUXDATA) indicates whether the output buffer byte came from the aux
 * port. We MUST check this to avoid eating keyboard scancodes.
 *
 * Packet format:
 *   Byte 0: [YO][XO][YS][XS][1][MB][RB][LB]
 *   Byte 1: X delta (signed 9-bit via bit 4 of byte 0 as sign)
 *   Byte 2: Y delta (signed 9-bit via bit 5 of byte 0, INVERTED)
 */

#include <kernel/types.h>
#include <drivers/mouse.h>
#include <arch/io.h>
#include <drivers/input.h>
#include <lib/string.h>
#include <lib/printk.h>

#define KBD_DATA    0x60
#define KBD_STATUS  0x64
#define KBD_CMD     0x64

#define STS_OBF     (1 << 0)   /* Output buffer full */
#define STS_IBF     (1 << 1)   /* Input buffer full  */
#define STS_AUX     (1 << 5)   /* Data from aux (mouse) port */

#define MOUSE_QUEUE  128

static mouse_state_t state;
static u8   pkt[3];
static int  pkt_idx = 0;
static int  initialized = 0;

static mouse_event_t evq[MOUSE_QUEUE];
static volatile int  evq_head = 0, evq_tail = 0;

static void (*mouse_cb)(const mouse_event_t *) = NULL;

/* ── 8042 helpers ─────────────────────────────────────────── */
static void wait_write(void) {
    int t = 100000;
    while ((inb(KBD_STATUS) & STS_IBF) && --t);
}

/* Read with timeout — returns 0xFF on timeout */
static u8 read_byte(void) {
    int t = 100000;
    while (!(inb(KBD_STATUS) & STS_OBF) && --t);
    if (!t) return 0xFF;
    return inb(KBD_DATA);
}

static void mouse_send(u8 byte) {
    wait_write();
    outb(KBD_CMD, 0xD4);   /* route next byte to aux port */
    wait_write();
    outb(KBD_DATA, byte);
}

/* Send command and discard ACK (0xFA) */
static void mouse_cmd(u8 byte) {
    mouse_send(byte);
    u8 ack = read_byte();
    (void)ack;  /* should be 0xFA = ACK */
}

/* ── Init ─────────────────────────────────────────────────── */
void mouse_init(int screen_w, int screen_h) {
    state.x = screen_w / 2;
    state.y = screen_h / 2;
    state.screen_w = screen_w;
    state.screen_h = screen_h;
    state.buttons  = 0;
    pkt_idx = 0;
    evq_head = evq_tail = 0;
    initialized = 0;

    /* 1. Enable aux device */
    wait_write(); outb(KBD_CMD, 0xA8);

    /* 2. Read+modify controller config to enable aux IRQ */
    wait_write(); outb(KBD_CMD, 0x20);
    u8 cfg = read_byte();
    cfg |=  0x02;   /* bit 1: enable aux IRQ12 */
    cfg &= ~0x20;   /* bit 5: clear aux clock disable */
    wait_write(); outb(KBD_CMD, 0x60);
    wait_write(); outb(KBD_DATA, cfg);

    /* 3. Mouse: set defaults, set sample rate, enable */
    mouse_cmd(0xF6);        /* set defaults           */
    mouse_cmd(0xF3);        /* set sample rate:       */
    mouse_send(100); read_byte(); /* 100 samples/sec   */
    mouse_cmd(0xE8);        /* set resolution:        */
    mouse_send(0x02); read_byte(); /* 4 counts/mm      */
    mouse_cmd(0xF4);        /* enable data reporting  */

    initialized = 1;
    printk(KERN_INFO "[MOUSE] PS/2 mouse initialized (%dx%d center)\n",
           screen_w, screen_h);
}

/* ── IRQ12 handler ────────────────────────────────────────── */
void mouse_handle_irq(void) {
    /* Read status — bail if output buffer is empty */
    u8 status = inb(KBD_STATUS);
    if (!(status & STS_OBF)) return;

    /* MUST be from aux port, not keyboard */
    if (!(status & STS_AUX)) return;

    u8 byte = inb(KBD_DATA);

    /* Byte 0 sync: bit 3 must be set (always 1 per protocol) */
    if (pkt_idx == 0) {
        if (!(byte & 0x08)) return;   /* desync — drop */
    }

    pkt[pkt_idx++] = byte;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    u8 flags = pkt[0];

    /* Discard overflow packets */
    if ((flags & 0x40) || (flags & 0x80)) return;

    /* Signed 9-bit deltas: sign bit is in flags byte */
    int dx = (int)pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((flags & 0x20) ? 256 : 0);
    dy = -dy;   /* PS/2 Y is inverted relative to screen */

    state.x += dx;
    state.y += dy;
    if (state.x < 0)               state.x = 0;
    if (state.y < 0)               state.y = 0;
    if (state.x >= state.screen_w) state.x = state.screen_w - 1;
    if (state.y >= state.screen_h) state.y = state.screen_h - 1;

    state.dx = dx;
    state.dy = dy;
    state.buttons = flags & 0x07;

    /* Enqueue */
    mouse_event_t ev = { dx, dy, state.buttons, state.x, state.y };
    int next = (evq_tail + 1) % MOUSE_QUEUE;
    if (next != evq_head) {
        evq[evq_tail] = ev;
        evq_tail = next;
    }

    if (mouse_cb) mouse_cb(&ev);

    /* Push to /dev/input/event1 for PaperDE / userspace */
    input_push_rel((s32)dx, (s32)dy, 0);
    /* Button changes */
    static u8 prev_buttons = 0;
    u8 cur = flags & 0x07;
    if ((cur ^ prev_buttons) & 0x01) input_push_btn(BTN_LEFT,   (cur & 0x01) ? 1 : 0);
    if ((cur ^ prev_buttons) & 0x02) input_push_btn(BTN_RIGHT,  (cur & 0x02) ? 1 : 0);
    if ((cur ^ prev_buttons) & 0x04) input_push_btn(BTN_MIDDLE, (cur & 0x04) ? 1 : 0);
    prev_buttons = cur;
}

const mouse_state_t *mouse_get_state(void) { return &state; }
int  mouse_event_pending(void)             { return evq_head != evq_tail; }

int mouse_get_event(mouse_event_t *out) {
    if (evq_head == evq_tail) return -1;
    *out = evq[evq_head];
    evq_head = (evq_head + 1) % MOUSE_QUEUE;
    return 0;
}

void mouse_set_callback(void (*cb)(const mouse_event_t *ev)) {
    mouse_cb = cb;
}
