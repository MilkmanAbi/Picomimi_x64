/**
 * wm_core.c — Window pool, Z-order, public API, main tick loop
 */
#include "wm_internal.h"

/* ═══════════════════════════════════════════════════════════
 * Global state definitions
 * ═══════════════════════════════════════════════════════════ */
wm_window_t  G_wins[MAX_WINS];
int          G_sw = 0, G_sh = 0;
int          G_focused_id   = -1;
int          G_next_z       = 1;
int          G_next_id      = 1;
volatile int G_full_redraw  = 1;
int          G_wm_running   = 0;

int          G_drag_id      = -1;
int          G_drag_ox      = 0, G_drag_oy = 0;

int          G_resize_id    = -1;
resize_dir_t G_resize_dir   = RESIZE_NONE;
int          G_resize_ox    = 0, G_resize_oy  = 0;
int          G_resize_wstart= 0, G_resize_hstart = 0;
int          G_resize_xstart= 0, G_resize_ystart  = 0;

int          G_mouse_x      = 0, G_mouse_y   = 0;
u8           G_prev_btn     = 0;
int          G_hover_win_id = -1;
int          G_hover_zone   = 0;

int          G_tooltip_timer    = 0;
int          G_tooltip_dock_idx = -1;
int          G_dock_hover       = -1;

wm_window_t *G_zlist[MAX_WINS];
int          G_zcount = 0;

/* ═══════════════════════════════════════════════════════════
 * Z-order rebuild — insertion sort on z value
 * ═══════════════════════════════════════════════════════════ */
void wm_rebuild_z(void) {
    G_zcount = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        wm_window_t *w = &G_wins[i];
        if (!w->id) continue;
        if (!(w->flags & WF_VISIBLE)) continue;
        if (w->flags & WF_MINIMIZED) continue;
        G_zlist[G_zcount++] = w;
    }
    /* insertion sort ascending z (bottom -> top) */
    for (int i = 1; i < G_zcount; i++) {
        wm_window_t *key = G_zlist[i];
        int j = i - 1;
        while (j >= 0 && G_zlist[j]->z > key->z) {
            G_zlist[j+1] = G_zlist[j];
            j--;
        }
        G_zlist[j+1] = key;
    }
}

/* ═══════════════════════════════════════════════════════════
 * Hit testing
 * ═══════════════════════════════════════════════════════════ */
