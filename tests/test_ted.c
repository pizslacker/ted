#include "ted.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*** global state & terminal/render stubs ***/

struct editorConfig E;

static int tests_run = 0;
static int tests_failed = 0;

void termWrite(const void *buf, size_t len) {
    (void)buf;
    (void)len;
}

void die(const char *s) {
    fprintf(stderr, "FATAL die() called during test: %s (%s)\n", s, strerror(errno));
    exit(2);
}

void disableRawMode(void) {}
void enableRawMode(void) {}

int editorReadKey(void) {
    return '\x1b';
}

void editorHandleSigwinch(int sig) {
    (void)sig;
}

void editorUpdateWindowSize(void) {
    E.screenrows = 24;
    E.screencols = 80;
}

int editorGutterWidth(void) {
    return E.line_numbers ? 4 : 0;
}

void editorRefreshScreen(void) {}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** test harness helpers ***/

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_INT(expected, actual) do { \
    int _exp = (expected); \
    int _act = (actual); \
    if (_exp != _act) { \
        fprintf(stderr, "  FAIL [%s:%d]: expected %d, got %d (%s)\n", \
                __FILE__, __LINE__, _exp, _act, #actual); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_STR(expected, actual) do { \
    const char *_exp = (expected); \
    const char *_act = (actual); \
    if ((_exp == NULL && _act != NULL) || (_exp != NULL && _act == NULL) || \
        (_exp != NULL && strcmp(_exp, _act) != 0)) { \
        fprintf(stderr, "  FAIL [%s:%d]: expected \"%s\", got \"%s\"\n", \
                __FILE__, __LINE__, _exp ? _exp : "(null)", _act ? _act : "(null)"); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    int _before = tests_failed; \
    tests_run++; \
    resetEditorState(); \
    fn(); \
    if (tests_failed == _before) { \
        printf("[PASS] %s\n", #fn); \
    } else { \
        printf("[FAIL] %s\n", #fn); \
    } \
    resetEditorState(); \
} while (0)

static void resetEditorState(void) {
    for (int i = 0; i < E.numrows; i++) {
        editorFreeRow(&E.row[i]);
    }
    free(E.row);
    editorClearUndoHistory();
    free(E.clipboard);
    free(E.filename);
    free(E.config_path);

    memset(&E, 0, sizeof(E));
    E.screenrows = 24;
    E.screencols = 80;
    E.tab_stop = 4;
    E.auto_indent = 1;
    E.line_numbers = 1;
    E.trim_trailing_whitespace = 0;
    E.quit_times = 2;
    E.color_comment = 36;
    E.color_keyword = 33;
    E.color_type = 32;
    E.color_string = 35;
    E.color_number = 31;
    E.color_match = 34;
    E.color_gutter = 90;
}

static void insertStringAtCursor(const char *s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\n') {
            editorInsertNewline();
        } else {
            editorInsertChar((unsigned char)s[i]);
        }
    }
}

/*** test cases ***/

static void test_row_tabs_and_cx_rx_conversion(void) {
    editorInsertRow(0, "\tfoo\tbar", 8);
    ASSERT_EQ_INT(1, E.numrows);
    ASSERT_EQ_INT(8, E.row[0].size);
    // '\t' at col 0 -> 4 spaces; "foo" -> 3 chars (cols 4..6); '\t' at col 7 -> 1 space (col 7); "bar" -> 3 chars
    ASSERT_EQ_STR("    foo bar", E.row[0].render);
    ASSERT_EQ_INT(11, E.row[0].rsize);

    ASSERT_EQ_INT(0, editorRowCxToRx(&E.row[0], 0));
    ASSERT_EQ_INT(4, editorRowCxToRx(&E.row[0], 1));
    ASSERT_EQ_INT(8, editorRowCxToRx(&E.row[0], 5));

    ASSERT_EQ_INT(0, editorRowRxToCx(&E.row[0], 2));
    ASSERT_EQ_INT(1, editorRowRxToCx(&E.row[0], 4));
    ASSERT_EQ_INT(5, editorRowRxToCx(&E.row[0], 8));
}

static void test_smart_indent_and_undo_redo_dirty_tracking(void) {
    insertStringAtCursor("if (ok) {\nreturn 1;");
    ASSERT_EQ_INT(2, E.numrows);
    ASSERT_EQ_STR("if (ok) {", E.row[0].chars);
    ASSERT_EQ_STR("    return 1;", E.row[1].chars);
    ASSERT_TRUE(E.dirty != 0);

    // Undo back to empty buffer
    while (E.undo_top > 0) {
        editorUndo();
    }
    ASSERT_EQ_INT(0, E.numrows);
    ASSERT_EQ_INT(0, E.dirty);

    // Redo forward to full buffer
    while (E.undo_top < E.undo_len) {
        editorRedo();
    }
    ASSERT_EQ_INT(2, E.numrows);
    ASSERT_EQ_STR("if (ok) {", E.row[0].chars);
    ASSERT_EQ_STR("    return 1;", E.row[1].chars);
    ASSERT_TRUE(E.dirty != 0);
}

static void test_selection_range_cut_paste_and_block_indent(void) {
    insertStringAtCursor("alpha\nbeta\ngamma");
    ASSERT_EQ_INT(3, E.numrows);

    // Select from row 0 col 2 ("pha\nbe") to row 1 col 2
    E.sel_active = 1;
    E.sel_start_cy = 0;
    E.sel_start_cx = 2;
    E.cy = 1;
    E.cx = 2;

    editorCutSelection();
    ASSERT_EQ_INT(2, E.numrows);
    ASSERT_EQ_STR("alta", E.row[0].chars);
    ASSERT_EQ_STR("gamma", E.row[1].chars);
    ASSERT_EQ_STR("pha\nbe", E.clipboard);

    // Undo cut restores all 3 lines
    editorUndo();
    ASSERT_EQ_INT(3, E.numrows);
    ASSERT_EQ_STR("alpha", E.row[0].chars);
    ASSERT_EQ_STR("beta", E.row[1].chars);
    ASSERT_EQ_STR("gamma", E.row[2].chars);

    // Redo cut, then paste at end of row 1
    editorRedo();
    ASSERT_EQ_INT(2, E.numrows);
    E.cy = 1;
    E.cx = E.row[1].size;
    editorPasteClipboard();
    ASSERT_EQ_INT(3, E.numrows);
    ASSERT_EQ_STR("gammapha", E.row[1].chars);
    ASSERT_EQ_STR("be", E.row[2].chars);

    // Block indent rows 0..2
    E.sel_active = 1;
    E.sel_start_cy = 0;
    E.sel_start_cx = 0;
    E.cy = 2;
    E.cx = 2;
    editorIndent();
    ASSERT_EQ_STR("    alta", E.row[0].chars);
    ASSERT_EQ_STR("    gammapha", E.row[1].chars);
    ASSERT_EQ_STR("    be", E.row[2].chars);

    // Block outdent rows 0..2
    editorOutdent();
    ASSERT_EQ_STR("alta", E.row[0].chars);
    ASSERT_EQ_STR("gammapha", E.row[1].chars);
    ASSERT_EQ_STR("be", E.row[2].chars);
}

static void test_syntax_extensions_and_shebang_detection(void) {
    // 1. Extension & basename detection
    E.filename = strdup("/tmp/project/Makefile");
    editorSelectSyntaxHighlight();
    ASSERT_TRUE(E.syntax != NULL);
    ASSERT_EQ_STR("makefile", E.syntax->filetype);

    free(E.filename);
    E.filename = strdup("main.rs");
    editorSelectSyntaxHighlight();
    ASSERT_TRUE(E.syntax != NULL);
    ASSERT_EQ_STR("rust", E.syntax->filetype);

    // 2. Shebang fallback for extensionless script
    free(E.filename);
    E.filename = strdup("/usr/local/bin/deploy");
    editorInsertRow(0, "#!/usr/bin/env python3", 22);
    editorInsertRow(1, "def run(x: int) -> None:", 24);
    editorSelectSyntaxHighlight();
    ASSERT_TRUE(E.syntax != NULL);
    ASSERT_EQ_STR("python", E.syntax->filetype);
    // "def" on row 1 should be highlighted as HL_KEYWORD1
    ASSERT_EQ_INT(HL_KEYWORD1, E.row[1].hl[0]);
    ASSERT_EQ_INT(HL_KEYWORD1, E.row[1].hl[2]);
}

static void test_matching_brackets_ignores_strings_and_comments(void) {
    editorInsertRow(0, "if (a == '(') { /* } */", 23);
    editorInsertRow(1, "    return 0;", 13);
    editorInsertRow(2, "}", 1);
    E.filename = strdup("test.c");
    editorSelectSyntaxHighlight();

    // Cursor on '(' at row 0, col 3 -> should match ')' at row 0, col 12 (skipping '(' inside "'('")
    E.cy = 0;
    E.cx = 3;
    int mr = -1, mrx = -1;
    ASSERT_EQ_INT(1, editorFindMatchingBracket(&mr, &mrx));
    ASSERT_EQ_INT(0, mr);
    ASSERT_EQ_INT(12, mrx);

    // Cursor on '{' at row 0, col 14 -> should match '}' at row 2, col 0 (skipping '}' inside /* } */)
    E.cy = 0;
    E.cx = 14;
    ASSERT_EQ_INT(1, editorFindMatchingBracket(&mr, &mrx));
    ASSERT_EQ_INT(2, mr);
    ASSERT_EQ_INT(0, mrx);
}

static void test_config_parser_and_atomic_save_trim(void) {
    char cfg_path[] = "/tmp/ted_test_cfg.XXXXXX";
    int cfd = mkstemp(cfg_path);
    ASSERT_TRUE(cfd != -1);

    const char *cfg_body =
        "# Sample config\n"
        "tab_stop = 2\n"
        "auto_indent = false\n"
        "line_numbers = no\n"
        "trim_trailing_whitespace = true\n"
        "quit_times = 1\n"
        "color_comment = green\n"
        "color_keyword = 93\n"
        "tab_stop = 99\n"; // Out-of-range value must be rejected, keeping 2
    ssize_t w = write(cfd, cfg_body, strlen(cfg_body));
    close(cfd);
    ASSERT_EQ_INT((int)strlen(cfg_body), (int)w);

    ASSERT_EQ_INT(1, editorLoadConfig(cfg_path, 0));
    unlink(cfg_path);

    ASSERT_EQ_INT(2, E.tab_stop);
    ASSERT_EQ_INT(0, E.auto_indent);
    ASSERT_EQ_INT(0, E.line_numbers);
    ASSERT_EQ_INT(1, E.trim_trailing_whitespace);
    ASSERT_EQ_INT(1, E.quit_times);
    ASSERT_EQ_INT(32, E.color_comment);
    ASSERT_EQ_INT(93, E.color_keyword);

    // Test atomic save + trailing whitespace trimming
    char out_path[] = "/tmp/ted_test_out.XXXXXX";
    int ofd = mkstemp(out_path);
    ASSERT_TRUE(ofd != -1);
    close(ofd);
    unlink(out_path);

    editorOpen(out_path);
    insertStringAtCursor("int x = 1;   \t\nint y = 2;  ");
    editorSave();
    ASSERT_EQ_INT(0, E.dirty);
    ASSERT_EQ_STR("int x = 1;", E.row[0].chars);
    ASSERT_EQ_STR("int y = 2;", E.row[1].chars);

    // Re-open saved file from disk and verify contents
    resetEditorState();
    editorOpen(out_path);
    unlink(out_path);
    ASSERT_EQ_INT(2, E.numrows);
    ASSERT_EQ_STR("int x = 1;", E.row[0].chars);
    ASSERT_EQ_STR("int y = 2;", E.row[1].chars);
}

int main(void) {
    printf("Running ted %s unit tests...\n", TED_VERSION);

    RUN_TEST(test_row_tabs_and_cx_rx_conversion);
    RUN_TEST(test_smart_indent_and_undo_redo_dirty_tracking);
    RUN_TEST(test_selection_range_cut_paste_and_block_indent);
    RUN_TEST(test_syntax_extensions_and_shebang_detection);
    RUN_TEST(test_matching_brackets_ignores_strings_and_comments);
    RUN_TEST(test_config_parser_and_atomic_save_trim);

    printf("\nSummary: %d/%d tests passed.\n", tests_run - tests_failed, tests_run);
    return tests_failed == 0 ? 0 : 1;
}