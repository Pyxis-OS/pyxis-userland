#include <pyxis/working_path.h>
#include "shell.h"
#include "syntax.h"
#include "values.h"
#include "../common/directory.h"
#include <mount.h>
#include <command_selection.h>
#include <environment.h>
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

enum command_result shell_resource_detail(struct shell *shell,
    const struct resource_description *description)
{
  if (!description->detail_size) {
    return COMMAND_FAILED;
  }
  if (!report_location(shell) || fputs("shell: provider: ", stderr) == EOF) {
    return COMMAND_FATAL;
  }
  for (size_t i = 0; i < description->detail_size; ++i) {
    unsigned char byte = description->detail[i];
    if (fputc(byte >= ' ' && byte <= '~' ? byte : '?', stderr) == EOF) {
      return COMMAND_FATAL;
    }
  }
  return fputc('\n', stderr) == EOF ? COMMAND_FATAL : COMMAND_FAILED;
}

static enum call_status root_available(struct shell *shell, const char *binding)
{
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(shell->environment, binding, &entry, NULL, 0);
  if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP) {
    handle_close(entry.handle);
  }
  if (status == CALL_OK || status == CALL_BUFFER_TOO_SMALL) {
    return CALL_ALREADY_EXISTS;
  }
  return status == CALL_NOT_FOUND ? CALL_OK : status;
}

/* Reserve shell-owned storage before acquisition. The shell runs one command
 * at a time and publishes the selected binding only after a complete open. */
