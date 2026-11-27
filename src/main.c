#include "ted.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct editorConfig E;

static void editorPrintUsage(const char *progname) {
    printf("Usage: %s [OPTION]... [FILE]\n"
           "Minimal zero-dependency terminal text editor.\n\n"
           "Options:\n"
           "  -c, --config FILE  Load configuration from FILE (default: ~/.tedrc)\n"
           "  -h, --help         Display this help message and exit\n"
           "  -v, --version      Output version information and exit\n\n"
           "Keybindings:\n"
           "  Ctrl-S             Save buffer atomically to disk\n"
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

static void editorPrintVersion(void) {
    printf("ted %s\n"
           "Written by k!M (pizslacker)\n",
           TED_VERSION);
}

static void initEditor(const char *config_path) {
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
    E.file_mtime = 0;
    E.has_file_mtime = 0;
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
    E.trim_trailing_whitespace = 0;
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