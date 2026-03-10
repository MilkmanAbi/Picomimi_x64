/**
 * Picomimi-x64 PS/2 Keyboard Driver
 *
 * Scancode Set 1 (XT) - this is what 8042 controller sends us by default.
 * Extended keys are prefixed by 0xE0. Key release = scancode | 0x80.
 *
 * Scancode table directly mirrors GRUB's at_keyboard.c scancode_set1[]
 * and linux/drivers/input/keyboard/atkbd.c atkbd_set2_keycode[].
 */

#include <kernel/types.h>
#include <drivers/keyboard.h>
#include <arch/io.h>
#include <drivers/input.h>
#include <lib/string.h>
#include <lib/printk.h>

/* ============================================================
 * 8042 ports
 * ============================================================ */

#define KBD_DATA    0x60    /* Data port (R/W) */
#define KBD_STATUS  0x64    /* Status register (R) */
#define KBD_CMD     0x64    /* Command register (W) */

#define KBD_STATUS_OBF  (1 << 0)   /* Output buffer full */
#define KBD_STATUS_IBF  (1 << 1)   /* Input buffer full */

/* ============================================================
 * Scancode Set 1 -> keycode table
 * Index = scancode byte (0x00-0x58)
 * Extended scancodes (prefixed 0xE0) handled separately.
 * From GRUB at_keyboard.c and OSDev scancode tables.
 * ============================================================ */

