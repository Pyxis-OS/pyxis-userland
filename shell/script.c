#include "shell.h"
#include <file.h>
#include <stdlib.h>

static enum command_result script_command(struct shell *shell, char *line, char **arguments)
{
  /* Only whole-line comments are special; quoted or embedded # stays literal. */
  char *start = line;
  while (*start == ' ' || *start == '\t') {
    ++start;
  }
  if (*start == '#') {
    return COMMAND_OK;
  }
  return shell_command(shell, line, arguments);
}

int shell_script(struct shell *shell, handle_t script, char *line, char **arguments)
{
  uint64_t offset = 0;
  size_t length = 0;
  shell->script_line = 1;
  for (;;) {
    char bytes[256];
    size_t read;
    enum call_status status = file_read(script, offset, bytes, sizeof(bytes), &read);
    if (status != CALL_OK) {
      shell_directory_error(shell, "shell", "reading script", status);
      return EXIT_FAILURE;
    }
    if (!read) {
      if (!length) {
        return EXIT_SUCCESS;
      }
      line[length] = '\0';
      enum command_result result = script_command(shell, line, arguments);
      return result == COMMAND_OK || result == COMMAND_EXIT ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (read > UINT64_MAX - offset) {
      shell_error(shell, "shell: Script offset overflow\n");
      return EXIT_FAILURE;
    }
    offset += read;

    for (size_t i = 0; i < read; ++i) {
      if (bytes[i] == '\n') {
        if (length && line[length - 1] == '\r') {
          --length;
        }
        line[length] = '\0';
        enum command_result result = script_command(shell, line, arguments);
        if (result != COMMAND_OK) {
          return result == COMMAND_EXIT ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        ++shell->script_line;
        length = 0;
      } else {
        if (!bytes[i]) {
          shell_error(shell, "shell: NUL byte in script\n");
          return EXIT_FAILURE;
        }
        if (length == SHELL_SCRIPT_LINE_MAX) {
          shell_error(shell, "shell: Line limit reached; command not executed\n");
          return EXIT_FAILURE;
        }
        line[length++] = bytes[i];
      }
    }
  }
}
