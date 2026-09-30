#include "shell.h"
#include "../common/directory.h"
#include <mount.h>
#include <namespace.h>
#include <handle.h>
#include <space.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

static enum call_status root_available(struct shell *shell, const char *binding)
{
  for (size_t i = 0; i < shell->directory.root_count; ++i) {
    if (!strcmp(shell->roots[i].name, binding)) {
      return CALL_ALREADY_EXISTS;
    }
  }
  if (shell->namespace != HANDLE_INVALID) {
    handle_t existing;
    enum call_status status = namespace_lookup(shell->namespace, binding, &existing);
    if (existing != HANDLE_INVALID) {
      handle_close(existing);
    }
    if (status == CALL_OK || status == CALL_ENDPOINT_CLOSED) {
      return CALL_ALREADY_EXISTS;
    }
    /* Startup root names allow bytes outside the service namespace alphabet. */
    if (status != CALL_NOT_FOUND && status != CALL_BAD_REQUEST) {
      return status;
    }
  }
  return CALL_OK;
}

/* Reserve shell-owned storage before acquisition. The shell runs one command
 * at a time and publishes the selected binding only after a complete open. */
static enum call_status reserve_root(struct shell *shell, const char *name,
    size_t length, char **reserved)
{
  *reserved = NULL;
  if (!length) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < length; ++i) {
    if (name[i] == ':' || name[i] == '/') {
      return CALL_BAD_REQUEST;
    }
  }
  if (length >= STARTUP_MAX_SIZE || shell->directory.root_count == STARTUP_ROOT_LIMIT) {
    return CALL_LIMIT;
  }
  char *binding = malloc(length + 1);
  if (!binding) {
    return CALL_NO_MEMORY;
  }
  memcpy(binding, name, length);
  binding[length] = 0;
  enum call_status status = root_available(shell, binding);
  if (status != CALL_OK) {
    free(binding);
    return status;
  }
  *reserved = binding;
  return CALL_OK;
}

static enum call_status publish_root(struct shell *shell, char *name, handle_t root)
{
  /* Acquisition can wait while a provider changes the service namespace. */
  enum call_status status = root_available(shell, name);
  if (status != CALL_OK) {
    handle_close(root);
    free(name);
    return status;
  }
  size_t index = shell->directory.root_count++;
  shell->roots[index] = (struct path_root){name, root};
  shell->owns_root[index] = true;
  return CALL_OK;
}

static enum command_result mount_host(struct shell *shell, char **arguments, size_t count)
{
  bool optional = false, access_set = false;
  uint64_t access = MOUNT_ACCESS_READ_ONLY;
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
      return shell_error(shell, "usage: mount [--optional] [--read-only | --read-write] host\n");
    }
  }
  char *name;
  enum call_status status = reserve_root(shell, "host", 4, &name);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: mount", "host", status);
  }
  if (shell->host_mount == HANDLE_INVALID) {
    free(name);
    return optional ? COMMAND_OK :
        shell_directory_error(shell, "shell: mount", "host", CALL_UNAVAILABLE);
  }
  handle_t root;
  status = mount_open_root(shell->host_mount, access, &root);
  if (status != CALL_OK) {
    free(name);
    return shell_directory_error(shell, "shell: mount", "host", status);
  }
  status = publish_root(shell, name, root);
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "shell: mount", arguments[count - 1], status);
}

static bool partition_number(const char *text, uint64_t *number)
{
  *number = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || *number > (UINT32_MAX - (*text - '0')) / 10) {
      return false;
    }
    *number = *number * 10 + (*text - '0');
  }
  return *number != 0;
}

static enum command_result mount_volume(struct shell *shell, char **arguments, size_t count)
{
  if (count >= 2 && !strcmp(arguments[count - 1], "host")) {
    return mount_host(shell, arguments, count);
  }
  bool optional = false, read_only = false;
  uint64_t partition = 0;
  const char *volume = NULL;
  if (count < 2) {
    goto usage;
  }
  for (size_t i = 1; i + 1 < count; ++i) {
    if (!strcmp(arguments[i], "--optional") && !optional) {
      optional = true;
    } else if (!strcmp(arguments[i], "--read-only") && !read_only) {
      read_only = true;
    } else if (!strcmp(arguments[i], "--partition") && !partition && i + 2 < count) {
      if (!partition_number(arguments[++i], &partition)) {
        goto usage;
      }
    } else if (!strcmp(arguments[i], "--volume") && !volume && i + 2 < count) {
      volume = arguments[++i];
    } else {
      goto usage;
    }
  }
  const char *destination = arguments[count - 1];
  size_t length = strlen(destination);
  if (!read_only || !partition || !volume || !*volume || strlen(volume) > MOUNT_VOLUME_NAME_MAX || length < 4 ||
      strcmp(destination + length - 3, "://")) {
    goto usage;
  }
  char *name;
  enum call_status status = reserve_root(shell, destination, length - 3, &name);
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: mount", destination, status);
  }
  if (shell->native_mount == HANDLE_INVALID) {
    free(name);
    return optional ? COMMAND_OK :
        shell_directory_error(shell, "shell: mount", destination, CALL_UNAVAILABLE);
  }
  handle_t root;
  status = mount_open_volume(shell->native_mount, partition, volume,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES, &root);
  if (status != CALL_OK) {
    free(name);
    return shell_directory_error(shell, "shell: mount", destination, status);
  }
  status = publish_root(shell, name, root);
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "shell: mount", arguments[count - 1], status);

usage:
  return shell_error(shell,
      "usage: mount [--optional] --partition N --volume NAME --read-only NAME://\n"
      "       mount [--optional] [--read-only | --read-write] host\n");
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
    return mount_volume(shell, arguments, count);
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
    bool replace = count > 1 && !strcmp(arguments[1], "replace");
    bool optional = false;
    bool read_only = false;
    size_t index = 2;
    while (index < count && arguments[index][0] == '-') {
      if (!strcmp(arguments[index], "--optional") && !replace && !optional) {
        optional = true;
      } else if (!strcmp(arguments[index], "--read-only") && !read_only) {
        read_only = true;
      } else {
        break;
      }
      ++index;
    }
    if (count < index + 2 || (strcmp(arguments[1], "start") && !replace) ||
        arguments[index][0] == '-') {
      return shell_error(shell,
          "usage: service start [--optional] [--read-only] NAME IMAGE [arguments...]\n"
          "       service replace [--read-only] NAME IMAGE [arguments...]\n");
    }
    return shell_launch_service(shell, arguments[index], replace, optional, read_only,
        arguments + index + 1, count - index - 1);
  }
  return shell_launch(shell, arguments, count,
      command.background ? SHELL_BACKGROUND : SHELL_FOREGROUND,
      stage->redirections, stage->redirection_count);
}
