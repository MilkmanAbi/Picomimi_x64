/**
 * Picomimi-x64 Window Manager
 *
 * A minimal but real compositing window manager, inspired by the
 * windows93.net aesthetic: classic Windows 9x chrome, title bars,
 * draggable windows, taskbar, desktop, overlapping Z-order.
 *
 * Architecture:
 *   - Flat window list with Z-order (no tree)
 *   - Software compositing: dirty-rect based repaints
 *   - Event model: WM_* messages dispatched to focused window
 *   - No heap-per-window: fixed pool of WM_MAX_WINDOWS
 *
 * How it hooks into the kernel:
 *   1. Timer IRQ (PIT ~60Hz) calls wm_tick() -> schedules repaints
 *   2. Keyboard IRQ callback -> wm_dispatch_key()
 *   3. Kernel shell becomes one window in the WM
 *
 * Window anatomy (Win9x style):
 *
 *  ┌──[ico] Title              [_][□][X]─┐
 *  │  menu bar (optional)                │
 *  ├─────────────────────────────────────┤
 *  │                                     │
 *  │           client area               │
 *  │                                     │
 *  └─────────────────────────────────────┘
 *
 * Titlebar height:  18px
 * Border width:      2px (3D raised)
 * Button width:     16px (each)
 */

#ifndef _WM_WM_H
#define _WM_WM_H

#include <kernel/types.h>
#include <drivers/fb.h>
#include <drivers/keyboard.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define WM_MAX_WINDOWS      32
#define WM_TITLE_HEIGHT     18
#define WM_BORDER           2
#define WM_BTN_W            16
#define WM_BTN_H            14
#define WM_TASKBAR_H        28
#define WM_MIN_WIDTH        80
#define WM_MIN_HEIGHT       (WM_TITLE_HEIGHT + WM_BORDER * 2 + 20)

/* Window flags */
#define WF_VISIBLE      (1 << 0)
#define WF_FOCUSED      (1 << 1)
#define WF_MINIMIZED    (1 << 2)
#define WF_MAXIMIZED    (1 << 3)
#define WF_RESIZABLE    (1 << 4)
#define WF_NO_TITLE     (1 << 5)
#define WF_NO_CLOSE     (1 << 6)
#define WF_NO_RESIZE    (1 << 7)
#define WF_DIRTY        (1 << 8)    /* needs repaint */
#define WF_DIALOG       (1 << 9)    /* modal dialog */

/* ============================================================
 * Messages (event system)
 * ============================================================ */

typedef enum {
    WM_NONE         = 0,
    WM_PAINT        = 1,    /* Repaint client area */
    WM_KEYDOWN      = 2,    /* Key pressed: param1 = keycode, param2 = ascii */
    WM_KEYUP        = 3,
    WM_FOCUS_IN     = 4,    /* Window gained focus */
    WM_FOCUS_OUT    = 5,    /* Window lost focus */
    WM_CLOSE        = 6,    /* X button clicked */
    WM_RESIZE       = 7,    /* param1=new_w, param2=new_h */
    WM_TIMER        = 8,    /* Periodic tick */
    WM_CHAR         = 9,    /* ASCII char typed (for terminal windows) */
} wm_msg_t;

/* ============================================================
 * Window structure
 * ============================================================ */

struct wm_window;
typedef struct wm_window wm_window_t;

/* Window procedure: return 0 if handled, -1 for default handling */
typedef int (*wm_wndproc_t)(wm_window_t *w, wm_msg_t msg,
                             u64 param1, u64 param2);

struct wm_window {
    int     id;             /* Unique window ID */
    int     x, y;          /* Screen position (top-left of frame) */
    int     w, h;           /* Total size including frame */
    int     saved_x, saved_y, saved_w, saved_h;  /* Pre-maximize state */
    u32     flags;

    char    title[64];

    /* Client area coordinates (relative to screen) */
    int     client_x, client_y;
    int     client_w, client_h;

    /* Z-order (higher = on top) */
    int     z;

    /* Event handler */
    wm_wndproc_t wndproc;

    /* Per-window back buffer (client area) */
    u8     *backbuf;        /* NULL = no private buffer */
    u32     backbuf_size;

    /* User data pointer (e.g. terminal state) */
    void   *userdata;

    /* Internal */
    int     drag_off_x, drag_off_y;
    int     is_dragging;
};

/* ============================================================
 * Initialization
 * ============================================================ */

/** Initialize WM. Call after fb_init() and kbd_init(). */
void wm_init(void);

/** Main WM loop. Processes events and repaints. Never returns. */
void wm_run(void);

/**
 * wm_tick() - Call from timer IRQ (60Hz).
 * Marks dirty windows and schedules repaints.
 */
void wm_tick(void);

/* ============================================================
 * Window management
 * ============================================================ */

/**
 * wm_create_window() - Create a new window.
 *
 * @x, @y:    Screen position
 * @w, @h:    Total window size (including titlebar + borders)
 * @title:    Title bar text
 * @flags:    WF_* flags
 * @wndproc:  Event handler (NULL = default)
 *
 * Returns window pointer, or NULL on failure.
 */
wm_window_t *wm_create_window(int x, int y, int w, int h,
                               const char *title,
                               u32 flags,
                               wm_wndproc_t wndproc);

/** Destroy a window (frees slot, triggers repaint of area behind it) */
void wm_destroy_window(wm_window_t *w);

/** Show/hide a window */
void wm_show(wm_window_t *w);
void wm_hide(wm_window_t *w);

/** Raise window to top of Z-order and focus it */
void wm_raise(wm_window_t *w);

/** Move window (clamped to screen) */
void wm_move(wm_window_t *w, int x, int y);

/** Resize window (clamped to minimums) */
void wm_resize(wm_window_t *w, int new_w, int new_h);

/** Maximize / restore */
void wm_maximize(wm_window_t *w);
void wm_restore(wm_window_t *w);

/** Mark window client area as needing repaint */
void wm_invalidate(wm_window_t *w);

/** Get window by ID */
wm_window_t *wm_get_window(int id);

/** Get currently focused window */
wm_window_t *wm_get_focused(void);

/* ============================================================
 * Drawing inside windows
 * All coordinates are relative to client area top-left.
 * ============================================================ */

void wm_fill_client(wm_window_t *w, fb_color_t color);
void wm_draw_text(wm_window_t *w, int x, int y,
                  const char *s, fb_color_t fg, fb_color_t bg);
void wm_fill_rect(wm_window_t *w, int x, int y, int cw, int ch, fb_color_t c);
void wm_draw_3d_rect(wm_window_t *w, int x, int y, int cw, int ch, int raised);
void wm_flush(wm_window_t *w);     /* Flush client area to screen */

/* ============================================================
 * Desktop
 * ============================================================ */

/** Draw the desktop background */
void wm_draw_desktop(void);

/** Draw the taskbar */
void wm_draw_taskbar(void);

/** Toggle Start menu (stub) */
void wm_toggle_start_menu(void);

/* ============================================================
 * Built-in applications
 * ============================================================ */

/** Open a terminal window (runs ksh inside a window) */
wm_window_t *wm_open_terminal(void);

/** Open a "About Picomimi" dialog */
wm_window_t *wm_open_about(void);

/** Open a system monitor window */
wm_window_t *wm_open_sysmon(void);

#endif /* _WM_WM_H */
