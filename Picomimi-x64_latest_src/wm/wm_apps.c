/**
 * wm_apps.c — Terminal, Sysmon, About
 *
 * Terminal design principles (complete rewrite):
 *   - SIMPLE: one flat char buffer, no complex state
 *   - Scrollback is a ring of fixed-width lines
 *   - Input line drawn SEPARATELY at fixed bottom position
 *   - WF_DIRTY managed by wm_tick blink timer, not self-loop
 *   - No clip dependency between chrome and content
 *   - Ctrl+C via ascii 0x03, Ctrl+L via 0x0C (correct keycodes)
 *   - Bold font simulation: draw char twice (x, x+1)
 *   - Word-wrap aware column calculation
 */
#include "wm_internal.h"

extern void printk_set_hook(void (*fn)(const char *s));
extern void ksh_execute_line(const char *line);
extern volatile u64 system_ticks;

/* ═══════════════════════════════════════════════════════════
 * TERMINAL
 * ═══════════════════════════════════════════════════════════ */

#define T_MAXLINES  512      /* ring buffer lines             */
#define T_LINECOLS  512      /* chars per scrollback line     */
#define T_IBUF      512      /* input buffer size             */
#define T_HIST      64       /* history depth                 */
#define T_FONT_W    FB_FONT_W
#define T_FONT_H    FB_FONT_H
#define T_PAD_L     8        /* left content padding          */
#define T_PAD_T     4        /* top content padding           */
#define T_INPUT_H   (T_FONT_H + 8)  /* input area height      */
#define T_INPUT_SEP 1        /* separator line height         */

/* Line colour tags (first byte of line if >= 0x01 and < 0x20) */
#define TC_NORMAL   0    /* default fg                    */
#define TC_OUTPUT   1    /* command output (dim)          */
#define TC_ERR      2    /* error / stderr                */
#define TC_PROMPT   3    /* prompt echo colour            */
#define TC_INFO     4    /* system info (cyan)            */

typedef struct {
    /* Scrollback ring */
    char  sb[T_MAXLINES][T_LINECOLS];  /* line text, null-term */
    u8    sb_col[T_MAXLINES];          /* TC_* colour tag      */
    int   sb_head;    /* index of oldest line              */
    int   sb_count;   /* total lines in ring (<=T_MAXLINES) */
    int   sb_scroll;  /* lines scrolled up from bottom     */

    /* Input */
    char  ibuf[T_IBUF];
    int   ilen;       /* length                            */
    int   icur;       /* cursor pos                        */

    /* History */
    char  hist[T_HIST][T_IBUF];
    int   hist_n;
    int   hist_pos;   /* -1 = fresh                        */

    /* Metrics (set on each paint) */
    int   cols;       /* chars that fit per line           */
    int   vis_rows;   /* visible scrollback rows           */

    /* Blink */
    u64   blink_last; /* last blink toggle tick            */
    int   blink_on;

    /* Capture active */
    int   capturing;
} term_t;

static term_t *g_cap_term = NULL;  /* term receiving printk */

/* ── scrollback line access ─────────────────────────────── */
static int sb_idx(term_t *t, int n) {
    /* n=0 = oldest. n=sb_count-1 = newest */
    return (t->sb_head + n) % T_MAXLINES;
}

static void sb_newline(term_t *t, u8 col) {
    int idx;
    if (t->sb_count < T_MAXLINES) {
        idx = (t->sb_head + t->sb_count) % T_MAXLINES;
        t->sb_count++;
    } else {
        /* Overwrite oldest */
        idx = t->sb_head;
        t->sb_head = (t->sb_head + 1) % T_MAXLINES;
    }
    t->sb[idx][0] = 0;
    t->sb_col[idx] = col;
}

/* Append char c to newest line */
static void sb_putc(term_t *t, char c) {
    if (t->sb_count == 0) sb_newline(t, TC_NORMAL);
    int idx = sb_idx(t, t->sb_count - 1);
    int len = 0;
    while (t->sb[idx][len]) len++;
    if (len < T_LINECOLS - 1) {
        t->sb[idx][len]   = c;
        t->sb[idx][len+1] = 0;
    }
}

