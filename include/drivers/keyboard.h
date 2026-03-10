/**
 * Picomimi-x64 Keyboard Driver
 *
 * Full PS/2 keyboard driver replacing the minimal stub in handlers.c.
 *
 * Features:
 *   - Scancode Set 1 (XT) translation (what PC keyboards send by default)
 *   - Modifier tracking: Shift, Ctrl, Alt, CapsLock, NumLock
 *   - Extended keys: arrows, F1-F12, Home/End/PgUp/PgDn, Insert/Delete
 *   - Key event queue (circular buffer, 256 entries)
 *   - Typed character queue for shell/WM use
 *   - Callback registration for GUI event dispatch
 *
 * References:
 *   - GRUB source: grub-core/term/i386/pc/at_keyboard.c
 *   - OSDev wiki: PS/2 Keyboard scancodes
 *   - PC/AT keyboard controller spec (8042)
 */

#ifndef _DRIVERS_KEYBOARD_H
#define _DRIVERS_KEYBOARD_H

#include <kernel/types.h>

/* ============================================================
 * Key codes (our internal virtual key codes, similar to X11 keysyms)
 * ============================================================ */

typedef u32 keycode_t;

/* Printable ASCII range */
#define KEY_SPACE       0x20
#define KEY_A           0x41
/* ... rest map directly to ASCII uppercase */

/* Special / non-printable keys */
#define KEY_NONE        0x0000
#define KEY_BACKSPACE   0x0008
#define KEY_TAB         0x0009
#define KEY_ENTER       0x000D
#define KEY_ESCAPE      0x001B

#define KEY_F1          0x0100
#define KEY_F2          0x0101
#define KEY_F3          0x0102
#define KEY_F4          0x0103
#define KEY_F5          0x0104
#define KEY_F6          0x0105
#define KEY_F7          0x0106
#define KEY_F8          0x0107
#define KEY_F9          0x0108
#define KEY_F10         0x0109
#define KEY_F11         0x010A
#define KEY_F12         0x010B

#define KEY_UP          0x0200
#define KEY_DOWN        0x0201
#define KEY_LEFT        0x0202
#define KEY_RIGHT       0x0203
#define KEY_HOME        0x0204
#define KEY_END         0x0205
#define KEY_PGUP        0x0206
#define KEY_PGDN        0x0207
#define KEY_INSERT      0x0208
#define KEY_DELETE      0x0209

#define KEY_LSHIFT      0x0300
#define KEY_RSHIFT      0x0301
#define KEY_LCTRL       0x0302
#define KEY_RCTRL       0x0303
#define KEY_LALT        0x0304
#define KEY_RALT        0x0305
#define KEY_CAPSLOCK    0x0306
#define KEY_NUMLOCK     0x0307
#define KEY_SCROLLLOCK  0x0308

#define KEY_PRINT       0x0400
#define KEY_PAUSE       0x0401

/* ============================================================
 * Key event
 * ============================================================ */

#define KEY_MOD_SHIFT   (1 << 0)
#define KEY_MOD_CTRL    (1 << 1)
#define KEY_MOD_ALT     (1 << 2)
#define KEY_MOD_CAPS    (1 << 3)
#define KEY_MOD_NUM     (1 << 4)

typedef struct {
    keycode_t   keycode;    /* KEY_* virtual key code */
    char        ascii;      /* Translated ASCII char (0 if non-printable) */
    u32         modifiers;  /* KEY_MOD_* bitmask */
    int         pressed;    /* 1 = keydown, 0 = keyup */
} key_event_t;

/* ============================================================
 * API
 * ============================================================ */

/** Initialize keyboard driver. Call after IDT/PIC are up. */
void kbd_init(void);

/**
 * kbd_handle_irq() - Called from IRQ1 handler (replaces keyboard_handler).
 * Reads one byte from 0x60, updates state, enqueues events.
 */
void kbd_handle_irq(void);

/** Poll: returns 1 if a key event is pending */
int kbd_event_pending(void);

/** Dequeue one key event. Returns 0 on success, -1 if queue empty. */
int kbd_get_event(key_event_t *out);

/** Typed character queue: returns next ASCII char, or 0 if empty */
char kbd_getchar(void);

/** Check if ASCII char is pending in typed queue */
int kbd_char_pending(void);

/**
 * Register a callback for all key events (used by WM for global hotkeys).
 * Pass NULL to unregister.
 */
void kbd_set_callback(void (*cb)(const key_event_t *ev));

/** Get current modifier state */
u32 kbd_get_modifiers(void);

#endif /* _DRIVERS_KEYBOARD_H */
