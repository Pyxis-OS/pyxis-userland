#include <errno.h>
#include <pyxis/environment.h>
#include <stdbool.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"
#include "thread_sync.h"

/* Published snapshots are immutable; the guard protects references and the
 * current pointer. Readers copy outside the guard while retaining their state. */
struct environment_state {
  size_t references;
  struct pyxis_environment_snapshot snapshot;
};

static struct libc_lock environment_guard;
static struct environment_state *environment;

static bool name_valid(const char *name)
{
  return name && *name && !strchr(name, '=');
}

void pyxis_environment_snapshot_close(struct pyxis_environment_snapshot *snapshot)
{
  if (!snapshot) {
    return;
  }
  if (snapshot->variables) {
    for (size_t index = 0; index < snapshot->count; index++) {
      free((void *)(uintptr_t)snapshot->variables[index].name);
      free((void *)(uintptr_t)snapshot->variables[index].value);
    }
    free(snapshot->variables);
  }
  *snapshot = (struct pyxis_environment_snapshot){0};
}

static enum call_status string_copy(const char *source, char **result)
{
  size_t length = strlen(source);
  if (length == SIZE_MAX) {
    return CALL_LIMIT;
  }
  char *copy = malloc(length + 1);
  if (!copy) {
    return CALL_NO_MEMORY;
  }
  memcpy(copy, source, length + 1);
  *result = copy;
  return CALL_OK;
}

static enum call_status snapshot_copy(const struct startup_variable *variables,
  size_t count, struct pyxis_environment_snapshot *snapshot)
{
  *snapshot = (struct pyxis_environment_snapshot){0};
  if (count && !variables) {
    return CALL_BAD_REQUEST;
  }
  if (count > SIZE_MAX / sizeof(*variables)) {
    return CALL_LIMIT;
  }
  if (!count) {
    return CALL_OK;
  }

  snapshot->variables = calloc(count, sizeof(*variables));
  if (!snapshot->variables) {
    return CALL_NO_MEMORY;
  }
  snapshot->count = count;
  for (size_t index = 0; index < count; index++) {
    const char *name = (const char *)(uintptr_t)variables[index].name;
    const char *value = (const char *)(uintptr_t)variables[index].value;
    enum call_status status = CALL_BAD_REQUEST;
    char *copy = NULL;
    if (name_valid(name) && value) {
      status = string_copy(name, &copy);
      if (status == CALL_OK) {
        snapshot->variables[index].name = (uint64_t)(uintptr_t)copy;
        status = string_copy(value, &copy);
        if (status == CALL_OK) {
          snapshot->variables[index].value = (uint64_t)(uintptr_t)copy;
        }
      }
    }
    if (status != CALL_OK) {
      pyxis_environment_snapshot_close(snapshot);
      return status;
    }
  }
  return CALL_OK;
}

static void environment_release(struct environment_state *state)
{
  libc_lock_acquire(&environment_guard);
  bool destroy = !--state->references;
  libc_lock_release(&environment_guard);
  if (destroy) {
    pyxis_environment_snapshot_close(&state->snapshot);
    free(state);
  }
}

static enum call_status environment_acquire(struct environment_state **result)
{
  libc_lock_acquire(&environment_guard);
  if (environment) {
    if (environment->references == SIZE_MAX) {
      libc_lock_release(&environment_guard);
      return CALL_LIMIT;
    }
    ++environment->references;
    *result = environment;
    libc_lock_release(&environment_guard);
    return CALL_OK;
  }
  libc_lock_release(&environment_guard);

  struct environment_state *initial = calloc(1, sizeof(*initial));
  if (!initial) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = snapshot_copy(startup_environment_variables(),
      startup_environment_count(), &initial->snapshot);
  if (status != CALL_OK) {
    free(initial);
    return status;
  }
  initial->references = 1;
  libc_lock_acquire(&environment_guard);
  if (!environment) {
    environment = initial;
    initial = NULL;
  }
  if (environment->references == SIZE_MAX) {
    status = CALL_LIMIT;
  } else {
    ++environment->references;
    *result = environment;
  }
  libc_lock_release(&environment_guard);
  if (initial) {
    environment_release(initial);
  }
  return status;
}

enum call_status pyxis_environment_snapshot_init(struct pyxis_environment_snapshot *snapshot)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = (struct pyxis_environment_snapshot){0};
  struct environment_state *state;
  enum call_status status = environment_acquire(&state);
  if (status == CALL_OK) {
    status = snapshot_copy(state->snapshot.variables, state->snapshot.count, snapshot);
    environment_release(state);
  }
  return status;
}

static size_t environment_find(const struct pyxis_environment_snapshot *snapshot,
    const char *name)
{
  for (size_t index = 0; index < snapshot->count; index++) {
    const char *stored_name = (const char *)(uintptr_t)snapshot->variables[index].name;
    if (strcmp(name, stored_name) == 0) {
      return index;
    }
  }
  return snapshot->count;
}

