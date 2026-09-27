#include "shell.h"
#include "../common/directory.h"
#include <mount.h>
#include <namespace.h>
#include <handle.h>
#include <space.h>
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
  bool optional = false, access_set = false;
  uint64_t access = MOUNT_ACCESS_READ_ONLY;
  if (count < 2 || strcmp(arguments[count - 1], "host")) {
    goto usage;
  }
  for (size_t i = 1; i + 1 < count; ++i) {
    if (!strcmp(arguments[i], "--optional") && !optional) {
      optional = true;
    } else if (!strcmp(arguments[i], "--read-only") && !access_set) {
      access = MOUNT_ACCESS_READ_ONLY;
      access_set = true;
    } else if (!strcmp(arguments[i], "--read-write") && !access_set) {
      access = MOUNT_ACCESS_READ_WRITE;
      access_set = true;
    } else {
      goto usage;
    }
  }
  if (shell->host != HANDLE_INVALID) {
    return shell_directory_error(shell, "shell: mount", "host", CALL_ALREADY_EXISTS);
  }
  if (shell->host_mount == HANDLE_INVALID) {
    return optional ? COMMAND_OK :
        shell_directory_error(shell, "shell: mount", "host", CALL_UNAVAILABLE);
  }

  handle_t root;
  enum call_status status = mount_open_root(shell->host_mount, access, &root);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: mount", "host", status);
  }
  shell->host = root;
  shell->owns_host = true;
  shell->roots[2] = (struct path_root){"host", root};
  shell->directory.root_count = 3;
  return COMMAND_OK;

usage:
  return shell_error(shell, "usage: mount [--optional] [--read-only | --read-write] host\n");
}

static enum command_result set_title(struct shell *shell, char **arguments, size_t count)
{
  bool optional = count > 1 && !strcmp(arguments[1], "--optional");
  if (count != (optional ? 3 : 2)) {
    return shell_error(shell, "usage: title [--optional] name\n");
  }
  enum call_status status = space_set_title(shell->space, arguments[count - 1]);
  /* libpyxis validates the text even when the fallback has no title grant. */
  if (optional && shell->space == HANDLE_INVALID && status == CALL_BAD_HANDLE) {
    return COMMAND_OK;
  }
  if (status != CALL_OK) {
    return shell_error(shell, "shell: title failed (status %u)\n", status);
  }
  return COMMAND_OK;
}

static bool is_builtin(const char *name)
{
  return !strcmp(name, "exit") || !strcmp(name, "cd") ||
      !strcmp(name, "mount") || !strcmp(name, "title") ||
      !strcmp(name, "session") || !strcmp(name, "namespace") ||
      !strcmp(name, "service");
}

enum command_result shell_command(struct shell *shell, char *line, char **arguments)
{
  struct shell_command_line command;
  const char *error = parse_line(line, arguments, SHELL_LINE_CAPACITY, &command);
  if (error) {
    return shell_error(shell, "shell: %s\n", error);
  }
  if (!command.stage_count) {
    return COMMAND_OK;
  }
  if (command.stage_count > 1) {
    for (size_t i = 0; i < command.stage_count; ++i) {
      const struct shell_stage *stage = &command.stages[i];
      if (!stage->count || !stage->arguments[0][0]) {
        return shell_error(shell, "shell: Pipeline stage requires a command name\n");
      }
      if (is_builtin(stage->arguments[0])) {
        return shell_error(shell, "shell: %s: Builtins are unsupported in pipelines\n",
            stage->arguments[0]);
      }
    }
    return shell_launch_pipeline(shell, &command);
  }

  const struct shell_stage *stage = &command.stages[0];
  arguments = stage->arguments;
  size_t count = stage->count;
  if (!arguments[0][0]) {
    return shell_error(shell, "shell: Empty command name\n");
  }
  bool builtin = is_builtin(arguments[0]);
  if (stage->redirection_count && (builtin || command.background)) {
    return shell_error(shell, "shell: Redirection is only supported for foreground external commands\n");
  }
  if (command.background && builtin) {
    return shell_error(shell, "shell: & is only supported for external commands\n");
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
  if (strcmp(arguments[0], "title") == 0) {
    return set_title(shell, arguments, count);
  }
  if (strcmp(arguments[0], "mount") == 0) {
    return mount_host(shell, arguments, count);
  }
  if (strcmp(arguments[0], "session") == 0) {
    if (count < 2) {
      return shell_error(shell, "usage: session program [arguments...]\n");
    }
    return shell_launch(shell, arguments + 1, count - 1, SHELL_SESSION, NULL, 0);
  }
  if (strcmp(arguments[0], "namespace") == 0) {
    if (count == 2 && !strcmp(arguments[1], "create")) {
      handle_t namespace_handle;
      enum call_status status = namespace_create(shell->namespace_service,
          &namespace_handle);
      if (status != CALL_OK) {
        return shell_error(shell, "namespace: create failed (status %u)\n", status);
      }
      if (shell->owns_namespace && handle_close(shell->namespace) != 0) {
        handle_close(namespace_handle);
        return shell_error(shell, "namespace: close previous namespace failed\n");
      }
      shell->namespace = namespace_handle;
      shell->owns_namespace = true;
      shell->directory.namespace = namespace_handle;
      return COMMAND_OK;
    }
    if (count == 3 && !strcmp(arguments[1], "remove")) {
      enum call_status status = namespace_remove(shell->namespace, arguments[2]);
      if (status != CALL_OK) {
        return shell_error(shell, "namespace: remove %s failed (status %u)\n",
            arguments[2], status);
      }
      return COMMAND_OK;
    }
    return shell_error(shell, "usage: namespace create | namespace remove NAME\n");
  }
  if (strcmp(arguments[0], "service") == 0) {
    if (count < 4 || (strcmp(arguments[1], "start") &&
        strcmp(arguments[1], "replace"))) {
      return shell_error(shell,
          "usage: service start|replace NAME IMAGE [arguments...]\n");
    }
    return shell_launch_service(shell, arguments[2],
        !strcmp(arguments[1], "replace"), arguments + 3, count - 3);
  }
  return shell_launch(shell, arguments, count,
      command.background ? SHELL_BACKGROUND : SHELL_FOREGROUND,
      stage->redirections, stage->redirection_count);
}
