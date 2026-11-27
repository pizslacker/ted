#include "ted.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*** filetypes ***/

/* C / C++ */
static char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };
static char *C_HL_keywords[] = {
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
static char *PY_HL_extensions[] = { ".py", ".pyw", NULL };
static char *PY_HL_keywords[] = {
    "def", "class", "if", "elif", "else", "for", "while", "break",
    "continue", "return", "import", "from", "as", "try", "except",
    "finally", "raise", "with", "yield", "lambda", "pass", "in", "is",
    "not", "and", "or", "async", "await", "global", "nonlocal", "del",
    "assert",

    "int|", "str|", "float|", "bool|", "list|", "dict|", "tuple|", "set|",
    "bytes|", "bytearray|", "None|", "True|", "False|", "self|", "cls|", NULL
};

/* POSIX Shell / Bash */
static char *SH_HL_extensions[] = { ".sh", ".bash", ".zsh", NULL };
static char *SH_HL_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "in", "function", "return", "exit", "break",
    "continue", "source",

    "local|", "export|", "readonly|", "declare|", "typeset|", "unset|",
    "shift|", "eval|", "exec|", "trap|", "set|", "echo|", "printf|",
    "read|", "test|", "true|", "false|", NULL
};

/* Makefile */
static char *MK_HL_extensions[] = { "Makefile", "makefile", "GNUmakefile", ".mk", NULL };
static char *MK_HL_keywords[] = {
    "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include",
    "-include", "sinclude", "define", "endef", "override", "export",
    "unexport", "vpath",

    ".PHONY|", ".DEFAULT_GOAL|", ".SUFFIXES|", ".SILENT|", ".ONESHELL|",
    "CC|", "CXX|", "CFLAGS|", "CXXFLAGS|", "LDFLAGS|", "LDLIBS|",
    "PREFIX|", "DESTDIR|", "BINDIR|", "MANDIR|", "SHELL|", "wildcard|",
    "patsubst|", "subst|", "shell|", "addprefix|", "addsuffix|", NULL
};

/* Rust */
static char *RS_HL_extensions[] = { ".rs", NULL };
static char *RS_HL_keywords[] = {
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
static char *GO_HL_extensions[] = { ".go", NULL };
static char *GO_HL_keywords[] = {
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
static char *ZIG_HL_extensions[] = { ".zig", NULL };
static char *ZIG_HL_keywords[] = {
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

static struct editorSyntax HLDB[] = {
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