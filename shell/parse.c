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

static const char *background_tail(const char *tail,
    struct shell_command_line *command)
{
  while (separator(*tail)) {
    ++tail;
  }
  if (*tail || !command->stage_count) {
    return "& requires a preceding command and must end the line";
  }
  if (!command->stages[command->stage_count - 1].count) {
    return "Pipeline stage requires a command";
  }
  if (command->stage_count > 1) {
    return "Background pipelines are unsupported";
  }
  command->background = true;
  return NULL;
}

static const char *begin_redirection(struct shell_stage *stage,
    struct shell_redirection **pending, enum startup_stream_index stream,
    const char *tail)
{
  if (!stage->count) {
    return "Redirection requires a preceding command";
  }
  if (*pending) {
    return "Redirection requires a filename";
  }
  if (*tail == '<' || *tail == '>' || *tail == '&' || *tail == '|') {
    return "Unsupported operator";
  }
  for (size_t i = 0; i < stage->redirection_count; ++i) {
    if (stage->redirections[i].stream == stream) {
      return "Duplicate redirection";
    }
  }
  *pending = &stage->redirections[stage->redirection_count++];
  (*pending)->stream = stream;
  (*pending)->path = NULL;
  return NULL;
}

static const char *begin_next_stage(char **arguments, size_t *used,
    struct shell_command_line *command, const char *tail)
{
  if (*tail == '|' || *tail == '&') {
    return "Unsupported operator";
  }
  if (!command->stage_count || !command->stages[command->stage_count - 1].count) {
    return "Pipeline stage requires a command";
  }
  if (command->stage_count == LAUNCH_BATCH_MAX) {
    return "Too many pipeline stages";
  }
  arguments[(*used)++] = NULL;
  command->stages[command->stage_count++] =
      (struct shell_stage){.arguments = arguments + *used};
  return NULL;
}

const char *parse_line(char *line, char **arguments, size_t capacity,
    struct shell_command_line *command)
{
  char *read = line, *write = line;
  struct shell_redirection *pending = NULL;
  size_t used = 0;
  command->stage_count = 0;
  command->background = false;
  if (!capacity) {
    return "Too many arguments";
  }

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
      const char *error = background_tail(read + 1, command);
      if (!error) {
        arguments[used] = NULL;
      }
      return error;
    }
    if (*read == '|') {
      if (pending) {
        return "Redirection requires a filename";
      }
      const char *error = begin_next_stage(arguments, &used, command, read + 1);
      if (error) {
        return error;
      }
      ++read;
      continue;
    }
    if (*read == '<' || *read == '>') {
      char operator = *read++;
      struct shell_stage *stage = command->stage_count ?
          &command->stages[command->stage_count - 1] : NULL;
      if (!stage) {
        return "Redirection requires a preceding command";
      }
      const char *error = begin_redirection(stage, &pending,
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
        struct shell_stage *stage = command->stage_count ?
            &command->stages[command->stage_count - 1] : NULL;
        if (!stage) {
          return "Redirection requires a preceding command";
        }
        const char *error = begin_redirection(stage, &pending, STARTUP_STDERR, read);
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
      if (used >= capacity - 1) {
        return "Too many arguments";
      }
      if (!command->stage_count) {
        command->stages[command->stage_count++] =
            (struct shell_stage){.arguments = arguments};
      }
      arguments[used++] = word;
      ++command->stages[command->stage_count - 1].count;
    }

    if (delimiter == '<' || delimiter == '>') {
      const char *error = begin_redirection(&command->stages[command->stage_count - 1],
          &pending,
          delimiter == '<' ? STARTUP_STDIN : STARTUP_STDOUT, read);
      if (error) {
        return error;
      }
    } else if (delimiter == '&') {
      const char *error = background_tail(read, command);
      if (!error) {
        arguments[used] = NULL;
      }
      return error;
    } else if (delimiter == '|') {
      const char *error = begin_next_stage(arguments, &used, command, read);
      if (error) {
        return error;
      }
    }
  }

  if (pending) {
    return "Redirection requires a filename";
  }
  if (command->stage_count && !command->stages[command->stage_count - 1].count) {
    return "Pipeline stage requires a command";
  }
  arguments[used] = NULL;
  return NULL;
}
