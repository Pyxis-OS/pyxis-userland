#include <abi/file.h>
#include <directory.h>
#include <handle.h>
#include <path.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "stream.h"

enum call_status stream_open_path(const char *path, uint64_t rights,
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
  char *parent_path = malloc(length + 1);
  if (!directories || !component || !parent_path) {
    free(parent_path);
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {directories, slots, component, length + 1};
  /* path_resolve only borrows the initial chain; its temporary copies retain
   * authority through each call. It never changes or closes the startup chain. */
  struct path_context context = {
    .directories = (handle_t *)startup_working_directories(), .count = depth,
  };
  enum call_status status = path_resolve(&context, path, DIRECTORY_KIND_FILE,
      rights, &workspace, handle);
  if (status != CALL_NOT_FOUND || !create) {
    goto done;
  }

  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    goto done;
  }
  const char *parent = ".";
  if (slash) {
    /* Keep the slash: a scheme root must remain "home://", not "home:/". */
    size_t parent_length = name - path;
    memcpy(parent_path, path, parent_length);
    parent_path[parent_length] = '\0';
    parent = parent_path;
  }
  uint64_t parent_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE;
  if (rights & FILE_RIGHT_READ) {
    parent_rights |= DIRECTORY_RIGHT_READ_FILES;
  }
  if (rights & FILE_RIGHT_WRITE) {
    parent_rights |= DIRECTORY_RIGHT_WRITE_FILES;
  }
  handle_t directory;
  status = path_resolve(&context, parent, DIRECTORY_KIND_DIRECTORY,
      parent_rights, &workspace, &directory);
  if (status == CALL_OK) {
    status = directory_create(directory, name, DIRECTORY_KIND_FILE, rights, handle);
    if (status == CALL_ALREADY_EXISTS) {
      /* Another creator won. Open its entry without replacing it. There is no
       * unlink/rename operation in this filesystem slice. */
      status = directory_lookup(directory, name, DIRECTORY_KIND_FILE, rights, handle);
    }
    handle_close(directory);
  }
done:
  free(parent_path);
  free(component);
  free(directories);
  return status;
}
