/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

#include <sys/ioctl.h>
#include <string.h>

#include "append_buffer.h"

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f) // bitwise AND operation with 00011111

enum editorKey
{
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

typedef struct erow
{
  int size;
  char *chars;
} erow;

struct editorConfig
{
  int cx, cy;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
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
int editorReadKey(void)
{
  int nread;
  char c;
  // read() returns the number of bytes read, or -1 if there was an error
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b')
  { // arrow key check
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[')
    {
      if (seq[1] >= '0' && seq[1] <= '9')
      {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~')
        {
          switch (seq[1])
          {
          case '1':
            return HOME_KEY;
          case '7':
            return HOME_KEY;
          case '4':
            return END_KEY;
          case '8':
            return END_KEY;
          case '3':
            return DEL_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          }
        }
      }
      else
      {

        switch (seq[1])
        {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    }
    else if (seq[0] == 'O')
    {
      switch (seq[1])
      {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
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

/** row opertions ***/
void editorAppendRow(char *s, size_t len)
{
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename)
{
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1)
  {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) // \n and \r indicate line ending sequence
      linelen--;

    editorAppendRow(line, linelen);
  }

  free(line);
  fclose(fp);
}

/*** input ***/

void editorMoveCursor(int key)
{
  switch (key)
  {
  case ARROW_LEFT:
    if (E.cx != 0)
      E.cx--;
    break;
  case ARROW_RIGHT:
    E.cx++;
    break;
  case ARROW_UP:
    if (E.cy != 0)
      E.cy--;
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows)
      E.cy++;
    break;
  }
}

void editorProcessKeypress(void)
{
  int c = editorReadKey();

  switch (c)
  {
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
    {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
  }

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

/*** output ***/

void editorScroll(void)
{
  if (E.cy < E.rowoff)
  { // cursor is above the visible window
    E.rowoff = E.cy;
  }

  if (E.cy >= E.rowoff + E.screenrows)
  { // cursor is below the visible window
    E.rowoff = E.cy - E.screenrows + 1;
  }

  if (E.cx < E.coloff)
  { // cursor is left of the visible window
    E.coloff = E.cx;
  }
  if (E.cx >= E.coloff + E.screencols)
  { // cursor is right of the visible window
    E.coloff = E.cx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab)
{
  int y;
  for (y = 0; y < E.screenrows; y++)
  {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows)
    {
      if (E.numrows == 0 && y == E.screenrows / 3)
      {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo Editor -- Version %s", KILO_VERSION);

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
    }
    else
    {
      int len = E.row[filerow].size - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].chars[E.coloff], len);
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
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide the cursor

  // \x1b = escape, 27 in decimal
  // J command for clearing the screen
  // abAppend(&ab, "\x1b[2J", 4);

  abAppend(&ab, "\x1b[H", 3); // repositioning the cursor

  editorDrawRows(&ab);

  // position the cursor at custom location
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // abAppend(&ab, "\x1b[H", 3); // repositioning the cursor to the top-left

  abAppend(&ab, "\x1b[?25h", 6); // show the cursor
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** init ***/

void initEditor(void)
{
  E.cx = 0;
  E.cy = 0;
  E.numrows = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[])
{
  enableRawMode();
  initEditor();

  if (argc >= 2)
  {
    editorOpen(argv[1]);
  }

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
