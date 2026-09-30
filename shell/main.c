#include "shell.h"
#include "../common/directory.h"
#include <startup.h>
#include <handle.h>
#include <terminal.h>
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

static bool report_completion(struct shell *shell, handle_t events, unsigned status)
{
  if (events == HANDLE_INVALID) {
    return true;
  }
  enum call_status result = terminal_command_complete(events, status);
  if (result != CALL_OK) {
    shell_directory_error(shell, "shell", "command completion", result);
    return false;
  }
  return true;
}

int main(int argc, char **argv)
{
  handle_t script = startup_resource("script");
  /* Kept outside launch state: only this interactive shell can report events. */
  handle_t events = script == HANDLE_INVALID ? startup_resource("terminal_events") :
      HANDLE_INVALID;
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
    .system_info = startup_resource("system_info"),
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
    .host_mount = startup_resource("host_mount"),
    .native_mount = startup_resource("native_mount"),
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
  size_t root_count = startup_root_count();
  if (root_count > STARTUP_ROOT_LIMIT) {
    shell_directory_error(&shell, "shell", "selected roots", CALL_LIMIT);
    goto done;
  }
  const struct startup_binding *roots = startup_roots();
  for (size_t i = 0; i < root_count; ++i) {
    shell.roots[i] = (struct path_root){(const char *)(uintptr_t)roots[i].name,
        roots[i].handle};
  }
  enum call_status status = shell_directory_init(&shell);
  if (status != CALL_OK) {
    shell_directory_error(&shell, "shell", "working directory", status);
    goto done;
  }

  shell.directory.roots = shell.roots;
  shell.directory.root_count = root_count;
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
      if (!report_completion(&shell, events, EXIT_FAILURE)) {
        break;
      }
      continue;
    }
    const char *start = line;
    while (*start == ' ' || *start == '\t') {
      ++start;
    }
    if (!*start) {
      continue;
    }
    enum command_result command = shell_command(&shell, line, arguments);
    if (command == COMMAND_FATAL) {
      break;
    }
    unsigned status = command == COMMAND_FAILED ? EXIT_FAILURE : EXIT_SUCCESS;
    if (!report_completion(&shell, events, status)) {
      break;
    }
    if (command == COMMAND_EXIT) {
      result = EXIT_SUCCESS;
      break;
    }
  }

done:
  if (shell.owns_namespace && handle_close(shell.namespace) != 0) {
    result = EXIT_FAILURE;
  }
  for (size_t i = 0; i < shell.directory.root_count; ++i) {
    if (shell.owns_root[i]) {
      if (handle_close(shell.roots[i].handle) != 0) {
        result = EXIT_FAILURE;
      }
      free((void *)shell.roots[i].name);
    }
  }
  shell_directory_close(&shell);
  free(arguments);
  free(line);
  /* Runtime exit releases the original startup grants, which may alias. */
  return result;
}