/* Write a string to scrollback with colour tag */
static void sb_puts(term_t *t, const char *s, u8 col) {
    if (!t || !s) return;
    if (t->sb_count == 0) sb_newline(t, col);

    while (*s) {
        char c = *s++;
        if (c == '\n' || c == '\r') {
            sb_newline(t, col);
        } else if (c == '\b') {
            if (t->sb_count > 0) {
                int idx = sb_idx(t, t->sb_count-1);
                int len = 0;
                while (t->sb[idx][len]) len++;
                if (len > 0) t->sb[idx][len-1] = 0;
            }
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x80) {
            /* Word wrap: if line is full, start a new continuation line */
            if (t->sb_count > 0 && t->cols > 0) {
                int idx = sb_idx(t, t->sb_count-1);
                int len = 0;
                while (t->sb[idx][len]) len++;
                if (len >= t->cols - 1) {
                    sb_newline(t, col);
                }
            }
            sb_putc(t, c);
        }
        /* skip non-ASCII (UTF-8 multibyte, etc.) */
    }
}

/* printk hook */
static void term_hook(const char *s) {
    if (g_cap_term) sb_puts(g_cap_term, s, TC_OUTPUT);
}

/* ── colour map ─────────────────────────────────────────── */
static fb_color_t term_col(u8 tag) {
    switch(tag) {
    case TC_OUTPUT: return FB_COLOR(0xA8,0xB8,0xC8); /* dim blue-gray  */
    case TC_ERR:    return FB_COLOR(0xFF,0x5F,0x56); /* red            */
    case TC_PROMPT: return FB_COLOR(0xD4,0xD4,0xD0); /* normal         */
    case TC_INFO:   return FB_COLOR(0x8B,0xE9,0xFD); /* cyan           */
    default:        return FB_COLOR(0xD4,0xD4,0xD0); /* normal         */
    }
}

/* ── draw one terminal text line ─────────────────────────── */
static void term_draw_line(int sx, int sy, int max_w,
                            const char *text, u8 col,
                            fb_color_t bg) {
    if (!text || !text[0]) return;
    fb_color_t fg = term_col(col);
    int cx = sx;
    for (int i = 0; text[i] && cx + T_FONT_W <= sx + max_w; i++) {
        fb_draw_char(cx, sy, text[i], fg, bg);
        cx += T_FONT_W;
    }
}

