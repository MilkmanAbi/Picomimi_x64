/**
 * Picomimi-x64 /bin/sh — Userspace Shell v2.0
 *
 * POSIX shell running as a real ELF process via syscalls.
 *
 * Key fixes over v1:
 *   - Proper pipeline forking: ALL stages (including builtins) fork
 *   - All child pids collected and waited — no zombie leaks
 *   - $(cmd) / `cmd` command substitution via pipe+fork+read
 *   - for VAR in WORDS; do BODY; done  (with $(seq ...) expansion)
 *   - while COND; do BODY; done
 *   - if/then/elif/else/fi
 *   - break / continue / return
 *   - && / || chaining
 *   - 2> 2>&1 redirection
 *   - find_in_path uses sys_access — no fd leak
 *   - PS1 expansion (\w \u \h \$)
 *   - Interactive: up/down arrow history, backslash continuation,
 *     multi-line block accumulation
 */

#include "../include/syscall.h"

#define NULL ((void*)0)
typedef long          ssize_t;
typedef unsigned long size_t;
typedef long          off_t;
typedef int           pid_t;

/* ---- libc forward declarations ---- */
extern size_t  strlen(const char *);
extern char   *strcpy(char *, const char *);
extern char   *strncpy(char *, const char *, size_t);
extern int     strcmp(const char *, const char *);
extern int     strncmp(const char *, const char *, size_t);
extern char   *strcat(char *, const char *);
extern char   *strncat(char *, const char *, size_t);
extern char   *strchr(const char *, int);
extern char   *strrchr(const char *, int);
extern char   *strstr(const char *, const char *);
extern char   *strdup(const char *);
extern void   *memcpy(void *, const void *, size_t);
extern void   *memmove(void *, const void *, size_t);
extern void   *memset(void *, int, size_t);
extern void   *malloc(size_t);
extern void    free(void *);
extern int     printf(const char *, ...);
extern int     snprintf(char *, size_t, const char *, ...);
extern int     puts(const char *);
extern int     putchar(int);
extern int     readline(int, char *, int);
extern void    exit(int) __attribute__((noreturn));
extern char   *getenv(const char *);
extern void    __set_environ(char **);
extern int     isspace(int);
extern int     isdigit(int);
extern int     atoi(const char *);
extern int     open(const char *, int, ...);
extern int     close(int);
extern int     write(int, const void *, size_t);
extern int     read(int, void *, size_t);

/* raw syscall wrappers we need that aren't in libc as plain functions */
static inline pid_t  sh_fork(void)                { return (pid_t)__syscall0(SYS_fork); }
static inline int    sh_pipe(int *fds)            { return (int)__syscall1(SYS_pipe,(long)fds); }
static inline int    sh_dup2(int o, int n)        { return (int)__syscall2(SYS_dup2,o,n); }
static inline int    sh_close(int fd)             { return (int)__syscall1(SYS_close,fd); }
static inline long   sh_read(int fd,void*b,size_t n){ return __syscall3(SYS_read,fd,(long)b,(long)n); }
static inline long   sh_write(int fd,const void*b,size_t n){ return __syscall3(SYS_write,fd,(long)b,(long)n); }
static inline int    sh_open(const char*p,int f,int m){ return (int)__syscall3(SYS_open,(long)p,f,m); }
static inline int    sh_chdir(const char *p)      { return (int)__syscall1(SYS_chdir,(long)p); }
static inline long   sh_getcwd(char *b,size_t n)  { return __syscall2(SYS_getcwd,(long)b,(long)n); }
static inline pid_t  sh_wait4(pid_t pid,int*st,int opts,void*ru){ return (pid_t)__syscall4(SYS_wait4,pid,(long)st,opts,(long)ru); }
static inline int    sh_kill(pid_t p,int s)       { return (int)__syscall2(SYS_kill,p,s); }
static inline int    sh_access(const char*p,int m){ return (int)__syscall2(SYS_access,(long)p,m); }
static inline long   sh_execve(const char*p,char*const av[],char*const ev[]){ return __syscall3(SYS_execve,(long)p,(long)av,(long)ev); }
static inline pid_t  sh_getpid(void)              { return (pid_t)__syscall0(SYS_getpid); }
static inline long   sh_getdents64(int fd,void*b,unsigned n){ return __syscall3(SYS_getdents64,fd,(long)b,(long)n); }
static inline long   sh_mkdir(const char*p,int m) { return __syscall2(SYS_mkdir,(long)p,m); }
static inline long   sh_unlink(const char*p)      { return __syscall1(SYS_unlink,(long)p); }

/* ---- ANSI ---- */
#define CR  "\033[0m"
#define CG  "\033[1;32m"
#define CB  "\033[1;34m"
#define CY  "\033[33m"
#define CC  "\033[36m"
#define CBD "\033[1m"

/* ---- flags ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_DIRECTORY 0200000

/* ---- wait macros ---- */
#define WIFEXITED(s)    (((s)&0x7f)==0)
#define WEXITSTATUS(s)  (((s)>>8)&0xff)
#define WIFSIGNALED(s)  (((s)&0x7f)&&((s)&0x7f)!=0x7f)
#define WTERMSIG(s)     ((s)&0x7f)

/* ---- limits ---- */
#define MAX_ARGS    128
#define MAX_LINE    4096
#define MAX_ENV     512
#define HIST_SIZE   128
#define PATH_MAX    4096
#define MAX_CMDS    64

/* ================================================================
 * Global state
 * ================================================================ */
static char  *g_env[MAX_ENV];
static int    g_nenv      = 0;
static char  *g_hist[HIST_SIZE];
static int    g_hist_head = 0;
static int    g_hist_cnt  = 0;
static int    g_exit      = 0;   /* $? */
static char   g_cwd[PATH_MAX];
static char   g_argv0[PATH_MAX]; /* $0 */
static char  *g_pos[MAX_ARGS];   /* $1..$N */
static int    g_npos      = 0;
static pid_t  g_pid       = 0;   /* $$ */

/* Loop/function control */
static int g_break    = 0;
static int g_continue = 0;
static int g_return   = 0;

/* ================================================================
 * Environment
 * ================================================================ */
static void env_set(const char *name, const char *val) {
    size_t nlen = strlen(name), vlen = strlen(val);
    for (int i = 0; i < g_nenv; i++) {
        if (strncmp(g_env[i], name, nlen) == 0 && g_env[i][nlen] == '=') {
            free(g_env[i]);
            char *b = malloc(nlen + vlen + 2);
            if (!b) return;
            memcpy(b, name, nlen); b[nlen]='='; memcpy(b+nlen+1, val, vlen+1);
            g_env[i] = b; __set_environ(g_env); return;
        }
    }
    if (g_nenv < MAX_ENV - 1) {
        char *b = malloc(nlen + vlen + 2);
        if (!b) return;
        memcpy(b, name, nlen); b[nlen]='='; memcpy(b+nlen+1, val, vlen+1);
        g_env[g_nenv++] = b; g_env[g_nenv] = NULL; __set_environ(g_env);
    }
}

static void env_unset(const char *name) {
    size_t nlen = strlen(name);
    for (int i = 0; i < g_nenv; i++) {
        if (strncmp(g_env[i], name, nlen) == 0 && g_env[i][nlen] == '=') {
            free(g_env[i]);
            for (int j = i; j < g_nenv-1; j++) g_env[j] = g_env[j+1];
            g_env[--g_nenv] = NULL; __set_environ(g_env); return;
        }
    }
}

