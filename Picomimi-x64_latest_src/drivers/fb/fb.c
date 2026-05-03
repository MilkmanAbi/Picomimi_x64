/**
 * Picomimi-x64 Framebuffer Driver Implementation
 *
 * GRUB sets up a linear framebuffer via VESA VBE or EFI GOP before
 * handing control to the kernel. It passes all parameters in the
 * Multiboot2 tag type 8 (framebuffer). We just need to:
 *   1. Read those parameters
 *   2. Map the physical framebuffer into virtual address space
 *   3. Provide pixel/rect/text drawing primitives
 *
 * grub.cfg must request a video mode:
 *   set gfxmode=1024x768x32
 *   set gfxpayload=keep
 *
 * Without this GRUB may hand us a text-mode framebuffer (type 2),
 * in which case we fall back gracefully to the existing VGA driver.
 *
 * Pixel format conversion follows GRUB's grub-core/video/fb/fbutil.c:
 *   - Read red_field_pos / green_field_pos / blue_field_pos from tag
 *   - Build a pack_color() function from those at init time
 *
 * The 8x16 font glyph table is derived from the standard VGA ROM
 * character set (CP437), which GRUB itself uses for its terminal.
 * Same source: grub-core/font/font_cmd.c + Unicode PUA mapping.
 */

#include <kernel/types.h>
#include <drivers/fb.h>
#include <mm/vmm.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printk.h>

/* ============================================================
 * Module state
 * ============================================================ */

static fb_info_t  fb;
static u8        *fb_buf     = NULL;   /* Points to active draw surface */
static u8        *fb_back    = NULL;   /* Back buffer (optional) */
static int        fb_dbl     = 0;      /* Double-buffering enabled? */

/* Clip rectangle */
static int clip_x  = 0, clip_y  = 0;
static int clip_x2 = 0, clip_y2 = 0;
static int clip_en = 0;

/* ============================================================
 * Multiboot2 framebuffer tag layout
 * (matches spec 3.6.11 - we mirror it here to avoid pulling in
 *  the full multiboot2 header which lives in kernel.c)
 * ============================================================ */

struct mb2_fb_tag {
    u32  type;          /* 8 */
    u32  size;
    u64  addr;
    u32  pitch;
    u32  width;
    u32  height;
    u8   bpp;
    u8   fb_type;       /* 0=indexed, 1=RGB, 2=EGA text */
    u16  reserved;
    /* For type 1 (RGB): */
    u8   red_pos;
    u8   red_mask;
    u8   green_pos;
    u8   green_mask;
    u8   blue_pos;
    u8   blue_mask;
} __packed;

/* ============================================================
 * 8x16 VGA ROM font (CP437 subset, glyphs 0x20-0x7E)
 * Extracted from standard VGA BIOS font at segment 0xC000.
 * GRUB uses the same data in grub-core/term/i386/pc/vga_text.c
 * Each entry = 16 bytes, one bit per pixel, MSB = leftmost pixel.
 * ============================================================ */

