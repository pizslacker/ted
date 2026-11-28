# Contributing to `ted`

Thank you for your interest in contributing to `ted`! `ted` is built to stay small, fast, portable across POSIX/Linux terminals, and free of external runtime dependencies.

## Design Principles

1. **Zero External Dependencies:** Core builds must rely exclusively on the C99 standard library and POSIX.1-2008 headers (`termios.h`, `unistd.h`, `sys/ioctl.h`, `sys/stat.h`, `signal.h`). Do not introduce `ncurses` or third-party libraries.
2. **Strict Compiler Hygiene:** All code must compile cleanly with zero warnings under:
   ```sh
   make CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror -pedantic"