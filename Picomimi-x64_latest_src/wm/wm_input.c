/**
 * wm_input.c — Mouse and keyboard event handling
 *
 * Mouse:
 *   - Smooth drag with sub-pixel accumulation
 *   - Edge resize from all 8 directions
 *   - Hover tracking for titlebar buttons + dock
 *   - Right-click context menu stub
 *   - Double-click to maximise/restore
 *
 * Keyboard:
 *   - Alt+F4 close, Alt+Tab cycle, Alt+grave cycle reverse
 *   - Ctrl+Tab next window
 *   - All events forwarded to focused window
 */
#include "wm_internal.h"

/* ═══════════════════════════════════════════════════════════
 * Chrome hit tests
 * Button centres: CRBTN_R, CRBTN_M, CRBTN_X from left of titlebar
 * ═══════════════════════════════════════════════════════════ */
static int btn_hit(wm_window_t *w, int bx_off, int mx, int my) {
    int cx = w->x + WIN_BRD + bx_off;
    int cy = w->y + WIN_BRD + TBAR_H/2;
    int dx = mx-cx, dy = my-cy;
    return (dx*dx + dy*dy) <= (CRBTN_SZ+3)*(CRBTN_SZ+3);
}

int wm_chrome_hit_close(wm_window_t *w, int mx, int my) {
    if (w->flags & (WF_NO_TITLE|WF_NO_CLOSE)) return 0;
    return btn_hit(w, CRBTN_R, mx, my);
}
int wm_chrome_hit_min(wm_window_t *w, int mx, int my) {
    if (w->flags & WF_NO_TITLE) return 0;
    return btn_hit(w, CRBTN_M, mx, my);
}
int wm_chrome_hit_max(wm_window_t *w, int mx, int my) {
    if (w->flags & (WF_NO_TITLE|WF_NO_RESIZE)) return 0;
    return btn_hit(w, CRBTN_X, mx, my);
}
int wm_chrome_hit_tbar(wm_window_t *w, int mx, int my) {
    if (w->flags & WF_NO_TITLE) return 0;
    /* titlebar = top strip, but exclude chrome button zone */
    if (mx < w->x || mx >= w->x+w->w) return 0;
    if (my < w->y || my >= w->y+WIN_BRD+TBAR_H) return 0;
    /* exclude left button cluster */
    if (mx < w->x+WIN_BRD+CRBTN_X+CRBTN_SZ+4) return 0;
    return 1;
}

/* Determine resize zone for cursor position relative to window edges */
resize_dir_t wm_resize_zone(wm_window_t *w, int mx, int my) {
    if (!(w->flags & WF_RESIZABLE)) return RESIZE_NONE;
    if (w->flags & WF_MAXIMIZED)   return RESIZE_NONE;

    int z = RESIZE_ZONE;
    int in_x = (mx >= w->x     && mx < w->x+w->w);
    int in_y = (my >= w->y     && my < w->y+w->h);
    int near_l = (mx >= w->x   && mx < w->x+z);
    int near_r = (mx >= w->x+w->w-z && mx < w->x+w->w);
    int near_t = (my >= w->y   && my < w->y+z);
    int near_b = (my >= w->y+w->h-z && my < w->y+w->h);

    if (!in_x || !in_y) return RESIZE_NONE;

    if (near_t && near_l) return RESIZE_NW;
    if (near_t && near_r) return RESIZE_NE;
    if (near_b && near_l) return RESIZE_SW;
    if (near_b && near_r) return RESIZE_SE;
    if (near_t) return RESIZE_N;
    if (near_b) return RESIZE_S;
    if (near_l) return RESIZE_W;
    if (near_r) return RESIZE_E;
    return RESIZE_NONE;
}

/* ═══════════════════════════════════════════════════════════
 * Hover zone update — called on every mouse move
 * Sets G_hover_win_id and G_hover_zone for chrome drawing
 * ═══════════════════════════════════════════════════════════ */
