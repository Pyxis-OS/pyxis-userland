#include <errno.h>
#include <pyxis/working_path.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "errors.h"
#include "working_path_internal.h"

static struct pyxis_working_snapshot working;
static bool initialized;

static size_t root_prefix(const char *path)
{
  const char *end = path;
  while (*end && *end != ':' && *end != '/') {
    ++end;
  }
  if (end != path && *end == ':' && end[1] == '/' && end[2] == '/') {
    return end - path + 3;
  }
  return 0;
}

/* Only the description is normalized. The original input still reaches
 * path_change, preserving ordered lookup and parent-navigation boundaries. */
enum call_status libc_path_description(const char *current, const char *path, char **result)
{
  *result = NULL;
  size_t root = root_prefix(path);
  if (!root && !current) {
    return CALL_OK;
  }
  const char *base = root ? path : current;
  size_t length = root ? root : strlen(base);
  const char *tail = root ? path + root : path;
  size_t extra = strlen(tail);
  if (extra > SIZE_MAX - 2 || length > SIZE_MAX - extra - 2) {
    return CALL_LIMIT;
  }
  char *display = malloc(length + extra + 2);
  if (!display) {
    return CALL_NO_MEMORY;
  }
  memcpy(display, base, length);
  root = root_prefix(base);
  while (*tail) {
    if (*tail == '/') {
      ++tail;
      continue;
    }
    const char *start = tail;
    while (*tail && *tail != '/') {
      ++tail;
    }
    size_t component = tail - start;
    if (component == 1 && start[0] == '.') {
      continue;
    }
    if (component == 2 && start[0] == '.' && start[1] == '.') {
      while (length > root && display[length - 1] != '/') {
        --length;
      }
      if (length > root) {
        --length;
      }
    } else {
      if (length > root) {
        display[length++] = '/';
      }
      memcpy(display + length, start, component);
      length += component;
    }
  }
  display[length] = '\0';
  *result = display;
  return CALL_OK;
}

void pyxis_working_snapshot_close(struct pyxis_working_snapshot *snapshot)
{
  if (!snapshot) {
    return;
  }
  handle_t *storage = snapshot->context.directories;
  /* path_context_close quarantines each slot after its one CLOSE attempt.
   * Uncertain release is left to process teardown, never retried here. */
  path_context_close(&snapshot->context);
  free(storage);
  free(snapshot->path);
  *snapshot = (struct pyxis_working_snapshot){0};
}

static enum call_status initialize(void)
{
  if (initialized) {
    return CALL_OK;
  }
  size_t depth = startup_working_directory_count();
  if (depth > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  struct pyxis_working_snapshot initial = {0};
  handle_t *storage = depth ? malloc(depth * sizeof(*storage)) : NULL;
  if (depth && !storage) {
    return CALL_NO_MEMORY;
  }
  const char *path = startup_working_path();
  enum call_status status = CALL_OK;
  if (depth && path && root_prefix(path)) {
    status = libc_path_description(NULL, path, &initial.path);
  }
  if (status == CALL_OK) {
    status = path_context_init(&initial.context, storage, depth,
        startup_working_directories(), depth);
  }
  if (status != CALL_OK) {
    free(initial.path);
    free(storage);
    return status;
  }
  working = initial;
  initialized = true;
  return CALL_OK;
}

enum call_status pyxis_working_context(const struct path_context **context)
{
  if (!context) {
    return CALL_BAD_REQUEST;
  }
  *context = NULL;
  enum call_status status = initialize();
  if (status == CALL_OK) {
    *context = &working.context;
  }
  return status;
}

const char *pyxis_working_path(void)
{
  enum call_status status = initialize();
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return NULL;
  }
  return working.path;
}

enum call_status pyxis_working_bindings(const struct path_root *roots,
    size_t count, handle_t namespace_handle)
{
  if (count && !roots) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = initialize();
  if (status == CALL_OK) {
    working.context.roots = roots;
    working.context.root_count = count;
    working.context.namespace_handle = namespace_handle;
  }
  return status;
}

enum call_status pyxis_working_snapshot_init(struct pyxis_working_snapshot *snapshot,
    const char *cwd_override)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = (struct pyxis_working_snapshot){0};
  if (cwd_override && !*cwd_override) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = initialize();
  if (status != CALL_OK) {
    return status;
  }
  size_t depth = working.context.count;
  size_t length = cwd_override ? strlen(cwd_override) : 0;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  size_t slots = depth + length + 1;
  handle_t *storage = malloc(slots * sizeof(*storage));
  struct path_workspace workspace = {0};
  if (cwd_override) {
    workspace.directories = malloc(slots * sizeof(handle_t));
    workspace.directory_capacity = slots;
    workspace.component = malloc(length + 1);
    workspace.component_capacity = length + 1;
  }
  if (!storage || (cwd_override && (!workspace.directories || !workspace.component))) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  if (cwd_override) {
    status = libc_path_description(working.path, cwd_override, &snapshot->path);
  } else if (working.path) {
    snapshot->path = strdup(working.path);
    if (!snapshot->path) {
      status = CALL_NO_MEMORY;
    }
  }
  if (status != CALL_OK) {
    goto done;
  }
  status = path_context_init(&snapshot->context, storage, slots,
      working.context.directories, depth);
  if (status != CALL_OK) {
    goto done;
  }
  storage = NULL;
  snapshot->context.roots = working.context.roots;
  snapshot->context.root_count = working.context.root_count;
  snapshot->context.namespace_handle = working.context.namespace_handle;
  if (cwd_override) {
    status = path_change(&snapshot->context, cwd_override, &workspace);
  }
done:
  free(workspace.component);
  free(workspace.directories);
  free(storage);
  if (status != CALL_OK) {
    pyxis_working_snapshot_close(snapshot);
  }
  return status;
}

enum call_status pyxis_working_change(const char *path)
{
  if (!path || !*path) {
    return CALL_BAD_REQUEST;
  }
  struct pyxis_working_snapshot next;
  enum call_status status = pyxis_working_snapshot_init(&next, path);
  if (status != CALL_OK) {
    return status;
  }
  struct pyxis_working_snapshot previous = working;
  working = next;
  pyxis_working_snapshot_close(&previous);
  return CALL_OK;
}

void pyxis_working_clear(void)
{
  pyxis_working_snapshot_close(&working);
  initialized = true;
}

int chdir(const char *path)
{
  enum call_status status = pyxis_working_change(path);
  if (status != CALL_OK) {
    errno = status == CALL_WRONG_TYPE ? ENOTDIR : libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}

char *getcwd(char *buffer, size_t size)
{
  enum call_status status = initialize();
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return NULL;
  }
  if (!working.path) {
    errno = ENOTSUP;
    return NULL;
  }
  size_t length = strlen(working.path) + 1;
  if ((buffer || size) && size < length) {
    errno = ERANGE;
    return NULL;
  }
  if (!buffer) {
    buffer = malloc(size ? size : length);
    if (!buffer) {
      errno = ENOMEM;
      return NULL;
    }
  }
  memcpy(buffer, working.path, length);
  return buffer;
}
