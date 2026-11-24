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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define TED_VERSION "0.2.0"
#define TED_TAB_STOP 4
#define TED_QUIT_TIMES 2
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    RESIZE_EVENT
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
    OP_JOIN_LINE
};

typedef struct editOp {
    enum editOpType type;
    int cy;             // Target row index
    int cx;             // Target column index
    int ch;             // Character for INSERT_CHAR / DELETE_CHAR
    char *indent;       // Auto-indent prefix inserted on SPLIT_LINE
    int indent_len;     // Length of indent prefix
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
    int hl_open_comment;    // Non-zero if line ends inside an unclosed /* comment
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
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
    volatile sig_atomic_t resized;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };

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

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
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
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
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
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
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

        // Multi-line comments
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
        case HL_MLCOMMENT: return 36; // Cyan
        case HL_KEYWORD1:  return 33; // Yellow
        case HL_KEYWORD2:  return 32; // Green
        case HL_STRING:    return 35; // Magenta
        case HL_NUMBER:    return 31; // Red
        case HL_MATCH:     return 34; // Blue
        default:           return 37; // White
    }
}

void editorSelectSyntaxHighlight(void) {
    E.syntax = NULL;
    if (E.filename == NULL) return;

    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                for (int filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TED_TAB_STOP - 1) - (rx % TED_TAB_STOP);
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
            cur_rx += (TED_TAB_STOP - 1) - (cur_rx % TED_TAB_STOP);
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
    row->render = malloc(row->size + tabs * (TED_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TED_TAB_STOP != 0) row->render[idx++] = ' ';
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

/*** undo / redo ***/

void editorFreeEditOp(editOp *op) {
    free(op->indent);
    op->indent = NULL;
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
    }

    E.cx = op->after_cx;
    E.cy = op->after_cy;
    E.dirty = (E.undo_top == E.saved_undo_top) ? 0 : 1;
    E.in_undo_redo = 0;

    editorSetStatusMessage("Redo (%d available)", E.undo_len - E.undo_top);
}

/*** editor operations ***/

void editorInsertChar(int c) {
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
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
}

void editorInsertNewline(void) {
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
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
        return;
    }

    erow *row = &E.row[E.cy];

    // Measure leading spaces/tabs up to the cursor position
    int indent = 0;
    while (indent < E.cx &&
           (row->chars[indent] == ' ' || row->chars[indent] == '\t')) {
        indent++;
    }

    // Check if the last non-whitespace character before cursor is '{'
    int extra_indent = 0;
    int check = E.cx - 1;
    while (check >= 0 && (row->chars[check] == ' ' || row->chars[check] == '\t')) {
        check--;
    }
    if (check >= 0 && row->chars[check] == '{') {
        extra_indent = TED_TAB_STOP;
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
        .before_cx = before_cx,
        .before_cy = before_cy,
        .after_cx = E.cx,
        .after_cy = E.cy
    };
    editorPushUndo(op);
}

void editorDelChar(void) {
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
            .before_cx = before_cx,
            .before_cy = before_cy,
            .after_cx = E.cx,
            .after_cy = E.cy
        };
        editorPushUndo(op);
    }
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

    editorClearUndoHistory();
    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

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

/*** find ***/

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
    int numwidth = gutter - 1;
    int textcols = E.screencols - gutter;
    if (textcols < 0) textcols = 0;

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

    const char *help_line = "Ctrl-S: Save | Ctrl-F: Find | Ctrl-Z/Y: Undo/Redo | Ctrl-Q: Quit";
    const int help_len = (int)strlen(help_line);

    const int total_banner_rows = logo_rows + 4;
    int banner_start_y = (E.screenrows - total_banner_rows) / 2;
    if (banner_start_y < 1) banner_start_y = 1;

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            char gbuf[32];
            int glen = snprintf(gbuf, sizeof(gbuf), "\x1b[90m%*s \x1b[39m",
                                numwidth, "~");
            abAppend(ab, gbuf, glen);

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
            char gbuf[32];
            int glen = snprintf(gbuf, sizeof(gbuf), "\x1b[90m%*d \x1b[39m",
                                numwidth, filerow + 1);
            abAppend(ab, gbuf, glen);

            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > textcols) len = textcols;

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;

            for (int j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
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
        } else if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
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

void editorProcessKeypress(void) {
    static int quit_times = TED_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case RESIZE_EVENT:
            E.resized = 0;
            editorUpdateWindowSize();
            return;

        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                       "Press Ctrl-Q %d more time(s) to quit.",
                                       quit_times);
                quit_times--;
                return;
            }
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

        case CTRL_KEY('z'):
            editorUndo();
            break;

        case CTRL_KEY('y'):
            editorRedo();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
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
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = TED_QUIT_TIMES;
}

/*** init ***/

void initEditor(void) {
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
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    E.resized = 0;

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
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-F = find | Ctrl-Z/Y = undo/redo | Ctrl-Q = quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
