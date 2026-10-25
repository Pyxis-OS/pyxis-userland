#include <errno.h>
#include <pyxis/environment.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"
#include "thread_sync.h"

struct environment_state {
  size_t references;
  struct environment_capture capture;
};

static struct libc_lock environment_guard;
static struct environment_state *environment;

void pyxis_environment_snapshot_close(struct pyxis_environment_snapshot *snapshot)
{
  if (snapshot) {
    environment_capture_close(&snapshot->capture);
  }
}

enum call_status pyxis_environment_snapshot_init(struct pyxis_environment_snapshot *snapshot)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  return environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_TEXT_ONLY, &snapshot->capture);
}

enum call_status pyxis_environment_text_snapshot_init(struct pyxis_environment_snapshot *snapshot)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  return environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE | ENVIRONMENT_SNAPSHOT_TEXT_ONLY,
      &snapshot->capture);
}

static void environment_release(struct environment_state *state)
{
  if (!state) {
    return;
  }
  libc_lock_acquire(&environment_guard);
  bool destroy = !--state->references;
  libc_lock_release(&environment_guard);
  if (destroy) {
    environment_capture_close(&state->capture);
    free(state);
  }
}

static enum call_status environment_acquire(struct environment_state **result)
{
  *result = NULL;
  struct environment_info_reply info;
  enum call_status status = environment_info(startup_environment_handle(), &info);
  if (status != CALL_OK) {
    return status;
  }
  libc_lock_acquire(&environment_guard);
  if (environment && environment->capture.generation == info.generation) {
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
  struct environment_state *next = calloc(1, sizeof(*next));
  if (!next) {
    return CALL_NO_MEMORY;
  }
  status = environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE | ENVIRONMENT_SNAPSHOT_TEXT_ONLY,
      &next->capture);
  if (status != CALL_OK) {
    free(next);
    return status;
  }
  next->references = 2;
  libc_lock_acquire(&environment_guard);
  struct environment_state *previous = environment;
  /* Readers reuse an equal publication too: replacing it could invalidate a
   * borrowed value even though the native environment has not changed. */
  if (previous && previous->capture.generation >= next->capture.generation) {
    if (previous->references == SIZE_MAX) {
      status = CALL_LIMIT;
    } else {
      ++previous->references;
      *result = previous;
    }
    previous = NULL;
    next->references = 1;
  } else {
    environment = next;
    *result = next;
    next = NULL;
  }
  libc_lock_release(&environment_guard);
  environment_release(previous);
  environment_release(next);
  return status;
}

enum call_status pyxis_environment_get(const char *name, const char **value)
{
  if (!value) {
    return CALL_BAD_REQUEST;
  }
  *value = NULL;
  char checked[ENVIRONMENT_NAME_MAX + 1];
  if (!environment_name(checked, name)) {
    return CALL_BAD_REQUEST;
  }
  struct environment_state *state;
  enum call_status status = environment_acquire(&state);
  if (status != CALL_OK) {
    return status;
  }
  const struct environment_value *entry = environment_capture_find(&state->capture, name);
  status = entry ? CALL_OK : CALL_NOT_FOUND;
  if (entry) {
    *value = entry->text;
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
  char checked[ENVIRONMENT_NAME_MAX + 1];
  if (!environment_name(checked, name)) {
    return CALL_BAD_REQUEST;
  }
  struct environment_state *state;
  enum call_status status = environment_acquire(&state);
  if (status != CALL_OK) {
    return status;
  }
  const struct environment_value *entry = environment_capture_find(&state->capture, name);
  if (!entry) {
    status = CALL_NOT_FOUND;
  } else {
    *value = strdup(entry->text);
    if (!*value) {
      status = CALL_NO_MEMORY;
    }
  }
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

int setenv(const char *name, const char *value, int overwrite)
{
  enum call_status status = environment_set_text(startup_environment_handle(), name,
      value, ENVIRONMENT_EXPORTED, overwrite ? 0 : ENVIRONMENT_SET_NO_REPLACE);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}

int unsetenv(const char *name)
{
  enum call_status status = environment_unset(startup_environment_handle(), name);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}