static const keycode_t sc1_normal[0x59] = {
    [0x00] = KEY_NONE,
    [0x01] = KEY_ESCAPE,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',
    [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',
    [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',
    [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LCTRL,
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2A] = KEY_LSHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',
    [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',
    [0x34] = '.',  [0x35] = '/',
    [0x36] = KEY_RSHIFT,
    [0x37] = '*',   /* numpad * */
    [0x38] = KEY_LALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,  [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,  [0x40] = KEY_F6,  [0x41] = KEY_F7,  [0x42] = KEY_F8,
    [0x43] = KEY_F9,  [0x44] = KEY_F10,
    [0x45] = KEY_NUMLOCK,
    [0x46] = KEY_SCROLLLOCK,
    /* Numpad 7/Home, 8/Up, 9/PgUp, -, 4/Left, 5, 6/Right, +,
       1/End, 2/Down, 3/PgDn, 0/Ins, ./Del */
    [0x47] = KEY_HOME,  [0x48] = KEY_UP,   [0x49] = KEY_PGUP,
    [0x4A] = '-',
    [0x4B] = KEY_LEFT,  [0x4C] = '5',     [0x4D] = KEY_RIGHT,
    [0x4E] = '+',
    [0x4F] = KEY_END,   [0x50] = KEY_DOWN, [0x51] = KEY_PGDN,
    [0x52] = KEY_INSERT,[0x53] = KEY_DELETE,
    [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

/* Extended (0xE0-prefixed) scancode -> keycode */
static const keycode_t sc1_extended[0x80] = {
    [0x1C] = KEY_ENTER,     /* numpad enter */
    [0x1D] = KEY_RCTRL,
    [0x35] = '/',           /* numpad / */
    [0x37] = KEY_PRINT,
    [0x38] = KEY_RALT,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PGUP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PGDN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
};

/* Shifted character table for ASCII printables */
static const char shift_map[128] = {
    ['1']='!', ['2']='@', ['3']='#', ['4']='$', ['5']='%',
    ['6']='^', ['7']='&', ['8']='*', ['9']='(', ['0']=')',
    ['-']='_', ['=']= '+', ['[']='{', [']']='}', ['\\']='|',
    [';']=':', ['\'']= '"', ['`']='~', [',']='<', ['.']='>',
    ['/']='?',
};

/* ============================================================
 * State
 * ============================================================ */

#define KBD_QUEUE_SIZE  256
#define KBD_CHAR_SIZE   512

static key_event_t evt_queue[KBD_QUEUE_SIZE];
static volatile int eq_head = 0, eq_tail = 0;

static char char_queue[KBD_CHAR_SIZE];
static volatile int cq_head = 0, cq_tail = 0;

static u32  modifiers   = 0;
static int  extended    = 0;    /* 1 after seeing 0xE0 */

static void (*kbd_callback)(const key_event_t *) = NULL;

/* ============================================================
 * Queue helpers
 * ============================================================ */

static void eq_push(const key_event_t *ev) {
    int next = (eq_tail + 1) % KBD_QUEUE_SIZE;
    if (next == eq_head) return;  /* full, drop */
    evt_queue[eq_tail] = *ev;
    eq_tail = next;
}

static void cq_push(char c) {
    int next = (cq_tail + 1) % KBD_CHAR_SIZE;
    if (next == cq_head) return;
    char_queue[cq_tail] = c;
    cq_tail = next;
}

/* ============================================================
 * Translate keycode + modifiers -> ASCII char
 * ============================================================ */

static char keycode_to_ascii(keycode_t kc, u32 mods) {
    if (kc == KEY_BACKSPACE) return '\b';
    if (kc == KEY_TAB)       return '\t';
    if (kc == KEY_ENTER)     return '\n';
    if (kc == KEY_ESCAPE)    return 0x1B;
    if (kc == ' ')           return ' ';

    if (kc < 128) {
        char c = (char)(kc & 0xFF);
        int shifted = (mods & KEY_MOD_SHIFT) != 0;
        int capslock = (mods & KEY_MOD_CAPS)  != 0;

        if (c >= 'a' && c <= 'z') {
            if (shifted ^ capslock)
                c = c - 'a' + 'A';
        } else if (shifted) {
            char s = shift_map[(int)c];
            if (s) c = s;
        }

        if ((mods & KEY_MOD_CTRL) && c >= '@' && c <= '_')
            return c - '@';     /* Ctrl+A = 0x01, etc. */

        return c;
    }
    return 0;
}

/* ============================================================
 * IRQ handler - called from keyboard_handler() in handlers.c
 * ============================================================ */

void kbd_handle_irq(void) {
    u8 status = inb(KBD_STATUS);
    if (!(status & KBD_STATUS_OBF))
        return;
    /* Bit 5 set = data is from aux (mouse) port — don't steal it */
    if (status & (1 << 5))
        return;

    u8 scancode = inb(KBD_DATA);

    /* Handle extended prefix */
    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    /* Key release vs press */
    int released = (scancode & 0x80) != 0;
    u8  sc       = scancode & 0x7F;

    /* Look up keycode */
    keycode_t kc = KEY_NONE;
    if (extended) {
        if (sc < 0x80) kc = sc1_extended[sc];
        extended = 0;
    } else {
        if (sc < 0x59) kc = sc1_normal[sc];
    }

    if (kc == KEY_NONE) return;

    /* Update modifiers */
    if (!released) {
        if (kc == KEY_LSHIFT || kc == KEY_RSHIFT) modifiers |= KEY_MOD_SHIFT;
        if (kc == KEY_LCTRL  || kc == KEY_RCTRL)  modifiers |= KEY_MOD_CTRL;
        if (kc == KEY_LALT   || kc == KEY_RALT)   modifiers |= KEY_MOD_ALT;
        if (kc == KEY_CAPSLOCK) modifiers ^= KEY_MOD_CAPS;
        if (kc == KEY_NUMLOCK)  modifiers ^= KEY_MOD_NUM;
    } else {
        if (kc == KEY_LSHIFT || kc == KEY_RSHIFT) modifiers &= ~KEY_MOD_SHIFT;
        if (kc == KEY_LCTRL  || kc == KEY_RCTRL)  modifiers &= ~KEY_MOD_CTRL;
        if (kc == KEY_LALT   || kc == KEY_RALT)   modifiers &= ~KEY_MOD_ALT;
    }

    /* Build event */
    key_event_t ev;
    ev.keycode   = kc;
    ev.modifiers = modifiers;
    ev.pressed   = !released;
    ev.ascii     = released ? 0 : keycode_to_ascii(kc, modifiers);

    /* Enqueue */
    eq_push(&ev);
    if (ev.pressed && ev.ascii)
        cq_push(ev.ascii);

    /* Also push to /dev/input/event0 for PaperDE / userspace */
    input_push_key((u16)kc, !released);

    /* Dispatch callback (for WM hotkeys) */
    if (kbd_callback)
        kbd_callback(&ev);
}

/* ============================================================
 * Public API
 * ============================================================ */

void kbd_init(void) {
    modifiers = 0;
    extended  = 0;
    eq_head = eq_tail = 0;
    cq_head = cq_tail = 0;
    printk(KERN_INFO "[KBD] PS/2 keyboard driver initialized (scancode set 1)\n");
}

int kbd_event_pending(void) {
    return eq_head != eq_tail;
}

int kbd_get_event(key_event_t *out) {
    if (eq_head == eq_tail) return -1;
    *out = evt_queue[eq_head];
    eq_head = (eq_head + 1) % KBD_QUEUE_SIZE;
    return 0;
}

char kbd_getchar(void) {
    if (cq_head == cq_tail) return 0;
    char c = char_queue[cq_head];
    cq_head = (cq_head + 1) % KBD_CHAR_SIZE;
    return c;
}

int kbd_char_pending(void) {
    return cq_head != cq_tail;
}

void kbd_set_callback(void (*cb)(const key_event_t *ev)) {
    kbd_callback = cb;
}

u32 kbd_get_modifiers(void) {
    return modifiers;
}