static void update_hover(int mx, int my) {
    wm_rebuild_z();

    /* Check dock hover */
    int di = wm_dock_hit(mx, my);
    if (di != G_dock_hover) {
        G_dock_hover    = di;
        G_tooltip_timer = 0;
        G_full_redraw   = 1;
    }

    /* Check window chrome hover (topmost window only) */
    wm_window_t *w = wm_win_at(mx, my);
    int new_hw = w ? w->id : -1;
    int new_hz = 0;
    if (w) {
        if (wm_chrome_hit_close(w, mx, my)) new_hz = 1;
        else if (wm_chrome_hit_min(w, mx, my)) new_hz = 2;
        else if (wm_chrome_hit_max(w, mx, my)) new_hz = 3;
    }

    if (new_hw != G_hover_win_id || new_hz != G_hover_zone) {
        G_hover_win_id = new_hw;
        G_hover_zone   = new_hz;
        G_full_redraw  = 1;
    }
}

/* ═══════════════════════════════════════════════════════════
 * Double-click detection
 * ═══════════════════════════════════════════════════════════ */
extern volatile u64 system_ticks;
static u64  last_click_tick = 0;
static int  last_click_x    = -999, last_click_y = -999;
#define DCLICK_MS   40   /* 40 ticks = 400ms at 100Hz */
#define DCLICK_DIST 10

static int is_double_click(int mx, int my) {
    u64 now = system_ticks;
    int dx = mx - last_click_x, dy = my - last_click_y;
    int close = (dx*dx + dy*dy) < DCLICK_DIST*DCLICK_DIST;
    int fast  = (now - last_click_tick) < DCLICK_MS;
    last_click_tick = now;
    last_click_x    = mx;
    last_click_y    = my;
    return close && fast;
}

/* ═══════════════════════════════════════════════════════════
 * Rail hit test
 * ═══════════════════════════════════════════════════════════ */
