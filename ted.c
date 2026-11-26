/*
 * ted
 * Copyright (C) k!M/pizslacker 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define TED_VERSION "0.8.5"
#define TED_MOUSE_SCROLL_LINES 3
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    SHIFT_ARROW_LEFT,
    SHIFT_ARROW_RIGHT,
    SHIFT_ARROW_UP,
    SHIFT_ARROW_DOWN,
    CTRL_ARROW_LEFT,
    CTRL_ARROW_RIGHT,
    CTRL_BACKSPACE,
    SHIFT_TAB,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    RESIZE_EVENT,
    MOUSE_EVENT
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*** data ***/

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

enum editOpType {
    OP_INSERT_CHAR,
    OP_DELETE_CHAR,
    OP_SPLIT_LINE,
    OP_JOIN_LINE,
    OP_INSERT_LINE,
    OP_DELETE_LINE,
    OP_REPLACE_RANGE
};

typedef struct editOp {
    enum editOpType type;
    int cy;             // Start row index
    int cx;             // Start column index
    int ch;             // Character for INSERT_CHAR / DELETE_CHAR
    char *indent;       // Line payload, auto-indent prefix, or inserted range text
    int indent_len;     // Length of indent/inserted range text
    char *old_text;     // Removed range text for OP_REPLACE_RANGE
    int old_len;        // Length of old_text
    int del_ey;         // End row index of deleted old_text
    int del_ex;         // End column index of deleted old_text
    int end_cy;         // End row index of inserted indent text
    int end_cx;         // End column index of inserted indent text
    int before_cx;      // Cursor X before edit
    int before_cy;      // Cursor Y before edit
    int after_cx;       // Cursor X after edit
    int after_cy;       // Cursor Y after edit
} editOp;

typedef struct erow {
    int idx;                // Row index within file (for multi-line state)
    int size;               // Length of raw chars
    int rsize;              // Length of rendered chars
    char *chars;            // Raw line content
    char *render;           // Rendered line content (tabs expanded)
    unsigned char *hl;      // Syntax highlight type per rendered byte
    int hl_open_comment;    // Non-zero if line ends inside an unclosed /* or """ comment
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    int saved_undo_top;             // Stack depth at last disk save
    editOp *undo_stack;             // Dynamic array of recorded edits
    int undo_top;                   // Number of active undoable operations
    int undo_len;                   // Total valid entries (undo_top .. undo_len = redoable)
    int undo_cap;                   // Allocated capacity of undo_stack
    int in_undo_redo;               // Re-entrancy guard during replay
    char *clipboard;                // Internal line or range clipboard buffer
    int clipboard_is_line;          // 1 if copied via Ctrl-K, 0 if range via Ctrl-C/X
    char *filename;
    char *config_path;              // Optional explicit config file path (-c/--config)
    char statusmsg[256];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
    volatile sig_atomic_t resized;

    // Visual selection state
    int sel_active;                 // 1 when selection anchor is active
    int sel_start_cx;               // Selection anchor column
    int sel_start_cy;               // Selection anchor row

    // XTerm SGR mouse event state
    int mouse_btn;                  // SGR button bitmask (0=left, 32=drag, 64=wheel up, 65=wheel down)
    int mouse_x;                    // 1-based terminal column
    int mouse_y;                    // 1-based terminal row
    int mouse_release;              // 1 if 'm' (button release), 0 if 'M' (press/drag/wheel)

    // Runtime configuration options (~/.tedrc)
    int tab_stop;                   // Default: 4 (clamped 1..16)
    int auto_indent;                // Default: 1
    int line_numbers;               // Default: 1
    int quit_times;                 // Default: 2 (clamped 0..10)
    int color_comment;              // Default: 36 (Cyan)
    int color_keyword;              // Default: 33 (Yellow)
    int color_type;                 // Default: 32 (Green)
    int color_string;               // Default: 35 (Magenta)
    int color_number;               // Default: 31 (Red)
    int color_match;                // Default: 34 (Blue)
    int color_gutter;               // Default: 90 (Dim gray)
};

struct editorConfig E;

/*** filetypes ***/

/* C / C++ */
char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "default", "sizeof", "goto", "do", "#include", "#define", "#ifdef",
    "#ifndef", "#endif", "#if", "#else", "#pragma",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "const|", "volatile|", "extern|", "size_t|",
    "ssize_t|", "uint8_t|", "uint16_t|", "uint32_t|", "uint64_t|",
    "int8_t|", "int16_t|", "int32_t|", "int64_t|", "bool|", "FILE|", NULL
};

/* Python */
char *PY_HL_extensions[] = { ".py", ".pyw", NULL };
char *PY_HL_keywords[] = {
    "def", "class", "if", "elif", "else", "for", "while", "break",
    "continue", "return", "import", "from", "as", "try", "except",
    "finally", "raise", "with", "yield", "lambda", "pass", "in", "is",
    "not", "and", "or", "async", "await", "global", "nonlocal", "del",
    "assert",

    "int|", "str|", "float|", "bool|", "list|", "dict|", "tuple|", "set|",
    "bytes|", "bytearray|", "None|", "True|", "False|", "self|", "cls|", NULL
};

/* POSIX Shell / Bash */
char *SH_HL_extensions[] = { ".sh", ".bash", ".zsh", NULL };
char *SH_HL_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "in", "function", "return", "exit", "break",
    "continue", "source",

    "local|", "export|", "readonly|", "declare|", "typeset|", "unset|",
    "shift|", "eval|", "exec|", "trap|", "set|", "echo|", "printf|",
    "read|", "test|", "true|", "false|", NULL
};

/* Makefile */
char *MK_HL_extensions[] = { "Makefile", "makefile", "GNUmakefile", ".mk", NULL };
char *MK_HL_keywords[] = {
    "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include",
    "-include", "sinclude", "define", "endef", "override", "export",
    "unexport", "vpath",

    ".PHONY|", ".DEFAULT_GOAL|", ".SUFFIXES|", ".SILENT|", ".ONESHELL|",
    "CC|", "CXX|", "CFLAGS|", "CXXFLAGS|", "LDFLAGS|", "LDLIBS|",
    "PREFIX|", "DESTDIR|", "BINDIR|", "MANDIR|", "SHELL|", "wildcard|",
    "patsubst|", "subst|", "shell|", "addprefix|", "addsuffix|", NULL
};

/* Rust */
char *RS_HL_extensions[] = { ".rs", NULL };
char *RS_HL_keywords[] = {
    "fn", "let", "mut", "const", "static", "if", "else", "match", "for",
    "while", "loop", "break", "continue", "return", "struct", "enum",
    "trait", "impl", "type", "pub", "mod", "use", "crate", "super",
    "where", "as", "in", "ref", "move", "async", "await", "unsafe", "extern",

    "i8|", "i16|", "i32|", "i64|", "i128|", "isize|", "u8|", "u16|",
    "u32|", "u64|", "u128|", "usize|", "f32|", "f64|", "bool|", "char|",
    "str|", "String|", "Vec|", "Option|", "Result|", "Some|", "None|",
    "Ok|", "Err|", "self|", "Self|", "true|", "false|", NULL
};

/* Go */
char *GO_HL_extensions[] = { ".go", NULL };
char *GO_HL_keywords[] = {
    "func", "var", "const", "type", "struct", "interface", "map", "chan",
    "package", "import", "if", "else", "switch", "case", "default", "for",
    "range", "break", "continue", "return", "goto", "fallthrough", "defer",
    "go", "select",

    "int|", "int8|", "int16|", "int32|", "int64|", "uint|", "uint8|",
    "uint16|", "uint32|", "uint64|", "uintptr|", "float32|", "float64|",
    "complex64|", "complex128|", "bool|", "byte|", "rune|", "string|",
    "error|", "any|", "true|", "false|", "nil|", "iota|", NULL
};

