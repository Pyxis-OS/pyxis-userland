#include "shell.h"

static bool separator(char character)
{
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\v' || character == '\f';
}

const char *parse_line(char *line, char **arguments, size_t capacity, size_t *count)
{
  char *read = line, *write = line;
  *count = 0;
  for (;;) {
    while (separator(*read)) {
      ++read;
    }
    if (!*read) {
      break;
    }
    if (*count + 1 >= capacity) {
      return "Too many arguments";
    }
    arguments[(*count)++] = write;
    char quote = 0;
    while (*read && (quote || !separator(*read))) {
      char character = *read++;
      if (character == '\\' && quote != '\'') {
        if (!*read) {
          return "Unfinished escape";
        }
        *write++ = *read++;
      } else if (quote && character == quote) {
        quote = 0;
      } else if (!quote && (character == '\'' || character == '"')) {
        quote = character;
      } else {
        *write++ = character;
      }
    }
    if (quote) {
      return "Unfinished quote";
    }
    /* Read past the delimiter before writing NUL: unquoted input may not
     * have moved, so read and write can still point to the same byte. */
    if (*read) {
      ++read;
    }
    *write++ = '\0';
  }
  arguments[*count] = NULL;
  return NULL;
}
