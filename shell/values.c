#include "values.h"
#include <clock.h>
#include <command_search.h>
#include <forwarder.h>
#include <handle.h>
#include <resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum call_status set_entry(struct environment_set_entry *entry,
    const char *name, const struct environment_value *value, bool exported)
{
  *entry = (struct environment_set_entry){0};
  if (!environment_name(entry->name, name)) {
    return CALL_BAD_REQUEST;
  }
  if (!strcmp(name, "path")) {
    enum call_status status = value->kind == ENVIRONMENT_CAP ?
        command_list_validate(value->handle) : CALL_WRONG_TYPE;
    if (status != CALL_OK) {
      return status;
    }
  }
  entry->kind = value->kind;
  entry->flags = exported ? ENVIRONMENT_EXPORTED : 0;
  if (value->kind == ENVIRONMENT_CAP) {
    if (shell_environment_runtime_role(name) || shell_environment_runtime_role(value->name)) {
      return CALL_DENIED;
    }
    entry->handle = value->handle;
    entry->rights = value->rights;
    entry->transport = value->transport;
  } else if (value->kind == ENVIRONMENT_TEXT) {
    entry->text_address = (uintptr_t)value->text;
    entry->text_length = value->length;
  } else {
    return CALL_WRONG_TYPE;
  }
  return CALL_OK;
}

static enum call_status append_value(struct environment_set_entry *entries, size_t *count,
    const struct shell_expanded_word *word, const char *name, bool exported)
{
  enum call_status status = set_entry(&entries[(*count)++], name, word->value, exported);
  struct shell_capture_result *capture = word->capture;
  if (status != CALL_OK || !capture) {
    return status;
  }
  const struct environment_value *companions[] = {&capture->observation, &capture->control};
  for (size_t i = 0; i < sizeof(companions) / sizeof(*companions); ++i) {
    const struct environment_value *value = companions[i];
    if (!value->handle) {
      continue;
    }
    struct environment_set_entry *entry = &entries[(*count)++];
    status = set_entry(entry, value->name, value, false);
    if (status != CALL_OK) {
      return status;
    }
    entry->set_flags = ENVIRONMENT_SET_REQUIRE_ABSENT;
  }
  return CALL_OK;
}

enum command_result shell_assign(struct shell *shell,
    const struct shell_expanded_word *word)
{
  struct environment_set_entry entries[3];
  size_t count = 0;
  enum call_status status = append_value(entries, &count, word, word->assignment_name, false);
  if (status == CALL_OK) {
    status = environment_set_batch(shell->environment, entries, count);
  }
  if (status != CALL_OK) {
    return shell_directory_error(shell, "shell: assignment", word->assignment_name, status);
  }
  if (word->capture) {
    word->capture->published = true;
    return word->capture->command;
  }
  return COMMAND_OK;
}

/* The snapshot fixes both TEXT sizing and the CAP's publication before SET.
 * No returned table pointer or independently acquired handle is an identity. */
static enum call_status capture_value(handle_t snapshot, const char *name,
    struct environment_value *value)
{
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(snapshot, name, &entry, NULL, 0);
  char *text = NULL;
  if (status == CALL_BUFFER_TOO_SMALL && entry.kind == ENVIRONMENT_TEXT) {
    if (entry.text_length == SIZE_MAX) {
      return CALL_LIMIT;
    }
    text = malloc(entry.text_length + 1);
    if (!text) {
      return CALL_NO_MEMORY;
    }
    status = environment_lookup(snapshot, name, &entry, text, entry.text_length + 1);
  }
  if (status != CALL_OK) {
    free(text);
    return status;
  }
  *value = (struct environment_value){
    .kind = entry.kind, .flags = entry.flags, .identity = entry.identity,
    .handle = entry.handle, .rights = entry.rights,
    .transport = entry.transport, .text = text, .length = entry.text_length,
  };
  return CALL_OK;
}

static enum command_result export_values(struct shell *shell,
    const struct shell_expansion *expansion)
{
  size_t count = expansion->command.stages[0].count - 1;
  if (!count) {
    return shell_error(shell, "usage: export NAME[=VALUE]...\n");
  }
  struct environment_set_entry *entries = calloc(count * 3, sizeof(*entries));
  struct environment_value *owned = calloc(count, sizeof(*owned));
  handle_t snapshot = HANDLE_INVALID;
  enum call_status status = entries && owned ? CALL_OK : CALL_NO_MEMORY;
  const char *name = "values";
  size_t used = 0;
  for (size_t i = 0; status == CALL_OK && i < count; ++i) {
    const struct shell_expanded_word *word = &expansion->words[i + 1];
    const struct environment_value *value = word->value;
    name = word->assignment_name;
    if (!name) {
      name = expansion->command.stages[0].arguments[i + 1];
      if (snapshot == HANDLE_INVALID) {
        status = environment_snapshot(shell->environment,
            ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, NULL);
      }
      if (status == CALL_OK) {
        status = capture_value(snapshot, name, &owned[i]);
      }
      value = &owned[i];
      if (status == CALL_OK && (value->flags & ENVIRONMENT_EXPORTED)) {
        continue;
      }
    }
    if (status == CALL_OK) {
      struct shell_expanded_word selected = *word;
      selected.value = value;
      status = append_value(entries, &used, &selected, name, true);
    }
  }
  if (status == CALL_OK && used) {
    status = environment_set_batch(shell->environment, entries, used);
  }
  enum command_result result = COMMAND_OK;
  if (status == CALL_OK) {
    for (size_t i = 0; i < count; ++i) {
      struct shell_capture_result *capture = expansion->words[i + 1].capture;
      if (capture) {
        capture->published = true;
        if (capture->command != COMMAND_OK) {
          result = COMMAND_FAILED;
        }
      }
    }
  }
  bool closed = true;
  if (snapshot != HANDLE_INVALID && handle_close(snapshot)) {
    closed = false;
  }
  for (size_t i = 0; owned && i < count; ++i) {
    if (owned[i].handle && handle_close(owned[i].handle)) {
      closed = false;
    }
    free(owned[i].text);
  }
  free(owned);
  free(entries);
  if (!closed) {
    shell_directory_error(shell, "export: cleanup", name, CALL_OUTCOME_UNKNOWN);
    return COMMAND_FATAL;
  }
  return status == CALL_OK ? result :
      shell_directory_error(shell, "export", name, status);
}

