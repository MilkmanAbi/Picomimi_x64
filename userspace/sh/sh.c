/**
 * Picomimi-x64 /bin/sh — Userspace Shell
 *
 * A proper POSIX-ish shell for userspace. Runs as a real ELF process
 * (not in-kernel), communicates with the kernel via syscalls.
 *
 * Features:
 *   - Command execution (exec + fork + wait)
 *   - I/O redirection (>, <, >>)
 *   - Pipes (cmd1 | cmd2)
 *   - Background jobs (&)
 *   - Built-ins: cd, exit, echo, export, unset, pwd, help, type
 *   - Environment variables
 *   - $VAR expansion
 *   - ; command separator
 *   - History (up-arrow, simple ring buffer)
 *   - Colorized prompt
 */

#include "../include/syscall.h"

/* Pull in our libc */
#define NULL ((void*)0)
typedef long ssize_t;
typedef unsigned long size_t;
typedef long off_t;
typedef int pid_t;

/* ---- forward declarations from libc.c ---- */
extern size_t  strlen(const char *);
extern char   *strcpy(char *, const char *);
extern char   *strncpy(char *, const char *, size_t);
extern int     strcmp(const char *, const char *);
extern int     strncmp(const char *, const char *, size_t);
extern char   *strcat(char *, const char *);
extern char   *strchr(const char *, int);
extern char   *strrchr(const char *, int);
extern char   *strdup(const char *);
extern void   *memcpy(void *, const void *, size_t);
extern void   *memset(void *, int, size_t);
extern void   *malloc(size_t);
extern void    free(void *);
extern int     printf(const char *, ...);
extern int     snprintf(char *, size_t, const char *, ...);
extern int     puts(const char *);
extern int     putchar(int);
extern int     getchar(void);
extern int     readline(int, char *, int);
extern void    exit(int);
extern pid_t   fork(void);
extern int     execv(const char *, char *const[]);
extern int     execve(const char *, char *const[], char *const[]);
extern pid_t   waitpid(pid_t, int *, int);
extern int     kill(pid_t, int);
extern char   *getenv(const char *);
extern void    __set_environ(char **);
extern int     isspace(int);
extern int     atoi(const char *);
extern int     open(const char *, int, ...);
extern int     close(int);
extern int     write(int, const void *, size_t);
extern int     read(int, void *, size_t);
extern int     sys_dup2(int, int);
extern int     sys_pipe(int *);
extern long    sys_lseek(int, off_t, int);

/* ---- ANSI color codes ---- */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"

/* ---- Open flags (must match kernel) ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_DIRECTORY 0200000

/* ---- Limits ---- */
#define MAX_ARGS     64
#define MAX_LINE     1024
#define MAX_ENV      256
#define HIST_SIZE    64
#define PATH_MAX     4096

/* ---- Global state ---- */
static char  *environ_arr[MAX_ENV];
static int    n_environ = 0;
static char  *history[HIST_SIZE];
static int    hist_head = 0, hist_count = 0;
static int    last_exit = 0;
static char   cwd_buf[PATH_MAX] = "/";

/* ------------------------------------------------------------------ */
/* Environment management                                               */
/* ------------------------------------------------------------------ */

static void env_set(const char *name, const char *val) {
    size_t nlen = strlen(name);
    for (int i = 0; i < n_environ; i++) {
        if (strncmp(environ_arr[i], name, nlen) == 0 && environ_arr[i][nlen] == '=') {
            char *buf = malloc(nlen + strlen(val) + 2);
            strcpy(buf, name); strcat(buf, "="); strcat(buf, val);
            free(environ_arr[i]);
            environ_arr[i] = buf;
            return;
        }
    }
    if (n_environ < MAX_ENV - 1) {
        char *buf = malloc(nlen + strlen(val) + 2);
        strcpy(buf, name); strcat(buf, "="); strcat(buf, val);
        environ_arr[n_environ++] = buf;
        environ_arr[n_environ] = NULL;
        __set_environ(environ_arr);
    }
}