enum call_status shell_reserve_root(struct shell *shell, const char *name,
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
  if (length > ENVIRONMENT_NAME_MAX) {
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

enum call_status shell_publish_root(struct shell *shell, char *name, handle_t root)
{
  struct handle_info info;
  enum call_status status = handle_query(root, &info);
  struct environment_set_entry entry = {0};
  if (status == CALL_OK && !environment_name(entry.name, name)) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK) {
    entry.kind = ENVIRONMENT_CAP;
    entry.flags = ENVIRONMENT_EXPORTED;
    entry.set_flags = ENVIRONMENT_SET_REQUIRE_ABSENT;
    entry.handle = root;
    entry.rights = info.rights;
    entry.transport = info.transport;
    status = environment_set_batch(shell->environment, &entry, 1);
  }
  if (handle_close(root) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  free(name);
  return status;
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
  enum call_status status = shell_reserve_root(shell, "host", 4, &name);
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
  status = shell_publish_root(shell, name, root);
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
  enum call_status status = shell_reserve_root(shell, destination, length - 3, &name);
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
  status = shell_publish_root(shell, name, root);
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

static enum command_result terminal_command(struct shell *shell, char **arguments, size_t count)
{
  const char *name = arguments[0];
  if (count != 1) {
    return shell_error(shell, "usage: %s\n", name);
  }
  struct handle_info info;
  enum call_status status = handle_query(shell->terminal.output, &info);
  if (status != CALL_OK || info.protocol != PROTOCOL_CONSOLE ||
      info.kind != HANDLE_KIND_NATIVE || info.transport ||
      !(info.rights & CONSOLE_RIGHT_WRITE)) {
    return shell_error(shell,
        "shell: %s: requires a writable CONSOLE grant as the named terminal output\n", name);
  }
  if (!strcmp(name, "reset")) {
    status = term_normalize(&shell->terminal);
  } else {
    status = term_fresh_line(&shell->terminal);
    if (status == CALL_OK) {
      status = term_print(&shell->terminal, "\x1b[2J\x1b[3J\x1b[H");
    }
  }
  return status == CALL_OK ? COMMAND_OK :
      shell_error(shell, "shell: %s: terminal output failed (status %u)\n", name, status);
}

bool shell_builtin(char **arguments, size_t count)
{
  const char *name = arguments[0];
  return shell_value_builtin(name) || !strcmp(name, "exit") || !strcmp(name, "cd") ||
      !strcmp(name, "mount") || !strcmp(name, "release") || !strcmp(name, "title") || !strcmp(name, "affinity") ||
      !strcmp(name, "session") || !strcmp(name, "which") ||
      !strcmp(name, "service") || !strcmp(name, "poweroff") || !strcmp(name, "reboot") ||
      !strcmp(name, "reset") || !strcmp(name, "clear") ||
      (!strcmp(name, "sync") && count > 1 &&
       (!strcmp(arguments[1], "--disk") || !strcmp(arguments[1], "--mount")));
}

static bool exit_status(const char *text, int *status)
{
  bool negative = *text == '-';
  if (negative) {
    ++text;
  }
  if (!*text) {
    return false;
  }
  uint32_t value = 0;
  uint32_t limit = negative ? (uint32_t)INT32_MAX + 1 : INT32_MAX;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || value > (limit - (*text - '0')) / 10) {
      return false;
    }
    value = value * 10 + (*text - '0');
  }
  *status = negative ? (int)(-(int64_t)value) : (int)value;
  return true;
}

static enum command_result builtin_command(struct shell *shell,
    const struct shell_expansion *expansion)
{
  const struct shell_stage *stage = &expansion->command.stages[0];
  char **arguments = stage->arguments;
  size_t count = stage->count;
  if (shell_value_builtin(arguments[0])) {
    return shell_values_command(shell, expansion);
  }
  if (strcmp(arguments[0], "exit") == 0) {
    int status = 0;
    if (count > 2 || (count == 2 && !exit_status(arguments[1], &status))) {
      return shell_error(shell, "usage: exit [signed-32-bit decimal status]\n");
    }
    shell->exit_status = status;
    shell->exit_requested = true;
    return COMMAND_EXIT;
  }
  if (strcmp(arguments[0], "which") == 0) {
    if (count != 2) return shell_error(shell, "usage: which NAME\n");
    struct command_selection *selection = NULL;
    enum call_status status = shell_open_image(shell, arguments[1], &selection);
    if (status == CALL_DENIED && shell->refusal[0]) {
      return shell_error(shell, "shell: which: %s: %s\n", arguments[1], shell->refusal);
    }
    if (status != CALL_OK) return shell_directory_error(shell, "shell: which", arguments[1], status);
    int written;
    if (selection->explicit_path) {
      written = printf("%s: explicit %s\n", arguments[1], selection->explicit_path);
    } else if (selection->package) {
      written = printf("%s: entry %zu [%s] %s@%s set %llu %s\n", arguments[1],
          selection->ordinal + 1, selection->label, selection->target.id,
          selection->target.version, (unsigned long long)selection->origin.set,
          selection->target.path);
    } else {
      written = printf("%s: entry %zu [%s] %s\n", arguments[1],
          selection->ordinal + 1, selection->label, selection->path);
    }
    command_selection_close(selection);
    return written < 0 ? COMMAND_FATAL : COMMAND_OK;
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
    if (count > 1 && !strcmp(arguments[1], "--fat32")) {
      return shell_mount_fat32(shell, arguments, count);
    }
    return mount_volume(shell, arguments, count);
  }
  if (strcmp(arguments[0], "release") == 0) {
    return shell_release_fat32(shell, arguments, count);
  }
  if (strcmp(arguments[0], "sync") == 0) {
    if (count > 1 && !strcmp(arguments[1], "--mount")) {
      return shell_sync_fat32(shell, arguments, count);
    }
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
  if (strcmp(arguments[0], "reset") == 0 || strcmp(arguments[0], "clear") == 0) {
    return terminal_command(shell, arguments, count);
  }
  if (strcmp(arguments[0], "session") == 0) {
    if (count < 2) {
      return shell_error(shell, "usage: session program [arguments...]\n");
    }
    struct shell_stage selected = *stage;
    selected.arguments = arguments + 1;
    selected.count = count - 1;
    return shell_launch_selected(shell, &selected, SHELL_SESSION, NULL);
  }
  /* is_builtin leaves only service. */
  bool replace = count > 1 && !strcmp(arguments[1], "replace");
  bool optional = false;
  bool read_only = false;
  struct shell_service_root roots[PROVIDER_SETUP_ROOT_MAX - 1] = {0};
  size_t root_count = 0;
  size_t index = 2;
  while (index < count && arguments[index][0] == '-') {
    if (!strcmp(arguments[index], "--optional") && !replace && !optional) {
      optional = true;
    } else if (!strcmp(arguments[index], "--read-only") && !read_only) {
      read_only = true;
    } else if ((!strcmp(arguments[index], "--root") ||
        !strcmp(arguments[index], "--optional-root") ||
        !strcmp(arguments[index], "--read-only-root") ||
        !strcmp(arguments[index], "--optional-read-only-root")) && !replace &&
        index + 1 < count && root_count < PROVIDER_SETUP_ROOT_MAX - 1) {
      const char *binding = arguments[index + 1];
      const char *equal = strchr(binding, '=');
      size_t length = equal ? (size_t)(equal - binding) : 0;
      if (!length || length > PROVIDER_SETUP_ROLE_MAX || !equal[1] ||
          !environment_name(roots[root_count].name, equal + 1)) {
        break;
      }
      memcpy(roots[root_count].role, binding, length);
      roots[root_count].optional = !strcmp(arguments[index], "--optional-root") ||
          !strcmp(arguments[index], "--optional-read-only-root");
      bool root_read_only = !strcmp(arguments[index], "--read-only-root") ||
          !strcmp(arguments[index], "--optional-read-only-root");
      roots[root_count].rights = root_read_only ? PROVIDER_RIGHT_OPEN_READ : UINT64_MAX;
      roots[root_count].transport = HANDLE_TRANSPORT_CALL;
      ++root_count;
      ++index;
    } else {
      break;
    }
    ++index;
  }
  if (count < index + 2 || (strcmp(arguments[1], "start") && !replace) ||
      arguments[index][0] == '-') {
    return shell_error(shell,
        "usage: service start [--optional] [--read-only] [ROOTS...] NAME IMAGE [arguments...]\n"
        "       ROOTS: --root ROLE=NAME | --optional-root ROLE=NAME | --read-only-root ROLE=NAME | --optional-read-only-root ROLE=NAME\n"
        "       service replace [--read-only] NAME IMAGE [arguments...]\n");
  }
  return shell_launch_service(shell, arguments[index], replace, optional, read_only,
      arguments + index + 1, count - index - 1, stage->operands, roots, root_count);
}

static enum command_result expanded_command(struct shell *shell,
    const struct shell_expansion *expansion, bool assignment, struct shell_outcome *outcome)
{
  const struct shell_command_line *command = &expansion->command;
  if (assignment) {
    enum command_result result = shell_assign(shell, &expansion->words[0]);
    *outcome = expansion->words[0].capture &&
        (expansion->words[0].capture->outcome.kind == TERMINAL_COMPLETION_TERMINATED ||
         expansion->words[0].capture->outcome.kind == TERMINAL_COMPLETION_FAULTED) ?
        expansion->words[0].capture->outcome :
        (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN, result == COMMAND_FAILED, false};
    return result;
  }
  if (!command->stage_count) {
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN, 0, false};
    return COMMAND_OK;
  }
  for (size_t i = 0; i < command->stage_count; ++i) {
    const struct shell_stage *stage = &command->stages[i];
    if (!stage->count || !stage->arguments[0][0]) {
      return shell_error(shell, "shell: Pipeline stage requires a command name\n");
    }
    if (stage->redirection_count && command->background) {
      return shell_error(shell, "shell: Redirection is only supported for foreground external commands\n");
    }
    if (command->stage_count > 1 && shell_builtin(stage->arguments, stage->count)) {
      return shell_error(shell, "shell: %s: Builtins are unsupported in pipelines\n",
          stage->arguments[0]);
    }
  }
  const struct shell_stage *stage = &command->stages[0];
  bool builtin = shell_builtin(stage->arguments, stage->count);
  if (stage->redirection_count && builtin) {
    return shell_error(shell, "shell: Redirection is only supported for foreground external commands\n");
  }
  if (command->background && builtin) {
    return shell_error(shell, "shell: & is only supported for external commands\n");
  }
  if (builtin) {
    enum command_result result = builtin_command(shell, expansion);
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN,
        result == COMMAND_FAILED, false};
    return result;
  }
  return shell_launch_pipeline(shell, command, outcome);
}

