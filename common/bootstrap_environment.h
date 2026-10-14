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
