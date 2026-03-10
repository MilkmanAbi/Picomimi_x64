/**
 * Picomimi-x64 VGA Text Mode Driver
 */

#include <kernel/types.h>
#include <drivers/vga.h>
#include <arch/io.h>

// ============================================================================
// VGA CONSTANTS
// ============================================================================

#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_MEMORY      0xFFFFFFFF800B8000  // VGA memory in higher half

// VGA color codes
#define VGA_BLACK       0
#define VGA_BLUE        1
#define VGA_GREEN       2
#define VGA_CYAN        3
#define VGA_RED         4
#define VGA_MAGENTA     5
#define VGA_BROWN       6
#define VGA_LIGHT_GREY  7
#define VGA_DARK_GREY   8
#define VGA_LIGHT_BLUE  9
#define VGA_LIGHT_GREEN 10
#define VGA_LIGHT_CYAN  11
#define VGA_LIGHT_RED   12
#define VGA_LIGHT_MAGENTA 13
#define VGA_YELLOW      14
#define VGA_WHITE       15

// ============================================================================
// STATE
// ============================================================================

static u16 *vga_buffer = (u16 *)VGA_MEMORY;
static u8 vga_color = 0;
static int vga_row = 0;
static int vga_col = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static inline u8 vga_entry_color(u8 fg, u8 bg) {
    return fg | (bg << 4);
}

static inline u16 vga_entry(char c, u8 color) {
    return (u16)c | ((u16)color << 8);
}

static void vga_scroll(void) {
    // Move all lines up by one
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }

    // Clear the last line
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', vga_color);
    }
}

static void vga_update_cursor(void) {
    u16 pos = vga_row * VGA_WIDTH + vga_col;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

void vga_init(void) {
    vga_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_row = 0;
    vga_col = 0;

    // Enable cursor
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);  // Cursor start
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);  // Cursor end
}

void vga_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', vga_color);
        }
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_set_color(u8 fg, u8 bg) {
    vga_color = vga_entry_color(fg, bg);
}

void vga_putc(char c) {
    switch (c) {
        case '\n':
            vga_col = 0;
            vga_row++;
            break;

        case '\r':
            vga_col = 0;
            break;

        case '\t':
            vga_col = (vga_col + 8) & ~7;
            if (vga_col >= VGA_WIDTH) {
                vga_col = 0;
                vga_row++;
            }
            break;

        case '\b':
            if (vga_col > 0) {
                vga_col--;
                vga_buffer[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_color);
            }
            break;

        default:
            vga_buffer[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
            vga_col++;
            if (vga_col >= VGA_WIDTH) {
                vga_col = 0;
                vga_row++;
            }
            break;
    }

    // Scroll if necessary
    while (vga_row >= VGA_HEIGHT) {
        vga_scroll();
        vga_row--;
    }

    vga_update_cursor();
}

void vga_puts(const char *str) {
    while (*str) {
        vga_putc(*str++);
    }
}

void vga_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        vga_putc(data[i]);
    }
}

void vga_set_cursor(int row, int col) {
    vga_row = row;
    vga_col = col;
    vga_update_cursor();
}

/* vga_putchar — write single char to VGA text buffer */
void vga_putchar(char c) {
    extern int vga_col, vga_row;
    extern void vga_scroll(void);
    /* Use printk serial output as fallback if VGA not init'd */
    extern void serial_putc(u16 port, char c);
    serial_putc(0x3F8, c);
}