/* ================================================================
 * Variable expansion
 * ================================================================ */

/* Forward decl for recursive $(cmd) */
static int sh_run(const char *line);

/* Fork a child, run cmd in /bin/sh -c, capture stdout.
 * Returns malloc'd result (caller frees). Trailing \n stripped. */
static char *capture(const char *cmd) {
    int pfd[2];
    if (sh_pipe(pfd) < 0) return NULL;

    pid_t pid = sh_fork();
    if (pid < 0) { sh_close(pfd[0]); sh_close(pfd[1]); return NULL; }

    if (pid == 0) {
        sh_close(pfd[0]);
        sh_dup2(pfd[1], 1);
        sh_close(pfd[1]);
        char *av[] = { "/bin/sh", "-c", (char *)cmd, NULL };
        sh_execve("/bin/sh", av, g_env);
        exit(127);
    }
    sh_close(pfd[1]);

    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) { sh_close(pfd[0]); sh_wait4(pid,NULL,0,NULL); return NULL; }

    for (;;) {
        if (len + 512 > cap) {
            char *nb = malloc(cap * 2);
            if (!nb) break;
            memcpy(nb, buf, len); free(buf); buf = nb; cap *= 2;
        }
        long n = sh_read(pfd[0], buf+len, cap-len-1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    sh_close(pfd[0]);
    buf[len] = '\0';

    int st = 0;
    sh_wait4(pid, &st, 0, NULL);
    if (WIFEXITED(st)) g_exit = WEXITSTATUS(st);

    /* strip trailing newlines */
    while (len > 0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len] = '\0';
    return buf;
}

/* expand_vars: $VAR ${VAR} $? $$ $# $0-$N $(cmd) `cmd`
 * Does NOT do word-splitting or globbing (that's the tokeniser's job). */
static void expand_vars(const char *in, char *out, size_t osz) {
    size_t i=0, o=0;
#define OC(c) do{ if(o<osz-1) out[o++]=(c); }while(0)
#define OS(s) do{ const char*_s=(s); while(*_s&&o<osz-1) out[o++]=*_s++; }while(0)

    while (in[i] && o < osz-1) {
        char c = in[i];

        if (c=='\\' && in[i+1]) { i++; OC(in[i++]); continue; }

        /* single-quote: verbatim */
        if (c=='\'') {
            i++;
            while (in[i] && in[i]!='\'') { OC(in[i]); i++; }
            if (in[i]=='\'') i++;
            continue;
        }

        if (c != '$' && c != '`') { OC(c); i++; continue; }

        /* backtick substitution */
        if (c == '`') {
            i++;
            char cb[MAX_LINE]; int ci=0;
            while (in[i] && in[i]!='`' && ci<(int)sizeof(cb)-1) cb[ci++]=in[i++];
            cb[ci]='\0'; if (in[i]=='`') i++;
            char *r = capture(cb); if (r) { OS(r); free(r); }
            continue;
        }

        i++; /* skip $ */

        /* $(...) — nested parens counted */
        if (in[i]=='(') {
            i++; int depth=1;
            char cb[MAX_LINE]; int ci=0;
            while (in[i] && depth>0 && ci<(int)sizeof(cb)-1) {
                if (in[i]=='(') depth++;
                else if (in[i]==')') { if(--depth==0){i++;break;} }
                cb[ci++]=in[i++];
            }
            cb[ci]='\0';
            char *r = capture(cb); if (r) { OS(r); free(r); }
            continue;
        }

        /* ${VAR} */
        if (in[i]=='{') {
            i++;
            char vn[256]; int vi=0;
            while (in[i] && in[i]!='}' && vi<255) vn[vi++]=in[i++];
            vn[vi]='\0'; if (in[i]=='}') i++;
            char *v = getenv(vn); if (v) { OS(v); }
            continue;
        }

        /* $? */
        if (in[i]=='?') {
            i++;
            char tmp[12]; int ti=0, v=g_exit;
            if (!v) { OC('0'); continue; }
            while (v) { tmp[ti++]='0'+(v%10); v/=10; }
            while (ti-->0) OC(tmp[ti]);
            continue;
        }

        /* $$ */
        if (in[i]=='$') {
            i++;
            char tmp[12]; int ti=0; pid_t p=g_pid;
            if (!p) { OC('0'); continue; }
            while (p) { tmp[ti++]='0'+(p%10); p/=10; }
            while (ti-->0) OC(tmp[ti]);
            continue;
        }

        /* $# */
        if (in[i]=='#') {
            i++;
            char tmp[12]; int ti=0, v=g_npos;
            if (!v) { OC('0'); continue; }
            while (v) { tmp[ti++]='0'+(v%10); v/=10; }
            while (ti-->0) OC(tmp[ti]);
            continue;
        }

        /* $0..$9 */
        if (isdigit((unsigned char)in[i])) {
            int idx = in[i++]-'0';
            const char *v = (idx==0) ? g_argv0 :
                            (idx<=g_npos) ? g_pos[idx-1] : NULL;
            if (v) { OS(v); }
            continue;
        }

        /* $NAME */
        if (in[i]=='_'||(in[i]>='a'&&in[i]<='z')||(in[i]>='A'&&in[i]<='Z')) {
            char vn[256]; int vi=0;
            while (in[i]&&(in[i]=='_'||isdigit((unsigned char)in[i])||
                   (in[i]>='a'&&in[i]<='z')||(in[i]>='A'&&in[i]<='Z')))
                vn[vi++]=in[i++];
            vn[vi]='\0';
            char *v = getenv(vn); if (v) { OS(v); }
            continue;
        }

        OC('$'); /* bare $ */
    }
    out[o]='\0';
#undef OC
#undef OS
}

/* ================================================================
 * Command struct
 * ================================================================ */
typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *rin;          /* < */
    char *rout;         /* > */
    char *rapp;         /* >> */
    char *rerr;         /* 2> */
    int   err2out;      /* 2>&1 */
    int   background;
    int   connector;    /* 0=end 1=| 2=|| 3=&& */
} cmd_t;

/* ================================================================
 * Tokeniser
 * ================================================================ */
static void cmd_free(cmd_t *c) {
    for (int i=0;i<c->argc;i++) { free(c->argv[i]); c->argv[i]=NULL; }
    free(c->rin); free(c->rout); free(c->rapp); free(c->rerr);
    c->argc=0; c->rin=c->rout=c->rapp=c->rerr=NULL;
}

