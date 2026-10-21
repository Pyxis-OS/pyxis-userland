#ifndef USERSPACE_LAUNCH_ENVIRONMENT_H
#define USERSPACE_LAUNCH_ENVIRONMENT_H

#include <abi/launcher.h>
#include <environment.h>
#include <bundle_launch.h>
#include <command_search.h>
#include <string.h>
#include <handle.h>
#include <abi/directory.h>

#define PROFILE_CAP(entry_name, grant_index) \
  {.name = (uintptr_t)(entry_name), .kind = ENVIRONMENT_CAP, \
    .flags = ENVIRONMENT_EXPORTED, .value = (grant_index)}
#define PROFILE_PRIVATE_CAP(entry_name, grant_index) \
  {.name = (uintptr_t)(entry_name), .kind = ENVIRONMENT_CAP, .value = (grant_index)}
#define PROFILE_TEXT(entry_name, entry_text) \
  {.name = (uintptr_t)(entry_name), .kind = ENVIRONMENT_TEXT, \
    .flags = ENVIRONMENT_EXPORTED, .value = (uintptr_t)(entry_text), \
    .length = strlen(entry_text)}

static inline struct launch_environment_entry profile_stdio_entry(
    enum environment_stream_index index, size_t grant)
{
  const char *name = index == ENVIRONMENT_STDIN ? "stdin" :
      index == ENVIRONMENT_STDOUT ? "stdout" : "stderr";
  return (struct launch_environment_entry)PROFILE_CAP(name, grant);
}

/* Launch-profile selection only. These handles and names borrow an owned
 * capture that must stay live until admission; path routing uses the live ENV. */
#define PROFILE_DIRECTORY_LIMIT 16
struct profile_directory {
  const char *name;
  handle_t handle;
};

static inline enum call_status profile_directories(const struct environment_capture *capture,
    struct profile_directory *directories, size_t capacity, size_t *count)
{
  *count = 0;
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *entry = &capture->values[i];
    if (entry->kind != ENVIRONMENT_CAP || !(entry->flags & ENVIRONMENT_EXPORTED) ||
        !strncmp(entry->name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1) ||
        !strcmp(entry->name, "job_provisioner") || !strcmp(entry->name, "job_work") ||
        !strcmp(entry->name, "staging")) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(entry->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol != PROTOCOL_DIRECTORY) {
      continue;
    }
    if (*count == capacity) {
      return CALL_LIMIT;
    }
    directories[(*count)++] = (struct profile_directory){entry->name, entry->handle};
  }
  return CALL_OK;
}

static inline bool profile_stdio_name(const char *name)
{
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    struct launch_environment_entry entry = profile_stdio_entry(i, 0);
    if (!strcmp(name, (const char *)(uintptr_t)entry.name) ||
        !strcmp(name, environment_stdio_append_name(i))) {
      return true;
    }
  }
  return false;
}

/* Named FILE/controller values and commands preserve explicit export. Other
 * native roles require the caller's fixed selection and masks. Private
 * controllers remain explicit trusted successor copies. */
static inline enum call_status profile_inherit_services(
    const struct environment_capture *capture, bool trusted,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count)
{
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *value = &capture->values[i];
    bool commands = !strcmp(value->name, "commands") && (value->flags & ENVIRONMENT_EXPORTED);
    if (commands) {
      if (value->kind != ENVIRONMENT_CAP) {
        return CALL_WRONG_TYPE;
      }
      enum call_status status = command_list_validate(value->handle);
      if (status != CALL_OK) {
        return status;
      }
    }
    if (value->kind != ENVIRONMENT_CAP ||
        !strncmp(value->name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1) ||
        !strcmp(value->name, "job_provisioner") || !strcmp(value->name, "job_work") ||
        !strcmp(value->name, "staging") || !strcmp(value->name, "script") ||
        !strcmp(value->name, "screenshot_saver") ||
        !strcmp(value->name, "wifi_auth") || !strcmp(value->name, "wifi_auth_observation") ||
        profile_stdio_name(value->name)) {
      continue;
    }
    bool selected = false;
    for (size_t j = 0; j < *entry_count; ++j) {
      selected |= !strcmp(value->name, (const char *)(uintptr_t)entries[j].name);
    }
    if (selected) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(value->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    size_t length = strlen(value->name);
    bool controller = trusted && !(value->flags & ENVIRONMENT_EXPORTED) &&
        length > sizeof(".control") - 1 &&
        !strcmp(value->name + length - (sizeof(".control") - 1), ".control") &&
        info.protocol == PROTOCOL_FORWARDER;
    bool named_value = info.kind == HANDLE_KIND_NATIVE &&
        (info.protocol == PROTOCOL_FILE || info.protocol == PROTOCOL_FORWARDER || commands);
    if (!controller && (!(value->flags & ENVIRONMENT_EXPORTED) ||
        (info.kind != HANDLE_KIND_EXPORTED && !named_value))) {
      continue;
    }
    if (*grant_count == grant_capacity || *entry_count == entry_capacity) {
      return CALL_LIMIT;
    }
    entries[(*entry_count)++] = (struct launch_environment_entry){
      .name = (uintptr_t)value->name, .kind = ENVIRONMENT_CAP,
      .flags = controller ? 0 : ENVIRONMENT_EXPORTED, .value = *grant_count,
    };
    grants[(*grant_count)++] = (struct launch_grant){value->handle, info.rights, info.transport};
  }
  return CALL_OK;
}

#endif
