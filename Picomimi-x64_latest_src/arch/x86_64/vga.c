/**
 * VGA Text Mode Console
 */
#include <kernel/types.h>

#define VGA_BASE    ((volatile u16 *)0xFFFFFFFF800B8000ULL)
#define VGA_COLS    80
#define VGA_ROWS    25

static int vga_col = 0;
static int vga_row = 0;

void vga_putchar(char c) {
    volatile u16 *fb = VGA_BASE;
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) vga_col--;
    } else {
        fb[vga_row * VGA_COLS + vga_col] = (u16)(0x0700 | (u8)c);
        vga_col++;
        if (vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
    }
    if (vga_row >= VGA_ROWS) {
        /* Scroll up */
        for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
            fb[i] = fb[i + VGA_COLS];
        for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
            fb[i] = 0x0720;
        vga_row = VGA_ROWS - 1;
    }
}

void vga_clear(void) {
    volatile u16 *fb = VGA_BASE;
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
        fb[i] = 0x0720;
    vga_col = 0;
    vga_row = 0;
}