/* Zig */
char *ZIG_HL_extensions[] = { ".zig", NULL };
char *ZIG_HL_keywords[] = {
    "fn", "const", "var", "pub", "extern", "export", "inline", "noinline",
    "comptime", "struct", "enum", "union", "error", "if", "else", "switch",
    "while", "for", "break", "continue", "return", "defer", "errdefer",
    "try", "catch", "orelse", "and", "or", "unreachable", "test",
    "@import", "@cImport", "@as", "@intCast", "@ptrCast", "@sizeof",

    "i8|", "i16|", "i32|", "i64|", "i128|", "isize|", "u8|", "u16|",
    "u32|", "u64|", "u128|", "usize|", "f16|", "f32|", "f64|", "f128|",
    "bool|", "void|", "noreturn|", "type|", "anyerror|", "anyopaque|",
    "c_int|", "c_uint|", "c_char|", "true|", "false|", "null|", "undefined|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "python",
        PY_HL_extensions,
        PY_HL_keywords,
        "#", "\"\"\"", "\"\"\"",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "sh",
        SH_HL_extensions,
        SH_HL_keywords,
        "#", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "makefile",
        MK_HL_extensions,
        MK_HL_keywords,
        "#", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "rust",
        RS_HL_extensions,
        RS_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "go",
        GO_HL_extensions,
        GO_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "zig",
        ZIG_HL_extensions,
        ZIG_HL_keywords,
        "//", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));
