/**
 * Picomimi-x64 Framebuffer Driver
 *
 * Uses GRUB's Multiboot2 framebuffer tag to get a linear framebuffer from
 * whatever the BIOS/UEFI/GRUB negotiated (VESA VBE, EFI GOP, etc.).
 *
 * GRUB does all the mode-setting heavy lifting via grub-mkrescue + grub.cfg.
 * We just consume the framebuffer_addr/pitch/width/height/bpp it hands us.
 *
 * Supports:
 *   - 32 bpp (most common: BGRX or RGBX)
 *   - 24 bpp
 *   - 16 bpp (RGB565)
 *   - Fallback to VGA text mode if no framebuffer tag present
 *
 * References:
 *   - GRUB source: grub-core/video/fb/fbfill.c, fbutil.c
 *   - Multiboot2 spec section 3.6.11 (framebuffer tag type 8)
 *   - GRUB video/video.h color_type enum
 */

#ifndef _DRIVERS_FB_H
#define _DRIVERS_FB_H

#include <kernel/types.h>

/* ============================================================
 * Framebuffer info (populated from Multiboot2 tag type 8)
 * Mirrors grub_video_mode_info layout
 * ============================================================ */

#define FB_TYPE_INDEXED     0   /* Palette / indexed color */
#define FB_TYPE_RGB         1   /* Direct RGB */
#define FB_TYPE_EGA_TEXT    2   /* EGA/VGA text mode */

typedef struct {
    u64     phys_addr;      /* Physical base address of framebuffer */
    u64     virt_addr;      /* Virtual address after mapping */
    u32     pitch;          /* Bytes per scanline */
    u32     width;          /* Pixels wide */
    u32     height;         /* Pixels tall */
    u8      bpp;            /* Bits per pixel */
    u8      type;           /* FB_TYPE_* */

    /* RGB channel masks (from Multiboot2 color_info) */
    u8      red_pos;
    u8      red_mask_size;
    u8      green_pos;
    u8      green_mask_size;
    u8      blue_pos;
    u8      blue_mask_size;

    int     initialized;
    int     is_text_fallback;
} fb_info_t;

/* ============================================================
 * Color (always stored as 32-bit ARGB internally)
 * ============================================================ */

typedef u32 fb_color_t;   /* 0xAARRGGBB */

#define FB_COLOR(r, g, b)       ((fb_color_t)(0xFF000000 | ((r)<<16) | ((g)<<8) | (b)))
#define FB_COLOR_A(a, r, g, b)  ((fb_color_t)(((a)<<24) | ((r)<<16) | ((g)<<8) | (b)))

/* Common palette */
#define FB_BLACK        FB_COLOR(0x00, 0x00, 0x00)
#define FB_WHITE        FB_COLOR(0xFF, 0xFF, 0xFF)
#define FB_GRAY         FB_COLOR(0xC0, 0xC0, 0xC0)
#define FB_DARK_GRAY    FB_COLOR(0x40, 0x40, 0x40)
#define FB_MID_GRAY     FB_COLOR(0x80, 0x80, 0x80)
#define FB_LIGHT_GRAY   FB_COLOR(0xD4, 0xD0, 0xC8)
#define FB_RED          FB_COLOR(0xFF, 0x00, 0x00)
#define FB_GREEN        FB_COLOR(0x00, 0xFF, 0x00)
#define FB_BLUE         FB_COLOR(0x00, 0x00, 0xFF)
#define FB_NAVY         FB_COLOR(0x00, 0x00, 0x80)
#define FB_TEAL         FB_COLOR(0x00, 0x80, 0x80)
#define FB_YELLOW       FB_COLOR(0xFF, 0xFF, 0x00)
#define FB_CYAN         FB_COLOR(0x00, 0xFF, 0xFF)
#define FB_MAGENTA      FB_COLOR(0xFF, 0x00, 0xFF)
#define FB_ORANGE       FB_COLOR(0xFF, 0x80, 0x00)

