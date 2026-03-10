/**
 * Picomimi-x64 VGA Driver Header
 */
#ifndef _DRIVERS_VGA_H
#define _DRIVERS_VGA_H

#include <kernel/types.h>

void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);
void vga_putc(char c);
void vga_puts(const char *str);
void vga_write(const char *data, size_t len);
void vga_set_cursor(int row, int col);

#endif // _DRIVERS_VGA_H
