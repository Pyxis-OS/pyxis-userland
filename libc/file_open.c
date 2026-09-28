#include <abi/file.h>
#include <errno.h>
#include <handle.h>
#include <path.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "errors.h"

enum call_status file_open_path(const char *path, uint64_t rights,
                               bool create, handle_t *handle)
{
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
  struct path_workspace workspace = {directories, slots, component, length + 1};
  /* Path resolution borrows the initial chain; its temporary copies retain
   * authority through each call. It never changes or closes the startup chain. */
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_open_file(&context, path, rights, create,
      &workspace, handle);
  free(component);
  free(directories);
  return status;
}

int remove(const char *path)
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
  struct path_workspace workspace = {directories, slots, component, length + 1};
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_remove(&context, path, DIRECTORY_KIND_ANY, &workspace);
  free(component);
  free(directories);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
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
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}
