#include <pyxis/working_path.h>
#include <pyxis/environment.h>
#include "shell.h"
#include <abi/file.h>
#include <program.h>
#include <resource.h>
#include <startup.h>
#include <stdio.h>
#include <handle.h>
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
  char refusal[COMMAND_REFUSAL_MAX];
  status = program_open_reason(shell->directory, command, &shell->workspace, selection, refusal);
  /* A refused package command says why; the caller still reports the status. */
  if (status == CALL_DENIED && refusal[0]) {
    fprintf(stderr, "shell: %s: %s\n", command, refusal);
  }
  return status;
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
