/**
 * Picomimi-x64 CPU Header
 */
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <kernel/types.h>

// ============================================================================
// MSR DEFINITIONS
// ============================================================================

#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083
#define MSR_SFMASK          0xC0000084
#define MSR_FS_BASE         0xC0000100
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

#define MSR_APIC_BASE       0x0000001B
#define MSR_TSC             0x00000010
#define MSR_TSC_DEADLINE    0x000006E0

// EFER bits
#define EFER_SCE    (1 << 0)    // Syscall Enable
#define EFER_LME    (1 << 8)    // Long Mode Enable
#define EFER_LMA    (1 << 10)   // Long Mode Active
#define EFER_NXE    (1 << 11)   // No-Execute Enable

// CR0 bits
#define CR0_PE      (1 << 0)    // Protection Enable
#define CR0_MP      (1 << 1)    // Monitor Coprocessor
#define CR0_EM      (1 << 2)    // Emulation
#define CR0_TS      (1 << 3)    // Task Switched
#define CR0_ET      (1 << 4)    // Extension Type
#define CR0_NE      (1 << 5)    // Numeric Error
#define CR0_WP      (1 << 16)   // Write Protect
#define CR0_AM      (1 << 18)   // Alignment Mask
#define CR0_NW      (1 << 29)   // Not Write-through
#define CR0_CD      (1 << 30)   // Cache Disable
#define CR0_PG      (1 << 31)   // Paging

// CR4 bits
#define CR4_VME     (1 << 0)    // Virtual 8086 Mode Extensions
#define CR4_PVI     (1 << 1)    // Protected Mode Virtual Interrupts
#define CR4_TSD     (1 << 2)    // Time Stamp Disable
#define CR4_DE      (1 << 3)    // Debugging Extensions
#define CR4_PSE     (1 << 4)    // Page Size Extension
#define CR4_PAE     (1 << 5)    // Physical Address Extension
#define CR4_MCE     (1 << 6)    // Machine Check Exception
#define CR4_PGE     (1 << 7)    // Page Global Enable
#define CR4_PCE     (1 << 8)    // Performance Counter Enable
#define CR4_OSFXSR  (1 << 9)    // FXSAVE/FXRSTOR Support
#define CR4_OSXMMEXCPT (1 << 10) // SIMD Floating Point Exceptions
#define CR4_UMIP    (1 << 11)   // User-Mode Instruction Prevention
#define CR4_VMXE    (1 << 13)   // VMX Enable
#define CR4_SMXE    (1 << 14)   // SMX Enable
#define CR4_FSGSBASE (1 << 16)  // FSGSBASE Enable
#define CR4_PCIDE   (1 << 17)   // PCID Enable
#define CR4_OSXSAVE (1 << 18)   // XSAVE Enable
#define CR4_SMEP    (1 << 20)   // Supervisor Mode Execution Prevention
#define CR4_SMAP    (1 << 21)   // Supervisor Mode Access Prevention

// ============================================================================
// CPU FUNCTIONS
// ============================================================================

static inline void wrmsr(u32 msr, u64 value) {
    u32 low = value & 0xFFFFFFFF;
    u32 high = value >> 32;
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high) : "memory");
}

static inline u64 rdmsr(u32 msr) {
    u32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((u64)high << 32) | low;
}

static inline u64 read_cr0(void) {
    u64 val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(u64 val) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(val) : "memory");
}

static inline u64 read_cr2(void) {
    u64 val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline u64 read_cr3(void) {
    u64 val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(u64 val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

static inline u64 read_cr4(void) {
    u64 val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(u64 val) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(val) : "memory");
}

static inline u64 rdtsc(void) {
    u32 low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

static inline void cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline void cpuid_count(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

static inline void invlpg(void *addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

// Defined in process.c or smp.c
u32 smp_processor_id(void);

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

void cpu_init(void);

#endif // _ARCH_CPU_H