wm_window_t *wm_win_at(int x, int y) {
    /* top-most first */
    for (int i = G_zcount-1; i >= 0; i--) {
        wm_window_t *w = G_zlist[i];
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h)
            return w;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Client rect
 * ═══════════════════════════════════════════════════════════ */
void wm_calc_client(wm_window_t *w) {
    int brd = WIN_BRD;
    if (w->flags & WF_NO_TITLE) {
        w->client_x = w->x + brd;
        w->client_y = w->y + brd;
        w->client_w = w->w - brd*2;
        w->client_h = w->h - brd*2;
    } else {
        w->client_x = w->x + brd;
        w->client_y = w->y + brd + TBAR_H;
        w->client_w = w->w - brd*2;
        w->client_h = w->h - brd*2 - TBAR_H;
    }
    if (w->client_w < 0) w->client_w = 0;
    if (w->client_h < 0) w->client_h = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Raise + focus
 * ═══════════════════════════════════════════════════════════ */
void wm_do_raise(wm_window_t *w) {
    if (!w) return;
    /* unfocus old */
    wm_window_t *old = wm_get_window(G_focused_id);
    if (old && old != w) {
        old->flags &= ~WF_FOCUSED;
        if (old->wndproc) old->wndproc(old, WM_FOCUS_OUT, 0, 0);
    }
    w->z = G_next_z++;
    w->flags |= WF_FOCUSED | WF_DIRTY;
    G_focused_id = w->id;
    if (w->wndproc) w->wndproc(w, WM_FOCUS_IN, 0, 0);
    G_full_redraw = 1;
}

/* ═══════════════════════════════════════════════════════════
 * Destroy
 * ═══════════════════════════════════════════════════════════ */
void wm_do_destroy(wm_window_t *w) {
    if (!w || !w->id) return;
    if (w->backbuf)  { kfree(w->backbuf);  w->backbuf  = NULL; }
    if (w->userdata) { kfree(w->userdata); w->userdata = NULL; }
    if (G_focused_id == w->id) G_focused_id = -1;
    if (G_drag_id    == w->id) G_drag_id    = -1;
    if (G_resize_id  == w->id) G_resize_id  = -1;
    memset(w, 0, sizeof(*w));
    G_full_redraw = 1;
}

/* ═══════════════════════════════════════════════════════════
 * wm_tick — 100Hz timer IRQ
 * ═══════════════════════════════════════════════════════════ */
void wm_tick(void) {
    if (!G_wm_running) return;

    static u32 ticks = 0;
    ticks++;

    /* Clock ticks every second */
    if (ticks % 100 == 0) G_full_redraw = 1;

    /* Dock hover tooltip */
    if (G_dock_hover >= 0) {
        G_tooltip_timer++;
        if (G_tooltip_timer == TOOLTIP_DELAY) G_full_redraw = 1;
    } else {
        if (G_tooltip_timer > 0) G_full_redraw = 1;
        G_tooltip_timer = 0;
    }

    if (G_full_redraw) {
        wm_repaint_all();
        wm_cur_draw(G_mouse_x, G_mouse_y);
        G_full_redraw = 0;
    } else {
        /* Incremental: for continuously-dirty windows (terminal, sysmon)
         * just repaint that window's chrome+client without touching others.
         * Drag/resize already sets G_full_redraw, so we only hit this path
         * for blink-type updates. Redraw ALL in z-order to handle shadows. */
        int any = 0;
        wm_rebuild_z();
        for (int i = 0; i < G_zcount; i++) {
            if (G_zlist[i]->flags & WF_DIRTY) { any = 1; break; }
        }
        if (any) {
            /* Full composite repaint — correct shadow/overlap handling */
            wm_draw_desktop();
            for (int i = 0; i < G_zcount; i++) {
                wm_window_t *w = G_zlist[i];
                int was_dirty = (w->flags & WF_DIRTY);
                w->flags &= ~WF_DIRTY;
                wm_draw_chrome(w);
                if (w->wndproc) w->wndproc(w, WM_PAINT, 0, 0);
                if (was_dirty) { /* leave dirty if self-set during paint */ }
            }
            wm_draw_topbar();
            wm_draw_rail();
            wm_draw_dock();
        }
        /* Cursor always redrawn */
        wm_cur_restore();
        wm_cur_draw(G_mouse_x, G_mouse_y);
    }

    fb_flip();
}

/* ═══════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════ */
void wm_init(void) {
    const fb_info_t *info = fb_get_info();
    if (!info->initialized) {
        printk(KERN_ERR "[WM] no framebuffer\n");
        return;
    }
    G_sw = info->width;
    G_sh = info->height;

    memset(G_wins, 0, sizeof(G_wins));
    G_focused_id = -1; G_next_z = 1; G_next_id = 1;
    G_full_redraw = 1; G_wm_running = 0;
    G_mouse_x = G_sw/2; G_mouse_y = G_sh/2;
    G_drag_id = -1; G_resize_id = -1;
    G_prev_btn = 0; G_dock_hover = -1;

    wm_draw_init();
    wm_input_init();
    wm_apps_init();

    printk(KERN_INFO "[WM] PaperWM v2 ready (%dx%d)\n", G_sw, G_sh);
    wm_repaint_all();
    wm_cur_draw(G_mouse_x, G_mouse_y);
    fb_flip();
}

void wm_run(void) {
    G_wm_running = 1;
    printk(KERN_INFO "[WM] entering main loop\n");
    extern wm_window_t *wm_open_terminal(void);
    extern wm_window_t *wm_open_sysmon(void);
    wm_open_sysmon();
    wm_open_terminal();
    for (;;) __asm__ volatile("hlt");
}

wm_window_t *wm_create_window(int x, int y, int w, int h,
                               const char *title, u32 flags,
                               wm_wndproc_t proc) {
    wm_window_t *win = NULL;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!G_wins[i].id) { win = &G_wins[i]; break; }
    }
    if (!win) return NULL;

    win->id = G_next_id++;
    win->x  = x; win->y = y;
    win->w  = (w < 160) ? 160 : w;
    win->h  = (h <  90) ?  90 : h;
    win->flags   = flags | WF_VISIBLE | WF_DIRTY;
    win->z       = G_next_z++;
    win->wndproc = proc;
    win->userdata = win->backbuf = NULL;
    win->is_dragging = 0;

    int tl = 0;
    while (tl < 63 && title[tl]) { win->title[tl] = title[tl]; tl++; }
    win->title[tl] = 0;

    wm_calc_client(win);
    G_focused_id = win->id;
    G_full_redraw = 1;
    printk(KERN_INFO "[WM] new window '%s' %dx%d @ %d,%d\n",
           win->title, win->w, win->h, x, y);
    return win;
}

