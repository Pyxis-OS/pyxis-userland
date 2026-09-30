#include "shell.h"
#include <abi/file.h>
#include <startup.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

static size_t root_prefix(const char *path)
{
  const char *end = path;
  while (*end && *end != ':' && *end != '/') {
    ++end;
  }
  if (end != path && *end == ':' && end[1] == '/' && end[2] == '/') {
    return end - path + 3;
  }
  return 0;
}

/* Normalize only the display spelling. path_change still walks the original
 * input, so missing/.. cannot bypass a failed lookup or a capability boundary. */
static enum call_status display_path(const char *current, const char *path, char **result)
{
  size_t root = root_prefix(path);
  const char *base = root ? path : current;
  size_t length = root ? root : strlen(base);
  const char *tail = root ? path + root : path;
  size_t extra = strlen(tail);
  if (extra > SIZE_MAX - 2 || length > SIZE_MAX - extra - 2) {
    return CALL_LIMIT;
  }
  char *display = malloc(length + extra + 2);
  if (!display) {
    return CALL_NO_MEMORY;
  }
  memcpy(display, base, length);
  root = root_prefix(base);

  while (*tail) {
    if (*tail == '/') {
      ++tail;
      continue;
    }
    const char *start = tail;
    while (*tail && *tail != '/') {
      ++tail;
    }
    size_t component = tail - start;
    if (component == 1 && start[0] == '.') {
      continue;
    }
    if (component == 2 && start[0] == '.' && start[1] == '.') {
      while (length > root && display[length - 1] != '/') {
        --length;
      }
      if (length > root) {
        --length;
      }
    } else {
      if (length > root) {
        display[length++] = '/';
      }
      memcpy(display + length, start, component);
      length += component;
    }
  }
  display[length] = '\0';
  *result = display;
  return CALL_OK;
}

static enum call_status prepare_workspace(struct shell *shell, size_t length)
{
  size_t depth = shell->directory.count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  size_t slots = depth + length + 1;
  if (slots > shell->directory.capacity) {
    handle_t *larger = realloc(shell->directory.directories, slots * sizeof(*larger));
    if (!larger) {
      return CALL_NO_MEMORY;
    }
    shell->directory.directories = larger;
    shell->directory.capacity = slots;
  }
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
  const char *path = startup_working_path();
  size_t depth = startup_working_directory_count();
  if (!depth) {
    return shell_change_directory(shell, "home://");
  }
  /* Display spelling is separate from authority in the supplied handles. */
  if (!path || !root_prefix(path)) {
    return CALL_BAD_REQUEST;
  }
  if (depth > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  handle_t *storage = malloc(depth * sizeof(*storage));
  if (!storage) {
    return CALL_NO_MEMORY;
  }
  char *display;
  enum call_status status = display_path(NULL, path, &display);
  if (status != CALL_OK) {
    free(storage);
    return status;
  }
  status = path_context_init(&shell->directory, storage, depth,
      startup_working_directories(), depth);
  if (status != CALL_OK) {
    free(display);
    free(storage);
  } else {
    shell->working_path = display;
  }
  return status;
}

void shell_directory_close(struct shell *shell)
{
  handle_t *storage = shell->directory.directories;
  path_context_close(&shell->directory);
  free(storage);
  free(shell->workspace.directories);
  free(shell->workspace.component);
  free(shell->working_path);
}

enum call_status shell_change_directory(struct shell *shell, const char *path)
{
  enum call_status status = prepare_workspace(shell, strlen(path));
  if (status != CALL_OK) {
    return status;
  }
  char *display;
  status = display_path(shell->working_path, path, &display);
  if (status != CALL_OK) {
    return status;
  }
  status = path_change(&shell->directory, path, &shell->workspace);
  if (status != CALL_OK) {
    free(display);
  } else {
    free(shell->working_path);
    shell->working_path = display;
  }
  return status;
}

enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image)
{
  *image = HANDLE_INVALID;
  if (!*command) {
    return CALL_BAD_REQUEST;
  }
  char *allocated = NULL;
  const char *path = command;
  if (!strchr(command, '/')) {
    size_t length = strlen(command);
    if (length > SIZE_MAX - sizeof("app://.pxe")) {
      return CALL_LIMIT;
    }
    allocated = malloc(length + sizeof("app://.pxe"));
    if (!allocated) {
      return CALL_NO_MEMORY;
    }
    memcpy(allocated, "app://", 6);
    memcpy(allocated + 6, command, length);
    memcpy(allocated + 6 + length, ".pxe", 5);
    path = allocated;
  }
  enum call_status status = prepare_workspace(shell, strlen(path));
  if (status == CALL_OK) {
    status = path_resolve(&shell->directory, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &shell->workspace, image);
  }
  free(allocated);
  return status;
}

/* Open only: truncation waits until every redirect has a retained file grant. */
enum call_status shell_open_redirect(struct shell *shell, const char *path,
    bool input, handle_t *file)
{
  *file = HANDLE_INVALID;
  size_t length = strlen(path);
  enum call_status status = prepare_workspace(shell, length);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t rights = input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE;
  return path_open_file(&shell->directory, path, rights, !input,
      &shell->workspace, file);
}
