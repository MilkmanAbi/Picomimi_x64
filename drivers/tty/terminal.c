/**
 * Picomimi-x64 Terminal Emulator
 * 
 * Full terminal emulator with:
 * - Scrollback buffer (configurable history)
 * - ANSI/VT100 escape sequence support
 * - Color support (16 colors + 256 extended)
 * - Cursor control
 * - Line editing
 * - UTF-8 support (basic)
 */

#include <kernel/types.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <arch/io.h>

// ============================================================================
// TERMINAL CONFIGURATION
// ============================================================================

#define TERM_WIDTH          80
#define TERM_HEIGHT         25
#define TERM_SCROLLBACK     1000    // Lines of scrollback history
#define TERM_TAB_WIDTH      8

// VGA memory
#define VGA_MEMORY          0xFFFFFFFF800B8000  // Higher-half VGA address
#define VGA_CTRL_REG        0x3D4
#define VGA_DATA_REG        0x3D5

// ============================================================================
// COLORS
// ============================================================================

typedef enum {
    COLOR_BLACK         = 0,
    COLOR_BLUE          = 1,
    COLOR_GREEN         = 2,
    COLOR_CYAN          = 3,
    COLOR_RED           = 4,
    COLOR_MAGENTA       = 5,
    COLOR_BROWN         = 6,
    COLOR_LIGHT_GRAY    = 7,
    COLOR_DARK_GRAY     = 8,
    COLOR_LIGHT_BLUE    = 9,
    COLOR_LIGHT_GREEN   = 10,
    COLOR_LIGHT_CYAN    = 11,
    COLOR_LIGHT_RED     = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_YELLOW        = 14,
    COLOR_WHITE         = 15,
} vga_color_t;

static inline u8 vga_entry_color(vga_color_t fg, vga_color_t bg) {
    return fg | (bg << 4);
}

static inline u16 vga_entry(unsigned char c, u8 color) {
    return (u16)c | ((u16)color << 8);
}

// ============================================================================
// TERMINAL CHARACTER CELL
// ============================================================================

typedef struct {
    char        c;          // Character
    u8          fg;         // Foreground color
    u8          bg;         // Background color
    u8          attr;       // Attributes (bold, underline, etc.)
} term_cell_t;

// Attributes
#define ATTR_BOLD       (1 << 0)
#define ATTR_DIM        (1 << 1)
#define ATTR_UNDERLINE  (1 << 2)
#define ATTR_BLINK      (1 << 3)
#define ATTR_REVERSE    (1 << 4)
#define ATTR_HIDDEN     (1 << 5)

// ============================================================================
// ESCAPE SEQUENCE PARSER STATE
// ============================================================================

typedef enum {
    STATE_NORMAL,
    STATE_ESC,          // Got ESC
    STATE_CSI,          // Got ESC[
    STATE_CSI_PARAM,    // Parsing parameters
    STATE_OSC,          // Operating System Command
    STATE_CHARSET,      // Character set selection
} parse_state_t;

#define MAX_ESC_PARAMS  16
#define MAX_ESC_INTERMEDIATE 4

// ============================================================================
// TERMINAL STRUCTURE
// ============================================================================

typedef struct terminal {
    // Screen buffer (visible)
    term_cell_t     screen[TERM_HEIGHT][TERM_WIDTH];
    
    // Scrollback buffer
    term_cell_t     *scrollback;
    int             scrollback_lines;
    int             scrollback_pos;     // Current scroll position
    int             scrollback_write;   // Next write position
    
    // Cursor
    int             cursor_x;
    int             cursor_y;
    int             saved_cursor_x;
    int             saved_cursor_y;
    int             cursor_visible;
    
    // Colors
    u8              fg_color;
    u8              bg_color;
    u8              attr;
    u8              default_fg;
    u8              default_bg;
    
    // Escape sequence parsing
    parse_state_t   state;
    int             esc_params[MAX_ESC_PARAMS];
    int             esc_param_count;
    char            esc_intermediate[MAX_ESC_INTERMEDIATE];
    int             esc_intermediate_count;
    int             esc_private;        // Private mode indicator (?)
    
    // Modes
    int             origin_mode;        // DECOM - origin mode
    int             autowrap;           // Auto-wrap at end of line
    int             insert_mode;        // Insert mode (vs replace)
    int             linefeed_mode;      // LF = LF+CR?
    int             local_echo;         // Local echo
    int             cursor_keys_app;    // Application cursor keys
    
    // Scroll region
    int             scroll_top;
    int             scroll_bottom;
    
    // Tabs
    u8              tabs[TERM_WIDTH];   // Tab stops
    
    // Statistics
    u64             chars_written;
    u64             escapes_processed;
} terminal_t;

