#ifndef USERSPACE_BOOTSTRAP_ENVIRONMENT_H
#define USERSPACE_BOOTSTRAP_ENVIRONMENT_H

#include <environment.h>
#include <startup.h>
#include <handle.h>
#include <pyxis/stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Application bootstrap authorities are selected on first use and retained
 * until process exit. This is not the live environment: mutable publications
 * must use an explicit owned LOOKUP. Only requested capabilities are captured;
 * private controllers and unrelated exported entries are never enumerated. */
struct bootstrap_capture {
  struct bootstrap_capture *next;
  char name[ENVIRONMENT_NAME_MAX + 1];
  handle_t handle;
};

static struct bootstrap_capture *bootstrap_captures;

static inline handle_t bootstrap_capability(const char *name)
{
  for (struct bootstrap_capture *capture = bootstrap_captures; capture;
      capture = capture->next) {
    if (!strcmp(capture->name, name)) {
      return capture->handle;
    }
  }
  struct bootstrap_capture *capture = malloc(sizeof(*capture));
  if (!capture || !environment_name(capture->name, name)) {
    free(capture);
    return HANDLE_INVALID;
  }
  if (environment_capability(startup_environment_handle(), name, &capture->handle) != CALL_OK) {
    free(capture);
    return HANDLE_INVALID;
  }
  capture->next = bootstrap_captures;
  bootstrap_captures = capture;
  return capture->handle;
}

/* One-shot bootstrap messages relinquish the table's original publication
 * after capturing it, so closing the local owner can finish the handoff. */
static inline handle_t bootstrap_take_capability(const char *name)
{
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(startup_environment_handle(), name, &entry, NULL, 0);
  if (status != CALL_OK || entry.kind != ENVIRONMENT_CAP) {
    return HANDLE_INVALID;
  }
  struct environment_unset_entry removed = {.identity = entry.identity};
  environment_name(removed.name, name);
  status = environment_unset_batch(startup_environment_handle(), &removed, 1);
  if (status != CALL_OK) {
    handle_close(entry.handle);
    return HANDLE_INVALID;
  }
  return entry.handle;
}

/* Qualification programs with fixed grant profiles reject extra capability
 * entries. Text contributes no authority and does not alter this selection. */
static inline bool bootstrap_only_capabilities(const char *const *names, size_t count)
{
  struct environment_capture capture = {0};
  if (environment_capture_init(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture) != CALL_OK) {
    return false;
  }
  bool valid = true;
  size_t selected = 0;
  for (size_t i = 0; valid && i < capture.count; ++i) {
    if (capture.values[i].kind != ENVIRONMENT_CAP) {
      continue;
    }
    bool found = false;
    for (size_t j = 0; j < count; ++j) {
      found |= !strcmp(capture.values[i].name, names[j]);
    }
    valid = found;
    ++selected;
  }
  environment_capture_close(&capture);
  return valid && selected == count;
}

static inline bool bootstrap_release_except(const handle_t *retained, size_t count)
{
  bool success = true;
  struct bootstrap_capture **next = &bootstrap_captures;
  while (*next) {
    struct bootstrap_capture *capture = *next;
    bool keep = false;
    for (size_t i = 0; i < count; ++i) {
      keep |= capture->handle == retained[i];
    }
    if (keep) {
      next = &capture->next;
      continue;
    }
    if (capture->handle != HANDLE_INVALID && handle_close(capture->handle) != 0) {
      success = false;
    }
    *next = capture->next;
    free(capture);
  }
  return success;
}

static inline struct pyxis_stdio_binding bootstrap_stdio(enum environment_stream_index index)
{
  struct pyxis_stdio_binding binding = {0};
  FILE *stream = index == ENVIRONMENT_STDIN ? stdin :
      index == ENVIRONMENT_STDOUT ? stdout : index == ENVIRONMENT_STDERR ? stderr : NULL;
  if (stream) {
    pyxis_stdio_stream(stream, &binding);
  }
  return binding;
}

#endif
