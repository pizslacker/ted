# ted — TinyEDitor

Minimal, zero-dependency terminal text editor written in C99 using raw `termios` input and ANSI escape sequences.

```text
  _            _
 | |_ ___   __| |
 | __/ _ \ / _` |
 | ||  __/| (_| |
  \__\___| \__,_|

  TinyEDitor  v1.0.0
```

## Features
- **Zero External Dependencies**:<br/>Built purely on standard C99 and POSIX (termios, ioctl, sigaction)—no ncurses or third-party libraries required.

- **Runtime Configuration (`~/.tedrc` & `Ctrl-R`)**:<br/>Customize tab width, auto-indentation, line numbers, quit confirmations, and ANSI syntax colors via `~/.tedrc` or `$XDG_CONFIG_HOME/ted/tedrc`, and reload changes live with `Ctrl-R`.

- **`+LINE` Startup Jump in `src/main.c`**:<br/>Parses any +N argument (matching vi and nano, such as ted +42 src/main.c or ted src/main.c +42), validates that N is a positive integer before entering raw mode, clamps it to [1, E.numrows], and centers the target line vertically in the viewport once editorOpen() finishes.

- **Modularized Codebase (`src/`)**:<br/>Split single-file `ted.c` into `src/ted.h` and 7 focused translation units (`main.c`, `terminal.c`, `syntax.c`, `buffer.c`, `edit.c`, `fileio.c`, and `render.c`).

- **Visual Selection & Range Clipboard (`Shift+Arrow`, Mouse Drag, `Ctrl-C/X/V`)**:<br/>Select arbitrary character or multi-line spans using `Shift+Arrow keys` or left-button mouse drag, copy (`Ctrl-C`), cut (`Ctrl-X`), and paste (`Ctrl-V`) with full atomic undo/redo support.

- **XTerm SGR Mouse Support**:<br/>Click anywhere in the text viewport to position the cursor (with full tab-stop and gutter awareness) and scroll smoothly using the mouse wheel.

- **Multi-Language Syntax Highlighting & Shebang Detection**:<br/>Highlights keywords, directives, built-in types, strings, character literals, numbers (decimal, float, hex), and single-line or multi-line comments across C/C++, Python, Shell/Bash, Makefile, Rust, Go, and Zig—plus automatic `#!` shebang detection for extensionless scripts.

- **Matching Bracket Highlighting**:<br/>Automatically highlights matching `( )`, `[ ]`, and `{ }` delimiters under the cursor while ignoring brackets inside strings and comments.

- **Incremental Search (`Ctrl-F`)**:<br/>Searches the buffer live as you type, cycles forward/backward across matches with arrow keys, and restores your original cursor and scroll position on Esc.

- **Atomic Undo / Redo (`Ctrl-Z` / `Ctrl-Y`)**:<br/>Reverses and replays character edits, word deletions, line splits, and line joins while keeping dirty-buffer status synchronized with disk saves.

- **Smart Indentation & Soft Tabs**:<br/>Preserves leading indentation on Enter, adds an extra 4-space indent after `{`, and supports 4-space Tab alignment and Shift-Tab outdentation.

- **Dynamic Line-Number Gutter**:<br/>Scales automatically with total file line count.

- **Line Manipulation (`Ctrl-D`, `Ctrl-K`, `Ctrl-U`)**:<br/>Duplicate the current line, cut it into an internal clipboard, and paste it below the cursor with full undo/redo support.

- **Safe File Handling & Live Resizing**:<br/>Validates directories and permissions before entering raw terminal mode, opens new named buffers cleanly when a target file does not yet exist, scales the line-number gutter dynamically, and responds immediately to terminal resizes via SIGWINCH.

- **Headless C Unit Test Suite (`tests/test_ted.c`)**:<br/>Added a standalone unit test runner covering tab-to-render coordinate mapping, smart auto-indentation, visual selection range cut/paste, multi-line block indent/outdent, 7-language syntax and #! shebang detection, string/comment-aware bracket matching, ~/.tedrc config validation, and atomic save trailing-whitespace trimming.

- **Build & Sanitizer Targets (`Makefile`)**:<br/>Added make check and make sanitize (compiling and running the test suite under -fsanitize=address,undefined with -Wall -Wextra -Werror -pedantic).

![TED-splash-0.7.0](images/TED-splash.png)

### Building & Installation

#### Build from Source
```Bash
make
```
#### Run
```Bash
./ted [OPTION]... [FILE]
```

Options:

  - `-h`, `--help` — Display usage summary and keybinding reference
  - `-v`, `--version` — Display version information
  - -- — End option parsing (useful for opening filenames starting with -)

### System-Wide Installation
Installs the ted binary to `/usr/local/bin/ted` and the manpage to `/usr/local/share/man/man1/ted.1`:
```Bash
sudo make install
```

To uninstall:
```Bash
sudo make uninstall
```

View the manual page at any time with:
```Bash
man ./ted.1   # Before installation
man ted       # After installation
```

### Memory & Undefined Behavior Safety

Every change touching buffer manipulation, undo/redo history, syntax scanning, or file I/O must pass both the standard test suite and the sanitizer suite:

```Bash
make check
make sanitize
```

## Architecture

Building a text editor in C comes down to three key upfront architectural decisions:

  - **UI Backend**
      > A terminal interface using raw ANSI escape sequences (like the classic kilo editor, under 1,000 lines of standard C with zero dependencies), an `ncurses` TUI, or a graphical window using SDL2 or GTK.
  
  `ted` does not depend on `libcurses`, `SDL2` or `GTK`, it directly emits ANSI escape sequences on the terminal and uses raw input.

  - **Buffer Data Structure**
      > An array of row structs (simplest for getting a working prototype on screen fast), a gap buffer (ideal for local cursor insertions), or a piece table / rope (best for large files and unlimited undo/redo).
  
  `ted` uses a dynamic array of line structs (`erow *row`), where each line owns two heap-allocated flat character arrays (`chars` and `render`) plus a parallel syntax-highlight byte array (`hl`).
      
  ***Memory Layout***
  ```text
  typedef struct erow {
      int idx;                // Line index in file
      int size;               // Length of raw chars
      int rsize;              // Length of rendered chars
      char *chars;            // Raw line buffer (allocated per line)
      char *render;           // Display buffer (tabs expanded to spaces)
      unsigned char *hl;      // Syntax highlight token ID per rendered byte
      int hl_open_comment;    // Multi-line comment state carried to next row
   } erow;
  ```
  
  - **Core Scope**
      > Starting with a minimal viable editor (raw mode input, viewport scrolling, file I/O, and search) versus designing upfront for syntax highlighting and multiple buffers.
  
  For `ted`'s current scope (source files up to tens of thousands of lines, live C syntax highlighting, and zero external dependencies), the array-of-`erows` design is the cleanest fit because line indexing, scrolling, and per-line syntax state map 1-to-1 to terminal rows.

### License

This project is open-source software licensed under the GNU General Public License v3.0 (GPLv3). See the [LICENSE](LICENSE) file for terms and conditions.

Copyright (C) k!M/pizslacker 2026
