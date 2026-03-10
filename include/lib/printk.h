/**
 * Picomimi-x64 Kernel Print Header
 * 
 * Linux-compatible printk interface
 */
#ifndef _LIB_PRINTK_H
#define _LIB_PRINTK_H

#include <kernel/types.h>

// ============================================================================
// STDARG (since we don't have libc)
// ============================================================================

typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_copy(dest, src)  __builtin_va_copy(dest, src)

// ============================================================================
// LOG LEVELS (Linux-compatible)
// ============================================================================

#define KERN_EMERG      "<0>"   // System is unusable
#define KERN_ALERT      "<1>"   // Action must be taken immediately
#define KERN_CRIT       "<2>"   // Critical conditions
#define KERN_ERR        "<3>"   // Error conditions
#define KERN_WARNING    "<4>"   // Warning conditions
#define KERN_NOTICE     "<5>"   // Normal but significant
#define KERN_INFO       "<6>"   // Informational
#define KERN_DEBUG      "<7>"   // Debug-level messages

#define KERN_EMERG_LEVEL    0
#define KERN_ALERT_LEVEL    1
#define KERN_CRIT_LEVEL     2
#define KERN_ERR_LEVEL      3
#define KERN_WARNING_LEVEL  4
#define KERN_NOTICE_LEVEL   5
#define KERN_INFO_LEVEL     6
#define KERN_DEBUG_LEVEL    7

#define KERN_DEFAULT_LEVEL  KERN_INFO_LEVEL

// ============================================================================
// FORMAT FLAGS
// ============================================================================

#define FLAG_ZEROPAD    (1 << 0)
#define FLAG_SIGNED     (1 << 1)
#define FLAG_PLUS       (1 << 2)
#define FLAG_SPACE      (1 << 3)
#define FLAG_LEFT       (1 << 4)
#define FLAG_PREFIX     (1 << 5)
#define FLAG_UPPERCASE  (1 << 6)

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintk(const char *fmt, va_list args);

int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

void early_printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ============================================================================
// CONVENIENCE MACROS
// ============================================================================

#define pr_emerg(fmt, ...)      printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)      printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)       printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)        printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)       printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)     printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)       printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)      printk(KERN_DEBUG fmt, ##__VA_ARGS__)

/* Optional hook: called with every formatted printk string.
 * Used by WM terminal to capture shell output. */
void printk_set_hook(void (*fn)(const char *s));

#endif // _LIB_PRINTK_H