// Global terminal instance
static terminal_t main_terminal;
static int terminal_initialized = 0;
static volatile u16 *vga_buffer = (volatile u16 *)VGA_MEMORY;

// ============================================================================
// LOW-LEVEL VGA
// ============================================================================

static void vga_set_cursor(int x, int y) {
    u16 pos = y * TERM_WIDTH + x;
    
    outb(VGA_CTRL_REG, 0x0F);
    outb(VGA_DATA_REG, (u8)(pos & 0xFF));
    outb(VGA_CTRL_REG, 0x0E);
    outb(VGA_DATA_REG, (u8)((pos >> 8) & 0xFF));
}

static void vga_enable_cursor(int start, int end) {
    outb(VGA_CTRL_REG, 0x0A);
    outb(VGA_DATA_REG, (inb(VGA_DATA_REG) & 0xC0) | start);
    outb(VGA_CTRL_REG, 0x0B);
    outb(VGA_DATA_REG, (inb(VGA_DATA_REG) & 0xE0) | end);
}

static void vga_disable_cursor(void) {
    outb(VGA_CTRL_REG, 0x0A);
    outb(VGA_DATA_REG, 0x20);
}

// ============================================================================
// TERMINAL RENDERING
// ============================================================================

static void term_render_cell(terminal_t *term, int x, int y) {
    term_cell_t *cell = &term->screen[y][x];
    u8 fg = cell->fg;
    u8 bg = cell->bg;
    
    // Apply attributes
    if (cell->attr & ATTR_BOLD) {
        fg |= 0x08;  // Bright version
    }
    if (cell->attr & ATTR_REVERSE) {
        u8 tmp = fg;
        fg = bg;
        bg = tmp;
    }
    if (cell->attr & ATTR_HIDDEN) {
        fg = bg;
    }
    
    u8 color = vga_entry_color(fg, bg);
    vga_buffer[y * TERM_WIDTH + x] = vga_entry(cell->c ? cell->c : ' ', color);
}

static void term_render_screen(terminal_t *term) {
    for (int y = 0; y < TERM_HEIGHT; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            term_render_cell(term, x, y);
        }
    }
    
    if (term->cursor_visible) {
        vga_set_cursor(term->cursor_x, term->cursor_y);
    }
}

static void term_render_line(terminal_t *term, int y) {
    for (int x = 0; x < TERM_WIDTH; x++) {
        term_render_cell(term, x, y);
    }
}

// ============================================================================
// SCROLLING
// ============================================================================

static void term_save_line_to_scrollback(terminal_t *term, int y) {
    if (!term->scrollback) return;
    
    // Copy line to scrollback
    int sb_idx = term->scrollback_write * TERM_WIDTH;
    for (int x = 0; x < TERM_WIDTH; x++) {
        term->scrollback[sb_idx + x] = term->screen[y][x];
    }
    
    term->scrollback_write = (term->scrollback_write + 1) % TERM_SCROLLBACK;
    if (term->scrollback_lines < TERM_SCROLLBACK) {
        term->scrollback_lines++;
    }
}

