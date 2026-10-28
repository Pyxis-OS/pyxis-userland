#include <pyxis/working_path.h>
#include <pyxis/environment.h>
#include "shell.h"
#include <abi/file.h>
#include <program.h>
#include <resource.h>
#include <startup.h>
#include <handle.h>
#include <file.h>
#include <stdlib.h>
#include <string.h>

static enum call_status prepare_workspace(struct shell *shell, size_t length)
{
  size_t depth = shell->directory->count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  size_t slots = depth + length + 1;
  if (slots > shell->workspace.directory_capacity) {
    handle_t *larger = realloc(shell->workspace.directories, slots * sizeof(*larger));
    if (!larger) {
      return CALL_NO_MEMORY;
    }
    shell->workspace.directories = larger;
    shell->workspace.directory_capacity = slots;
  }
  if (length + 1 > shell->workspace.component_capacity) {
    char *larger = realloc(shell->workspace.component, length + 1);
    if (!larger) {
      return CALL_NO_MEMORY;
    }
    shell->workspace.component = larger;
    shell->workspace.component_capacity = length + 1;
  }
  return CALL_OK;
}

enum call_status shell_directory_init(struct shell *shell)
{
  enum call_status status = pyxis_working_context(&shell->directory);
  size_t initial_count = startup_working_directory_count();
  if (status == CALL_OK && initial_count) {
    if (initial_count > SIZE_MAX / sizeof(*shell->initial_directories)) {
      return CALL_LIMIT;
    }
    shell->initial_directories = malloc(initial_count * sizeof(*shell->initial_directories));
    if (!shell->initial_directories) {
      return CALL_NO_MEMORY;
    }
    memcpy(shell->initial_directories, startup_working_directories(),
        initial_count * sizeof(*shell->initial_directories));
    for (size_t i = 0; i < initial_count; ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (shell->initial_directories[i] == shell->initial_directories[j]) {
          shell->initial_directories[i] = HANDLE_INVALID;
          break;
        }
      }
    }
    shell->initial_directory_count = initial_count;
  }
  if (status == CALL_OK && !shell->directory->count) {
    status = pyxis_working_change("tmp://");
  }
  return status;
}

void shell_directory_close(struct shell *shell)
{
  pyxis_working_clear();
  if (shell->history_home != HANDLE_INVALID) {
    handle_close(shell->history_home);
    shell->history_home = HANDLE_INVALID;
  }
  for (size_t i = 0; i < shell->initial_directory_count; ++i) {
    if (shell->initial_directories[i] != HANDLE_INVALID) {
      handle_close(shell->initial_directories[i]);
    }
  }
  free(shell->initial_directories);
  shell->initial_directories = NULL;
  shell->initial_directory_count = 0;
  free(shell->workspace.directories);
  free(shell->workspace.component);
}

enum call_status shell_change_directory(struct shell *shell, const char *path)
{
  (void)shell;
  return pyxis_working_change(path);
}

static enum call_status prepare_open_workspace(struct shell *shell, const char *path)
{
  return prepare_workspace(shell, strlen(path));
}

enum call_status shell_open_image(struct shell *shell, const char *command,
    struct command_selection **selection)
{
  *selection = NULL;
  shell->refusal[0] = 0;
  size_t length = strlen(command);
  if (length > SIZE_MAX - PROGRAM_LOOKUP_EXTRA) {
    return CALL_LIMIT;
  }
  enum call_status status = prepare_workspace(shell, length + PROGRAM_LOOKUP_EXTRA - 1);
  if (status == CALL_OK) {
    status = prepare_open_workspace(shell, command);
  }
  if (status != CALL_OK) {
    return status;
  }
  return program_open_reason(shell->directory, command, &shell->workspace, selection,
      shell->refusal);
}

/* Open only: truncation waits until every redirect has a retained file grant. */
enum call_status shell_open_redirect(struct shell *shell, const char *path,
    bool input, handle_t *file)
{
  *file = HANDLE_INVALID;
  enum call_status status = prepare_open_workspace(shell, path);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t rights = input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE;
  return path_open_file(shell->directory, path, rights, !input,
      &shell->workspace, file);
}

