#include "directory.h"
#include <path.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum call_status resolve_path(const char *path, uint64_t kind, uint64_t rights,
    handle_t *handle)
{
  *handle = HANDLE_INVALID;
  size_t length = strlen(path);
  size_t depth = startup_working_directory_count();
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
  struct path_workspace workspace = {directories, slots, component, length + 1};
  /* Resolution only borrows this chain; do not close the startup handles. */
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_resolve(&context, path, kind, rights, &workspace, handle);
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
  size_t depth = startup_working_directory_count();
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
  struct path_workspace workspace = {directories, slots, component, length + 1};
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_remove(&context, path, kind, &workspace);
  free(component);
  free(directories);
  return status;
}

int report_directory_error(const char *program, const char *path, enum call_status status)
{
  const char *message;
  switch (status) {
  case CALL_BAD_HANDLE: message = "Invalid handle"; break;
  case CALL_DENIED: message = "Permission denied or path escapes its boundary"; break;
  case CALL_BAD_OPERATION: message = "Operation not supported"; break;
  case CALL_BAD_REQUEST: message = "Invalid path or request"; break;
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
  default:
    return fprintf(stderr, "%s: %s: Native call failed (status %u)\n",
        program, path, (unsigned)status);
  }
  return fprintf(stderr, "%s: %s: %s\n", program, path, message);
}