static void term_scroll_up(terminal_t *term, int top, int bottom, int lines) {
    if (lines <= 0) return;
    if (lines > bottom - top + 1) lines = bottom - top + 1;
    
    // Save lines going off top to scrollback
    if (top == term->scroll_top) {
        for (int i = 0; i < lines; i++) {
            term_save_line_to_scrollback(term, top + i);
        }
    }
    
    // Move lines up
    for (int y = top; y <= bottom - lines; y++) {
        memcpy(term->screen[y], term->screen[y + lines], 
               sizeof(term_cell_t) * TERM_WIDTH);
    }
    
    // Clear new lines at bottom
    for (int y = bottom - lines + 1; y <= bottom; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            term->screen[y][x].c = ' ';
            term->screen[y][x].fg = term->default_fg;
            term->screen[y][x].bg = term->default_bg;
            term->screen[y][x].attr = 0;
        }
    }
    
    // Re-render affected lines
    for (int y = top; y <= bottom; y++) {
        term_render_line(term, y);
    }
}

static void term_scroll_down(terminal_t *term, int top, int bottom, int lines) {
    if (lines <= 0) return;
    if (lines > bottom - top + 1) lines = bottom - top + 1;
    
    // Move lines down
    for (int y = bottom; y >= top + lines; y--) {
        memcpy(term->screen[y], term->screen[y - lines],
               sizeof(term_cell_t) * TERM_WIDTH);
    }
    
    // Clear new lines at top
    for (int y = top; y < top + lines; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            term->screen[y][x].c = ' ';
            term->screen[y][x].fg = term->default_fg;
            term->screen[y][x].bg = term->default_bg;
            term->screen[y][x].attr = 0;
        }
    }
    
    // Re-render
    for (int y = top; y <= bottom; y++) {
        term_render_line(term, y);
    }
}

// View scrollback (scroll viewport up into history)
void term_scroll_view(terminal_t *term, int lines) {
    if (!term->scrollback) return;
    
    term->scrollback_pos += lines;
    
    if (term->scrollback_pos < 0) {
        term->scrollback_pos = 0;
    }
    if (term->scrollback_pos > term->scrollback_lines) {
        term->scrollback_pos = term->scrollback_lines;
    }
    
    if (term->scrollback_pos > 0) {
        // Render scrollback
        int sb_start = (term->scrollback_write - term->scrollback_pos + TERM_SCROLLBACK) 
                       % TERM_SCROLLBACK;
        
        for (int y = 0; y < TERM_HEIGHT; y++) {
            int sb_line = (sb_start + y) % TERM_SCROLLBACK;
            int sb_idx = sb_line * TERM_WIDTH;
            
            if (y < term->scrollback_pos) {
                for (int x = 0; x < TERM_WIDTH; x++) {
                    term_cell_t *cell = &term->scrollback[sb_idx + x];
                    u8 color = vga_entry_color(cell->fg, cell->bg);
                    vga_buffer[y * TERM_WIDTH + x] = vga_entry(cell->c ? cell->c : ' ', color);
                }
            } else {
                // Regular screen
                term_render_line(term, y);
            }
        }
    } else {
        term_render_screen(term);
    }
}

// ============================================================================
// CURSOR CONTROL
// ============================================================================

static void term_move_cursor(terminal_t *term, int x, int y) {
    if (x < 0) x = 0;
    if (x >= TERM_WIDTH) x = TERM_WIDTH - 1;
    
    int top = term->origin_mode ? term->scroll_top : 0;
    int bottom = term->origin_mode ? term->scroll_bottom : TERM_HEIGHT - 1;
    
    if (y < top) y = top;
    if (y > bottom) y = bottom;
    
    term->cursor_x = x;
    term->cursor_y = y;
    
    if (term->cursor_visible) {
        vga_set_cursor(x, y);
    }
}

static void term_newline(terminal_t *term) {
    if (term->cursor_y >= term->scroll_bottom) {
        term_scroll_up(term, term->scroll_top, term->scroll_bottom, 1);
    } else {
        term->cursor_y++;
    }
    
    if (term->linefeed_mode) {
        term->cursor_x = 0;
    }
    
    if (term->cursor_visible) {
        vga_set_cursor(term->cursor_x, term->cursor_y);
    }
}

