#include "shell.h"
#include <environment.h>
#include <stdlib.h>
#include <string.h>

bool shell_environment_runtime_role(const char *name)
{
  return !strcmp(name, "job_provisioner") || !strcmp(name, "interpreter_root") ||
      !strcmp(name, "interpreter_image");
}

enum call_status shell_environment_init(struct shell *shell)
{
  struct environment_capture captured = {0};
  enum call_status status = environment_capture_init(shell->environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &captured);
  if (status != CALL_OK) {
    return status;
  }
  size_t count = 0;
  for (size_t i = 0; i < captured.count; ++i) {
    count += captured.values[i].kind == ENVIRONMENT_CAP;
  }
  if (count > SIZE_MAX / sizeof(*shell->initial_entries)) {
    status = CALL_LIMIT;
  } else if (count) {
    shell->initial_entries = malloc(count * sizeof(*shell->initial_entries));
    if (!shell->initial_entries) {
      status = CALL_NO_MEMORY;
    }
  }
  if (status == CALL_OK) {
    for (size_t i = 0; i < captured.count; ++i) {
      const struct environment_value *value = &captured.values[i];
      if (value->kind == ENVIRONMENT_CAP) {
        struct shell_initial_entry *entry = &shell->initial_entries[shell->initial_entry_count++];
        memcpy(entry->name, value->name, sizeof(entry->name));
        entry->identity = value->identity;
      }
    }
  }
  environment_capture_close(&captured);
  return status;
}

void shell_environment_close(struct shell *shell)
{
  free(shell->initial_entries);
  shell->initial_entries = NULL;
  shell->initial_entry_count = 0;
}

bool shell_environment_initial(const struct shell *shell, const struct environment_value *value)
{
  for (size_t i = 0; i < shell->initial_entry_count; ++i) {
    const struct shell_initial_entry *entry = &shell->initial_entries[i];
    if (entry->identity == value->identity && !strcmp(entry->name, value->name)) {
      return true;
    }
  }
  return false;
}
