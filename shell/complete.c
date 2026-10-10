#include "shell.h"
#include <directory.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

/* Names are offered only when they can be typed as a bare word. */
#define COMPLETION_LIMIT 4096

static const char *const builtins[] = {
  "affinity", "cd", "exit", "mount", "namespace", "poweroff", "reboot", "service",
  "session", "title",
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

/* Programs are NAME.pxe files. Failure to list a root, such as a withheld
 * ENUMERATE right, simply offers fewer names. */
static bool add_programs(struct shell *shell, const char *root, const char *prefix,
    size_t prefix_length, struct names *names)
{
  handle_t directory;
  if (shell_open_directory(shell, root, DIRECTORY_RIGHT_ENUMERATE, &directory) != CALL_OK) {
    return true;
  }
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
    if (entry.kind != DIRECTORY_KIND_FILE || length <= 4 || strcmp(name + length - 4, ".pxe") ||
        length - 4 < prefix_length || memcmp(name, prefix, prefix_length)) {
      continue;
    }
    bool bare = true;
    for (size_t i = 0; bare && i < length - 4; ++i) {
      bare = bare_character((unsigned char)name[i]);
    }
    if (bare && !add_name(names, name, length - 4)) {
      ok = false;
      break;
    }
  }
  free(name);
  handle_close(directory);
  return ok;
}

static int compare_names(const void *left, const void *right)
{
  return strcmp(*(char *const *)left, *(char *const *)right);
}

/* The word being typed ends at the cursor. It is completable only as the
 * command name of a pipeline stage: not a later argument, a redirection
 * target or anything inside quotes. Returns whether so, and where it starts. */
static bool command_word(const char *line, size_t cursor, size_t *start)
{
  bool in_word = false, in_quote = false, command_position = true, redirect_pending = false;
  bool word_completable = false;
  char quote = 0;
  size_t word_start = cursor;
  for (size_t i = 0; i < cursor; ++i) {
    char byte = line[i];
    if (in_quote) {
      in_quote = byte != quote;
      continue;
    }
    if (byte == ' ' || byte == '\t') {
      if (in_word && word_completable) {
        command_position = false;
      }
      in_word = false;
    } else if (byte == '|') {
      in_word = false;
      command_position = true;
      redirect_pending = false;
    } else if (byte == '<' || byte == '>' || byte == '&') {
      in_word = false;
      redirect_pending = byte != '&';
    } else {
      if (!in_word) {
        in_word = true;
        word_start = i;
        word_completable = command_position && !redirect_pending;
        if (redirect_pending) {
          redirect_pending = false;
        }
      }
      if (byte == '\'' || byte == '"') {
        in_quote = true;
        quote = byte;
      }
    }
  }
  if (in_quote) {
    return false;
  }
  if (in_word) {
    *start = word_start;
    return word_completable;
  }
  *start = cursor;
  return command_position && !redirect_pending;
}

bool shell_complete(void *context, const char *line, size_t cursor,
    struct term_candidates *result)
{
  struct shell *shell = context;
  size_t start;
  if (!command_word(line, cursor, &start)) {
    return false;
  }
  const char *prefix = line + start;
  size_t prefix_length = cursor - start;
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
  ok = ok && add_programs(shell, "bin://", prefix, prefix_length, &names) &&
      add_programs(shell, "boot://", prefix, prefix_length, &names);

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