// ============================================================================
// ESCAPE SEQUENCE HANDLERS
// ============================================================================

// Get parameter with default
static int term_get_param(terminal_t *term, int idx, int def) {
    if (idx >= term->esc_param_count || term->esc_params[idx] == 0) {
        return def;
    }
    return term->esc_params[idx];
}

// CSI m - Select Graphic Rendition (colors and attributes)
static void term_csi_sgr(terminal_t *term) {
    for (int i = 0; i < term->esc_param_count || i == 0; i++) {
        int param = term_get_param(term, i, 0);
        
        switch (param) {
        case 0:  // Reset
            term->fg_color = term->default_fg;
            term->bg_color = term->default_bg;
            term->attr = 0;
            break;
        case 1:  // Bold
            term->attr |= ATTR_BOLD;
            break;
        case 2:  // Dim
            term->attr |= ATTR_DIM;
            break;
        case 4:  // Underline
            term->attr |= ATTR_UNDERLINE;
            break;
        case 5:  // Blink
            term->attr |= ATTR_BLINK;
            break;
        case 7:  // Reverse
            term->attr |= ATTR_REVERSE;
            break;
        case 8:  // Hidden
            term->attr |= ATTR_HIDDEN;
            break;
        case 21: // Bold off
        case 22: // Normal intensity
            term->attr &= ~(ATTR_BOLD | ATTR_DIM);
            break;
        case 24: // Underline off
            term->attr &= ~ATTR_UNDERLINE;
            break;
        case 25: // Blink off
            term->attr &= ~ATTR_BLINK;
            break;
        case 27: // Reverse off
            term->attr &= ~ATTR_REVERSE;
            break;
        case 28: // Hidden off
            term->attr &= ~ATTR_HIDDEN;
            break;
        
        // Foreground colors
        case 30: term->fg_color = COLOR_BLACK; break;
        case 31: term->fg_color = COLOR_RED; break;
        case 32: term->fg_color = COLOR_GREEN; break;
        case 33: term->fg_color = COLOR_BROWN; break;
        case 34: term->fg_color = COLOR_BLUE; break;
        case 35: term->fg_color = COLOR_MAGENTA; break;
        case 36: term->fg_color = COLOR_CYAN; break;
        case 37: term->fg_color = COLOR_LIGHT_GRAY; break;
        case 39: term->fg_color = term->default_fg; break;
        
        // Background colors
        case 40: term->bg_color = COLOR_BLACK; break;
        case 41: term->bg_color = COLOR_RED; break;
        case 42: term->bg_color = COLOR_GREEN; break;
        case 43: term->bg_color = COLOR_BROWN; break;
        case 44: term->bg_color = COLOR_BLUE; break;
        case 45: term->bg_color = COLOR_MAGENTA; break;
        case 46: term->bg_color = COLOR_CYAN; break;
        case 47: term->bg_color = COLOR_LIGHT_GRAY; break;
        case 49: term->bg_color = term->default_bg; break;
        
        // Bright foreground (90-97)
        case 90: term->fg_color = COLOR_DARK_GRAY; break;
        case 91: term->fg_color = COLOR_LIGHT_RED; break;
        case 92: term->fg_color = COLOR_LIGHT_GREEN; break;
        case 93: term->fg_color = COLOR_YELLOW; break;
        case 94: term->fg_color = COLOR_LIGHT_BLUE; break;
        case 95: term->fg_color = COLOR_LIGHT_MAGENTA; break;
        case 96: term->fg_color = COLOR_LIGHT_CYAN; break;
        case 97: term->fg_color = COLOR_WHITE; break;
        
        // Bright background (100-107)
        case 100: term->bg_color = COLOR_DARK_GRAY; break;
        case 101: term->bg_color = COLOR_LIGHT_RED; break;
        case 102: term->bg_color = COLOR_LIGHT_GREEN; break;
        case 103: term->bg_color = COLOR_YELLOW; break;
        case 104: term->bg_color = COLOR_LIGHT_BLUE; break;
        case 105: term->bg_color = COLOR_LIGHT_MAGENTA; break;
        case 106: term->bg_color = COLOR_LIGHT_CYAN; break;
        case 107: term->bg_color = COLOR_WHITE; break;
        
        // 256 color mode: 38;5;n and 48;5;n
        case 38:
            if (i + 2 < term->esc_param_count && term->esc_params[i+1] == 5) {
                int color = term->esc_params[i+2];
                if (color < 16) {
                    term->fg_color = color;
                }
                i += 2;
            }
            break;
        case 48:
            if (i + 2 < term->esc_param_count && term->esc_params[i+1] == 5) {
                int color = term->esc_params[i+2];
                if (color < 16) {
                    term->bg_color = color;
                }
                i += 2;
            }
            break;
        }
    }
}

