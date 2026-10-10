#include <pyxis/working_path.h>
#include "shell.h"
#include "../common/directory.h"
#include <mount.h>
#include <namespace.h>
#include <power.h>
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
  for (size_t i = 0; i < shell->directory->root_count; ++i) {
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
  if (length >= STARTUP_MAX_SIZE || shell->directory->root_count == STARTUP_ROOT_LIMIT) {
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
  size_t index = shell->directory->root_count;
  shell->roots[index] = (struct path_root){name, root};
  status = pyxis_working_bindings(shell->roots, index + 1, shell->namespace);
  if (status != CALL_OK) {
    shell->roots[index] = (struct path_root){0};
    handle_close(root);
    free(name);
    return status;
  }
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
  bool optional = false, read_only = false, access_set = false, no_info = false;
  uint64_t partition = 0;
  const char *volume = NULL;
  if (count < 2) {
    goto usage;
  }
  for (size_t i = 1; i + 1 < count; ++i) {
    if (!strcmp(arguments[i], "--optional") && !optional) {
      optional = true;
    } else if (!strcmp(arguments[i], "--read-only") && !access_set) {
      read_only = true;
      access_set = true;
    } else if (!strcmp(arguments[i], "--read-write") && !access_set) {
      access_set = true;
    } else if (!strcmp(arguments[i], "--no-info") && !no_info) {
      no_info = true;
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
  if (!access_set || !partition || !volume || !*volume || strlen(volume) > MOUNT_VOLUME_NAME_MAX || length < 4 ||
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
  uint64_t rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES;
  if (!read_only) {
    rights = DIRECTORY_CONTENT_RIGHTS;
  }
  if (!no_info) {
    struct handle_info authority;
    status = handle_query(shell->native_mount, &authority);
    if (status != CALL_OK) {
      free(name);
      return shell_directory_error(shell, "shell: mount", destination, status);
    }
    if (authority.rights & MOUNT_RIGHT_OBSERVE) {
      rights |= DIRECTORY_RIGHT_FILESYSTEM_INFO;
    }
  }
  handle_t root;
  status = mount_open_volume(shell->native_mount, partition, volume, rights, &root);
  if (status != CALL_OK) {
    free(name);
    return shell_directory_error(shell, "shell: mount", destination, status);
  }
  status = publish_root(shell, name, root);
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "shell: mount", arguments[count - 1], status);

usage:
  return shell_error(shell,
      "usage: mount [--optional] [--no-info] --partition N --volume NAME "
      "(--read-only | --read-write) NAME://\n"
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

/* Shell parsing bound only; the kernel rejects indices beyond the boot. */
#define AFFINITY_CPU_LIMIT 8192

static bool parse_cpu_index(const char **cursor, size_t *value)
{
  const char *text = *cursor;
  if (*text < '0' || *text > '9') {
    return false;
  }
  size_t index = 0;
  while (*text >= '0' && *text <= '9') {
    index = index * 10 + (size_t)(*text++ - '0');
    if (index >= AFFINITY_CPU_LIMIT) {
      return false;
    }
  }
  *cursor = text;
  *value = index;
  return true;
}

/* LIST is comma-separated boot CPU indices and inclusive A-B ranges. */
static bool parse_cpu_list(const char *list, uint64_t *cpus, size_t *cpu_count)
{
  const char *cursor = list;
  *cpu_count = 0;
  for (;;) {
    size_t first, last;
    if (!parse_cpu_index(&cursor, &first)) {
      return false;
    }
    last = first;
    if (*cursor == '-') {
      ++cursor;
      if (!parse_cpu_index(&cursor, &last) || last < first) {
        return false;
      }
    }
    for (size_t cpu = first; cpu <= last; ++cpu) {
      cpus[cpu / 64] |= UINT64_C(1) << (cpu % 64);
    }
    if (last + 1 > *cpu_count) {
      *cpu_count = last + 1;
    }
    if (!*cursor) {
      return true;
    }
    if (*cursor++ != ',') {
      return false;
    }
  }
}

static enum command_result set_affinity(struct shell *shell, char **arguments, size_t count)
{
  uint64_t cpus[AFFINITY_CPU_LIMIT / 64] = {0};
  size_t cpu_count;
  if (count != 2 || !parse_cpu_list(arguments[1], cpus, &cpu_count)) {
    return shell_error(shell, "usage: affinity LIST (for example 2-3 or 1,3)\n");
  }
  enum call_status status = space_set_affinity(shell->space, cpus, cpu_count);
  switch (status) {
  case CALL_OK:
    return COMMAND_OK;
  case CALL_ENDPOINT_CLOSED:
    return shell_error(shell, "shell: affinity: this space has already launched a program\n");
  case CALL_DENIED:
    return shell_error(shell, "shell: affinity: no authority, or a CPU outside the space's ceiling\n");
  case CALL_BAD_REQUEST:
    return shell_error(shell, "shell: affinity: empty set or a CPU this boot does not have\n");
  default:
    return shell_error(shell, "shell: affinity failed (status %u)\n", status);
  }
}

/* Success never returns: the kernel powers off or restarts. */
static enum command_result power_command(struct shell *shell, char **arguments, size_t count)
{
  const char *name = arguments[0];
  if (count != 1) {
    return shell_error(shell, "usage: %s\n", name);
  }
  if (shell->power == HANDLE_INVALID) {
    return shell_error(shell, "%s: this space has no power authority\n", name);
  }
  enum call_status status = strcmp(name, "poweroff") == 0 ? power_off(shell->power) :
      power_restart(shell->power);
  return shell_error(shell, "%s: failed (status %u); the system stays up\n", name, status);
}

static bool is_builtin(char **arguments, size_t count)
{
  const char *name = arguments[0];
  return !strcmp(name, "exit") || !strcmp(name, "cd") ||
      !strcmp(name, "mount") || !strcmp(name, "title") || !strcmp(name, "affinity") ||
      !strcmp(name, "session") || !strcmp(name, "namespace") ||
      !strcmp(name, "service") || !strcmp(name, "poweroff") || !strcmp(name, "reboot") ||
      (!strcmp(name, "sync") && count > 1 && !strcmp(arguments[1], "--disk"));
}

static enum command_result builtin_command(struct shell *shell, char **arguments, size_t count)
{
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
  if (strcmp(arguments[0], "affinity") == 0) {
    return set_affinity(shell, arguments, count);
  }
  if (strcmp(arguments[0], "mount") == 0) {
    return mount_volume(shell, arguments, count);
  }
  if (strcmp(arguments[0], "sync") == 0) {
    if (count != 2) {
      return shell_error(shell, "usage: sync --disk | sync path...\n");
    }
    enum call_status status = shell->native_mount == HANDLE_INVALID ? CALL_UNAVAILABLE :
        mount_sync(shell->native_mount);
    return status == CALL_OK ? COMMAND_OK :
        shell_directory_error(shell, "shell: sync", "configured native disk", status);
  }
  if (strcmp(arguments[0], "poweroff") == 0 || strcmp(arguments[0], "reboot") == 0) {
    return power_command(shell, arguments, count);
  }
  if (strcmp(arguments[0], "session") == 0) {
    if (count < 2) {
      return shell_error(shell, "usage: session program [arguments...]\n");
    }
    return shell_launch(shell, arguments + 1, count - 1, SHELL_SESSION, NULL, 0, NULL);
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
      status = pyxis_working_bindings(shell->roots, shell->directory->root_count,
          namespace_handle);
      if (status != CALL_OK) {
        return shell_error(shell, "namespace: update working bindings failed (status %u)\n", status);
      }
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
  /* is_builtin leaves only service. */
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

enum command_result shell_command(struct shell *shell, char *line, char **arguments,
    struct shell_outcome *outcome)
{
  /* Syntax and command-form diagnostics below report a rejected line. */
  *outcome = (struct shell_outcome){TERMINAL_COMPLETION_REJECTED, 0};
  struct shell_command_line command;
  const char *error = parse_line(line, arguments, SHELL_LINE_CAPACITY, &command);
  if (error) {
    return shell_error(shell, "shell: %s\n", error);
  }
  if (!command.stage_count) {
    /* Only scripts reach this: interactive input skips blank lines. */
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN, 0};
    return COMMAND_OK;
  }
  if (command.stage_count > 1) {
    for (size_t i = 0; i < command.stage_count; ++i) {
      const struct shell_stage *stage = &command.stages[i];
      if (!stage->count || !stage->arguments[0][0]) {
        return shell_error(shell, "shell: Pipeline stage requires a command name\n");
      }
      if (is_builtin(stage->arguments, stage->count)) {
        return shell_error(shell, "shell: %s: Builtins are unsupported in pipelines\n",
            stage->arguments[0]);
      }
    }
    return shell_launch_pipeline(shell, &command, outcome);
  }

  const struct shell_stage *stage = &command.stages[0];
  arguments = stage->arguments;
  size_t count = stage->count;
  if (!arguments[0][0]) {
    return shell_error(shell, "shell: Empty command name\n");
  }
  bool builtin = is_builtin(arguments, count);
  if (stage->redirection_count && (builtin || command.background)) {
    return shell_error(shell, "shell: Redirection is only supported for foreground external commands\n");
  }
  if (command.background && builtin) {
    return shell_error(shell, "shell: & is only supported for external commands\n");
  }
  if (builtin) {
    /* session and service remain builtin transactions, including their launches. */
    enum command_result result = builtin_command(shell, arguments, count);
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN, result == COMMAND_FAILED};
    return result;
  }
  return shell_launch(shell, arguments, count,
      command.background ? SHELL_BACKGROUND : SHELL_FOREGROUND,
      stage->redirections, stage->redirection_count, outcome);
}