static void env_unset(const char *name) {
    size_t nlen = strlen(name);
    for (int i = 0; i < n_environ; i++) {
        if (strncmp(environ_arr[i], name, nlen) == 0 && environ_arr[i][nlen] == '=') {
            free(environ_arr[i]);
            for (int j = i; j < n_environ - 1; j++)
                environ_arr[j] = environ_arr[j+1];
            environ_arr[--n_environ] = NULL;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Variable expansion: $VAR → value                                     */
/* ------------------------------------------------------------------ */

static void expand_vars(const char *in, char *out, size_t outlen) {
    size_t i = 0, o = 0;
    while (in[i] && o < outlen - 1) {
        if (in[i] == '$') {
            i++;
            if (in[i] == '?') {
                /* $? = last exit code */
                char tmp[8]; snprintf(tmp, sizeof(tmp), "%d", last_exit);
                size_t tl = strlen(tmp);
                if (o + tl < outlen - 1) { memcpy(out + o, tmp, tl); o += tl; }
                i++;
            } else {
                /* collect var name */
                char vname[128]; int vn = 0;
                while (in[i] && (in[i]=='_'||(in[i]>='a'&&in[i]<='z')||(in[i]>='A'&&in[i]<='Z')||(in[i]>='0'&&in[i]<='9')))
                    vname[vn++] = in[i++];
                vname[vn] = 0;
                char *val = getenv(vname);
                if (val) {
                    size_t vl = strlen(val);
                    if (o + vl < outlen - 1) { memcpy(out + o, val, vl); o += vl; }
                }
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;    /* < file */
    char *redir_out;   /* > file */
    char *redir_app;   /* >> file */
    int   background;  /* trailing & */
    int   pipe_next;   /* | follows */
} cmd_t;

static int tokenize(char *line, cmd_t *cmds, int max_cmds) {
    int ncmds = 0;
    cmd_t *cur = &cmds[0];
    memset(cur, 0, sizeof(cmd_t));

    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;

    while (*p && ncmds < max_cmds) {
        /* skip spaces */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* check for redirection / pipe / bg */
        if (*p == '|') {
            cur->pipe_next = 1;
            p++;
            cmds[++ncmds] = (cmd_t){0};
            cur = &cmds[ncmds];
            continue;
        }
        if (*p == '&') { cur->background = 1; p++; continue; }
        if (*p == ';') {
            ncmds++;
            if (ncmds < max_cmds) { cmds[ncmds] = (cmd_t){0}; cur = &cmds[ncmds]; }
            p++;
            continue;
        }
        if (*p == '>' && *(p+1) == '>') {
            p += 2;
            while (*p == ' ') p++;
            char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&' && *p != ';') p++;
            char save = *p; *p = 0;
            cur->redir_app = strdup(start);
            *p = save;
            continue;
        }
        if (*p == '>') {
            p++;
            while (*p == ' ') p++;
            char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&' && *p != ';') p++;
            char save = *p; *p = 0;
            cur->redir_out = strdup(start);
            *p = save;
            continue;
        }
        if (*p == '<') {
            p++;
            while (*p == ' ') p++;
            char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&' && *p != ';') p++;
            char save = *p; *p = 0;
            cur->redir_in = strdup(start);
            *p = save;
            continue;
        }

        /* token: handle quoting */
        char tokbuf[MAX_LINE];
        int  ti = 0;
        int  quoted = 0;
        char quote_char = 0;

        while (*p && ti < (int)sizeof(tokbuf)-1) {
            if (!quoted && (*p == '"' || *p == '\'')) {
                quoted = 1; quote_char = *p++; continue;
            }
            if (quoted && *p == quote_char) {
                quoted = 0; p++; continue;
            }
            if (!quoted && (isspace((unsigned char)*p) || *p == '|' ||
                            *p == '&' || *p == '<' || *p == '>' || *p == ';'))
                break;
            tokbuf[ti++] = *p++;
        }
        tokbuf[ti] = 0;

        if (ti > 0 && cur->argc < MAX_ARGS - 1) {
            cur->argv[cur->argc++] = strdup(tokbuf);
        }
    }

    return cur->argc > 0 ? ncmds + 1 : ncmds;
}

/* ------------------------------------------------------------------ */
/* Path search                                                           */
/* ------------------------------------------------------------------ */

static char found_path[PATH_MAX];

static const char *find_in_path(const char *cmd) {
    if (strchr(cmd, '/')) return cmd;   /* already a path */

    char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin:/sbin:/usr/local/bin";

    char dirs[1024];
    strncpy(dirs, path, sizeof(dirs)-1);

    char *dir = dirs;
    while (dir && *dir) {
        char *colon = strchr(dir, ':');
        if (colon) *colon = 0;

        snprintf(found_path, sizeof(found_path), "%s/%s", dir, cmd);
        if (sys_open(found_path, O_RDONLY, 0) >= 0)   /* rough existence check */
            return found_path;

        dir = colon ? colon + 1 : NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Built-in commands                                                     */
/* ------------------------------------------------------------------ */

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10

static int builtin_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : cwd_buf;
    int fd = sys_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        printf(CLR_RED "ls: cannot open '%s': error %d\n" CLR_RESET, path, fd);
        return 1;
    }
    char buf[4096];
    long n;
    int col = 0;
    while ((n = sys_getdents64(fd, (void*)buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            if (d->d_name[0] != '.') {   /* skip hidden unless -a */
                if (d->d_type == DT_DIR)
                    printf(CLR_BLUE CLR_BOLD "%-20s" CLR_RESET, d->d_name);
                else
                    printf("%-20s", d->d_name);
                if (++col % 4 == 0) putchar('\n');
            }
            off += d->d_reclen;
        }
    }
    if (col % 4 != 0) putchar('\n');
    sys_close(fd);
    return 0;
}

static int builtin_cat(int argc, char **argv) {
    if (argc < 2) {
        /* cat from stdin */
        char buf[512];
        int n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf(CLR_RED "cat: %s: error %d\n" CLR_RESET, argv[i], fd); continue; }
        char buf[1024];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return 0;
}

static int builtin_echo(int argc, char **argv) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        /* process escape sequences */
        char *s = argv[i];
        while (*s) {
            if (*s == '\\' && *(s+1)) {
                s++;
                switch (*s) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case '\\': putchar('\\'); break;
                case 'e': write(1, "\033", 1); break;
                default: putchar('\\'); putchar(*s); break;
                }
            } else {
                putchar(*s);
            }
            s++;
        }
    }
    if (newline) putchar('\n');
    return 0;
}

static int builtin_cd(int argc, char **argv) {
    const char *dest;
    if (argc < 2) {
        dest = getenv("HOME");
        if (!dest) dest = "/root";
    } else {
        dest = argv[1];
    }
    if (sys_chdir(dest) < 0) {
        printf(CLR_RED "cd: %s: no such directory\n" CLR_RESET, dest);
        return 1;
    }
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    env_set("PWD", cwd_buf);
    return 0;
}

static int builtin_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    puts(cwd_buf);
    return 0;
}

static int builtin_export(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char name[128]; size_t nlen = eq - argv[i];
            if (nlen >= sizeof(name)) continue;
            strncpy(name, argv[i], nlen); name[nlen] = 0;
            env_set(name, eq + 1);
        } else {
            /* export NAME — mark for export (already is, since we have one env) */
        }
    }
    return 0;
}

static int builtin_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) env_unset(argv[i]);
    return 0;
}