enum call_status shell_open_redirect_value(struct shell *shell,
    const struct shell_redirection *redirect, struct resource_result *result)
{
  *result = (struct resource_result){0};
  bool input = redirect->stream == ENVIRONMENT_STDIN;
  uint64_t intent = input ? PROVIDER_INTENT_READ : PROVIDER_INTENT_WRITE;
  uint64_t flags = redirect->append ? PROVIDER_OPEN_APPEND : 0;
  if (redirect->root != HANDLE_INVALID) {
    struct handle_info info;
    enum call_status status = handle_query(redirect->root, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (!input && (info.protocol == PROTOCOL_PROVIDER || info.protocol == PROTOCOL_DIRECTORY)) {
      flags |= PROVIDER_OPEN_CREATE;
    }
    return resource_open_held(redirect->root, redirect->remainder, intent, flags, 0, result);
  }
  if (redirect->value) {
    if (redirect->value->kind != ENVIRONMENT_CAP) {
      return CALL_BAD_REQUEST;
    }
    struct handle_info info;
    enum call_status status = handle_query(redirect->value->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (!input && info.protocol == PROTOCOL_PROVIDER) {
      flags |= PROVIDER_OPEN_CREATE;
    }
    return resource_open_held(redirect->value->handle, "", intent, flags, 0, result);
  }
  if (!input) {
    flags |= PROVIDER_OPEN_CREATE;
  }
  return resource_open_path(shell->directory, redirect->path, intent, flags, 0, result);
}

static enum call_status classify_output(struct shell_redirection *redirect,
    struct shell_redirect_target *target)
{
  struct describe_reply info;
  enum call_status status = handle_describe(target->file, 0, &info);
  if (status != CALL_OK) {
    return status;
  }
  /* Retain a declared failure for the normal redirect result/capture handoff.
   * No helper will launch; publication reports this same held resource. */
  if (!redirect->append && info.description.outcome != RESOURCE_OK) {
    return CALL_OK;
  }
  uint64_t offset;
  size_t written;
  status = file_append(target->file, NULL, 0, &offset, &written);
  if (status == CALL_BAD_OPERATION && !redirect->append) {
    redirect->positioned_output = true;
    return CALL_OK;
  }
  return status;
}

/* Retain the full cwd chain for ordered .. traversal. Explicit roots instead
 * start a confined chain; neither route rediscovers a captured environment name. */
enum call_status shell_prepare_output_redirect(struct shell *shell,
    struct shell_redirection *redirect, struct shell_redirect_target *target)
{
  const char *path = redirect->remainder;
  handle_t root = redirect->root;
  handle_t captured = HANDLE_INVALID;
  enum call_status status = CALL_OK;
  if (redirect->value) {
    if (redirect->value->kind != ENVIRONMENT_CAP) {
      return CALL_BAD_REQUEST;
    }
    root = redirect->value->handle;
    path = "";
  } else if (root == HANDLE_INVALID) {
    path = redirect->path;
    if (!path || !*path || *path == '/') {
      return CALL_BAD_REQUEST;
    }
    const char *end = path;
    while (*end && *end != '/' && *end != ':') {
      ++end;
    }
    if (*end == ':' && end[1] == '/' && end[2] == '/') {
      size_t length = end - path;
      if (!length || length > ENVIRONMENT_NAME_MAX) {
        return CALL_BAD_REQUEST;
      }
      char name[ENVIRONMENT_NAME_MAX + 1];
      memcpy(name, path, length);
      name[length] = 0;
      status = environment_lookup_file(shell->environment, name, &captured);
      if (status != CALL_OK) {
        return status;
      }
      root = captured;
      path = end + 3;
    }
  }
  if (root != HANDLE_INVALID) {
    struct handle_info info;
    status = handle_query(root, &info);
    if (status != CALL_OK) {
      goto done;
    }
    if (info.protocol == PROTOCOL_FILE) {
      struct resource_result held = {0};
      status = resource_open_held(root, path, PROVIDER_INTENT_WRITE, 0, 0, &held);
      if (status == CALL_OK) {
        target->file = held.resource;
        held.resource = HANDLE_INVALID;
        status = classify_output(redirect, target);
      }
      enum call_status closed = resource_result_close(&held);
      if (status == CALL_OK) {
        status = closed;
      }
      goto done;
    }
    if (info.protocol != PROTOCOL_DIRECTORY || info.kind != HANDLE_KIND_NATIVE ||
        info.transport) {
      status = CALL_BAD_OPERATION;
      goto done;
    }
  }
  size_t initial_count = root != HANDLE_INVALID ? 1 : shell->directory->count;
  size_t length = strlen(path);
  if (!initial_count) {
    status = CALL_UNAVAILABLE;
    goto done;
  }
  if (initial_count > SIZE_MAX - length ||
      initial_count + length > SIZE_MAX / sizeof(handle_t)) {
    status = CALL_LIMIT;
    goto done;
  }
  handle_t *chain = calloc(initial_count + length, sizeof(*chain));
  char *component = malloc(length + 1);
  if (!chain || !component) {
    free(component);
    free(chain);
    status = CALL_NO_MEMORY;
    goto done;
  }
  size_t count = 0;
  while (count < initial_count) {
    status = handle_copy(root != HANDLE_INVALID ? root : shell->directory->directories[count],
        &chain[count]);
    if (status != CALL_OK) {
      goto unwind;
    }
    ++count;
  }
  for (;;) {
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      status = CALL_WRONG_TYPE;
      break;
    }
    const char *start = path;
    while (*path && *path != '/') {
      ++path;
    }
    size_t size = path - start;
    bool separator = *path != 0;
    while (*path == '/') {
      ++path;
    }
    if (size == 1 && start[0] == '.') {
      continue;
    }
    if (size == 2 && start[0] == '.' && start[1] == '.') {
      if (count == 1) {
        status = CALL_DENIED;
        break;
      }
      if (handle_close(chain[--count])) {
        status = CALL_BAD_HANDLE;
        break;
      }
      continue;
    }
    memcpy(component, start, size);
    component[size] = 0;
    handle_t parent = chain[count - 1];
    struct handle_info info;
    status = handle_query(parent, &info);
    if (status != CALL_OK) {
      break;
    }
    if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_DIRECTORY ||
        info.transport) {
      status = CALL_BAD_OPERATION;
      break;
    }
    if (*path) {
      status = directory_lookup(parent, component, DIRECTORY_KIND_DIRECTORY,
          info.rights & DIRECTORY_RIGHTS, &chain[count]);
      if (status != CALL_OK) {
        break;
      }
      ++count;
      continue;
    }
    if (separator) {
      status = CALL_WRONG_TYPE;
      break;
    }
    status = directory_lookup(parent, component, DIRECTORY_KIND_FILE,
        FILE_RIGHT_WRITE, &target->file);
    if (status == CALL_OK) {
      status = classify_output(redirect, target);
    } else if (status == CALL_NOT_FOUND) {
      status = directory_append_support(parent);
      if (status == CALL_BAD_OPERATION && !redirect->append) {
        redirect->positioned_output = true;
        status = CALL_OK;
      }
      if (status == CALL_OK) {
        target->parent = chain[--count];
        target->component = component;
        component = NULL;
      }
    }
    break;
  }
unwind:
  while (count) {
    if (handle_close(chain[--count])) {
      status = CALL_BAD_HANDLE;
    }
  }
  free(component);
  free(chain);
done:
  if (captured != HANDLE_INVALID && handle_close(captured)) {
    status = CALL_BAD_HANDLE;
  }
  return status;
}

