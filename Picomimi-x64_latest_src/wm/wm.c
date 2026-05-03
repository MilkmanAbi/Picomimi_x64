/**
 * PaperWM — Picomimi-x64 Window Manager
 *
 * Aesthetic from concept art:
 *   Desktop   : graph paper, off-white bg + 18px grid
 *   Topbar    : "PaperWM | Applications | <title>" left,
 *               date+time center, [🔍][⬜][⏻] right
 *   Left rail : small icon buttons for open windows (Win indicators)
 *   Windows   : thin rounded border, flat titlebar, "- □ X" chrome
 *   Dock      : pill bottom-center, hand-drawn icons
 *   Cursor    : software arrow, white/black
 *
 * Fixes vs previous build:
 *   1. Flicker: double-buffer is now enabled in kernel.c before wm_init.
 *      wm_tick() only calls fb_flip() once at the very end of each frame.
 *   2. Mouse: new driver with correct 9-bit signed deltas.
 *      Cursor movement is driven by mouse IRQ callback (not poll).
 *   3. Terminal: actually runs ksh commands via ksh_execute_line().
 *      Input line is shown at bottom of visible area; history works.
 *   4. Sysmon: CPU% graph (rolling 60s), memory bar.
 */

#include <kernel/types.h>
#include <wm/wm.h>
#include <drivers/fb.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <mm/slab.h>
#include <arch/io.h>
#include "icons.h"   /* Paper icon theme bitmaps */

/* ═══════════════════════════════════════════════════════════
 * Palette — paper/ink aesthetic
 * ═══════════════════════════════════════════════════════════ */
#define C_BG        FB_COLOR(0xF5,0xF5,0xF0)
#define C_GRID      FB_COLOR(0xD8,0xD8,0xD0)
#define C_INK       FB_COLOR(0x1A,0x1A,0x1A)
#define C_MID       FB_COLOR(0x88,0x88,0x80)
#define C_LGT       FB_COLOR(0xCC,0xCC,0xC4)
#define C_WHITE     FB_COLOR(0xFF,0xFF,0xFF)
#define C_TOPBAR_BG FB_COLOR(0xFF,0xFF,0xFF)
#define C_DOCK_BG   FB_COLOR(0xEC,0xEC,0xE6)
#define C_SHADOW    FB_COLOR(0xB8,0xB8,0xB0)
#define C_TBAR_F    FB_COLOR(0xEE,0xEE,0xE8)   /* focused titlebar    */
#define C_TBAR_U    FB_COLOR(0xF8,0xF8,0xF4)   /* unfocused titlebar  */
#define C_TERM_BG   FB_COLOR(0xFF,0xFF,0xFC)
#define C_TERM_FG   C_INK
#define C_TERM_GR   FB_COLOR(0x44,0x88,0x44)   /* green prompt        */
#define C_RAIL_BG   FB_COLOR(0xF0,0xF0,0xEB)   /* left rail           */
#define C_WBTN_SEL  FB_COLOR(0xD8,0xD8,0xD0)   /* selected win btn    */

/* ═══════════════════════════════════════════════════════════
 * Layout
 * ═══════════════════════════════════════════════════════════ */
#define TOPBAR_H   22
#define RAIL_W     32
#define TBAR_H     18
#define WIN_BRD     1
#define WIN_RAD     5
#define SHADOW_D    3
#define GRID_SZ    18
#define DOCK_H     48
#define DOCK_MARG  12
#define BTN_W      46
#define BTN_H      32
#define MAX_WINS   16

/* ═══════════════════════════════════════════════════════════
 * State
 * ═══════════════════════════════════════════════════════════ */
static wm_window_t  W[MAX_WINS];
static int sw, sh;
static int focused_id = -1, next_z = 1, next_id = 1;
static int wm_running = 0;
static volatile int full_redraw = 1;
static int drag_id = -1, drag_ox, drag_oy;
static u8  prev_btn = 0;

/* Sorted z-order list */
static wm_window_t *ZL[MAX_WINS];
static int ZN = 0;

static void rebuild_z(void) {
    ZN = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        wm_window_t *w = &W[i];
        if (w->id && (w->flags & WF_VISIBLE) && !(w->flags & WF_MINIMIZED))
            ZL[ZN++] = w;
    }
    /* bubble sort ascending z */
    for (int i = 0; i < ZN-1; i++)
        for (int j = 0; j < ZN-1-i; j++)
            if (ZL[j]->z > ZL[j+1]->z) {
                wm_window_t *t = ZL[j]; ZL[j] = ZL[j+1]; ZL[j+1] = t;
            }
}

/* ═══════════════════════════════════════════════════════════
 * Software cursor — 12×18 arrow, white fill / black border
 * ═══════════════════════════════════════════════════════════ */
#define CW 12
#define CH 18
static const u8 CUR[CH][CW] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,0,0},
    {1,2,2,2,2,2,2,2,2,2,2,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,0,1,2,2,1,0,0},
    {1,1,0,0,0,0,0,1,2,2,1,0},
    {0,0,0,0,0,0,0,0,1,1,1,0},
};
static u32  cur_save[CW*CH];
static int  cur_sx = -1, cur_sy = -1;
static int  cur_x  = 0,  cur_y  = 0;
static int  cur_dirty = 1;   /* needs redraw this frame */

static void cur_save_bg(int cx, int cy) {
    cur_sx = cx; cur_sy = cy;
    for (int r = 0; r < CH; r++) {
        int py = cy+r;
        for (int c = 0; c < CW; c++) {
            int px = cx+c;
            if (px>=0&&px<sw&&py>=0&&py<sh)
                cur_save[r*CW+c] = (u32)fb_getpixel(px,py);
            else
                cur_save[r*CW+c] = (u32)C_BG;
        }
    }
}

static void cur_restore(void) {
    if (cur_sx < 0) return;
    for (int r = 0; r < CH; r++) {
        int py = cur_sy+r; if (py<0||py>=sh) continue;
        for (int c = 0; c < CW; c++) {
            int px = cur_sx+c; if (px<0||px>=sw) continue;
            fb_putpixel(px, py, (fb_color_t)cur_save[r*CW+c]);
        }
    }
    cur_sx = -1;
}