static enum command_result unset_values(struct shell *shell, char **arguments, size_t count)
{
  if (count < 2) {
    return shell_error(shell, "usage: unset NAME...\n");
  }
  struct environment_unset_entry *entries = calloc(count - 1, sizeof(*entries));
  enum call_status status = entries ? CALL_OK : CALL_NO_MEMORY;
  size_t i = 1;
  for (; status == CALL_OK && i < count; ++i) {
    if (!environment_name(entries[i - 1].name, arguments[i])) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (status == CALL_OK) {
    status = environment_unset_batch(shell->environment, entries, count - 1);
  }
  free(entries);
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "unset", arguments[i ? i - 1 : 0], status);
}

static bool print_field(const char *label, const char *text, size_t size)
{
  if (printf("%s: ", label) < 0) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = text[i];
    if (putchar(byte >= ' ' && byte <= '~' ? byte : '?') == EOF) {
      return false;
    }
  }
  return putchar('\n') != EOF;
}

static enum command_result describe_value(struct shell *shell,
    const struct shell_expansion *expansion)
{
  const struct shell_stage *stage = &expansion->command.stages[0];
  if (stage->count != 2) {
    return shell_error(shell, "usage: describe PATH | describe $VALUE\n");
  }
  const struct environment_value *value = expansion->words[1].value;
  struct resource_result opened = {0};
  struct describe_reply info = {0};
  enum call_status status;
  if (value->kind == ENVIRONMENT_CAP) {
    status = handle_describe(value->handle, 0, &info);
  } else {
    uint64_t now;
    status = clock_now(shell->clock, &now);
    if (status == CALL_OK && now > UINT64_MAX - UINT64_C(30000000000)) {
      status = CALL_LIMIT;
    }
    if (status == CALL_OK) {
      status = resource_open_path(shell->directory, stage->arguments[1],
          PROVIDER_INTENT_READ, 0, now + UINT64_C(30000000000), &opened);
      info = opened.info;
    }
  }
  enum command_result result = COMMAND_OK;
  if (status != CALL_OK) {
    result = shell_directory_error(shell, "describe", stage->arguments[1], status);
  } else {
    static const char *const outcomes[RESOURCE_OUTCOME_COUNT] = {
      "ok", "not found", "denied", "I/O error", "too large", "aborted", "uncertain",
    };
    const struct resource_description *description = &info.description;
    if (printf("protocol: %ju; kind: %s; rights: 0x%jx; transport: 0x%jx\n"
        "outcome: %s\n", (uintmax_t)info.protocol,
        info.kind == HANDLE_KIND_NATIVE ? "native" : "exported",
        (uintmax_t)info.rights, (uintmax_t)info.transport,
        outcomes[description->outcome]) < 0 ||
        ((description->flags & RESOURCE_LENGTH_KNOWN) ?
        printf("length: %ju\n", (uintmax_t)description->length) :
        printf("length: unknown\n")) < 0 ||
        printf("rereadable: %s; complete: %s; error body: %s\n",
        description->flags & RESOURCE_REREADABLE ? "declared" : "not declared",
        description->flags & RESOURCE_COMPLETE ? "declared" : "not declared",
        description->flags & RESOURCE_ERROR_BODY ? "available" : "none declared") < 0 ||
        !print_field("media type", description->media_type, description->media_type_size) ||
        !print_field("detail", description->detail, description->detail_size)) {
      result = COMMAND_FATAL;
    } else if (description->outcome != RESOURCE_OK) {
      result = COMMAND_FAILED;
    }
  }
  if (resource_result_close(&opened) != CALL_OK) {
    shell_directory_error(shell, "describe: cleanup", stage->arguments[1], CALL_OUTCOME_UNKNOWN);
    result = COMMAND_FATAL;
  }
  return result;
}

bool shell_value_builtin(const char *name)
{
  return !strcmp(name, "export") || !strcmp(name, "unset") ||
      !strcmp(name, "describe") || !strcmp(name, "revoke") || !strcmp(name, "revocable");
}

enum command_result shell_values_command(struct shell *shell,
    const struct shell_expansion *expansion)
{
  const struct shell_stage *stage = &expansion->command.stages[0];
  const char *name = stage->arguments[0];
  if (!strcmp(name, "export")) {
    return export_values(shell, expansion);
  }
  if (!strcmp(name, "unset")) {
    return unset_values(shell, stage->arguments, stage->count);
  }
  if (!strcmp(name, "describe")) {
    return describe_value(shell, expansion);
  }
  if (!strcmp(name, "revocable")) {
    return shell_error(shell, "revocable: requires a named capture and the pending capture prerequisites\n");
  }
  if (stage->count != 2 || expansion->words[1].value->kind != ENVIRONMENT_CAP) {
    return shell_error(shell, "usage: revoke ${NAME.control}\n");
  }
  enum call_status status = forwarder_cut(expansion->words[1].value->handle);
  return status == CALL_OK ? COMMAND_OK :
      shell_directory_error(shell, "revoke", stage->arguments[1], status);
}
