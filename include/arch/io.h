/**
 * Picomimi-x64 x86 I/O Port Access
 */
#ifndef _ARCH_IO_H
#define _ARCH_IO_H

#include <kernel/types.h>

// ============================================================================
// PORT I/O - BYTE
// ============================================================================

static __always_inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static __always_inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// PORT I/O - WORD
// ============================================================================

static __always_inline void outw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static __always_inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// PORT I/O - DWORD
// ============================================================================

static __always_inline void outl(u16 port, u32 val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static __always_inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// STRING I/O
// ============================================================================

static __always_inline void outsb(u16 port, const void *addr, u64 count) {
    __asm__ volatile("rep outsb" : "+S"(addr), "+c"(count) : "d"(port));
}

static __always_inline void insb(u16 port, void *addr, u64 count) {
    __asm__ volatile("rep insb" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static __always_inline void outsw(u16 port, const void *addr, u64 count) {
    __asm__ volatile("rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
}

static __always_inline void insw(u16 port, void *addr, u64 count) {
    __asm__ volatile("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static __always_inline void outsl(u16 port, const void *addr, u64 count) {
    __asm__ volatile("rep outsl" : "+S"(addr), "+c"(count) : "d"(port));
}

static __always_inline void insl(u16 port, void *addr, u64 count) {
    __asm__ volatile("rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

// ============================================================================
// I/O DELAY
// ============================================================================

static __always_inline void io_delay(void) {
    // I/O to unused port causes ~1us delay
    outb(0x80, 0);
}

#endif // _ARCH_IO_H
