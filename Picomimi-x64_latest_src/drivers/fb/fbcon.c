/**
 * fbcon.c — Framebuffer Text Console
 *
 * Philosophy: VGA/VESA framebuffer is already set up by GRUB.
 * We just draw text into it. No WM, no compositor, no complexity.
 * This is the primary output surface until PaperDE arrives.
 *
 * Features:
 *   - 80/132-column terminal backed by the linear framebuffer
 *   - 8x16 VGA ROM font (same one fb.c uses)
 *   - Scrolling, cursor, basic ANSI colour (8 colours)
 *   - printk() hooks into this after init
 *   - Runs completely in-kernel, zero userspace dependency
 */

#include <kernel/types.h>
#include <drivers/fb.h>
#include <lib/string.h>
#include <lib/printk.h>

/* ------------------------------------------------------------------ */
/*  Console state                                                       */
/* ------------------------------------------------------------------ */

#define FBCON_COLS_MAX   160
#define FBCON_ROWS_MAX    60
#define FBCON_TAB_STOP     8

static int fbcon_cols = 80;
static int fbcon_rows = 25;
static int fbcon_cx   = 0;      /* cursor column */
static int fbcon_cy   = 0;      /* cursor row    */
static int fbcon_fg   = 7;      /* light grey    */
static int fbcon_bg   = 0;      /* black         */
static int fbcon_ready = 0;

/* Cell buffer — character + attribute per cell */
typedef struct {
    u8 ch;
    u8 fg;
    u8 bg;
} fbcon_cell_t;

static fbcon_cell_t fbcon_buf[FBCON_ROWS_MAX][FBCON_COLS_MAX];

/* CGA/VGA 16-colour palette in 32bpp ARGB */
static const u32 fbcon_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ------------------------------------------------------------------ */
/*  Cell → pixel drawing                                                */
/* ------------------------------------------------------------------ */

static void fbcon_draw_cell(int col, int row, fbcon_cell_t *c)
{
    const fb_info_t *fb = fb_get_info();
    if (!fb || !fb->initialized) return;

    int x = col * 8;
    int y = row * 16;

    fb_fill_rect(x, y, 8, 16, fbcon_palette[c->bg & 0xf]);
    if (c->ch > 0x20)
        fb_draw_char(x, y, c->ch, fbcon_palette[c->fg & 0xf],
                     fbcon_palette[c->bg & 0xf]);
}

static void fbcon_flush_row(int row)
{
    for (int c = 0; c < fbcon_cols; c++)
        fbcon_draw_cell(c, row, &fbcon_buf[row][c]);
}

static void fbcon_flush_all(void)
{
    for (int r = 0; r < fbcon_rows; r++)
        fbcon_flush_row(r);
}

/* ------------------------------------------------------------------ */
/*  Scroll                                                              */
/* ------------------------------------------------------------------ */

static void fbcon_scroll_up(void)
{
    /* Shift cell buffer up one row */
    for (int r = 0; r < fbcon_rows - 1; r++) {
        for (int c = 0; c < fbcon_cols; c++)
            fbcon_buf[r][c] = fbcon_buf[r+1][c];
    }
    /* Clear last row */
    for (int c = 0; c < fbcon_cols; c++) {
        fbcon_buf[fbcon_rows-1][c].ch = ' ';
        fbcon_buf[fbcon_rows-1][c].fg = fbcon_fg;
        fbcon_buf[fbcon_rows-1][c].bg = fbcon_bg;
    }
    /* Blit: scroll the framebuffer directly for speed */
    const fb_info_t *fb = fb_get_info();
    if (fb && fb->initialized) {
        int row_bytes = 16 * fb->pitch;
        u8 *base = (u8 *)fb->virt_addr;
        memmove(base, base + row_bytes,
                row_bytes * (fbcon_rows - 1));
        /* Clear last visible row in fb */
        u8 *last = base + row_bytes * (fbcon_rows - 1);
        memset(last, 0, row_bytes);
    }
}

/* ------------------------------------------------------------------ */
/*  Cursor draw/erase                                                   */
/* ------------------------------------------------------------------ */

static void fbcon_cursor_toggle(void)
{
    const fb_info_t *fb = fb_get_info();
    if (!fb || !fb->initialized) return;
    int x = fbcon_cx * 8;
    int y = fbcon_cy * 16 + 14;  /* underline cursor */
    fb_fill_rect(x, y, 8, 2, fbcon_palette[fbcon_fg & 0xf]);
}