int editorRowCxToRx(erow *row, int cx);
int editorRowRxToCx(erow *row, int rx);
void editorPushUndo(editOp op);

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }

    // Enable button press (1000), button drag motion (1002), and SGR coordinates (1006)
    write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (E.resized) {
            return RESIZE_EVENT;
        }
        if (nread == -1 && errno != EAGAIN && errno != EINTR) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[5];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (seq[0] == '\x7f' || seq[0] == '\b') return CTRL_BACKSPACE;

        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] == '<') {
                // SGR mouse packet: ESC [ < Cb ; Cx ; Cy (M or m)
                char mbuf[32];
                unsigned int mlen = 0;
                char term = 0;
                while (mlen < sizeof(mbuf) - 1) {
                    char ch;
                    if (read(STDIN_FILENO, &ch, 1) != 1) return '\x1b';
                    if (ch == 'M' || ch == 'm') {
                        term = ch;
                        break;
                    }
                    mbuf[mlen++] = ch;
                }
                mbuf[mlen] = '\0';

                int cb = 0, cx = 0, cy = 0;
                if (term && sscanf(mbuf, "%d;%d;%d", &cb, &cx, &cy) == 3) {
                    E.mouse_btn = cb;
                    E.mouse_x = cx;
                    E.mouse_y = cy;
                    E.mouse_release = (term == 'm') ? 1 : 0;
                    return MOUSE_EVENT;
                }
                return '\x1b';
            } else if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[1] == '1' && seq[2] == ';') {
                    // CSI 1;<mod><dir> (modifier 2 = Shift, 5 = Ctrl)
                    if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b';
                    if (seq[3] == '2') {
                        switch (seq[4]) {
                            case 'A': return SHIFT_ARROW_UP;
                            case 'B': return SHIFT_ARROW_DOWN;
                            case 'C': return SHIFT_ARROW_RIGHT;
                            case 'D': return SHIFT_ARROW_LEFT;
                        }
                    } else if (seq[3] == '5') {
                        switch (seq[4]) {
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                        }
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    case 'Z': return SHIFT_TAB;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'C': return CTRL_ARROW_RIGHT;
                case 'D': return CTRL_ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }

    if (c == CTRL_KEY('h')) {
        return CTRL_BACKSPACE;
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }

    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

void editorHandleSigwinch(int sig) {
    (void)sig;
    E.resized = 1;
}

void editorUpdateWindowSize(void) {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
    E.screenrows -= 2; // Reserve space for status bar and message bar

    if (E.screenrows < 1) E.screenrows = 1;
    if (E.screencols < 1) E.screencols = 1;

    // Clamp scroll offsets so the viewport stays valid when shrinking
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

/*** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];{}", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // Single-line comments
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        // Multi-line comments (and Python """ docstrings)
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // Strings and character literals
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // Numeric literals (integers, floats, and hex prefixes)
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER) ||
                ((c == 'x' || c == 'X') && prev_hl == HL_NUMBER &&
                 i > 0 && row->render[i - 1] == '0')) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // Keywords and types
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows) {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return E.color_comment;
        case HL_KEYWORD1:  return E.color_keyword;
        case HL_KEYWORD2:  return E.color_type;
        case HL_STRING:    return E.color_string;
        case HL_NUMBER:    return E.color_number;
        case HL_MATCH:     return E.color_match;
        default:           return 37;
    }
}

static void editorApplySyntax(struct editorSyntax *s) {
    E.syntax = s;
    for (int filerow = 0; filerow < E.numrows; filerow++) {
        editorUpdateSyntax(&E.row[filerow]);
    }
}

void editorSelectSyntaxHighlight(void) {
    E.syntax = NULL;

    if (E.filename != NULL) {
        const char *slash = strrchr(E.filename, '/');
        const char *base = slash ? slash + 1 : E.filename;
        char *ext = strrchr(base, '.');

        for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
            struct editorSyntax *s = &HLDB[j];
            for (unsigned int i = 0; s->filematch[i]; i++) {
                int is_ext = (s->filematch[i][0] == '.');
                if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                    (!is_ext && !strcmp(base, s->filematch[i]))) {
                    editorApplySyntax(s);
                    return;
                }
            }
        }
    }

    // Fallback: inspect shebang on line 1 (e.g., #!/usr/bin/env python3, #!/bin/sh)
    if (E.numrows > 0 && E.row[0].size > 2 &&
        E.row[0].chars[0] == '#' && E.row[0].chars[1] == '!') {
        const char *target = NULL;
        if (strstr(E.row[0].chars, "python")) {
            target = "python";
        } else if (strstr(E.row[0].chars, "bash") ||
                   strstr(E.row[0].chars, "zsh") ||
                   strstr(E.row[0].chars, "/sh")) {
            target = "sh";
        }

        if (target != NULL) {
            for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
                if (strcmp(HLDB[j].filetype, target) == 0) {
                    editorApplySyntax(&HLDB[j]);
                    return;
                }
            }
        }
    }
}

int editorFindMatchingBracket(int *out_row, int *out_rx) {
    if (E.cy >= E.numrows) return 0;
    erow *row = &E.row[E.cy];
    if (E.cx >= row->size) return 0;

    char open_ch = row->chars[E.cx];
    char close_ch = '\0';
    int dir = 0;

    switch (open_ch) {
        case '(': close_ch = ')'; dir = 1; break;
        case '[': close_ch = ']'; dir = 1; break;
        case '{': close_ch = '}'; dir = 1; break;
        case ')': close_ch = '('; dir = -1; break;
        case ']': close_ch = '['; dir = -1; break;
        case '}': close_ch = '{'; dir = -1; break;
        default: return 0;
    }

    // Ignore delimiters inside strings or comments
    int cur_rx = editorRowCxToRx(row, E.cx);
    if (cur_rx < row->rsize && row->hl != NULL) {
        unsigned char tok = row->hl[cur_rx];
        if (tok == HL_STRING || tok == HL_COMMENT || tok == HL_MLCOMMENT) {
            return 0;
        }
    }

    int depth = 1;
    int r = E.cy;
    int c = E.cx + dir;

    while (r >= 0 && r < E.numrows) {
        erow *cur = &E.row[r];
        while (c >= 0 && c < cur->size) {
            char ch = cur->chars[c];
            if (ch == open_ch || ch == close_ch) {
                int rx = editorRowCxToRx(cur, c);
                unsigned char tok = (rx < cur->rsize && cur->hl) ? cur->hl[rx] : HL_NORMAL;
                if (tok != HL_STRING && tok != HL_COMMENT && tok != HL_MLCOMMENT) {
                    if (ch == open_ch) {
                        depth++;
                    } else {
                        depth--;
                        if (depth == 0) {
                            *out_row = r;
                            *out_rx = rx;
                            return 1;
                        }
                    }
                }
            }
            c += dir;
        }
        r += dir;
        if (r >= 0 && r < E.numrows) {
            c = (dir == 1) ? 0 : (E.row[r].size - 1);
        }
    }

    return 0;
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (E.tab_stop - 1) - (rx % E.tab_stop);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (E.tab_stop - 1) - (cur_rx % E.tab_stop);
        }
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (E.tab_stop - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % E.tab_stop != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** selection & range helpers ***/

void editorClearSelection(void) {
    E.sel_active = 0;
}

int editorGetSelectionBounds(int *sy, int *sx, int *ey, int *ex) {
    if (!E.sel_active || E.numrows == 0) return 0;

    int a_cy = E.sel_start_cy;
    int a_cx = E.sel_start_cx;
    int b_cy = E.cy;
    int b_cx = E.cx;

    if (a_cy >= E.numrows) {
        a_cy = E.numrows - 1;
        a_cx = E.row[a_cy].size;
    }
    if (b_cy >= E.numrows) {
        b_cy = E.numrows - 1;
        b_cx = E.row[b_cy].size;
    }

    if (a_cy == b_cy && a_cx == b_cx) return 0;

    if (a_cy < b_cy || (a_cy == b_cy && a_cx <= b_cx)) {
        *sy = a_cy;
        *sx = a_cx;
        *ey = b_cy;
        *ex = b_cx;
    } else {
        *sy = b_cy;
        *sx = b_cx;
        *ey = a_cy;
        *ex = a_cx;
    }
    return 1;
}

char *editorExtractRange(int sy, int sx, int ey, int ex, int *out_len) {
    if (sy == ey) {
        int len = ex - sx;
        char *buf = malloc(len + 1);
        memcpy(buf, &E.row[sy].chars[sx], len);
        buf[len] = '\0';
        *out_len = len;
        return buf;
    }

    int total = (E.row[sy].size - sx) + 1;
    for (int r = sy + 1; r < ey; r++) {
        total += E.row[r].size + 1;
    }
    total += ex;

    char *buf = malloc(total + 1);
    char *p = buf;

    int first_len = E.row[sy].size - sx;
    memcpy(p, &E.row[sy].chars[sx], first_len);
    p += first_len;
    *p++ = '\n';

    for (int r = sy + 1; r < ey; r++) {
        memcpy(p, E.row[r].chars, E.row[r].size);
        p += E.row[r].size;
        *p++ = '\n';
    }

    memcpy(p, E.row[ey].chars, ex);
    p += ex;
    *p = '\0';

    *out_len = total;
    return buf;
}

void editorDeleteRangeRaw(int sy, int sx, int ey, int ex) {
    if (sy >= E.numrows) return;
    if (sy == ey) {
        erow *row = &E.row[sy];
        int count = ex - sx;
        if (count > 0) {
            memmove(&row->chars[sx], &row->chars[ex], row->size - ex + 1);
            row->size -= count;
            editorUpdateRow(row);
            E.dirty++;
        }
        return;
    }

    erow *first = &E.row[sy];
    first->size = sx;
    first->chars[sx] = '\0';

    erow *last = &E.row[ey];
    editorRowAppendString(first, &last->chars[ex], last->size - ex);

    for (int r = ey; r > sy; r--) {
        editorDelRow(r);
    }
}

void editorInsertRangeRaw(int sy, int sx, const char *text, int len,
                          int *out_ey, int *out_ex) {
    if (E.numrows == 0) {
        editorInsertRow(0, "", 0);
    }

    int cur_y = sy;
    int cur_x = sx;
    int seg_start = 0;

    for (int i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            int seg_len = i - seg_start;
            erow *row = &E.row[cur_y];
            row->chars = realloc(row->chars, row->size + seg_len + 1);
            memmove(&row->chars[cur_x + seg_len], &row->chars[cur_x],
                    row->size - cur_x + 1);
            memcpy(&row->chars[cur_x], &text[seg_start], seg_len);
            row->size += seg_len;
            editorUpdateRow(row);
            E.dirty++;
            cur_x += seg_len;

            if (i < len && text[i] == '\n') {
                row = &E.row[cur_y];
                int tail_len = row->size - cur_x;
                editorInsertRow(cur_y + 1, &row->chars[cur_x], tail_len);
                row = &E.row[cur_y];
                row->size = cur_x;
                row->chars[cur_x] = '\0';
                editorUpdateRow(row);
                cur_y++;
                cur_x = 0;
                seg_start = i + 1;
            }
        }
    }

    *out_ey = cur_y;
    *out_ex = cur_x;
}

int editorDeleteActiveSelection(void) {
    int sy, sx, ey, ex;
    if (!editorGetSelectionBounds(&sy, &sx, &ey, &ex)) return 0;

    int before_cx = E.cx;
    int before_cy = E.cy;
    int len = 0;
    char *old_text = editorExtractRange(sy, sx, ey, ex, &len);

    editorDeleteRangeRaw(sy, sx, ey, ex);
    editorClearSelection();
    E.cy = sy;
    E.cx = sx;

    editOp op = {
        .type = OP_REPLACE_RANGE,
        .cy = sy,
        .cx = sx,
        .ch = 0,
        .indent = NULL,
        .indent_len = 0,
        .old_text = old_text,
        .old_len = len,
        .del_ey = ey,
        .del_ex = ex,
        .end_cy = sy,
        .end_cx = sx,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    return 1;
}

/*** undo / redo ***/

void editorFreeEditOp(editOp *op) {
    free(op->indent);
    op->indent = NULL;
    free(op->old_text);
    op->old_text = NULL;
}

void editorClearUndoHistory(void) {
    for (int i = 0; i < E.undo_len; i++) {
        editorFreeEditOp(&E.undo_stack[i]);
    }
    free(E.undo_stack);
    E.undo_stack = NULL;
    E.undo_top = 0;
    E.undo_len = 0;
    E.undo_cap = 0;
    E.saved_undo_top = 0;
}

void editorPushUndo(editOp op) {
    if (E.in_undo_redo) return;

    // Discard any redoable operations above current undo_top
    for (int i = E.undo_top; i < E.undo_len; i++) {
        editorFreeEditOp(&E.undo_stack[i]);
    }
    E.undo_len = E.undo_top;

    // If we diverged past the saved checkpoint, saved state is no longer reachable
    if (E.saved_undo_top > E.undo_top) {
        E.saved_undo_top = -1;
    }

    if (E.undo_top == E.undo_cap) {
        E.undo_cap = E.undo_cap == 0 ? 64 : E.undo_cap * 2;
        E.undo_stack = realloc(E.undo_stack, sizeof(editOp) * E.undo_cap);
    }

    E.undo_stack[E.undo_top++] = op;
    E.undo_len = E.undo_top;
}

void editorUndo(void) {
    if (E.undo_top == 0) {
        editorSetStatusMessage("Nothing to undo");
        return;
    }

    editorClearSelection();
    E.in_undo_redo = 1;
    editOp *op = &E.undo_stack[--E.undo_top];

    switch (op->type) {
        case OP_INSERT_CHAR:
            editorRowDelChar(&E.row[op->cy], op->cx);
            break;

        case OP_DELETE_CHAR:
            editorRowInsertChar(&E.row[op->cy], op->cx, op->ch);
            break;

        case OP_SPLIT_LINE: {
            erow *next = &E.row[op->cy + 1];
            char *tail = &next->chars[op->indent_len];
            int tail_len = next->size - op->indent_len;
            editorRowAppendString(&E.row[op->cy], tail, tail_len);
            editorDelRow(op->cy + 1);
            break;
        }

        case OP_JOIN_LINE: {
            erow *prev = &E.row[op->cy - 1];
            editorInsertRow(op->cy, &prev->chars[op->cx], prev->size - op->cx);
            prev = &E.row[op->cy - 1];
            prev->size = op->cx;
            prev->chars[prev->size] = '\0';
            editorUpdateRow(prev);
            break;
        }

        case OP_INSERT_LINE:
            editorDelRow(op->cy);
            break;

        case OP_DELETE_LINE:
            editorInsertRow(op->cy, op->indent ? op->indent : "", op->indent_len);
            break;

        case OP_REPLACE_RANGE:
            if (op->indent_len > 0) {
                editorDeleteRangeRaw(op->cy, op->cx, op->end_cy, op->end_cx);
            }
            if (op->old_len > 0) {
                int dummy_y, dummy_x;
                editorInsertRangeRaw(op->cy, op->cx, op->old_text, op->old_len,
                                     &dummy_y, &dummy_x);
            }
            break;
    }

    E.cx = op->before_cx;
    E.cy = op->before_cy;
    E.dirty = (E.undo_top == E.saved_undo_top) ? 0 : 1;
    E.in_undo_redo = 0;

    editorSetStatusMessage("Undo (%d remaining)", E.undo_top);
}

void editorRedo(void) {
    if (E.undo_top >= E.undo_len) {
        editorSetStatusMessage("Nothing to redo");
        return;
    }

    editorClearSelection();
    E.in_undo_redo = 1;
    editOp *op = &E.undo_stack[E.undo_top++];

    switch (op->type) {
        case OP_INSERT_CHAR:
            if (op->cy == E.numrows) {
                editorInsertRow(E.numrows, "", 0);
            }
            editorRowInsertChar(&E.row[op->cy], op->cx, op->ch);
            break;

        case OP_DELETE_CHAR:
            editorRowDelChar(&E.row[op->cy], op->cx);
            break;

        case OP_SPLIT_LINE: {
            if (op->cy == E.numrows) {
                editorInsertRow(op->cy, "", 0);
            } else {
                erow *row = &E.row[op->cy];
                int tail_len = row->size - op->cx;
                int new_len = op->indent_len + tail_len;
                char *new_line = malloc(new_len + 1);

                if (op->indent_len > 0) {
                    memcpy(new_line, op->indent, op->indent_len);
                }
                memcpy(new_line + op->indent_len, &row->chars[op->cx], tail_len);
                new_line[new_len] = '\0';

                editorInsertRow(op->cy + 1, new_line, new_len);
                free(new_line);

                row = &E.row[op->cy];
                row->size = op->cx;
                row->chars[row->size] = '\0';
                editorUpdateRow(row);
            }
            break;
        }

        case OP_JOIN_LINE: {
            erow *row = &E.row[op->cy];
            editorRowAppendString(&E.row[op->cy - 1], row->chars, row->size);
            editorDelRow(op->cy);
            break;
        }

        case OP_INSERT_LINE:
            editorInsertRow(op->cy, op->indent ? op->indent : "", op->indent_len);
            break;

        case OP_DELETE_LINE:
            editorDelRow(op->cy);
            break;

        case OP_REPLACE_RANGE:
            if (op->old_len > 0) {
                editorDeleteRangeRaw(op->cy, op->cx, op->del_ey, op->del_ex);
            }
            if (op->indent_len > 0) {
                int dummy_y, dummy_x;
                editorInsertRangeRaw(op->cy, op->cx, op->indent, op->indent_len,
                                     &dummy_y, &dummy_x);
            }
            break;
    }

    E.cx = op->after_cx;
    E.cy = op->after_cy;
    E.dirty = (E.undo_top == E.saved_undo_top) ? 0 : 1;
    E.in_undo_redo = 0;

    editorSetStatusMessage("Redo (%d available)", E.undo_len - E.undo_top);
}

/*** editor operations ***/

void editorInsertChar(int c) {
    editorDeleteActiveSelection();

    int before_cx = E.cx;
    int before_cy = E.cy;

    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;

    editOp op = {
        .type = OP_INSERT_CHAR,
        .cy = before_cy,
        .cx = before_cx,
        .ch = c,
        .indent = NULL,
        .indent_len = 0,
        .old_text = NULL,
        .old_len = 0,
        .del_ey = 0,
        .del_ex = 0,
        .end_cy = 0,
        .end_cx = 0,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
}

void editorInsertNewline(void) {
    editorDeleteActiveSelection();

    int before_cx = E.cx;
    int before_cy = E.cy;

    if (E.cy == E.numrows) {
        editorInsertRow(E.cy, "", 0);
        E.cy++;
        E.cx = 0;

        editOp op = {
            .type = OP_SPLIT_LINE,
            .cy = before_cy,
            .cx = before_cx,
            .ch = 0,
            .indent = NULL,
            .indent_len = 0,
            .old_text = NULL,
            .old_len = 0,
            .del_ey = 0,
            .del_ex = 0,
            .end_cy = 0,
            .end_cx = 0,
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
        return;
    }

    erow *row = &E.row[E.cy];
    int indent = 0;
    int extra_indent = 0;

    if (E.auto_indent) {
        while (indent < E.cx &&
               (row->chars[indent] == ' ' || row->chars[indent] == '\t')) {
            indent++;
        }

        int check = E.cx - 1;
        while (check >= 0 && (row->chars[check] == ' ' || row->chars[check] == '\t')) {
            check--;
        }
        if (check >= 0 && row->chars[check] == '{') {
            extra_indent = E.tab_stop;
        }
    }

    int total_indent = indent + extra_indent;
    int tail_len = row->size - E.cx;
    int new_len = total_indent + tail_len;
    char *new_line = malloc(new_len + 1);

    memcpy(new_line, row->chars, indent);
    memset(new_line + indent, ' ', extra_indent);
    memcpy(new_line + total_indent, &row->chars[E.cx], tail_len);
    new_line[new_len] = '\0';

    char *saved_indent = NULL;
    if (total_indent > 0) {
        saved_indent = malloc(total_indent + 1);
        memcpy(saved_indent, new_line, total_indent);
        saved_indent[total_indent] = '\0';
    }

    editorInsertRow(E.cy + 1, new_line, new_len);
    free(new_line);

    // Re-fetch row pointer after realloc in editorInsertRow
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);

    E.cy++;
    E.cx = total_indent;

    editOp op = {
        .type = OP_SPLIT_LINE,
        .cy = before_cy,
        .cx = before_cx,
        .ch = 0,
        .indent = saved_indent,
        .indent_len = total_indent,
        .old_text = NULL,
        .old_len = 0,
        .del_ey = 0,
        .del_ex = 0,
        .end_cy = 0,
        .end_cx = 0,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
}

void editorDelChar(void) {
    if (editorDeleteActiveSelection()) return;
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    int before_cx = E.cx;
    int before_cy = E.cy;
    erow *row = &E.row[E.cy];

    if (E.cx > 0) {
        int deleted_ch = (unsigned char)row->chars[E.cx - 1];
        editorRowDelChar(row, E.cx - 1);
        E.cx--;

        editOp op = {
            .type = OP_DELETE_CHAR,
            .cy = before_cy,
            .cx = E.cx,
            .ch = deleted_ch,
            .indent = NULL,
            .indent_len = 0,
            .old_text = NULL,
            .old_len = 0,
            .del_ey = 0,
            .del_ex = 0,
            .end_cy = 0,
            .end_cx = 0,
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
    } else {
        int join_col = E.row[E.cy - 1].size;
        E.cx = join_col;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;

        editOp op = {
            .type = OP_JOIN_LINE,
            .cy = before_cy,
            .cx = join_col,
            .ch = 0,
            .indent = NULL,
            .indent_len = 0,
            .old_text = NULL,
            .old_len = 0,
            .del_ey = 0,
            .del_ex = 0,
            .end_cy = 0,
            .end_cx = 0,
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
    }
}

void editorMoveWord(int key) {
    editorClearSelection();
    if (key == CTRL_ARROW_LEFT) {
        if (E.cx == 0) {
            if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            return;
        }
        erow *row = &E.row[E.cy];
        while (E.cx > 0 && is_separator(row->chars[E.cx - 1])) {
            E.cx--;
        }
        while (E.cx > 0 && !is_separator(row->chars[E.cx - 1])) {
            E.cx--;
        }
    } else if (key == CTRL_ARROW_RIGHT) {
        if (E.cy >= E.numrows) return;
        erow *row = &E.row[E.cy];
        if (E.cx >= row->size) {
            if (E.cy < E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            return;
        }
        while (E.cx < row->size && !is_separator(row->chars[E.cx])) {
            E.cx++;
        }
        while (E.cx < row->size && is_separator(row->chars[E.cx])) {
            E.cx++;
        }
    }
}

void editorDelWord(void) {
    if (editorDeleteActiveSelection()) return;
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    if (E.cx == 0) {
        editorDelChar();
        return;
    }

    erow *row = &E.row[E.cy];
    int target_cx = E.cx;

    while (target_cx > 0 && is_separator(row->chars[target_cx - 1])) {
        target_cx--;
    }
    while (target_cx > 0 && !is_separator(row->chars[target_cx - 1])) {
        target_cx--;
    }

    int count = E.cx - target_cx;
    while (count-- > 0) {
        editorDelChar();
    }
}

void editorIndent(void) {
    int sy, sx, ey, ex;
    if (editorGetSelectionBounds(&sy, &sx, &ey, &ex) && sy != ey) {
        int end_row = (ex == 0 && ey > sy) ? ey - 1 : ey;
        int before_cx = E.cx;
        int before_cy = E.cy;
        int end_col = E.row[end_row].size;

        int old_len = 0;
        char *old_text = editorExtractRange(sy, 0, end_row, end_col, &old_len);

        char spaces[17];
        memset(spaces, ' ', E.tab_stop);
        spaces[E.tab_stop] = '\0';

        for (int r = sy; r <= end_row; r++) {
            int dummy_y, dummy_x;
            editorInsertRangeRaw(r, 0, spaces, E.tab_stop, &dummy_y, &dummy_x);
        }

        if (E.sel_start_cy <= end_row) E.sel_start_cx += E.tab_stop;
        if (E.cy <= end_row) E.cx += E.tab_stop;

        int new_end_col = E.row[end_row].size;
        int new_len = 0;
        char *new_text = editorExtractRange(sy, 0, end_row, new_end_col, &new_len);

        editOp op = {
            .type = OP_REPLACE_RANGE,
            .cy = sy,
            .cx = 0,
            .ch = 0,
            .indent = new_text,
            .indent_len = new_len,
            .old_text = old_text,
            .old_len = old_len,
            .del_ey = end_row,
            .del_ex = end_col,
            .end_cy = end_row,
            .end_cx = new_end_col,
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
        return;
    }

    editorClearSelection();
    int spaces = E.tab_stop - (E.cx % E.tab_stop);
    for (int i = 0; i < spaces; i++) {
        editorInsertChar(' ');
    }
}

void editorOutdent(void) {
    int sy, sx, ey, ex;
    if (editorGetSelectionBounds(&sy, &sx, &ey, &ex) && sy != ey) {
        int end_row = (ex == 0 && ey > sy) ? ey - 1 : ey;
        int before_cx = E.cx;
        int before_cy = E.cy;
        int end_col = E.row[end_row].size;

        int old_len = 0;
        char *old_text = editorExtractRange(sy, 0, end_row, end_col, &old_len);

        int any_removed = 0;
        for (int r = sy; r <= end_row; r++) {
            erow *row = &E.row[r];
            int removed = 0;
            if (row->size > 0 && row->chars[0] == '\t') {
                editorRowDelChar(row, 0);
                removed = 1;
            } else {
                while (removed < E.tab_stop && row->size > 0 && row->chars[0] == ' ') {
                    editorRowDelChar(row, 0);
                    removed++;
                }
            }
            if (removed > 0) {
                any_removed = 1;
                if (E.sel_start_cy == r) {
                    E.sel_start_cx = (E.sel_start_cx > removed) ? (E.sel_start_cx - removed) : 0;
                }
                if (E.cy == r) {
                    E.cx = (E.cx > removed) ? (E.cx - removed) : 0;
                }
            }
        }

        if (!any_removed) {
            free(old_text);
            return;
        }

        int new_end_col = E.row[end_row].size;
        int new_len = 0;
        char *new_text = editorExtractRange(sy, 0, end_row, new_end_col, &new_len);

        editOp op = {
            .type = OP_REPLACE_RANGE,
            .cy = sy,
            .cx = 0,
            .ch = 0,
            .indent = new_text,
            .indent_len = new_len,
            .old_text = old_text,
            .old_len = old_len,
            .del_ey = end_row,
            .del_ex = end_col,
            .end_cy = end_row,
            .end_cx = new_end_col,
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
        return;
    }

    editorClearSelection();
    if (E.cy >= E.numrows) return;
    erow *row = &E.row[E.cy];
    if (row->size == 0) return;

    int removed = 0;
    if (row->chars[0] == '\t') {
        editorRowDelChar(row, 0);
        editOp op = {
            .type = OP_DELETE_CHAR,
            .cy = E.cy,
            .cx = 0,
            .ch = '\t',
            .indent = NULL,
            .indent_len = 0,
            .old_text = NULL,
            .old_len = 0,
            .del_ey = 0,
            .del_ex = 0,
            .end_cy = 0,
            .end_cx = 0,
            .before_cx = E.cx,
            .before_cy = E.cy,
            .after_cx = E.cx > 0 ? E.cx - 1 : 0,
            .after_cy = E.cy
        };
        editorPushUndo(op);
        removed = 1;
    } else {
        while (removed < E.tab_stop && row->size > 0 && row->chars[0] == ' ') {
            int before_cx = E.cx > removed ? E.cx - removed : 0;
            int after_cx = before_cx > 0 ? before_cx - 1 : 0;

            editorRowDelChar(row, 0);
            editOp op = {
                .type = OP_DELETE_CHAR,
                .cy = E.cy,
                .cx = 0,
                .ch = ' ',
                .indent = NULL,
                .indent_len = 0,
                .old_text = NULL,
                .old_len = 0,
                .del_ey = 0,
                .del_ex = 0,
                .end_cy = 0,
                .end_cx = 0,
                .before_cx = before_cx,
                .before_cy = E.cy,
                .after_cx = after_cx,
                .after_cy = E.cy
            };
            editorPushUndo(op);
            removed++;
        }
    }

    if (removed > 0) {
        E.cx -= removed;
        if (E.cx < 0) E.cx = 0;
    }
}

void editorDuplicateLine(void) {
    editorClearSelection();
    if (E.cy >= E.numrows) return;

    int before_cx = E.cx;
    int before_cy = E.cy;
    erow *row = &E.row[E.cy];

    int len = row->size;
    char *copy = malloc(len + 1);
    memcpy(copy, row->chars, len);
    copy[len] = '\0';

    editorInsertRow(E.cy + 1, copy, len);
    E.cy++;
    if (E.cx > E.row[E.cy].size) {
        E.cx = E.row[E.cy].size;
    }

    editOp op = {
        .type = OP_INSERT_LINE,
        .cy = E.cy,
        .cx = 0,
        .ch = 0,
        .indent = copy,
        .indent_len = len,
        .old_text = NULL,
        .old_len = 0,
        .del_ey = 0,
        .del_ex = 0,
        .end_cy = 0,
        .end_cx = 0,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    editorSetStatusMessage("Line duplicated");
}

void editorCutLine(void) {
    editorClearSelection();
    if (E.cy >= E.numrows) return;

    int before_cx = E.cx;
    int before_cy = E.cy;
    erow *row = &E.row[E.cy];

    int len = row->size;
    char *saved = malloc(len + 1);
    memcpy(saved, row->chars, len);
    saved[len] = '\0';

    free(E.clipboard);
    E.clipboard = malloc(len + 1);
    memcpy(E.clipboard, saved, len + 1);
    E.clipboard_is_line = 1;

    editorDelRow(E.cy);
    if (E.cy >= E.numrows && E.numrows > 0) {
        E.cy = E.numrows - 1;
    }
    if (E.numrows == 0) {
        E.cy = 0;
        E.cx = 0;
    } else if (E.cx > E.row[E.cy].size) {
        E.cx = E.row[E.cy].size;
    }

    editOp op = {
        .type = OP_DELETE_LINE,
        .cy = before_cy,
        .cx = 0,
        .ch = 0,
        .indent = saved,
        .indent_len = len,
        .old_text = NULL,
        .old_len = 0,
        .del_ey = 0,
        .del_ex = 0,
        .end_cy = 0,
        .end_cx = 0,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    editorSetStatusMessage("Line cut (Ctrl-U / Ctrl-V to paste)");
}

void editorPasteLine(void) {
    editorClearSelection();
    if (E.clipboard == NULL) {
        editorSetStatusMessage("Clipboard is empty");
        return;
    }

    int before_cx = E.cx;
    int before_cy = E.cy;
    int target_row = (E.cy < E.numrows) ? E.cy + 1 : E.numrows;
    int len = (int)strlen(E.clipboard);

    char *copy = malloc(len + 1);
    memcpy(copy, E.clipboard, len + 1);

    editorInsertRow(target_row, copy, len);
    E.cy = target_row;
    if (E.cx > E.row[E.cy].size) {
        E.cx = E.row[E.cy].size;
    }

    editOp op = {
        .type = OP_INSERT_LINE,
        .cy = E.cy,
        .cx = 0,
        .ch = 0,
        .indent = copy,
        .indent_len = len,
        .old_text = NULL,
        .old_len = 0,
        .del_ey = 0,
        .del_ex = 0,
        .end_cy = 0,
        .end_cx = 0,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    editorSetStatusMessage("Line pasted");
}

void editorCopySelection(void) {
    int sy, sx, ey, ex;
    if (!editorGetSelectionBounds(&sy, &sx, &ey, &ex)) {
        editorSetStatusMessage("No text selected");
        return;
    }

    int len = 0;
    char *text = editorExtractRange(sy, sx, ey, ex, &len);
    free(E.clipboard);
    E.clipboard = text;
    E.clipboard_is_line = 0;
    editorSetStatusMessage("Copied %d byte(s)", len);
}

void editorCutSelection(void) {
    int sy, sx, ey, ex;
    if (!editorGetSelectionBounds(&sy, &sx, &ey, &ex)) {
        editorSetStatusMessage("No text selected");
        return;
    }

    int before_cx = E.cx;
    int before_cy = E.cy;
    int len = 0;
    char *text = editorExtractRange(sy, sx, ey, ex, &len);

    free(E.clipboard);
    E.clipboard = malloc(len + 1);
    memcpy(E.clipboard, text, len + 1);
    E.clipboard_is_line = 0;

    editorDeleteRangeRaw(sy, sx, ey, ex);
    editorClearSelection();
    E.cy = sy;
    E.cx = sx;

    editOp op = {
        .type = OP_REPLACE_RANGE,
        .cy = sy,
        .cx = sx,
        .ch = 0,
        .indent = NULL,
        .indent_len = 0,
        .old_text = text,
        .old_len = len,
        .del_ey = ey,
        .del_ex = ex,
        .end_cy = sy,
        .end_cx = sx,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    editorSetStatusMessage("Cut %d byte(s)", len);
}

void editorPasteClipboard(void) {
    if (E.clipboard == NULL) {
        editorSetStatusMessage("Clipboard is empty");
        return;
    }

    if (E.clipboard_is_line) {
        editorPasteLine();
        return;
    }

    int before_cx = E.cx;
    int before_cy = E.cy;
    int sy = E.cy;
    int sx = E.cx;
    char *old_text = NULL;
    int old_len = 0;
    int del_ey = sy;
    int del_ex = sx;

    if (editorGetSelectionBounds(&sy, &sx, &del_ey, &del_ex)) {
        old_text = editorExtractRange(sy, sx, del_ey, del_ex, &old_len);
        editorDeleteRangeRaw(sy, sx, del_ey, del_ex);
        editorClearSelection();
    }

    int ins_len = (int)strlen(E.clipboard);
    char *ins_copy = malloc(ins_len + 1);
    memcpy(ins_copy, E.clipboard, ins_len + 1);

    int new_ey = sy;
    int new_ex = sx;
    editorInsertRangeRaw(sy, sx, ins_copy, ins_len, &new_ey, &new_ex);
    E.cy = new_ey;
    E.cx = new_ex;

    editOp op = {
        .type = OP_REPLACE_RANGE,
        .cy = sy,
        .cx = sx,
        .ch = 0,
        .indent = ins_copy,
        .indent_len = ins_len,
        .old_text = old_text,
        .old_len = old_len,
        .del_ey = del_ey,
        .del_ex = del_ex,
        .end_cy = new_ey,
        .end_cx = new_ex,
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
    editorSetStatusMessage("Pasted %d byte(s)", ins_len);
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    editorClearSelection();
    editorClearUndoHistory();
    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) {
            E.dirty = 0;
            editorSetStatusMessage("New file: %s", filename);
            return;
        }
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);

    // Re-evaluate syntax in case an extensionless file starts with a #! shebang
    if (E.syntax == NULL) {
        editorSelectSyntaxHighlight();
    }

    editorClearUndoHistory();
    E.dirty = 0;
}

void editorSave(void) {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    } else if (E.syntax == NULL) {
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                E.saved_undo_top = E.undo_top;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find & navigation ***/

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    // Restore original syntax colors on the previously highlighted line
    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(void) {
    editorClearSelection();
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
                               editorFindCallback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

void editorGotoLine(void) {
    if (E.numrows == 0) {
        editorSetStatusMessage("Buffer is empty");
        return;
    }

    editorClearSelection();
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Go to line (1-%d): %%s (ESC to cancel)", E.numrows);

    char *input = editorPrompt(prompt, NULL);
    if (input == NULL) return;

    char *endptr;
    errno = 0;
    long line = strtol(input, &endptr, 10);

    while (*endptr == ' ' || *endptr == '\t') endptr++;
    if (errno != 0 || endptr == input || *endptr != '\0') {
        editorSetStatusMessage("Invalid line number: '%s'", input);
        free(input);
        return;
    }

    free(input);

    if (line < 1) line = 1;
    if (line > E.numrows) line = E.numrows;

    E.cy = (int)(line - 1);
    E.cx = 0;

    if (E.cy < E.rowoff || E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - (E.screenrows / 2);
        if (E.rowoff < 0) E.rowoff = 0;
        if (E.rowoff > E.numrows - E.screenrows) {
            E.rowoff = E.numrows - E.screenrows;
            if (E.rowoff < 0) E.rowoff = 0;
        }
    }

    editorSetStatusMessage("Jumped to line %ld", line);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new_buf = realloc(ab->b, ab->len + len);
    if (new_buf == NULL) return;

    memcpy(&new_buf[ab->len], s, len);
    ab->b = new_buf;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

int editorGutterWidth(void) {
    if (!E.line_numbers) return 0;

    int lines = E.numrows > 0 ? E.numrows : 1;
    int digits = 0;
    while (lines > 0) {
        lines /= 10;
        digits++;
    }
    if (digits < 3) digits = 3;
    return digits + 1; // Digits + 1 trailing space separator
}

void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    int textcols = E.screencols - editorGutterWidth();
    if (textcols < 1) textcols = 1;

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + textcols) {
        E.coloff = E.rx - textcols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int gutter = editorGutterWidth();
    int numwidth = gutter > 0 ? gutter - 1 : 0;
    int textcols = E.screencols - gutter;
    if (textcols < 0) textcols = 0;

    int match_row = -1, match_rx = -1;
    int has_match = editorFindMatchingBracket(&match_row, &match_rx);

    int sel_sy = 0, sel_sx = 0, sel_ey = 0, sel_ex = 0;
    int has_sel = editorGetSelectionBounds(&sel_sy, &sel_sx, &sel_ey, &sel_ex);

    // Classic slant-style ASCII banner (all lines exact width: 22 chars)
    static const char *logo[] = {
        "  _            _      ",
        " | |_ ___   __| |     ",
        " | __/ _ \\ / _` |     ",
        " | ||  __/| (_| |     ",
        "  \\__\\___| \\__,_|     "
    };
    const int logo_rows = (int)(sizeof(logo) / sizeof(logo[0]));
    const int logo_width = 22;

    char version_line[64];
    int ver_len = snprintf(version_line, sizeof(version_line),
                           "TinyEDitor  v%s", TED_VERSION);

    const char *help_line = "Ctrl-S: Save | Ctrl-F: Find | Ctrl-G: Goto | Ctrl-Z/Y: Undo/Redo | Ctrl-Q: Quit";
    const int help_len = (int)strlen(help_line);

    const int total_banner_rows = logo_rows + 4;
    int banner_start_y = (E.screenrows - total_banner_rows) / 2;
    if (banner_start_y < 1) banner_start_y = 1;

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (gutter > 0) {
                char gbuf[32];
                int glen = snprintf(gbuf, sizeof(gbuf), "\x1b[%dm%*s \x1b[39m",
                                    E.color_gutter, numwidth, "~");
                abAppend(ab, gbuf, glen);
            } else {
                char gbuf[32];
                int glen = snprintf(gbuf, sizeof(gbuf), "\x1b[%dm~\x1b[39m",
                                    E.color_gutter);
                abAppend(ab, gbuf, glen);
            }

            if (E.numrows == 0 &&
                E.screenrows >= total_banner_rows + 2 &&
                textcols >= help_len + 2) {
                int rel_y = y - banner_start_y;

                if (rel_y >= 0 && rel_y < logo_rows) {
                    int pad = (textcols - logo_width) / 2;
                    while (pad-- > 0) abAppend(ab, " ", 1);
                    abAppend(ab, "\x1b[1;36m", 7); // Bold cyan
                    abAppend(ab, logo[rel_y], logo_width);
                    abAppend(ab, "\x1b[0m", 4);
                } else if (rel_y == logo_rows + 1) {
                    int pad = (textcols - ver_len) / 2;
                    while (pad-- > 0) abAppend(ab, " ", 1);
                    abAppend(ab, "\x1b[1;37m", 7); // Bold white
                    abAppend(ab, version_line, ver_len);
                    abAppend(ab, "\x1b[0m", 4);
                } else if (rel_y == logo_rows + 3) {
                    int pad = (textcols - help_len) / 2;
                    while (pad-- > 0) abAppend(ab, " ", 1);
                    abAppend(ab, "\x1b[90m", 5); // Dim gray
                    abAppend(ab, help_line, help_len);
                    abAppend(ab, "\x1b[0m", 4);
                }
            }
        } else {
            if (gutter > 0) {
                char gbuf[32];
                int glen = snprintf(gbuf, sizeof(gbuf), "\x1b[%dm%*d \x1b[39m",
                                    E.color_gutter, numwidth, filerow + 1);
                abAppend(ab, gbuf, glen);
            }

            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > textcols) len = textcols;

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;

            for (int j = 0; j < len; j++) {
                int abs_rx = j + E.coloff;
                int raw_cx = editorRowRxToCx(&E.row[filerow], abs_rx);

                int in_sel = 0;
                if (has_sel && filerow >= sel_sy && filerow <= sel_ey) {
                    if (sel_sy == sel_ey) {
                        in_sel = (raw_cx >= sel_sx && raw_cx < sel_ex);
                    } else if (filerow == sel_sy) {
                        in_sel = (raw_cx >= sel_sx);
                    } else if (filerow == sel_ey) {
                        in_sel = (raw_cx < sel_ex);
                    } else {
                        in_sel = 1;
                    }
                }

                int is_bracket = has_match &&
                    ((filerow == E.cy && abs_rx == E.rx) ||
                     (filerow == match_row && abs_rx == match_rx));

                if (in_sel) {
                    abAppend(ab, "\x1b[7m", 4);
                    char out_ch = iscntrl(c[j]) ? ((c[j] <= 26) ? '@' + c[j] : '?') : c[j];
                    abAppend(ab, &out_ch, 1);
                    abAppend(ab, "\x1b[27m", 5);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (is_bracket) {
                    abAppend(ab, "\x1b[1;7m", 6); // Bold + reverse video
                    abAppend(ab, &c[j], 1);
                    abAppend(ab, "\x1b[22;27m", 8);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows,
                       E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        E.syntax ? E.syntax->filetype : "no ft",
                        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen(void) {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + editorGutterWidth() + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** config ***/

static char *trimWhitespace(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

static int parseBool(const char *val, int *out) {
    if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
        strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
        strcasecmp(val, "no") == 0 || strcasecmp(val, "off") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parseAnsiColor(const char *val, int *out) {
    if (strcasecmp(val, "black") == 0)   { *out = 30; return 1; }
    if (strcasecmp(val, "red") == 0)     { *out = 31; return 1; }
    if (strcasecmp(val, "green") == 0)   { *out = 32; return 1; }
    if (strcasecmp(val, "yellow") == 0)  { *out = 33; return 1; }
    if (strcasecmp(val, "blue") == 0)    { *out = 34; return 1; }
    if (strcasecmp(val, "magenta") == 0) { *out = 35; return 1; }
    if (strcasecmp(val, "cyan") == 0)    { *out = 36; return 1; }
    if (strcasecmp(val, "white") == 0)   { *out = 37; return 1; }
    if (strcasecmp(val, "gray") == 0 || strcasecmp(val, "grey") == 0) {
        *out = 90;
        return 1;
    }

    char *endptr;
    errno = 0;
    long code = strtol(val, &endptr, 10);
    if (errno != 0 || endptr == val || *endptr != '\0') return 0;

    if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
        *out = (int)code;
        return 1;
    }
    return 0;
}

static int parseIntRange(const char *val, int min_v, int max_v, int *out) {
    char *endptr;
    errno = 0;
    long n = strtol(val, &endptr, 10);
    if (errno != 0 || endptr == val || *endptr != '\0') return 0;
    if (n < min_v || n > max_v) return 0;
    *out = (int)n;
    return 1;
}

static int editorResolveConfigPath(char *out, size_t out_size, const char *override_path) {
    if (override_path != NULL) {
        return snprintf(out, out_size, "%s", override_path) < (int)out_size;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw != NULL) home = pw->pw_dir;
    }
    if (home == NULL || home[0] == '\0') return 0;

    // Prefer ~/.tedrc if present; otherwise check $XDG_CONFIG_HOME/ted/tedrc
    if (snprintf(out, out_size, "%s/.tedrc", home) < (int)out_size &&
        access(out, R_OK) == 0) {
        return 1;
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        if (snprintf(out, out_size, "%s/ted/tedrc", xdg) < (int)out_size &&
            access(out, R_OK) == 0) {
            return 1;
        }
    } else if (snprintf(out, out_size, "%s/.config/ted/tedrc", home) < (int)out_size &&
               access(out, R_OK) == 0) {
        return 1;
    }

    return 0;
}

int editorLoadConfig(const char *override_path, int report_status) {
    char path[PATH_MAX];
    if (!editorResolveConfigPath(path, sizeof(path), override_path)) {
        if (report_status) {
            editorSetStatusMessage("No config file found (~/.tedrc)");
        }
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (report_status || override_path != NULL) {
            editorSetStatusMessage("Can't open config '%s': %s", path, strerror(errno));
        }
        return 0;
    }

    char line[512];
    int lineno = 0;
    int warnings = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;

        char *comment = strpbrk(line, "#;");
        if (comment) *comment = '\0';

        char *trimmed = trimWhitespace(line);
        if (*trimmed == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            warnings++;
            editorSetStatusMessage("Config warning (%s:%d): missing '='", path, lineno);
            continue;
        }

        *eq = '\0';
        char *key = trimWhitespace(trimmed);
        char *val = trimWhitespace(eq + 1);

        int ok = 1;
        if (strcmp(key, "tab_stop") == 0) {
            ok = parseIntRange(val, 1, 16, &E.tab_stop);
        } else if (strcmp(key, "auto_indent") == 0) {
            ok = parseBool(val, &E.auto_indent);
        } else if (strcmp(key, "line_numbers") == 0) {
            ok = parseBool(val, &E.line_numbers);
        } else if (strcmp(key, "quit_times") == 0) {
            ok = parseIntRange(val, 0, 10, &E.quit_times);
        } else if (strcmp(key, "color_comment") == 0) {
            ok = parseAnsiColor(val, &E.color_comment);
        } else if (strcmp(key, "color_keyword") == 0) {
            ok = parseAnsiColor(val, &E.color_keyword);
        } else if (strcmp(key, "color_type") == 0) {
            ok = parseAnsiColor(val, &E.color_type);
        } else if (strcmp(key, "color_string") == 0) {
            ok = parseAnsiColor(val, &E.color_string);
        } else if (strcmp(key, "color_number") == 0) {
            ok = parseAnsiColor(val, &E.color_number);
        } else if (strcmp(key, "color_match") == 0) {
            ok = parseAnsiColor(val, &E.color_match);
        } else if (strcmp(key, "color_gutter") == 0) {
            ok = parseAnsiColor(val, &E.color_gutter);
        } else {
            warnings++;
            editorSetStatusMessage("Config warning (%s:%d): unknown key '%s'",
                                   path, lineno, key);
            continue;
        }

        if (!ok) {
            warnings++;
            editorSetStatusMessage("Config warning (%s:%d): invalid value '%s' for %s",
                                   path, lineno, val, key);
        }
    }

    fclose(fp);

    if (report_status && warnings == 0) {
        editorSetStatusMessage("Reloaded config: %s", path);
    }
    return 1;
}

void editorReloadConfig(void) {
    if (editorLoadConfig(E.config_path, 1)) {
        for (int i = 0; i < E.numrows; i++) {
            editorUpdateRow(&E.row[i]);
        }
    }
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == RESIZE_EVENT) {
            E.resized = 0;
            editorUpdateWindowSize();
            continue;
        } else if (c == MOUSE_EVENT) {
            continue;
        } else if (c == DEL_KEY || c == CTRL_BACKSPACE || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

static void editorMapScreenToBuffer(int term_x, int term_y, int *out_cy, int *out_cx) {
    int click_row = term_y - 1;
    if (click_row < 0) click_row = 0;
    if (click_row >= E.screenrows) click_row = E.screenrows - 1;

    int target_cy = click_row + E.rowoff;
    if (target_cy >= E.numrows) {
        target_cy = E.numrows - 1;
    }
    if (target_cy < 0) target_cy = 0;

    int gutter = editorGutterWidth();
    int click_col = (term_x - 1) - gutter;
    if (click_col < 0) click_col = 0;

    int target_rx = click_col + E.coloff;
    erow *row = &E.row[target_cy];
    int target_cx = (target_rx >= row->rsize) ? row->size : editorRowRxToCx(row, target_rx);

    *out_cy = target_cy;
    *out_cx = target_cx;
}

void editorHandleMouse(void) {
    if (E.mouse_release) return; // Ignore button-release packets

    // Strip modifier bits (Shift=4, Meta=8, Ctrl=16)
    int btn = E.mouse_btn & ~0x1c;

    if (btn == 64) {
        // Scroll wheel up
        if (E.rowoff > 0) {
            E.rowoff -= TED_MOUSE_SCROLL_LINES;
            if (E.rowoff < 0) E.rowoff = 0;
            if (E.cy >= E.rowoff + E.screenrows) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy < 0) E.cy = 0;
                int rowlen = (E.cy < E.numrows) ? E.row[E.cy].size : 0;
                if (E.cx > rowlen) E.cx = rowlen;
            }
        }
        return;
    }

    if (btn == 65) {
        // Scroll wheel down
        if (E.numrows > E.screenrows && E.rowoff < E.numrows - E.screenrows) {
            E.rowoff += TED_MOUSE_SCROLL_LINES;
            if (E.rowoff > E.numrows - E.screenrows) {
                E.rowoff = E.numrows - E.screenrows;
            }
            if (E.cy < E.rowoff) {
                E.cy = E.rowoff;
                int rowlen = (E.cy < E.numrows) ? E.row[E.cy].size : 0;
                if (E.cx > rowlen) E.cx = rowlen;
            }
        }
        return;
    }

    if (btn == 0) {
        // Left-click inside text viewport sets cursor and selection anchor
        int click_row = E.mouse_y - 1;
        if (click_row < 0 || click_row >= E.screenrows) return;
        if (E.numrows == 0) return;

        int target_cy, target_cx;
        editorMapScreenToBuffer(E.mouse_x, E.mouse_y, &target_cy, &target_cx);
        E.cy = target_cy;
        E.cx = target_cx;
        editorClearSelection();
        E.sel_start_cy = target_cy;
        E.sel_start_cx = target_cx;
        return;
    }

    if (btn == 32) {
        // Left-button drag extends visual selection
        if (E.numrows == 0) return;
        int target_cy, target_cx;
        editorMapScreenToBuffer(E.mouse_x, E.mouse_y, &target_cy, &target_cx);
        E.cy = target_cy;
        E.cx = target_cx;
        E.sel_active = 1;
    }
}

void editorProcessKeypress(void) {
    static int quit_times = -1;
    if (quit_times == -1) quit_times = E.quit_times;

    int c = editorReadKey();

    switch (c) {
        case RESIZE_EVENT:
            E.resized = 0;
            editorUpdateWindowSize();
            return;

        case MOUSE_EVENT:
            editorHandleMouse();
            return;

        case '\r':
            editorInsertNewline();
            break;

        case '\t':
            editorIndent();
            break;

        case SHIFT_TAB:
            editorOutdent();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                       "Press Ctrl-Q %d more time(s) to quit.",
                                       quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case CTRL_KEY('g'):
            editorGotoLine();
            break;

        case CTRL_KEY('r'):
            editorReloadConfig();
            break;

        case CTRL_KEY('c'):
            editorCopySelection();
            break;

        case CTRL_KEY('x'):
            editorCutSelection();
            break;

        case CTRL_KEY('v'):
            editorPasteClipboard();
            break;

        case CTRL_KEY('d'):
            editorDuplicateLine();
            break;

        case CTRL_KEY('k'):
            editorCutLine();
            break;

        case CTRL_KEY('u'):
            editorPasteLine();
            break;

        case CTRL_KEY('z'):
            editorUndo();
            break;

        case CTRL_KEY('y'):
            editorRedo();
            break;

        case HOME_KEY:
            editorClearSelection();
            E.cx = 0;
            break;

        case END_KEY:
            editorClearSelection();
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case BACKSPACE:
        case DEL_KEY:
            if (c == DEL_KEY && !E.sel_active) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_BACKSPACE:
            editorDelWord();
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
            editorClearSelection();
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }

            int times = E.screenrows;
            while (times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorClearSelection();
            editorMoveCursor(c);
            break;

        case SHIFT_ARROW_UP:
        case SHIFT_ARROW_DOWN:
        case SHIFT_ARROW_LEFT:
        case SHIFT_ARROW_RIGHT: {
            if (!E.sel_active) {
                E.sel_active = 1;
                E.sel_start_cx = E.cx;
                E.sel_start_cy = E.cy;
            }
            int base_key = ARROW_LEFT + (c - SHIFT_ARROW_LEFT);
            editorMoveCursor(base_key);
            break;
        }

        case CTRL_ARROW_LEFT:
        case CTRL_ARROW_RIGHT:
            editorMoveWord(c);
            break;

        case CTRL_KEY('l'):
            break;

        case '\x1b':
            editorClearSelection();
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = E.quit_times;
}

/*** cli & init ***/

void editorPrintUsage(const char *progname) {
    printf("Usage: %s [OPTION]... [FILE]\n"
           "Minimal zero-dependency terminal text editor.\n\n"
           "Options:\n"
           "  -c, --config FILE  Load configuration from FILE (default: ~/.tedrc)\n"
           "  -h, --help         Display this help message and exit\n"
           "  -v, --version      Output version information and exit\n\n"
           "Keybindings:\n"
           "  Ctrl-S             Save buffer to disk\n"
           "  Ctrl-F             Incremental search (Arrows to cycle, Esc to cancel)\n"
           "  Ctrl-G             Jump to a specific line number\n"
           "  Ctrl-R             Reload ~/.tedrc configuration live\n"
           "  Shift+Arrows       Select text range (or drag with mouse)\n"
           "  Ctrl-C / X / V     Copy / Cut / Paste selected range\n"
           "  Ctrl-D             Duplicate current line\n"
           "  Ctrl-K / Ctrl-U    Cut current line / Paste line below cursor\n"
           "  Ctrl-Z / Ctrl-Y    Undo / Redo\n"
           "  Tab / Shift-Tab    Indent / Outdent line or selected block\n"
           "  Ctrl-Left/Right    Jump backward / forward by word\n"
           "  Ctrl-Backspace     Delete previous word\n"
           "  Ctrl-Q             Quit (press %d more time(s) to discard unsaved changes)\n",
           progname, 2);
}

void editorPrintVersion(void) {
    printf("ted %s\n"
           "Written by k!M (pizslacker)\n",
           TED_VERSION);
}

void initEditor(const char *config_path) {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.saved_undo_top = 0;
    E.undo_stack = NULL;
    E.undo_top = 0;
    E.undo_len = 0;
    E.undo_cap = 0;
    E.in_undo_redo = 0;
    E.clipboard = NULL;
    E.clipboard_is_line = 0;
    E.filename = NULL;
    E.config_path = config_path ? strdup(config_path) : NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    E.resized = 0;
    E.sel_active = 0;
    E.sel_start_cx = 0;
    E.sel_start_cy = 0;
    E.mouse_btn = 0;
    E.mouse_x = 0;
    E.mouse_y = 0;
    E.mouse_release = 0;

    // Default runtime settings (overridable via ~/.tedrc)
    E.tab_stop = 4;
    E.auto_indent = 1;
    E.line_numbers = 1;
    E.quit_times = 2;
    E.color_comment = 36;
    E.color_keyword = 33;
    E.color_type = 32;
    E.color_string = 35;
    E.color_number = 31;
    E.color_match = 34;
    E.color_gutter = 90;

    editorLoadConfig(E.config_path, 0);
    editorUpdateWindowSize();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = editorHandleSigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        die("sigaction");
    }
}

int main(int argc, char *argv[]) {
    const char *filename = NULL;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            editorPrintUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            editorPrintVersion();
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option '%s' requires an argument\n",
                        argv[0], argv[i]);
                return 1;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                filename = argv[i + 1];
            }
            break;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unrecognized option '%s'\n"
                            "Try '%s --help' for more information.\n",
                    argv[0], argv[i], argv[0]);
            return 1;
        } else {
            filename = argv[i];
        }
    }

    if (filename != NULL) {
        struct stat st;
        if (stat(filename, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                fprintf(stderr, "%s: '%s': Is a directory\n", argv[0], filename);
                return 1;
            }
        } else if (errno != ENOENT) {
            fprintf(stderr, "%s: cannot open '%s': %s\n",
                    argv[0], filename, strerror(errno));
            return 1;
        }
    }

    enableRawMode();
    initEditor(config_path);
    if (filename != NULL) {
        editorOpen((char *)filename);
    }

    if (E.statusmsg[0] == '\0') {
        editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = find | Ctrl-G = goto | Ctrl-Z/Y = undo/redo | Ctrl-Q = quit");
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}