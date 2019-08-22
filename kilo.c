/*** includes ***/

// Add feature test macros for portability. These are above the includes, as the
// headers will decide what to include based on the macros
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
// termios contains the definitions used by terminal i/o interfaces
#include <termios.h>
// unistd is the header that provides access to the POSIX api
#include <unistd.h>

/*** defines ***/


#define KILO_VERSION "0.0.1"
// 0x1f is 00011111
// Masking the upper 3 bits effectively does what the Ctrl key does
// ASCII seems to have been designed this way on purpose, and also
// to toggle bit 5 to switch between lowercase and uppercase
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

// The typedef lets us refer to the type as "erow" instead of "struct erow"
typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  // Most C lib functions that fail will set the global "errno"
  // perror() looks at errno and prints a descriptive error message and also the string passed to it, which is meant to provide context
  perror(s); // from stdio
  exit(1); // from stdlib
}

void disableRawMode() {
  // NOTE: we don't clear the screen here, because the error message printed by
  // die() would get erased
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  // STDIN_FILENO is a file descriptor for standard input, and is 0
  // tcgetattr() will fail if given a text file or pipe as stdin instead of the terminal
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  // atexit is from stdlib
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  // c_iflag contains for "input modes"
  // ICRNL sets whether carriage returns (CR) are translated to newlines (NL)
  //
  // IXON sets the sending of XOFF (ctrl-s) and XON (ctrl-q)
  // BRKINT, INPCK, ISTRP, and CS8 are typically already turned off, and don't apply to modern terminals
  // BRKNT sets whether a break condition will cause a SIGINT to be sent, like ctrl-c.
  // INPCK enables parity checking
  // ISTRIP causes the 8th bit to be set to 0
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  // c_cflag contains flags for "control modes"
  // CS8 is not a flag, it is a bit mask with multiple bits, which is why we use bitwise OR. This sets the character size to 8 bits per byte
  raw.c_cflag |= (CS8);
  // c_oflag contains flags for "output modes"
  // OPOST set output processing, such as converting "\n" to "\r\n". This was for typewriters and teletypes back in the day, where the carriage first returns to the starting location and then shifts down, and this output is probably the only output flag set by default
  raw.c_oflag &= ~(OPOST);
  // c_lflag contains flags for "local modes" such as echo
  // This turns off the ECHO and ICANON bit
  // ICANON sets canonical mode, or cooked mode, which doesn't send keyboard input until the user presses enter
  // IEXTEN fixes ctrl-v, which on some systems waits for another character and then sends that character literally. Also fixes ctrl-o on macOS
  // ISIG controls the sending of SIGINT (ctrl-c) and SIGTSTP (ctrl-z)
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // c_cc are "control characters", an array of bytes that control terminal settings
  // VMIN sets the min number of bytes before read() can return, so by setting it to 0 it returns as soon as there is any input
  raw.c_cc[VMIN] = 0;
  // VTIME sets the max time to wait before read() returns
  raw.c_cc[VTIME] = 1;

  // 2nd arg to tcsetattr are optional actions
  // TCSAFLUSH waits for pending output and discards unread output
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/**
 * Waits for one keypress and returns it
 */
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    // If we read an escape character, immediately read two more bytes
    // Setting it as 3 bytes to handle longer escape sequences
    char seq[3];
    // If either of these timeout, assume the user just pressed Esc
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    // "Home" and "End" sequences depend on the OS and terminal emulator, so we
    // need to handle all of the different sequences
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
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
        }
      } else {
        switch(seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  // The "n" command (device status report) can be used to query the terminal
  // for status info. The argument "6" asks for cursor position
  // The reply is an escape sequence, such as "<esc>[24;101R"
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  // printf expects strings to end with a 0 byte
  buf[i] = '\0';

  // First check if responded with an escape sequence
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  // Then start the string at the 3rd element, using sscanf to parse
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // ioctl(), TIOCGWINSZ, and struct winsize all come from sys/ioctl.h
  // On failure, ioctl() returns -1, but we also needs to check if the values
  // are 0, which is also an error. ioctl() isn't guaranteed to be able to
  // request the window size on all systems, so we need a fallback method of
  // moving the cursor to the bottom right and querying its position.
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // The "C" (cursor forward) command moves the cursor to the right
    // The "B" (cursor down) command moves the cursor down
    // Both commands do not move past the edge of the screen, so we use a large
    // value 999, as opposed to setting the cursor with H, which doesn't specify
    // what to do when you try to move the cursor offscreen
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
  // Increase the size of E.row by one
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  // Add one to account for string termination
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  // size_t for returning size in bytes
  // size_t when it could be a size or a (negative) error value
  size_t linecap = 0;
  ssize_t linelen;
  // getline() is useful when we don't how much memory to allocate for each
  // line, as it manages memory.
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/

// C doesn't have a dynamic string class, so we write one ourselves so that we
// can create a string buffer that we can append to
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  // Ask realloc() to give us a block of memory which is the size of the current
  // string (ab->len) plus the size of the string we are appending (len).
  // realloc() will either extend the size of the current block or free and
  // allocate a new block somewhere else.
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  // memcpy() comes from string.h
  // Copies string "s" after the end of data in the buffer
  memcpy(&new[ab->len], s, len);
  // Update pointers and length after the copy
  ab->b = new;
  ab->len += len;
}

// Destructor that deallocates the dynamic memory used by an abuf
void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

/**
 * Draws each row of the buffer of text being edited
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y >= E.numrows) {
      // Only display the welcome if there are no lines
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        // snprintf() writes formatted output to a sized buffer
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        // Truncate incase the window is too narrow
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[y].size;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, E.row[y].chars, len);
    }

    // Instead of "J" to clear the screen, we clear the line as an optimization
    // "K" command (erase in line) clears the line and is analogous to the J command
    // 2 erases the whole line, 1 to the cursor left, and 0 to the right (default)
    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      // Don't print on the last line to avoid scrolling the terminal
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  // "h" and "l" commands are "set mode" and "reset mode", used to turn off
  // various terminal features. VT100 doesn't document ?25, which hides the
  // cursor, so this won't work in some terminals
  abAppend(&ab, "\x1b[?25l", 6);
  // "\x1b" is the escape character, or 27 in decimal
  // Escape sequences always start with the escape character
  // followed by "["
  // Escape sequences tell the terminal to do various formatting
  // "H" command positions the cursor, and takes arguments for row and column (1 indexed)
  // "<esc>[12;40H" would place the cursor in the middle of a 80x24 size terminal
  // The default args are 1;1
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  // Move the cursor position using "H" command
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));
  // Turn cursor back on
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    }
    break;
  case ARROW_RIGHT:
    if (E.cx != E.screencols - 1) {
      E.cx++;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--;
    }
    break;
  case ARROW_DOWN:
    if (E.cy != E.screenrows - 1) {
      E.cy++;
    }
    break;
  }
}

/**
 * Handles a keypress
 */
void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    E.cx = E.screencols - 1;
    break;

  case PAGE_UP:
  case PAGE_DOWN:
    {
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.numrows = 0;
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  // Call only if a filename is passed in
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
