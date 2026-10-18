#include <package.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURRENT_NAME "current"
#define SETS_NAME "sets"
#define STORE_NAME "store"
#define TREE_NAME "tree"
#define FORMAT 1
/* current is one small table; a set record lists at most a few thousand
 * packages, well inside the literal parser's own bounds. */
#define CURRENT_LIMIT 256
#define SET_LIMIT CONFIG_LITERAL_SOURCE_LIMIT
#define SET_NAME_MAX 32

static bool alphanumeric(char byte)
{
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
      (byte >= 'A' && byte <= 'Z');
}

bool package_id_valid(const char *id)
{
  size_t length = strnlen(id, PACKAGE_ID_MAX + 1);
  if (!length || length > PACKAGE_ID_MAX || !alphanumeric(id[0]) ||
      (id[0] >= 'A' && id[0] <= 'Z')) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    bool lower = (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9');
    if (!lower && id[i] != '.' && id[i] != '-') {
      return false;
    }
  }
  return true;
}

static bool version_valid(const char *version, size_t length)
{
  if (!length || length > PACKAGE_VERSION_MAX || !alphanumeric(version[0])) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!alphanumeric(version[i]) && version[i] != '.' && version[i] != '+' &&
        version[i] != '-') {
      return false;
    }
  }
  return true;
}

/* Read all of NAME under DIRECTORY, refusing files over LIMIT. */
static enum call_status read_file(handle_t directory, const char *name, size_t limit,
    char **bytes, size_t *size)
{
  *bytes = NULL;
  handle_t file;
  enum call_status status = directory_lookup(directory, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t length;
  status = file_size(file, &length);
  if (status == CALL_OK && length > limit) {
    status = CALL_FILE_TOO_LARGE;
  }
  char *buffer = status == CALL_OK ? malloc(length ? length : 1) : NULL;
  if (status == CALL_OK && !buffer) {
    status = CALL_NO_MEMORY;
  }
  size_t used = 0;
  while (status == CALL_OK && used < length) {
    size_t count;
    status = file_read(file, used, buffer + used, length - used, &count);
    if (status == CALL_OK && !count) {
      status = CALL_IO;
    }
    used += status == CALL_OK ? count : 0;
  }
  handle_close(file);
  if (status != CALL_OK) {
    free(buffer);
    return status;
  }
  *bytes = buffer;
  *size = used;
  return CALL_OK;
}

static enum call_status parse(const char *bytes, size_t size, struct config_literal **document)
{
  struct config_literal_diagnostic diagnostic;
  return config_literal_parse(bytes, size, document, &diagnostic) ? CALL_OK : CALL_BAD_REQUEST;
}

static bool string_key(const struct config_literal_entry *entry, const char *key)
{
  return entry->key->kind == CONFIG_LITERAL_STRING &&
      entry->key->as.string.size == strlen(key) &&
      !memcmp(entry->key->as.string.bytes, key, entry->key->as.string.size);
}

/* TABLE has exactly the named string keys, each once (duplicates already refuse). */
static bool exact_fields(const struct config_literal_value *table, const char *const *keys,
    size_t count)
{
  if (!table || table->kind != CONFIG_LITERAL_TABLE || table->as.table.count != count) {
    return false;
  }
  for (const struct config_literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    bool known = false;
    for (size_t i = 0; i < count && !known; ++i) {
      known = string_key(entry, keys[i]);
    }
    if (!known) {
      return false;
    }
  }
  return true;
}

static bool format_valid(const struct config_literal_value *table)
{
  const struct config_literal_value *format = config_literal_field(table, "format");
  return format->kind == CONFIG_LITERAL_INTEGER && format->as.integer == FORMAT;
}

static bool text(const struct config_literal_value *value)
{
  return value && value->kind == CONFIG_LITERAL_STRING &&
      strlen(value->as.string.bytes) == value->as.string.size;
}

/* Read current and its set record. NULL *set with CALL_OK is the empty set. */
static enum call_status load_set(handle_t packages, struct config_literal **set,
    const char **reason)
{
  *set = NULL;
  char *bytes;
  size_t size;
  enum call_status status = read_file(packages, CURRENT_NAME, CURRENT_LIMIT, &bytes, &size);
  if (status == CALL_NOT_FOUND) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    *reason = "cannot read current";
    return status;
  }
  struct config_literal *current;
  status = parse(bytes, size, &current);
  free(bytes);
  static const char *const current_keys[] = {"format", "set"};
  if (status != CALL_OK || !exact_fields(current->root, current_keys, 2) ||
      !format_valid(current->root) ||
      config_literal_field(current->root, "set")->kind != CONFIG_LITERAL_INTEGER ||
      config_literal_field(current->root, "set")->as.integer < 1) {
    config_literal_free(current);
    *reason = "current is not a valid set pointer";
    return CALL_BAD_REQUEST;
  }
  char name[SET_NAME_MAX];
  snprintf(name, sizeof(name), "%lld.lua",
      (long long)config_literal_field(current->root, "set")->as.integer);
  config_literal_free(current);

  handle_t sets;
  status = directory_lookup(packages, SETS_NAME, DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &sets);
  if (status == CALL_OK) {
    status = read_file(sets, name, SET_LIMIT, &bytes, &size);
    handle_close(sets);
  }
  if (status != CALL_OK) {
    *reason = "cannot read the current set record";
    return status;
  }
  struct config_literal *record;
  status = parse(bytes, size, &record);
  free(bytes);
  if (status != CALL_OK) {
    *reason = "the current set record is not literal data";
    return status;
  }

  static const char *const set_keys[] = {"format", "packages"};
  static const char *const package_keys[] = {"id", "version"};
  const struct config_literal_value *list = config_literal_field(record->root, "packages");
  bool valid = exact_fields(record->root, set_keys, 2) && format_valid(record->root) &&
      list->kind == CONFIG_LITERAL_TABLE;
  int64_t index = 0;
  for (const struct config_literal_entry *entry = valid ? list->as.table.entries : NULL;
       valid && entry; entry = entry->next) {
    const struct config_literal_value *id = config_literal_field(entry->value, "id");
    const struct config_literal_value *version = config_literal_field(entry->value, "version");
    valid = entry->key->kind == CONFIG_LITERAL_INTEGER && entry->key->as.integer == ++index &&
        exact_fields(entry->value, package_keys, 2) && text(id) && text(version) &&
        package_id_valid(id->as.string.bytes) &&
        version_valid(version->as.string.bytes, version->as.string.size);
    for (const struct config_literal_entry *prior = list->as.table.entries;
         valid && prior != entry; prior = prior->next) {
      valid = strcmp(config_literal_field(prior->value, "id")->as.string.bytes,
          id->as.string.bytes) != 0;
    }
  }
  if (!valid) {
    config_literal_free(record);
    *reason = "the current set record is invalid";
    return CALL_BAD_REQUEST;
  }
  *set = record;
  return CALL_OK;
}

