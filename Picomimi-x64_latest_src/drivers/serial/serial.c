/**
 * Picomimi-x64 Serial Driver
 * 
 * 8250/16550 UART driver for early kernel debugging
 */

#include <kernel/types.h>
#include <drivers/serial.h>
#include <arch/io.h>

// ============================================================================
// SERIAL PORT REGISTERS
// ============================================================================

// Register offsets (when DLAB = 0)
#define SERIAL_DATA         0   // Data register (R/W)
#define SERIAL_IER          1   // Interrupt Enable Register
#define SERIAL_IIR          2   // Interrupt Identification Register (R)
#define SERIAL_FCR          2   // FIFO Control Register (W)
#define SERIAL_LCR          3   // Line Control Register
#define SERIAL_MCR          4   // Modem Control Register
#define SERIAL_LSR          5   // Line Status Register
#define SERIAL_MSR          6   // Modem Status Register
#define SERIAL_SR           7   // Scratch Register

// Register offsets (when DLAB = 1)
#define SERIAL_DLL          0   // Divisor Latch Low
#define SERIAL_DLH          1   // Divisor Latch High

// Line Control Register bits
#define LCR_DLAB            0x80    // Divisor Latch Access Bit
#define LCR_BREAK           0x40    // Break Enable
#define LCR_PARITY_MASK     0x38    // Parity bits
#define LCR_PARITY_NONE     0x00
#define LCR_PARITY_ODD      0x08
#define LCR_PARITY_EVEN     0x18
#define LCR_STOP_2          0x04    // 2 stop bits (1 if clear)
#define LCR_WORD_MASK       0x03    // Word length bits
#define LCR_WORD_5          0x00
#define LCR_WORD_6          0x01
#define LCR_WORD_7          0x02
#define LCR_WORD_8          0x03

// FIFO Control Register bits
#define FCR_ENABLE          0x01    // Enable FIFOs
#define FCR_CLEAR_RX        0x02    // Clear receive FIFO
#define FCR_CLEAR_TX        0x04    // Clear transmit FIFO
#define FCR_DMA_MODE        0x08    // DMA mode select
#define FCR_TRIGGER_1       0x00    // Trigger level 1 byte
#define FCR_TRIGGER_4       0x40    // Trigger level 4 bytes
#define FCR_TRIGGER_8       0x80    // Trigger level 8 bytes
#define FCR_TRIGGER_14      0xC0    // Trigger level 14 bytes

// Line Status Register bits
#define LSR_DR              0x01    // Data Ready
#define LSR_OE              0x02    // Overrun Error
#define LSR_PE              0x04    // Parity Error
#define LSR_FE              0x08    // Framing Error
#define LSR_BI              0x10    // Break Interrupt
#define LSR_THRE            0x20    // Transmitter Holding Register Empty
#define LSR_TEMT            0x40    // Transmitter Empty
#define LSR_FIFO_ERR        0x80    // Error in FIFO

// Modem Control Register bits
#define MCR_DTR             0x01    // Data Terminal Ready
#define MCR_RTS             0x02    // Request To Send
#define MCR_OUT1            0x04    // Out1
#define MCR_OUT2            0x08    // Out2 (enables IRQ)
#define MCR_LOOP            0x10    // Loopback mode

// Interrupt Enable Register bits
#define IER_RX              0x01    // Receive data available
#define IER_TX              0x02    // Transmitter empty
#define IER_LSR             0x04    // Line status change
#define IER_MSR             0x08    // Modem status change

// ============================================================================
// GLOBAL STATE
// ============================================================================

static u16 serial_ports[4] = { 0 };
static bool serial_initialized[4] = { false };

// ============================================================================
// LOW-LEVEL I/O
// ============================================================================

static inline void serial_outb(u16 port, u8 reg, u8 val) {
    outb(port + reg, val);
}

static inline u8 serial_inb(u16 port, u8 reg) {
    return inb(port + reg);
}

// ============================================================================
// SERIAL PORT INITIALIZATION
// ============================================================================

int serial_init(u16 base_port) {
    int port_idx;
    
    // Determine port index
    switch (base_port) {
        case SERIAL_COM1: port_idx = 0; break;
        case SERIAL_COM2: port_idx = 1; break;
        case SERIAL_COM3: port_idx = 2; break;
        case SERIAL_COM4: port_idx = 3; break;
        default: return -1;
    }

    serial_ports[port_idx] = base_port;

    // Disable all interrupts
    serial_outb(base_port, SERIAL_IER, 0x00);

    // Enable DLAB to set baud rate
    serial_outb(base_port, SERIAL_LCR, LCR_DLAB);

    // Set baud rate to 115200 (divisor = 1)
    serial_outb(base_port, SERIAL_DLL, 0x01);   // Low byte
    serial_outb(base_port, SERIAL_DLH, 0x00);   // High byte

    // 8 bits, no parity, 1 stop bit (8N1)
    serial_outb(base_port, SERIAL_LCR, LCR_WORD_8 | LCR_PARITY_NONE);

    // Enable and clear FIFOs, 14-byte trigger
    serial_outb(base_port, SERIAL_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_14);

    // Enable DTR, RTS, and OUT2 (for interrupts)
    serial_outb(base_port, SERIAL_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

    serial_initialized[port_idx] = true;
    return 0;
}

// ============================================================================
// SERIAL OUTPUT
// ============================================================================

static bool serial_is_transmit_empty(u16 port) {
    return (serial_inb(port, SERIAL_LSR) & LSR_THRE) != 0;
}

void serial_putc(u16 port, char c) {
    // Wait for transmit buffer to be empty
    while (!serial_is_transmit_empty(port)) {
        cpu_relax();
    }
    
    serial_outb(port, SERIAL_DATA, c);
}

void serial_puts(u16 port, const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_putc(port, '\r');
        }
        serial_putc(port, *str++);
    }
}

void serial_write(u16 port, const void *buf, size_t len) {
    const char *p = buf;
    while (len--) {
        if (*p == '\n') {
            serial_putc(port, '\r');
        }
        serial_putc(port, *p++);
    }
}

// ============================================================================
// SERIAL INPUT
// ============================================================================

static bool serial_received(u16 port) {
    return (serial_inb(port, SERIAL_LSR) & LSR_DR) != 0;
}

int serial_getc(u16 port) {
    if (!serial_received(port)) {
        return -1;
    }
    return serial_inb(port, SERIAL_DATA);
}

int serial_getc_blocking(u16 port) {
    extern void schedule(void);
    while (!serial_received(port)) {
        schedule();
    }
    return serial_inb(port, SERIAL_DATA);
}

// ============================================================================
// CONVENIENCE FUNCTIONS
// ============================================================================

void serial_printf(u16 port, const char *fmt, ...) {
    // Simple implementation - just use printk for now
    // TODO: Implement proper varargs formatting
    serial_puts(port, fmt);
}
