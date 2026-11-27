#include "ted.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*** editor operations ***/

void editorInsertChar(int c) {
    editorDeleteActiveSelection();

    int before_cx = E.cx;
    int before_cy = E.cy;
    int created_row = 0;

    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
        created_row = 1;
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;

    editOp op = {
        .type = OP_INSERT_CHAR,
        .cy = before_cy,
        .cx = before_cx,
        .ch = c,
        .created_row = created_row,
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
    int created_row = 0;

    if (E.cy == E.numrows) {
        editorInsertRow(E.cy, "", 0);
        created_row = 1;
        E.cy++;
        E.cx = 0;

        editOp op = {
            .type = OP_SPLIT_LINE,
            .cy = before_cy,
            .cx = before_cx,
            .ch = 0,
            .created_row = created_row,
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
        .created_row = 0,
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
            .created_row = 0,
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
            .created_row = 0,
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
            .created_row = 0,
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
            .created_row = 0,
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
            .created_row = 0,
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
                .created_row = 0,
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
        .created_row = 0,
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
        .created_row = 0,
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
        .created_row = 0,
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
        .created_row = 0,
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
        .created_row = 0,
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

/*** find & navigation ***/

static void editorFindCallback(char *query, int key) {
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

static void editorMoveCursor(int key) {
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

static void editorHandleMouse(void) {
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
            termWrite("\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[2J\x1b[H", 31);
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