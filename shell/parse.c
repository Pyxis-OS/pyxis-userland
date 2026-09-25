#include "shell.h"

static bool separator(char character)
{
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\v' || character == '\f';
}

static const char *background_tail(const char *tail, size_t count, bool *background)
{
  while (separator(*tail)) {
    ++tail;
  }
  if (*tail || !count) {
    return "& requires a preceding command and must end the line";
  }
  *background = true;
  return NULL;
}

const char *parse_line(char *line, char **arguments, size_t capacity, size_t *count,
    bool *background)
{
  char *read = line, *write = line;
  *count = 0;
  *background = false;
  for (;;) {
    while (separator(*read)) {
      ++read;
    }
    if (!*read) {
      break;
    }
    if (*read == '&') {
      arguments[*count] = NULL;
      return background_tail(read + 1, *count, background);
    }
    if (*count + 1 >= capacity) {
      return "Too many arguments";
    }
    arguments[(*count)++] = write;
    char quote = 0;
    while (*read && (quote || (!separator(*read) && *read != '&'))) {
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
    if (*read == '&') {
      const char *error = background_tail(read + 1, *count, background);
      *write = '\0';
      arguments[*count] = NULL;
      return error;
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