// CSI n - Device Status Report
static void term_csi_dsr(terminal_t *term) {
    int param = term_get_param(term, 0, 0);
    (void)param;
    // Would send response back through input
}

// Handle CSI sequence
static void term_handle_csi(terminal_t *term, char final) {
    int p1 = term_get_param(term, 0, 1);
    int p2 = term_get_param(term, 1, 1);
    
    term->escapes_processed++;
    
    switch (final) {
    case 'A':  // CUU - Cursor Up
        term_move_cursor(term, term->cursor_x, term->cursor_y - p1);
        break;
        
    case 'B':  // CUD - Cursor Down
        term_move_cursor(term, term->cursor_x, term->cursor_y + p1);
        break;
        
    case 'C':  // CUF - Cursor Forward
        term_move_cursor(term, term->cursor_x + p1, term->cursor_y);
        break;
        
    case 'D':  // CUB - Cursor Back
        term_move_cursor(term, term->cursor_x - p1, term->cursor_y);
        break;
        
    case 'E':  // CNL - Cursor Next Line
        term_move_cursor(term, 0, term->cursor_y + p1);
        break;
        
    case 'F':  // CPL - Cursor Previous Line
        term_move_cursor(term, 0, term->cursor_y - p1);
        break;
        
    case 'G':  // CHA - Cursor Horizontal Absolute
        term_move_cursor(term, p1 - 1, term->cursor_y);
        break;
        
    case 'H':  // CUP - Cursor Position
    case 'f':  // HVP - Horizontal and Vertical Position
        term_move_cursor(term, p2 - 1, p1 - 1);
        break;
        
    case 'J':  // ED - Erase in Display
        {
            int mode = term_get_param(term, 0, 0);
            int start_y, end_y;
            
            switch (mode) {
            case 0:  // Cursor to end of screen
                // Clear rest of current line
                for (int x = term->cursor_x; x < TERM_WIDTH; x++) {
                    term->screen[term->cursor_y][x].c = ' ';
                    term->screen[term->cursor_y][x].fg = term->fg_color;
                    term->screen[term->cursor_y][x].bg = term->bg_color;
                    term->screen[term->cursor_y][x].attr = 0;
                }
                // Clear lines below
                start_y = term->cursor_y + 1;
                end_y = TERM_HEIGHT;
                break;
            case 1:  // Start of screen to cursor
                // Clear lines above
                start_y = 0;
                end_y = term->cursor_y;
                // Clear beginning of current line
                for (int x = 0; x <= term->cursor_x; x++) {
                    term->screen[term->cursor_y][x].c = ' ';
                    term->screen[term->cursor_y][x].fg = term->fg_color;
                    term->screen[term->cursor_y][x].bg = term->bg_color;
                    term->screen[term->cursor_y][x].attr = 0;
                }
                break;
            case 2:  // Entire screen
            case 3:  // Entire screen + scrollback
                start_y = 0;
                end_y = TERM_HEIGHT;
                if (mode == 3) {
                    term->scrollback_lines = 0;
                    term->scrollback_write = 0;
                }
                break;
            default:
                return;
            }
            
            for (int y = start_y; y < end_y; y++) {
                for (int x = 0; x < TERM_WIDTH; x++) {
                    term->screen[y][x].c = ' ';
                    term->screen[y][x].fg = term->fg_color;
                    term->screen[y][x].bg = term->bg_color;
                    term->screen[y][x].attr = 0;
                }
            }
            
            term_render_screen(term);
        }
        break;
        
    case 'K':  // EL - Erase in Line
        {
            int mode = term_get_param(term, 0, 0);
            int start_x, end_x;
            
            switch (mode) {
            case 0: start_x = term->cursor_x; end_x = TERM_WIDTH; break;
            case 1: start_x = 0; end_x = term->cursor_x + 1; break;
            case 2: start_x = 0; end_x = TERM_WIDTH; break;
            default: return;
            }
            
            for (int x = start_x; x < end_x; x++) {
                term->screen[term->cursor_y][x].c = ' ';
                term->screen[term->cursor_y][x].fg = term->fg_color;
                term->screen[term->cursor_y][x].bg = term->bg_color;
                term->screen[term->cursor_y][x].attr = 0;
            }
            
            term_render_line(term, term->cursor_y);
        }
        break;
        
    case 'L':  // IL - Insert Lines
        term_scroll_down(term, term->cursor_y, term->scroll_bottom, p1);
        break;
        
    case 'M':  // DL - Delete Lines
        term_scroll_up(term, term->cursor_y, term->scroll_bottom, p1);
        break;
        
    case 'S':  // SU - Scroll Up
        term_scroll_up(term, term->scroll_top, term->scroll_bottom, p1);
        break;
        
    case 'T':  // SD - Scroll Down
        term_scroll_down(term, term->scroll_top, term->scroll_bottom, p1);
        break;
        
    case 'd':  // VPA - Vertical Line Position Absolute
        term_move_cursor(term, term->cursor_x, p1 - 1);
        break;
        
    case 'm':  // SGR - Select Graphic Rendition
        term_csi_sgr(term);
        break;
        
    case 'n':  // DSR - Device Status Report
        term_csi_dsr(term);
        break;
        
    case 'r':  // DECSTBM - Set Top and Bottom Margins
        {
            int top = term_get_param(term, 0, 1) - 1;
            int bottom = term_get_param(term, 1, TERM_HEIGHT) - 1;
            
            if (top < 0) top = 0;
            if (bottom >= TERM_HEIGHT) bottom = TERM_HEIGHT - 1;
            if (top < bottom) {
                term->scroll_top = top;
                term->scroll_bottom = bottom;
                term_move_cursor(term, 0, term->origin_mode ? top : 0);
            }
        }
        break;
        
    case 's':  // SCP - Save Cursor Position
        term->saved_cursor_x = term->cursor_x;
        term->saved_cursor_y = term->cursor_y;
        break;
        
    case 'u':  // RCP - Restore Cursor Position
        term_move_cursor(term, term->saved_cursor_x, term->saved_cursor_y);
        break;
        
    case 'h':  // SM - Set Mode
    case 'l':  // RM - Reset Mode
        {
            int set = (final == 'h');
            
            if (term->esc_private) {
                // DEC private modes
                switch (p1) {
                case 1:   // DECCKM - Cursor keys mode
                    term->cursor_keys_app = set;
                    break;
                case 6:   // DECOM - Origin mode
                    term->origin_mode = set;
                    term_move_cursor(term, 0, set ? term->scroll_top : 0);
                    break;
                case 7:   // DECAWM - Auto-wrap mode
                    term->autowrap = set;
                    break;
                case 25:  // DECTCEM - Cursor visibility
                    term->cursor_visible = set;
                    if (set) {
                        vga_enable_cursor(14, 15);
                        vga_set_cursor(term->cursor_x, term->cursor_y);
                    } else {
                        vga_disable_cursor();
                    }
                    break;
                }
            } else {
                // ANSI modes
                switch (p1) {
                case 4:   // IRM - Insert mode
                    term->insert_mode = set;
                    break;
                case 20:  // LNM - Linefeed mode
                    term->linefeed_mode = set;
                    break;
                }
            }
        }
        break;
    }
}

