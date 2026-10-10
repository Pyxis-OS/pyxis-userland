#include "shell.h"
#include <directory.h>
#include <handle.h>
#include <provider.h>
#include <stdlib.h>
#include <string.h>

/* A directory with more entries than this is not completed: the list would be
 * unusable, and a prefix common to a partial set would be wrong. */
#define ENTRY_LIMIT 4096

struct text {
  char *data;
  size_t length, capacity;
};

static bool text_add(struct text *text, char byte)
{
  if (text->length + 1 >= text->capacity) {
    size_t capacity = text->capacity ? text->capacity * 2 : 32;
    char *larger = realloc(text->data, capacity);
    if (!larger) {
      return false;
    }
    text->data = larger;
    text->capacity = capacity;
  }
  text->data[text->length++] = byte;
  text->data[text->length] = '\0';
  return true;
}

static bool separator(char byte)
{
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' ||
         byte == '\f';
}

/* Reads the line up to the cursor the way parse_line does: operators end a
 * word, and a backslash or quote groups bytes into it. Words compacted into
 * one argument are one word here too. */
bool completion_scan(const char *line, size_t cursor, struct completion_word *word)
{
  struct text value = {.data = calloc(1, 32), .capacity = 32};
  if (!value.data) {
    return false;
  }

  bool in_word = false, command_position = true, redirect_pending = false, blocked = false;
  bool word_command = false, plain = true;
  char quote = 0;
  size_t start = cursor;
  bool ok = true;
  for (size_t i = 0; ok && i < cursor; ++i) {
    char byte = line[i];
    bool end_word = !quote && (separator(byte) || byte == '|' || byte == '<' || byte == '>' ||
                               byte == '&');
    if (end_word) {
      if (in_word && word_command) {
        command_position = false;
      }
      in_word = false;
      if (byte == '|') {
        command_position = true;
        redirect_pending = false;
      } else if (byte == '<' || byte == '>') {
        redirect_pending = true;
      } else if (byte == '&') {
        blocked = true;
      }
      continue;
    }
    if (!in_word) {
      in_word = true;
      start = i;
      word_command = command_position && !redirect_pending;
      redirect_pending = false;
      plain = true;
      quote = 0;
      value.length = 0;
      value.data[0] = '\0';
    }
    if (byte == '\\' && quote != '\'') {
      plain = false;
      if (i + 1 < cursor) {
        ok = text_add(&value, line[++i]);
      }
    } else if (quote && byte == quote) {
      plain = false;
      quote = 0;
    } else if (!quote && (byte == '\'' || byte == '"')) {
      plain = false;
      quote = byte;
    } else {
      ok = text_add(&value, byte);
    }
  }
  if (!ok || blocked) {
    free(value.data);
    return false;
  }
  if (!in_word) {
    start = cursor;
    word_command = command_position && !redirect_pending;
    plain = true;
    quote = 0;
    value.length = 0;
    value.data[0] = '\0';
  }
  *word = (struct completion_word){
    .start = start, .command = word_command, .plain = plain, .quote = quote, .value = value.data,
  };
  return true;
}

/* One value is the text of a whole path word, as the user means it. */
struct entry {
  char *value;
  bool directory;
};

struct entries {
  struct entry *items;
  size_t count, capacity;
};

static bool entries_add(struct entries *entries, const char *directory, size_t directory_length,
    const char *name, const char *suffix, bool is_directory)
{
  if (entries->count == entries->capacity) {
    size_t capacity = entries->capacity ? entries->capacity * 2 : 32;
    struct entry *larger = realloc(entries->items, capacity * sizeof(*larger));
    if (!larger) {
      return false;
    }
    entries->items = larger;
    entries->capacity = capacity;
  }
  size_t name_length = strlen(name);
  size_t suffix_length = strlen(suffix);
  char *value = malloc(directory_length + name_length + suffix_length + 1);
  if (!value) {
    return false;
  }
  memcpy(value, directory, directory_length);
  memcpy(value + directory_length, name, name_length);
  memcpy(value + directory_length + name_length, suffix, suffix_length + 1);
  entries->items[entries->count++] = (struct entry){value, is_directory};
  return true;
}

static void entries_free(struct entries *entries)
{
  for (size_t i = 0; i < entries->count; ++i) {
    free(entries->items[i].value);
  }
  free(entries->items);
}

/* A name can be offered when every byte is printable ASCII and the open quote
 * can hold it: single quotes have no way to write a single quote. */
static bool offerable(const char *name, char quote)
{
  for (const unsigned char *byte = (const unsigned char *)name; *byte; ++byte) {
    if (*byte < ' ' || *byte > '~' || (quote == '\'' && *byte == '\'')) {
      return false;
    }
  }
  return true;
}

static bool special_unquoted(char byte)
{
  return byte == ' ' || byte == '\'' || byte == '"' || byte == '\\' || byte == '|' ||
         byte == '<' || byte == '>' || byte == '&';
}

