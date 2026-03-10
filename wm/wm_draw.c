/**
 * wm_draw.c — All rendering: primitives, chrome, desktop, dock, topbar, rail, cursor
 */
#include "wm_internal.h"

/* ═══════════════════════════════════════════════════════════
 * Utility math
 * ═══════════════════════════════════════════════════════════ */
int wm_slen(const char *s) { int n=0; while(s[n]) n++; return n; }

void wm_uitoa(char *buf, u64 v) {
    char tmp[22]; int i=0;
    if (!v) { buf[0]='0'; buf[1]=0; return; }
    while (v) { tmp[i++]='0'+v%10; v/=10; }
    int j=0; while (i>0) buf[j++]=tmp[--i]; buf[j]=0;
}
void wm_itoa(char *buf, s64 v) {
    if (v < 0) { buf[0]='-'; wm_uitoa(buf+1,(u64)-v); }
    else wm_uitoa(buf, (u64)v);
}

/* ═══════════════════════════════════════════════════════════
 * Alpha blend helper
 * ═══════════════════════════════════════════════════════════ */
static inline fb_color_t blend(fb_color_t fg, fb_color_t bg, u8 a) {
    if (a == 255) return fg;
    if (a == 0)   return bg;
    u8 fr=(fg>>16)&0xFF, fg2=(fg>>8)&0xFF, fb2=(fg>>0)&0xFF;
    u8 br=(bg>>16)&0xFF, bg2=(bg>>8)&0xFF, bb=(bg>>0)&0xFF;
    u8 or2=(u8)((fr*a + br*(255-a))>>8);
    u8 og =(u8)((fg2*a + bg2*(255-a))>>8);
    u8 ob =(u8)((fb2*a + bb*(255-a))>>8);
    return FB_COLOR(or2,og,ob);
}

/* ═══════════════════════════════════════════════════════════
 * Rounded rect — antialiased corners, clean fill
 * Strategy: fill 3 rects for body, draw AA corner pixels
 * ═══════════════════════════════════════════════════════════ */
void wm_rrect_fill(int x, int y, int w, int h, int r, fb_color_t fill) {
    if (w<=0||h<=0) return;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    if (r < 0) r = 0;

    /* Three bands */
    if (r > 0) {
        fb_fill_rect(x+r, y,     w-r*2, r,     fill);
        fb_fill_rect(x+r, y+h-r, w-r*2, r,     fill);
    }
    fb_fill_rect(x, y+r, w, h-r*2, fill);

    /* Corners: plot filled pixels inside circle r */
    for (int cy=0; cy<r; cy++) {
        for (int cx=0; cx<r; cx++) {
            int dx=r-cx, dy=r-cy;
            int d2=dx*dx+dy*dy;
            if (d2 <= r*r) {
                fb_putpixel(x+cx,         y+cy,         fill);
                fb_putpixel(x+w-1-cx,     y+cy,         fill);
                fb_putpixel(x+cx,         y+h-1-cy,     fill);
                fb_putpixel(x+w-1-cx,     y+h-1-cy,     fill);
            }
        }
    }
}

void wm_rrect(int x, int y, int w, int h, int r,
              fb_color_t fill, fb_color_t bord) {
    wm_rrect_fill(x, y, w, h, r, fill);
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;

    /* Border straights */
    fb_draw_hline(x+r, y,     w-r*2, bord);
    fb_draw_hline(x+r, y+h-1, w-r*2, bord);
    fb_draw_vline(x,     y+r, h-r*2, bord);
    fb_draw_vline(x+w-1, y+r, h-r*2, bord);

    /* Border arcs */
    for (int cy=0; cy<r; cy++) {
        for (int cx=0; cx<r; cx++) {
            int dx=r-cx, dy=r-cy;
            int d2=dx*dx+dy*dy;
            /* ring between (r-1)^2 and r^2 */
            if (d2<=r*r && d2>(r-1)*(r-1)) {
                fb_putpixel(x+cx,         y+cy,         bord);
                fb_putpixel(x+w-1-cx,     y+cy,         bord);
                fb_putpixel(x+cx,         y+h-1-cy,     bord);
                fb_putpixel(x+w-1-cx,     y+h-1-cy,     bord);
            }
        }
    }
}