static bool same_file_identity(const struct file_info_reply *first,
    const struct file_info_reply *second)
{
  uint64_t identity = FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID;
  return (first->valid & identity) == identity && (second->valid & identity) == identity &&
      first->domain == second->domain && first->object == second->object;
}

enum call_status shell_lookup_created_redirect(struct shell_redirect_target *target,
    const struct shell_redirect_target *earlier, handle_t earlier_file, bool *matched)
{
  *matched = false;
  if (!earlier->created || strcmp(target->component, earlier->component)) {
    return CALL_OK;
  }
  struct file_info_reply parent, earlier_parent;
  enum call_status status = directory_info(target->parent, &parent);
  if (status != CALL_OK) {
    return status;
  }
  status = directory_info(earlier->parent, &earlier_parent);
  if (status != CALL_OK) {
    return status;
  }
  if (!same_file_identity(&parent, &earlier_parent)) {
    return CALL_OK;
  }
  *matched = true;
  /* Obtain this redirect's own grant and lineage. A replacement or removal is
   * final; neither the earlier FILE's authority nor CREATE is substituted. */
  handle_t file = HANDLE_INVALID;
  status = directory_lookup(target->parent, target->component,
      DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, &file);
  if (status != CALL_OK) {
    return status;
  }
  struct file_info_reply current, created;
  status = file_info(file, &current);
  if (status == CALL_OK) {
    status = file_info(earlier_file, &created);
  }
  if (status == CALL_OK && !same_file_identity(&current, &created)) {
    status = CALL_ALREADY_EXISTS;
  }
  if (status == CALL_OK) {
    target->file = file;
  } else if (handle_close(file)) {
    status = CALL_BAD_HANDLE;
  }
  return status;
}

enum call_status shell_publish_output_redirect(const struct shell_redirection *redirect,
    struct shell_redirect_target *target, struct resource_result *result)
{
  *result = (struct resource_result){0};
  if (target->file == HANDLE_INVALID) {
    uint64_t requirements = redirect->positioned_output ? 0 : DIRECTORY_CREATE_REQUIRE_APPEND;
    enum call_status status = directory_create_required(target->parent, target->component,
        DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, requirements, &target->file);
    if (status != CALL_OK) {
      return status;
    }
    target->created = true;
  }
  enum call_status status = resource_result_adopt(target->file, result);
  if (status == CALL_OK) {
    target->file = HANDLE_INVALID;
  }
  return status;
}

enum call_status shell_open_directory(struct shell *shell, const char *path, uint64_t rights,
    handle_t *directory)
{
  enum call_status status = prepare_workspace(shell, strlen(path));
  if (status == CALL_OK) {
    status = path_resolve(shell->directory, path, DIRECTORY_KIND_DIRECTORY, rights,
        &shell->workspace, directory);
  }
  return status;
}

enum call_status shell_open_native_directory(struct shell *shell, const char *path,
    uint64_t required, handle_t *directory)
{
  *directory = HANDLE_INVALID;
  enum call_status status = prepare_workspace(shell, strlen(path));
  if (status == CALL_OK) {
    status = path_open_directory_native(shell->directory, path, required,
        &shell->workspace, directory);
  }
  return status;
}
