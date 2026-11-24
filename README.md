# tinyEDitor

- Tiny self-contained editor
- Zero dependencies
- ~1031 LOC (_Lines-Of-Code_)
- Centered ASCII splash banner
- Live SIGWINCH window-resize handling
- Dynamic line numbers
- Live C syntax highlighting
- Smart auto-indentation
- Ctrl-Z/Y: Undo/Redo
- and incremental search (Ctrl-F).

![TED-splash](images/TED-splash.png)

---

![TED-0.0.4](images/TED.png)

Building a text editor in C comes down to three key upfront architectural decisions:

  - **UI Backend**
      > A terminal interface using raw ANSI escape sequences (like the classic kilo editor, under 1,000 lines of standard C with zero dependencies), an `ncurses` TUI, or a graphical window using SDL2 or GTK.
  
  `ted` does not depend on `libcurses`, `SDL2` or `GTK`, it directly emits VT100 escapes on the terminal.

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
