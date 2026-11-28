#ifndef TED_H
#define TED_H

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <signal.h>
#include <stddef.h>
#include <termios.h>
#include <time.h>

/*** defines ***/

#define TED_VERSION "0.9.9"
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
    int created_row;    // 1 if op auto-created row E.cy on an empty trailing line
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

    // File safety & save hygiene
    time_t file_mtime;              // Last known on-disk mtime of E.filename
    int has_file_mtime;             // 1 if E.filename existed on disk when opened/saved
    int trim_trailing_whitespace;   // Default: 0 (set via ~/.tedrc)

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

extern struct editorConfig E;

/*** terminal.c ***/
void termWrite(const void *buf, size_t len);
void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
int editorReadKey(void);
void editorHandleSigwinch(int sig);
void editorUpdateWindowSize(void);

/*** syntax.c ***/
int is_separator(int c);
void editorUpdateSyntax(erow *row);
int editorSyntaxToColor(int hl);
void editorSelectSyntaxHighlight(void);
int editorFindMatchingBracket(int *out_row, int *out_rx);

/*** buffer.c ***/
int editorRowCxToRx(erow *row, int cx);
int editorRowRxToCx(erow *row, int rx);
void editorUpdateRow(erow *row);
void editorInsertRow(int at, char *s, size_t len);
void editorFreeRow(erow *row);
void editorDelRow(int at);
void editorRowInsertChar(erow *row, int at, int c);
void editorRowAppendString(erow *row, char *s, size_t len);
void editorRowDelChar(erow *row, int at);

void editorClearSelection(void);
int editorGetSelectionBounds(int *sy, int *sx, int *ey, int *ex);
char *editorExtractRange(int sy, int sx, int ey, int ex, int *out_len);
void editorDeleteRangeRaw(int sy, int sx, int ey, int ex);
void editorInsertRangeRaw(int sy, int sx, const char *text, int len,
                          int *out_ey, int *out_ex);
int editorDeleteActiveSelection(void);

void editorClearUndoHistory(void);
void editorPushUndo(editOp op);
void editorUndo(void);
void editorRedo(void);

/*** edit.c ***/
void editorInsertChar(int c);
void editorInsertNewline(void);
void editorDelChar(void);
void editorMoveWord(int key);
void editorDelWord(void);
void editorIndent(void);
void editorOutdent(void);
void editorDuplicateLine(void);
void editorCutLine(void);
void editorPasteLine(void);
void editorCopySelection(void);
void editorCutSelection(void);
void editorPasteClipboard(void);
void editorFind(void);
void editorGotoLine(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorProcessKeypress(void);

/*** fileio.c ***/
void editorOpen(char *filename);
void editorSave(void);
int editorLoadConfig(const char *override_path, int report_status);
void editorReloadConfig(void);

/*** render.c ***/
int editorGutterWidth(void);
void editorRefreshScreen(void);
void editorSetStatusMessage(const char *fmt, ...);

#endif /* TED_H */
