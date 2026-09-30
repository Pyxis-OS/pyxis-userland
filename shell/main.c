#include "shell.h"
#include "../common/directory.h"
#include <startup.h>
#include <handle.h>
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
  handle_t script = startup_resource("script");
  if (script != HANDLE_INVALID && argc < 2) {
    fputs("shell: Missing script diagnostic name\n", stderr);
    return EXIT_FAILURE;
  }
  struct shell shell = {
    .script_name = script != HANDLE_INVALID ? argv[1] : NULL,
    .script_line = 1,
    .terminal = {startup_resource("input"), startup_resource("output")},
    .launcher = startup_resource("launcher"),
    .terminal_service = startup_resource("terminal"),
    .memory = startup_resource("memory"),
    .display = startup_resource("display"),
    .clock = startup_resource("clock"),
    .echo = startup_resource("echo"),
    .udp = startup_resource("udp"),
    .tcp = startup_resource("tcp"),
    .pipe = startup_resource("pipe"),
    .service = startup_resource("service"),
    .namespace_service = startup_resource("namespace_service"),
    .namespace = startup_namespace(),
    .random = startup_resource("random"),
    .net_config = startup_resource("net_config"),
    .keyboard = startup_resource("keyboard"),
    .space = startup_resource("space"),
    .profile = startup_resource("profile"),
    .app = startup_root("app"),
    .home = startup_root("home"),
    .host = startup_root("host"),
    .host_mount = startup_resource("host_mount"),
  };
  if (shell.terminal.input == HANDLE_INVALID || shell.terminal.output == HANDLE_INVALID ||
      shell.launcher == HANDLE_INVALID || shell.memory == HANDLE_INVALID ||
      shell.app == HANDLE_INVALID || shell.home == HANDLE_INVALID) {
    shell_error(&shell, "shell: Missing startup resource or filesystem root\n");
    return EXIT_FAILURE;
  }

  int result = EXIT_FAILURE;
  char *line = malloc(script != HANDLE_INVALID ? SHELL_SCRIPT_LINE_MAX + 1 : SHELL_LINE_CAPACITY);
  char **arguments = malloc(SHELL_LINE_CAPACITY * sizeof(*arguments));
  if (!line || !arguments) {
    shell_directory_error(&shell, "shell", "command storage", CALL_NO_MEMORY);
    goto done;
  }
  enum call_status status = shell_directory_init(&shell);
  if (status != CALL_OK) {
    shell_directory_error(&shell, "shell", "working directory", status);
    goto done;
  }

  shell.roots[0] = (struct path_root){"app", shell.app};
  shell.roots[1] = (struct path_root){"home", shell.home};
  shell.roots[2] = (struct path_root){"host", shell.host};
  shell.directory.roots = shell.roots;
  shell.directory.root_count = shell.host != HANDLE_INVALID ? 3 : 2;
  shell.directory.namespace = shell.namespace;

  if (script != HANDLE_INVALID) {
    result = shell_script(&shell, script, line, arguments);
    goto done;
  }

  for (;;) {
    struct term_line_result read = read_command(&shell, line);
    if (read.status == TERM_LINE_EOF) {
      result = EXIT_SUCCESS;
      break;
    }
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
    enum command_result command = shell_command(&shell, line, arguments);
    if (command == COMMAND_EXIT) {
      result = EXIT_SUCCESS;
      break;
    }
    if (command == COMMAND_FATAL) {
      break;
    }
  }

done:
  if (shell.owns_namespace && handle_close(shell.namespace) != 0) {
    result = EXIT_FAILURE;
  }
  shell_directory_close(&shell);
  if (shell.owns_host && handle_close(shell.host) != 0) {
    result = EXIT_FAILURE;
  }
  free(arguments);
  free(line);
  /* Runtime exit releases the original startup grants, which may alias. */
  return result;
}