enum call_status package_set_check(handle_t packages, size_t *count, const char **reason)
{
  struct config_literal *set;
  enum call_status status = load_set(packages, &set, reason);
  if (status == CALL_OK) {
    *count = set ? config_literal_field(set->root, "packages")->as.table.count : 0;
    config_literal_free(set);
  }
  return status;
}

/* Replace an owned intermediate *DIRECTORY with its child NAME. */
static enum call_status lookup_directory(handle_t *directory, const char *name, uint64_t rights)
{
  handle_t child;
  enum call_status status = directory_lookup(*directory, name, DIRECTORY_KIND_DIRECTORY,
      rights, &child);
  handle_close(*directory);
  *directory = child;
  return status;
}

enum call_status package_open_tree(handle_t packages, const char *id, uint64_t rights,
    handle_t *tree, const char **reason)
{
  *tree = HANDLE_INVALID;
  if (!package_id_valid(id) || !rights || (rights & ~PACKAGE_TREE_RIGHTS)) {
    *reason = "invalid package request";
    return CALL_BAD_REQUEST;
  }
  struct config_literal *set;
  enum call_status status = load_set(packages, &set, reason);
  if (status != CALL_OK) {
    return status;
  }
  const char *version = NULL;
  for (const struct config_literal_entry *entry = set ?
         config_literal_field(set->root, "packages")->as.table.entries : NULL;
       entry && !version; entry = entry->next) {
    if (!strcmp(config_literal_field(entry->value, "id")->as.string.bytes, id)) {
      version = config_literal_field(entry->value, "version")->as.string.bytes;
    }
  }
  if (!version) {
    config_literal_free(set);
    *reason = "the package is not installed";
    return CALL_NOT_FOUND;
  }

  /* Each step keeps RIGHTS plus LOOKUP so the tree can still carry RIGHTS. */
  uint64_t path_rights = rights | DIRECTORY_RIGHT_LOOKUP;
  handle_t directory;
  status = directory_lookup(packages, STORE_NAME, DIRECTORY_KIND_DIRECTORY, path_rights,
      &directory);
  if (status == CALL_OK) {
    status = lookup_directory(&directory, id, path_rights);
  }
  if (status == CALL_OK) {
    status = lookup_directory(&directory, version, path_rights);
  }
  if (status == CALL_OK) {
    status = lookup_directory(&directory, TREE_NAME, rights);
  }
  config_literal_free(set);
  if (status != CALL_OK) {
    *reason = status == CALL_NOT_FOUND ? "the package's store tree is missing" :
        "cannot open the package's store tree";
    return status;
  }
  *tree = directory;
  return CALL_OK;
}
