/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
// termios contains the definitions used by terminal i/o interfaces
#include <termios.h>
// unistd is the header that provides access to the POSIX api
#include <unistd.h>

/*** defines ***/

// 0x1f is 00011111
// Masking the upper 3 bits effectively does what the Ctrl key does
// ASCII seems to have been designed this way on purpose, and also
// to toggle bit 5 to switch between lowercase and uppercase
#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editorConfig {
  int screenrows;
  int screencols;
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
char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
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
    abAppend(ab, "~", 1);

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
  // "J" clears the screen, and "2" is an argument to clear the entire screen
  // "<esc>[1J" would clear up to the cursor, and "<esc>[2J" would clear the cursor to end
  // "4" passed to write() means we're writing 4 bytes
  abAppend(&ab, "\x1b[2J", 4);
  // "H" command positions the cursor, and takes arguments for row and column (1 indexed)
  // "<esc>[12;40H" would place the cursor in the middle of a 80x24 size terminal
  // The default args are 1;1
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  // Repositions cursor after drawing
  abAppend(&ab, "\x1b[H", 3);
  // Turn cursor back on
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

/**
 * Handles a keypress
 */
void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

/*** init ***/

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