/* ── paint ─────────────────────────────────────────────── */
static void term_paint(wm_window_t *w) {
    term_t *t = (term_t *)w->userdata;
    if (!t) return;

    int cx = w->client_x, cy = w->client_y;
    int cw = w->client_w, ch = w->client_h;
    if (cw < 40 || ch < 40) return;

    /* Compute metrics */
    t->cols     = (cw - T_PAD_L*2 - 4) / T_FONT_W;
    if (t->cols < 10) t->cols = 10;
    int input_area_h = T_INPUT_H + T_INPUT_SEP + 2;
    int scroll_h = ch - input_area_h - T_PAD_T;
    t->vis_rows = scroll_h / T_FONT_H;
    if (t->vis_rows < 1) t->vis_rows = 1;

    fb_color_t bg     = C_TERM_BG;
    fb_color_t inp_bg = FB_COLOR(0x12,0x12,0x22);
    fb_color_t sep_c  = FB_COLOR(0x2A,0x2A,0x45);

    /* ── Background ── */
    fb_set_clip(cx, cy, cw, ch);
    fb_fill_rect(cx, cy, cw, ch, bg);

    /* ── Scrollback ── */
    /* Which line is the bottom visible? */
    /* sb_scroll=0 means showing newest. sb_scroll=N means scrolled up N lines. */
    int bottom_line = t->sb_count - 1 - t->sb_scroll;
    int first_line  = bottom_line - t->vis_rows + 1;

    for (int row = 0; row < t->vis_rows; row++) {
        int li = first_line + row;
        if (li < 0 || li >= t->sb_count) continue;
        int idx = sb_idx(t, li);
        int py  = cy + T_PAD_T + row * T_FONT_H;
        term_draw_line(cx + T_PAD_L, py, cw - T_PAD_L*2,
                       t->sb[idx], t->sb_col[idx], bg);
    }

    /* ── Scrollbar ── */
    if (t->sb_count > t->vis_rows) {
        int sb_x = cx + cw - 5;
        int sb_h = scroll_h;
        int sb_y = cy + T_PAD_T;
        fb_fill_rect(sb_x, sb_y, 4, sb_h, FB_COLOR(0x20,0x20,0x38));
        /* thumb */
        int th_h = (sb_h * t->vis_rows) / t->sb_count;
        if (th_h < 12) th_h = 12;
        int max_scroll = t->sb_count - t->vis_rows;
        int scroll_pos = max_scroll - t->sb_scroll;
        if (max_scroll <= 0) max_scroll = 1;
        int th_y = sb_y + (scroll_pos * (sb_h - th_h)) / max_scroll;
        fb_fill_rect(sb_x, th_y, 4, th_h, FB_COLOR(0x55,0x55,0x90));
    }

    /* ── Input separator ── */
    int sep_y = cy + ch - input_area_h;
    fb_fill_rect(cx, sep_y, cw, T_INPUT_SEP, sep_c);
    fb_fill_rect(cx, sep_y + T_INPUT_SEP, cw, input_area_h - T_INPUT_SEP, inp_bg);

    /* ── Prompt + input ── */
    int ip_y = sep_y + T_INPUT_SEP + 4;
    int ip_x = cx + T_PAD_L;

    /* "paper" in green */
    fb_draw_string(ip_x, ip_y, "paper", C_TERM_GRN, inp_bg);
    ip_x += 5 * T_FONT_W;
    /* "@pico" in cyan */
    fb_draw_string(ip_x, ip_y, "@pico", C_TERM_CYA, inp_bg);
    ip_x += 5 * T_FONT_W;
    /* ":~$ " in yellow */
    fb_draw_string(ip_x, ip_y, ":~$ ", C_TERM_YEL, inp_bg);
    ip_x += 4 * T_FONT_W;

    /* Input text */
    t->ibuf[t->ilen] = 0;
    int text_start_x = ip_x;

    /* Draw chars, highlight cursor position */
    for (int i = 0; i <= t->ilen; i++) {
        int cpx = text_start_x + i * T_FONT_W;
        if (cpx + T_FONT_W > cx + cw - T_PAD_L) break;

        int is_cur = (i == t->icur);

        /* Blink toggle */
        if (is_cur && t->blink_on) {
            /* Cursor block */
            fb_fill_rect(cpx, ip_y, T_FONT_W, T_FONT_H, C_TERM_CUR);
            if (i < t->ilen) {
                char cc[2] = { t->ibuf[i], 0 };
                fb_draw_string(cpx, ip_y, cc, inp_bg, C_TERM_CUR);
            }
        } else if (i < t->ilen) {
            char cc[2] = { t->ibuf[i], 0 };
            fb_draw_string(cpx, ip_y, cc, C_TERM_FG, inp_bg);
        } else if (!t->blink_on) {
            /* Underscore cursor when off */
            fb_draw_hline(cpx, ip_y + T_FONT_H - 2, T_FONT_W,
                          FB_COLOR(0x88,0x88,0xAA));
        }
    }

    fb_clear_clip();
}

/* ── execute ─────────────────────────────────────────────── */
static void term_exec(term_t *t, const char *cmd) {
    /* Echo with prompt (plain text) */
    sb_puts(t, "paper@pico:~$ ", TC_PROMPT);
    sb_puts(t, cmd,              TC_PROMPT);
    sb_puts(t, "\n",             TC_PROMPT);

    /* History */
    if (cmd[0]) {
        int last = (t->hist_n > 0) ? (t->hist_n - 1) % T_HIST : -1;
        int dup  = (last >= 0 && t->hist[last][0] &&
                    memcmp(t->hist[last], cmd, T_IBUF) == 0);
        if (!dup) {
            int slot = t->hist_n % T_HIST;
            int i = 0;
            while (cmd[i] && i < T_IBUF-1) { t->hist[slot][i]=cmd[i]; i++; }
            t->hist[slot][i] = 0;
            t->hist_n++;
        }
    }
    t->hist_pos = -1;

    /* Run command, capture output */
    g_cap_term = t;
    printk_set_hook(term_hook);
    ksh_execute_line(cmd);
    printk_set_hook(NULL);
    g_cap_term = NULL;

    /* Next prompt */
    sb_puts(t, "paper@pico:~$ ", TC_NORMAL);

    /* Scroll to bottom */
    t->sb_scroll = 0;
}

