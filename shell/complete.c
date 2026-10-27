#include "shell.h"
#include <directory.h>
#include <command_search.h>
#include <package_commands.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

/* Names are offered only when they can be typed as a bare word. */
#define COMPLETION_LIMIT 4096

static const char *const builtins[] = {
  "affinity", "cd", "clear", "describe", "exit", "export", "mount", "poweroff",
  "reboot", "release", "reset", "revoke", "revocable", "service", "session",
  "sync", "title", "unset", "which",
};

struct names {
  char **items;
  size_t count, capacity;
};

static bool bare_character(unsigned char byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '+' ||
         byte == '-';
}

static bool add_name(struct names *names, const char *name, size_t length)
{
  if (names->count == COMPLETION_LIMIT) {
    return true;
  }
  if (names->count == names->capacity) {
    size_t capacity = names->capacity ? names->capacity * 2 : 64;
    char **larger = realloc(names->items, capacity * sizeof(*larger));
    if (!larger) {
      return false;
    }
    names->items = larger;
    names->capacity = capacity;
  }
  char *copy = malloc(length + 1);
  if (!copy) {
    return false;
  }
  memcpy(copy, name, length);
  copy[length] = '\0';
  names->items[names->count++] = copy;
  return true;
}

/* Completion offers hints only from held entries; selection still validates
 * the actual target at launch. Withheld ENUMERATE offers no names. */
static bool add_programs(handle_t directory, bool projected, const char *prefix,
    size_t prefix_length, struct names *names)
{
  bool ok = true;
  size_t capacity = 128;
  char *name = malloc(capacity);
  struct directory_cursor cursor = {0};
  for (; name;) {
    struct directory_enumerate_reply entry;
    if (directory_enumerate(directory, &cursor, name, capacity, &entry) != CALL_OK ||
        entry.outcome == DIRECTORY_END || entry.outcome == DIRECTORY_CHANGED) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      if (!larger) {
        break;
      }
      name = larger;
      capacity = entry.name_size;
      continue;
    }
    cursor = entry.cursor;
    size_t length = strlen(name);
    if (!projected) {
      if (length <= 4 ||
          !((entry.kind == DIRECTORY_KIND_FILE && !strcmp(name + length - 4, ".pxe")) ||
            (entry.kind == DIRECTORY_KIND_DIRECTORY && !strcmp(name + length - 4, ".pxb")))) {
        continue;
      }
      length -= 4;
    }
    if (length < prefix_length || memcmp(name, prefix, prefix_length)) continue;
    bool bare = true;
    for (size_t i = 0; bare && i < length; ++i) {
      bare = bare_character((unsigned char)name[i]);
    }
    if (bare && !add_name(names, name, length)) {
      ok = false;
      break;
    }
  }
  free(name);
  return ok;
}

static int compare_names(const void *left, const void *right)
{
  return strcmp(*(char *const *)left, *(char *const *)right);
}

/* The first word of a pipeline stage, typed bare. */
static bool complete_command(struct shell *shell, const struct completion_word *word,
    struct term_candidates *result)
{
  if (!word->plain || word->quote) {
    return false;
  }
  const char *prefix = word->value;
  size_t prefix_length = strlen(prefix);
  size_t start = word->start;
  for (size_t i = 0; i < prefix_length; ++i) {
    if (!bare_character((unsigned char)prefix[i])) {
      return false;
    }
  }

  struct names names = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
    size_t length = strlen(builtins[i]);
    if (length >= prefix_length && !memcmp(builtins[i], prefix, prefix_length)) {
      ok = add_name(&names, builtins[i], length);
    }
  }
  struct command_search_capture search = {0};
  enum call_status status = command_search_capture_init(shell->environment, &search);
  if (status != CALL_OK && status != CALL_NOT_FOUND) ok = false;
  for (size_t i = 0; ok && i < search.count; ++i) {
    handle_t fixed = HANDLE_INVALID;
    struct package_command_info info;
    status = package_commands_info(search.entries[i].handle, &info);
    bool projected = status == CALL_OK;
    if (projected) {
      status = package_commands_capture(search.entries[i].handle, HANDLE_INVALID, &fixed);
    }
    if (status == CALL_OK || status == CALL_BAD_OPERATION) {
      ok = add_programs(projected ? fixed : search.entries[i].handle,
          projected, prefix, prefix_length, &names);
    }
    if (fixed) handle_close(fixed);
  }
  command_search_capture_close(&search);

  size_t unique = 0;
  if (ok) {
    qsort(names.items, names.count, sizeof(*names.items), compare_names);
    for (size_t i = 0; i < names.count; ++i) {
      if (unique && !strcmp(names.items[unique - 1], names.items[i])) {
        free(names.items[i]);
      } else {
        names.items[unique++] = names.items[i];
      }
    }
  }
  if (!ok || !unique) {
    for (size_t i = 0; i < names.count; ++i) {
      free(names.items[i]);
    }
    free(names.items);
    return false;
  }
  *result = (struct term_candidates){.names = names.items, .count = unique, .start = start};
  return true;
}

bool shell_complete(void *context, const char *line, size_t cursor,
    struct term_candidates *result)
{
  struct shell *shell = context;
  struct completion_word word;
  if (!completion_scan(line, cursor, &word)) {
    return false;
  }
  bool path = !word.command || strchr(word.value, '/') || strchr(word.value, ':');
  bool found = path ? shell_complete_path(shell, &word, result)
                    : complete_command(shell, &word, result);
  free(word.value);
  return found;
}
