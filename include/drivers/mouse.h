/**
 * Picomimi-x64 PS/2 Mouse Driver
 *
 * IRQ12 on slave PIC. Three-byte packet protocol.
 * Supports relative movement + 3 buttons.
 */
#ifndef _DRIVERS_MOUSE_H
#define _DRIVERS_MOUSE_H

#include <kernel/types.h>

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

typedef struct {
    int x, y;           /* absolute screen position */
    int dx, dy;         /* last relative delta */
    u8  buttons;        /* MOUSE_BTN_* bitmask */
    int screen_w;
    int screen_h;
} mouse_state_t;

typedef struct {
    int dx, dy;
    u8  buttons;
    int x, y;
} mouse_event_t;

void mouse_init(int screen_w, int screen_h);
void mouse_handle_irq(void);

/* Poll current state */
const mouse_state_t *mouse_get_state(void);

/* Event queue */
int  mouse_event_pending(void);
int  mouse_get_event(mouse_event_t *out);

/* Callback for WM */
void mouse_set_callback(void (*cb)(const mouse_event_t *ev));

#endif
