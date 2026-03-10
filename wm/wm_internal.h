/**
 * wm_internal.h - Private header shared across WM source files
 *
 * Include only from wm_core.c, wm_draw.c, wm_input.c, wm_apps.c
 */
#pragma once
#include <kernel/types.h>
#include <wm/wm.h>
#include <drivers/fb.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <mm/slab.h>
#include <arch/io.h>
#include "icons.h"

/* ═══════════════════════════════════════════════════════════
 * Palette
 * ═══════════════════════════════════════════════════════════ */
#define C_BG          FB_COLOR(0xF4,0xF4,0xEF)
#define C_BG2         FB_COLOR(0xF0,0xF0,0xEB)
#define C_GRID_MAJ    FB_COLOR(0xD4,0xD4,0xCC)
#define C_GRID_MIN    FB_COLOR(0xE4,0xE4,0xDC)
#define C_INK         FB_COLOR(0x18,0x18,0x18)
#define C_INK2        FB_COLOR(0x44,0x44,0x44)
#define C_MID         FB_COLOR(0x88,0x88,0x80)
#define C_LGT         FB_COLOR(0xCC,0xCC,0xC4)
#define C_LGT2        FB_COLOR(0xE0,0xE0,0xD8)
#define C_WHITE       FB_COLOR(0xFF,0xFF,0xFF)
#define C_TOPBAR      FB_COLOR(0xFF,0xFF,0xFF)
#define C_TOPBAR_LINE FB_COLOR(0xE0,0xE0,0xD8)
#define C_RAIL        FB_COLOR(0xF0,0xF0,0xEB)
#define C_RAIL_LINE   FB_COLOR(0xD8,0xD8,0xD0)
#define C_DOCK_BG     FB_COLOR(0xF8,0xF8,0xF3)
#define C_DOCK_LINE   FB_COLOR(0xCC,0xCC,0xC2)
#define C_DOCK_SHADOW FB_COLOR(0xC0,0xC0,0xB8)
#define C_WIN_BG      FB_COLOR(0xFF,0xFF,0xFF)
#define C_WIN_BORDER  FB_COLOR(0xC0,0xC0,0xB8)
#define C_WIN_SHADOW  FB_COLOR(0xCC,0xCC,0xC4)
#define C_TBAR_F      FB_COLOR(0xF5,0xF5,0xF0)
#define C_TBAR_U      FB_COLOR(0xFB,0xFB,0xF8)
#define C_TBAR_LINE   FB_COLOR(0xD8,0xD8,0xD0)
#define C_TBAR_TEXT_F FB_COLOR(0x18,0x18,0x18)
#define C_TBAR_TEXT_U FB_COLOR(0xAA,0xAA,0xA0)
#define C_BTN_CLOSE   FB_COLOR(0xFF,0x5F,0x56)
#define C_BTN_MIN     FB_COLOR(0xFF,0xBD,0x2E)
#define C_BTN_MAX     FB_COLOR(0x27,0xC9,0x3F)
#define C_BTN_HOVER   FB_COLOR(0x22,0x22,0x22)
#define C_SEL_BG      FB_COLOR(0x31,0x7A,0xD1)
#define C_SEL_FG      C_WHITE
#define C_TERM_BG     FB_COLOR(0x1A,0x1A,0x2E)
#define C_TERM_FG     FB_COLOR(0xD4,0xD4,0xD0)
#define C_TERM_GRN    FB_COLOR(0x50,0xFA,0x7B)
#define C_TERM_YEL    FB_COLOR(0xF1,0xFA,0x8C)
#define C_TERM_CYA    FB_COLOR(0x8B,0xE9,0xFD)
#define C_TERM_CUR    FB_COLOR(0xFF,0xFF,0xFF)
#define C_HOVER       FB_COLOR(0xE8,0xE8,0xE0)
#define C_NOTIFY      FB_COLOR(0xFF,0x5F,0x56)

/* ═══════════════════════════════════════════════════════════
 * Layout constants
 * ═══════════════════════════════════════════════════════════ */
#define TOPBAR_H      24
#define RAIL_W        40
#define TBAR_H        22
#define WIN_BRD        1
#define WIN_RAD        7
#define SHADOW_OFF     4
#define SHADOW_BLR     3
#define GRID_MAJOR    54
#define GRID_MINOR    18
#define DOCK_BTN_SZ   44
#define DOCK_PAD       8
#define DOCK_GAP       6
#define DOCK_SEP      10
#define DOCK_MARG     14
#define DOCK_RAD      16
#define BTN_RAD        4
#define RESIZE_ZONE    5
#define MAX_WINS      24
#define RAIL_BTN_H    32
#define RAIL_BTN_W    34
#define RAIL_BTN_PAD   3
#define TOOLTIP_DELAY 60