/* Win98/classic desktop palette */
#define FB_WIN_DESKTOP      FB_COLOR(0x00, 0x80, 0x80)   /* teal */
#define FB_WIN_TITLEBAR     FB_COLOR(0x00, 0x00, 0x80)   /* navy */
#define FB_WIN_TITLEBAR_END FB_COLOR(0x10, 0x84, 0xD0)   /* gradient end */
#define FB_WIN_TITLETEXT    FB_COLOR(0xFF, 0xFF, 0xFF)
#define FB_WIN_FACE         FB_COLOR(0xD4, 0xD0, 0xC8)   /* button face */
#define FB_WIN_HILIGHT      FB_COLOR(0xFF, 0xFF, 0xFF)
#define FB_WIN_SHADOW       FB_COLOR(0x40, 0x40, 0x40)
#define FB_WIN_FRAME        FB_COLOR(0x00, 0x00, 0x00)
#define FB_WIN_TASKBAR      FB_COLOR(0xC0, 0xC0, 0xC0)

/* ============================================================
 * Initialization
 * ============================================================ */

/**
 * fb_init() - Parse Multiboot2 tag and set up framebuffer.
 *
 * Call from kernel_main() right after parse_multiboot2().
 * Passes in the raw Multiboot2 framebuffer tag pointer.
 *
 * @mbi_tag: pointer to the multiboot2_tag_framebuffer struct
 * Returns 0 on success, -1 on fallback to text mode.
 */
int fb_init(void *mbi_tag);

/**
 * fb_get_info() - Get pointer to framebuffer info struct.
 */
const fb_info_t *fb_get_info(void);

/* ============================================================
 * Primitive drawing
 * ============================================================ */

void fb_clear(fb_color_t color);
void fb_putpixel(int x, int y, fb_color_t color);
fb_color_t fb_getpixel(int x, int y);

void fb_fill_rect(int x, int y, int w, int h, fb_color_t color);
void fb_draw_rect(int x, int y, int w, int h, fb_color_t color);
void fb_draw_line(int x0, int y0, int x1, int y1, fb_color_t color);
void fb_draw_hline(int x, int y, int len, fb_color_t color);
void fb_draw_vline(int x, int y, int len, fb_color_t color);

/* 3D-style raised/sunken rectangle (Win9x look) */
void fb_draw_3d_rect(int x, int y, int w, int h, int raised);

/* Horizontal gradient fill */
void fb_fill_gradient_h(int x, int y, int w, int h,
                        fb_color_t c_start, fb_color_t c_end);

/* Copy rectangle (for window movement, double-buffering) */
void fb_blit(int dst_x, int dst_y,
             int src_x, int src_y,
             int w, int h);

/* ============================================================
 * Font / text rendering
 * Uses built-in 8x16 PSF-style bitmap font (VGA ROM derived)
 * ============================================================ */

void fb_draw_char(int x, int y, char c, fb_color_t fg, fb_color_t bg);
void fb_draw_string(int x, int y, const char *s, fb_color_t fg, fb_color_t bg);
void fb_draw_string_transparent(int x, int y, const char *s, fb_color_t fg);

/* Font metrics */
#define FB_FONT_W   8
#define FB_FONT_H   16

/* ============================================================
 * Double-buffering (optional, for flicker-free rendering)
 * ============================================================ */

int  fb_backbuffer_init(void);   /* Allocate back buffer */
void fb_flip(void);              /* Blit back buffer -> front */
void fb_set_backbuffer(int en);  /* Enable/disable double buffering */

/* ============================================================
 * Clipping
 * ============================================================ */

void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);

#endif /* _DRIVERS_FB_H */

/* Enhanced double-buffer + compositing (PaperDE GPU layer) */
void  fb_mark_dirty(int x, int y, int w, int h);
void  fb_flip_dirty(void);
void  fb_fill_roundrect(int x, int y, int w, int h, int r, u32 color);
void  fb_draw_rect_outline(int x, int y, int w, int h, int thickness, u32 color);
void  fb_blit_alpha(int dx, int dy, int w, int h, const u32 *src, int src_stride);
void  fb_fill_gradient_h(int x, int y, int w, int h, u32 col_left, u32 col_right);
void  fb_fill_gradient_v(int x, int y, int w, int h, u32 col_top, u32 col_bottom);
void  fb_copy_region(int dst_x, int dst_y, int src_x, int src_y, int w, int h);
int   fb_width(void);
int   fb_height(void);
int   fb_bpp(void);
int   fb_pitch(void);
u8   *fb_backbuf(void);
u8   *fb_frontbuf(void);
