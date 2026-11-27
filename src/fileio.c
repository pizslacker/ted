#include "ted.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*** file i/o ***/

static char *editorRowsToString(int *buflen) {
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
    E.has_file_mtime = 0;

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

    struct stat st;
    if (stat(filename, &st) == 0) {
        E.file_mtime = st.st_mtime;
        E.has_file_mtime = 1;
    }

    // Re-evaluate syntax in case an extensionless file starts with a #! shebang
    if (E.syntax == NULL) {
        editorSelectSyntaxHighlight();
    }

    editorClearUndoHistory();
    E.dirty = 0;
}

static void editorTrimTrailingWhitespace(void) {
    int trimmed_lines = 0;
    for (int r = 0; r < E.numrows; r++) {
        erow *row = &E.row[r];
        int orig_size = row->size;
        while (row->size > 0 &&
               (row->chars[row->size - 1] == ' ' ||
                row->chars[row->size - 1] == '\t')) {
            row->size--;
        }
        if (row->size != orig_size) {
            row->chars[row->size] = '\0';
            editorUpdateRow(row);
            trimmed_lines++;
            if (E.cy == r && E.cx > row->size) {
                E.cx = row->size;
            }
        }
    }
    if (trimmed_lines > 0) {
        editorClearSelection();
    }
}

static int editorWriteAll(int fd, const char *buf, int len) {
    int written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (int)n;
    }
    return 0;
}

void editorSave(void) {
    int is_new_name = 0;
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        E.has_file_mtime = 0;
        is_new_name = 1;
        editorSelectSyntaxHighlight();
    } else if (E.syntax == NULL) {
        editorSelectSyntaxHighlight();
    }

    // Guard against directories and external on-disk modifications
    struct stat lst;
    int target_exists = (lstat(E.filename, &lst) == 0);
    int is_symlink = (target_exists && S_ISLNK(lst.st_mode));

    struct stat st;
    int resolved_exists = (stat(E.filename, &st) == 0);
    if (resolved_exists) {
        if (S_ISDIR(st.st_mode)) {
            editorSetStatusMessage("Can't save! '%s' is a directory", E.filename);
            if (is_new_name) {
                free(E.filename);
                E.filename = NULL;
            }
            return;
        }
        if (E.has_file_mtime && st.st_mtime != E.file_mtime) {
            char *ans = editorPrompt("File changed on disk! Overwrite? (y/N): %s", NULL);
            int confirmed = (ans != NULL && (ans[0] == 'y' || ans[0] == 'Y') && ans[1] == '\0');
            free(ans);
            if (!confirmed) {
                editorSetStatusMessage("Save aborted (external changes preserved)");
                return;
            }
        }
    }

    if (E.trim_trailing_whitespace) {
        editorTrimTrailingWhitespace();
    }

    int len;
    char *buf = editorRowsToString(&len);
    mode_t file_mode = resolved_exists ? (st.st_mode & 0777) : 0644;

    // Atomic save via sibling temp file + fsync + rename (unless target is a symlink)
    if (!is_symlink) {
        char tmppath[PATH_MAX];
        const char *slash = strrchr(E.filename, '/');
        int n;
        if (slash != NULL) {
            int dirlen = (int)(slash - E.filename);
            n = snprintf(tmppath, sizeof(tmppath), "%.*s/.ted.tmp.XXXXXX", dirlen, E.filename);
        } else {
            n = snprintf(tmppath, sizeof(tmppath), ".ted.tmp.XXXXXX");
        }

        if (n > 0 && n < (int)sizeof(tmppath)) {
            int tmpfd = mkstemp(tmppath);
            if (tmpfd != -1) {
                fchmod(tmpfd, file_mode);
                if (editorWriteAll(tmpfd, buf, len) == 0 &&
                    fsync(tmpfd) == 0 &&
                    close(tmpfd) == 0 &&
                    rename(tmppath, E.filename) == 0) {
                    free(buf);
                    E.dirty = 0;
                    E.saved_undo_top = E.undo_top;
                    if (stat(E.filename, &st) == 0) {
                        E.file_mtime = st.st_mtime;
                        E.has_file_mtime = 1;
                    }
                    editorSetStatusMessage("%d bytes written to disk", len);
                    return;
                }
                close(tmpfd);
                unlink(tmppath);
            }
        }
    }

    // Fallback: direct write (used for symlinks or directories denying temp creation)
    int fd = open(E.filename, O_WRONLY | O_CREAT | O_TRUNC, file_mode);
    if (fd != -1) {
        if (editorWriteAll(fd, buf, len) == 0 && fsync(fd) == 0) {
            close(fd);
            free(buf);
            E.dirty = 0;
            E.saved_undo_top = E.undo_top;
            if (stat(E.filename, &st) == 0) {
                E.file_mtime = st.st_mtime;
                E.has_file_mtime = 1;
            }
            editorSetStatusMessage("%d bytes written to disk", len);
            return;
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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
        } else if (strcmp(key, "trim_trailing_whitespace") == 0) {
            ok = parseBool(val, &E.trim_trailing_whitespace);
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