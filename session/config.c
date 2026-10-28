#include "../common/launch_environment.h"
#include "config.h"
#include "../libconfig/handoff.h"
#include <abi/console.h>
#include <handle.h>
#include <environment.h>
#include <path.h>
#include <pyxis/working_path.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <startup.h>

static bool check_timezone(const char *name)
{
  if (!strcmp(name, "UTC")) {
    return true;
  }
  if (!session_timezone_valid(name)) {
    return false;
  }

  const char prefix[] = "boot://share/zoneinfo/";
  size_t length = strlen(name);
  if (length > SIZE_MAX - sizeof(prefix)) {
    return false;
  }
  char *path = malloc(sizeof(prefix) + length);
  if (!path) {
    return false;
  }
  memcpy(path, prefix, sizeof(prefix) - 1);
  memcpy(path + sizeof(prefix) - 1, name, length + 1);
  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) {
    return false;
  }
  /* The packaged database is trusted; libc validates the complete TZif when
   * a child first converts local time. Reject non-zone metadata files here. */
  char magic[4];
  bool valid = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
      !memcmp(magic, "TZif", sizeof(magic));
  return fclose(file) == 0 && valid;
}

static enum call_status resolve_path_entry(const char *path,
    struct command_list_create_entry *entry)
{
  *entry = (struct command_list_create_entry){0};
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
      /* The label describes the grant; truncation never changes resolution. */
      size_t label_length = length < COMMAND_LIST_LABEL_MAX ? length : COMMAND_LIST_LABEL_MAX;
      memcpy(entry->label, path, label_length);
    } else {
      handle_close(entry->handle);
      *entry = (struct command_list_create_entry){0};
    }
  }
  return status;
}

enum call_status session_path_create(const struct session_config *config,
    handle_t *list, size_t *failed_entry)
{
  *list = HANDLE_INVALID;
  *failed_entry = SIZE_MAX;
  if (!config->path_configured) {
    return CALL_NOT_FOUND;
  }
  if (config->path_count > SIZE_MAX / sizeof(struct command_list_create_entry)) {
    return CALL_LIMIT;
  }
  struct command_list_create_entry *entries = config->path_count ?
      calloc(config->path_count, sizeof(*entries)) : NULL;
  if (config->path_count && !entries) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = CALL_OK;
  size_t resolved = 0;
  for (; resolved < config->path_count; ++resolved) {
    status = resolve_path_entry(config->path[resolved], &entries[resolved]);
    if (status != CALL_OK) {
      *failed_entry = resolved;
      break;
    }
  }
  if (status == CALL_OK) {
    status = command_list_create(startup_environment_handle(), entries, resolved, list);
  }
  for (size_t i = 0; i < resolved; ++i) {
    handle_close(entries[i].handle);
  }
  free(entries);
  return status;
}

bool session_config_read(struct session_config *config)
{
  handle_t source = HANDLE_INVALID;
  enum call_status source_status = environment_capability(startup_environment_handle(), "session_config", &source);
  if (source_status != CALL_OK && source_status != CALL_NOT_FOUND) {
    fprintf(stderr, "session: selected session FILE unavailable (status %u)\n", source_status);
    return false;
  }
  if (source == HANDLE_INVALID) {
    if (session_config_read_file(SESSION_CONFIG_PATH, config) == CONFIG_ERROR) {
      return false;
    }
  } else {
    *config = (struct session_config){0};
    struct config_handoff_bytes snapshot = {0};
    enum call_status status;
    bool read = config_handoff_read(source, &snapshot, &status);
    bool closed = handle_close(source) == 0;
    if (!read || !closed) {
      fprintf(stderr, "session: selected session FILE unavailable (status %u, closed %u)\n",
          status, (unsigned)closed);
      config_handoff_bytes_free(&snapshot);
      return false;
    }
    enum config_result result = session_config_read_literal_bytes("selected session FILE",
        snapshot.bytes, snapshot.size, config);
    config_handoff_bytes_free(&snapshot);
    if (result != CONFIG_OK) {
      return false;
    }
  }
  if (!check_timezone(config->timezone)) {
    fprintf(stderr, "session: unavailable or invalid timezone '%s'\n", config->timezone);
    session_config_free(config);
    return false;
  }
  return true;
}

static bool configured_variable(const struct session_config *config, const char *name)
{
  for (size_t i = 0; i < config->environment_count; ++i) {
    if (!strcmp(config->environment[i].name, name)) {
      return true;
    }
  }
  return false;
}

struct launch_environment_entry *session_environment(const struct session_config *config,
    const struct pyxis_environment_snapshot *snapshot,
    size_t *count)
{
  size_t inherited = snapshot->capture.count;
  size_t limit = SIZE_MAX / sizeof(struct launch_environment_entry);
  if (inherited > limit - 2 || config->environment_count > limit - 2 - inherited) {
    fputs("session: environment too large\n", stderr);
    return NULL;
  }
  struct launch_environment_entry *environment =
      malloc((inherited + config->environment_count + 2) * sizeof(*environment));
  if (!environment) {
    fputs("session: cannot allocate environment\n", stderr);
    return NULL;
  }

  size_t used = 0;
  const struct environment_value *source = snapshot->capture.values;
  for (size_t i = 0; i < inherited; ++i) {
    const char *name = source[i].name;
    if (!strcmp(name, "stdin.append") || !strcmp(name, "stdout.append") ||
        !strcmp(name, "stderr.append")) {
      continue;
    }
    /* The boot-carried discovery name cannot be shadowed by configuration. */
    if (!strcmp(name, "PYXIS_REMOTE_BEACON") || !strcmp(name, "DNS_SERVER") ||
        (!session_variable_reserved(name) && !configured_variable(config, name))) {
      environment[used++] = (struct launch_environment_entry)PROFILE_TEXT(name, source[i].text);
    }
  }
  for (size_t i = 0; i < config->environment_count; ++i) {
    environment[used++] = (struct launch_environment_entry)PROFILE_TEXT(config->environment[i].name, config->environment[i].value);
  }
  environment[used++] = (struct launch_environment_entry)PROFILE_TEXT("TZ", config->timezone);
  *count = used;
  return environment;
}