static const u8 font8x16[96][16] = {
    /* 0x20 space */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x21 !     */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0,0,0,0,0,0},
    /* 0x22 "     */ {0,0,0x66,0x66,0x66,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x23 #     */ {0,0,0x36,0x36,0x7F,0x36,0x36,0x7F,0x36,0x36,0,0,0,0,0,0},
    /* 0x24 $     */ {0,0x18,0x7E,0xDB,0xD8,0xD8,0x7E,0x1B,0x1B,0xDB,0x7E,0x18,0,0,0,0},
    /* 0x25 %     */ {0,0,0x63,0x66,0x0C,0x18,0x33,0x63,0,0,0,0,0,0,0,0},
    /* 0x26 &     */ {0,0,0x1C,0x36,0x36,0x1C,0x3B,0x6E,0x66,0x66,0x3B,0,0,0,0,0},
    /* 0x27 '     */ {0,0,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x28 (     */ {0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0,0},
    /* 0x29 )     */ {0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0,0},
    /* 0x2A *     */ {0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0,0},
    /* 0x2B +     */ {0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0,0,0},
    /* 0x2C ,     */ {0,0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    /* 0x2D -     */ {0,0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0,0},
    /* 0x2E .     */ {0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0,0},
    /* 0x2F /     */ {0,0,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0,0,0,0,0,0,0},
    /* 0x30 0     */ {0,0,0x3E,0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x3E,0,0,0,0,0},
    /* 0x31 1     */ {0,0,0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0},
    /* 0x32 2     */ {0,0,0x3C,0x66,0x66,0x06,0x0C,0x18,0x30,0x66,0x7E,0,0,0,0,0},
    /* 0x33 3     */ {0,0,0x7E,0x66,0x06,0x06,0x3E,0x06,0x06,0x66,0x7E,0,0,0,0,0},
    /* 0x34 4     */ {0,0,0x0E,0x1E,0x36,0x66,0x66,0x7F,0x06,0x06,0x0F,0,0,0,0,0},
    /* 0x35 5     */ {0,0,0x7E,0x60,0x60,0x7C,0x06,0x06,0x06,0x66,0x3C,0,0,0,0,0},
    /* 0x36 6     */ {0,0,0x1C,0x30,0x60,0x7C,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0},
    /* 0x37 7     */ {0,0,0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x38 8     */ {0,0,0x3C,0x66,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C,0,0,0,0,0},
    /* 0x39 9     */ {0,0,0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x0C,0x38,0,0,0,0,0},
    /* 0x3A :     */ {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0},
    /* 0x3B ;     */ {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0},
    /* 0x3C <     */ {0,0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0},
    /* 0x3D =     */ {0,0,0,0,0,0x7E,0,0x7E,0,0,0,0,0,0,0,0},
    /* 0x3E >     */ {0,0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0,0},
    /* 0x3F ?     */ {0,0,0x3C,0x66,0x66,0x06,0x0C,0x0C,0x0C,0,0x0C,0,0,0,0,0},
    /* 0x40 @     */ {0,0,0x3E,0x63,0x6F,0x6F,0x6E,0x60,0x60,0x63,0x3E,0,0,0,0,0},
    /* 0x41 A     */ {0,0,0x08,0x1C,0x36,0x63,0x63,0x7F,0x63,0x63,0x63,0,0,0,0,0},
    /* 0x42 B     */ {0,0,0x7C,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x7C,0,0,0,0,0},
    /* 0x43 C     */ {0,0,0x3C,0x66,0x60,0x60,0x60,0x60,0x60,0x66,0x3C,0,0,0,0,0},
    /* 0x44 D     */ {0,0,0x78,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0x78,0,0,0,0,0},
    /* 0x45 E     */ {0,0,0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x7E,0,0,0,0,0},
    /* 0x46 F     */ {0,0,0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0,0,0,0,0},
    /* 0x47 G     */ {0,0,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x66,0x3E,0,0,0,0,0},
    /* 0x48 H     */ {0,0,0x63,0x63,0x63,0x63,0x7F,0x63,0x63,0x63,0x63,0,0,0,0,0},
    /* 0x49 I     */ {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0},
    /* 0x4A J     */ {0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x6C,0x38,0,0,0,0,0},
    /* 0x4B K     */ {0,0,0x63,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x63,0,0,0,0,0},
    /* 0x4C L     */ {0,0,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0,0,0,0,0},
    /* 0x4D M     */ {0,0,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0x63,0,0,0,0,0},
    /* 0x4E N     */ {0,0,0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x63,0x63,0,0,0,0,0},
    /* 0x4F O     */ {0,0,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0},
    /* 0x50 P     */ {0,0,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0,0,0,0,0},
    /* 0x51 Q     */ {0,0,0x3C,0x66,0x66,0x66,0x66,0x66,0x76,0x6C,0x36,0,0,0,0,0},
    /* 0x52 R     */ {0,0,0x7C,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x63,0,0,0,0,0},
    /* 0x53 S     */ {0,0,0x3C,0x66,0x60,0x60,0x3C,0x06,0x06,0x66,0x3C,0,0,0,0,0},
    /* 0x54 T     */ {0,0,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x55 U     */ {0,0,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x3E,0,0,0,0,0},
    /* 0x56 V     */ {0,0,0x63,0x63,0x63,0x63,0x63,0x63,0x36,0x1C,0x08,0,0,0,0,0},
    /* 0x57 W     */ {0,0,0x63,0x63,0x63,0x63,0x6B,0x6B,0x7F,0x77,0x63,0,0,0,0,0},
    /* 0x58 X     */ {0,0,0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x63,0x63,0,0,0,0,0},
    /* 0x59 Y     */ {0,0,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x5A Z     */ {0,0,0x7E,0x06,0x0C,0x18,0x18,0x30,0x60,0x60,0x7E,0,0,0,0,0},
    /* 0x5B [     */ {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0},
    /* 0x5C \     */ {0,0,0xC0,0x60,0x30,0x18,0x0C,0x06,0x03,0,0,0,0,0,0,0},
    /* 0x5D ]     */ {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0},
    /* 0x5E ^     */ {0,0,0x08,0x1C,0x36,0x63,0,0,0,0,0,0,0,0,0,0},
    /* 0x5F _     */ {0,0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0},
    /* 0x60 `     */ {0,0,0x18,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x61 a     */ {0,0,0,0,0,0x3C,0x06,0x3E,0x66,0x66,0x3B,0,0,0,0,0},
    /* 0x62 b     */ {0,0,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0,0,0,0,0},
    /* 0x63 c     */ {0,0,0,0,0,0x3C,0x66,0x60,0x60,0x66,0x3C,0,0,0,0,0},
    /* 0x64 d     */ {0,0,0x06,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x3B,0,0,0,0,0},
    /* 0x65 e     */ {0,0,0,0,0,0x3C,0x66,0x7E,0x60,0x66,0x3C,0,0,0,0,0},
    /* 0x66 f     */ {0,0,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x30,0x30,0,0,0,0,0},
    /* 0x67 g     */ {0,0,0,0,0,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C,0,0,0},
    /* 0x68 h     */ {0,0,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0},
    /* 0x69 i     */ {0,0,0x18,0x00,0x00,0x78,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0},
    /* 0x6A j     */ {0,0,0x06,0,0,0x1E,0x06,0x06,0x06,0x06,0x66,0x3C,0,0,0,0},
    /* 0x6B k     */ {0,0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0x63,0,0,0,0,0},
    /* 0x6C l     */ {0,0,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0},
    /* 0x6D m     */ {0,0,0,0,0,0x66,0x7F,0x6B,0x6B,0x6B,0x63,0,0,0,0,0},
    /* 0x6E n     */ {0,0,0,0,0,0x7C,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0},
    /* 0x6F o     */ {0,0,0,0,0,0x3C,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0},
    /* 0x70 p     */ {0,0,0,0,0,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0,0,0},
    /* 0x71 q     */ {0,0,0,0,0,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0,0,0},
    /* 0x72 r     */ {0,0,0,0,0,0x7C,0x66,0x60,0x60,0x60,0x60,0,0,0,0,0},
    /* 0x73 s     */ {0,0,0,0,0,0x3E,0x60,0x3C,0x06,0x66,0x3C,0,0,0,0,0},
    /* 0x74 t     */ {0,0,0,0x30,0x30,0x7C,0x30,0x30,0x30,0x36,0x1C,0,0,0,0,0},
    /* 0x75 u     */ {0,0,0,0,0,0x66,0x66,0x66,0x66,0x66,0x3E,0,0,0,0,0},
    /* 0x76 v     */ {0,0,0,0,0,0x63,0x63,0x63,0x36,0x1C,0x08,0,0,0,0,0},
    /* 0x77 w     */ {0,0,0,0,0,0x63,0x6B,0x6B,0x6B,0x7F,0x36,0,0,0,0,0},
    /* 0x78 x     */ {0,0,0,0,0,0x63,0x36,0x1C,0x1C,0x36,0x63,0,0,0,0,0},
    /* 0x79 y     */ {0,0,0,0,0,0x66,0x66,0x66,0x3E,0x06,0x3C,0,0,0,0,0},
    /* 0x7A z     */ {0,0,0,0,0,0x7E,0x0C,0x18,0x30,0x60,0x7E,0,0,0,0,0},
    /* 0x7B {     */ {0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0,0,0,0,0},
    /* 0x7C |     */ {0,0,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x7D }     */ {0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0,0,0,0,0},
    /* 0x7E ~     */ {0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Pack an internal ARGB color into the native framebuffer pixel format */
static inline u32 pack_color(fb_color_t c) {
    u8 r = (c >> 16) & 0xFF;
    u8 g = (c >>  8) & 0xFF;
    u8 b = (c >>  0) & 0xFF;
    return (u32)(
        ((u32)r << fb.red_pos)   |
        ((u32)g << fb.green_pos) |
        ((u32)b << fb.blue_pos)
    );
}

static inline u8 *pixel_addr(int x, int y) {
    return fb_buf + (u32)y * fb.pitch + (u32)x * (fb.bpp / 8);
}

static inline void write_pixel_raw(u8 *addr, u32 packed) {
    switch (fb.bpp) {
    case 32:
        *(u32 *)addr = packed;
        break;
    case 24:
        addr[0] = (packed >>  0) & 0xFF;
        addr[1] = (packed >>  8) & 0xFF;
        addr[2] = (packed >> 16) & 0xFF;
        break;
    case 16:
        *(u16 *)addr = (u16)packed;
        break;
    default:
        *(u32 *)addr = packed;
        break;
    }
}

/* ============================================================
 * Clipping helpers
 * ============================================================ */

static inline int clip_x_lo(void) { return clip_en ? clip_x  : 0; }
static inline int clip_y_lo(void) { return clip_en ? clip_y  : 0; }
static inline int clip_x_hi(void) { return clip_en ? clip_x2 : (int)fb.width; }
static inline int clip_y_hi(void) { return clip_en ? clip_y2 : (int)fb.height; }

static inline int in_clip(int x, int y) {
    return (x >= clip_x_lo() && x < clip_x_hi() &&
            y >= clip_y_lo() && y < clip_y_hi());
}

/* ============================================================
 * fb_init
 * ============================================================ */

int fb_init(void *mbi_tag) {
    if (!mbi_tag) {
        printk(KERN_WARNING "[FB] No Multiboot2 framebuffer tag found, using VGA text fallback\n");
        fb.is_text_fallback = 1;
        fb.initialized = 0;
        return -1;
    }

    struct mb2_fb_tag *tag = (struct mb2_fb_tag *)mbi_tag;

    if (tag->fb_type == FB_TYPE_EGA_TEXT) {
        printk(KERN_WARNING "[FB] GRUB gave us EGA text mode. Add 'set gfxpayload=keep' to grub.cfg\n");
        fb.is_text_fallback = 1;
        fb.initialized = 0;
        return -1;
    }

    fb.phys_addr      = tag->addr;
    fb.pitch          = tag->pitch;
    fb.width          = tag->width;
    fb.height         = tag->height;
    fb.bpp            = tag->bpp;
    fb.type           = tag->fb_type;

    /* RGB channel info */
    if (tag->fb_type == FB_TYPE_RGB) {
        fb.red_pos        = tag->red_pos;
        fb.red_mask_size  = tag->red_mask;
        fb.green_pos      = tag->green_pos;
        fb.green_mask_size= tag->green_mask;
        fb.blue_pos       = tag->blue_pos;
        fb.blue_mask_size = tag->blue_mask;
    } else {
        /* Indexed - use sane defaults (RGB in low bytes) */
        fb.red_pos = 16; fb.red_mask_size = 8;
        fb.green_pos = 8; fb.green_mask_size = 8;
        fb.blue_pos = 0; fb.blue_mask_size = 8;
    }

    /*
     * Map framebuffer into virtual address space.
     * The kernel's higher-half VMM needs to be asked to map
     * the physical pages. We use ioremap if available.
     */
    extern void *ioremap(u64 phys, u64 size);
    u64 fb_size = (u64)fb.pitch * fb.height;
    void *vaddr = ioremap(fb.phys_addr, fb_size);
    if (!vaddr) {
        /* ioremap not yet up or failed - use identity/higher-half offset */
        fb.virt_addr = fb.phys_addr + 0xFFFFFFFF80000000ULL;
        printk(KERN_WARNING "[FB] ioremap failed, using direct offset mapping\n");
    } else {
        fb.virt_addr = (u64)vaddr;
    }

    fb_buf = (u8 *)fb.virt_addr;
    fb.initialized = 1;

    clip_x2 = fb.width;
    clip_y2 = fb.height;
    clip_en = 0;

    printk(KERN_INFO "[FB] Framebuffer: %ux%u @%ubpp, pitch=%u, phys=0x%lx virt=0x%lx\n",
           fb.width, fb.height, fb.bpp, fb.pitch, fb.phys_addr, fb.virt_addr);
    printk(KERN_INFO "[FB] RGB layout: R[%u+%u] G[%u+%u] B[%u+%u]\n",
           fb.red_pos, fb.red_mask_size,
           fb.green_pos, fb.green_mask_size,
           fb.blue_pos, fb.blue_mask_size);

    return 0;
}

const fb_info_t *fb_get_info(void) {
    return &fb;
}

/* ============================================================
 * Clipping API
 * ============================================================ */

void fb_set_clip(int x, int y, int w, int h) {
    clip_x  = x;      clip_y  = y;
    clip_x2 = x + w;  clip_y2 = y + h;
    clip_en = 1;
}

void fb_clear_clip(void) {
    clip_en = 0;
    clip_x2 = fb.width;
    clip_y2 = fb.height;
}

/* ============================================================
 * Pixel operations
 * ============================================================ */

void fb_clear(fb_color_t color) {
    if (!fb.initialized) return;
    u32 packed = pack_color(color);
    u32 bpp_bytes = fb.bpp / 8;

    for (u32 y = 0; y < fb.height; y++) {
        u8 *row = fb_buf + y * fb.pitch;
        for (u32 x = 0; x < fb.width; x++) {
            write_pixel_raw(row + x * bpp_bytes, packed);
        }
    }
}

void fb_putpixel(int x, int y, fb_color_t color) {
    if (!fb.initialized) return;
    if (x < 0 || y < 0 || x >= (int)fb.width || y >= (int)fb.height) return;
    if (clip_en && !in_clip(x, y)) return;
    write_pixel_raw(pixel_addr(x, y), pack_color(color));
}

fb_color_t fb_getpixel(int x, int y) {
    if (!fb.initialized || x < 0 || y < 0 ||
        x >= (int)fb.width || y >= (int)fb.height) return 0;
    u8 *addr = pixel_addr(x, y);
    u32 raw;
    switch (fb.bpp) {
    case 32: raw = *(u32 *)addr; break;
    case 24: raw = addr[0] | (addr[1]<<8) | (addr[2]<<16); break;
    case 16: raw = *(u16 *)addr; break;
    default: raw = *(u32 *)addr; break;
    }
    u8 r = (raw >> fb.red_pos)   & 0xFF;
    u8 g = (raw >> fb.green_pos) & 0xFF;
    u8 b = (raw >> fb.blue_pos)  & 0xFF;
    return FB_COLOR(r, g, b);
}

/* ============================================================
 * Rectangle fills
 * ============================================================ */

void fb_fill_rect(int x, int y, int w, int h, fb_color_t color) {
    if (!fb.initialized || w <= 0 || h <= 0) return;

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (clip_en) {
        if (x0 < clip_x)  x0 = clip_x;
        if (y0 < clip_y)  y0 = clip_y;
        if (x1 > clip_x2) x1 = clip_x2;
        if (y1 > clip_y2) y1 = clip_y2;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb.width)  x1 = fb.width;
    if (y1 > (int)fb.height) y1 = fb.height;
    if (x0 >= x1 || y0 >= y1) return;

    u32 packed = pack_color(color);
    u32 bpp_bytes = fb.bpp / 8;

    for (int py = y0; py < y1; py++) {
        u8 *row = fb_buf + py * fb.pitch + x0 * bpp_bytes;
        for (int px = x0; px < x1; px++) {
            write_pixel_raw(row, packed);
            row += bpp_bytes;
        }
    }
}

void fb_draw_rect(int x, int y, int w, int h, fb_color_t color) {
    if (!fb.initialized || w <= 0 || h <= 0) return;
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_draw_hline(int x, int y, int len, fb_color_t color) {
    if (!fb.initialized) return;
    fb_fill_rect(x, y, len, 1, color);
}

void fb_draw_vline(int x, int y, int len, fb_color_t color) {
    if (!fb.initialized) return;
    fb_fill_rect(x, y, 1, len, color);
}

/* Bresenham line */
void fb_draw_line(int x0, int y0, int x1, int y1, fb_color_t color) {
    if (!fb.initialized) return;
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        fb_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ============================================================
 * 3D-style raised/sunken rectangle (Win9x look)
 * raised = 1: top/left bright, bottom/right dark (button up)
 * raised = 0: top/left dark, bottom/right bright (button pressed)
 * ============================================================ */

void fb_draw_3d_rect(int x, int y, int w, int h, int raised) {
    if (!fb.initialized || w <= 1 || h <= 1) return;
    fb_color_t hi  = raised ? FB_WIN_HILIGHT : FB_WIN_SHADOW;
    fb_color_t lo  = raised ? FB_WIN_SHADOW  : FB_WIN_HILIGHT;
    fb_color_t mid = FB_WIN_FACE;

    /* Outer edges */
    fb_draw_hline(x,         y,         w,   raised ? FB_WIN_HILIGHT : FB_WIN_SHADOW);
    fb_draw_vline(x,         y,         h,   raised ? FB_WIN_HILIGHT : FB_WIN_SHADOW);
    fb_draw_hline(x,         y + h - 1, w,   raised ? FB_WIN_SHADOW  : FB_WIN_HILIGHT);
    fb_draw_vline(x + w - 1, y,         h,   raised ? FB_WIN_SHADOW  : FB_WIN_HILIGHT);

    /* Inner edges (1px inside) */
    if (w > 2 && h > 2) {
        fb_draw_hline(x + 1,     y + 1,     w - 2, hi);
        fb_draw_vline(x + 1,     y + 1,     h - 2, hi);
        fb_draw_hline(x + 1,     y + h - 2, w - 2, lo);
        fb_draw_vline(x + w - 2, y + 1,     h - 2, lo);
    }
    (void)mid;
}

/* fb_fill_gradient_h — see enhanced version below */

/* ============================================================
 * Blit (copy rectangle within framebuffer)
 * Used for window dragging and scrolling
 * ============================================================ */

void fb_blit(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    if (!fb.initialized || w <= 0 || h <= 0) return;
    u32 bpp_bytes = fb.bpp / 8;
    u32 row_bytes = (u32)w * bpp_bytes;

    if (dst_y < src_y) {
        /* Copy top to bottom */
        for (int py = 0; py < h; py++) {
            u8 *src = fb_buf + (src_y + py) * fb.pitch + src_x * bpp_bytes;
            u8 *dst = fb_buf + (dst_y + py) * fb.pitch + dst_x * bpp_bytes;
            memcpy(dst, src, row_bytes);
        }
    } else {
        /* Copy bottom to top (overlapping regions) */
        for (int py = h - 1; py >= 0; py--) {
            u8 *src = fb_buf + (src_y + py) * fb.pitch + src_x * bpp_bytes;
            u8 *dst = fb_buf + (dst_y + py) * fb.pitch + dst_x * bpp_bytes;
            memcpy(dst, src, row_bytes);
        }
    }
}

/* ============================================================
 * Font / text rendering
 * ============================================================ */

void fb_draw_char(int x, int y, char c, fb_color_t fg, fb_color_t bg) {
    if (!fb.initialized) return;
    if (c < 0x20 || c > 0x7E) c = '?';

    const u8 *glyph = font8x16[(u8)(c - 0x20)];
    u32 packed_fg = pack_color(fg);
    u32 packed_bg = pack_color(bg);
    int draw_bg   = (bg != (fb_color_t)(FB_COLOR(0,0,0) - 1)); /* sentinel: -1 = transparent */

    for (int row = 0; row < FB_FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)fb.height) continue;
        u8 bits = glyph[row];
        for (int col = 0; col < FB_FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= (int)fb.width) continue;
            if (clip_en && !in_clip(px, py)) continue;
            int set = (bits >> (7 - col)) & 1;
            if (set) {
                write_pixel_raw(pixel_addr(px, py), packed_fg);
            } else if (draw_bg) {
                write_pixel_raw(pixel_addr(px, py), packed_bg);
            }
        }
    }
}

void fb_draw_string(int x, int y, const char *s, fb_color_t fg, fb_color_t bg) {
    if (!fb.initialized || !s) return;
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FB_FONT_H; }
        else if (*s == '\t') { cx += FB_FONT_W * 4; }
        else { fb_draw_char(cx, y, *s, fg, bg); cx += FB_FONT_W; }
        s++;
    }
}

/* Transparent background variant (used for overlays) */
void fb_draw_string_transparent(int x, int y, const char *s, fb_color_t fg) {
    if (!fb.initialized || !s) return;
    /* Use sentinel bg value to signal "no background" */
    fb_color_t transparent_sentinel = (fb_color_t)0xFFFFFFFF;
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FB_FONT_H; }
        else {
            /* Draw only foreground pixels */
            const u8 *glyph = font8x16[(*s < 0x20 || *s > 0x7E) ? 0 : (u8)(*s - 0x20)];
            u32 packed_fg = pack_color(fg);
            for (int row = 0; row < FB_FONT_H; row++) {
                int py = y + row;
                if (py < 0 || py >= (int)fb.height) continue;
                u8 bits = glyph[row];
                for (int col = 0; col < FB_FONT_W; col++) {
                    int px = cx + col;
                    if (px < 0 || px >= (int)fb.width) continue;
                    if ((bits >> (7 - col)) & 1)
                        write_pixel_raw(pixel_addr(px, py), packed_fg);
                }
            }
            cx += FB_FONT_W;
        }
        s++;
    }
    (void)transparent_sentinel;
}

/* ============================================================
 * Double buffering
 * ============================================================ */

int fb_backbuffer_init(void) {
    if (!fb.initialized) return -1;
    u64 sz = (u64)fb.pitch * fb.height;
    fb_back = (u8 *)kmalloc(sz, GFP_KERNEL);
    if (!fb_back) {
        printk(KERN_WARNING "[FB] Could not allocate back buffer (%lu bytes)\n", sz);
        return -1;
    }
    printk(KERN_INFO "[FB] Double buffering enabled (%lu KB)\n", sz / 1024);
    return 0;
}

void fb_set_backbuffer(int en) {
    if (!fb_back) return;
    fb_dbl = en;
    if (en) fb_buf = fb_back;
    else    fb_buf = (u8 *)fb.virt_addr;
}

void fb_flip(void) {
    if (!fb_dbl || !fb_back || !fb.initialized) return;
    u64 sz = (u64)fb.pitch * fb.height;
    memcpy((u8 *)fb.virt_addr, fb_back, sz);
}

/* ============================================================
 * Enhanced double-buffering + compositing primitives
 * Added for PaperDE GPU layer
 * ============================================================ */

/* Dirty rectangle tracking — only blit changed regions to VRAM */
#define MAX_DIRTY_RECTS  64

typedef struct {
    int x, y, w, h;
} dirty_rect_t;

static dirty_rect_t dirty_rects[MAX_DIRTY_RECTS];
static int          dirty_count  = 0;
static int          dirty_all    = 1;   /* First flip is always full */

void fb_mark_dirty(int x, int y, int w, int h) {
    if (!fb_dbl) return;
    if (dirty_all) return;  /* Already full-dirty */

    /* Clamp */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb.width)  w = (int)fb.width  - x;
    if (y + h > (int)fb.height) h = (int)fb.height - y;
    if (w <= 0 || h <= 0) return;

    if (dirty_count >= MAX_DIRTY_RECTS) {
        dirty_all = 1;  /* Too many rects — fall back to full flip */
        return;
    }
    dirty_rects[dirty_count++] = (dirty_rect_t){ x, y, w, h };
}

void fb_flip_dirty(void) {
    if (!fb_dbl || !fb_back || !fb.initialized) return;

    int bpp = fb.bpp / 8;

    if (dirty_all || dirty_count == 0) {
        /* Full blit */
        memcpy((u8 *)fb.virt_addr, fb_back, (size_t)fb.pitch * fb.height);
        dirty_all   = 0;
        dirty_count = 0;
        return;
    }

    for (int i = 0; i < dirty_count; i++) {
        dirty_rect_t *r = &dirty_rects[i];
        for (int row = 0; row < r->h; row++) {
            u8 *src = fb_back         + (r->y + row) * fb.pitch + r->x * bpp;
            u8 *dst = (u8 *)fb.virt_addr + (r->y + row) * fb.pitch + r->x * bpp;
            memcpy(dst, src, (size_t)r->w * bpp);
        }
    }
    dirty_count = 0;
}

/* ---- Compositing primitives ---- */

/* Alpha-blend a single ARGB pixel over a destination pixel (no hardware accel) */
static inline u32 blend_pixel(u32 dst, u32 src) {
    u32 a = (src >> 24) & 0xFF;
    if (a == 0)   return dst;
    if (a == 255) return src & 0xFFFFFF;
    u32 rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a)) >> 8;
    u32 g  = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

/* Draw a filled rounded rectangle (used extensively by PaperDE widgets) */
void fb_fill_roundrect(int x, int y, int w, int h, int r, u32 color) {
    if (!fb_buf || !fb.initialized) return;
    /* Clamp */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;

    int bpp = fb.bpp / 8;

    for (int py = 0; py < h; py++) {
        int ay = (py < r) ? (r - py) : (py >= h - r) ? (py - (h - r - 1)) : 0;
        for (int px = 0; px < w; px++) {
            int ax = (px < r) ? (r - px) : (px >= w - r) ? (px - (w - r - 1)) : 0;
            /* Simple circle approximation for corner */
            if (ax > 0 && ay > 0 && (ax * ax + ay * ay) > r * r) continue;

            int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= (int)fb.width || fy >= (int)fb.height) continue;

            u8 *p = fb_buf + fy * fb.pitch + fx * bpp;
            if (bpp == 4) { *(u32 *)p = color; }
            else if (bpp == 3) { p[0] = color; p[1] = color >> 8; p[2] = color >> 16; }
        }
    }
    fb_mark_dirty(x, y, w, h);
}

/* Draw rectangle outline */
void fb_draw_rect_outline(int x, int y, int w, int h, int thickness, u32 color) {
    fb_fill_rect(x, y, w, thickness, color);                   /* top */
    fb_fill_rect(x, y + h - thickness, w, thickness, color);   /* bottom */
    fb_fill_rect(x, y, thickness, h, color);                   /* left */
    fb_fill_rect(x + w - thickness, y, thickness, h, color);   /* right */
}

/* Blit a 32bpp ARGB sprite onto the framebuffer with per-pixel alpha */
void fb_blit_alpha(int dx, int dy, int w, int h, const u32 *src, int src_stride) {
    if (!fb_buf || !fb.initialized) return;
    int bpp = fb.bpp / 8;
    for (int row = 0; row < h; row++) {
        int fy = dy + row;
        if (fy < 0 || fy >= (int)fb.height) continue;
        for (int col = 0; col < w; col++) {
            int fx = dx + col;
            if (fx < 0 || fx >= (int)fb.width) continue;
            u32 pixel = src[row * src_stride + col];
            u8 *d = fb_buf + fy * fb.pitch + fx * bpp;
            u32 dst_px = (bpp == 4) ? *(u32 *)d :
                         (bpp == 3) ? (d[0] | (u32)d[1] << 8 | (u32)d[2] << 16) : 0;
            u32 out = blend_pixel(dst_px, pixel);
            if (bpp == 4) *(u32 *)d = out;
            else if (bpp == 3) { d[0] = out; d[1] = out >> 8; d[2] = out >> 16; }
        }
    }
    fb_mark_dirty(dx, dy, w, h);
}

/* Horizontal gradient fill — used for title bars, buttons */
void fb_fill_gradient_h(int x, int y, int w, int h, u32 col_left, u32 col_right) {
    if (!fb_buf || !fb.initialized) return;
    int bpp = fb.bpp / 8;
    for (int col = 0; col < w; col++) {
        u32 r = ((col_left >> 16 & 0xFF) * (w - col) + (col_right >> 16 & 0xFF) * col) / w;
        u32 g = ((col_left >>  8 & 0xFF) * (w - col) + (col_right >>  8 & 0xFF) * col) / w;
        u32 b = ((col_left       & 0xFF) * (w - col) + (col_right       & 0xFF) * col) / w;
        u32 color = (r << 16) | (g << 8) | b;
        int fx = x + col;
        if (fx < 0 || fx >= (int)fb.width) continue;
        for (int row = 0; row < h; row++) {
            int fy = y + row;
            if (fy < 0 || fy >= (int)fb.height) continue;
            u8 *p = fb_buf + fy * fb.pitch + fx * bpp;
            if (bpp == 4) *(u32 *)p = color;
            else if (bpp == 3) { p[0] = color; p[1] = color >> 8; p[2] = color >> 16; }
        }
    }
    fb_mark_dirty(x, y, w, h);
}

/* Vertical gradient fill */
void fb_fill_gradient_v(int x, int y, int w, int h, u32 col_top, u32 col_bottom) {
    if (!fb_buf || !fb.initialized) return;
    int bpp = fb.bpp / 8;
    for (int row = 0; row < h; row++) {
        u32 r = ((col_top >> 16 & 0xFF) * (h - row) + (col_bottom >> 16 & 0xFF) * row) / h;
        u32 g = ((col_top >>  8 & 0xFF) * (h - row) + (col_bottom >>  8 & 0xFF) * row) / h;
        u32 b = ((col_top       & 0xFF) * (h - row) + (col_bottom       & 0xFF) * row) / h;
        u32 color = (r << 16) | (g << 8) | b;
        int fy = y + row;
        if (fy < 0 || fy >= (int)fb.height) continue;
        u8 *p = fb_buf + fy * fb.pitch + x * bpp;
        for (int col = 0; col < w && x + col < (int)fb.width; col++) {
            if (x + col < 0) { p += bpp; continue; }
            if (bpp == 4) *(u32 *)p = color;
            else if (bpp == 3) { p[0] = color; p[1] = color >> 8; p[2] = color >> 16; }
            p += bpp;
        }
    }
    fb_mark_dirty(x, y, w, h);
}

/* Screen-to-screen copy (for window dragging, scrolling) */
void fb_copy_region(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    if (!fb_buf || !fb.initialized) return;
    int bpp = fb.bpp / 8;
    size_t row_bytes = (size_t)w * bpp;

    if (dst_y < src_y) {
        for (int row = 0; row < h; row++) {
            u8 *s = fb_buf + (src_y + row) * fb.pitch + src_x * bpp;
            u8 *d = fb_buf + (dst_y + row) * fb.pitch + dst_x * bpp;
            memmove(d, s, row_bytes);
        }
    } else {
        for (int row = h - 1; row >= 0; row--) {
            u8 *s = fb_buf + (src_y + row) * fb.pitch + src_x * bpp;
            u8 *d = fb_buf + (dst_y + row) * fb.pitch + dst_x * bpp;
            memmove(d, s, row_bytes);
        }
    }
    fb_mark_dirty(dst_x, dst_y, w, h);
}

/* Query info */
int  fb_width(void)  { return fb.initialized ? (int)fb.width  : 0; }
int  fb_height(void) { return fb.initialized ? (int)fb.height : 0; }
int  fb_bpp(void)    { return fb.initialized ? (int)fb.bpp    : 0; }
int  fb_pitch(void)  { return fb.initialized ? (int)fb.pitch  : 0; }
u8  *fb_backbuf(void) { return fb_back; }
u8  *fb_frontbuf(void) { return (u8 *)fb.virt_addr; }

/* ============================================================
 * /dev/fb0 userspace interface
 * ============================================================ */

#include <fs/vfs.h>
#include <kernel/syscall.h>

/* FBIO ioctls (Linux-compatible) */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOPUT_VSCREENINFO  0x4601
#define FBIOGET_FSCREENINFO  0x4602
#define FBIO_GET_PHYSADDR    0x4690  /* picomimi extension: get phys addr */

/* fb_var_screeninfo (simplified) */
typedef struct {
    u32 xres, yres;
    u32 xres_virtual, yres_virtual;
    u32 xoffset, yoffset;
    u32 bits_per_pixel;
    u32 grayscale;
    u32 red_offset, red_length, red_msb_right;
    u32 green_offset, green_length, green_msb_right;
    u32 blue_offset, blue_length, blue_msb_right;
    u32 transp_offset, transp_length, transp_msb_right;
    u32 nonstd, activate, height, width;
    u32 accel_flags, pixclock, left_margin, right_margin;
    u32 upper_margin, lower_margin, hsync_len, vsync_len;
    u32 sync, vmode, rotate, colorspace;
    u32 reserved[4];
} fb_var_t;

/* fb_fix_screeninfo (simplified) */
typedef struct {
    char id[16];
    u64  smem_start;
    u32  smem_len;
    u32  type;
    u32  type_aux;
    u32  visual;
    u16  xpanstep, ypanstep, ywrapstep;
    u32  line_length;
    u64  mmio_start;
    u32  mmio_len;
    u32  accel;
    u16  capabilities;
    u16  reserved[2];
} fb_fix_t;

static s64 fb0_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    (void)f;
    if (!fb.initialized) return -ENODEV;
    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        fb_var_t v = {0};
        v.xres = v.xres_virtual = (u32)fb.width;
        v.yres = v.yres_virtual = (u32)fb.height;
        v.bits_per_pixel = (u32)fb.bpp;
        v.red_offset   = (u32)fb.red_pos;   v.red_length   = (u32)(u32)fb.red_mask_size;
        v.green_offset = (u32)fb.green_pos; v.green_length = (u32)(u32)fb.green_mask_size;
        v.blue_offset  = (u32)fb.blue_pos;  v.blue_length  = (u32)(u32)fb.blue_mask_size;
        memcpy((void *)arg, &v, sizeof(v));
        return 0;
    }
    case FBIOPUT_VSCREENINFO:
        return 0; /* accept but ignore */
    case FBIOGET_FSCREENINFO: {
        fb_fix_t fix = {0};
        memcpy(fix.id, "picomimi-fb0", 12);
        fix.smem_start  = (u64)fb.phys_addr;
        fix.smem_len    = (u32)(fb.pitch * fb.height);
        fix.type        = 0; /* FB_TYPE_PACKED_PIXELS */
        fix.visual      = 2; /* FB_VISUAL_TRUECOLOR */
        fix.line_length = (u32)fb.pitch;
        memcpy((void *)arg, &fix, sizeof(fix));
        return 0;
    }
    case FBIO_GET_PHYSADDR:
        *(u64 *)arg = (u64)fb.phys_addr;
        return 0;
    default:
        return -ENOTTY;
    }
}

static s64 fb0_read(struct file *f, char *buf, size_t len, u64 *off) {
    (void)f;
    if (!fb.initialized || !fb.virt_addr) return -ENODEV;
    u64 fb_size = (u64)fb.pitch * fb.height;
    if (*off >= fb_size) return 0;
    size_t avail = (size_t)(fb_size - *off);
    if (len > avail) len = avail;
    memcpy(buf, (u8 *)fb.virt_addr + *off, len);
    *off += len;
    return (s64)len;
}

static s64 fb0_write(struct file *f, const char *buf, size_t len, u64 *off) {
    (void)f;
    if (!fb.initialized || !fb.virt_addr) return -ENODEV;
    u64 fb_size = (u64)fb.pitch * fb.height;
    if (*off >= fb_size) return -ENOSPC;
    size_t avail = (size_t)(fb_size - *off);
    if (len > avail) len = avail;
    memcpy((u8 *)fb.virt_addr + *off, buf, len);
    *off += len;
    return (s64)len;
}

const file_operations_t fb_fops = {
    .read   = fb0_read,
    .write  = fb0_write,
    .ioctl  = fb0_ioctl,
};
