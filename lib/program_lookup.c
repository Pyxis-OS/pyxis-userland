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
  const char *catalog;
  enum call_status status = pyxis_environment_get("PYXIS_BUNDLE_CATALOG", &catalog);
  if (status != CALL_OK || !catalog) {
    return status == CALL_OK ? CALL_NOT_FOUND : status;
  }
  status = bundle_command_open(context, catalog, command, bundle);
  if (status == CALL_OK) {
    *image = bundle_program_image(*bundle);
  }
  return status;
}

static enum call_status open_name(const struct path_context *context,
    const char *name, bool rescue, struct path_workspace *workspace, handle_t *image,
    struct bundle_program **bundle)
{
  size_t length = strlen(name);
  if (length > SIZE_MAX - sizeof("boot://.pxe")) {
    return CALL_LIMIT;
  }
  char *path = malloc(length + sizeof("boot://.pxe"));
  if (!path) {
    return CALL_NO_MEMORY;
  }
  memcpy(path, "bin://", 6);
  memcpy(path + 6, name, length);
  memcpy(path + 6 + length, ".pxe", 5);
  enum call_status status = path_resolve(context, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, workspace, image);
  if (status == CALL_NOT_FOUND) {
    memcpy(path + 6 + length, ".pxb", 5);
    status = open_bundle(context, path, image, bundle);
  }
  if (status == CALL_NOT_FOUND) {
    status = open_catalog(context, name, image, bundle);
  }
  if (status == CALL_NOT_FOUND && rescue) {
    memcpy(path, "boot://", 7);
    memcpy(path + 7, name, length);
    memcpy(path + 7 + length, ".pxe", 5);
    status = path_resolve(context, path, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        workspace, image);
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
