#include <errno.h>
#include <path.h>
#include <pyxis/directory.h>
#include <pyxis/working_path.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "errors.h"
#include "working_path_internal.h"

static enum call_status workspace_init(const struct path_context *context, const char *path,
    struct path_workspace *workspace)
{
  if (!path || !*path || path[0] == '/') {
    return CALL_BAD_REQUEST;
  }
  size_t length = strlen(path);
  size_t depth = context->count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  *workspace = (struct path_workspace){
    .directory_capacity = depth + length + 1, .component_capacity = length + 1,
  };
  workspace->directories = malloc(workspace->directory_capacity * sizeof(handle_t));
  workspace->component = malloc(workspace->component_capacity);
  return workspace->directories && workspace->component ? CALL_OK : CALL_NO_MEMORY;
}

static void workspace_close(struct path_workspace *workspace)
{
  free(workspace->component);
  free(workspace->directories);
}

/* Explicit roots are captured by the one ordered walk. Relative paths instead
 * retain one cwd snapshot; neither operation re-reads the mutable selection. */
static enum call_status working_init(const char *path, struct pyxis_working_snapshot *working)
{
  *working = (struct pyxis_working_snapshot){0};
  if (!path || !*path || path[0] == '/') {
    return CALL_BAD_REQUEST;
  }
  return libc_path_root_prefix(path) ? CALL_OK : pyxis_working_snapshot_init(working, NULL);
}

enum call_status libc_directory_open_native(const char *path, handle_t *handle)
{
  *handle = HANDLE_INVALID;
  struct pyxis_working_snapshot working;
  enum call_status status = working_init(path, &working);
  struct path_workspace workspace = {0};
  if (status == CALL_OK) {
    status = workspace_init(&working.context, path, &workspace);
  }
  if (status == CALL_OK) {
    status = path_open_directory_native(&working.context, path, DIRECTORY_RIGHT_ENUMERATE,
        &workspace, handle);
  }
  workspace_close(&workspace);
  pyxis_working_snapshot_close(&working);
  return status;
}

static int create_directory(const char *path, bool recursive, uintmax_t *created)
{
  if (recursive && path && !*path) {
    if (created) {
      *created = 0;
    }
    return 0;
  }
  struct pyxis_working_snapshot working;
  enum call_status status = working_init(path, &working);
  struct path_workspace workspace = {0};
  if (status == CALL_OK) {
    status = workspace_init(&working.context, path, &workspace);
  }
  if (status == CALL_OK) {
    status = path_create_directory_native_count(&working.context, path, recursive,
        &workspace, created);
  }
  workspace_close(&workspace);
  pyxis_working_snapshot_close(&working);
  if (status != CALL_OK) {
    errno = status == CALL_WRONG_TYPE ? ENOTDIR : libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}

int pyxis_directory_create(const char *path)
{
  return create_directory(path, false, NULL);
}

int pyxis_directory_create_all(const char *path)
{
  return create_directory(path, true, NULL);
}

int pyxis_directory_create_all_count(const char *path, uintmax_t *created)
{
  if (!created) {
    errno = EINVAL;
    return -1;
  }
  return create_directory(path, true, created);
}

static int remove_entry(const char *path, uint64_t kind)
{
  struct pyxis_working_snapshot working;
  enum call_status status = working_init(path, &working);
  struct path_workspace workspace = {0};
  if (status == CALL_OK) {
    status = workspace_init(&working.context, path, &workspace);
  }
  if (status == CALL_OK) {
    status = path_remove_native(&working.context, path, kind, &workspace);
  }
  workspace_close(&workspace);
  pyxis_working_snapshot_close(&working);
  if (status != CALL_OK) {
    errno = status == CALL_WRONG_TYPE && kind == DIRECTORY_KIND_DIRECTORY ? ENOTDIR :
        libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}

int pyxis_directory_unlink(const char *path)
{
  return remove_entry(path, DIRECTORY_KIND_FILE);
}

int pyxis_directory_remove(const char *path)
{
  return remove_entry(path, DIRECTORY_KIND_DIRECTORY);
}

int pyxis_directory_remove_any(const char *path)
{
  return remove_entry(path, DIRECTORY_KIND_ANY);
}

int pyxis_directory_rename(const char *source, const char *destination)
{
  struct pyxis_working_snapshot working = {0};
  enum call_status status = CALL_BAD_REQUEST;
  struct path_workspace workspaces[2] = {0};
  if (!source || !*source || !destination || !*destination) {
    goto done;
  }
  status = libc_path_root_prefix(source) && libc_path_root_prefix(destination) ? CALL_OK :
      pyxis_working_snapshot_init(&working, NULL);
  if (status == CALL_OK) {
    status = workspace_init(&working.context, source, &workspaces[0]);
  }
  if (status == CALL_OK) {
    status = workspace_init(&working.context, destination, &workspaces[1]);
  }
  if (status == CALL_OK) {
    status = path_rename_native(&working.context, source, destination, DIRECTORY_RENAME_REPLACE,
        &workspaces[0], &workspaces[1]);
  }
done:
  workspace_close(&workspaces[0]);
  workspace_close(&workspaces[1]);
  pyxis_working_snapshot_close(&working);
  if (status != CALL_OK) {
    errno = libc_path_errno(status, source, destination);
    return -1;
  }
  return 0;
}
