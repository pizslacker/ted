#include "ted.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new_buf = realloc(ab->b, ab->len + len);
    if (new_buf == NULL) return;

    memcpy(&new_buf[ab->len], s, len);
    ab->b = new_buf;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
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

static void editorScroll(void) {
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

static void editorDrawRows(struct abuf *ab) {
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

static void editorDrawStatusBar(struct abuf *ab) {
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

static void editorDrawMessageBar(struct abuf *ab) {
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

    termWrite(ab.b, (size_t)ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}