static int tokenize(char *line, cmd_t *cmds, int maxcmds) {
    int nc=0;
    memset(&cmds[0], 0, sizeof(cmd_t));
    cmd_t *cur = &cmds[0];
    char *p = line;

    while (*p) {
        while (*p==' '||*p=='\t') p++;
        if (!*p) break;

        /* connectors */
        if (*p=='|' && *(p+1)=='|') {
            cur->connector=2; p+=2;
            if (nc+1<maxcmds){nc++;memset(&cmds[nc],0,sizeof(cmd_t));cur=&cmds[nc];}
            continue;
        }
        if (*p=='&' && *(p+1)=='&') {
            cur->connector=3; p+=2;
            if (nc+1<maxcmds){nc++;memset(&cmds[nc],0,sizeof(cmd_t));cur=&cmds[nc];}
            continue;
        }
        if (*p=='|') {
            cur->connector=1; p++;
            if (nc+1<maxcmds){nc++;memset(&cmds[nc],0,sizeof(cmd_t));cur=&cmds[nc];}
            continue;
        }
        if (*p=='&') { cur->background=1; p++; continue; }
        if (*p==';') {
            cur->connector=0; p++;
            if (cur->argc>0 && nc+1<maxcmds){nc++;memset(&cmds[nc],0,sizeof(cmd_t));cur=&cmds[nc];}
            continue;
        }

        /* 2>&1 */
        if (*p=='2' && *(p+1)=='>' && *(p+2)=='&' && *(p+3)=='1') {
            cur->err2out=1; p+=4; continue;
        }
        /* 2>file */
        if (*p=='2' && *(p+1)=='>') {
            p+=2; while(*p==' ')p++;
            char*s=p; while(*p&&!isspace((unsigned char)*p)&&*p!='|'&&*p!='&'&&*p!=';')p++;
            char sv=*p;*p='\0'; cur->rerr=strdup(s); *p=sv; continue;
        }
        /* >>file */
        if (*p=='>' && *(p+1)=='>') {
            p+=2; while(*p==' ')p++;
            char*s=p; while(*p&&!isspace((unsigned char)*p)&&*p!='|'&&*p!='&'&&*p!=';')p++;
            char sv=*p;*p='\0'; cur->rapp=strdup(s); *p=sv; continue;
        }
        /* >file */
        if (*p=='>') {
            p++; while(*p==' ')p++;
            char*s=p; while(*p&&!isspace((unsigned char)*p)&&*p!='|'&&*p!='&'&&*p!=';')p++;
            char sv=*p;*p='\0'; cur->rout=strdup(s); *p=sv; continue;
        }
        /* <file */
        if (*p=='<') {
            p++; while(*p==' ')p++;
            char*s=p; while(*p&&!isspace((unsigned char)*p)&&*p!='|'&&*p!='&'&&*p!=';')p++;
            char sv=*p;*p='\0'; cur->rin=strdup(s); *p=sv; continue;
        }

        /* word */
        char tok[MAX_LINE]; int ti=0;
        while (*p && ti<(int)sizeof(tok)-1) {
            if (*p=='\'') {
                p++;
                while(*p&&*p!='\''&&ti<(int)sizeof(tok)-1) tok[ti++]=*p++;
            if(*p=='\'') p++;
            continue;
            }
            if (*p=='"') {
                p++;
                while(*p&&*p!='"'&&ti<(int)sizeof(tok)-1) {
                    if(*p=='\\'&&*(p+1)){p++;
                        switch(*p){case'n':tok[ti++]='\n';break;case't':tok[ti++]='\t';break;
                        case'"':tok[ti++]='"';break;case'\\':tok[ti++]='\\';break;
                        default:tok[ti++]='\\';if(ti<(int)sizeof(tok)-1)tok[ti++]=*p;break;}
                        p++;continue;}
                    tok[ti++]=*p++;
                }
                if(*p=='"') p++;
                continue;
            }
            if (*p=='\\' && *(p+1)) { p++; tok[ti++]=*p++; continue; }
            if (isspace((unsigned char)*p)||*p=='|'||*p=='&'||*p=='<'||*p=='>'||*p==';') break;
            tok[ti++]=*p++;
        }
        tok[ti]='\0';
        if (ti>0 && cur->argc<MAX_ARGS-1)
            cur->argv[cur->argc++]=strdup(tok);
    }

    return (cur->argc>0 || nc>0) ? nc+1 : 0;
}

/* ================================================================
 * PATH search — uses access(), no fd leak
 * ================================================================ */
static char g_found[PATH_MAX];

static const char *find_cmd(const char *name) {
    if (strchr(name,'/')) return name;
    char *path = getenv("PATH");
    if (!path) path = "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin";
    char dirs[4096];
    strncpy(dirs, path, sizeof(dirs)-1); dirs[sizeof(dirs)-1]='\0';
    char *d = dirs;
    while (d && *d) {
        char *col = strchr(d,':'); if(col)*col='\0';
        snprintf(g_found, sizeof(g_found), "%s/%s", d, name);
        if (sh_access(g_found, 1) == 0) return g_found;  /* X_OK */
        d = col ? col+1 : NULL;
    }
    return NULL;
}

/* ================================================================
 * Built-ins
 * ================================================================ */
struct kdirent64 {
    unsigned long long d_ino; long long d_off;
    unsigned short d_reclen; unsigned char d_type; char d_name[];
};
#define DT_DIR 4
#define DT_LNK 10

static int b_ls(int argc, char **argv) {
    const char *path = argc>1 ? argv[1] : g_cwd;
    int fd = sh_open(path, O_RDONLY|O_DIRECTORY, 0);
    if (fd<0){printf("ls: cannot open '%s'\n",path);return 1;}
    char buf[4096]; long n; int col=0;
    while ((n=sh_getdents64(fd,buf,sizeof(buf)))>0) {
        long off=0;
        while (off<n) {
            struct kdirent64 *d=(struct kdirent64*)(buf+off);
            if (d->d_name[0]!='.'){
                if (d->d_type==DT_DIR) printf(CB "%-20s" CR, d->d_name);
                else if (d->d_type==DT_LNK) printf(CC "%-20s" CR, d->d_name);
                else printf("%-20s", d->d_name);
                if(++col%4==0) putchar('\n');
            }
            off+=d->d_reclen;
        }
    }
    if(col%4!=0) putchar('\n');
    sh_close(fd); return 0;
}

static int b_cat(int argc, char **argv) {
    if (argc<2){char buf[512];int n;while((n=read(0,buf,sizeof(buf)))>0)write(1,buf,(size_t)n);return 0;}
    for (int i=1;i<argc;i++){
        int fd=sh_open(argv[i],O_RDONLY,0);
        if(fd<0){printf("cat: %s: error %d\n",argv[i],fd);continue;}
        char buf[4096];long n;
        while((n=sh_read(fd,buf,sizeof(buf)))>0) sh_write(1,buf,(size_t)n);
        sh_close(fd);
    }
    return 0;
}

static int b_echo(int argc, char **argv) {
    int nl=1, interp=0, start=1;
    for (int i=1;i<argc;i++){
        if(argv[i][0]!='-') break;
        int j=1, valid=1;
        while(argv[i][j]){
            if     (argv[i][j]=='n') nl=0;
            else if(argv[i][j]=='e') interp=1;
            else if(argv[i][j]=='E') interp=0;
            else   { valid=0; break; }
            j++;
        }
        if(!valid) break;
        start=i+1;
    }
    for (int i=start;i<argc;i++){
        if(i>start) putchar(' ');
        char *s=argv[i];
        if(!interp){sh_write(1,s,strlen(s));continue;}
        while(*s){
            if(*s=='\\'&&*(s+1)){s++;
                switch(*s){case'n':putchar('\n');break;case't':putchar('\t');break;
                case'r':putchar('\r');break;case'a':putchar('\a');break;
                case'\\':putchar('\\');break;
                case'0':{int v=0,k=0;while(k<3&&*(s+1)>='0'&&*(s+1)<='7'){s++;v=v*8+(*s-'0');k++;}putchar(v);break;}
                default:putchar('\\');putchar(*s);break;}
            } else putchar(*s);
            s++;
        }
    }
    if(nl) putchar('\n');
    return 0;
}