/* ------------------------------------------------------------------ */
/*  Putchar                                                              */
/* ------------------------------------------------------------------ */

void fbcon_putchar(char ch)
{
    if (!fbcon_ready) return;

    switch (ch) {
    case '\r':
        fbcon_cx = 0;
        break;

    case '\n':
        fbcon_cx = 0;
        fbcon_cy++;
        if (fbcon_cy >= fbcon_rows) {
            fbcon_scroll_up();
            fbcon_cy = fbcon_rows - 1;
        }
        break;

    case '\t':
        fbcon_cx = (fbcon_cx + FBCON_TAB_STOP) & ~(FBCON_TAB_STOP - 1);
        if (fbcon_cx >= fbcon_cols) {
            fbcon_cx = 0;
            fbcon_cy++;
            if (fbcon_cy >= fbcon_rows) {
                fbcon_scroll_up();
                fbcon_cy = fbcon_rows - 1;
            }
        }
        break;

    case '\b':
        if (fbcon_cx > 0) {
            fbcon_cx--;
            fbcon_buf[fbcon_cy][fbcon_cx].ch = ' ';
            fbcon_draw_cell(fbcon_cx, fbcon_cy,
                            &fbcon_buf[fbcon_cy][fbcon_cx]);
        }
        break;

    default:
        if ((unsigned char)ch < 0x20) break;  /* ignore other control */

        fbcon_buf[fbcon_cy][fbcon_cx].ch = (u8)ch;
        fbcon_buf[fbcon_cy][fbcon_cx].fg = (u8)fbcon_fg;
        fbcon_buf[fbcon_cy][fbcon_cx].bg = (u8)fbcon_bg;
        fbcon_draw_cell(fbcon_cx, fbcon_cy,
                        &fbcon_buf[fbcon_cy][fbcon_cx]);
        fbcon_cx++;
        if (fbcon_cx >= fbcon_cols) {
            fbcon_cx = 0;
            fbcon_cy++;
            if (fbcon_cy >= fbcon_rows) {
                fbcon_scroll_up();
                fbcon_cy = fbcon_rows - 1;
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                           */
/* ------------------------------------------------------------------ */

void fbcon_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        fbcon_putchar(s[i]);
}

void fbcon_puts(const char *s)
{
    while (*s) fbcon_putchar(*s++);
}

void fbcon_set_color(int fg, int bg)
{
    fbcon_fg = fg & 0xf;
    fbcon_bg = bg & 0xf;
}

void fbcon_clear(void)
{
    fbcon_cx = fbcon_cy = 0;
    for (int r = 0; r < fbcon_rows; r++)
        for (int c = 0; c < fbcon_cols; c++) {
            fbcon_buf[r][c].ch = ' ';
            fbcon_buf[r][c].fg = 7;
            fbcon_buf[r][c].bg = 0;
        }
    const fb_info_t *fb = fb_get_info();
    if (fb && fb->initialized)
        fb_fill_rect(0, 0, fb->width, fb->height, 0);
}

void fbcon_init(void)
{
    const fb_info_t *fb = fb_get_info();
    if (!fb || !fb->initialized) {
        /* No framebuffer — serial-only mode, nothing to do */
        return;
    }

    fbcon_cols = (int)(fb->width  / 8);
    fbcon_rows = (int)(fb->height / 16);
    if (fbcon_cols > FBCON_COLS_MAX) fbcon_cols = FBCON_COLS_MAX;
    if (fbcon_rows > FBCON_ROWS_MAX) fbcon_rows = FBCON_ROWS_MAX;

    fbcon_ready = 1;
    fbcon_clear();

    /* Draw a thin header bar */
    fb_fill_rect(0, 0, fb->width, 17, 0x001a2b3c);
    fb_draw_string(4, 1,
        "Picomimi-x64  |  press any key for shell",
        0xAADDFF, 0x001a2b3c);
    fb_fill_rect(0, 17, fb->width, 1, 0x004466);

    /* Offset text area below header */
    fbcon_cy = 2;

    printk(KERN_INFO "fbcon: %dx%d cell console on %dx%d framebuffer\n",
           fbcon_cols, fbcon_rows, fb->width, fb->height);
}
