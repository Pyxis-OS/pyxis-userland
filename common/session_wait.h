#ifndef COMMON_SESSION_WAIT_H
#define COMMON_SESSION_WAIT_H

#include <network_environment.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Runtime policy, not authority. Only session successors inherit this marker. */
#define SESSION_WAIT_ENV "PYXIS_SESSION_WAIT"

static inline enum call_status session_wait_environment(
    struct network_environment *environment, bool enabled)
{
  size_t count = 0;
  for (size_t i = 0; i < environment->count; ++i) {
    if (strcmp((const char *)environment->variables[i].name, SESSION_WAIT_ENV)) {
      environment->variables[count++] = environment->variables[i];
    }
  }
  environment->count = count;
  if (!enabled) {
    return CALL_OK;
  }
  if (count >= SIZE_MAX / sizeof(*environment->variables)) {
    return CALL_LIMIT;
  }
  struct startup_variable *variables = realloc(environment->variables,
      (count + 1) * sizeof(*variables));
  if (!variables) {
    return CALL_NO_MEMORY;
  }
  environment->variables = variables;
  environment->variables[count] = (struct startup_variable){
    (uintptr_t)SESSION_WAIT_ENV, (uintptr_t)"1",
  };
  environment->count = count + 1;
  return CALL_OK;
}

#endif