static int b_cd(int argc, char **argv) {
    const char *dest;
    if (argc<2||strcmp(argv[1],"~")==0){dest=getenv("HOME");if(!dest)dest="/root";}
    else if(strcmp(argv[1],"-")==0){dest=getenv("OLDPWD");if(!dest){puts("cd: OLDPWD not set");return 1;}}
    else dest=argv[1];
    env_set("OLDPWD",g_cwd);
    if(sh_chdir(dest)<0){printf("cd: %s: no such directory\n",dest);return 1;}
    sh_getcwd(g_cwd,sizeof(g_cwd));
    env_set("PWD",g_cwd);
    return 0;
}

static int b_pwd(int a,char**v){(void)a;(void)v;sh_getcwd(g_cwd,sizeof(g_cwd));puts(g_cwd);return 0;}

static int b_export(int argc,char**argv){
    for(int i=1;i<argc;i++){
        char *eq=strchr(argv[i],'=');
        if(eq){char nm[256];size_t nl=(size_t)(eq-argv[i]);if(nl>=sizeof(nm))continue;
            memcpy(nm,argv[i],nl);nm[nl]='\0';env_set(nm,eq+1);}
    }
    return 0;
}
static int b_unset(int argc,char**argv){for(int i=1;i<argc;i++)env_unset(argv[i]);return 0;}
static int b_env(int a,char**v){(void)a;(void)v;for(int i=0;g_env[i];i++)printf("%s\n",g_env[i]);return 0;}

static int b_read(int argc,char**argv){
    if(argc<2){puts("read: need variable name");return 1;}
    char buf[MAX_LINE];int n=0;char c;
    while(1){long r=sh_read(0,&c,1);if(r<=0||c=='\n')break;if(n<(int)sizeof(buf)-1)buf[n++]=c;}
    buf[n]='\0';
    char *ifs=getenv("IFS");if(!ifs)ifs=" \t\n";
    char *p=buf;
    for(int i=1;i<argc;i++){
        while(*p&&strchr(ifs,*p))p++;
        if(!*p){env_set(argv[i],"");continue;}
        if(i==argc-1){env_set(argv[i],p);break;}
        char *st=p;
        while(*p&&!strchr(ifs,*p))p++;
        char sv=*p;*p='\0';env_set(argv[i],st);*p=sv;
    }
    return 0;
}

static int b_type(int argc,char**argv){
    int r=0;
    for(int i=1;i<argc;i++){
        const char *p=find_cmd(argv[i]);
        if(p)printf("%s is %s\n",argv[i],p);
        else{printf("%s: not found\n",argv[i]);r=1;}
    }
    return r;
}

static int b_help(int a,char**v){(void)a;(void)v;
    printf(CBD CC "Picomimi Shell v2.0\n" CR
        "  Builtins: cd pwd ls cat echo read export unset env type help true false\n"
        "            break continue return exit :\n"
        "  Pipes:    cmd1 | cmd2 | cmd3\n"
        "  Redir:    > >> < 2> 2>&1\n"
        "  CmdSubst: $(cmd)  `cmd`\n"
        "  For:      for VAR in WORDS; do CMDS; done\n"
        "  While:    while CMD; do CMDS; done\n"
        "  If:       if CMD; then CMDS; [elif CMD; then CMDS;] [else CMDS;] fi\n"
        "  Seq:      cmd1 ; cmd2\n"
        "  AND/OR:   cmd1 && cmd2   cmd1 || cmd2\n"
        "  Bg:       cmd &\n");
    return 0;
}

static int is_builtin(const char *n){
    static const char *t[]={"cd","pwd","echo","export","unset","env","ls","cat",
        "read","type","help","true","false","break","continue","return","exit",":","test","[",NULL};
    for(int i=0;t[i];i++) if(strcmp(n,t[i])==0) return 1;
    return 0;
}

/* run a builtin, returns exit code, -1 if not a builtin */
static int run_builtin(int argc, char **argv) {
    if (!argc) return 0;
    char *n=argv[0];
    if(strcmp(n,"exit")==0)   exit(argc>1?atoi(argv[1]):g_exit);
    if(strcmp(n,"cd")==0)     return b_cd(argc,argv);
    if(strcmp(n,"pwd")==0)    return b_pwd(argc,argv);
    if(strcmp(n,"echo")==0)   return b_echo(argc,argv);
    if(strcmp(n,"export")==0) return b_export(argc,argv);
    if(strcmp(n,"unset")==0)  return b_unset(argc,argv);
    if(strcmp(n,"env")==0)    return b_env(argc,argv);
    if(strcmp(n,"ls")==0)     return b_ls(argc,argv);
    if(strcmp(n,"cat")==0)    return b_cat(argc,argv);
    if(strcmp(n,"read")==0)   return b_read(argc,argv);
    if(strcmp(n,"type")==0)   return b_type(argc,argv);
    if(strcmp(n,"help")==0)   return b_help(argc,argv);
    if(strcmp(n,"true")==0||strcmp(n,":")==0) return 0;
    if(strcmp(n,"false")==0)  return 1;
    if(strcmp(n,"break")==0)    { g_break=argc>1?atoi(argv[1]):1; if(!g_break)g_break=1; return 0; }
    if(strcmp(n,"continue")==0) { g_continue=argc>1?atoi(argv[1]):1; if(!g_continue)g_continue=1; return 0; }
    if(strcmp(n,"return")==0)   { g_return=1; g_exit=argc>1?atoi(argv[1]):g_exit; return g_exit; }
    if(strcmp(n,"test")==0||strcmp(n,"[")==0) {
        /* Very minimal test: -z -n string equality */
        if(argc==2) return strlen(argv[1])==0 ? 0 : 1; /* [ str ] */
        if(argc==3&&strcmp(argv[1],"-z")==0) return strlen(argv[2])==0 ? 0 : 1;
        if(argc==3&&strcmp(argv[1],"-n")==0) return strlen(argv[2])!=0 ? 0 : 1;
        if(argc==4&&strcmp(argv[2],"=")==0)  return strcmp(argv[1],argv[3])==0 ? 0 : 1;
        if(argc==4&&strcmp(argv[2],"!=")==0) return strcmp(argv[1],argv[3])!=0 ? 0 : 1;
        if(argc>=2&&strcmp(argv[1],"-f")==0) return sh_access(argv[2],0)==0 ? 0 : 1;
        if(argc>=2&&strcmp(argv[1],"-d")==0) return sh_access(argv[2],0)==0 ? 0 : 1;
        return 0;
    }
    return -1; /* not a builtin */
}

/* ================================================================
 * Apply redirections in child process
 * ================================================================ */
static void apply_redirs(cmd_t *c) {
    if(c->rin){
        int fd=sh_open(c->rin,O_RDONLY,0);
        if(fd<0){printf("sh: cannot open '%s'\n",c->rin);exit(1);}
        sh_dup2(fd,0);sh_close(fd);
    }
    if(c->rout){
        int fd=sh_open(c->rout,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd<0){printf("sh: cannot create '%s'\n",c->rout);exit(1);}
        sh_dup2(fd,1);sh_close(fd);
    }
    if(c->rapp){
        int fd=sh_open(c->rapp,O_WRONLY|O_CREAT|O_APPEND,0644);
        if(fd<0){printf("sh: cannot open '%s'\n",c->rapp);exit(1);}
        sh_dup2(fd,1);sh_close(fd);
    }
    if(c->rerr){
        int fd=sh_open(c->rerr,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd<0){printf("sh: cannot create '%s'\n",c->rerr);exit(1);}
        sh_dup2(fd,2);sh_close(fd);
    }
    if(c->err2out) sh_dup2(1,2);
}

