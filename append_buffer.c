#include <stdlib.h>
#include <string.h>

#include "append_buffer.h"

/** Appends the string provided to this dynamic string. */
void abAppend(struct abuf *ab, const char *s, int len)
{
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) // insufficient memory, memory fragmentation
    return;
  memcpy(&new[ab->len], s, len);

  ab->b = new;
  ab->len += len;
}

/** Free the memory allocated for the dynamic string. */
void abFree(struct abuf *ab)
{
  free(ab->b);
}
