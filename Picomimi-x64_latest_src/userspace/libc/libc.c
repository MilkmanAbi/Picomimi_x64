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

/* fprintf and snprintf are defined later with full vsnprintf support */

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

/* ------------------------------------------------------------------ */
/* Directory API (POSIX opendir/readdir/closedir)                       */
/* ------------------------------------------------------------------ */

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

typedef struct {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
} dirent_t;
typedef dirent_t Dirent;

typedef struct {
    int   fd;
    char  buf[4096];
    int   buf_pos;
    int   buf_len;
} DIR;

DIR *opendir(const char *path) {
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof(DIR));
    if (!d) { close(fd); return NULL; }
    d->fd      = fd;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

struct __kernel_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

dirent_t *readdir(DIR *d) {
    static dirent_t ent;
    if (!d) return NULL;

    /* Refill buffer if empty */
    if (d->buf_pos >= d->buf_len) {
        long n = __syscall3(SYS_getdents64, (long)d->fd,
                            (long)d->buf, (long)sizeof(d->buf));
        if (n <= 0) return NULL;
        d->buf_len = (int)n;
        d->buf_pos = 0;
    }

    struct __kernel_dirent64 *kd =
        (struct __kernel_dirent64 *)(d->buf + d->buf_pos);
    d->buf_pos += kd->d_reclen;

    ent.d_ino    = (unsigned long)kd->d_ino;
    ent.d_off    = (long)kd->d_off;
    ent.d_reclen = kd->d_reclen;
    ent.d_type   = kd->d_type;
    /* d_name is right after the fixed fields */
    strncpy(ent.d_name, kd->d_name, 255);
    ent.d_name[255] = '\0';
    return &ent;
}

int closedir(DIR *d) {
    if (!d) return -1;
    int r = close(d->fd);
    free(d);
    return r;
}

/* ------------------------------------------------------------------ */
/* stat / fstat                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned long  st_nlink;
    unsigned int   st_mode;
    unsigned int   st_uid;
    unsigned int   st_gid;
    unsigned int   __pad0;
    unsigned long  st_rdev;
    long           st_size;
    long           st_blksize;
    long           st_blocks;
    long           st_atim_sec;
    long           st_atim_nsec;
    long           st_mtim_sec;
    long           st_mtim_nsec;
    long           st_ctim_sec;
    long           st_ctim_nsec;
    long           __unused[3];
} stat_t;

int stat(const char *path, stat_t *buf) {
    return (int)__syscall2(4 /* SYS_stat */, (long)path, (long)buf);
}
int lstat(const char *path, stat_t *buf) {
    return (int)__syscall2(6 /* SYS_lstat */, (long)path, (long)buf);
}
int fstat(int fd, stat_t *buf) {
    return (int)__syscall2(5 /* SYS_fstat */, (long)fd, (long)buf);
}

/* ------------------------------------------------------------------ */
/* Stdio: sprintf / fprintf / fwrite / fflush stubs                     */
/* ------------------------------------------------------------------ */

/* Very small sprintf (no floats) */
static int _fmt_num(char *out, size_t left, unsigned long v, int base,
                    int pad, char padch) {
    char tmp[22];
    const char *digs = "0123456789abcdef";
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = digs[v % (unsigned)base]; v /= (unsigned)base; } }
    /* reverse */
    for (int i=0,j=n-1; i<j; i++,j--) {
        char t=tmp[i]; tmp[i]=tmp[j]; tmp[j]=t;
    }
    int written = 0;
    while (pad > n && left > 1) { *out++ = padch; left--; written++; pad--; }
    for (int i = 0; i < n && left > 1; i++, left--) { *out++ = tmp[i]; written++; }
    return written;
}

int vsnprintf(char *buf, size_t sz, const char *fmt, __builtin_va_list ap) {
    char *p = buf;
    size_t left = sz;
    while (*fmt && left > 1) {
        if (*fmt != '%') { *p++ = *fmt++; left--; continue; }
        fmt++; /* skip % */
        char padch = ' ';
        int  padw  = 0;
        if (*fmt == '0') { padch = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { padw = padw*10 + (*fmt++ - '0'); }
        int lng = 0;
        if (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'l') { lng++; fmt++; }
        char conv = *fmt++;
        int w = 0;
        switch (conv) {
        case 'd': case 'i': {
            long v = lng ? __builtin_va_arg(ap, long) : (long)__builtin_va_arg(ap, int);
            if (v < 0 && left > 1) { *p++ = '-'; left--; v = -v; }
            w = _fmt_num(p, left, (unsigned long)v, 10, padw, padch);
            p += w; left -= (size_t)w; break; }
        case 'u': {
            unsigned long v = lng ? __builtin_va_arg(ap, unsigned long)
                                  : (unsigned long)__builtin_va_arg(ap, unsigned int);
            w = _fmt_num(p, left, v, 10, padw, padch);
            p += w; left -= (size_t)w; break; }
        case 'x': case 'X': {
            unsigned long v = lng ? __builtin_va_arg(ap, unsigned long)
                                  : (unsigned long)__builtin_va_arg(ap, unsigned int);
            w = _fmt_num(p, left, v, 16, padw, padch);
            p += w; left -= (size_t)w; break; }
        case 'p': {
            unsigned long v = (unsigned long)__builtin_va_arg(ap, void *);
            if (left > 2) { *p++ = '0'; *p++ = 'x'; left -= 2; }
            w = _fmt_num(p, left, v, 16, padw, padch);
            p += w; left -= (size_t)w; break; }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && left > 1) { *p++ = *s++; left--; }
            break; }
        case 'c': {
            int c = __builtin_va_arg(ap, int);
            if (left > 1) { *p++ = (char)c; left--; } break; }
        case '%':
            if (left > 1) { *p++ = '%'; left--; } break;
        default:
            if (left > 1) { *p++ = conv; left--; } break;
        }
    }
    if (sz > 0) *p = '\0';
    return (int)(p - buf);
}