enum call_status pyxis_environment_get(const char *name, const char **value)
{
  if (!value) {
    return CALL_BAD_REQUEST;
  }
  *value = NULL;
  if (!name_valid(name)) {
    return CALL_BAD_REQUEST;
  }
  struct environment_state *state;
  enum call_status status = environment_acquire(&state);
  if (status != CALL_OK) {
    return status;
  }
  size_t index = environment_find(&state->snapshot, name);
  if (index == state->snapshot.count) {
    status = CALL_NOT_FOUND;
  } else {
    *value = (const char *)(uintptr_t)state->snapshot.variables[index].value;
  }
  environment_release(state);
  return status;
}

enum call_status pyxis_environment_copy(const char *name, char **value)
{
  if (!value) {
    return CALL_BAD_REQUEST;
  }
  *value = NULL;
  if (!name_valid(name)) {
    return CALL_BAD_REQUEST;
  }
  struct environment_state *state;
  enum call_status status = environment_acquire(&state);
  if (status != CALL_OK) {
    return status;
  }
  size_t index = environment_find(&state->snapshot, name);
  status = index == state->snapshot.count ? CALL_NOT_FOUND :
      string_copy((const char *)(uintptr_t)state->snapshot.variables[index].value, value);
  environment_release(state);
  return status;
}

char *getenv(const char *name)
{
  const char *value;
  enum call_status status = pyxis_environment_get(name, &value);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    errno = libc_call_errno(status);
  }
  return (char *)value;
}

static int environment_mutate(const char *name, const char *value, bool overwrite)
{
  if (!name_valid(name)) {
    errno = EINVAL;
    return -1;
  }
  for (;;) {
    struct environment_state *previous;
    enum call_status status = environment_acquire(&previous);
    if (status != CALL_OK) {
      errno = libc_call_errno(status);
      return -1;
    }
    size_t index = environment_find(&previous->snapshot, name);
    bool found = index < previous->snapshot.count;
    if ((value && found && !overwrite) || (!value && !found)) {
      environment_release(previous);
      return 0;
    }
    struct environment_state *next = calloc(1, sizeof(*next));
    if (!next) {
      environment_release(previous);
      errno = ENOMEM;
      return -1;
    }
    next->references = 1;
    status = snapshot_copy(previous->snapshot.variables, previous->snapshot.count,
        &next->snapshot);
    if (status == CALL_OK && !value) {
      free((void *)(uintptr_t)next->snapshot.variables[index].name);
      free((void *)(uintptr_t)next->snapshot.variables[index].value);
      --next->snapshot.count;
      memmove(&next->snapshot.variables[index], &next->snapshot.variables[index + 1],
          (next->snapshot.count - index) * sizeof(*next->snapshot.variables));
    } else if (status == CALL_OK) {
      char *copy = NULL;
      status = string_copy(value, &copy);
      if (status == CALL_OK && found) {
        free((void *)(uintptr_t)next->snapshot.variables[index].value);
        next->snapshot.variables[index].value = (uint64_t)(uintptr_t)copy;
      } else if (status == CALL_OK) {
        char *name_copy = NULL;
        status = string_copy(name, &name_copy);
        if (status == CALL_OK &&
            next->snapshot.count >= SIZE_MAX / sizeof(struct startup_variable)) {
          status = CALL_LIMIT;
        }
        struct startup_variable *variables = NULL;
        if (status == CALL_OK) {
          variables = realloc(next->snapshot.variables,
              (next->snapshot.count + 1) * sizeof(*variables));
          if (!variables) {
            status = CALL_NO_MEMORY;
          }
        }
        if (status == CALL_OK) {
          next->snapshot.variables = variables;
          variables[next->snapshot.count++] = (struct startup_variable){
            .name = (uint64_t)(uintptr_t)name_copy,
            .value = (uint64_t)(uintptr_t)copy,
          };
        } else {
          free(name_copy);
          free(copy);
        }
      }
    }
    if (status != CALL_OK) {
      environment_release(next);
      environment_release(previous);
      errno = libc_call_errno(status);
      return -1;
    }
    libc_lock_acquire(&environment_guard);
    bool publish = environment == previous;
    if (publish) {
      environment = next;
    }
    libc_lock_release(&environment_guard);
    if (publish) {
      environment_release(previous); /* Drop the publication reference. */
      environment_release(previous); /* Drop this reader reference. */
      return 0;
    }
    environment_release(next);
    environment_release(previous);
  }
}

int setenv(const char *name, const char *value, int overwrite)
{
  if (!value) {
    errno = EINVAL;
    return -1;
  }
  return environment_mutate(name, value, overwrite != 0);
}

int unsetenv(const char *name)
{
  return environment_mutate(name, NULL, false);
}
