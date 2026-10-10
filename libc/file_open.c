#include <abi/file.h>
#include <clock.h>
#include <errno.h>
#include <handle.h>
#include <path.h>
#include <provider.h>
#include <random.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "descriptor.h"
#include "errors.h"

#define TEMPORARY_ALPHABET_MASK 63

/* Shared by file opens and metadata queries: create applies only to files. */
static enum call_status open_path(const char *path, uint64_t kind, uint64_t rights,
                                  bool create, bool exclusive, bool native_only,
                                  struct pyxis_response_info *info, handle_t *handle)
{
  if (info) {
    *info = (struct pyxis_response_info){0};
  }
  *handle = HANDLE_INVALID;
  if (!path || !*path) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strlen(path);
  size_t depth = startup_working_directory_count();
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  /* Each path byte can introduce at most one component. This sizes temporary
   * storage from the request, not a fixed path/depth limit. */
  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct provider_http_workspace *http = NULL;
  if (!native_only && kind == DIRECTORY_KIND_FILE && provider_http_uri(path)) {
    http = malloc(sizeof(*http));
    if (!http) {
      free(component);
      free(directories);
      return CALL_NO_MEMORY;
    }
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1,
    .http = http, .response = info, .clock = startup_resource("clock"),
  };
  /* Path resolution borrows the initial chain; its temporary copies retain
   * authority through each call. It never changes or closes the startup chain. */
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status;
  if (native_only) {
    status = path_resolve_native(&context, path, kind, rights, &workspace, handle);
  } else if (exclusive) {
    status = path_create_file(&context, path, rights, &workspace, handle);
  } else if (kind == DIRECTORY_KIND_FILE) {
    status = path_open_file(&context, path, rights, create, &workspace, handle);
  } else {
    status = path_resolve(&context, path, kind, rights, &workspace, handle);
  }
  free(http);
  free(component);
  free(directories);
  return status;
}

enum call_status file_open_path_response(const char *path, uint64_t rights,
    bool create, struct pyxis_response_info *info, handle_t *handle)
{
  return open_path(path, DIRECTORY_KIND_FILE, rights, create, false, false, info, handle);
}

enum call_status file_open_path(const char *path, uint64_t rights,
                               bool create, handle_t *handle)
{
  return open_path(path, DIRECTORY_KIND_FILE, rights, create, false, false, NULL, handle);
}

enum call_status file_create_path(const char *path, uint64_t rights, handle_t *handle)
{
  return open_path(path, DIRECTORY_KIND_FILE, rights, true, true, false, NULL, handle);
}

enum call_status directory_open_path(const char *path, uint64_t rights, handle_t *handle)
{
  return open_path(path, DIRECTORY_KIND_DIRECTORY, rights, false, false, false, NULL, handle);
}

int access(const char *path, int mode)
{
  if (mode & ~(R_OK | W_OK | X_OK)) {
    errno = EINVAL;
    return -1;
  }
  if (mode & X_OK) {
    errno = ENOTSUP;
    return -1;
  }
  uint64_t file_rights = ((mode & R_OK) ? FILE_RIGHT_READ : 0) |
      ((mode & W_OK) ? FILE_RIGHT_WRITE : 0);
  uint64_t directory_rights = ((mode & R_OK) ? DIRECTORY_RIGHT_ENUMERATE : 0) |
      ((mode & W_OK) ? DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE : 0);
  handle_t handle;
  /* Identify without child rights first: directory enumeration/mutation grants
   * are independent of the READ_FILES/WRITE_FILES needed for a file lookup. */
  enum call_status status = open_path(path, DIRECTORY_KIND_FILE, 0,
      false, false, true, NULL, &handle);
  if (status == CALL_OK) {
    if (descriptor_release_handle(handle) < 0) {
      return -1;
    }
    if (mode == F_OK) {
      return 0;
    }
    status = open_path(path, DIRECTORY_KIND_FILE, file_rights,
        false, false, true, NULL, &handle);
  } else if (status == CALL_WRONG_TYPE) {
    status = open_path(path, DIRECTORY_KIND_DIRECTORY, directory_rights,
        false, false, true, NULL, &handle);
  }
  if (status != CALL_OK) {
    errno = status == CALL_WRONG_TYPE ? ENOTDIR : libc_path_errno(status, path, NULL);
    return -1;
  }
  return descriptor_release_handle(handle);
}

