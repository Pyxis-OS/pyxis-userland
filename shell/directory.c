#include "shell.h"
#include <abi/file.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>

static uint64_t path_rights(const char *path, uint64_t current)
{
  if (strncmp(path, "app://", 6) == 0) {
    return APP_DIRECTORY_RIGHTS;
  }
  if (strncmp(path, "home://", 7) == 0) {
    return HOME_DIRECTORY_RIGHTS;
  }
  return current;
}

/* Normalize only the display spelling. path_change still walks the original
 * input, so missing/.. cannot bypass a failed lookup or a capability boundary. */
static enum call_status display_path(const char *current, const char *path, char **result)
{
  size_t root = strncmp(path, "home://", 7) == 0 ? 7 :
                strncmp(path, "app://", 6) == 0 ? 6 : 0;
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
  root = strncmp(base, "home://", 7) == 0 ? 7 : 6;

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
    shell->directory.directory_rights = APP_DIRECTORY_RIGHTS;
    return shell_change_directory(shell, "home://");
  }
  /* The initial shell contract names these two namespaces. The display prefix
   * selects requested rights, never a replacement for the supplied handles. */
  if (!path || (strncmp(path, "app://", 6) && strncmp(path, "home://", 7))) {
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
      startup_working_directories(), depth, path_rights(path, 0));
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
  uint64_t previous_rights = shell->directory.directory_rights;
  shell->directory.directory_rights = path_rights(path, previous_rights);
  status = path_change(&shell->directory, path, &shell->workspace);
  if (status != CALL_OK) {
    shell->directory.directory_rights = previous_rights;
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