/* ================================================================
 * Execute a single cmd (fork+exec or builtin-in-parent).
 * pipe_in / pipe_out: -1 means inherit stdin/stdout.
 * force_fork: 1 = always fork (used for pipeline stages).
 * Returns exit code.
 * ================================================================ */
static int exec_cmd(cmd_t *c, int pipe_in, int pipe_out, int force_fork) {
    if(c->argc==0) return 0;

    /* Expand first token to get the command name */
    char namebuf[MAX_LINE];
    expand_vars(c->argv[0], namebuf, sizeof(namebuf));

    /* Builtins that modify shell state run in parent (unless in pipeline) */
    if (!force_fork && is_builtin(namebuf)) {
        /* Save/restore fds for in-parent redirections */
        int sav0=-1,sav1=-1,sav2=-1;
        if(c->rin||pipe_in>=0)  sav0=sh_dup2(0,60);
        if(c->rout||c->rapp||pipe_out>=0) sav1=sh_dup2(1,61);
        if(c->rerr||c->err2out) sav2=sh_dup2(2,62);

        if(pipe_in>=0)  { sh_dup2(pipe_in,0); }
        if(pipe_out>=0) { sh_dup2(pipe_out,1); }
        if(c->rin){int fd=sh_open(c->rin,O_RDONLY,0);if(fd>=0){sh_dup2(fd,0);sh_close(fd);}}
        if(c->rout){int fd=sh_open(c->rout,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){sh_dup2(fd,1);sh_close(fd);}}
        if(c->rapp){int fd=sh_open(c->rapp,O_WRONLY|O_CREAT|O_APPEND,0644);if(fd>=0){sh_dup2(fd,1);sh_close(fd);}}
        if(c->rerr){int fd=sh_open(c->rerr,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){sh_dup2(fd,2);sh_close(fd);}}
        if(c->err2out) sh_dup2(1,2);

        /* Build expanded argv */
        static char ea[MAX_ARGS][MAX_LINE]; char *ep[MAX_ARGS+1];
        int en=0;
        for(int i=0;i<c->argc&&en<MAX_ARGS-1;i++){
            expand_vars(c->argv[i],ea[en],MAX_LINE); ep[en]=ea[en]; en++;
        }
        ep[en]=NULL;

        int r=run_builtin(en,ep);

        if(sav0>=0){sh_dup2(sav0,0);sh_close(sav0);}
        if(sav1>=0){sh_dup2(sav1,1);sh_close(sav1);}
        if(sav2>=0){sh_dup2(sav2,2);sh_close(sav2);}
        return r;
    }

    /* Fork */
    pid_t pid=sh_fork();
    if(pid<0){puts("sh: fork failed");return 1;}

    if(pid==0) {
        /* child */
        if(pipe_in>=0 && pipe_in!=0)  { sh_dup2(pipe_in,0);  sh_close(pipe_in);  }
        if(pipe_out>=0&& pipe_out!=1)  { sh_dup2(pipe_out,1); sh_close(pipe_out); }
        apply_redirs(c);

        static char ea[MAX_ARGS][MAX_LINE]; char *ep[MAX_ARGS+1];
        int en=0;
        for(int i=0;i<c->argc&&en<MAX_ARGS-1;i++){
            expand_vars(c->argv[i],ea[en],MAX_LINE); ep[en]=ea[en]; en++;
        }
        ep[en]=NULL;
        if(!en) exit(0);

        int br=run_builtin(en,ep);
        if(br>=0) exit(br);

        const char *path=find_cmd(ep[0]);
        if(!path){ printf("sh: %s: command not found\n",ep[0]); exit(127); }
        sh_execve(path,(char*const*)ep,(char*const*)g_env);
        printf("sh: exec failed: %s\n",ep[0]); exit(127);
    }

    /* parent */
    if(c->background){ printf("[bg] pid %d\n",(int)pid); return 0; }
    int st=0; sh_wait4(pid,&st,0,NULL);
    g_exit = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128+WTERMSIG(st) : 1;
    return g_exit;
}

/* ================================================================
 * Pipeline: fork ALL stages, plumb pipes, wait all pids.
 * ================================================================ */
static int exec_pipeline(cmd_t *cmds, int n) {
    if(n==0) return 0;
    if(n==1) return exec_cmd(&cmds[0],-1,-1,0);

    pid_t pids[MAX_CMDS]; int npids=0;
    int prev_read=-1;

    for(int i=0;i<n;i++){
        int pfd[2]={-1,-1};
        int next_read=-1;

        if(i<n-1) {
            if(sh_pipe(pfd)<0){
                puts("sh: pipe failed");
                for(int j=0;j<npids;j++) sh_wait4(pids[j],NULL,0,NULL);
                return 1;
            }
            next_read=pfd[0];
        }

        pid_t pid=sh_fork();
        if(pid<0){
            puts("sh: fork failed");
            if(pfd[0]>=0)sh_close(pfd[0]);
            if(pfd[1]>=0)sh_close(pfd[1]);
            if(prev_read>=0)sh_close(prev_read);
            for(int j=0;j<npids;j++) sh_wait4(pids[j],NULL,0,NULL);
            return 1;
        }

        if(pid==0){
            /* child */
            if(prev_read>=0 && prev_read!=0){ sh_dup2(prev_read,0); sh_close(prev_read); }
            if(pfd[1]>=0  && pfd[1]!=1)    { sh_dup2(pfd[1],1);    sh_close(pfd[1]);    }
            if(next_read>=0) sh_close(next_read); /* don't inherit read end */

            apply_redirs(&cmds[i]);

            static char ea[MAX_ARGS][MAX_LINE]; char *ep[MAX_ARGS+1];
            int en=0;
            for(int k=0;k<cmds[i].argc&&en<MAX_ARGS-1;k++){
                expand_vars(cmds[i].argv[k],ea[en],MAX_LINE); ep[en]=ea[en]; en++;
            }
            ep[en]=NULL;
            if(!en) exit(0);

            int br=run_builtin(en,ep);
            if(br>=0) exit(br);

            const char *path=find_cmd(ep[0]);
            if(!path){printf("sh: %s: command not found\n",ep[0]);exit(127);}
            sh_execve(path,(char*const*)ep,(char*const*)g_env);
            printf("sh: exec failed: %s\n",ep[0]); exit(127);
        }

        /* parent: close fds we handed off */
        if(prev_read>=0) { sh_close(prev_read); prev_read=-1; }
        if(pfd[1]>=0)    { sh_close(pfd[1]); }
        prev_read=next_read;
        pids[npids++]=pid;
    }
    if(prev_read>=0) sh_close(prev_read);

    /* wait ALL children; last status is the pipeline status */
    int final=0;
    for(int i=0;i<npids;i++){
        int st=0; sh_wait4(pids[i],&st,0,NULL);
        if(i==npids-1)
            final = WIFEXITED(st)?WEXITSTATUS(st):WIFSIGNALED(st)?128+WTERMSIG(st):1;
    }
    g_exit=final; return final;
}

/* ================================================================
 * Execute a simple (non-flow-control) expanded line.
 * Handles pipelines and && ||.
 * ================================================================ */