/* ── proc ─────────────────────────────────────────────────── */
static int term_proc(wm_window_t *w, wm_msg_t msg, u64 p1, u64 p2) {
    term_t *t = (term_t *)w->userdata;

    switch (msg) {

    case WM_PAINT:
        if (!t) return 0;
        /* Update cursor blink */
        if (system_ticks - t->blink_last >= 50) {
            t->blink_on   = !t->blink_on;
            t->blink_last = system_ticks;
        }
        term_paint(w);
        w->flags |= WF_DIRTY; /* keep repainting for cursor blink */
        return 0;

    case WM_CHAR: {
        if (!t) return 0;
        char c   = (char)(p1 & 0xFF);
        u32  mod = (u32)(p2 & 0xFFFF);

        if (c == '\r' || c == '\n') {
            t->ibuf[t->ilen] = 0;
            char cmd[T_IBUF];
            int i = 0;
            while (i < t->ilen) { cmd[i] = t->ibuf[i]; i++; }
            cmd[i] = 0;
            t->ilen = 0; t->icur = 0;
            term_exec(t, cmd);

        } else if (c == '\b' || c == 0x7F) {
            if (t->icur > 0) {
                for (int i = t->icur-1; i < t->ilen-1; i++)
                    t->ibuf[i] = t->ibuf[i+1];
                t->ilen--;
                t->icur--;
                if (t->ilen < 0) t->ilen = 0;
                if (t->icur < 0) t->icur = 0;
                t->ibuf[t->ilen] = 0;
            }

        } else if (c == 0x03) {
            /* Ctrl+C (ascii ETX) */
            sb_puts(t, "^C\n", TC_ERR);
            sb_puts(t, "paper@pico:~$ ", TC_NORMAL);
            t->ilen = 0; t->icur = 0;

        } else if (c == 0x0C) {
            /* Ctrl+L (ascii FF) — clear */
            t->sb_head  = 0;
            t->sb_count = 0;
            t->sb_scroll = 0;
            sb_puts(t, "paper@pico:~$ ", TC_NORMAL);

        } else if (c == 0x01) {
            /* Ctrl+A — home */
            t->icur = 0;

        } else if (c == 0x05) {
            /* Ctrl+E — end */
            t->icur = t->ilen;

        } else if (c == 0x0B) {
            /* Ctrl+K — kill to end of line */
            t->ilen = t->icur;
            t->ibuf[t->ilen] = 0;

        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F
                   && t->ilen < T_IBUF - 1) {
            (void)mod;
            /* Insert at cursor */
            for (int i = t->ilen; i > t->icur; i--)
                t->ibuf[i] = t->ibuf[i-1];
            t->ibuf[t->icur++] = c;
            t->ilen++;
            t->ibuf[t->ilen] = 0;
        }
        t->blink_on   = 1;
        t->blink_last = system_ticks;
        w->flags |= WF_DIRTY;
        return 0;
    }

    case WM_KEYDOWN:
        if (!t) return 0;
        switch ((u32)p1) {
        case KEY_UP:
            if (t->hist_n > 0) {
                if (t->hist_pos < 0) t->hist_pos = t->hist_n - 1;
                else if (t->hist_pos > 0) t->hist_pos--;
                int slot = t->hist_pos % T_HIST;
                int i = 0;
                while (t->hist[slot][i] && i < T_IBUF-1) {
                    t->ibuf[i] = t->hist[slot][i]; i++;
                }
                t->ibuf[i] = 0; t->ilen = i; t->icur = i;
            } else {
                if (t->sb_scroll < t->sb_count - t->vis_rows)
                    t->sb_scroll++;
            }
            break;
        case KEY_DOWN:
            if (t->hist_pos >= 0) {
                t->hist_pos++;
                if (t->hist_pos >= t->hist_n) {
                    t->hist_pos = -1;
                    t->ilen = 0; t->icur = 0; t->ibuf[0] = 0;
                } else {
                    int slot = t->hist_pos % T_HIST;
                    int i = 0;
                    while (t->hist[slot][i] && i < T_IBUF-1) {
                        t->ibuf[i] = t->hist[slot][i]; i++;
                    }
                    t->ibuf[i] = 0; t->ilen = i; t->icur = i;
                }
            } else {
                if (t->sb_scroll > 0) t->sb_scroll--;
            }
            break;
        case KEY_LEFT:
            if (t->icur > 0) t->icur--;
            break;
        case KEY_RIGHT:
            if (t->icur < t->ilen) t->icur++;
            break;
        case KEY_HOME:
            t->icur = 0;
            break;
        case KEY_END:
            t->icur = t->ilen;
            break;
        case KEY_PGUP: {
            int max_scroll = t->sb_count - t->vis_rows;
            if (max_scroll < 0) max_scroll = 0;
            t->sb_scroll += t->vis_rows / 2;
            if (t->sb_scroll > max_scroll) t->sb_scroll = max_scroll;
            break;
        }
        case KEY_PGDN:
            t->sb_scroll -= t->vis_rows / 2;
            if (t->sb_scroll < 0) t->sb_scroll = 0;
            break;
        }
        t->blink_on = 1;
        w->flags |= WF_DIRTY;
        return 0;

    case WM_FOCUS_IN:
        if (t) {
            g_cap_term = t;
            printk_set_hook(term_hook);
        }
        return 0;

    case WM_FOCUS_OUT:
        return 0;

    case WM_RESIZE:
        wm_calc_client(w);
        w->flags |= WF_DIRTY;
        return 0;

    case WM_CLOSE:
        if (g_cap_term == t) {
            g_cap_term = NULL;
            printk_set_hook(NULL);
        }
        wm_do_destroy(w);
        return 0;

    default:
        return -1;
    }
    return 0;
}

