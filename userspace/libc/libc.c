/**
 * Picomimi-x64 Minimal libc
 *
 * Provides the bare-minimum C library functions needed for /bin/sh and
 * basic userspace tools. No dynamic linking — everything is statically
 * linked into each binary.
 *
 * Sections:
 *   - string functions (strlen, strcpy, strcmp, ...)
 *   - memory (memcpy, memset, memmove)
 *   - stdio (write-only: printf, puts, putchar via sys_write)
 *   - process (exit, _exit, fork, exec)
 *   - I/O (read, write, open, close, getchar, readline)
 *   - heap (sbrk / malloc via sys_brk)
 */

#include "../include/syscall.h"


/* ------------------------------------------------------------------ */
/* Types                                                                 */
/* ------------------------------------------------------------------ */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int  uint32_t;
typedef unsigned long uint64_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
#define NULL ((void*)0)
#define EOF  (-1)

/* ------------------------------------------------------------------ */
/* String                                                                */
/* ------------------------------------------------------------------ */

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++));
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n-- && (*d++ = *s++));
    while (n-- && (*d++ = 0));   /* NUL-pad */
    return r;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++));
    return r;
}

char *strncat(char *d, const char *s, size_t n) {
    char *r = d;
    while (*d) d++;
    while (n-- && (*d++ = *s++));
    *d = 0;
    return r;
}

char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (char *)last;
}

char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    size_t nlen = strlen(n);
    while (*h) {
        if (!strncmp(h, n, nlen)) return (char *)h;
        h++;
    }
    return NULL;
}

char *strdup(const char *s);  /* forward */

/* ------------------------------------------------------------------ */
/* Memory                                                                */
/* ------------------------------------------------------------------ */

void *memcpy(void *d, const void *s, size_t n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    char *dd = d; const char *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else         { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

void *memset(void *s, int c, size_t n) {
    char *p = s;
    while (n--) *p++ = (char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = a, *bb = b;
    while (n--) { if (*aa != *bb) return *aa - *bb; aa++; bb++; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Heap (bump allocator using sys_brk)                                   */
/* ------------------------------------------------------------------ */

static char *heap_end = NULL;

static char *sbrk(long inc) {
    if (!heap_end) {
        heap_end = (char *)sys_brk(0);   /* Get current brk */
        if ((long)heap_end < 0) return (char*)-1;
    }
    char *old = heap_end;
    char *new_end = heap_end + inc;
    long result = sys_brk((long)new_end);
    if (result < 0) return (char*)-1;
    heap_end = new_end;
    return old;
}

/* Simple free-list allocator */
typedef struct block { size_t size; struct block *next; int free; } block_t;
static block_t *heap_head = NULL;
#define BLOCK_HDR sizeof(block_t)

void *malloc(size_t size) {
    size = (size + 7) & ~7UL;   /* 8-byte align */
    block_t *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            b->free = 0;
            return (char *)b + BLOCK_HDR;
        }
        b = b->next;
    }
    block_t *nb = (block_t *)sbrk((long)(size + BLOCK_HDR));
    if (!nb || nb == (block_t*)-1) return NULL;
    nb->size = size;
    nb->free = 0;
    nb->next = NULL;
    if (!heap_head) { heap_head = nb; }
    else { block_t *p = heap_head; while (p->next) p = p->next; p->next = nb; }
    return (char *)nb + BLOCK_HDR;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((char *)ptr - BLOCK_HDR);
    b->free = 1;
}

void *calloc(size_t n, size_t sz) {
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    block_t *b = (block_t *)((char *)ptr - BLOCK_HDR);
    if (b->size >= size) return ptr;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                   */
/* ------------------------------------------------------------------ */

int putchar(int c) {
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    sys_write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

int write(int fd, const void *buf, size_t n)  { return (int)sys_write(fd, buf, n); }
int read(int fd, void *buf, size_t n)         { return (int)sys_read(fd, buf, n); }
int open(const char *p, int f, ...)           { return sys_open(p, f, 0644); }
int close(int fd)                             { return sys_close(fd); }
off_t lseek(int fd, off_t off, int w)         { return (off_t)sys_lseek(fd, off, w); }

/* Minimal printf — supports %s %d %u %x %c %% %ld %lu %p */
static void _putchars(const char *s, size_t n) { sys_write(1, s, n); }

static void _print_uint(unsigned long v, int base, int width, int zero_pad) {
    char tmp[24]; int i = 0;
    const char *digits = "0123456789abcdef";
    if (!v) { tmp[i++] = '0'; }
    else while (v) { tmp[i++] = digits[v % base]; v /= base; }
    while (i < width) tmp[i++] = zero_pad ? '0' : ' ';
    /* reverse */
    for (int a=0, b=i-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    _putchars(tmp, i);
}

int vprintf(const char *fmt, __builtin_va_list ap) {
    int written = 0;
    while (*fmt) {
        if (*fmt != '%') { putchar(*fmt++); written++; continue; }
        fmt++;
        int width = 0, zero_pad = 0, is_long = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt++ - '0'); }
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; }  /* ll */
        switch (*fmt++) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    size_t l = strlen(s); _putchars(s, l); written += l; break; }
        case 'd': { long v = is_long ? __builtin_va_arg(ap, long)
                                      : __builtin_va_arg(ap, int);
                    if (v < 0) { putchar('-'); v = -v; written++; }
                    _print_uint((unsigned long)v, 10, width, zero_pad); break; }
        case 'u': { unsigned long v = is_long ? __builtin_va_arg(ap, unsigned long)
                                               : __builtin_va_arg(ap, unsigned int);
                    _print_uint(v, 10, width, zero_pad); break; }
        case 'x':
        case 'X': { unsigned long v = is_long ? __builtin_va_arg(ap, unsigned long)
                                               : __builtin_va_arg(ap, unsigned int);
                    _print_uint(v, 16, width, zero_pad); break; }
        case 'p': { unsigned long v = (unsigned long)__builtin_va_arg(ap, void*);
                    _putchars("0x", 2); _print_uint(v, 16, 16, 1); break; }
        case 'c': { putchar(__builtin_va_arg(ap, int)); written++; break; }
        case '%': putchar('%'); written++; break;
        default:  putchar('?'); written++; break;
        }
    }
    return written;
}

int printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int fprintf(int fd, const char *fmt, ...) {
    /* For now just write to fd=2 (stderr) or fd=1 (stdout) */
    (void)fd;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    /* Very minimal snprintf — only %s %d %u %x supported */
    (void)sz;  /* TODO: respect limit */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    /* Redirect printf output to buffer — quick hack via write hook */
    /* For now: just do basic substitution */
    char *out = buf;
    size_t rem = sz;
    while (*fmt && rem > 1) {
        if (*fmt != '%') { *out++ = *fmt++; rem--; continue; }
        fmt++;
        switch (*fmt++) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    while (*s && rem > 1) { *out++ = *s++; rem--; } break; }
        case 'd': { int v = __builtin_va_arg(ap, int);
                    char tmp[12]; int i=0;
                    if (v<0){*out++='-';rem--;v=-v;}
                    if(!v){tmp[i++]='0';}
                    else while(v){tmp[i++]='0'+v%10;v/=10;}
                    for(int a=0,b=i-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
                    for(int j=0;j<i&&rem>1;j++){*out++=tmp[j];rem--;} break; }
        case '%': if(rem>1){*out++='%';rem--;} break;
        default: break;
        }
    }
    *out = 0;
    __builtin_va_end(ap);
    return (int)(out - buf);
}

