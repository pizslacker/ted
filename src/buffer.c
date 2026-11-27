#include "ted.h"

#include <stdlib.h>
#include <string.h>

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

static void editorFreeEditOp(editOp *op) {
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