wm_window_t *wm_open_terminal(void) {
    int tw = (G_sw - RAIL_W) * 62 / 100;
    int th = G_sh - TOPBAR_H - (DOCK_PAD*2 + DOCK_BTN_SZ + DOCK_MARG) - 30;
    int tx = RAIL_W + 18;
    int ty = TOPBAR_H + 18;
    if (tw < 480) tw = 480;
    if (th < 320) th = 320;

    wm_window_t *w = wm_create_window(tx, ty, tw, th,
                                       "Terminal", WF_RESIZABLE, term_proc);
    if (!w) return NULL;

    term_t *t = (term_t *)kmalloc(sizeof(term_t), GFP_KERNEL);
    if (!t) { wm_do_destroy(w); return NULL; }
    memset(t, 0, sizeof(*t));
    t->hist_pos = -1;
    t->cols     = 80; /* will be updated on first paint */
    t->blink_on = 1;

    /* Welcome */
    sb_puts(t, "PaperWM Terminal  /  Picomimi-x64\n", TC_INFO);
    sb_puts(t, "F1=new terminal  F2=sysmon  F11=maximise\n", TC_OUTPUT);
    sb_puts(t, "Ctrl+C=cancel  Ctrl+L=clear  Up/Down=history\n", TC_OUTPUT);
    sb_puts(t, "\n", TC_NORMAL);
    sb_puts(t, "paper@pico:~$ ", TC_NORMAL);

    w->userdata = t;
    g_cap_term  = t;
    printk_set_hook(term_hook);
    return w;
}

/* ═══════════════════════════════════════════════════════════
 * SYSMON
 * ═══════════════════════════════════════════════════════════ */
#define SM_HIST 60

typedef struct {
    u8  cpu[SM_HIST];
    int cpu_pos;
    u64 last_tick;
    u32 rng;
} sysmon_t;