/* Soft multi-layer shadow */
void wm_shadow(int x, int y, int w, int h, int r) {
    /* Layer 3 (outermost, lightest) */
    fb_color_t s3 = FB_COLOR(0xDC,0xDC,0xD4);
    fb_color_t s2 = FB_COLOR(0xCC,0xCC,0xC4);
    fb_color_t s1 = FB_COLOR(0xBC,0xBC,0xB4);
    wm_rrect_fill(x+6, y+5, w, h, r, s3);
    wm_rrect_fill(x+5, y+4, w, h, r, s2);
    wm_rrect_fill(x+4, y+3, w, h, r, s1);
}

/* ═══════════════════════════════════════════════════════════
 * Icon blitter with bilinear-ish scaling
 * src_w×src_h → dst_w×dst_h, alpha-blended over bg
 * ═══════════════════════════════════════════════════════════ */
void blit_icon_sz(int cx, int cy,
                  const icon_pixel_t *icon,
                  int src_w, int src_h,
                  int dst_w, int dst_h,
                  fb_color_t bg) {
    u8 bg_r = (bg>>16)&0xFF, bg_g = (bg>>8)&0xFF, bg_b = (bg>>0)&0xFF;

    for (int dy=0; dy<dst_h; dy++) {
        int py = cy+dy;
        if (py<0||py>=G_sh) continue;
        int sy = dy*src_h/dst_h;

        for (int dx=0; dx<dst_w; dx++) {
            int px = cx+dx;
            if (px<0||px>=G_sw) continue;
            int sx = dx*src_w/dst_w;

            icon_pixel_t p = icon[sy*src_w+sx];
            u8 a  = (p>>24)&0xFF;
            u8 ir = (p>>16)&0xFF;
            u8 ig = (p>> 8)&0xFF;
            u8 ib = (p>> 0)&0xFF;

            if (a == 0) continue;
            fb_color_t out;
            if (a == 255) {
                out = FB_COLOR(ir,ig,ib);
            } else {
                u8 or2=(u8)((ir*a + bg_r*(255-a))>>8);
                u8 og =(u8)((ig*a + bg_g*(255-a))>>8);
                u8 ob =(u8)((ib*a + bg_b*(255-a))>>8);
                out = FB_COLOR(or2,og,ob);
            }
            fb_putpixel(px, py, out);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Software cursor — 16×24 precision arrow
 * 0=transparent, 1=black outline, 2=white body, 3=mid-gray AA
 * ═══════════════════════════════════════════════════════════ */
#define CW 16
#define CH 24
static const u8 CUR_MAP[CH][CW] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,2,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,2,2,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,1,1,1,0,0,0},
    {1,2,2,2,2,2,2,1,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,1,0,1,2,2,2,1,0,0,0},
    {1,2,2,2,2,1,0,0,0,1,2,2,2,1,0,0},
    {1,2,2,2,1,0,0,0,0,0,1,2,2,2,1,0},
    {1,2,2,1,0,0,0,0,0,0,0,1,2,2,2,1},
    {1,2,1,0,0,0,0,0,0,0,0,0,1,2,2,1},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,1,1,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

static u32 cur_save[CW*CH];
static int cur_sx=-1, cur_sy=-1;

void wm_cur_save(int x, int y) {
    cur_sx=x; cur_sy=y;
    for (int r=0;r<CH;r++) {
        for (int c=0;c<CW;c++) {
            int px=x+c, py=y+r;
            if (px>=0&&px<G_sw&&py>=0&&py<G_sh)
                cur_save[r*CW+c]=(u32)fb_getpixel(px,py);
            else
                cur_save[r*CW+c]=(u32)C_BG;
        }
    }
}
void wm_cur_restore(void) {
    if (cur_sx<0) return;
    for (int r=0;r<CH;r++) {
        for (int c=0;c<CW;c++) {
            int px=cur_sx+c, py=cur_sy+r;
            if (px>=0&&px<G_sw&&py>=0&&py<G_sh)
                fb_putpixel(px,py,(fb_color_t)cur_save[r*CW+c]);
        }
    }
    cur_sx=-1;
}
void wm_cur_draw(int x, int y) {
    wm_cur_save(x,y);
    for (int r=0;r<CH;r++) {
        int py=y+r; if(py<0||py>=G_sh) continue;
        for (int c=0;c<CW;c++) {
            int px=x+c; if(px<0||px>=G_sw) continue;
            u8 p=CUR_MAP[r][c];
            if      (p==1) fb_putpixel(px,py,FB_COLOR(0x08,0x08,0x08));
            else if (p==2) fb_putpixel(px,py,C_WHITE);
            else if (p==3) fb_putpixel(px,py,FB_COLOR(0xA0,0xA0,0xA0));
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Desktop — paper graph background
 * ═══════════════════════════════════════════════════════════ */
void wm_draw_desktop(void) {
    int y0 = TOPBAR_H;
    int dock_top = G_sh - DOCK_MARG - DOCK_PAD*2 - DOCK_BTN_SZ;

    /* Main bg */
    fb_fill_rect(0, y0, G_sw, G_sh-y0, C_BG);

    /* Left rail */
    fb_fill_rect(0, y0, RAIL_W, dock_top-y0, C_RAIL);
    fb_draw_vline(RAIL_W-1, y0, dock_top-y0, C_RAIL_LINE);

    /* Minor grid lines */
    for (int gy = y0 + (GRID_MINOR - (y0 % GRID_MINOR)) % GRID_MINOR;
         gy < dock_top; gy += GRID_MINOR)
        fb_draw_hline(RAIL_W, gy, G_sw-RAIL_W, C_GRID_MIN);

    for (int gx = RAIL_W + GRID_MINOR; gx < G_sw; gx += GRID_MINOR)
        fb_draw_vline(gx, y0, dock_top-y0, C_GRID_MIN);

    /* Major grid lines (every 3 minor = 54px) */
    for (int gy = y0 + (GRID_MAJOR - (y0 % GRID_MAJOR)) % GRID_MAJOR;
         gy < dock_top; gy += GRID_MAJOR)
        fb_draw_hline(RAIL_W, gy, G_sw-RAIL_W, C_GRID_MAJ);

    for (int gx = RAIL_W + GRID_MAJOR; gx < G_sw; gx += GRID_MAJOR)
        fb_draw_vline(gx, y0, dock_top-y0, C_GRID_MAJ);
}

/* ═══════════════════════════════════════════════════════════
 * Rail — open window indicators on the left
 * ═══════════════════════════════════════════════════════════ */
void wm_draw_rail(void) {
    int y0 = TOPBAR_H + 8;
    int bx = (RAIL_W - RAIL_BTN_W) / 2;

    /* Background */
    fb_fill_rect(0, TOPBAR_H, RAIL_W,
                 G_sh - TOPBAR_H - DOCK_MARG - DOCK_PAD*2 - DOCK_BTN_SZ,
                 C_RAIL);
    fb_draw_vline(RAIL_W-1, TOPBAR_H,
                  G_sh - TOPBAR_H - DOCK_MARG - DOCK_PAD*2 - DOCK_BTN_SZ,
                  C_RAIL_LINE);

    wm_rebuild_z();
    int n = G_zcount;
    if (n > 10) n = 10;

    for (int i = 0; i < n; i++) {
        wm_window_t *w = G_zlist[G_zcount-1-i]; /* top first */
        int by = y0 + i * (RAIL_BTN_H + RAIL_BTN_PAD);
        int focused = (w->id == G_focused_id);

        /* Draw indicator pill */
        if (focused) {
            wm_rrect(bx, by, RAIL_BTN_W, RAIL_BTN_H, 5,
                     C_SEL_BG, C_SEL_BG);
        } else {
            wm_rrect(bx, by, RAIL_BTN_W, RAIL_BTN_H, 5,
                     C_HOVER, C_LGT);
        }

        /* First letter of title, centred */
        char ch[2] = { w->title[0] ? w->title[0] : '?', 0 };
        int tx = bx + (RAIL_BTN_W - FB_FONT_W) / 2;
        int ty = by + (RAIL_BTN_H - FB_FONT_H) / 2;
        fb_color_t tbg = focused ? C_SEL_BG : C_HOVER;
        fb_color_t tfg = focused ? C_WHITE   : C_INK2;
        fb_draw_string(tx, ty, ch, tfg, tbg);
    }

    /* Rail shows minimized count at bottom */
    int minimized = 0;
    for (int i=0;i<MAX_WINS;i++)
        if (G_wins[i].id && (G_wins[i].flags & WF_MINIMIZED)) minimized++;
    if (minimized > 0) {
        /* small dot indicator */
        int dot_y = y0 + n*(RAIL_BTN_H+RAIL_BTN_PAD) + 4;
        fb_fill_rect(bx+RAIL_BTN_W/2-2, dot_y, 5, 5,
                     FB_COLOR(0xAA,0xAA,0xA0));
    }
}

/* ═══════════════════════════════════════════════════════════
 * Topbar
 * ═══════════════════════════════════════════════════════════ */
void wm_draw_topbar(void) {
    fb_fill_rect(0, 0, G_sw, TOPBAR_H, C_TOPBAR);
    fb_draw_hline(0, TOPBAR_H-1, G_sw, C_TOPBAR_LINE);

    /* Left: breadcrumb */
    char left[128];
    int li = 0;
    const char *brand = "PaperWM";
    for (int i=0; brand[i]&&li<120; i++) left[li++]=brand[i];

    wm_window_t *fw = wm_get_focused();
    if (fw) {
        left[li++]=' '; left[li++]='|'; left[li++]=' ';
        for (int i=0; fw->title[i]&&li<120; i++) left[li++]=fw->title[i];
    }
    left[li]=0;

    int ty = (TOPBAR_H - FB_FONT_H) / 2;
    fb_draw_string(8, ty, left, C_INK, C_TOPBAR);

    /* Centre: clock */
    extern volatile u64 system_ticks;
    u64 t   = system_ticks / 100;
    int hh  = (int)((t / 3600) % 24);
    int mm  = (int)((t /   60) % 60);
    int ss  = (int)( t         % 60);

    static const char *mons[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    int dom = 4 + (int)(t/86400);
    const char *mon = mons[(2 + dom/31) % 12];

    char clk[32]; int ci=0;
    char tmp[8];
    /* day */
    wm_uitoa(tmp, dom);
    for(int i=0;tmp[i];i++) clk[ci++]=tmp[i];
    clk[ci++]=' ';
    for(int i=0;mon[i];i++) clk[ci++]=mon[i];
    clk[ci++]=' '; clk[ci++]=' ';
    clk[ci++]='0'+hh/10; clk[ci++]='0'+hh%10;
    clk[ci++]=':';
    clk[ci++]='0'+mm/10; clk[ci++]='0'+mm%10;
    clk[ci++]=':';
    clk[ci++]='0'+ss/10; clk[ci++]='0'+ss%10;
    clk[ci]=0;

    int cw = ci * FB_FONT_W;
    fb_draw_string((G_sw-cw)/2, ty, clk, C_INK2, C_TOPBAR);

    /* Right: status icons */
    int rx = G_sw - 80;
    fb_draw_string(rx, ty, "WM  X", C_MID, C_TOPBAR);
}

/* ═══════════════════════════════════════════════════════════
 * Dock
 * ═══════════════════════════════════════════════════════════ */

/* Forward-declared in wm_internal.h; defined here */
dock_entry_t G_dock[12];
int          G_ndock = 0;

static void dock_open_terminal(void);
static void dock_open_sysmon(void);
static void dock_open_about(void);

void wm_draw_init(void) {
    /* Build dock entries */
    G_ndock = 0;
#define DE(lbl,ico,act,sep) \
    G_dock[G_ndock].label=lbl; \
    G_dock[G_ndock].icon=(ico); \
    G_dock[G_ndock].action=(act); \
    G_dock[G_ndock].separator_after=(sep); \
    G_ndock++;

    DE("Files",   icon_folder,   NULL,               0)
    DE("Term",    icon_terminal, dock_open_terminal, 0)
    DE("Monitor", icon_sysmon,   dock_open_sysmon,   0)
    DE("Editor",  icon_editor,   NULL,               0)
    DE("Mail",    icon_mail,     NULL,               1)
    DE("Trash",   icon_trash,    NULL,               0)
#undef DE
}

/* Geometry helpers */
static int dock_total_w(void) {
    int w = DOCK_PAD * 2;
    for (int i=0; i<G_ndock; i++) {
        w += DOCK_BTN_SZ;
        if (i < G_ndock-1) w += DOCK_GAP;
        if (G_dock[i].separator_after) w += DOCK_SEP;
    }
    return w;
}
static int dock_ph(void) { return DOCK_BTN_SZ + DOCK_PAD*2; }
static int dock_px(void) { return (G_sw - dock_total_w()) / 2; }
static int dock_py(void) { return G_sh - DOCK_MARG - dock_ph(); }

/* Get screen rect of dock button i */
static void dock_btn_rect(int i, int *bx, int *by, int *bw, int *bh) {
    int x = dock_px() + DOCK_PAD;
    for (int j=0; j<i; j++) {
        x += DOCK_BTN_SZ + DOCK_GAP;
        if (G_dock[j].separator_after) x += DOCK_SEP;
    }
    *bx = x;
    *by = dock_py() + DOCK_PAD;
    *bw = DOCK_BTN_SZ;
    *bh = DOCK_BTN_SZ;
}

/* Public: get dock index at (mx,my), -1 if none */
int wm_dock_hit(int mx, int my) {
    int ph = dock_ph();
    int py = dock_py();
    if (my < py || my >= py+ph) return -1;
    for (int i=0; i<G_ndock; i++) {
        int bx,by,bw,bh;
        dock_btn_rect(i,&bx,&by,&bw,&bh);
        if (mx>=bx&&mx<bx+bw&&my>=by&&my<by+bh) return i;
    }
    return -1;
}

void wm_draw_dock(void) {
    int pw = dock_total_w();
    int ph = dock_ph();
    int px = dock_px();
    int py = dock_py();

    /* Multi-layer shadow */
    wm_rrect_fill(px+5, py+5, pw, ph, DOCK_RAD,
                  FB_COLOR(0xD8,0xD8,0xD0));
    wm_rrect_fill(px+3, py+3, pw, ph, DOCK_RAD,
                  FB_COLOR(0xE0,0xE0,0xD8));

    /* Pill background */
    wm_rrect(px, py, pw, ph, DOCK_RAD, C_DOCK_BG, C_DOCK_LINE);

    /* Buttons */
    for (int i=0; i<G_ndock; i++) {
        int bx,by,bw,bh;
        dock_btn_rect(i,&bx,&by,&bw,&bh);

        int hovered = (G_dock_hover == i);

        /* Button backing */
        fb_color_t btn_bg = hovered
            ? FB_COLOR(0xFF,0xFF,0xFF)
            : C_BG;
        fb_color_t btn_bd = hovered
            ? C_LGT
            : C_LGT2;

        wm_rrect(bx, by, bw, bh, 8, btn_bg, btn_bd);

        /* Hover: slight lift shadow */
        if (hovered) {
            fb_draw_hline(bx+4, by+bh+1, bw-8, FB_COLOR(0xD0,0xD0,0xC8));
            fb_draw_hline(bx+6, by+bh+2, bw-12, FB_COLOR(0xDC,0xDC,0xD4));
        }

        /* Icon — centred, 32×32 inside 44×44 button */
        if (G_dock[i].icon) {
            int margin = (bw - 32) / 2;
            blit_icon_sz(bx+margin, by+margin,
                         G_dock[i].icon,
                         ICON_W, ICON_H,
                         32, 32,
                         btn_bg);
        }

        /* Separator after */
        if (G_dock[i].separator_after && i < G_ndock-1) {
            int sx = bx + bw + DOCK_GAP/2 + 2;
            int sy = by + 8;
            fb_draw_vline(sx, sy, bh-16, C_LGT);
        }

        /* Tooltip */
        if (hovered && G_tooltip_timer >= TOOLTIP_DELAY) {
            const char *lbl = G_dock[i].label;
            int lw = wm_slen(lbl)*FB_FONT_W + 12;
            int lh = FB_FONT_H + 6;
            int lx = bx + (bw-lw)/2;
            int ly = by - lh - 4;
            if (lx < 4) lx = 4;
            if (lx+lw > G_sw-4) lx = G_sw-4-lw;
            wm_rrect(lx, ly, lw, lh, 4,
                     FB_COLOR(0x28,0x28,0x28),
                     FB_COLOR(0x44,0x44,0x44));
            fb_draw_string(lx+6, ly+3, lbl,
                           C_WHITE, FB_COLOR(0x28,0x28,0x28));
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Window chrome
 * ═══════════════════════════════════════════════════════════ */

/* Circle button with optional glyph on hover */
static void draw_chrome_btn(int cx, int cy, int r,
                             fb_color_t col, int hovered, char glyph) {
    /* Fill circle */
    for (int dy=-r; dy<=r; dy++)
        for (int dx=-r; dx<=r; dx++)
            if (dx*dx+dy*dy <= r*r)
                fb_putpixel(cx+dx, cy+dy, col);

    /* Thin dark ring */
    fb_color_t ring = blend(FB_COLOR(0,0,0), col, 40);
    for (int dy=-r; dy<=r; dy++)
        for (int dx=-r; dx<=r; dx++) {
            int d=dx*dx+dy*dy;
            if (d<=r*r && d>(r-1)*(r-1))
                fb_putpixel(cx+dx, cy+dy, ring);
        }

    /* Glyph on hover */
    if (hovered && glyph) {
        char gs[2] = {glyph, 0};
        fb_draw_string(cx - FB_FONT_W/2,
                       cy - FB_FONT_H/2,
                       gs, FB_COLOR(0x18,0x18,0x18), col);
    }
}

void wm_draw_chrome(wm_window_t *w) {
    int foc = (w->id == G_focused_id);

    /* Multi-layer shadow */
    wm_shadow(w->x, w->y, w->w, w->h, WIN_RAD);

    /* Window frame */
    wm_rrect(w->x, w->y, w->w, w->h, WIN_RAD,
             C_WIN_BG, C_WIN_BORDER);

    if (w->flags & WF_NO_TITLE) {
        fb_fill_rect(w->client_x, w->client_y,
                     w->client_w, w->client_h, C_WIN_BG);
        return;
    }

    /* Titlebar background */
    fb_color_t tbc = foc ? C_TBAR_F : C_TBAR_U;
    /* Clip titlebar to inside rounded top */
    fb_set_clip(w->x+WIN_BRD, w->y+WIN_BRD,
                w->w-WIN_BRD*2, TBAR_H);
    fb_fill_rect(w->x+WIN_BRD, w->y+WIN_BRD,
                 w->w-WIN_BRD*2, TBAR_H, tbc);
    fb_clear_clip();

    /* Titlebar bottom separator */
    fb_draw_hline(w->x+WIN_BRD,
                  w->y+WIN_BRD+TBAR_H-1,
                  w->w-WIN_BRD*2, C_TBAR_LINE);

    /* Traffic-light buttons */
    int btn_y = w->y + WIN_BRD + TBAR_H/2;
    int close_x = w->x + WIN_BRD + CRBTN_R;
    int min_x   = w->x + WIN_BRD + CRBTN_M;
    int max_x   = w->x + WIN_BRD + CRBTN_X;

    int hw = (G_hover_win_id == w->id);
    int hz = G_hover_zone;

    draw_chrome_btn(close_x, btn_y, CRBTN_SZ, C_BTN_CLOSE, hw&&hz==1, 'x');
    draw_chrome_btn(min_x,   btn_y, CRBTN_SZ, C_BTN_MIN,   hw&&hz==2, '-');
    draw_chrome_btn(max_x,   btn_y, CRBTN_SZ, C_BTN_MAX,   hw&&hz==3, '+');

    /* Title text — centred between buttons and right edge */
    int title_x = w->x + WIN_BRD + CRBTN_X + CRBTN_SZ + 10;
    int title_w = w->w - WIN_BRD*2 - title_x + w->x + WIN_BRD - 8;
    if (title_w > 0) {
        fb_color_t tfg = foc ? C_TBAR_TEXT_F : C_TBAR_TEXT_U;
        fb_set_clip(title_x, w->y+WIN_BRD, title_w, TBAR_H);
        /* Centre vertically */
        int tty = w->y + WIN_BRD + (TBAR_H - FB_FONT_H) / 2;
        fb_draw_string(title_x, tty, w->title, tfg, tbc);
        fb_clear_clip();
    }

    /* Client area */
    fb_fill_rect(w->client_x, w->client_y,
                 w->client_w, w->client_h, C_WIN_BG);

    /* Resize handle indicator (bottom-right corner, 3 dots) */
    if ((w->flags & WF_RESIZABLE) && foc) {
        int hx = w->x + w->w - 12;
        int hy = w->y + w->h - 12;
        fb_putpixel(hx+6, hy+6, C_LGT);
        fb_putpixel(hx+4, hy+6, C_LGT);
        fb_putpixel(hx+6, hy+4, C_LGT);
        fb_putpixel(hx+2, hy+6, C_LGT);
        fb_putpixel(hx+6, hy+2, C_LGT);
        fb_putpixel(hx+4, hy+4, C_LGT);
    }
}

/* ═══════════════════════════════════════════════════════════
 * Full repaint
 * ═══════════════════════════════════════════════════════════ */
void wm_repaint_all(void) {
    wm_draw_desktop();
    wm_rebuild_z();
    for (int i = 0; i < G_zcount; i++) {
        wm_window_t *w = G_zlist[i];
        wm_draw_chrome(w);
        if (w->wndproc) w->wndproc(w, WM_PAINT, 0, 0);
        w->flags &= ~WF_DIRTY;
    }
    wm_draw_topbar();
    wm_draw_rail();
    wm_draw_dock();
}

/* Stubs for dock actions — implemented in wm_apps.c, 
   but we need function pointers here */
static void dock_open_terminal(void) {
    extern wm_window_t *wm_open_terminal(void);
    wm_open_terminal();
}
static void dock_open_sysmon(void) {
    extern wm_window_t *wm_open_sysmon(void);
    wm_open_sysmon();
}
static void __attribute__((unused)) dock_open_about(void) {
    extern wm_window_t *wm_open_about(void);
    wm_open_about();
    (void)dock_open_about;
}
