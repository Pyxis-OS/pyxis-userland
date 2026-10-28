#include "session.h"
#include <directory.h>
#include <handle.h>
#include <path.h>
#include <pyxis/working_path.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum call_status session_path_resolve(const char *path,
    struct command_list_create_entry *entry)
{
  *entry = (struct command_list_create_entry){0};
  if (!session_path_valid(path)) {
    return CALL_BAD_REQUEST;
  }
  const struct path_context *context;
  enum call_status status = pyxis_working_context(&context);
  if (status != CALL_OK) {
    return status;
  }
  size_t length = strlen(path), depth = context->count;
  if (length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  size_t slots = depth + length + 1;
  handle_t *directories = malloc(slots * sizeof(*directories));
  char *component = malloc(length + 1);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = slots,
    .component = component, .component_capacity = length + 1,
  };
  status = path_open_directory_native(context, path,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &workspace, &entry->handle);
  free(component);
  free(directories);
  if (status == CALL_OK) {
    status = handle_rights(entry->handle, &entry->rights, NULL);
    if (status == CALL_OK) {
      entry->rights &= DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
          DIRECTORY_RIGHT_READ_FILES;
      /* Labels describe grants; truncation never changes resolution. */
      size_t label_length = length < COMMAND_LIST_LABEL_MAX ? length : COMMAND_LIST_LABEL_MAX;
      memcpy(entry->label, path, label_length);
    } else {
      if (handle_close(entry->handle) != CALL_OK) {
        status = CALL_OUTCOME_UNKNOWN;
      }
      *entry = (struct command_list_create_entry){0};
    }
  }
  return status;
}

bool session_path_skip_status(enum call_status status)
{
  return status == CALL_NOT_FOUND || status == CALL_DENIED || status == CALL_WRONG_TYPE ||
      status == CALL_BAD_OPERATION || status == CALL_UNAVAILABLE ||
      status == CALL_ENDPOINT_CLOSED || status == CALL_LINK_NOT_FOLLOWED ||
      status == CALL_NAME_TOO_LONG || status == CALL_IO || status == CALL_BUSY ||
      status == CALL_TIMED_OUT;
}

void session_path_warning(const char *path, enum call_status status,
    struct config_failure *warning)
{
  const char *reason = status == CALL_NOT_FOUND ? "not found" :
      status == CALL_DENIED ? "inaccessible" :
      status == CALL_WRONG_TYPE || status == CALL_BAD_OPERATION ? "not a native directory" :
      status == CALL_LINK_NOT_FOLLOWED ? "symbolic link not followed" :
      status == CALL_NAME_TOO_LONG ? "name too long" : "unavailable";
  struct config_failure name;
  config_failure_format(&name, "%s", path);
  /* Bound the already escaped name so the reason survives long locations. */
  config_failure_format(warning, "path: %.180s%s %s, skipped", name.text,
      strlen(name.text) > 180 ? "..." : "", reason);
}

enum call_status session_path_check(const struct session_config *config)
{
  for (size_t i = 0; i < config->path_count; ++i) {
    struct command_list_create_entry entry;
    enum call_status status = session_path_resolve(config->path[i], &entry);
    if (status == CALL_OK) {
      if (handle_close(entry.handle) != CALL_OK) {
        return CALL_OUTCOME_UNKNOWN;
      }
    } else if (session_path_skip_status(status)) {
      struct config_failure warning;
      session_path_warning(config->path[i], status, &warning);
      fprintf(stderr, "%s\n", warning.text);
    } else {
      return status;
    }
  }
  return CALL_OK;
}
