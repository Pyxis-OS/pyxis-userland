#include <directory.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <path.h>
#include <pyxis/working_path.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "errors.h"
#include "working_path_internal.h"

#define IDENTITY_VALID (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID)

static bool explicit_scheme(const char *path)
{
  const char *end = path;
  while (*end && *end != ':' && *end != '/') {
    ++end;
  }
  return end != path && *end == ':' && end[1] == '/' && end[2] == '/';
}

static bool same_identity(const struct file_info_reply *first,
    const struct file_info_reply *second)
{
  return (first->valid & IDENTITY_VALID) == IDENTITY_VALID &&
      (second->valid & IDENTITY_VALID) == IDENTITY_VALID &&
      first->domain == second->domain && first->object == second->object;
}

static enum call_status target_open(const struct path_context *context, const char *path,
    struct path_workspace *workspace, uint64_t *kind, handle_t *handle)
{
  *kind = DIRECTORY_KIND_FILE;
  enum call_status status = path_resolve_native(context, path, *kind, FILE_RIGHT_READ,
      workspace, handle);
  if (status == CALL_DENIED) {
    status = path_resolve_native(context, path, *kind, FILE_RIGHT_WRITE, workspace, handle);
  }
  if (status == CALL_WRONG_TYPE || status == CALL_DENIED) {
    enum call_status directory = path_resolve_native(context, path,
        DIRECTORY_KIND_DIRECTORY, 0, workspace, handle);
    if (directory == CALL_OK) {
      *kind = DIRECTORY_KIND_DIRECTORY;
      return CALL_OK;
    }
    if (directory != CALL_WRONG_TYPE) {
      return directory;
    }
  }
  return status;
}

static enum call_status target_info(handle_t handle, uint64_t kind,
    struct file_info_reply *info)
{
  return kind == DIRECTORY_KIND_DIRECTORY ? directory_info(handle, info) :
      file_info(handle, info);
}

/* Retaining a cwd is sufficient for relative lookup, but its tracked description
 * does not prove any of its parents. Check the whole live chain, not just cwd. */
static enum call_status prove_ancestry(const struct path_context *original, const char *path)
{
  if (!path) {
    return CALL_BAD_OPERATION;
  }
  if (strlen(path) >= PATH_MAX) {
    return CALL_LIMIT;
  }
  struct pyxis_working_snapshot candidate;
  enum call_status status = pyxis_working_snapshot_init(&candidate, path);
  if (status != CALL_OK) {
    return status;
  }
  if (candidate.context.count != original->count) {
    status = CALL_BAD_OPERATION;
  }
  for (size_t i = 0; status == CALL_OK && i < original->count; ++i) {
    struct file_info_reply first, second;
    status = directory_info(original->directories[i], &first);
    if (status == CALL_OK) {
      status = directory_info(candidate.context.directories[i], &second);
    }
    if (status == CALL_OK && !same_identity(&first, &second)) {
      status = CALL_BAD_OPERATION;
    }
  }
  pyxis_working_snapshot_close(&candidate);
  return status;
}

char *realpath(const char *__restrict path, char *__restrict resolved)
{
  if (!path || !*path) {
    errno = EINVAL;
    return NULL;
  }
  size_t length = strnlen(path, PATH_MAX);
  if (length == PATH_MAX) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    errno = libc_path_errno(status, path, NULL);
    return NULL;
  }
  if (context->count > SIZE_MAX / sizeof(handle_t) - PATH_MAX - 1) {
    errno = EOVERFLOW;
    return NULL;
  }
  size_t slots = context->count + PATH_MAX + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(PATH_MAX);
  if (!directories || !component) {
    free(component);
    free(directories);
    errno = ENOMEM;
    return NULL;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = PATH_MAX,
  };
  handle_t original = HANDLE_INVALID, candidate = HANDLE_INVALID;
  uint64_t kind, candidate_kind;
  char *description = NULL;
  int error = 0;
  status = target_open(context, path, &workspace, &kind, &original);
  if (status != CALL_OK) {
    goto done;
  }
  struct file_info_reply original_info, candidate_info;
  status = target_info(original, kind, &original_info);
  if (status == CALL_OK && (original_info.valid & IDENTITY_VALID) != IDENTITY_VALID) {
    status = CALL_BAD_OPERATION;
  }
  const char *current = pyxis_working_path();
  if (status == CALL_OK && !explicit_scheme(path)) {
    status = prove_ancestry(context, current);
  }
  if (status == CALL_OK) {
    status = libc_path_description(current, path, &description);
  }
  if (status == CALL_OK && (!description || strlen(description) >= PATH_MAX)) {
    error = description ? ENAMETOOLONG : ENOTSUP;
    goto done;
  }
  if (status == CALL_OK) {
    status = target_open(context, description, &workspace, &candidate_kind, &candidate);
  }
  if (status == CALL_OK) {
    status = target_info(candidate, candidate_kind, &candidate_info);
  }
  if (status == CALL_OK &&
      (candidate_kind != kind || !same_identity(&original_info, &candidate_info))) {
    status = CALL_BAD_OPERATION;
  }
done:
  if (!error && status != CALL_OK) {
    error = libc_path_errno(status, path, NULL);
  }
  if (candidate != HANDLE_INVALID && descriptor_release_handle(candidate) < 0 && !error) {
    error = errno;
  }
  if (original != HANDLE_INVALID && descriptor_release_handle(original) < 0 && !error) {
    error = errno;
  }
  free(component);
  free(directories);
  if (error) {
    free(description);
    errno = error;
    return NULL;
  }
  if (resolved) {
    memcpy(resolved, description, strlen(description) + 1);
    free(description);
    return resolved;
  }
  return description;
}
