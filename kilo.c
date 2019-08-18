#include <ctype.h>
#include <stdlib.h>
// termios contains the definitions used by terminal i/o interfaces
#include <termios.h>
// unistd is the header that provides access to the POSIX api
#include <unistd.h>

struct termios orig_termios;

void disableRawMode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
  // STDIN_FILENO is a file descriptor for standard input, and is 0
  tcgetattr(STDIN_FILENO, &orig_termios);
  // atexit is from stdlib
  atexit(disableRawMode);
  struct termios raw = orig_termios;
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
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
  enableRawMode();

  while (1) {
    char c = '\0';
    read(STDIN_FILENO, &c, 1);
    // iscntrl, from ctype, tests whether a character is a control character
    // \r is added because we disabled output processing which automatically inserts it with newlines
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q') break;
  }

  return 0;
}
