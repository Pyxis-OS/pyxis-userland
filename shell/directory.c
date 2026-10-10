#include <pyxis/working_path.h>
#include <pyxis/environment.h>
#include "shell.h"
#include <abi/file.h>
#include <bundle.h>
#include <startup.h>
#include <handle.h>
#include <provider.h>
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
  if (status == CALL_OK && !shell->directory->count) {
    status = pyxis_working_change("tmp://");
  }
  return status;
}

void shell_directory_close(struct shell *shell)
{
  pyxis_working_clear();
  free(shell->workspace.directories);
  free(shell->workspace.component);
  free(shell->workspace.http);
}

enum call_status shell_change_directory(struct shell *shell, const char *path)
{
  (void)shell;
  return pyxis_working_change(path);
}

static enum call_status prepare_open_workspace(struct shell *shell, const char *path)
{
  enum call_status status = prepare_workspace(shell, strlen(path));
  if (status == CALL_OK && provider_http_uri(path) && !shell->workspace.http) {
    shell->workspace.http = malloc(sizeof(*shell->workspace.http));
    if (!shell->workspace.http) {
      return CALL_NO_MEMORY;
    }
  }
  return status;
}

static enum call_status open_path(struct shell *shell, const char *path, handle_t *image)
{
  enum call_status status = prepare_open_workspace(shell, path);
  if (status == CALL_OK) {
    status = path_resolve(shell->directory, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &shell->workspace, image);
  }
  return status;
}

/* A bare name is ROOT://NAME.pxe, for each root in lookup order. */
static enum call_status open_program(struct shell *shell, const char *root,
    const char *command, handle_t *image)
{
  static const char separator[] = "://", suffix[] = ".pxe";
  size_t root_length = strlen(root), length = strlen(command);
  if (length > SIZE_MAX - root_length - sizeof(separator) - sizeof(suffix)) {
    return CALL_LIMIT;
  }
  char *path = malloc(root_length + sizeof(separator) - 1 + length + sizeof(suffix));
  if (!path) {
    return CALL_NO_MEMORY;
  }
  memcpy(path, root, root_length);
  memcpy(path + root_length, separator, sizeof(separator) - 1);
  memcpy(path + root_length + sizeof(separator) - 1, command, length);
  memcpy(path + root_length + sizeof(separator) - 1 + length, suffix, sizeof(suffix));
  enum call_status status = open_path(shell, path, image);
  free(path);
  return status;
}

static enum call_status open_bundle_command(struct shell *shell, const char *command,
    handle_t *image, struct bundle_program **bundle)
{
  const char *catalog;
  enum call_status catalog_status = pyxis_environment_get("PYXIS_BUNDLE_CATALOG", &catalog);
  if (catalog_status != CALL_OK) {
    return catalog_status;
  }
  if (!catalog) {
    return CALL_NOT_FOUND;
  }
  if (!*catalog) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = bundle_command_open(shell->directory, catalog, command, bundle);
  if (status == CALL_OK) {
    *image = bundle_program_image(*bundle);
  }
  return status;
}

enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image,
    struct bundle_program **bundle)
{
  *image = HANDLE_INVALID;
  *bundle = NULL;
  if (!*command) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strlen(command);
  while (length && command[length - 1] == '/') {
    --length;
  }
  if (length > 4 && !memcmp(command + length - 4, ".pxb", 4)) {
    enum call_status status = bundle_open(shell->directory, command, bundle);
    if (status == CALL_OK) {
      *image = bundle_program_image(*bundle);
    }
    return status;
  }
  if (strchr(command, '/')) {
    enum call_status status = open_path(shell, command, image);
    if (status == CALL_NOT_FOUND && !strncmp(command, "bin://", 6) &&
        command[6] && !strchr(command + 6, '/') &&
        (length < 4 || memcmp(command + length - 4, ".pxe", 4))) {
      status = open_bundle_command(shell, command + 6, image, bundle);
    }
    return status;
  }
  /* Installed programs in bin:// come first; the archive's rescue set follows.
   * An unbound bin:// is NOT_FOUND, like a missing program. */
  enum call_status status = open_program(shell, "bin", command, image);
  if (status == CALL_NOT_FOUND) {
    status = open_bundle_command(shell, command, image, bundle);
  }
  if (status == CALL_NOT_FOUND) {
    status = open_program(shell, "boot", command, image);
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
