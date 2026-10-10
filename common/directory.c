#include "directory.h"
#include <path.h>
#include <provider.h>
#include <pyxis/working_path.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum call_status resolve_path(const char *path, uint64_t kind, uint64_t rights,
    handle_t *handle)
{
  *handle = HANDLE_INVALID;
  size_t length = strlen(path);
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    return status;
  }
  size_t depth = context->count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }

  /* One slot per path byte is enough even for single-character components. */
  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct provider_http_workspace *http = NULL;
  if (kind == DIRECTORY_KIND_FILE && provider_http_uri(path)) {
    http = malloc(sizeof(*http));
    if (!http) {
      free(component);
      free(directories);
      return CALL_NO_MEMORY;
    }
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1, .http = http,
  };
  /* Resolution borrows libc's retained chain. */
  status = path_resolve(context, path, kind, rights, &workspace, handle);
  free(http);
  free(component);
  free(directories);
  return status;
}

enum call_status resolve_directory(const char *path, uint64_t rights, handle_t *directory)
{
  return resolve_path(path, DIRECTORY_KIND_DIRECTORY, rights, directory);
}

enum call_status resolve_file(const char *path, uint64_t rights, handle_t *file)
{
  return resolve_path(path, DIRECTORY_KIND_FILE, rights, file);
}

enum call_status remove_path(const char *path, uint64_t kind)
{
  size_t length = strlen(path);
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    return status;
  }
  size_t depth = context->count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }

  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1,
  };
  status = path_remove(context, path, kind, &workspace);
  free(component);
  free(directories);
  return status;
}

/* Native volumes reject a name that is not UTF-8, which BAD_REQUEST does not say. */
static bool valid_utf8(const char *text)
{
  size_t length = strlen(text);
  while (length) {
    int count = mbtowc(NULL, text, length);
    if (count <= 0) {
      return false;
    }
    text += count;
    length -= (size_t)count;
  }
  return true;
}

int report_directory_error(const char *program, const char *path, enum call_status status)
{
  const char *message;
  switch (status) {
  case CALL_BAD_HANDLE: message = "Invalid handle"; break;
  case CALL_DENIED: message = "Permission denied or path escapes its boundary"; break;
  case CALL_BAD_OPERATION: message = "Operation not supported"; break;
  case CALL_BAD_REQUEST:
    message = valid_utf8(path) ? "Invalid path or request" : "Invalid character (not valid UTF-8)";
    break;
  case CALL_UNAVAILABLE: message = "Working directory or resource unavailable"; break;
  case CALL_BUSY: message = "Resource busy"; break;
  case CALL_NO_MEMORY: message = "Out of memory"; break;
  case CALL_NO_SPACE: message = "No space left on device"; break;
  case CALL_QUOTA: message = "Storage quota exceeded"; break;
  case CALL_FILE_TOO_LARGE: message = "File too large"; break;
  case CALL_OUTCOME_UNKNOWN: message = "Operation outcome unknown; changes may have occurred"; break;
  case CALL_LIMIT: message = "Resource limit exceeded"; break;
  case CALL_NOT_FOUND: message = "Not found"; break;
  case CALL_WRONG_TYPE: message = "Unexpected object type"; break;
  case CALL_ALREADY_EXISTS: message = "Already exists"; break;
  case CALL_READ_ONLY: message = "Read-only filesystem"; break;
  case CALL_NOT_EMPTY: message = "Directory not empty"; break;
  case CALL_LINK_NOT_FOLLOWED: message = "Symbolic link not followed"; break;
  case CALL_NAME_TOO_LONG: message = "Name too long"; break;
  default:
    return fprintf(stderr, "%s: %s: Native call failed (status %u)\n",
        program, path, (unsigned)status);
  }
  return fprintf(stderr, "%s: %s: %s\n", program, path, message);
}