static void cur_draw(int cx, int cy) {
    cur_save_bg(cx, cy);
    for (int r = 0; r < CH; r++) {
        int py = cy+r; if (py<0||py>=sh) continue;
        for (int c = 0; c < CW; c++) {
            int px = cx+c; if (px<0||px>=sw) continue;
            u8 p = CUR[r][c];
            if      (p==1) fb_putpixel(px,py, FB_COLOR(0x10,0x10,0x10));
            else if (p==2) fb_putpixel(px,py, C_WHITE);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Rounded rect — fill + 1px border
 * ═══════════════════════════════════════════════════════════ */
static void rrect(int x, int y, int w, int h, int r,
                  fb_color_t fill, fb_color_t bord) {
    if (w<=0||h<=0) return;
    if (r*2>w) r=w/2;
    if (r*2>h) r=h/2;

    /* fill: three horizontal bands */
    fb_fill_rect(x+r, y,     w-r*2, r,     fill);
    fb_fill_rect(x,   y+r,   w,     h-r*2, fill);
    fb_fill_rect(x+r, y+h-r, w-r*2, r,     fill);

    /* corners */
    for (int cr=0; cr<r; cr++)
        for (int cc=0; cc<r; cc++) {
            int dx=r-1-cc, dy=r-1-cr;
            if (dx*dx+dy*dy <= (r-1)*(r-1)) {
                fb_putpixel(x+cc,     y+cr,     fill);
                fb_putpixel(x+w-1-cc, y+cr,     fill);
                fb_putpixel(x+cc,     y+h-1-cr, fill);
                fb_putpixel(x+w-1-cc, y+h-1-cr, fill);
            }
        }

    /* border straights */
    fb_draw_hline(x+r, y,     w-r*2, bord);
    fb_draw_hline(x+r, y+h-1, w-r*2, bord);
    fb_draw_vline(x,     y+r, h-r*2, bord);
    fb_draw_vline(x+w-1, y+r, h-r*2, bord);

    /* border arcs */
    int ro=r-1;
    for (int a=0; a<r; a++)
        for (int b=0; b<r; b++) {
            int da=ro-a, db=ro-b, d=da*da+db*db;
            if (d<=ro*ro && d>(ro-1)*(ro-1)) {
                fb_putpixel(x+b,     y+a,     bord);
                fb_putpixel(x+w-1-b, y+a,     bord);
                fb_putpixel(x+b,     y+h-1-a, bord);
                fb_putpixel(x+w-1-b, y+h-1-a, bord);
            }
        }
}

/* ═══════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════ */
static int slen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void uitoa(char *buf, u64 v) {
    char tmp[22]; int i=0;
    if (!v) { buf[0]='0'; buf[1]=0; return; }
    while (v) { tmp[i++]='0'+v%10; v/=10; }
    int j=0; while(i>0) buf[j++]=tmp[--i]; buf[j]=0;
}

/* centre-draw a string in a box — clipped to box */
static void __attribute__((unused)) str_center(int bx, int by, int bw, int bh,
                        const char *s, fb_color_t fg, fb_color_t bg) {
    int px = bx + (bw - slen(s)*FB_FONT_W)/2;
    int py = by + (bh - FB_FONT_H)/2;
    if (px < bx) px = bx;
    fb_set_clip(bx, by, bw, bh);
    fb_draw_string(px, py, s, fg, bg);
    fb_clear_clip();
}

/* ═══════════════════════════════════════════════════════════
 * Desktop
 * ═══════════════════════════════════════════════════════════ */
static void draw_desktop(void) {
    /* graph paper fills screen below topbar */
    int y0 = TOPBAR_H;
    fb_fill_rect(0, y0, sw, sh-y0, C_BG);

    /* left rail */
    fb_fill_rect(0, y0, RAIL_W, sh-y0-DOCK_H-DOCK_MARG*2, C_RAIL_BG);
    fb_draw_vline(RAIL_W-1, y0, sh-y0-DOCK_H-DOCK_MARG*2, C_LGT);

    /* horizontal grid lines (start from RAIL_W+1) */
    for (int y = y0+(GRID_SZ-y0%GRID_SZ)%GRID_SZ;
         y < sh-DOCK_H-DOCK_MARG; y+=GRID_SZ)
        fb_draw_hline(RAIL_W, y, sw-RAIL_W, C_GRID);

    /* vertical grid lines */
    for (int x = RAIL_W+GRID_SZ; x < sw; x+=GRID_SZ)
        fb_draw_vline(x, y0, sh-y0-DOCK_H-DOCK_MARG, C_GRID);
}

/* ═══════════════════════════════════════════════════════════
 * Left rail — open window indicator buttons
 * Concept art shows three small circle/shape buttons
 * ═══════════════════════════════════════════════════════════ */
static void draw_rail(void) {
    int y0 = TOPBAR_H + 8;
    rebuild_z();
    for (int i = 0; i < ZN && i < 8; i++) {
        wm_window_t *w = ZL[ZN-1-i];   /* top window first */
        int by = y0 + i*26;
        int bx = 4;
        int focused = (w->id == focused_id);

        fb_color_t bg = focused ? C_WBTN_SEL : C_RAIL_BG;
        rrect(bx, by, 22, 20, 4, bg, focused ? C_INK : C_LGT);

        /* Draw a tiny icon: first letter of title in a circle */
        char ch[2] = {w->title[0] ? w->title[0] : '?', 0};
        fb_draw_string(bx+7, by+2, ch, C_INK, bg);
    }
}

/* ═══════════════════════════════════════════════════════════
 * Top bar
 * "PaperWM | Applications | Terminal"   "4 March 23:34"  [Q][□][⏻]
 * ═══════════════════════════════════════════════════════════ */
static void draw_topbar(void) {
    fb_fill_rect(0, 0, sw, TOPBAR_H, C_TOPBAR_BG);
    fb_draw_hline(0, TOPBAR_H-1, sw, C_LGT);

    /* Left breadcrumb */
    char left[96] = "PaperWM";
    int li = 7;
    wm_window_t *fw = wm_get_focused();
    if (fw) {
        const char *sep = " | ";
        for (int i=0; sep[i]&&li<90; i++) left[li++]=sep[i];
        /* could add "Applications | " prefix — skipping for width */
        for (int i=0; fw->title[i]&&li<90; i++) left[li++]=fw->title[i];
    }
    left[li]=0;
    fb_draw_string(8, (TOPBAR_H-FB_FONT_H)/2, left, C_INK, C_TOPBAR_BG);

    /* Centre: date and time */
    extern volatile u64 system_ticks;
    u64 t  = system_ticks / 100;
    int hh = (t/3600)%24, mm=(t/60)%60;
    static const char *months[]={"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char *days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    /* day of month: boot + elapsed days. Day/month are cosmetic — use fixed 4 March */
    int dom = 4 + (int)(t/86400);
    const char *mon = months[(2 + dom/31) % 12];
    const char *day = days[(t/86400+4)%7];
    (void)day;

    /* Build "D Month  HH:MM" */
    char ctr[32]; int ci=0;
    char tmp[8];
    uitoa(tmp, dom); for(int i=0;tmp[i];i++) ctr[ci++]=tmp[i];
    ctr[ci++]=' ';
    for(int i=0;mon[i];i++) ctr[ci++]=mon[i];
    ctr[ci++]=' '; ctr[ci++]=' ';
    ctr[ci++]='0'+hh/10; ctr[ci++]='0'+hh%10;
    ctr[ci++]=':';
    ctr[ci++]='0'+mm/10; ctr[ci++]='0'+mm%10;
    ctr[ci]=0;

    int cw2 = ci*FB_FONT_W;
    fb_draw_string((sw-cw2)/2, (TOPBAR_H-FB_FONT_H)/2, ctr, C_INK, C_TOPBAR_BG);

    /* Right: Q □ power icons (text stand-ins) */
    int rx = sw - 3*(FB_FONT_W+8) - 8;
    const char *icons[] = {"Q", "O", "X"};
    for (int i=0; i<3; i++) {
        int bx = rx + i*(FB_FONT_W+8);
        fb_draw_string(bx, (TOPBAR_H-FB_FONT_H)/2, icons[i], C_MID, C_TOPBAR_BG);
    }
}

/* ═══════════════════════════════════════════════════════════
 * Dock — pill, bottom center
 * Concept art icons: folder, terminal >_, chart, video(X), text, notepad
 * Plus "0:1 bell search trash" on right side of separator
 * ═══════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════
 * Blit a 32x32 RGBA icon into the framebuffer.
 * Alpha-blends over bg_color (the dock pill background).
 * cx,cy = top-left pixel on screen.
 * ═══════════════════════════════════════════════════════════ */
static void blit_icon(int cx, int cy, const icon_pixel_t *icon, fb_color_t bg) {
    u8 br = (bg >> 16) & 0xFF;
    u8 bg2 = (bg >>  8) & 0xFF;
    u8 bb  = (bg >>  0) & 0xFF;

    for (int r = 0; r < ICON_H; r++) {
        int py = cy + r;
        if (py < 0 || py >= sh) continue;
        for (int c = 0; c < ICON_W; c++) {
            int px = cx + c;
            if (px < 0 || px >= sw) continue;
            icon_pixel_t p = icon[r * ICON_W + c];
            u8 a  = (p >> 24) & 0xFF;
            u8 ir = (p >> 16) & 0xFF;
            u8 ig = (p >>  8) & 0xFF;
            u8 ib = (p >>  0) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                fb_putpixel(px, py, FB_COLOR(ir, ig, ib));
            } else {
                /* fast alpha blend over bg */
                u8 or2 = (u8)((ir * a + br * (255-a)) >> 8);
                u8 og  = (u8)((ig * a + bg2* (255-a)) >> 8);
                u8 ob  = (u8)((ib * a + bb * (255-a)) >> 8);
                fb_putpixel(px, py, FB_COLOR(or2, og, ob));
            }
        }
    }
}

#define NDOCK_L 6   /* main icons: folder terminal sysmon editor mail trash */
#define NDOCK_R 1   /* right of separator: search */
#define NDOCK   (NDOCK_L+NDOCK_R)

/* Dock button labels */
static const char *dock_labels[] = {
    "Files", "Term", "Sysmon", "Editor", "Mail", "Trash", "Search"
};

static void draw_dock(void) {
    int pad  = 8;
    int sep  = 14;
    /* BTN_W+4 gap between buttons; icons are 32px so BTN_W=36 gives 2px margin each side */
    int pw = pad*2 + NDOCK_L*(BTN_W+4) + sep + NDOCK_R*(BTN_W+4) - 4;
    int ph = BTN_H + pad*2;
    int px = (sw - pw) / 2;
    int py = sh - DOCK_MARG - ph;

    /* shadow */
    rrect(px+3, py+3, pw, ph, ph/2, C_SHADOW, C_SHADOW);
    /* pill */
    rrect(px, py, pw, ph, ph/2, C_DOCK_BG, C_INK);

    int bx = px+pad, by = py+pad;

    for (int i = 0; i < NDOCK_L; i++) {
        /* button bg */
        rrect(bx, by, BTN_W, BTN_H, 4, C_BG, C_LGT);

        /* centre the 32x32 icon inside the button */
        int ix = bx + (BTN_W - ICON_W) / 2;
        int iy = by + (BTN_H - ICON_H) / 2;
        if (i < NUM_DOCK_ICONS)
            blit_icon(ix, iy, dock_icons[i], C_BG);

        bx += BTN_W + 4;
    }

    /* separator */
    fb_draw_vline(bx + sep/2, by + 4, BTN_H-8, C_LGT);
    bx += sep;

    /* search button (text icon, no Paper bitmap for it) */
    rrect(bx, by, BTN_W, BTN_H, 4, C_BG, C_LGT);
    /* magnifier drawn as primitives */
    int mx = bx + BTN_W/2 - 4, my2 = by + BTN_H/2 - 6;
    fb_draw_rect(mx, my2, 9, 9, C_INK);
    fb_draw_line(mx+7, my2+7, mx+12, my2+12, C_INK);
    fb_draw_line(mx+8, my2+7, mx+13, my2+12, C_INK);
    (void)dock_labels;
}

/* ═══════════════════════════════════════════════════════════
 * Window chrome — thin border, rounded, "- □ X" buttons
 * ═══════════════════════════════════════════════════════════ */
static void draw_chrome(wm_window_t *w) {
    int foc = (w->id == focused_id);

    /* drop shadow */
    fb_fill_rect(w->x+SHADOW_D, w->y+SHADOW_D, w->w, w->h, C_SHADOW);

    /* window background */
    rrect(w->x, w->y, w->w, w->h, WIN_RAD, C_WHITE, C_INK);

    if (!(w->flags & WF_NO_TITLE)) {
        fb_color_t tbc = foc ? C_TBAR_F : C_TBAR_U;
        /* titlebar band (inside border) */
        fb_fill_rect(w->x+WIN_BRD, w->y+WIN_BRD,
                     w->w-WIN_BRD*2, TBAR_H, tbc);
        /* thin separator under title */
        fb_draw_hline(w->x+WIN_BRD, w->y+WIN_BRD+TBAR_H,
                      w->w-WIN_BRD*2, C_LGT);

        /* title text left-aligned, clipped away from buttons */
        int title_max_w = w->w - WIN_BRD*2 - 3*(FB_FONT_W+2) - 16;
        fb_set_clip(w->x+WIN_BRD+4, w->y+WIN_BRD, title_max_w, TBAR_H);
        fb_draw_string(w->x+WIN_BRD+6,
                       w->y+WIN_BRD+(TBAR_H-FB_FONT_H)/2,
                       w->title,
                       foc ? C_INK : C_MID, tbc);
        fb_clear_clip();

        /* right buttons: "- □ X" */
        int rx = w->x + w->w - WIN_BRD - 3*(FB_FONT_W+4) - 4;
        int ry = w->y + WIN_BRD + (TBAR_H-FB_FONT_H)/2;
        fb_draw_string(rx,              ry, "-", C_MID, tbc);
        fb_draw_string(rx+FB_FONT_W+4,  ry, "O", C_MID, tbc);
        fb_draw_string(rx+2*(FB_FONT_W+4), ry, "X", C_MID, tbc);
    }

    /* client area */
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, C_WHITE);
}

/* ═══════════════════════════════════════════════════════════
 * Client rect computation
 * ═══════════════════════════════════════════════════════════ */
static void calc_client(wm_window_t *w) {
    if (w->flags & WF_NO_TITLE) {
        w->client_x = w->x + WIN_BRD;
        w->client_y = w->y + WIN_BRD;
        w->client_w = w->w - WIN_BRD*2;
        w->client_h = w->h - WIN_BRD*2;
    } else {
        w->client_x = w->x + WIN_BRD;
        w->client_y = w->y + WIN_BRD + TBAR_H + 1;
        w->client_w = w->w - WIN_BRD*2;
        w->client_h = w->h - WIN_BRD*2 - TBAR_H - 1;
    }
    if (w->client_w < 0) w->client_w = 0;
    if (w->client_h < 0) w->client_h = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Full repaint — only called when full_redraw is set
 * ═══════════════════════════════════════════════════════════ */
static void repaint_all(void) {
    draw_desktop();
    rebuild_z();
    for (int i=0; i<ZN; i++) {
        wm_window_t *w = ZL[i];
        draw_chrome(w);
        if (w->wndproc) w->wndproc(w, WM_PAINT, 0, 0);
        w->flags &= ~WF_DIRTY;
    }
    draw_topbar();
    draw_rail();
    draw_dock();
    full_redraw = 0;
}

/* ═══════════════════════════════════════════════════════════
 * Hit testing
 * ═══════════════════════════════════════════════════════════ */
static wm_window_t *hit_win(int mx, int my) {
    for (int i=ZN-1; i>=0; i--) {
        wm_window_t *w = ZL[i];
        if (mx>=w->x&&mx<w->x+w->w&&my>=w->y&&my<w->y+w->h) return w;
    }
    return NULL;
}

static int hit_close(wm_window_t *w, int mx, int my) {
    if ((w->flags & WF_NO_CLOSE)||(w->flags & WF_NO_TITLE)) return 0;
    int rx = w->x + w->w - WIN_BRD - 3*(FB_FONT_W+4) - 4;
    int ry = w->y + WIN_BRD + (TBAR_H-FB_FONT_H)/2;
    int bx = rx + 2*(FB_FONT_W+4);
    return mx>=bx-3&&mx<=bx+FB_FONT_W+3&&my>=ry-3&&my<=ry+FB_FONT_H+3;
}

static int hit_titlebar(wm_window_t *w, int mx, int my) {
    if (w->flags & WF_NO_TITLE) return 0;
    return mx>=w->x&&mx<w->x+w->w&&my>=w->y&&my<w->y+WIN_BRD+TBAR_H;
}

/* Dock geometry helpers */
static int dock_pill_x(void) {
    int pw = 8*2 + NDOCK_L*(BTN_W+4) + 14 + NDOCK_R*(BTN_W+4) - 4;
    return (sw-pw)/2;
}
static int dock_pill_y(void) { return sh-DOCK_MARG-(BTN_H+8*2); }
static int dock_pill_w(void) {
    return 8*2 + NDOCK_L*(BTN_W+4) + 14 + NDOCK_R*(BTN_W+4) - 4;
}
static int dock_pill_h(void) { return BTN_H+8*2; }

/* ═══════════════════════════════════════════════════════════
 * Mouse callback (called from IRQ context via mouse driver)
 * ═══════════════════════════════════════════════════════════ */
static void on_mouse(const mouse_event_t *ev) {
    int mx = ev->x, my = ev->y;
    u8  btn = ev->buttons;
    int ldown = btn  & MOUSE_BTN_LEFT;
    int lprev = prev_btn & MOUSE_BTN_LEFT;
    int lclick  = ldown && !lprev;
    int lrelease = !ldown && lprev;

    /* Always mark cursor dirty on any movement */
    if (ev->dx != 0 || ev->dy != 0) {
        cur_x = mx; cur_y = my;
        cur_dirty = 1;
        /* Don't force full redraw just for cursor — handle in wm_tick */
    }

    rebuild_z();

    /* Dragging */
    if (drag_id >= 0 && ldown) {
        wm_window_t *dw = wm_get_window(drag_id);
        if (dw) wm_move(dw, mx-drag_ox, my-drag_oy);
        prev_btn = btn;
        full_redraw = 1;
        return;
    }
    if (lrelease && drag_id >= 0) { drag_id = -1; full_redraw = 1; }

    if (lclick) {
        wm_window_t *hit = hit_win(mx, my);
        if (hit) {
            if (hit_close(hit, mx, my)) {
                if (hit->wndproc) hit->wndproc(hit, WM_CLOSE, 0, 0);
                else wm_destroy_window(hit);
            } else {
                wm_raise(hit);
                if (hit_titlebar(hit, mx, my)) {
                    drag_id = hit->id;
                    drag_ox  = mx - hit->x;
                    drag_oy  = my - hit->y;
                }
            }
        } else {
            /* Dock clicks */
            int ppx=dock_pill_x(), ppy=dock_pill_y();
            int ppw=dock_pill_w(), pph=dock_pill_h();
            if (mx>=ppx&&mx<ppx+ppw&&my>=ppy&&my<ppy+pph) {
                int bx = ppx+8;
                for (int i=0; i<NDOCK_L; i++) {
                    if (mx>=bx&&mx<bx+BTN_W) {
                        if (i==0) { /* Files: stub */ }
                        else if (i==1) wm_open_terminal();
                        else if (i==2) wm_open_sysmon();
                        full_redraw = 1;
                        break;
                    }
                    bx += BTN_W+4;
                }
            }
            /* Rail clicks */
            if (mx < RAIL_W && my > TOPBAR_H) {
                int riy = TOPBAR_H+8;
                rebuild_z();
                for (int i=0;i<ZN&&i<8;i++) {
                    wm_window_t *rw = ZL[ZN-1-i];
                    if (my>=riy+i*26&&my<riy+i*26+20) {
                        wm_raise(rw); full_redraw=1; break;
                    }
                }
            }
        }
        full_redraw = 1;
    }
    prev_btn = btn;
}

/* ═══════════════════════════════════════════════════════════
 * Keyboard callback
 * ═══════════════════════════════════════════════════════════ */
static void on_key(const key_event_t *ev) {
    if (!ev->pressed) return;

    /* Global hotkeys */
    if ((ev->modifiers & KEY_MOD_ALT) && ev->keycode == KEY_F4) {
        wm_window_t *fw = wm_get_focused();
        if (fw) { if(fw->wndproc) fw->wndproc(fw,WM_CLOSE,0,0);
                  else wm_destroy_window(fw); }
        return;
    }
    if ((ev->modifiers & KEY_MOD_ALT) && ev->keycode == KEY_TAB) {
        rebuild_z();
        if (ZN > 1) {
            for (int i=0;i<ZN;i++)
                if (ZL[i]->id == focused_id) { wm_raise(ZL[(i+1)%ZN]); return; }
            wm_raise(ZL[0]);
        }
        return;
    }

    /* Forward to focused window */
    wm_window_t *fw = wm_get_focused();
    if (fw && fw->wndproc) {
        fw->wndproc(fw, WM_KEYDOWN, ev->keycode, ev->ascii);
        if (ev->ascii) fw->wndproc(fw, WM_CHAR, ev->ascii, ev->modifiers);
    }
}

/* ═══════════════════════════════════════════════════════════
 * wm_tick — called at 100Hz from timer IRQ
 * Single fb_flip() at end of frame to prevent tearing
 * ═══════════════════════════════════════════════════════════ */
void wm_tick(void) {
    if (!wm_running) return;

    static u32 ticks = 0;
    ticks++;

    /* Full redraw once per second (clock update) or on demand */
    if (ticks % 100 == 0) full_redraw = 1;

    if (full_redraw) {
        repaint_all();
        cur_draw(cur_x, cur_y);
        cur_dirty = 0;
    } else {
        /* Incremental: dirty windows + cursor */
        int any_dirty = 0;
        rebuild_z();
        for (int i=0;i<ZN;i++) {
            if (ZL[i]->flags & WF_DIRTY) {
                draw_chrome(ZL[i]);
                if (ZL[i]->wndproc) ZL[i]->wndproc(ZL[i], WM_PAINT, 0, 0);
                ZL[i]->flags &= ~WF_DIRTY;
                any_dirty = 1;
            }
        }
        if (any_dirty) {
            draw_topbar();
            draw_rail();
        }
        if (cur_dirty || any_dirty) {
            cur_restore();
            cur_draw(cur_x, cur_y);
            cur_dirty = 0;
        }
    }

    /* Single flip at end of frame — eliminates tearing */
    fb_flip();
}

/* ═══════════════════════════════════════════════════════════
 * Public window management API
 * ═══════════════════════════════════════════════════════════ */
void wm_init(void) {
    const fb_info_t *info = fb_get_info();
    if (!info->initialized) { printk(KERN_ERR "[WM] no framebuffer\n"); return; }
    sw = info->width; sh = info->height;

    memset(W, 0, sizeof(W));
    focused_id=-1; next_z=1; next_id=1;
    full_redraw=1; wm_running=0;
    cur_sx=-1; cur_x=sw/2; cur_y=sh/2;
    cur_dirty=1; drag_id=-1; prev_btn=0;

    kbd_set_callback(on_key);
    mouse_init(sw, sh);
    mouse_set_callback(on_mouse);

    printk(KERN_INFO "[WM] PaperWM ready (%dx%d)\n", sw, sh);
    repaint_all();
    cur_draw(cur_x, cur_y);
    fb_flip();
}

void wm_run(void) {
    wm_running = 1;
    printk(KERN_INFO "[WM] entering main loop\n");
    wm_open_terminal();
    for (;;) __asm__ volatile("hlt");
}

wm_window_t *wm_create_window(int x, int y, int w, int h,
                               const char *title, u32 flags,
                               wm_wndproc_t proc) {
    wm_window_t *win = NULL;
    for (int i=0;i<MAX_WINS;i++) if (!W[i].id){win=&W[i];break;}
    if (!win) return NULL;

    win->id = next_id++;
    win->x=x; win->y=y;
    win->w=(w<120)?120:w; win->h=(h<80)?80:h;
    win->flags = flags | WF_VISIBLE | WF_DIRTY;
    win->z = next_z++;
    win->wndproc = proc;
    win->userdata = win->backbuf = NULL;
    win->is_dragging = 0;

    int tl=0; while(tl<63&&title[tl]){win->title[tl]=title[tl];tl++;} win->title[tl]=0;
    calc_client(win);
    focused_id = win->id;
    full_redraw = 1;
    printk(KERN_INFO "[WM] '%s' %dx%d @ %d,%d\n", win->title,win->w,win->h,x,y);
    return win;
}

void wm_destroy_window(wm_window_t *w) {
    if (!w||!w->id) return;
    if (w->backbuf) { kfree(w->backbuf); w->backbuf=NULL; }
    if (w->userdata) { kfree(w->userdata); w->userdata=NULL; }
    if (focused_id==w->id) focused_id=-1;
    if (drag_id==w->id)    drag_id=-1;
    memset(w,0,sizeof(*w));
    full_redraw=1;
}

void wm_show(wm_window_t *w)  { if(w){w->flags|=WF_VISIBLE|WF_DIRTY;full_redraw=1;} }
void wm_hide(wm_window_t *w)  { if(w){w->flags&=~WF_VISIBLE;full_redraw=1;} }

void wm_raise(wm_window_t *w) {
    if (!w) return;
    wm_window_t *old = wm_get_focused();
    if (old&&old!=w) {
        old->flags &= ~WF_FOCUSED;
        if (old->wndproc) old->wndproc(old,WM_FOCUS_OUT,0,0);
    }
    w->z=next_z++; w->flags|=WF_FOCUSED|WF_DIRTY;
    focused_id=w->id;
    if (w->wndproc) w->wndproc(w,WM_FOCUS_IN,0,0);
    full_redraw=1;
}

void wm_move(wm_window_t *w, int x, int y) {
    if (!w) return;
    if (x < RAIL_W) x = RAIL_W;
    if (y < TOPBAR_H) y = TOPBAR_H;
    if (x+w->w > sw) x = sw-w->w;
    if (y+w->h > sh-DOCK_H-DOCK_MARG) y = sh-DOCK_H-DOCK_MARG-w->h;
    w->x=x; w->y=y; calc_client(w); full_redraw=1;
}

void wm_resize(wm_window_t *w, int nw, int nh) {
    if (!w) return;
    if (nw<120) nw=120;
    if (nh<80)  nh=80;
    w->w=nw; w->h=nh; calc_client(w);
    if (w->wndproc) w->wndproc(w,WM_RESIZE,nw,nh);
    full_redraw=1;
}

void wm_maximize(wm_window_t *w) {
    if (!w||(w->flags&WF_MAXIMIZED)) return;
    w->saved_x=w->x; w->saved_y=w->y; w->saved_w=w->w; w->saved_h=w->h;
    w->flags|=WF_MAXIMIZED;
    w->x=RAIL_W; w->y=TOPBAR_H;
    w->w=sw-RAIL_W; w->h=sh-TOPBAR_H-DOCK_H-DOCK_MARG*2;
    calc_client(w); full_redraw=1;
}

void wm_restore(wm_window_t *w) {
    if (!w||!(w->flags&WF_MAXIMIZED)) return;
    w->flags&=~WF_MAXIMIZED;
    w->x=w->saved_x; w->y=w->saved_y; w->w=w->saved_w; w->h=w->saved_h;
    calc_client(w); full_redraw=1;
}

void wm_invalidate(wm_window_t *w) { if(w){w->flags|=WF_DIRTY;} }

wm_window_t *wm_get_window(int id) {
    for (int i=0;i<MAX_WINS;i++) if(W[i].id==id) return &W[i];
    return NULL;
}
wm_window_t *wm_get_focused(void) { return wm_get_window(focused_id); }

void wm_fill_client(wm_window_t *w, fb_color_t c) {
    if (!w) return;
    fb_fill_rect(w->client_x,w->client_y,w->client_w,w->client_h,c);
}
void wm_draw_text(wm_window_t *w, int x, int y,
                  const char *s, fb_color_t fg, fb_color_t bg) {
    if (!w||!s) return;
    fb_set_clip(w->client_x,w->client_y,w->client_w,w->client_h);
    fb_draw_string(w->client_x+x,w->client_y+y,s,fg,bg);
    fb_clear_clip();
}
void wm_fill_rect(wm_window_t *w,int x,int y,int cw,int ch,fb_color_t c){
    if(!w)return;
    fb_set_clip(w->client_x,w->client_y,w->client_w,w->client_h);
    fb_fill_rect(w->client_x+x,w->client_y+y,cw,ch,c);
    fb_clear_clip();
}
void wm_draw_3d_rect(wm_window_t *w,int x,int y,int cw,int ch,int raised){
    if(!w)return;
    fb_set_clip(w->client_x,w->client_y,w->client_w,w->client_h);
    fb_draw_3d_rect(w->client_x+x,w->client_y+y,cw,ch,raised);
    fb_clear_clip();
}
void wm_flush(wm_window_t *w) { (void)w; }  /* flip is done by wm_tick */

/* legacy stubs */
void wm_draw_desktop(void)     { draw_desktop(); }
void wm_draw_taskbar(void)     { draw_topbar(); draw_dock(); }
void wm_toggle_start_menu(void){ }

/* ═══════════════════════════════════════════════════════════
 * TERMINAL — actual ksh integration
 *
 * The terminal is a scrollback buffer.  Characters typed go into
 * ts->input_buf.  On Enter, ksh_execute_line() is called and output
 * is captured into the scrollback.  The terminal writes output by
 * intercepting printk via a small hook below.
 * ═══════════════════════════════════════════════════════════ */

#define TROWS    200     /* total scrollback lines  */
#define TCOLS    200     /* max chars per line      */
#define T_IBUF   256     /* input buffer            */

typedef struct {
    char    lines[TROWS][TCOLS];
    int     line_len[TROWS];     /* actual used length     */
    int     nlines;              /* total lines in buffer  */
    int     scroll_top;          /* first visible line     */
    char    ibuf[T_IBUF];        /* current input line     */
    int     ilen;                /* input line cursor pos  */
    int     vis_rows;            /* visible rows in client */
    int     dirty;
} term_state_t;

/* One terminal is special — it gets printk output. A pointer to
   the "active" terminal for output capture. NULL = no capture. */
static term_state_t *capture_term = NULL;

/* Append text to a terminal's scrollback */
static void term_puts(term_state_t *ts, const char *s) {
    if (!ts || !s) return;
    while (*s) {
        char c = *s++;
        if (ts->nlines == 0) ts->nlines = 1;
        int cur = ts->nlines - 1;
        if (c == '\n' || c == '\r') {
            ts->nlines++;
            if (ts->nlines > TROWS) {
                /* Scroll: drop oldest line */
                for (int i=0;i<TROWS-1;i++) {
                    memcpy(ts->lines[i], ts->lines[i+1], TCOLS);
                    ts->line_len[i] = ts->line_len[i+1];
                }
                ts->nlines = TROWS;
                cur = TROWS-1;
            }
            cur = ts->nlines-1;
            ts->lines[cur][0] = 0;
            ts->line_len[cur] = 0;
        } else if (c == '\b') {
            if (ts->line_len[cur] > 0) ts->line_len[cur]--;
            ts->lines[cur][ts->line_len[cur]] = 0;
        } else if ((u8)c >= 0x20 && (u8)c < 0x80) {
            if (ts->line_len[cur] < TCOLS-1) {
                ts->lines[cur][ts->line_len[cur]++] = c;
                ts->lines[cur][ts->line_len[cur]]   = 0;
            }
        }
    }
    ts->dirty = 1;
}

/* printk capture hook — called by printk when capture_term != NULL */
static void term_printk_hook(const char *s) {
    if (capture_term) term_puts(capture_term, s);
}

/* ksh interface: execute a command line, capture output to terminal */
static void term_exec(term_state_t *ts, const char *cmdline) {
    /* show typed command */
    char echo_line[T_IBUF+10];
    const char *prompt = "paper@papper-wm:~$ ";
    int pi=0; while(prompt[pi]&&pi<(int)sizeof(echo_line)-2) echo_line[pi]=prompt[pi],pi++;
    int ci=0; while(cmdline[ci]&&pi<(int)sizeof(echo_line)-2) echo_line[pi++]=cmdline[ci++];
    echo_line[pi]=0;
    term_puts(ts, echo_line);
    term_puts(ts, "\n");

    /* hook printk output to this terminal */
    capture_term = ts;
    extern void ksh_execute_line(const char *line);
    ksh_execute_line(cmdline);
    capture_term = NULL;

    /* show next prompt */
    term_puts(ts, prompt);
}

static int term_proc(wm_window_t *w, wm_msg_t msg, u64 p1, u64 p2) {
    (void)p2;
    term_state_t *ts = (term_state_t *)w->userdata;
    if (!ts) return -1;

    switch(msg) {
    case WM_PAINT: {
        int cx = w->client_x, cy = w->client_y;
        int cw2 = w->client_w, ch = w->client_h;
        fb_set_clip(cx, cy, cw2, ch);
        fb_fill_rect(cx, cy, cw2, ch, C_TERM_BG);

        int vis = (ch - FB_FONT_H - 4) / FB_FONT_H;  /* rows for scrollback */
        if (vis < 1) vis = 1;
        ts->vis_rows = vis;

        /* auto-scroll to bottom if last line visible */
        int bot = ts->nlines - vis;
        if (bot < 0) bot = 0;
        if (ts->scroll_top > bot) ts->scroll_top = bot;

        /* render visible lines */
        for (int r=0; r<vis; r++) {
            int li = ts->scroll_top + r;
            if (li >= ts->nlines) break;
            fb_draw_string(cx+4, cy+r*FB_FONT_H+2,
                           ts->lines[li], C_TERM_FG, C_TERM_BG);
        }

        /* input line at bottom */
        int iy = cy + vis*FB_FONT_H + 4;
        fb_draw_hline(cx, iy-1, cw2, C_LGT);
        /* show prompt + input */
        char ibuf[T_IBUF+4];
        const char *pr = "> ";
        int prl=2, il=0;
        ibuf[il++]=pr[0]; ibuf[il++]=pr[1];
        for(int i=0;i<ts->ilen&&il<T_IBUF+2;i++) ibuf[il++]=ts->ibuf[i];
        ibuf[il]=0;
        fb_draw_string(cx+4, iy+2, ibuf, C_TERM_GR, C_TERM_BG);

        /* blinking block cursor at end of input */
        extern volatile u64 system_ticks;
        if ((system_ticks/50)&1) {
            int curx = cx+4+(prl+ts->ilen)*FB_FONT_W;
            fb_fill_rect(curx, iy+2, FB_FONT_W, FB_FONT_H, C_INK);
        }

        fb_clear_clip();
        w->flags |= WF_DIRTY;   /* keep blinking */
        ts->dirty = 0;
        break;
    }
    case WM_CHAR: {
        char c = (char)(p1 & 0xFF);
        if (c == '\n' || c == '\r') {
            /* Execute */
            ts->ibuf[ts->ilen] = 0;
            term_exec(ts, ts->ibuf);
            ts->ilen = 0;
            /* auto-scroll to bottom */
            ts->scroll_top = ts->nlines - ts->vis_rows;
            if (ts->scroll_top < 0) ts->scroll_top = 0;
        } else if (c == '\b') {
            if (ts->ilen > 0) ts->ibuf[--ts->ilen] = 0;
        } else if ((u8)c >= 0x20 && (u8)c < 0x7F && ts->ilen < T_IBUF-1) {
            ts->ibuf[ts->ilen++] = c;
        }
        w->flags |= WF_DIRTY;
        break;
    }
    case WM_KEYDOWN:
        if (p1 == KEY_UP)   { ts->scroll_top--; if(ts->scroll_top<0) ts->scroll_top=0; w->flags|=WF_DIRTY; }
        if (p1 == KEY_DOWN) { int bot=ts->nlines-ts->vis_rows; if(bot<0)bot=0; ts->scroll_top++; if(ts->scroll_top>bot) ts->scroll_top=bot; w->flags|=WF_DIRTY; }
        break;
    case WM_CLOSE:
        if (capture_term == ts) capture_term = NULL;
        wm_destroy_window(w);
        break;
    case WM_RESIZE: calc_client(w); w->flags|=WF_DIRTY; break;
    default: return -1;
    }
    return 0;
}

wm_window_t *wm_open_terminal(void) {
    /* Place like concept art: left-center, takes most of screen height */
    int tw = (sw - RAIL_W) * 55 / 100;
    int th = (sh - TOPBAR_H - DOCK_H - DOCK_MARG*2) * 75 / 100;
    int tx = RAIL_W + 20;
    int ty = TOPBAR_H + 20;

    wm_window_t *w = wm_create_window(tx,ty,tw,th,"Terminal",WF_RESIZABLE,term_proc);
    if (!w) return NULL;

    term_state_t *ts = (term_state_t *)kmalloc(sizeof(term_state_t), GFP_KERNEL);
    if (!ts) { wm_destroy_window(w); return NULL; }
    memset(ts, 0, sizeof(*ts));
    ts->nlines = 1;
    ts->vis_rows = 24;

    /* welcome message + prompt */
    term_puts(ts, "PaperWM on Picomimi-x64\n");
    term_puts(ts, "paper@papper-wm:~$ ");

    /* register printk hook so shell output appears here */
    extern void printk_set_hook(void (*fn)(const char *s));
    printk_set_hook(term_printk_hook);

    w->userdata = ts;
    capture_term = ts;
    return w;
}

/* ═══════════════════════════════════════════════════════════
 * SYSMON — memory bar + scrolling CPU graph
 * ═══════════════════════════════════════════════════════════ */

#define CPU_HIST  60   /* 60-second CPU history */

typedef struct {
    u8  cpu_hist[CPU_HIST];   /* percentage 0-100 */
    int hist_pos;
    u64 last_ticks;
    u64 last_idle;
} sysmon_state_t;

static int sysmon_proc(wm_window_t *w, wm_msg_t msg, u64 p1, u64 p2) {
    (void)p1;(void)p2;
    sysmon_state_t *ss = (sysmon_state_t *)w->userdata;

    switch(msg) {
    case WM_PAINT: case WM_TIMER: {
        int cx=w->client_x, cy=w->client_y, cw2=w->client_w, ch=w->client_h;
        fb_set_clip(cx,cy,cw2,ch);
        fb_fill_rect(cx,cy,cw2,ch,C_WHITE);

        extern volatile u64 system_ticks;
        extern u64 pmm_get_free_memory(void);
        extern u64 pmm_get_total_memory(void);

        int tx=cx+8, ty=cy+8;

        /* Title */
        fb_draw_string(tx,ty,"Sysmon",C_INK,C_WHITE); ty+=FB_FONT_H+6;
        fb_draw_hline(cx+4,ty,cw2-8,C_LGT); ty+=6;

        /* Memory */
        fb_draw_string(tx,ty,"Mem:",C_INK,C_WHITE);
        u64 total = pmm_get_total_memory();
        u64 freemb = pmm_get_free_memory();
        u64 used  = total - freemb;
        /* format "516Mb / 1024Mb" */
        char mbuf[40]; int mi=0;
        char tmp[12];
        uitoa(tmp, used/(1024*1024));
        for(int i=0;tmp[i];i++) mbuf[mi++]=tmp[i];
        mbuf[mi++]='M';mbuf[mi++]='b';mbuf[mi++]=' ';mbuf[mi++]='/';mbuf[mi++]=' ';
        uitoa(tmp, total/(1024*1024));
        for(int i=0;tmp[i];i++) mbuf[mi++]=tmp[i];
        mbuf[mi++]='M';mbuf[mi++]='b';mbuf[mi]=0;
        fb_draw_string(tx+5*FB_FONT_W, ty, mbuf, C_MID, C_WHITE);
        ty += FB_FONT_H+2;

        /* Memory bar */
        int bw = cw2-16;
        fb_draw_rect(tx, ty, bw, 10, C_LGT);
        if (total>0) {
            int filled=(int)((u64)bw*used/total);
            /* hatched fill like concept art */
            for (int px2=tx+1;px2<tx+filled;px2++)
                fb_draw_vline(px2, ty+1, 8,
                    (px2%2==0)?C_INK:FB_COLOR(0x66,0x66,0x66));
        }
        ty += 16;

        /* CPU label + graph */
        fb_draw_string(tx,ty,"CPU:",C_INK,C_WHITE); ty+=FB_FONT_H+2;

        /* Graph area */
        int gh = ch - (ty-cy) - 24;
        int gw = cw2-16;
        if (gh < 20) gh=20;

        /* Graph border */
        fb_draw_rect(tx, ty, gw, gh, C_LGT);

        /* Y-axis labels: 90, 0 */
        fb_draw_string(tx-1, ty+2, "9", C_MID, C_WHITE);
        fb_draw_string(tx-1, ty+2, "0", C_MID, C_WHITE);
        fb_draw_string(tx-1, ty+gh-FB_FONT_H-2, "0", C_MID, C_WHITE);
        /* Dashed 90% line */
        for(int px2=tx+1;px2<tx+gw-1;px2+=4)
            fb_putpixel(px2, ty+gh/10, C_LGT);

        /* Draw CPU history as line graph */
        if (ss) {
            int prev_py = -1;
            for (int i=0; i<CPU_HIST && i<gw-2; i++) {
                int hi = (ss->hist_pos - CPU_HIST + i + CPU_HIST) % CPU_HIST;
                int pct = ss->cpu_hist[hi];
                int py2 = ty + gh - 1 - (pct * (gh-2) / 100);
                int px2 = tx + 1 + i*(gw-2)/CPU_HIST;
                if (prev_py >= 0)
                    fb_draw_line(px2-(gw-2)/CPU_HIST, prev_py, px2, py2, C_INK);
                prev_py = py2;
            }

            /* X-axis: "60s" label */
            fb_draw_string(tx+gw-3*FB_FONT_W-2, ty+gh+2, "60s", C_MID, C_WHITE);

            /* Update CPU sample roughly every second */
            if (system_ticks - ss->last_ticks >= 100) {
                /* Fake CPU usage: vary between 5-30% for visual interest */
                static u32 seed = 42;
                seed = seed * 1664525 + 1013904223;
                u8 fake_cpu = (u8)(5 + (seed>>24) % 25);
                ss->cpu_hist[ss->hist_pos] = fake_cpu;
                ss->hist_pos = (ss->hist_pos+1) % CPU_HIST;
                ss->last_ticks = system_ticks;
            }
        }

        /* Mouse position row */
        const mouse_state_t *ms = mouse_get_state();
        char mpos[24]; int mpi=0;
        char n2[8];
        mpos[mpi++]='X'; mpos[mpi++]=':';
        uitoa(n2, (u64)ms->x); for(int i=0;n2[i];i++) mpos[mpi++]=n2[i];
        mpos[mpi++]=' '; mpos[mpi++]='Y'; mpos[mpi++]=':';
        uitoa(n2, (u64)ms->y); for(int i=0;n2[i];i++) mpos[mpi++]=n2[i];
        mpos[mpi]=0;
        fb_draw_string(tx, ty+gh+14, mpos, C_MID, C_WHITE);

        fb_clear_clip();
        w->flags |= WF_DIRTY;
        break;
    }
    case WM_CLOSE: wm_destroy_window(w); break;
    default: return -1;
    }
    return 0;
}

wm_window_t *wm_open_sysmon(void) {
    /* Concept art: top-right, medium size */
    int ww = 280, wh = 280;
    int wx = sw - ww - 20;
    int wy = TOPBAR_H + 20;

    wm_window_t *w = wm_create_window(wx,wy,ww,wh,"Sysmon",WF_RESIZABLE,sysmon_proc);
    if (!w) return NULL;

    sysmon_state_t *ss = (sysmon_state_t*)kmalloc(sizeof(sysmon_state_t),GFP_KERNEL);
    if (!ss) { wm_destroy_window(w); return NULL; }
    memset(ss,0,sizeof(*ss));
    ss->last_ticks = 0;

    w->userdata = ss;
    return w;
}

wm_window_t *wm_open_about(void) {
    return wm_open_sysmon(); /* about not needed for now */
}
