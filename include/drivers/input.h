/**
 * Picomimi-x64 Input Event Subsystem — Public API
 */
#pragma once
#include <kernel/types.h>

/* Called from keyboard IRQ handler */
void input_push_key(u16 keycode, int pressed);
void input_push_scancode(u8 scancode);   /* PS/2 set-1 scancode */

/* Called from mouse IRQ handler */
void input_push_rel(s32 dx, s32 dy, s32 wheel);
void input_push_btn(u16 btn, int pressed);

/* BTN codes (match Linux) */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* Linux EV_ types */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03

/* Initialise — call from kernel_main after devfs is up */
void input_init(void);