static void sysmon_paint(wm_window_t *w) {
    sysmon_t *s = (sysmon_t *)w->userdata;
    if (!s) return;

    int cx = w->client_x, cy = w->client_y;
    int cw = w->client_w, ch = w->client_h;
    if (cw < 60 || ch < 80) return;

    extern u64 pmm_get_free_memory(void);
    extern u64 pmm_get_total_memory(void);

    fb_set_clip(cx, cy, cw, ch);
    fb_fill_rect(cx, cy, cw, ch, C_WIN_BG);

    int pad = 12;
    int y   = cy + pad;
    int xp  = cx + pad;
    int bw  = cw - pad*2;

    /* Title */
    fb_draw_string(xp, y, "System Monitor", C_INK, C_WIN_BG);
    y += FB_FONT_H + 4;
    fb_draw_hline(xp, y, bw, C_LGT);
    y += 8;

    /* ── Memory ── */
    u64 total = pmm_get_total_memory();
    u64 freemb = pmm_get_free_memory();
    u64 used  = (total > freemb) ? total - freemb : 0;

    /* Label row */
    fb_draw_string(xp, y, "Memory", C_INK2, C_WIN_BG);
    /* right-align value */
    char mbuf[32]; int mi = 0;
    char tmp[12];
    wm_uitoa(tmp, used >> 20);
    for (int i = 0; tmp[i]; i++) mbuf[mi++] = tmp[i];
    mbuf[mi++]='M'; mbuf[mi++]='B'; mbuf[mi++]=' '; mbuf[mi++]='/'; mbuf[mi++]=' ';
    wm_uitoa(tmp, total >> 20);
    for (int i = 0; tmp[i]; i++) mbuf[mi++] = tmp[i];
    mbuf[mi++]='M'; mbuf[mi++]='B'; mbuf[mi] = 0;
    int mw = mi * FB_FONT_W;
    fb_draw_string(cx + cw - pad - mw, y, mbuf, C_MID, C_WIN_BG);
    y += FB_FONT_H + 3;

    /* Bar */
    int bh2 = 14;
    fb_fill_rect(xp, y, bw, bh2, FB_COLOR(0xEE,0xEE,0xE8));
    if (total > 0 && used > 0) {
        int fill = (int)((u64)bw * used / total);
        if (fill < 1) fill = 1;
        /* Gradient: blue left -> purple right */
        for (int px = 0; px < fill; px++) {
            int r = 0x31 + (0x80-0x31)*px/bw;
            int g = 0x7A - 0x30*px/bw;
            int b = 0xD1;
            fb_draw_vline(xp+px, y, bh2, FB_COLOR(r,g,b));
        }
        /* Highlight stripe */
        fb_draw_hline(xp, y+1, fill, FB_COLOR(0x88,0xBB,0xFF));
    }
    fb_draw_rect(xp, y, bw, bh2, C_LGT);
    y += bh2 + 12;

    /* ── CPU ── */
    /* Update sample */
    if (system_ticks - s->last_tick >= 100) {
        s->rng = s->rng * 1664525u + 1013904223u;
        u8 fake = (u8)(5 + (s->rng >> 25) % 55);
        s->cpu[s->cpu_pos] = fake;
        s->cpu_pos = (s->cpu_pos + 1) % SM_HIST;
        s->last_tick = system_ticks;
    }

    int cur_pct = s->cpu[(s->cpu_pos - 1 + SM_HIST) % SM_HIST];
    char cpct[8]; int ci = 0;
    wm_uitoa(tmp, cur_pct);
    for (int i = 0; tmp[i]; i++) cpct[ci++] = tmp[i];
    cpct[ci++]='%'; cpct[ci]=0;

    fb_draw_string(xp, y, "CPU", C_INK2, C_WIN_BG);
    fb_draw_string(cx+cw-pad-ci*FB_FONT_W, y, cpct,
                   FB_COLOR(0x31,0x7A,0xD1), C_WIN_BG);
    y += FB_FONT_H + 3;

    /* Graph */
    int gh = ch - (y - cy) - pad - 28;
    if (gh < 30) gh = 30;
    int gw = bw;

    fb_fill_rect(xp, y, gw, gh, FB_COLOR(0xF8,0xF8,0xFC));
    fb_draw_rect(xp, y, gw, gh, C_LGT);

    /* Dashed grid lines at 25/50/75% */
    for (int pct = 25; pct < 100; pct += 25) {
        int gy = y + gh - (pct * gh / 100);
        for (int gx = xp+2; gx < xp+gw-2; gx += 4)
            fb_putpixel(gx, gy, C_LGT2);
    }

    /* Filled area graph */
    int prev_gx = xp+1, prev_gy = y + gh - 1;
    for (int i = 0; i < SM_HIST; i++) {
        int hi  = (s->cpu_pos - SM_HIST + i + SM_HIST) % SM_HIST;
        int pct = s->cpu[hi];
        int gx  = xp + 1 + i * (gw-2) / SM_HIST;
        int gy2 = y + gh - 1 - (pct * (gh-2) / 100);

        if (i > 0) {
            /* Fill column */
            int steps = gx - prev_gx;
            if (steps < 1) steps = 1;
            for (int si = 0; si <= steps; si++) {
                int lx = prev_gx + si;
                int ly = prev_gy + (gy2 - prev_gy) * si / steps;
                if (ly < y + gh - 1)
                    fb_draw_vline(lx, ly, (y+gh-1) - ly,
                                  FB_COLOR(0xBC,0xD8,0xF4));
            }
            fb_draw_line(prev_gx, prev_gy, gx, gy2,
                         FB_COLOR(0x31,0x7A,0xD1));
        }
        prev_gx = gx; prev_gy = gy2;
    }
    y += gh + 6;

    /* ── Info row ── */
    fb_draw_hline(xp, y, bw, C_LGT2);
    y += 4;

    u64 uptime = system_ticks / 100;
    char ubuf[24]; int ui = 0;
    wm_uitoa(tmp, uptime);
    ubuf[ui++]='U';ubuf[ui++]='p';ubuf[ui++]=':';ubuf[ui++]=' ';
    for (int i=0;tmp[i];i++) ubuf[ui++]=tmp[i];
    ubuf[ui++]='s'; ubuf[ui]=0;
    fb_draw_string(xp, y, ubuf, C_MID, C_WIN_BG);

    const mouse_state_t *ms = mouse_get_state();
    char mpos[20]; int mpi=0;
    wm_uitoa(tmp,(u64)ms->x); mpos[mpi++]='X';mpos[mpi++]=':';
    for(int i=0;tmp[i];i++) mpos[mpi++]=tmp[i];
    mpos[mpi++]=' ';mpos[mpi++]='Y';mpos[mpi++]=':';
    wm_uitoa(tmp,(u64)ms->y);
    for(int i=0;tmp[i];i++) mpos[mpi++]=tmp[i];
    mpos[mpi]=0;
    fb_draw_string(cx+cw-pad-mpi*FB_FONT_W, y, mpos, C_MID, C_WIN_BG);

    fb_clear_clip();
    w->flags |= WF_DIRTY; /* continuous update */
}

