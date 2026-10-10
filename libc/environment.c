#include <errno.h>
#include <pyxis/environment.h>
#include <stdbool.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"

static struct pyxis_environment_snapshot environment;
static bool environment_initialized;

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

static enum call_status environment_init(void)
{
  if (environment_initialized) {
    return CALL_OK;
  }
  enum call_status status = snapshot_copy(startup_environment_variables(),
    startup_environment_count(), &environment);
  if (status == CALL_OK) {
    environment_initialized = true;
  }
  return status;
}

enum call_status pyxis_environment_snapshot_init(struct pyxis_environment_snapshot *snapshot)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = (struct pyxis_environment_snapshot){0};
  enum call_status status = environment_init();
  if (status != CALL_OK) {
    return status;
  }
  return snapshot_copy(environment.variables, environment.count, snapshot);
}

static size_t environment_find(const char *name)
{
  for (size_t index = 0; index < environment.count; index++) {
    const char *stored_name = (const char *)(uintptr_t)environment.variables[index].name;
    if (strcmp(name, stored_name) == 0) {
      return index;
    }
  }
  return environment.count;
}

static int environment_ready(const char *name)
{
  if (!name_valid(name)) {
    errno = EINVAL;
    return -1;
  }
  enum call_status status = environment_init();
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
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
  enum call_status status = environment_init();
  if (status != CALL_OK) {
    return status;
  }
  size_t index = environment_find(name);
  if (index == environment.count) {
    return CALL_NOT_FOUND;
  }
  *value = (const char *)(uintptr_t)environment.variables[index].value;
  return CALL_OK;
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
  if (!value) {
    errno = EINVAL;
    return -1;
  }
  if (environment_ready(name) < 0) {
    return -1;
  }
  size_t index = environment_find(name);
  if (index < environment.count && !overwrite) {
    return 0;
  }

  char *value_copy = NULL;
  enum call_status status = string_copy(value, &value_copy);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  if (index < environment.count) {
    free((void *)(uintptr_t)environment.variables[index].value);
    environment.variables[index].value = (uint64_t)(uintptr_t)value_copy;
    return 0;
  }

  char *name_copy = NULL;
  status = string_copy(name, &name_copy);
  if (status != CALL_OK) {
    free(value_copy);
    errno = libc_call_errno(status);
    return -1;
  }
  if (environment.count >= SIZE_MAX / sizeof(*environment.variables)) {
    free(name_copy);
    free(value_copy);
    errno = EOVERFLOW;
    return -1;
  }
  struct startup_variable *variables = realloc(environment.variables,
    (environment.count + 1) * sizeof(*variables));
  if (!variables) {
    free(name_copy);
    free(value_copy);
    errno = ENOMEM;
    return -1;
  }
  variables[environment.count] = (struct startup_variable){
    .name = (uint64_t)(uintptr_t)name_copy,
    .value = (uint64_t)(uintptr_t)value_copy,
  };
  environment.variables = variables;
  environment.count++;
  return 0;
}

int unsetenv(const char *name)
{
  if (environment_ready(name) < 0) {
    return -1;
  }
  size_t index = environment_find(name);
  if (index == environment.count) {
    return 0;
  }
  free((void *)(uintptr_t)environment.variables[index].name);
  free((void *)(uintptr_t)environment.variables[index].value);
  environment.count--;
  memmove(&environment.variables[index], &environment.variables[index + 1],
    (environment.count - index) * sizeof(*environment.variables));
  return 0;
}