void wm_destroy_window(wm_window_t *w) { wm_do_destroy(w); }

void wm_show(wm_window_t *w) {
    if (w) { w->flags |= WF_VISIBLE|WF_DIRTY; G_full_redraw = 1; }
}
void wm_hide(wm_window_t *w) {
    if (w) { w->flags &= ~WF_VISIBLE; G_full_redraw = 1; }
}
void wm_raise(wm_window_t *w) { wm_do_raise(w); }

void wm_move(wm_window_t *w, int x, int y) {
    if (!w) return;
    /* Clamp: can't go behind rail or above topbar */
    if (x < RAIL_W)  x = RAIL_W;
    if (y < TOPBAR_H) y = TOPBAR_H;
    /* Clamp right/bottom so titlebar is always reachable */
    if (x + w->w > G_sw) x = G_sw - w->w;
    int dock_top = G_sh - DOCK_MARG - DOCK_PAD*2 - DOCK_BTN_SZ;
    if (y + TBAR_H > dock_top) y = dock_top - TBAR_H;
    w->x = x; w->y = y;
    wm_calc_client(w);
    G_full_redraw = 1;
}

void wm_resize(wm_window_t *w, int nw, int nh) {
    if (!w) return;
    if (nw < 160) nw = 160;
    if (nh <  90) nh =  90;
    w->w = nw; w->h = nh;
    wm_calc_client(w);
    if (w->wndproc) w->wndproc(w, WM_RESIZE, nw, nh);
    G_full_redraw = 1;
}

void wm_maximize(wm_window_t *w) {
    if (!w || (w->flags & WF_MAXIMIZED)) return;
    w->saved_x = w->x; w->saved_y = w->y;
    w->saved_w = w->w; w->saved_h = w->h;
    w->flags |= WF_MAXIMIZED;
    w->x = RAIL_W; w->y = TOPBAR_H;
    w->w = G_sw - RAIL_W;
    w->h = G_sh - TOPBAR_H - (DOCK_PAD*2 + DOCK_BTN_SZ + DOCK_MARG);
    wm_calc_client(w);
    G_full_redraw = 1;
}

void wm_restore(wm_window_t *w) {
    if (!w || !(w->flags & WF_MAXIMIZED)) return;
    w->flags &= ~WF_MAXIMIZED;
    w->x = w->saved_x; w->y = w->saved_y;
    w->w = w->saved_w; w->h = w->saved_h;
    wm_calc_client(w);
    G_full_redraw = 1;
}

void wm_invalidate(wm_window_t *w) {
    if (w) w->flags |= WF_DIRTY;
}

wm_window_t *wm_get_window(int id) {
    for (int i = 0; i < MAX_WINS; i++)
        if (G_wins[i].id == id) return &G_wins[i];
    return NULL;
}
wm_window_t *wm_get_focused(void) { return wm_get_window(G_focused_id); }

/* Drawing helpers for apps */
void wm_fill_client(wm_window_t *w, fb_color_t c) {
    if (!w) return;
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, c);
}
void wm_draw_text(wm_window_t *w, int x, int y,
                  const char *s, fb_color_t fg, fb_color_t bg) {
    if (!w || !s) return;
    fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
    fb_draw_string(w->client_x+x, w->client_y+y, s, fg, bg);
    fb_clear_clip();
}
void wm_fill_rect(wm_window_t *w, int x, int y, int cw, int ch, fb_color_t c) {
    if (!w) return;
    fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
    fb_fill_rect(w->client_x+x, w->client_y+y, cw, ch, c);
    fb_clear_clip();
}
void wm_draw_3d_rect(wm_window_t *w, int x, int y, int cw, int ch, int raised) {
    if (!w) return;
    fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
    fb_draw_3d_rect(w->client_x+x, w->client_y+y, cw, ch, raised);
    fb_clear_clip();
}
void wm_flush(wm_window_t *w) { (void)w; }

/* Legacy stubs */
void wm_draw_taskbar(void)      { wm_draw_topbar(); wm_draw_dock(); }
void wm_toggle_start_menu(void) { }