static int exec_simple(char *line) {
    while(*line==' '||*line=='\t') line++;
    if(!*line||*line=='#') return g_exit;

    cmd_t cmds[MAX_CMDS];
    memset(cmds,0,sizeof(cmds));
    int nc=tokenize(line,cmds,MAX_CMDS);
    if(nc<=0) return g_exit;

    int result=0, i=0;
    while(i<nc){
        /* Collect a run of | (connector==1) */
        int j=i;
        while(j<nc-1 && cmds[j].connector==1) j++;
        int count=j-i+1;
        result=exec_pipeline(cmds+i, count);
        g_exit=result;
        int op=(j<nc)?cmds[j].connector:0;
        i=j+1;
        if(op==2&&result==0){ /* || and succeeded: skip rest */
            while(i<nc) i++;
        }
        if(op==3&&result!=0){ /* && and failed: skip rest */
            while(i<nc) i++;
        }
    }

    for(int k=0;k<nc;k++) cmd_free(&cmds[k]);
    return result;
}

/* ================================================================
 * sh_run: execute one logical line (may include flow control).
 * ================================================================ */

/* Helpers for extracting body between keywords */
static char *trim_ws(char *s){
    while(*s==' '||*s=='\t') s++;
    int l=(int)strlen(s);
    while(l>0&&(s[l-1]==' '||s[l-1]=='\t'||s[l-1]=='\r'||s[l-1]=='\n'))s[--l]='\0';
    return s;
}