/* getchar via sys_read(stdin) */
int getchar(void) {
    char c;
    long r = sys_read(0, &c, 1);
    return (r == 1) ? (int)(unsigned char)c : EOF;
}

/* readline: read a line from fd into buf (max len-1 chars + NUL) */
int readline(int fd, char *buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c;
        long r = sys_read(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\n') { buf[i++] = '\n'; break; }
        buf[i++] = c;
    }
    buf[i] = 0;
    return i;
}

/* ------------------------------------------------------------------ */
/* Process                                                               */
/* ------------------------------------------------------------------ */

void _exit(int code) { sys_exit(code); }
void exit(int code)  { _exit(code); }

pid_t fork(void)  { return sys_fork(); }
pid_t getpid(void){ return sys_getpid(); }

int execv(const char *path, char *const argv[]) {
    return (int)sys_execve(path, argv, NULL);
}
int execvp(const char *file, char *const argv[]) {
    /* Simple: just pass file as path — no PATH search here */
    return execv(file, argv);
}
int execve(const char *p, char *const av[], char *const ev[]) {
    return (int)sys_execve(p, av, ev);
}
pid_t waitpid(pid_t pid, int *status, int options) {
    return sys_wait4(pid, status, options, NULL);
}
int kill(pid_t pid, int sig) { return sys_kill(pid, sig); }

/* ------------------------------------------------------------------ */
/* Environment / misc                                                    */
/* ------------------------------------------------------------------ */

/* Minimal getenv — searches envp directly via global */
static char **__environ = NULL;

void __set_environ(char **env) { __environ = env; }

char *getenv(const char *name) {
    if (!__environ) return NULL;
    size_t nlen = strlen(name);
    for (char **e = __environ; *e; e++) {
        if (strncmp(*e, name, nlen) == 0 && (*e)[nlen] == '=')
            return (*e) + nlen + 1;
    }
    return NULL;
}

/* atoi / atol */
int atoi(const char *s) {
    int neg = 0, r = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') r = r*10 + (*s++ - '0');
    return neg ? -r : r;
}
long atol(const char *s) {
    int neg = 0; long r = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') r = r*10 + (*s++ - '0');
    return neg ? -r : r;
}

/* isXXX helpers */
int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
int isdigit(int c) { return c>='0'&&c<='9'; }
int isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isalnum(int c) { return isdigit(c)||isalpha(c); }
int tolower(int c) { return (c>='A'&&c<='Z') ? c+32 : c; }
int toupper(int c) { return (c>='a'&&c<='z') ? c-32 : c; }

/* abs */
int abs(int x) { return x < 0 ? -x : x; }