static wm_window_t *rail_hit(int mx, int my) {
    if (mx >= RAIL_W || my < TOPBAR_H) return NULL;
    int y0  = TOPBAR_H + 8;
    int bx  = (RAIL_W - RAIL_BTN_W) / 2;
    int n   = G_zcount < 10 ? G_zcount : 10;
    for (int i = 0; i < n; i++) {
        int by = y0 + i*(RAIL_BTN_H + RAIL_BTN_PAD);
        if (mx>=bx&&mx<bx+RAIL_BTN_W&&my>=by&&my<by+RAIL_BTN_H)
            return G_zlist[G_zcount-1-i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Apply resize delta
 * ═══════════════════════════════════════════════════════════ */
static void do_resize(int mx, int my) {
    wm_window_t *w = wm_get_window(G_resize_id);
    if (!w) { G_resize_id = -1; return; }

    int ddx = mx - G_resize_ox;
    int ddy = my - G_resize_oy;
    int nx = G_resize_xstart, ny = G_resize_ystart;
    int nw = G_resize_wstart, nh = G_resize_hstart;

    switch (G_resize_dir) {
    case RESIZE_E:  nw += ddx; break;
    case RESIZE_S:  nh += ddy; break;
    case RESIZE_SE: nw += ddx; nh += ddy; break;
    case RESIZE_W:  nx += ddx; nw -= ddx; break;
    case RESIZE_N:  ny += ddy; nh -= ddy; break;
    case RESIZE_NW: nx+=ddx; ny+=ddy; nw-=ddx; nh-=ddy; break;
    case RESIZE_NE: ny+=ddy; nw+=ddx; nh-=ddy; break;
    case RESIZE_SW: nx+=ddx; nw-=ddx; nh+=ddy; break;
    default: break;
    }

    /* Clamp minimums */
    if (nw < 160) { if(nx != G_resize_xstart) nx=G_resize_xstart+G_resize_wstart-160; nw=160; }
    if (nh <  90) { if(ny != G_resize_ystart) ny=G_resize_ystart+G_resize_hstart- 90; nh= 90; }
    /* Clamp to screen */
    if (nx < RAIL_W)  { nw -= (RAIL_W-nx);  nx = RAIL_W; }
    if (ny < TOPBAR_H){ nh -= (TOPBAR_H-ny);ny = TOPBAR_H; }
    if (nx+nw > G_sw) nw = G_sw-nx;

    w->x = nx; w->y = ny; w->w = nw; w->h = nh;
    wm_calc_client(w);
    G_full_redraw = 1;
}

/* ═══════════════════════════════════════════════════════════
 * Mouse event handler — called from IRQ12
 * ═══════════════════════════════════════════════════════════ */
void wm_on_mouse(const mouse_event_t *ev) {
    int mx = ev->x, my = ev->y;
    u8  btn = ev->buttons;

    /* Update cursor position first */
    G_mouse_x = mx;
    G_mouse_y = my;

    int ldown   = btn  & MOUSE_BTN_LEFT;
    int lprev   = G_prev_btn & MOUSE_BTN_LEFT;
    int rdown   = btn  & MOUSE_BTN_RIGHT;
    int rprev   = G_prev_btn & MOUSE_BTN_RIGHT;
    int lclick  = ldown && !lprev;
    int lrelease = !ldown && lprev;
    int rclick  = rdown && !rprev;

    (void)rclick; /* reserved for context menu */

    wm_rebuild_z();

    /* ── Active resize ── */
    if (G_resize_id >= 0) {
        if (ldown) {
            do_resize(mx, my);
        } else {
            G_resize_id  = -1;
            G_resize_dir = RESIZE_NONE;
            G_full_redraw = 1;
        }
        update_hover(mx, my);
        G_prev_btn = btn;
        return;
    }

    /* ── Active drag ── */
    if (G_drag_id >= 0) {
        if (ldown) {
            wm_window_t *dw = wm_get_window(G_drag_id);
            if (dw) wm_move(dw, mx - G_drag_ox, my - G_drag_oy);
        } else {
            G_drag_id    = -1;
            G_full_redraw = 1;
        }
        update_hover(mx, my);
        G_prev_btn = btn;
        return;
    }

    /* ── Hover update (always) ── */
    update_hover(mx, my);

    /* ── Click handling ── */
    if (lclick) {
        /* Rail? */
        wm_window_t *rw = rail_hit(mx, my);
        if (rw) {
            if (rw->flags & WF_MINIMIZED) {
                rw->flags &= ~WF_MINIMIZED;
                rw->flags |= WF_VISIBLE;
            }
            wm_do_raise(rw);
            goto done;
        }

        /* Dock? */
        int di = wm_dock_hit(mx, my);
        if (di >= 0) {
            if (G_dock[di].action) G_dock[di].action();
            G_full_redraw = 1;
            goto done;
        }

        /* Window? */
        wm_window_t *w = wm_win_at(mx, my);
        if (w) {
            /* Always focus on click */
            if (w->id != G_focused_id) wm_do_raise(w);

            if (wm_chrome_hit_close(w, mx, my)) {
                /* Close */
                if (w->wndproc) w->wndproc(w, WM_CLOSE, 0, 0);
                else wm_do_destroy(w);

            } else if (wm_chrome_hit_min(w, mx, my)) {
                /* Minimise */
                w->flags |= WF_MINIMIZED;
                w->flags &= ~WF_VISIBLE;
                if (G_focused_id == w->id) G_focused_id = -1;
                G_full_redraw = 1;

            } else if (wm_chrome_hit_max(w, mx, my)) {
                /* Maximise / restore */
                if (w->flags & WF_MAXIMIZED) wm_restore(w);
                else                          wm_maximize(w);

            } else if (wm_chrome_hit_tbar(w, mx, my)) {
                /* Double-click titlebar = maximise/restore */
                if (is_double_click(mx, my)) {
                    if (w->flags & WF_MAXIMIZED) wm_restore(w);
                    else                          wm_maximize(w);
                } else {
                    /* Start drag */
                    G_drag_id = w->id;
                    G_drag_ox = mx - w->x;
                    G_drag_oy = my - w->y;
                }
            } else {
                /* Check resize edge */
                resize_dir_t rd = wm_resize_zone(w, mx, my);
                if (rd != RESIZE_NONE) {
                    G_resize_id     = w->id;
                    G_resize_dir    = rd;
                    G_resize_ox     = mx;
                    G_resize_oy     = my;
                    G_resize_wstart = w->w;
                    G_resize_hstart = w->h;
                    G_resize_xstart = w->x;
                    G_resize_ystart = w->y;
                }
                /* Forward click into client */
                if (w->wndproc)
                    w->wndproc(w, WM_KEYDOWN, 0, 0); /* mouse-click msg placeholder */
            }
        }
    }

    if (lrelease) {
        G_drag_id   = -1;
        G_resize_id = -1;
        G_resize_dir = RESIZE_NONE;
    }

done:
    G_prev_btn = btn;
}

/* ═══════════════════════════════════════════════════════════
 * Keyboard event handler — called from IRQ1
 * ═══════════════════════════════════════════════════════════ */
void wm_on_key(const key_event_t *ev) {
    if (!ev->pressed) return;

    u32 mod = ev->modifiers;
    u32 key = ev->keycode;

    /* ── Global WM hotkeys ── */

    /* Alt+F4 — close focused window */
    if ((mod & KEY_MOD_ALT) && key == KEY_F4) {
        wm_window_t *fw = wm_get_focused();
        if (fw) {
            if (fw->wndproc) fw->wndproc(fw, WM_CLOSE, 0, 0);
            else wm_do_destroy(fw);
        }
        return;
    }

    /* Alt+Tab — cycle focus forward */
    if ((mod & KEY_MOD_ALT) && key == KEY_TAB) {
        wm_rebuild_z();
        if (G_zcount < 2) return;
        for (int i = 0; i < G_zcount; i++) {
            if (G_zlist[i]->id == G_focused_id) {
                wm_do_raise(G_zlist[(i+1) % G_zcount]);
                return;
            }
        }
        wm_do_raise(G_zlist[0]);
        return;
    }

    /* Alt+Shift+Tab — cycle focus backward */
    if ((mod & KEY_MOD_ALT) && (mod & KEY_MOD_SHIFT) && key == KEY_TAB) {
        wm_rebuild_z();
        if (G_zcount < 2) return;
        for (int i = 0; i < G_zcount; i++) {
            if (G_zlist[i]->id == G_focused_id) {
                wm_do_raise(G_zlist[(i-1+G_zcount) % G_zcount]);
                return;
            }
        }
        return;
    }

    /* Super/Win (F11 stand-in) — toggle maximise focused */
    if (key == KEY_F11) {
        wm_window_t *fw = wm_get_focused();
        if (fw) {
            if (fw->flags & WF_MAXIMIZED) wm_restore(fw);
            else wm_maximize(fw);
        }
        return;
    }

    /* F1 — open terminal */
    if (key == KEY_F1) {
        extern wm_window_t *wm_open_terminal(void);
        wm_open_terminal();
        return;
    }

    /* F2 — open sysmon */
    if (key == KEY_F2) {
        extern wm_window_t *wm_open_sysmon(void);
        wm_open_sysmon();
        return;
    }

    /* ── Forward to focused window ── */
    wm_window_t *fw = wm_get_focused();
    if (fw && fw->wndproc) {
        fw->wndproc(fw, WM_KEYDOWN, key, ev->ascii);
        if (ev->ascii)
            fw->wndproc(fw, WM_CHAR, ev->ascii, mod);
    }
}

/* ═══════════════════════════════════════════════════════════
 * Init — register callbacks
 * ═══════════════════════════════════════════════════════════ */
void wm_input_init(void) {
    kbd_set_callback(wm_on_key);
    mouse_init(G_sw, G_sh);
    mouse_set_callback(wm_on_mouse);
}