// ============================================================================
// CHARACTER OUTPUT
// ============================================================================

static void term_putchar_internal(terminal_t *term, char c) {
    unsigned char uc = (unsigned char)c;
    
    // Handle special characters
    switch (uc) {
    case '\0':
        return;
        
    case '\a':  // Bell
        // TODO: Beep
        return;
        
    case '\b':  // Backspace
        if (term->cursor_x > 0) {
            term->cursor_x--;
        }
        return;
        
    case '\t':  // Tab
        {
            int next_tab = term->cursor_x + 1;
            while (next_tab < TERM_WIDTH && !term->tabs[next_tab]) {
                next_tab++;
            }
            term->cursor_x = next_tab < TERM_WIDTH ? next_tab : TERM_WIDTH - 1;
        }
        return;
        
    case '\n':  // Newline
        term_newline(term);
        return;
        
    case '\r':  // Carriage return
        term->cursor_x = 0;
        return;
        
    case 0x1b:  // Escape (ESC) - use hex explicitly
        term->state = STATE_ESC;
        term->esc_param_count = 0;
        term->esc_intermediate_count = 0;
        term->esc_private = 0;
        memset(term->esc_params, 0, sizeof(term->esc_params));
        return;  // Don't print ESC character
    }
    
    // Regular character - print it
    if (term->insert_mode) {
        // Shift characters to the right
        for (int x = TERM_WIDTH - 1; x > term->cursor_x; x--) {
            term->screen[term->cursor_y][x] = term->screen[term->cursor_y][x - 1];
        }
    }
    
    term->screen[term->cursor_y][term->cursor_x].c = c;
    term->screen[term->cursor_y][term->cursor_x].fg = term->fg_color;
    term->screen[term->cursor_y][term->cursor_x].bg = term->bg_color;
    term->screen[term->cursor_y][term->cursor_x].attr = term->attr;
    
    term_render_cell(term, term->cursor_x, term->cursor_y);
    
    term->cursor_x++;
    term->chars_written++;
    
    // Handle wrap
    if (term->cursor_x >= TERM_WIDTH) {
        if (term->autowrap) {
            term->cursor_x = 0;
            term_newline(term);
        } else {
            term->cursor_x = TERM_WIDTH - 1;
        }
    }
    
    if (term->cursor_visible) {
        vga_set_cursor(term->cursor_x, term->cursor_y);
    }
}

