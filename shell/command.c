#include "shell.h"
#include "../common/directory.h"
#include <mount.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool report_location(const struct shell *shell)
{
  return !shell->script_name ||
      fprintf(stderr, "%s:%zu: ", shell->script_name, shell->script_line) >= 0;
}

enum command_result shell_error(struct shell *shell, const char *format, ...)
{
  if (!report_location(shell)) {
    return COMMAND_FATAL;
  }
  va_list arguments;
  va_start(arguments, format);
  int written = vfprintf(stderr, format, arguments);
  va_end(arguments);
  return written < 0 ? COMMAND_FATAL : COMMAND_FAILED;
}

enum command_result shell_directory_error(struct shell *shell, const char *operation,
    const char *path, enum call_status status)
{
  if (!report_location(shell) || report_directory_error(operation, path, status) < 0) {
    return COMMAND_FATAL;
  }
  return COMMAND_FAILED;
}

static enum command_result mount_host(struct shell *shell, char **arguments, size_t count)
{
  bool optional = count == 3 && !strcmp(arguments[1], "--optional");
  if ((count != 2 && !optional) || strcmp(arguments[count - 1], "host")) {
    return shell_error(shell, "usage: mount [--optional] host\n");
  }
  if (shell->host != HANDLE_INVALID) {
    return shell_directory_error(shell, "shell: mount", "host", CALL_ALREADY_EXISTS);
  }
  if (shell->host_mount == HANDLE_INVALID) {
    return optional ? COMMAND_OK :
        shell_directory_error(shell, "shell: mount", "host", CALL_UNAVAILABLE);
  }

  handle_t root;
  enum call_status status = mount_open_root(shell->host_mount, &root);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: mount", "host", status);
  }
  shell->host = root;
  shell->owns_host = true;
  shell->roots[2] = (struct path_root){"host", root};
  shell->directory.root_count = 3;
  return COMMAND_OK;
}

enum command_result shell_command(struct shell *shell, char *line, char **arguments)
{
  size_t count;
  const char *error = parse_line(line, arguments, SHELL_LINE_CAPACITY, &count);
  if (error) {
    return shell_error(shell, "shell: %s\n", error);
  }
  if (!count) {
    return COMMAND_OK;
  }
  if (strcmp(arguments[0], "exit") == 0) {
    if (count != 1) {
      return shell_error(shell, "usage: exit\n");
    }
    return COMMAND_EXIT;
  }
  if (strcmp(arguments[0], "cd") == 0) {
    if (count != 2) {
      return shell_error(shell, "usage: cd path\n");
    }
    enum call_status status = shell_change_directory(shell, arguments[1]);
    if (status != CALL_OK) {
      return shell_directory_error(shell, "shell: cd", arguments[1], status);
    }
    return COMMAND_OK;
  }
  if (strcmp(arguments[0], "mount") == 0) {
    return mount_host(shell, arguments, count);
  }
  if (strcmp(arguments[0], "session") == 0) {
    if (count < 2) {
      return shell_error(shell, "usage: session program [arguments...]\n");
    }
    return shell_launch(shell, arguments + 1, count - 1, SHELL_SESSION);
  }
  return shell_launch(shell, arguments, count, SHELL_FOREGROUND);
}
