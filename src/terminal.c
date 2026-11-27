#include "ted.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

void termWrite(const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += (size_t)n;
    }
}

void die(const char *s) {
    termWrite("\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    termWrite("\x1b[2J\x1b[H", 7);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    termWrite("\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }

    // Enable button press (1000), button drag motion (1002), and SGR coordinates (1006)
    termWrite("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (E.resized) {
            return RESIZE_EVENT;
        }
        if (nread == -1 && errno != EAGAIN && errno != EINTR) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[5];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (seq[0] == '\x7f' || seq[0] == '\b') return CTRL_BACKSPACE;

        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] == '<') {
                // SGR mouse packet: ESC [ < Cb ; Cx ; Cy (M or m)
                char mbuf[32];
                unsigned int mlen = 0;
                char term = 0;
                while (mlen < sizeof(mbuf) - 1) {
                    char ch;
                    if (read(STDIN_FILENO, &ch, 1) != 1) return '\x1b';
                    if (ch == 'M' || ch == 'm') {
                        term = ch;
                        break;
                    }
                    mbuf[mlen++] = ch;
                }
                mbuf[mlen] = '\0';

                int cb = 0, cx = 0, cy = 0;
                if (term && sscanf(mbuf, "%d;%d;%d", &cb, &cx, &cy) == 3) {
                    E.mouse_btn = cb;
                    E.mouse_x = cx;
                    E.mouse_y = cy;
                    E.mouse_release = (term == 'm') ? 1 : 0;
                    return MOUSE_EVENT;
                }
                return '\x1b';
            } else if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[1] == '1' && seq[2] == ';') {
                    // CSI 1;<mod><dir> (modifier 2 = Shift, 5 = Ctrl)
                    if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b';
                    if (seq[3] == '2') {
                        switch (seq[4]) {
                            case 'A': return SHIFT_ARROW_UP;
                            case 'B': return SHIFT_ARROW_DOWN;
                            case 'C': return SHIFT_ARROW_RIGHT;
                            case 'D': return SHIFT_ARROW_LEFT;
                        }
                    } else if (seq[3] == '5') {
                        switch (seq[4]) {
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                        }
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    case 'Z': return SHIFT_TAB;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'C': return CTRL_ARROW_RIGHT;
                case 'D': return CTRL_ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }

    if (c == CTRL_KEY('h')) {
        return CTRL_BACKSPACE;
    }

    return c;
}

static int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

static int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }

    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

void editorHandleSigwinch(int sig) {
    (void)sig;
    E.resized = 1;
}

void editorUpdateWindowSize(void) {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
    E.screenrows -= 2; // Reserve space for status bar and message bar

    if (E.screenrows < 1) E.screenrows = 1;
    if (E.screencols < 1) E.screencols = 1;

    // Clamp scroll offsets so the viewport stays valid when shrinking
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}