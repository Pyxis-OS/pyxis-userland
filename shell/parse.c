#include "shell.h"

static bool separator(char character)
{
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\v' || character == '\f';
}

static bool digit(char character)
{
  return character >= '0' && character <= '9';
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

static const char *begin_redirection(struct shell_command_line *command,
    struct shell_redirection **pending, enum startup_stream_index stream,
    const char *tail)
{
  if (!command->count) {
    return "Redirection requires a preceding command";
  }
  if (*pending) {
    return "Redirection requires a filename";
  }
  if (*tail == '<' || *tail == '>' || *tail == '&' || *tail == '|') {
    return "Unsupported operator";
  }
  for (size_t i = 0; i < command->redirection_count; ++i) {
    if (command->redirections[i].stream == stream) {
      return "Duplicate redirection";
    }
  }
  *pending = &command->redirections[command->redirection_count++];
  (*pending)->stream = stream;
  (*pending)->path = NULL;
  return NULL;
}

const char *parse_line(char *line, char **arguments, size_t capacity,
    struct shell_command_line *command)
{
  char *read = line, *write = line;
  struct shell_redirection *pending = NULL;
  command->count = 0;
  command->background = false;
  command->redirection_count = 0;

  for (;;) {
    while (separator(*read)) {
      ++read;
    }
    if (!*read) {
      break;
    }
    if (*read == '&') {
      if (pending) {
        return "Redirection requires a filename";
      }
      arguments[command->count] = NULL;
      return background_tail(read + 1, command->count, &command->background);
    }
    if (*read == '|') {
      return "Unsupported operator";
    }
    if (*read == '<' || *read == '>') {
      char operator = *read++;
      const char *error = begin_redirection(command, &pending,
          operator == '<' ? STARTUP_STDIN : STARTUP_STDOUT, read);
      if (error) {
        return error;
      }
      continue;
    }
    if (digit(*read)) {
      char *after_digits = read;
      while (digit(*after_digits)) {
        ++after_digits;
      }
      if (*after_digits == '<' || *after_digits == '>') {
        if (after_digits != read + 1 || *read != '2' || *after_digits != '>') {
          return "Unsupported descriptor redirection";
        }
        read = after_digits + 1;
        const char *error = begin_redirection(command, &pending, STARTUP_STDERR, read);
        if (error) {
          return error;
        }
        continue;
      }
    }

    char *word = write;
    char quote = 0;
    while (*read && (quote || (!separator(*read) && *read != '<' && *read != '>' &&
        *read != '&' && *read != '|'))) {
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

    /* Consume the delimiter before writing NUL: uncompacted input can leave
     * read and write pointing at the same operator or separator byte. */
    char delimiter = *read;
    if (delimiter) {
      ++read;
    }
    *write++ = '\0';

    if (pending) {
      if (write == word + 1) {
        return "Redirection requires a filename";
      }
      pending->path = word;
      pending = NULL;
    } else {
      if (command->count + 1 >= capacity) {
        return "Too many arguments";
      }
      arguments[command->count++] = word;
    }

    if (delimiter == '<' || delimiter == '>') {
      const char *error = begin_redirection(command, &pending,
          delimiter == '<' ? STARTUP_STDIN : STARTUP_STDOUT, read);
      if (error) {
        return error;
      }
    } else if (delimiter == '&') {
      arguments[command->count] = NULL;
      return background_tail(read, command->count, &command->background);
    } else if (delimiter == '|') {
      return "Unsupported operator";
    }
  }

  if (pending) {
    return "Redirection requires a filename";
  }
  arguments[command->count] = NULL;
  return NULL;
}