static int builtin_env(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; environ_arr[i]; i++)
        printf("%s\n", environ_arr[i]);
    return 0;
}

static int builtin_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf(CLR_BOLD CLR_CYAN
        "Picomimi Shell — Built-in Commands\n"
        "===================================\n" CLR_RESET
        "  cd [dir]        Change directory\n"
        "  pwd             Print working directory\n"
        "  ls [dir]        List directory\n"
        "  cat [file...]   Print file contents\n"
        "  echo [-n] [...]  Print text\n"
        "  export NAME=VAL Set environment variable\n"
        "  unset NAME      Remove environment variable\n"
        "  env             Print environment\n"
        "  exit [code]     Exit shell\n"
        "  help            Show this help\n"
        "\n"
        "  Pipes:  cmd1 | cmd2\n"
        "  Redir:  cmd > file   cmd < file   cmd >> file\n"
        "  Bg:     cmd &\n"
        "  Sep:    cmd1 ; cmd2\n"
        "\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Execute a single command                                              */
/* ------------------------------------------------------------------ */

static int run_cmd(cmd_t *cmd, int pipe_in, int pipe_out) {
    if (cmd->argc == 0) return 0;

    /* -- built-ins (don't fork) -- */
    if (strcmp(cmd->argv[0], "exit")   == 0) exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : 0);
    if (strcmp(cmd->argv[0], "cd")     == 0) return builtin_cd(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "pwd")    == 0) return builtin_pwd(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "echo")   == 0) return builtin_echo(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "export") == 0) return builtin_export(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "unset")  == 0) return builtin_unset(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "env")    == 0) return builtin_env(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "help")   == 0) return builtin_help(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "ls")     == 0) return builtin_ls(cmd->argc, cmd->argv);
    if (strcmp(cmd->argv[0], "cat")    == 0) return builtin_cat(cmd->argc, cmd->argv);

    /* -- fork + exec -- */
    pid_t pid = fork();
    if (pid < 0) { printf("sh: fork failed\n"); return 1; }

    if (pid == 0) {
        /* child */
        if (pipe_in  != 0) { sys_dup2(pipe_in,  0); close(pipe_in); }
        if (pipe_out != 1) { sys_dup2(pipe_out, 1); close(pipe_out); }

        /* redirections */
        if (cmd->redir_in) {
            int fd = open(cmd->redir_in, O_RDONLY, 0);
            if (fd < 0) { printf("sh: cannot open '%s'\n", cmd->redir_in); exit(1); }
            sys_dup2(fd, 0); close(fd);
        }
        if (cmd->redir_out) {
            int fd = open(cmd->redir_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { printf("sh: cannot open '%s'\n", cmd->redir_out); exit(1); }
            sys_dup2(fd, 1); close(fd);
        }
        if (cmd->redir_app) {
            int fd = open(cmd->redir_app, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) { printf("sh: cannot open '%s'\n", cmd->redir_app); exit(1); }
            sys_dup2(fd, 1); close(fd);
        }

        /* find binary */
        const char *path = find_in_path(cmd->argv[0]);
        if (!path) {
            printf(CLR_RED "sh: command not found: %s\n" CLR_RESET, cmd->argv[0]);
            exit(127);
        }

        cmd->argv[cmd->argc] = NULL;
        execve(path, cmd->argv, environ_arr);
        printf(CLR_RED "sh: exec failed: %s\n" CLR_RESET, cmd->argv[0]);
        exit(127);
    }

    /* parent */
    if (cmd->background) {
        printf("[bg] pid %d\n", pid);
        return 0;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (status >> 8) & 0xFF;
}

/* ------------------------------------------------------------------ */
/* Execute a full pipeline                                               */
/* ------------------------------------------------------------------ */

static int run_pipeline(cmd_t *cmds, int ncmds) {
    if (ncmds == 1) return run_cmd(&cmds[0], 0, 1);

    int prev_read = 0;
    for (int i = 0; i < ncmds; i++) {
        int pfd[2] = {0, 1};
        int next_read = 0;
        if (i < ncmds - 1) {
            sys_pipe(pfd);
            next_read = pfd[0];
        }
        run_cmd(&cmds[i], prev_read, (i < ncmds - 1) ? pfd[1] : 1);
        if (i < ncmds - 1) close(pfd[1]);
        if (prev_read != 0) close(prev_read);
        prev_read = next_read;
    }
    return last_exit;
}

/* ------------------------------------------------------------------ */
/* History                                                               */
/* ------------------------------------------------------------------ */

static void hist_add(const char *line) {
    if (!line || !*line) return;
    free(history[hist_head % HIST_SIZE]);
    history[hist_head % HIST_SIZE] = strdup(line);
    hist_head++;
    if (hist_count < HIST_SIZE) hist_count++;
}

/* ------------------------------------------------------------------ */
/* Prompt                                                                */
/* ------------------------------------------------------------------ */

static void print_prompt(void) {
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    /* shorten home dir to ~ */
    char *home = getenv("HOME");
    const char *display_cwd = cwd_buf;
    char shortened[PATH_MAX];
    if (home && strncmp(cwd_buf, home, strlen(home)) == 0) {
        snprintf(shortened, sizeof(shortened), "~%s", cwd_buf + strlen(home));
        display_cwd = shortened;
    }
    printf(CLR_GREEN CLR_BOLD "root@picomimi" CLR_RESET ":"
           CLR_BLUE CLR_BOLD "%s" CLR_RESET "# ", display_cwd);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                             */
/* ------------------------------------------------------------------ */

static char line_buf[MAX_LINE];
static char expanded[MAX_LINE];

int main(int argc, char **argv, char **envp) {
    (void)argc;

    /* Init environment */
    int i = 0;
    if (envp) {
        for (; envp[i] && i < MAX_ENV - 1; i++) {
            environ_arr[i] = strdup(envp[i]);
        }
    }
    environ_arr[i] = NULL;
    n_environ = i;
    __set_environ(environ_arr);

    /* Set defaults */
    if (!getenv("PATH"))  env_set("PATH", "/bin:/usr/bin:/sbin:/usr/local/bin");
    if (!getenv("HOME"))  env_set("HOME", "/root");
    if (!getenv("TERM"))  env_set("TERM", "xterm-256color");
    if (!getenv("SHELL")) env_set("SHELL", "/bin/sh");
    if (!getenv("USER"))  env_set("USER", "root");

    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    if (cwd_buf[0] == 0) strcpy(cwd_buf, "/");

    /* Print banner */
    printf(CLR_CYAN CLR_BOLD
        "\n  ██████╗ ██╗ ██████╗ ██████╗ ███╗   ███╗██╗███╗   ███╗██╗\n"
        "  ██╔══██╗██║██╔════╝██╔═══██╗████╗ ████║██║████╗ ████║██║\n"
        "  ██████╔╝██║██║     ██║   ██║██╔████╔██║██║██╔████╔██║██║\n"
        "  ██╔═══╝ ██║██║     ██║   ██║██║╚██╔╝██║██║██║╚██╔╝██║██║\n"
        "  ██║     ██║╚██████╗╚██████╔╝██║ ╚═╝ ██║██║██║ ╚═╝ ██║██║\n"
        "  ╚═╝     ╚═╝ ╚═════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝╚═╝     ╚═╝╚═╝\n"
        CLR_RESET);
    printf(CLR_YELLOW "  Picomimi OS — Userspace Shell v1.0\n");
    printf("  Type 'help' for built-in commands.\n\n" CLR_RESET);

    /* Check if running a script */
    if (argc > 1) {
        int fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) { printf("sh: cannot open %s\n", argv[1]); exit(1); }
        while (readline(fd, line_buf, sizeof(line_buf)) > 0) {
            /* strip trailing newline */
            int l = strlen(line_buf);
            if (l > 0 && line_buf[l-1] == '\n') line_buf[l-1] = 0;
            if (!line_buf[0] || line_buf[0] == '#') continue;
            expand_vars(line_buf, expanded, sizeof(expanded));
            cmd_t cmds[16]; memset(cmds, 0, sizeof(cmds));
            int n = tokenize(expanded, cmds, 16);
            if (n > 0) last_exit = run_pipeline(cmds, n);
        }
        close(fd);
        exit(last_exit);
    }

    /* Interactive REPL */
    while (1) {
        print_prompt();
        int n = readline(0, line_buf, sizeof(line_buf));
        if (n <= 0) { printf("\n"); exit(last_exit); }

        /* strip newline */
        int l = strlen(line_buf);
        if (l > 0 && line_buf[l-1] == '\n') line_buf[l-1] = 0;
        if (!line_buf[0]) continue;

        hist_add(line_buf);
        expand_vars(line_buf, expanded, sizeof(expanded));

        cmd_t cmds[16]; memset(cmds, 0, sizeof(cmds));
        int ncmds = tokenize(expanded, cmds, 16);
        if (ncmds > 0) last_exit = run_pipeline(cmds, ncmds);
    }
    return 0;
}