// Main character processing with escape sequence parsing
void term_putchar(terminal_t *term, char c) {
    unsigned char uc = (unsigned char)c;
    
    switch (term->state) {
    case STATE_NORMAL:
        term_putchar_internal(term, c);
        break;
        
    case STATE_ESC:
        switch (uc) {
        case '[':
            term->state = STATE_CSI;
            break;
        case ']':
            term->state = STATE_OSC;
            break;
        case '(':
        case ')':
        case '*':
        case '+':
            term->state = STATE_CHARSET;
            break;
        case '7':  // DECSC - Save cursor
            term->saved_cursor_x = term->cursor_x;
            term->saved_cursor_y = term->cursor_y;
            term->state = STATE_NORMAL;
            break;
        case '8':  // DECRC - Restore cursor
            term_move_cursor(term, term->saved_cursor_x, term->saved_cursor_y);
            term->state = STATE_NORMAL;
            break;
        case 'D':  // IND - Index (down)
            term_newline(term);
            term->state = STATE_NORMAL;
            break;
        case 'E':  // NEL - Next line
            term->cursor_x = 0;
            term_newline(term);
            term->state = STATE_NORMAL;
            break;
        case 'M':  // RI - Reverse index (up)
            if (term->cursor_y <= term->scroll_top) {
                term_scroll_down(term, term->scroll_top, term->scroll_bottom, 1);
            } else {
                term->cursor_y--;
            }
            term->state = STATE_NORMAL;
            break;
        case 'c':  // RIS - Reset
            // Full terminal reset
            term->fg_color = term->default_fg;
            term->bg_color = term->default_bg;
            term->attr = 0;
            term->cursor_x = 0;
            term->cursor_y = 0;
            term->scroll_top = 0;
            term->scroll_bottom = TERM_HEIGHT - 1;
            term->state = STATE_NORMAL;
            break;
        default:
            term->state = STATE_NORMAL;
        }
        break;
        
    case STATE_CSI:
        if (c == '?') {
            term->esc_private = 1;
        } else if (c >= '0' && c <= '9') {
            term->state = STATE_CSI_PARAM;
            term->esc_params[0] = c - '0';
            term->esc_param_count = 1;
        } else if (c == ';') {
            term->state = STATE_CSI_PARAM;
            term->esc_param_count = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            term_handle_csi(term, c);
            term->state = STATE_NORMAL;
        } else {
            term->state = STATE_NORMAL;
        }
        break;
        
    case STATE_CSI_PARAM:
        if (c >= '0' && c <= '9') {
            int idx = term->esc_param_count > 0 ? term->esc_param_count - 1 : 0;
            term->esc_params[idx] = term->esc_params[idx] * 10 + (c - '0');
        } else if (c == ';') {
            if (term->esc_param_count < MAX_ESC_PARAMS) {
                term->esc_param_count++;
            }
        } else if (c >= 0x40 && c <= 0x7E) {
            term_handle_csi(term, c);
            term->state = STATE_NORMAL;
        } else {
            term->state = STATE_NORMAL;
        }
        break;
        
    case STATE_OSC:
        // OSC sequences end with BEL or ST
        if (c == '\a' || c == '\\') {
            term->state = STATE_NORMAL;
        }
        break;
        
    case STATE_CHARSET:
        // Single character charset selection, ignore
        term->state = STATE_NORMAL;
        break;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void term_write(terminal_t *term, const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        term_putchar(term, str[i]);
    }
}

void term_puts(terminal_t *term, const char *str) {
    while (*str) {
        term_putchar(term, *str++);
    }
}

void term_clear(terminal_t *term) {
    for (int y = 0; y < TERM_HEIGHT; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            term->screen[y][x].c = ' ';
            term->screen[y][x].fg = term->default_fg;
            term->screen[y][x].bg = term->default_bg;
            term->screen[y][x].attr = 0;
        }
    }
    term->cursor_x = 0;
    term->cursor_y = 0;
    term_render_screen(term);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void term_init(void) {
    terminal_t *term = &main_terminal;
    
    memset(term, 0, sizeof(*term));
    
    // Defaults
    term->default_fg = COLOR_LIGHT_GRAY;
    term->default_bg = COLOR_BLACK;
    term->fg_color = term->default_fg;
    term->bg_color = term->default_bg;
    
    term->cursor_visible = 1;
    term->autowrap = 1;
    term->scroll_top = 0;
    term->scroll_bottom = TERM_HEIGHT - 1;
    
    // Initialize tab stops (every 8 columns)
    for (int i = 0; i < TERM_WIDTH; i += TERM_TAB_WIDTH) {
        term->tabs[i] = 1;
    }
    
    // Allocate scrollback buffer
    term->scrollback = kmalloc(sizeof(term_cell_t) * TERM_WIDTH * TERM_SCROLLBACK, GFP_KERNEL);
    if (term->scrollback) {
        memset(term->scrollback, 0, sizeof(term_cell_t) * TERM_WIDTH * TERM_SCROLLBACK);
    }
    
    // Clear screen
    term_clear(term);
    
    // Enable cursor
    vga_enable_cursor(14, 15);
    
    // Mark as initialized BEFORE using printk (which routes here)
    terminal_initialized = 1;
    
    printk(KERN_INFO "Terminal initialized (%dx%d, %d lines scrollback)\n",
           TERM_WIDTH, TERM_HEIGHT, TERM_SCROLLBACK);
}

// Get main terminal for printk
terminal_t *term_get_main(void) {
    if (!terminal_initialized) {
        return NULL;
    }
    return &main_terminal;
}

// Print to main terminal (used by printk)
void term_kprint(const char *str) {
    term_puts(&main_terminal, str);
}
