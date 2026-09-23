#include "shell.h"
#include "../common/directory.h"
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct term_line_result read_command(struct shell *shell, char *line)
{
  size_t columns, rows;
  enum call_status status = term_size(&shell->terminal, &columns, &rows);
  if (status != CALL_OK) {
    return (struct term_line_result){.status = TERM_LINE_ERROR, .error = status};
  }

  /* Keep at least half the first row available for input. Only the displayed
   * prompt is shortened; child startup metadata keeps the full working path. */
  char prompt[SHELL_LINE_CAPACITY];
  size_t budget = columns / 2;
  if (budget > sizeof(prompt) - 1) {
    budget = sizeof(prompt) - 1;
  }
  size_t available = budget > 2 ? budget - 2 : 0;
  const char *path = shell->working_path;
  size_t length = strlen(path);
  size_t used = 0;
  if (length > available) {
    if (available >= 3) {
      memcpy(prompt, "...", 3);
      used = 3;
    }
    path += length - (available - used);
    length = available - used;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = path[i];
    prompt[used++] = byte >= ' ' && byte <= '~' ? byte : '?';
  }
  memcpy(prompt + used, "> ", 3);
  return term_read_line(&shell->terminal, prompt, line, SHELL_LINE_CAPACITY);
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  if (startup_resource("script") != HANDLE_INVALID) {
    fputs("shell: Script execution is not implemented yet\n", stderr);
    return EXIT_FAILURE;
  }
  struct shell shell = {
    .terminal = {startup_resource("input"), startup_resource("output")},
    .launcher = startup_resource("launcher"),
    .memory = startup_resource("memory"),
    .app = startup_root("app"),
    .home = startup_root("home"),
  };
  if (shell.terminal.input == HANDLE_INVALID || shell.terminal.output == HANDLE_INVALID ||
      shell.launcher == HANDLE_INVALID || shell.memory == HANDLE_INVALID ||
      shell.app == HANDLE_INVALID || shell.home == HANDLE_INVALID) {
    fputs("shell: Missing startup resource or filesystem root\n", stderr);
    return EXIT_FAILURE;
  }

  int result = EXIT_FAILURE;
  char *line = malloc(SHELL_LINE_CAPACITY);
  char **arguments = malloc(SHELL_LINE_CAPACITY * sizeof(*arguments));
  if (!line || !arguments) {
    perror("shell");
    goto done;
  }
  enum call_status status = shell_directory_init(&shell);
  if (status != CALL_OK) {
    report_directory_error("shell", "working directory", status);
    goto done;
  }

  for (;;) {
    struct term_line_result read = read_command(&shell, line);
    if (read.status == TERM_LINE_CANCELLED) {
      continue;
    }
    if (read.status == TERM_LINE_INPUT_LOST) {
      if (fputs("shell: Input lost; please re-enter the command\n", stderr) == EOF) {
        break;
      }
      continue;
    }
    if (read.status == TERM_LINE_ERROR) {
      report_directory_error("shell", "terminal", read.error);
      break;
    }
    if (read.limit_reached) {
      if (fputs("shell: Line limit reached; command not executed\n", stderr) == EOF) {
        break;
      }
      continue;
    }
    size_t count;
    const char *error = parse_line(line, arguments, SHELL_LINE_CAPACITY, &count);
    if (error) {
      if (fprintf(stderr, "shell: %s\n", error) < 0) {
        break;
      }
      continue;
    }
    if (!count) {
      continue;
    }
    if (strcmp(arguments[0], "exit") == 0) {
      if (count == 1) {
        result = EXIT_SUCCESS;
        break;
      }
      if (fputs("usage: exit\n", stderr) == EOF) {
        break;
      }
    } else if (strcmp(arguments[0], "cd") == 0) {
      if (count != 2) {
        if (fputs("usage: cd path\n", stderr) == EOF) {
          break;
        }
        continue;
      }
      status = shell_change_directory(&shell, arguments[1]);
      if (status != CALL_OK && report_directory_error("shell: cd", arguments[1], status) < 0) {
        break;
      }
    } else if (!shell_launch(&shell, arguments, count)) {
      break;
    }
  }

done:
  shell_directory_close(&shell);
  free(arguments);
  free(line);
  /* Runtime exit releases the original startup grants, which may alias. */
  return result;
}
