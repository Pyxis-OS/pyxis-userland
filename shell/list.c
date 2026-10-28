#include "syntax.h"
#include <command_search.h>
#include <directory.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_ENUMERATE)

struct list_entries {
  struct command_list_create_entry *items;
  size_t count, capacity;
};

/* Takes the owned directory only on success. */
static enum call_status append_entry(struct list_entries *entries, handle_t directory,
    uint64_t rights, const char *label)
{
  if (entries->count == entries->capacity) {
    size_t capacity = entries->capacity ? entries->capacity * 2 : 8;
    if (capacity < entries->capacity || capacity > SIZE_MAX / sizeof(*entries->items)) {
      return CALL_LIMIT;
    }
    struct command_list_create_entry *larger = realloc(entries->items,
        capacity * sizeof(*larger));
    if (!larger) {
      return CALL_NO_MEMORY;
    }
    entries->items = larger;
    entries->capacity = capacity;
  }
  struct command_list_create_entry *entry = &entries->items[entries->count++];
  *entry = (struct command_list_create_entry){.handle = directory,
    .rights = rights & LIST_DIRECTORY_RIGHTS};
  /* Labels are bounded diagnostics; the complete path was used for resolution. */
  snprintf(entry->label, sizeof(entry->label), "%s", label);
  return CALL_OK;
}

static enum call_status append_list(struct list_entries *entries, handle_t list)
{
  struct command_list_info_reply info;
  enum call_status status = command_list_info(list, &info);
  for (size_t i = 0; status == CALL_OK && i < info.count; ++i) {
    struct command_list_entry_reply entry;
    status = command_list_get(list, i, &entry);
    if (status == CALL_OK) {
      status = append_entry(entries, entry.handle, entry.rights, entry.label);
      if (status != CALL_OK) {
        if (handle_close(entry.handle)) {
          status = CALL_OUTCOME_UNKNOWN;
        }
      }
    }
  }
  return status;
}

static enum call_status append_member(struct shell *shell, struct list_entries *entries,
    const struct environment_value *member)
{
  handle_t directory = HANDLE_INVALID;
  enum call_status status;
  struct handle_info info;
  const char *label;
  if (member->kind == ENVIRONMENT_TEXT) {
    status = shell_open_native_directory(shell, member->text,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &directory);
    if (status != CALL_OK) {
      return status;
    }
    status = handle_query(directory, &info);
    label = member->text;
  } else if (member->kind == ENVIRONMENT_CAP) {
    status = handle_query(member->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.kind != HANDLE_KIND_NATIVE || info.transport) {
      return CALL_WRONG_TYPE;
    }
    if (info.protocol == PROTOCOL_COMMAND_LIST) {
      return append_list(entries, member->handle);
    }
    if (info.protocol != PROTOCOL_DIRECTORY) {
      return CALL_WRONG_TYPE;
    }
    status = handle_copy_restricted(member->handle, info.rights & LIST_DIRECTORY_RIGHTS,
        0, &directory);
    label = member->name;
  } else {
    return CALL_WRONG_TYPE;
  }
  if (status == CALL_OK) {
    status = append_entry(entries, directory, info.rights, label);
  }
  if (status != CALL_OK && directory != HANDLE_INVALID) {
    if (handle_close(directory)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
  return status;
}

enum call_status shell_list_create(struct shell *shell,
    const struct environment_value *members, size_t count, handle_t *list)
{
  *list = HANDLE_INVALID;
  struct list_entries entries = {0};
  enum call_status status = CALL_OK;
  for (size_t i = 0; status == CALL_OK && i < count; ++i) {
    status = append_member(shell, &entries, &members[i]);
  }
  if (status == CALL_OK) {
    status = command_list_create(shell->environment, entries.items, entries.count, list);
  }
  for (size_t i = 0; i < entries.count; ++i) {
    if (handle_close(entries.items[i].handle)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
  free(entries.items);
  if (status != CALL_OK && *list != HANDLE_INVALID) {
    if (handle_close(*list)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    *list = HANDLE_INVALID;
  }
  return status;
}
