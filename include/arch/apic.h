/**
 * Picomimi-x64 APIC Header
 */
#ifndef _ARCH_APIC_H
#define _ARCH_APIC_H

#include <kernel/types.h>

// Local APIC registers (offsets from APIC base)
#define LAPIC_ID            0x020   // Local APIC ID
#define LAPIC_VERSION       0x030   // Local APIC Version
#define LAPIC_TPR           0x080   // Task Priority Register
#define LAPIC_APR           0x090   // Arbitration Priority Register
#define LAPIC_PPR           0x0A0   // Processor Priority Register
#define LAPIC_EOI           0x0B0   // End of Interrupt
#define LAPIC_RRD           0x0C0   // Remote Read Register
#define LAPIC_LDR           0x0D0   // Logical Destination Register
#define LAPIC_DFR           0x0E0   // Destination Format Register
#define LAPIC_SVR           0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_ISR           0x100   // In-Service Register (8 x 32-bit)
#define LAPIC_TMR           0x180   // Trigger Mode Register (8 x 32-bit)
#define LAPIC_IRR           0x200   // Interrupt Request Register (8 x 32-bit)
#define LAPIC_ESR           0x280   // Error Status Register
#define LAPIC_ICR_LO        0x300   // Interrupt Command Register (low)
#define LAPIC_ICR_HI        0x310   // Interrupt Command Register (high)
#define LAPIC_TIMER_LVT     0x320   // Timer Local Vector Table
#define LAPIC_THERMAL_LVT   0x330   // Thermal Local Vector Table
#define LAPIC_PERF_LVT      0x340   // Performance Counter LVT
#define LAPIC_LINT0_LVT     0x350   // Local Interrupt 0 LVT
#define LAPIC_LINT1_LVT     0x360   // Local Interrupt 1 LVT
#define LAPIC_ERROR_LVT     0x370   // Error LVT
#define LAPIC_TIMER_ICR     0x380   // Timer Initial Count Register
#define LAPIC_TIMER_CCR     0x390   // Timer Current Count Register
#define LAPIC_TIMER_DCR     0x3E0   // Timer Divide Configuration Register

// SVR bits
#define SVR_ENABLE          (1 << 8)
#define SVR_FOCUS           (1 << 9)

// LVT bits
#define LVT_MASKED          (1 << 16)
#define LVT_TIMER_PERIODIC  (1 << 17)
#define LVT_TIMER_ONESHOT   0

// ICR delivery modes
#define ICR_FIXED           (0 << 8)
#define ICR_SMI             (2 << 8)
#define ICR_NMI             (4 << 8)
#define ICR_INIT            (5 << 8)
#define ICR_STARTUP         (6 << 8)

// ICR destination modes
#define ICR_PHYSICAL        (0 << 11)
#define ICR_LOGICAL         (1 << 11)

// ICR delivery status
#define ICR_IDLE            (0 << 12)
#define ICR_PENDING         (1 << 12)

// ICR level
#define ICR_DEASSERT        (0 << 14)
#define ICR_ASSERT          (1 << 14)

// ICR trigger mode
#define ICR_EDGE            (0 << 15)
#define ICR_LEVEL           (1 << 15)

// ICR destination shorthand
#define ICR_NO_SHORTHAND    (0 << 18)
#define ICR_SELF            (1 << 18)
#define ICR_ALL_INCLUDING   (2 << 18)
#define ICR_ALL_EXCLUDING   (3 << 18)

// I/O APIC registers
#define IOAPIC_REGSEL       0x00    // Register Select
#define IOAPIC_REGWIN       0x10    // Register Window

// I/O APIC indirect registers
#define IOAPIC_ID           0x00
#define IOAPIC_VERSION      0x01
#define IOAPIC_ARB          0x02
#define IOAPIC_REDTBL_BASE  0x10

// APIC timer vectors
#define APIC_TIMER_VECTOR   32
#define APIC_SPURIOUS_VECTOR 0xFF

void lapic_init(void);
void lapic_eoi(void);
u32 lapic_id(void);
void lapic_write(u32 reg, u32 value);
u32 lapic_read(u32 reg);
void lapic_send_ipi(u32 apic_id, u32 vector);
void lapic_send_init(u32 apic_id);
void lapic_send_sipi(u32 apic_id, u8 vector);
void lapic_timer_init(u32 vector, u32 divide);
void lapic_timer_stop(void);

void ioapic_init(void);
void ioapic_write(u32 reg, u32 value);
u32 ioapic_read(u32 reg);
void ioapic_enable_irq(u8 irq, u8 vector, u32 apic_id);
void ioapic_disable_irq(u8 irq);

void smp_init(void);
void ap_main(void);

#endif // _ARCH_APIC_H