enum call_status file_temporary_name(char *suffix)
{
  handle_t clock = startup_resource("clock");
  handle_t random = startup_resource("random");
  if (clock == HANDLE_INVALID || random == HANDLE_INVALID) {
    return CALL_UNAVAILABLE;
  }
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (now > UINT64_MAX - RANDOM_MAX_WAIT_NS) {
    return CALL_LIMIT;
  }
  unsigned char bytes[TEMPORARY_NAME_LENGTH];
  status = random_read(random, bytes, sizeof(bytes), now + RANDOM_MAX_WAIT_NS);
  if (status != CALL_OK) {
    return status;
  }
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    suffix[i] = alphabet[bytes[i] & TEMPORARY_ALPHABET_MASK];
  }
  return CALL_OK;
}

int mkstemp(char *template)
{
  if (!template) {
    errno = EINVAL;
    return -1;
  }
  size_t length = strlen(template);
  if (length < TEMPORARY_NAME_LENGTH ||
      memcmp(template + length - TEMPORARY_NAME_LENGTH, "XXXXXX", TEMPORARY_NAME_LENGTH)) {
    errno = EINVAL;
    return -1;
  }
  int saved_errno = errno;
  const struct descriptor_mode mode = {
    .readable = true, .writable = true, .create = true, .exclusive = true,
  };
  for (int attempt = 0; attempt < TEMPORARY_CREATE_ATTEMPTS; ++attempt) {
    enum call_status status = file_temporary_name(template + length - TEMPORARY_NAME_LENGTH);
    if (status != CALL_OK) {
      errno = libc_call_errno(status);
      return -1;
    }
    int descriptor = descriptor_open(template, &mode, NULL);
    if (descriptor >= 0) {
      errno = saved_errno;
      return descriptor;
    }
    if (errno != EEXIST) {
      return -1;
    }
  }
  errno = EEXIST;
  return -1;
}

static int remove_kind(const char *path, uint64_t kind)
{
  if (!path || !*path) {
    errno = EINVAL;
    return -1;
  }
  size_t length = strlen(path);
  size_t depth = startup_working_directory_count();
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    errno = EOVERFLOW;
    return -1;
  }

  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    errno = ENOMEM;
    return -1;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1,
  };
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_remove(&context, path, kind, &workspace);
  free(component);
  free(directories);
  if (status != CALL_OK) {
    errno = kind == DIRECTORY_KIND_DIRECTORY && status == CALL_WRONG_TYPE ?
        ENOTDIR : libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}

int remove(const char *path)
{
  return remove_kind(path, DIRECTORY_KIND_ANY);
}

int unlink(const char *path)
{
  return remove_kind(path, DIRECTORY_KIND_FILE);
}

int rmdir(const char *path)
{
  return remove_kind(path, DIRECTORY_KIND_DIRECTORY);
}

int mkdir(const char *path, mode_t mode)
{
  (void)mode;
  if (!path || !*path) {
    errno = EINVAL;
    return -1;
  }
  size_t length = strlen(path);
  size_t depth = startup_working_directory_count();
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    errno = EOVERFLOW;
    return -1;
  }

  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    errno = ENOMEM;
    return -1;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1,
  };
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_create_directory(&context, path, &workspace);
  free(component);
  free(directories);
  if (status != CALL_OK) {
    errno = libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}

int rename(const char *old_path, const char *new_path)
{
  const char *paths[] = {old_path, new_path};
  struct path_workspace workspaces[2] = {0};
  size_t depth = startup_working_directory_count();
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < 2; ++i) {
    if (!paths[i] || !*paths[i]) {
      status = CALL_BAD_REQUEST;
      goto done;
    }
    size_t length = strlen(paths[i]);
    if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
        depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
      status = CALL_LIMIT;
      goto done;
    }
    workspaces[i].directory_capacity = depth + length + 1;
    workspaces[i].component_capacity = length + 1;
    workspaces[i].directories = malloc(workspaces[i].directory_capacity * sizeof(handle_t));
    workspaces[i].component = malloc(length + 1);
    if (!workspaces[i].directories || !workspaces[i].component) {
      status = CALL_NO_MEMORY;
      goto done;
    }
  }

  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  status = path_rename(&context, old_path, new_path, DIRECTORY_RENAME_REPLACE,
      &workspaces[0], &workspaces[1]);
done:
  for (size_t i = 0; i < 2; ++i) {
    free(workspaces[i].component);
    free(workspaces[i].directories);
  }
  if (status != CALL_OK) {
    errno = libc_path_errno(status, old_path, new_path);
    return -1;
  }
  return 0;
}