/* Chrome button positions (right to left from window edge) */
#define CRBTN_R  10
#define CRBTN_M  26
#define CRBTN_X  42
#define CRBTN_Y  11
#define CRBTN_SZ  8

/* ═══════════════════════════════════════════════════════════
 * Internal window state (extends wm_window_t via userdata chain)
 * ═══════════════════════════════════════════════════════════ */
typedef enum {
    RESIZE_NONE = 0,
    RESIZE_N, RESIZE_S, RESIZE_E, RESIZE_W,
    RESIZE_NE, RESIZE_NW, RESIZE_SE, RESIZE_SW
} resize_dir_t;

/* ═══════════════════════════════════════════════════════════
 * Global WM state - shared across wm_*.c files
 * ═══════════════════════════════════════════════════════════ */
extern wm_window_t  G_wins[MAX_WINS];
extern int          G_sw, G_sh;
extern int          G_focused_id;
extern int          G_next_z;
extern int          G_next_id;
extern volatile int G_full_redraw;
extern int          G_wm_running;

/* Drag state */
extern int          G_drag_id;
extern int          G_drag_ox, G_drag_oy;

/* Resize state */
extern int          G_resize_id;
extern resize_dir_t G_resize_dir;
extern int          G_resize_ox, G_resize_oy;
extern int          G_resize_wstart, G_resize_hstart;
extern int          G_resize_xstart, G_resize_ystart;

/* Mouse state */
extern int          G_mouse_x, G_mouse_y;
extern u8           G_prev_btn;
extern int          G_hover_win_id;   /* window under cursor */
extern int          G_hover_zone;     /* 0=none 1=close 2=min 3=max */

/* Tooltip state */
extern int          G_tooltip_timer;
extern int          G_tooltip_dock_idx;

/* Z-order list */
extern wm_window_t *G_zlist[MAX_WINS];
extern int          G_zcount;

/* Dock hover */
extern int          G_dock_hover;    /* -1 = none, else icon index */

/* ═══════════════════════════════════════════════════════════
 * Functions exported between wm_*.c files
 * ═══════════════════════════════════════════════════════════ */

/* wm_core.c */
void     wm_rebuild_z(void);
wm_window_t *wm_win_at(int x, int y);
void     wm_calc_client(wm_window_t *w);
void     wm_do_raise(wm_window_t *w);
void     wm_do_destroy(wm_window_t *w);

/* wm_draw.c */
void     wm_draw_init(void);
void     wm_repaint_all(void);
void     wm_draw_chrome(wm_window_t *w);
void     wm_draw_desktop(void);
void     wm_draw_topbar(void);
void     wm_draw_rail(void);
void     wm_draw_dock(void);
void     wm_cur_save(int x, int y);
void     wm_cur_restore(void);
void     wm_cur_draw(int x, int y);
void     blit_icon_sz(int cx, int cy, const icon_pixel_t *icon,
                      int src_w, int src_h, int dst_w, int dst_h,
                      fb_color_t bg);

/* drawing primitives exported for apps */
void     wm_rrect(int x, int y, int w, int h, int r,
                  fb_color_t fill, fb_color_t bord);
void     wm_rrect_fill(int x, int y, int w, int h, int r, fb_color_t fill);
void     wm_shadow(int x, int y, int w, int h, int r);
void     wm_uitoa(char *buf, u64 v);
void     wm_itoa(char *buf, s64 v);
int      wm_slen(const char *s);

/* wm_input.c */
void     wm_input_init(void);
void     wm_on_mouse(const mouse_event_t *ev);
void     wm_on_key(const key_event_t *ev);

/* wm_apps.c */
void     wm_apps_init(void);

/* Dock entries - defined in wm_draw.c, used by wm_input.c */
typedef struct {
    const char          *label;
    const icon_pixel_t  *icon;
    void               (*action)(void);
    int                  separator_after;
} dock_entry_t;

extern dock_entry_t     G_dock[];
extern int              G_ndock;

/* wm_draw.c - dock geometry query */
int wm_dock_hit(int mx, int my);

/* hit-test helpers used by wm_input.c */
int  wm_chrome_hit_close(wm_window_t *w, int mx, int my);
int  wm_chrome_hit_min  (wm_window_t *w, int mx, int my);
int  wm_chrome_hit_max  (wm_window_t *w, int mx, int my);
int  wm_chrome_hit_tbar (wm_window_t *w, int mx, int my);
resize_dir_t wm_resize_zone(wm_window_t *w, int mx, int my);

