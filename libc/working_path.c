#include <errno.h>
#include <pyxis/working_path.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "errors.h"
#include "working_path_internal.h"
#include "thread_sync.h"

/* Published chains are immutable. Retention keeps grants live while readers
 * duplicate or resolve outside the publication guard. */
struct working_state {
  size_t references;
  struct pyxis_working_snapshot snapshot;
};

static struct libc_lock working_guard;
static struct working_state *working_state;
/* Stable borrowed view; internal readers retain working_state instead. */
static struct pyxis_working_snapshot working;
static bool initialized;

size_t libc_path_root_prefix(const char *path)
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
  size_t root = libc_path_root_prefix(path);
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
  root = libc_path_root_prefix(base);
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

static enum call_status initial_snapshot(struct pyxis_working_snapshot *result)
{
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
  if (depth && path && libc_path_root_prefix(path)) {
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
  *result = initial;
  return CALL_OK;
}

static void working_release(struct working_state *state)
{
  if (!state) {
    return;
  }
  libc_lock_acquire(&working_guard);
  bool destroy = !--state->references;
  libc_lock_release(&working_guard);
  if (destroy) {
    pyxis_working_snapshot_close(&state->snapshot);
    free(state);
  }
}

static enum call_status working_acquire(struct working_state **result)
{
  *result = NULL;
  libc_lock_acquire(&working_guard);
  if (initialized) {
    if (working_state && working_state->references == SIZE_MAX) {
      libc_lock_release(&working_guard);
      return CALL_LIMIT;
    }
    if (working_state) {
      ++working_state->references;
    }
    *result = working_state;
    libc_lock_release(&working_guard);
    return CALL_OK;
  }
  libc_lock_release(&working_guard);
  struct working_state *initial = calloc(1, sizeof(*initial));
  if (!initial) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = initial_snapshot(&initial->snapshot);
  if (status != CALL_OK) {
    free(initial);
    return status;
  }
  initial->references = 1;
  libc_lock_acquire(&working_guard);
  if (!initialized) {
    working_state = initial;
    working = initial->snapshot;
    initialized = true;
    initial = NULL;
  }
  if (working_state && working_state->references == SIZE_MAX) {
    status = CALL_LIMIT;
  } else {
    if (working_state) {
      ++working_state->references;
    }
    *result = working_state;
  }
  libc_lock_release(&working_guard);
  working_release(initial);
  return status;
}

enum call_status pyxis_working_context(const struct path_context **context)
{
  if (!context) {
    return CALL_BAD_REQUEST;
  }
  *context = NULL;
  struct working_state *state;
  enum call_status status = working_acquire(&state);
  if (status == CALL_OK) {
    *context = &working.context;
    working_release(state);
  }
  return status;
}

const char *pyxis_working_path(void)
{
  struct working_state *state;
  enum call_status status = working_acquire(&state);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return NULL;
  }
  const char *path = state ? state->snapshot.path : NULL;
  working_release(state);
  return path;
}

enum call_status libc_working_snapshot_from(const struct pyxis_working_snapshot *source,
    struct pyxis_working_snapshot *snapshot, const char *cwd_override)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = (struct pyxis_working_snapshot){0};
  if (cwd_override && !*cwd_override) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = CALL_OK;
  size_t depth = source->context.count;
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
    status = libc_path_description(source->path, cwd_override, &snapshot->path);
  } else if (source->path) {
    snapshot->path = strdup(source->path);
    if (!snapshot->path) {
      status = CALL_NO_MEMORY;
    }
  }
  if (status != CALL_OK) {
    goto done;
  }
  status = path_context_init(&snapshot->context, storage, slots,
      source->context.directories, depth);
  if (status != CALL_OK) {
    goto done;
  }
  storage = NULL;
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

enum call_status pyxis_working_snapshot_init(struct pyxis_working_snapshot *snapshot,
    const char *cwd_override)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = (struct pyxis_working_snapshot){0};
  struct working_state *state;
  enum call_status status = working_acquire(&state);
  if (status == CALL_OK) {
    const struct pyxis_working_snapshot empty = {0};
    status = libc_working_snapshot_from(state ? &state->snapshot : &empty,
        snapshot, cwd_override);
    working_release(state);
  }
  return status;
}

static enum call_status working_replace(const char *path)
{
  for (;;) {
    struct working_state *previous;
    enum call_status status = working_acquire(&previous);
    if (status != CALL_OK) {
      return status;
    }
    struct working_state *next = calloc(1, sizeof(*next));
    if (!next) {
      working_release(previous);
      return CALL_NO_MEMORY;
    }
    next->references = 1;
    const struct pyxis_working_snapshot empty = {0};
    status = libc_working_snapshot_from(previous ? &previous->snapshot : &empty,
        &next->snapshot, path);
    if (status != CALL_OK) {
      working_release(next);
      working_release(previous);
      return status;
    }
    libc_lock_acquire(&working_guard);
    bool publish = working_state == previous;
    if (publish) {
      working_state = next;
      working = next->snapshot;
    }
    libc_lock_release(&working_guard);
    if (publish) {
      working_release(previous); /* Drop the publication reference. */
      working_release(previous); /* Drop this reader reference. */
      return CALL_OK;
    }
    working_release(next);
    working_release(previous);
  }
}

enum call_status pyxis_working_change(const char *path)
{
  if (!path || !*path) {
    return CALL_BAD_REQUEST;
  }
  return working_replace(path);
}

void pyxis_working_clear(void)
{
  libc_lock_acquire(&working_guard);
  struct working_state *previous = working_state;
  working_state = NULL;
  working = (struct pyxis_working_snapshot){0};
  initialized = true;
  libc_lock_release(&working_guard);
  working_release(previous);
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
  struct working_state *state;
  enum call_status status = working_acquire(&state);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return NULL;
  }
  if (!state || !state->snapshot.path) {
    working_release(state);
    errno = ENOTSUP;
    return NULL;
  }
  size_t length = strlen(state->snapshot.path) + 1;
  int error = 0;
  if ((buffer || size) && size < length) {
    error = ERANGE;
  } else {
    if (!buffer) {
      buffer = malloc(size ? size : length);
      if (!buffer) {
        error = ENOMEM;
      }
    }
    if (!error) {
      memcpy(buffer, state->snapshot.path, length);
    }
  }
  working_release(state);
  if (error) {
    errno = error;
    return NULL;
  }
  return buffer;
}