enum command_result shell_command(struct shell *shell, char *line,
    struct shell_outcome *outcome)
{
  *outcome = (struct shell_outcome){TERMINAL_COMPLETION_REJECTED, 0, false};
  struct shell_syntax syntax = {0};
  const char *error = shell_syntax_parse(line, &syntax);
  if (error) {
    return shell_error(shell, "shell: %s\n", error);
  }
  enum command_result result = COMMAND_OK;
  if (!syntax.count) {
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_BUILTIN, 0, false};
  }
  bool faulted = false;
  for (size_t i = 0; i < syntax.count; ++i) {
    enum shell_condition condition = syntax.commands[i].condition;
    if (condition == SHELL_ALWAYS) {
      if (i && shell->script_name && result == COMMAND_FAILED) {
        break;
      }
      faulted = false;
    } else if (faulted || !shell_condition_selected(condition, result)) {
      continue;
    }
    struct shell_expansion expansion = {0};
    enum call_status status;
    *outcome = (struct shell_outcome){TERMINAL_COMPLETION_REJECTED, 0, false};
    error = shell_syntax_expand(shell, &syntax.commands[i], &expansion, &status);
    if (error) {
      result = status == CALL_OK ? shell_error(shell, "shell: %s\n", error) :
          shell_error(shell, "shell: %s (status %u)\n", error, status);
      if (status == CALL_OUTCOME_UNKNOWN) {
        result = COMMAND_FATAL;
      } else if (expansion.failure_outcome.kind == TERMINAL_COMPLETION_TERMINATED ||
          expansion.failure_outcome.kind == TERMINAL_COMPLETION_FAULTED) {
        *outcome = expansion.failure_outcome;
      } else if (status == CALL_ABANDONED) {
        *outcome = (struct shell_outcome){TERMINAL_COMPLETION_TERMINATED, 0, false};
      }
    } else {
      result = expanded_command(shell, &expansion, syntax.commands[i].assignment, outcome);
      if (!shell_expansion_close(&expansion)) {
        shell_directory_error(shell, "shell: cleanup", "expanded values", CALL_OUTCOME_UNKNOWN);
        result = COMMAND_FATAL;
      }
    }
    if (result == COMMAND_FATAL || result == COMMAND_EXIT ||
        outcome->kind == TERMINAL_COMPLETION_TERMINATED || outcome->cancelled) {
      break;
    }
    faulted = outcome->kind == TERMINAL_COMPLETION_FAULTED;
  }
  shell_syntax_close(&syntax);
  return result;
}
