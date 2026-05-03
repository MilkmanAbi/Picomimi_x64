#pragma once
#include <kernel/types.h>

void fbcon_init(void);
void fbcon_putchar(char ch);
void fbcon_write(const char *s, size_t n);
void fbcon_puts(const char *s);
void fbcon_set_color(int fg, int bg);
void fbcon_clear(void);
