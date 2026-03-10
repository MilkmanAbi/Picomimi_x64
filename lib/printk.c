/**
 * Picomimi-x64 Kernel Print (printk)
 * 
 * Linux-compatible printk implementation
 */

#include <kernel/types.h>
#include <lib/printk.h>
#include <drivers/serial.h>
#include <drivers/vga.h>

// ============================================================================
// LOG LEVEL STRINGS
// ============================================================================

static const char *log_level_str[] __used = {
    [0] = "EMERG",
    [1] = "ALERT",
    [2] = "CRIT",
    [3] = "ERR",
    [4] = "WARNING",
    [5] = "NOTICE",
    [6] = "INFO",
    [7] = "DEBUG",
};

// Current console log level
static int console_loglevel = KERN_INFO_LEVEL;

// ============================================================================
// NUMBER TO STRING CONVERSION
// ============================================================================

static char *number_to_str(char *buf, u64 num, int base, int width, int precision, int flags) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;
    
    char tmp[66];
    int i = 0;
    bool negative = false;

    // Handle zero
    if (num == 0) {
        tmp[i++] = '0';
    } else {
        // Handle signed numbers
        if ((flags & FLAG_SIGNED) && (s64)num < 0) {
            negative = true;
            num = -(s64)num;
        }

        // Convert to string (reversed)
        while (num != 0) {
            tmp[i++] = digits[num % base];
            num /= base;
        }
    }

    // Pad with zeros
    while (i < precision) {
        tmp[i++] = '0';
    }

    // Add prefix
    if (flags & FLAG_PREFIX) {
        if (base == 16) {
            tmp[i++] = (flags & FLAG_UPPERCASE) ? 'X' : 'x';
            tmp[i++] = '0';
        } else if (base == 8 && tmp[i-1] != '0') {
            tmp[i++] = '0';
        }
    }

    // Add sign
    if (negative) {
        tmp[i++] = '-';
    } else if (flags & FLAG_PLUS) {
        tmp[i++] = '+';
    } else if (flags & FLAG_SPACE) {
        tmp[i++] = ' ';
    }

    // Pad with spaces (if right-aligned)
    if (!(flags & FLAG_LEFT)) {
        char pad = (flags & FLAG_ZEROPAD) ? '0' : ' ';
        while (i < width) {
            tmp[i++] = pad;
        }
    }

    // Copy reversed string to buffer
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }

    // Pad with spaces (if left-aligned)
    if (flags & FLAG_LEFT) {
        while (j < width) {
            buf[j++] = ' ';
        }
    }

    buf[j] = '\0';
    return buf + j;
}

// ============================================================================
// VSNPRINTF IMPLEMENTATION
// ============================================================================

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    char *str = buf;
    char *end = buf + size - 1;
    
    if (size == 0) return 0;

    for (; *fmt && str < end; fmt++) {
        if (*fmt != '%') {
            *str++ = *fmt;
            continue;
        }

        fmt++; // Skip '%'

        // Parse flags
        int flags = 0;
        while (1) {
            switch (*fmt) {
                case '-': flags |= FLAG_LEFT; fmt++; continue;
                case '+': flags |= FLAG_PLUS; fmt++; continue;
                case ' ': flags |= FLAG_SPACE; fmt++; continue;
                case '#': flags |= FLAG_PREFIX; fmt++; continue;
                case '0': flags |= FLAG_ZEROPAD; fmt++; continue;
            }
            break;
        }

        // Parse width
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                width = -width;
                flags |= FLAG_LEFT;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        // Parse precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                fmt++;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        // Parse length modifier
        int length = 0;
        switch (*fmt) {
            case 'h':
                length = 1;
                fmt++;
                if (*fmt == 'h') { length = 2; fmt++; }
                break;
            case 'l':
                length = 3;
                fmt++;
                if (*fmt == 'l') { length = 4; fmt++; }
                break;
            case 'z':
            case 't':
                length = 3;
                fmt++;
                break;
        }

        // Process conversion specifier
        char tmp[68];
        const char *s;
        int len;

        switch (*fmt) {
            case 'c': {
                char c = (char)va_arg(args, int);
                if (!(flags & FLAG_LEFT)) {
                    while (--width > 0 && str < end) *str++ = ' ';
                }
                if (str < end) *str++ = c;
                while (--width > 0 && str < end) *str++ = ' ';
                break;
            }

            case 's': {
                s = va_arg(args, const char *);
                if (!s) s = "(null)";
                len = 0;
                while (s[len] && (precision < 0 || len < precision)) len++;
                if (!(flags & FLAG_LEFT)) {
                    while (len < width-- && str < end) *str++ = ' ';
                }
                for (int i = 0; i < len && str < end; i++) *str++ = s[i];
                while (len < width-- && str < end) *str++ = ' ';
                break;
            }

            case 'p': {
                u64 ptr = (u64)va_arg(args, void *);
                flags |= FLAG_PREFIX;
                number_to_str(tmp, ptr, 16, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case 'd':
            case 'i': {
                s64 num;
                if (length >= 3) num = va_arg(args, s64);
                else num = va_arg(args, s32);
                flags |= FLAG_SIGNED;
                number_to_str(tmp, num, 10, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case 'u': {
                u64 num;
                if (length >= 3) num = va_arg(args, u64);
                else num = va_arg(args, u32);
                number_to_str(tmp, num, 10, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case 'x': {
                u64 num;
                if (length >= 3) num = va_arg(args, u64);
                else num = va_arg(args, u32);
                number_to_str(tmp, num, 16, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case 'X': {
                u64 num;
                if (length >= 3) num = va_arg(args, u64);
                else num = va_arg(args, u32);
                flags |= FLAG_UPPERCASE;
                number_to_str(tmp, num, 16, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case 'o': {
                u64 num;
                if (length >= 3) num = va_arg(args, u64);
                else num = va_arg(args, u32);
                number_to_str(tmp, num, 8, width, precision, flags);
                for (s = tmp; *s && str < end; ) *str++ = *s++;
                break;
            }

            case '%':
                if (str < end) *str++ = '%';
                break;

            default:
                if (str < end) *str++ = '%';
                if (*fmt && str < end) *str++ = *fmt;
                break;
        }
    }

    *str = '\0';
    return str - buf;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

// ============================================================================
// PRINTK IMPLEMENTATION
// ============================================================================

static spinlock_t printk_lock __used = { .raw_lock = { 0 } };

// Output hook for WM terminal capture
static void (*printk_hook_fn)(const char *s) = NULL;

void printk_set_hook(void (*fn)(const char *s)) {
    printk_hook_fn = fn;
}

int vprintk(const char *fmt, va_list args) {
    static char buf[1024];
    int level = KERN_DEFAULT_LEVEL;

    // Parse log level
    if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
        level = fmt[1] - '0';
        fmt += 3;
    }

    // Format the message
    int len = vsnprintf(buf, sizeof(buf), fmt, args);

    // Output to serial (COM1) always
    serial_write(SERIAL_COM1, buf, len);

    // Output to WM terminal hook if registered
    if (printk_hook_fn) {
        printk_hook_fn(buf);
    }

    // Output to simple VGA console
    if (level <= console_loglevel) {
        vga_puts(buf);
    }

    return len;
}

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vprintk(fmt, args);
    va_end(args);
    return ret;
}

// ============================================================================
// EARLY PRINTK (before console is fully initialized)
// ============================================================================

void early_printk(const char *fmt, ...) {
    static char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    serial_write(SERIAL_COM1, buf, len);
}
