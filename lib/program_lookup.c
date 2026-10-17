#include <program.h>
#include <file.h>
#include <pyxis/environment.h>
#include <stdlib.h>
#include <string.h>

static bool suffix(const char *path, size_t length, const char *ending)
{
  return length >= 4 && !memcmp(path + length - 4, ending, 4);
}

static enum call_status open_bundle(const struct path_context *context,
    const char *path, handle_t *image, struct bundle_program **bundle)
{
  enum call_status status = bundle_open(context, path, bundle);
  if (status == CALL_OK) {
    *image = bundle_program_image(*bundle);
  }
  return status;
}

static enum call_status open_catalog(const struct path_context *context,
    const char *command, handle_t *image, struct bundle_program **bundle)
{
  char *catalog;
  enum call_status status = pyxis_environment_copy("PYXIS_BUNDLE_CATALOG", &catalog);
  if (status != CALL_OK || !catalog) {
    return status == CALL_OK ? CALL_NOT_FOUND : status;
  }
  status = bundle_command_open(context, catalog, command, bundle);
  free(catalog);
  if (status == CALL_OK) {
    *image = bundle_program_image(*bundle);
  }
  return status;
}

static void candidate(char *path, const char *root, const char *name, size_t length,
    const char *ending)
{
  size_t root_length = strlen(root);
  memcpy(path, root, root_length);
  memcpy(path + root_length, name, length);
  memcpy(path + root_length + length, ending, 5);
}

/* User programs come last: on installed systems boot:// holds system programs
 * such as ls and cp, not only a rescue fallback, so home://bin must not shadow
 * them. A missing home root or bin directory is an absent candidate. */
static enum call_status open_name(const struct path_context *context,
    const char *name, bool bare, struct path_workspace *workspace, handle_t *image,
    struct bundle_program **bundle)
{
  size_t length = strlen(name);
  if (length > SIZE_MAX - PROGRAM_LOOKUP_EXTRA) {
    return CALL_LIMIT;
  }
  char *path = malloc(length + PROGRAM_LOOKUP_EXTRA);
  if (!path) {
    return CALL_NO_MEMORY;
  }
  candidate(path, "bin://", name, length, ".pxe");
  enum call_status status = path_resolve(context, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, workspace, image);
  if (status == CALL_NOT_FOUND) {
    candidate(path, "bin://", name, length, ".pxb");
    status = open_bundle(context, path, image, bundle);
  }
  if (status == CALL_NOT_FOUND) {
    status = open_catalog(context, name, image, bundle);
  }
  if (status == CALL_NOT_FOUND && bare) {
    candidate(path, "boot://", name, length, ".pxe");
    status = path_resolve(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, image);
  }
  if (status == CALL_NOT_FOUND && bare) {
    candidate(path, "home://bin/", name, length, ".pxe");
    status = path_resolve(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, image);
  }
  if (status == CALL_NOT_FOUND && bare) {
    candidate(path, "home://bin/", name, length, ".pxb");
    status = open_bundle(context, path, image, bundle);
  }
  free(path);
  return status;
}

enum call_status program_open(const struct path_context *context, const char *command,
    struct path_workspace *workspace, handle_t *image, struct bundle_program **bundle)
{
  if (image) {
    *image = HANDLE_INVALID;
  }
  if (bundle) {
    *bundle = NULL;
  }
  if (!context || !command || !*command || !workspace || !image || !bundle) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strlen(command), end = length;
  while (end && command[end - 1] == '/') {
    --end;
  }
  if (suffix(command, end, ".pxb")) {
    return open_bundle(context, command, image, bundle);
  }
  if (!strchr(command, '/')) {
    return open_name(context, command, true, workspace, image, bundle);
  }
  enum call_status status = path_resolve(context, command, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, workspace, image);
  if (status == CALL_NOT_FOUND && !strncmp(command, "bin://", 6) && command[6] &&
      !strchr(command + 6, '/') && !suffix(command, length, ".pxe")) {
    status = open_name(context, command + 6, false, workspace, image, bundle);
  }
  return status;
}
