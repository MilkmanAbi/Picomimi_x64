/**
 * Picomimi-x64 Serial Driver Header
 */
#ifndef _DRIVERS_SERIAL_H
#define _DRIVERS_SERIAL_H

#include <kernel/types.h>

// Standard PC serial port addresses
#define SERIAL_COM1     0x3F8
#define SERIAL_COM2     0x2F8
#define SERIAL_COM3     0x3E8
#define SERIAL_COM4     0x2E8

// Functions
int serial_init(u16 base_port);
void serial_putc(u16 port, char c);
void serial_puts(u16 port, const char *str);
void serial_write(u16 port, const void *buf, size_t len);
int serial_getc(u16 port);
int serial_getc_blocking(u16 port);
void serial_printf(u16 port, const char *fmt, ...);

#endif // _DRIVERS_SERIAL_H
