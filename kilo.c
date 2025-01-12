/*** includes ***/

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "append_buffer.h"

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f) // bitwise AND operation with 00011111

/*** data ***/

struct editorConfig
{
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/**
 * Prints an error message (found in errno and `s`) and exits the program.
 * @param s Context of where the error occurred.
 */
void die(const char *s)
{
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

/**
 * Disable raw mode and return the terminal to its original state.
 */
void disableRawMode(void)
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

/**
 * Moves the terminal out of canonical mode and into raw mode.
 * Automatically, move the terminal back to canonical mode when the program exits.
 */
void enableRawMode(void)
{
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");

  atexit(disableRawMode); // disable raw mode when the program exits

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(IXON | ICRNL);
  raw.c_cflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP); // turn off software flow control
  raw.c_oflag &= ~(OPOST);                                  // turn off output processing
  raw.c_cflag |= (CS8);                                     // set character size to 8 bits per byte
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);          // turn off echoing, canonical mode, and signals (TODO: for undo button, this needs to be fix))
                                                            // ctrl-z = byte 26 (undo)
                                                            // ctrl-y = byte 25 (redo)

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

/**
 * Reads a key/input from the terminal.
 */
char editorReadKey(void)
{
  int nread;
  char c;
  // read() returns the number of bytes read, or -1 if there was an error
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  return c;
}

/**
 * Get the location of the cursor in the terminal.
 */
int getCursorPosition(int *rows, int *cols)
{
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1)
  {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break; // always the last character from the `\x1b[6n]` command
    i++;
  }

  buf[i] = '\0';

  // parsing the location of cursor from the buffer
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

/**
 * Sets the pointers (rows and cols) to the window size.
 * If the window size is 0 or there is failure to get the window size, return -1.
 */
int getWindowSize(int *rows, int *cols)
{
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
  {                                                           // fallback if ioctl fails
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) // bottom right corner
      return -1;
    return getCursorPosition(rows, cols);
  }
  else
  {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
  }
}

/*** input ***/

void editorProcessKeypress(void)
{
  char c = editorReadKey();

  switch (c)
  {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

/*** output ***/

void editorDrawRows(struct abuf *ab)
{
  int y;
  for (y = 0; y < E.screenrows; y++)
  {

    if (y == E.screenrows / 3)
    {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);

      if (welcomelen > E.screencols)
        welcomelen = E.screencols;

      int padding = (E.screencols - welcomelen) / 2;
      if (padding)
      {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--)
        abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    }
    else
    {
      abAppend(ab, "~", 1);
    }

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1)
    {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/**
 * Clears the screen for the editor.
 * This will use VT100 escape sequences.
 */
void editorRefreshScreen(void)
{
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide the cursor

  // \x1b = escape, 27 in decimal
  // J command for clearing the screen
  // abAppend(&ab, "\x1b[2J", 4);

  abAppend(&ab, "\x1b[H", 3); // repositioning the cursor

  editorDrawRows(&ab);
  abAppend(&ab, "\x1b[H", 3); // repositioning the cursor to the top-left

  abAppend(&ab, "\x1b[?25h", 6); // show the cursor
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** init ***/

void initEditor(void)
{
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(void)
{
  enableRawMode();
  initEditor();

  // errno != EAGAIN is for Cygwin; don't treat EAGAIN as an error
  // all control characters ASII (0-31, 127)

  // use "\r\n" for every line
  while (1)
  {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
