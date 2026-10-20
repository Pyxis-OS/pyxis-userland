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
  struct environment_unset_entry removed[3];
  size_t removed_count = 0, count = 0;
  for (size_t i = 0; i < captured.count; ++i) {
    const struct environment_value *value = &captured.values[i];
    if (shell_environment_runtime_role(value->name)) {
      struct environment_unset_entry *entry = &removed[removed_count++];
      memcpy(entry->name, value->name, sizeof(entry->name));
      entry->identity = value->identity;
    } else {
      count += value->kind == ENVIRONMENT_CAP;
    }
  }
  /* Startup retained runtime handles before this snapshot. They must never be
   * URI bindings in the user-controlled working environment. */
  if (removed_count) {
    status = environment_unset_batch(shell->environment, removed, removed_count);
    if (status != CALL_OK) {
      environment_capture_close(&captured);
      return status;
    }
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
      if (value->kind == ENVIRONMENT_CAP && !shell_environment_runtime_role(value->name)) {
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