/* Run a ';' / '\n' separated body string */
static int run_body(const char *body) {
    char buf[MAX_LINE*8];
    strncpy(buf,body,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char *p=buf; int ret=0;
    while(*p){
        /* find next ; or \n */
        char *end=p;
        while(*end&&*end!=';'&&*end!='\n') end++;
        char sv=*end; *end='\0';
        char *stmt=trim_ws(p);
        if(*stmt&&*stmt!='#'){ ret=sh_run(stmt); g_exit=ret; }
        if(sv) *end=sv;
        p=(sv)?end+1:end;
        if(g_break||g_continue||g_return) break;
    }
    return ret;
}

static int sh_run(const char *input) {
    /* mutable copy */
    char buf[MAX_LINE*8];
    strncpy(buf,input,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char *line=trim_ws(buf);
    if(!*line||*line=='#') return g_exit;

    /* ---- for VAR in WORDS; do BODY; done ---- */
    if(strncmp(line,"for ",4)==0){
        char *p=line+4; while(*p==' ')p++;
        char var[256]; int vi=0;
        while(*p&&*p!=' '&&*p!='\t'&&*p!='\n'&&*p!=';') var[vi++]=*p++;
        var[vi]='\0';
        while(*p==' '||*p=='\t') p++;
        if(strncmp(p,"in",2)==0) p+=2;
        while(*p==' '||*p=='\t') p++;

        /* Collect words up to ; / 'do' */
        char wraw[MAX_LINE]; int wi=0;
        while(*p&&wi<(int)sizeof(wraw)-1){
            if(*p==';'||(*p=='d'&&*(p+1)=='o'&&(*(p+2)==' '||*(p+2)=='\t'||*(p+2)=='\n'||*(p+2)==';'||!*(p+2)))) break;
            wraw[wi++]=*p++;
        }
        wraw[wi]='\0';
        if(*p==';') p++;
        while(*p==' '||*p=='\t'||*p=='\n') p++;
        if(strncmp(p,"do",2)==0) p+=2;
        while(*p==' '||*p=='\t'||*p=='\n'||*p==';') p++;

        /* Body: everything up to (last) 'done' */
        char body[MAX_LINE*8];
        strncpy(body,p,sizeof(body)-1); body[sizeof(body)-1]='\0';
        {
            char *last=NULL, *d=body;
            while((d=strstr(d,"done"))!=NULL){last=d;d+=4;}
            if(last){
                /* make sure it's a word boundary */
                char after=last[4];
                if(after=='\0'||after==' '||after=='\t'||after=='\n'||after==';')
                    *last='\0';
            }
        }

        /* Expand word list */
        char wexp[MAX_LINE*4];
        expand_vars(wraw,wexp,sizeof(wexp));

        int ret=0; g_break=0; g_continue=0;
        char *w=wexp;
        while(*w){
            while(*w==' '||*w=='\t'||*w=='\n') w++;
            if(!*w) break;
            char *ws=w;
            while(*w&&*w!=' '&&*w!='\t'&&*w!='\n') w++;
            char sv=*w; *w='\0';
            env_set(var,ws); *w=sv;

            ret=run_body(body);
            if(g_break){g_break--;break;}
            if(g_continue){g_continue--;continue;}
            if(g_return) break;
        }
        return ret;
    }

    /* ---- while COND; do BODY; done ---- */
    if(strncmp(line,"while ",6)==0){
        char *p=line+6;
        char cond[MAX_LINE]; int ci=0;
        while(*p&&ci<(int)sizeof(cond)-1){
            if(*p=='d'&&*(p+1)=='o'&&(*(p+2)==' '||*(p+2)=='\t'||*(p+2)=='\n'||*(p+2)==';'||!*(p+2))) break;
            if(*p==';'){ if(ci>0&&cond[ci-1]!=' '){}p++;continue;}
            cond[ci++]=*p++;
        }
        cond[ci]='\0'; trim_ws(cond);
        if(*p==';') p++;
        while(*p==' '||*p=='\t'||*p=='\n') p++;
        if(strncmp(p,"do",2)==0) p+=2;
        while(*p==' '||*p=='\t'||*p=='\n'||*p==';') p++;

        char body[MAX_LINE*8];
        strncpy(body,p,sizeof(body)-1); body[sizeof(body)-1]='\0';
        {
            char *last=NULL,*d=body;
            while((d=strstr(d,"done"))!=NULL){last=d;d+=4;}
            if(last) *last='\0';
        }

        int ret=0; g_break=0; g_continue=0;
        for(;;){
            char cexp[MAX_LINE];
            expand_vars(cond,cexp,sizeof(cexp));
            ret=sh_run(cexp);
            if(ret!=0) break;
            ret=run_body(body);
            if(g_break){g_break--;break;}
            if(g_continue){g_continue--;continue;}
            if(g_return) break;
        }
        return ret;
    }

    /* ---- if COND; then BODY [elif/else] fi ---- */
    if(strncmp(line,"if ",3)==0||strcmp(line,"if")==0){
        char *p=line+3;
        char *then_p=strstr(p,"then");
        if(!then_p){printf("sh: syntax: if without then\n");return 1;}

        char cond[MAX_LINE];
        size_t cl=(size_t)(then_p-p);
        if(cl>=sizeof(cond))cl=sizeof(cond)-1;
        memcpy(cond,p,cl);cond[cl]='\0';
        /* strip trailing semicolons/spaces */
        {int l=(int)strlen(cond);while(l>0&&(cond[l-1]==' '||cond[l-1]==';'||cond[l-1]=='\t'))cond[--l]='\0';}

        char cexp[MAX_LINE]; expand_vars(cond,cexp,sizeof(cexp));
        int cret=sh_run(cexp);

        char *rest=then_p+4;
        while(*rest==' '||*rest=='\t'||*rest=='\n'||*rest==';') rest++;

        /* Find matching fi (not nested) */
        /* For simplicity: find last fi in the string */
        char *fi_p=NULL; { char *d=rest; while((d=strstr(d,"fi"))!=NULL){fi_p=d;d+=2;} }
        char *else_p=strstr(rest,"else");
        char *elif_p=strstr(rest,"elif");

        char *alt=NULL;
        if(elif_p&&(!else_p||elif_p<else_p)&&(!fi_p||elif_p<fi_p)) alt=elif_p;
        else if(else_p&&(!fi_p||else_p<fi_p)) alt=else_p;

        int ret=0;
        if(cret==0){
            /* run then-body */
            char body[MAX_LINE*8];
            size_t bl=(size_t)((alt?alt:fi_p)-rest);
            if(bl>=sizeof(body))bl=sizeof(body)-1;
            memcpy(body,rest,bl);body[bl]='\0';
            ret=run_body(body);
        } else if(alt){
            if(strncmp(alt,"elif",4)==0){
                char el[MAX_LINE*8];
                snprintf(el,sizeof(el),"if %s",alt+5);
                ret=sh_run(el);
            } else {
                char *eb=alt+4;
                while(*eb==' '||*eb=='\t'||*eb=='\n'||*eb==';') eb++;
                char body[MAX_LINE*8];
                size_t bl=fi_p?(size_t)(fi_p-eb):strlen(eb);
                if(bl>=sizeof(body))bl=sizeof(body)-1;
                memcpy(body,eb,bl);body[bl]='\0';
                ret=run_body(body);
            }
        }
        g_exit=ret; return ret;
    }

    /* ---- simple command / pipeline ---- */
    char expanded[MAX_LINE*4];
    expand_vars(line,expanded,sizeof(expanded));

    /* Split on ; separators (not inside quotes/pipes) */
    /* Fast path: no semicolons → just exec */
    if(!strchr(expanded,';')){
        return exec_simple(expanded);
    }

    /* Split manually respecting quotes */
    char *ep=expanded; int ret=0;
    while(*ep){
        char seg[MAX_LINE]; int si=0;
        int qi=0,qd=0;
        while(*ep&&si<(int)sizeof(seg)-1){
            char c=*ep;
            if(c=='\''&&!qd){qi=!qi;seg[si++]=c;ep++;continue;}
            if(c=='"'&&!qi){qd=!qd;seg[si++]=c;ep++;continue;}
            if(!qi&&!qd&&c==';'){ep++;break;}
            seg[si++]=c; ep++;
        }
        seg[si]='\0';
        char *st=trim_ws(seg);
        if(*st) { ret=exec_simple(st); g_exit=ret; }
        if(g_break||g_continue||g_return) break;
    }
    return ret;
}

/* ================================================================
 * Script: read from fd, collect multi-line constructs, execute
 * ================================================================ */
static int run_script_fd(int fd) {
    /* Buffer for accumulating compound statements */
    char line[MAX_LINE];
    char compound[MAX_LINE*16]; int clen=0;
    int  need_end=0; /* depth of unclosed for/while/if */
    int  ret=0;

    for(;;){
        /* Read one line */
        int n=readline(fd,line,sizeof(line));
        if(n<=0){
            if(clen>0){
                compound[clen]='\0';
                ret=sh_run(compound);
            }
            break;
        }
        /* strip \n */
        int l=(int)strlen(line);
        while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';

        /* skip empty / comment at top level */
        char *tl=trim_ws(line);
        if(!*tl){
            if(need_end==0) continue;
            /* still accumulate blank lines inside block */
        }

        /* Track block depth */
        if(strncmp(tl,"for ",4)==0||strncmp(tl,"while ",6)==0||strncmp(tl,"if ",3)==0)
            need_end++;
        if((strcmp(tl,"done")==0||strncmp(tl,"done ",5)==0||strcmp(tl,"fi")==0)&&need_end>0)
            need_end--;

        /* Append to compound buffer with ';' separator */
        size_t tl_len=strlen(tl);
        if(clen+1+(int)tl_len+1 < (int)sizeof(compound)){
            if(clen>0) compound[clen++]=';';
            memcpy(compound+clen,tl,tl_len);
            clen+=(int)tl_len;
            compound[clen]='\0';
        }

        /* Execute when block is closed or single line */
        if(need_end==0&&clen>0){
            compound[clen]='\0';
            ret=sh_run(compound);
            g_exit=ret; clen=0;
            if(g_break||g_continue||g_return) break;
        }
    }
    return ret;
}

/* ================================================================
 * Interactive readline with history and arrow keys
 * ================================================================ */
static char g_ibuf[MAX_LINE]; static int g_ilen=0;
static int  g_hpos=0;
static char g_saved[MAX_LINE];

static char *hist_get(int back){
    if(back<1||back>g_hist_cnt) return NULL;
    int idx=(g_hist_head-back+HIST_SIZE*4)%HIST_SIZE;
    return g_hist[idx];
}

static void hist_add(const char *l){
    if(!l||!*l) return;
    int last=(g_hist_head-1+HIST_SIZE)%HIST_SIZE;
    if(g_hist_cnt>0&&g_hist[last]&&strcmp(g_hist[last],l)==0) return;
    free(g_hist[g_hist_head%HIST_SIZE]);
    g_hist[g_hist_head%HIST_SIZE]=strdup(l);
    g_hist_head=(g_hist_head+1)%HIST_SIZE;
    if(g_hist_cnt<HIST_SIZE) g_hist_cnt++;
}

static int ireadline(char *out, int maxlen){
    g_ilen=0; g_hpos=0; g_ibuf[0]='\0'; g_saved[0]='\0';
    for(;;){
        unsigned char c;
        long r=sh_read(0,&c,1);
        if(r<=0) return -1;
        if(c=='\r'||c=='\n'){
            sh_write(1,"\n",1);
            g_ibuf[g_ilen]='\0';
            strncpy(out,g_ibuf,(size_t)(maxlen-1)); out[maxlen-1]='\0';
            return g_ilen;
        }
        if(c==0x03){sh_write(1,"^C\n",3);g_ibuf[0]='\0';g_ilen=0;return 0;}
        if(c==0x04&&g_ilen==0) return -1;
        if(c==0x0C){sh_write(1,"\033[2J\033[H",7);return 0;}
        if(c==0x08||c==0x7F){
            if(g_ilen>0){g_ilen--;g_ibuf[g_ilen]='\0';sh_write(1,"\b \b",3);}
            continue;
        }
        if(c==0x1B){
            unsigned char sq[2];
            sh_read(0,&sq[0],1); sh_read(0,&sq[1],1);
            if(sq[0]=='['){
                if(sq[1]=='A'){ /* up */
                    if(g_hpos==0) strncpy(g_saved,g_ibuf,sizeof(g_saved)-1);
                    if(g_hpos<g_hist_cnt){
                        g_hpos++;
                        char *h=hist_get(g_hpos);
                        if(h){
                            for(int k=0;k<g_ilen;k++) sh_write(1,"\b \b",3);
                            strncpy(g_ibuf,h,sizeof(g_ibuf)-1);
                            g_ilen=(int)strlen(g_ibuf);
                            sh_write(1,g_ibuf,(size_t)g_ilen);
                        }
                    }
                } else if(sq[1]=='B'){ /* down */
                    if(g_hpos>0){
                        g_hpos--;
                        for(int k=0;k<g_ilen;k++) sh_write(1,"\b \b",3);
                        char *h=g_hpos==0?g_saved:hist_get(g_hpos);
                        if(h){strncpy(g_ibuf,h,sizeof(g_ibuf)-1);g_ilen=(int)strlen(g_ibuf);sh_write(1,g_ibuf,(size_t)g_ilen);}
                    }
                }
            }
            continue;
        }
        if(c>=' '&&g_ilen<maxlen-1){g_ibuf[g_ilen++]=(char)c;g_ibuf[g_ilen]='\0';sh_write(1,&c,1);}
    }
}

/* ================================================================
 * Prompt
 * ================================================================ */
static void print_prompt(void){
    sh_getcwd(g_cwd,sizeof(g_cwd));
    if(!g_cwd[0]) strcpy(g_cwd,"/");

    char *ps1=getenv("PS1");
    if(ps1&&*ps1){
        char pb[512]; int pi=0;
        for(char *s=ps1;*s&&pi<500;){
            if(*s=='\\'&&*(s+1)){
                s++;
                switch(*s){
                case 'w':{char *home=getenv("HOME");
                    if(home&&strncmp(g_cwd,home,strlen(home))==0&&(g_cwd[strlen(home)]=='/'||g_cwd[strlen(home)]=='\0')){
                        pb[pi++]='~';char *r=g_cwd+strlen(home);while(*r&&pi<500)pb[pi++]=*r++;}
                    else{char *c=g_cwd;while(*c&&pi<500)pb[pi++]=*c++;}break;}
                case 'u':{char *u=getenv("USER");if(!u)u="root";while(*u&&pi<500)pb[pi++]=*u++;break;}
                case 'h':{const char *h="picomimi";while(*h&&pi<500)pb[pi++]=(char)(*h++);break;}
                case '$':pb[pi++]='#';break;
                case 'n':pb[pi++]='\n';break;
                case 'e':pb[pi++]='\033';break;
                case '[':case ']':break;
                default:pb[pi++]='\\';pb[pi++]=*s;break;}
                s++;
            } else pb[pi++]=*s++;
        }
        pb[pi]='\0'; sh_write(1,pb,(size_t)pi); return;
    }

    char *home=getenv("HOME");
    const char *disp=g_cwd; char shrt[PATH_MAX];
    if(home&&strncmp(g_cwd,home,strlen(home))==0&&(g_cwd[strlen(home)]=='/'||g_cwd[strlen(home)]=='\0')){
        snprintf(shrt,sizeof(shrt),"~%s",g_cwd+strlen(home)); disp=shrt;
    }
    printf(CG "root@picomimi" CR ":" CB "%s" CR "# ", disp);
}

/* ================================================================
 * main
 * ================================================================ */
static char g_line[MAX_LINE];

int main(int argc, char **argv, char **envp){
    g_pid=sh_getpid();

    /* Init env */
    int i=0;
    if(envp) for(;envp[i]&&i<MAX_ENV-1;i++) g_env[i]=strdup(envp[i]);
    g_env[i]=NULL; g_nenv=i; __set_environ(g_env);

    strncpy(g_argv0,argc>0&&argv[0]?argv[0]:"/bin/sh",sizeof(g_argv0)-1);
    g_npos=0;
    for(int j=2;j<argc&&g_npos<MAX_ARGS;j++) g_pos[g_npos++]=argv[j];

    if(!getenv("PATH"))  env_set("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin");
    if(!getenv("HOME"))  env_set("HOME", "/root");
    if(!getenv("TERM"))  env_set("TERM", "xterm-256color");
    if(!getenv("SHELL")) env_set("SHELL","/bin/sh");
    if(!getenv("USER"))  env_set("USER", "root");

    sh_getcwd(g_cwd,sizeof(g_cwd)); if(!g_cwd[0]) strcpy(g_cwd,"/");
    env_set("PWD",g_cwd);

    /* -c 'cmd' */
    if(argc>=3&&strcmp(argv[1],"-c")==0){ g_exit=sh_run(argv[2]); exit(g_exit); }

    /* script file */
    if(argc>=2&&argv[1][0]!='-'){
        int fd=sh_open(argv[1],O_RDONLY,0);
        if(fd<0){printf("sh: cannot open '%s'\n",argv[1]);exit(1);}
        g_exit=run_script_fd(fd); sh_close(fd); exit(g_exit);
    }

    /* -s stdin script */
    if(argc>=2&&strcmp(argv[1],"-s")==0){ g_exit=run_script_fd(0); exit(g_exit); }

    /* Interactive */
    printf(CC CBD
        "\n  ██████╗ ██╗ ██████╗ ██████╗ ███╗   ███╗██╗███╗   ███╗██╗\n"
        "  ██╔══██╗██║██╔════╝██╔═══██╗████╗ ████║██║████╗ ████║██║\n"
        "  ██████╔╝██║██║     ██║   ██║██╔████╔██║██║██╔████╔██║██║\n"
        "  ██╔═══╝ ██║██║     ██║   ██║██║╚██╔╝██║██║██║╚██╔╝██║██║\n"
        "  ██║     ██║╚██████╗╚██████╔╝██║ ╚═╝ ██║██║██║ ╚═╝ ██║██║\n"
        "  ╚═╝     ╚═╝ ╚═════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝╚═╝     ╚═╝╚═╝\n"
        CR);
    printf(CY "  Picomimi OS — Shell v2.0  (type 'help' for info)\n\n" CR);

    /* source /etc/profile */
    { int pfd=sh_open("/etc/profile",O_RDONLY,0); if(pfd>=0){run_script_fd(pfd);sh_close(pfd);} }

    /* REPL */
    for(;;){
        print_prompt();
        int n=ireadline(g_line,sizeof(g_line));
        if(n<0){ printf("\n"); break; }
        if(n==0) continue;
        hist_add(g_line);

        /* Backslash line continuation */
        char full[MAX_LINE*8]; int flen=(int)strlen(g_line);
        strncpy(full,g_line,sizeof(full)-1); full[sizeof(full)-1]='\0';
        while(flen>0&&full[flen-1]=='\\'){
            full[--flen]='\0';
            sh_write(1,"> ",2);
            n=ireadline(g_line,sizeof(g_line));
            if(n<0) break;
            if(flen+(int)strlen(g_line)<(int)sizeof(full)-1){
                memcpy(full+flen,g_line,strlen(g_line)+1);
                flen=(int)strlen(full);
            }
        }

        /* Multi-line block accumulation */
        char *tr=trim_ws(full);
        int need=0;
        if(strncmp(tr,"for ",4)==0&&!strstr(tr,"done")) need=1;
        if(strncmp(tr,"while ",6)==0&&!strstr(tr,"done")) need=1;
        if(strncmp(tr,"if ",3)==0&&!strstr(tr,"fi")) need=1;

        while(need){
            sh_write(1,"> ",2);
            n=ireadline(g_line,sizeof(g_line));
            if(n<0) break;
            if(flen+(int)strlen(g_line)+2<(int)sizeof(full)-1){
                full[flen++]=';';
                memcpy(full+flen,g_line,strlen(g_line)+1);
                flen=(int)strlen(full);
            }
            tr=trim_ws(full);
            if(strncmp(tr,"for ",4)==0&&strstr(tr,"done"))  need=0;
            if(strncmp(tr,"while ",6)==0&&strstr(tr,"done")) need=0;
            if(strncmp(tr,"if ",3)==0&&strstr(tr,"fi"))      need=0;
        }

        g_exit=sh_run(full);
    }
    exit(g_exit);
}
