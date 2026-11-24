# Text EDitor

![TED-0.0.4](images/TED.png)

Building a text editor in C comes down to three key upfront architectural decisions:

  - **UI Backend**: A terminal interface using raw ANSI escape sequences (like the classic kilo editor, under 1,000 lines of standard C with zero dependencies), an `ncurses` TUI, or a graphical window using SDL2 or GTK.
      - `ted` does not depend on `libcurses`, `SDL2` or `GTK`, it directly emits VT100 escapes on the terminal.

  - **Buffer Data Structure**: An array of row structs (simplest for getting a working prototype on screen fast), a gap buffer (ideal for local cursor insertions), or a piece table / rope (best for large files and unlimited undo/redo).
      - `ted` uses a dynamic array of line structs (`erow *row`), where each line owns two heap-allocated flat character arrays (`chars` and `render`) plus a parallel syntax-highlight byte array (`hl`).

  - **Core Scope**: Starting with a minimal viable editor (raw mode input, viewport scrolling, file I/O, and search) versus designing upfront for syntax highlighting and multiple buffers.