static int sysmon_proc(wm_window_t *w, wm_msg_t msg, u64 p1, u64 p2) {
    (void)p1;(void)p2;
    switch (msg) {
    case WM_PAINT: case WM_TIMER:
        sysmon_paint(w);
        return 0;
    case WM_RESIZE:
        wm_calc_client(w);
        w->flags |= WF_DIRTY;
        return 0;
    case WM_CLOSE:
        wm_do_destroy(w);
        return 0;
    default:
        return -1;
    }
}

wm_window_t *wm_open_sysmon(void) {
    int ww = 300, wh = 360;
    int wx = G_sw - ww - 18;
    int wy = TOPBAR_H + 18;
    wm_window_t *w = wm_create_window(wx,wy,ww,wh,"Sysmon",WF_RESIZABLE,sysmon_proc);
    if (!w) return NULL;

    sysmon_t *s = (sysmon_t *)kmalloc(sizeof(sysmon_t), GFP_KERNEL);
    if (!s) { wm_do_destroy(w); return NULL; }
    memset(s, 0, sizeof(*s));
    s->rng = 0xCAFEBABE;
    w->userdata = s;
    return w;
}

/* ═══════════════════════════════════════════════════════════
 * ABOUT
 * ═══════════════════════════════════════════════════════════ */
static int about_proc(wm_window_t *w, wm_msg_t msg, u64 p1, u64 p2) {
    (void)p1;(void)p2;
    switch (msg) {
    case WM_PAINT: {
        int cx=w->client_x,cy=w->client_y,cw=w->client_w,ch=w->client_h;
        fb_set_clip(cx,cy,cw,ch);
        fb_fill_rect(cx,cy,cw,ch,C_WIN_BG);
        int x=cx+20, y=cy+20;
        fb_draw_string(x,y,"PaperWM",C_INK,C_WIN_BG);
        y+=FB_FONT_H+2;
        fb_draw_string(x,y,"Picomimi-x64 kernel",C_MID,C_WIN_BG);
        y+=FB_FONT_H*2;
        fb_draw_hline(cx+20,y,cw-40,C_LGT); y+=8;
        fb_draw_string(x,y,"Kernel:   alpha (in-dev)",C_INK2,C_WIN_BG); y+=FB_FONT_H+2;
        fb_draw_string(x,y,"WM:       PaperWM v2",C_INK2,C_WIN_BG); y+=FB_FONT_H+2;
        fb_draw_string(x,y,"Icons:    Paper theme (CC-BY-SA 4.0)",C_INK2,C_WIN_BG); y+=FB_FONT_H+2;
        fb_draw_string(x,y,"Display:  1024x768 32bpp",C_INK2,C_WIN_BG);
        fb_clear_clip();
        return 0;
    }
    case WM_CLOSE: wm_do_destroy(w); return 0;
    default: return -1;
    }
}

wm_window_t *wm_open_about(void) {
    int ww=360,wh=220;
    return wm_create_window((G_sw-ww)/2,(G_sh-wh)/2,ww,wh,
                             "About PaperWM",0,about_proc);
}

/* ── init ─────────────────────────────────────────────────── */
void wm_apps_init(void) { /* nothing */ }