/* The value as typed in the word's quote state, starting with the open quote. */
static char *render(const char *value, char quote)
{
  size_t length = strlen(value);
  char *text = malloc(2 * length + 2);
  if (!text) {
    return NULL;
  }
  size_t used = 0;
  if (quote) {
    text[used++] = quote;
  }
  for (size_t i = 0; i < length; ++i) {
    bool escape = quote == 0 ? special_unquoted(value[i])
                             : quote == '"' && (value[i] == '"' || value[i] == '\\');
    if (escape) {
      text[used++] = '\\';
    }
    text[used++] = value[i];
  }
  text[used] = '\0';
  return text;
}

/* Lists DIRECTORY (empty for the working directory) into entries, keeping the
 * names that begin with PREFIX. Not listable, such as for a withheld ENUMERATE
 * right or a missing path, adds nothing. Returns false for too many entries or
 * no memory. */
static bool list_directory(struct shell *shell, const char *directory, const char *prefix,
    char quote, struct entries *entries)
{
  handle_t handle;
  if (shell_open_directory(shell, *directory ? directory : ".", DIRECTORY_RIGHT_ENUMERATE,
      &handle) != CALL_OK) {
    return true;
  }
  size_t directory_length = strlen(directory);
  size_t prefix_length = strlen(prefix);
  bool ok = true;
  size_t capacity = 128, seen = 0;
  char *name = malloc(capacity);
  struct directory_cursor cursor = {0};
  for (ok = name != NULL; ok;) {
    struct directory_enumerate_reply entry;
    if (directory_enumerate(handle, &cursor, name, capacity, &entry) != CALL_OK ||
        entry.outcome == DIRECTORY_END || entry.outcome == DIRECTORY_CHANGED) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      ok = larger != NULL;
      if (ok) {
        name = larger;
        capacity = entry.name_size;
      }
      continue;
    }
    cursor = entry.cursor;
    if (++seen > ENTRY_LIMIT) {
      ok = false;
      break;
    }
    bool is_directory = entry.kind == DIRECTORY_KIND_DIRECTORY;
    if (!strcmp(name, ".") || !strcmp(name, "..") || memcmp(name, prefix, prefix_length) ||
        (name[0] == '.' && prefix[0] != '.') || !offerable(name, quote)) {
      continue;
    }
    ok = entries_add(entries, directory, directory_length, name, is_directory ? "/" : "",
        is_directory);
  }
  free(name);
  handle_close(handle);
  return ok;
}

/* Root names that the value could still be spelling: home:// from h, home: or
 * home:/. A complete root:// is a directory to list, not a candidate. */
static bool add_roots(struct shell *shell, const char *value, char quote,
    struct entries *entries)
{
  size_t length = strlen(value);
  for (size_t i = 0; i < shell->directory.root_count; ++i) {
    const char *name = shell->roots[i].name;
    size_t name_length = strlen(name);
    size_t common = length < name_length ? length : name_length;
    if (length >= name_length + 3 || memcmp(name, value, common) ||
        (length > name_length && memcmp("://", value + name_length, length - name_length)) ||
        !offerable(name, quote)) {
      continue;
    }
    if (!entries_add(entries, "", 0, name, "://", true)) {
      return false;
    }
  }
  return true;
}

static int compare_entries(const void *left, const void *right)
{
  return strcmp(((const struct entry *)left)->value, ((const struct entry *)right)->value);
}

bool shell_complete_path(struct shell *shell, const struct completion_word *word,
    struct term_candidates *result)
{
  const char *value = word->value;
  if (provider_http_uri(value) || (word->quote == '\'' && strchr(value, '\''))) {
    return false;
  }
  const char *slash = strrchr(value, '/');
  size_t directory_length = slash ? (size_t)(slash - value) + 1 : 0;
  char *directory = malloc(directory_length + 1);
  if (!directory) {
    return false;
  }
  memcpy(directory, value, directory_length);
  directory[directory_length] = '\0';
  const char *prefix = value + directory_length;

  struct entries entries = {0};
  bool ok = list_directory(shell, directory, prefix, word->quote, &entries);
  free(directory);
  ok = ok && add_roots(shell, value, word->quote, &entries);
  if (!ok || !entries.count) {
    entries_free(&entries);
    return false;
  }

  qsort(entries.items, entries.count, sizeof(*entries.items), compare_entries);
  size_t unique = 0;
  for (size_t i = 0; i < entries.count; ++i) {
    if (unique && !strcmp(entries.items[unique - 1].value, entries.items[i].value)) {
      free(entries.items[i].value);
    } else {
      entries.items[unique++] = entries.items[i];
    }
  }

  char **names = calloc(unique, sizeof(*names));
  bool rendered = names != NULL;
  for (size_t i = 0; rendered && i < unique; ++i) {
    names[i] = render(entries.items[i].value, word->quote);
    rendered = names[i] != NULL;
  }
  bool directory_match = unique == 1 && entries.items[0].directory;
  entries.count = unique;
  entries_free(&entries);
  if (!rendered) {
    for (size_t i = 0; names && i < unique; ++i) {
      free(names[i]);
    }
    free(names);
    return false;
  }

  /* A lone file ends the word; a lone directory stays open for the next Tab. */
  const char *finish = NULL;
  if (directory_match) {
    finish = "";
  } else if (word->quote) {
    finish = word->quote == '"' ? "\" " : "' ";
  }
  *result = (struct term_candidates){
    .names = names, .count = unique, .start = word->start, .finish = finish,
  };
  return true;
}