int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, sz, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, 65536, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* FILE* stdio stubs — enough for fprintf(stderr,...) patterns */
typedef struct { int fd; } FILE;
static FILE _stdin_f  = {0};
static FILE _stdout_f = {1};
static FILE _stderr_f = {2};
FILE *stdin  = &_stdin_f;
FILE *stdout = &_stdout_f;
FILE *stderr = &_stderr_f;

int fprintf(FILE *f, const char *fmt, ...) {
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    write(f->fd, buf, (size_t)n);
    return n;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    write(f->fd, s, n);
    return (int)n;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    write(f->fd, &ch, 1);
    return c;
}

int fflush(FILE *f) { (void)f; return 0; }

/* ------------------------------------------------------------------ */
/* String extras                                                         */
/* ------------------------------------------------------------------ */

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (s) *saveptr = s;
    char *p = *saveptr;
    if (!p || !*p) return NULL;
    /* skip leading delimiters */
    while (*p && strchr(delim, *p)) p++;
    if (!*p) { *saveptr = p; return NULL; }
    char *start = p;
    while (*p && !strchr(delim, *p)) p++;
    if (*p) { *p++ = '\0'; }
    *saveptr = p;
    return start;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

long strtol(const char *s, char **endp, int base) {
    while (*s == ' ') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (*(s+1)=='x'||*(s+1)=='X')) { base=16; s+=2; }
        else if (*s == '0') { base=8; s++; }
        else base=10;
    } else if (base == 16 && *s=='0' && (*(s+1)=='x'||*(s+1)=='X')) s+=2;
    long v = 0;
    while (*s) {
        int d;
        if (*s>='0'&&*s<='9') d=*s-'0';
        else if (*s>='a'&&*s<='f') d=*s-'a'+10;
        else if (*s>='A'&&*s<='F') d=*s-'A'+10;
        else break;
        if (d >= base) break;
        v = v*base + d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    return (unsigned long)strtol(s, endp, base);
}

/* ------------------------------------------------------------------ */
/* Signal                                                                */
/* ------------------------------------------------------------------ */

#define SIG_DFL ((void(*)(int))0)
#define SIG_IGN ((void(*)(int))1)

typedef void (*sighandler_t)(int);

struct sigaction_u {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask[2];
};

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction_u act, old;
    act.sa_handler  = handler;
    act.sa_flags    = 0x04000000; /* SA_RESTORER */
    act.sa_restorer = NULL;
    act.sa_mask[0]  = 0;
    act.sa_mask[1]  = 0;
    __syscall4(13 /* SYS_rt_sigaction */, (long)sig,
               (long)&act, (long)&old, (long)8);
    return old.sa_handler;
}

/* ------------------------------------------------------------------ */
/* sleep / usleep                                                         */
/* ------------------------------------------------------------------ */

struct timespec_u { long tv_sec; long tv_nsec; };

int sleep(unsigned int secs) {
    struct timespec_u ts = { (long)secs, 0 };
    __syscall2(SYS_nanosleep, (long)&ts, 0);
    return 0;
}

int usleep(unsigned long us) {
    struct timespec_u ts = { (long)(us/1000000), (long)((us%1000000)*1000) };
    __syscall2(SYS_nanosleep, (long)&ts, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* setjmp / longjmp (minimal)                                            */
/* ------------------------------------------------------------------ */

typedef unsigned long jmp_buf[8];

int __attribute__((returns_twice)) setjmp(jmp_buf env) {
    __asm__ volatile(
        "mov %%rbx, 0(%0)\n"
        "mov %%r12, 8(%0)\n"
        "mov %%r13,16(%0)\n"
        "mov %%r14,24(%0)\n"
        "mov %%r15,32(%0)\n"
        "mov %%rbp,40(%0)\n"
        "lea 1f(%%rip),%%rax\n"
        "mov %%rax, 48(%0)\n"
        "mov %%rsp, 56(%0)\n"
        "xor %%eax, %%eax\n"
        "1:\n"
        : : "r"(env) : "rax","memory");
    return 0;  /* compiler will handle the return_twice attribute */
}

__attribute__((noreturn)) void longjmp(jmp_buf env, int val) {
    if (val == 0) val = 1;
    __asm__ volatile(
        "mov 0(%0), %%rbx\n"
        "mov 8(%0), %%r12\n"
        "mov 16(%0), %%r13\n"
        "mov 24(%0), %%r14\n"
        "mov 32(%0), %%r15\n"
        "mov 40(%0), %%rbp\n"
        "mov 56(%0), %%rsp\n"
        "mov 48(%0), %%rcx\n"
        "mov %1, %%eax\n"
        "jmp *%%rcx\n"
        : : "r"(env), "r"(val) : "rax","rbx","rcx","r12","r13","r14","r15","rbp","rsp");
    __builtin_unreachable();
}

