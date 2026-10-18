#include "catalog.h"
#include "ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_LIMIT 4096

const char *config_file_name(enum config_file file)
{
  static const char *names[] = {"boot", "session", "network", "limine"};
  return (unsigned)file < CONFIG_FILE_COUNT ? names[file] : "unknown";
}

bool config_entry_id_valid(const char *bytes, size_t size)
{
  if (!size || size > CONFIG_ENTRY_ID_LIMIT) return false;
  for (size_t i = 0; i < size; ++i) {
    char byte = bytes[i];
    bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9');
    if (!alphanumeric && (i == 0 || (byte != '_' && byte != '-' && byte != '.' && byte != ':'))) {
      return false;
    }
  }
  return true;
}

static bool error(const char *name, const char *id, const char *reason)
{
  fprintf(stderr, "catalog: %s: %s%s%s\n", name, id ? id : "",
      id ? ": " : "", reason);
  return false;
}

static bool fields(const struct config_literal_value *value,
    const char *const *keys, size_t count)
{
  if (!value || value->kind != CONFIG_LITERAL_TABLE) return false;
  for (const struct config_literal_entry *entry = value->as.table.entries; entry; entry = entry->next) {
    if (entry->key->kind != CONFIG_LITERAL_STRING) return false;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      size_t size = strlen(keys[i]);
      if (entry->key->as.string.size == size && !memcmp(entry->key->as.string.bytes, keys[i], size)) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

static char *string(const struct config_literal_value *value, size_t limit)
{
  if (!value || value->kind != CONFIG_LITERAL_STRING || !value->as.string.size ||
      value->as.string.size > limit || memchr(value->as.string.bytes, '\0', value->as.string.size)) {
    return NULL;
  }
  char *copy = malloc(value->as.string.size + 1);
  if (!copy) return NULL;
  memcpy(copy, value->as.string.bytes, value->as.string.size);
  copy[value->as.string.size] = '\0';
  return copy;
}

static bool file_value(const struct config_literal_value *value, enum config_file *file)
{
  if (!value || value->kind != CONFIG_LITERAL_STRING) return false;
  for (unsigned i = 0; i < CONFIG_FILE_COUNT; ++i) {
    const char *name = config_file_name(i);
    if (strlen(name) == value->as.string.size && !memcmp(name, value->as.string.bytes, strlen(name))) {
      *file = i;
      return true;
    }
  }
  return false;
}

static bool kind_value(const struct config_literal_value *value, enum config_entry_kind *kind)
{
  static const char *names[] = {"volume", "space", "setting", "limine"};
  if (!value || value->kind != CONFIG_LITERAL_STRING) return false;
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (strlen(names[i]) == value->as.string.size &&
        !memcmp(names[i], value->as.string.bytes, strlen(names[i]))) {
      *kind = i;
      return true;
    }
  }
  return false;
}

static bool compatible(enum config_file file, enum config_entry_kind kind)
{
  if (file == CONFIG_FILE_BOOT) return kind == CONFIG_ENTRY_VOLUME || kind == CONFIG_ENTRY_SPACE;
  if (file == CONFIG_FILE_LIMINE) return kind == CONFIG_ENTRY_LIMINE;
  return kind == CONFIG_ENTRY_SETTING;
}

static bool default_valid(const struct config_catalog_entry *entry,
    const struct config_literal_value *value)
{
  if (!value) return false;
  if (entry->kind == CONFIG_ENTRY_VOLUME || entry->kind == CONFIG_ENTRY_SPACE) {
    if (value->kind != CONFIG_LITERAL_TABLE) return false;
    if (entry->kind == CONFIG_ENTRY_SPACE) {
      const struct config_literal_value *name = config_literal_field(value, "name");
      if (!name || name->kind != CONFIG_LITERAL_STRING || name->as.string.size != strlen(entry->name) ||
          memcmp(name->as.string.bytes, entry->name, name->as.string.size)) return false;
    }
  }
  if (entry->kind == CONFIG_ENTRY_LIMINE && (value->kind != CONFIG_LITERAL_STRING ||
      !config_limine_entry_valid(entry->name, value->as.string.bytes, value->as.string.size))) return false;
  return true;
}

static bool is_word(const struct config_literal_value *value, const char *word)
{
  return value && value->kind == CONFIG_LITERAL_STRING && value->as.string.size == strlen(word) &&
      !memcmp(value->as.string.bytes, word, value->as.string.size);
}

static bool migration(const struct config_literal_value *value, struct config_catalog_entry *entry)
{
  if (!value) return true;
  const struct config_literal_value *op = config_literal_field(value, "op");
  if (is_word(op, "replace")) {
    const char *keys[] = {"op", "value"};
    entry->migration.kind = CONFIG_MIGRATION_REPLACE;
    entry->migration.value = config_literal_field(value, "value");
    if (!fields(value, keys, 2) || !default_valid(entry, entry->migration.value)) return false;
    enum config_literal_kind a = entry->default_value->kind, b = entry->migration.value->kind;
    bool numeric = (a == CONFIG_LITERAL_NUMBER || a == CONFIG_LITERAL_INTEGER) &&
        (b == CONFIG_LITERAL_NUMBER || b == CONFIG_LITERAL_INTEGER);
    return a == b || numeric;
  }
  if (is_word(op, "move")) {
    const char *keys[] = {"op", "from_file", "from_kind", "from_name"};
    entry->migration.kind = CONFIG_MIGRATION_MOVE;
    entry->migration.from_name = string(config_literal_field(value, "from_name"), NAME_LIMIT);
    return fields(value, keys, 4) && entry->migration.from_name &&
        file_value(config_literal_field(value, "from_file"), &entry->migration.from_file) &&
        kind_value(config_literal_field(value, "from_kind"), &entry->migration.from_kind) &&
        compatible(entry->migration.from_file, entry->migration.from_kind);
  }
  if (is_word(op, "rename-field")) {
    const char *keys[] = {"op", "from", "to"};
    entry->migration.kind = CONFIG_MIGRATION_RENAME_FIELD;
    entry->migration.from = string(config_literal_field(value, "from"), NAME_LIMIT);
    entry->migration.to = string(config_literal_field(value, "to"), NAME_LIMIT);
    return fields(value, keys, 3) && entry->default_value->kind == CONFIG_LITERAL_TABLE &&
        entry->migration.from && entry->migration.to &&
        strcmp(entry->migration.from, entry->migration.to);
  }
  return false;
}

void config_catalog_free(struct config_catalog *catalog)
{
  if (!catalog) return;
  for (size_t i = 0; i < catalog->count; ++i) {
    free(catalog->entries[i].id);
    free(catalog->entries[i].name);
    free(catalog->entries[i].migration.from_name);
    free(catalog->entries[i].migration.from);
    free(catalog->entries[i].migration.to);
  }
  free(catalog->entries);
  free(catalog->catalog_id);
  free(catalog->revision);
  config_literal_free(catalog->document);
  free(catalog);
}

static bool decode(const char *name, struct config_catalog *catalog)
{
  const struct config_literal_value *root = catalog->document->root;
  const char *root_keys[] = {"catalog_id", "revision", "entries"};
  if (!fields(root, root_keys, 3)) return error(name, NULL, "unknown catalog field or invalid structure");
  catalog->catalog_id = string(config_literal_field(root, "catalog_id"), NAME_LIMIT);
  catalog->revision = string(config_literal_field(root, "revision"), 63);
  if (!catalog->catalog_id) {
    return error(name, NULL, "catalog_id must be a nonempty string of at most 4096 bytes");
  }
  if (!catalog->revision || !strcmp(catalog->revision, ".") || !strcmp(catalog->revision, "..")) {
    return error(name, NULL, "revision must be a nonempty installer component of at most 63 bytes");
  }
  for (size_t i = 0; catalog->revision[i]; ++i) {
    unsigned char byte = (unsigned char)catalog->revision[i];
    if (byte < 33 || byte > 126 || byte == '/') {
      return error(name, NULL, "revision must match the installer's printable component grammar");
    }
  }
  const struct config_literal_value *entries = config_literal_field(root, "entries");
  if (!entries || entries->kind != CONFIG_LITERAL_TABLE || entries->as.table.count > CONFIG_CATALOG_ENTRY_LIMIT) {
    return error(name, NULL, "entries must be an array of at most 128 declarations");
  }
  catalog->entries = calloc(entries->as.table.count ? entries->as.table.count : 1, sizeof(*catalog->entries));
  if (!catalog->entries) return error(name, NULL, "cannot allocate catalog entries");
  for (const struct config_literal_entry *source = entries->as.table.entries; source; source = source->next) {
    bool valid_index = source->key->kind == CONFIG_LITERAL_INTEGER && source->key->as.integer >= 1 &&
        (unsigned long long)source->key->as.integer <= entries->as.table.count;
    if (source->key->kind == CONFIG_LITERAL_NUMBER) {
      double index = source->key->as.number;
      valid_index = index >= 1 && index <= entries->as.table.count && (double)(size_t)index == index;
    }
    if (!valid_index) {
      return error(name, NULL, "entries must be a dense integer array");
    }
    struct config_catalog_entry *entry = &catalog->entries[catalog->count++];
    const struct config_literal_value *value = source->value;
    const char *keys[] = {"id", "file", "kind", "name", "required", "default", "migration"};
    if (!fields(value, keys, 7)) return error(name, NULL, "unknown declaration field or invalid structure");
    entry->id = string(config_literal_field(value, "id"), CONFIG_ENTRY_ID_LIMIT);
    entry->name = string(config_literal_field(value, "name"), NAME_LIMIT);
    if (!entry->id || !config_entry_id_valid(entry->id, strlen(entry->id))) {
      return error(name, NULL, "entry ID must be a stable ASCII label of at most 64 bytes");
    }
    if (!entry->name || !file_value(config_literal_field(value, "file"), &entry->file) ||
        !kind_value(config_literal_field(value, "kind"), &entry->kind) || !compatible(entry->file, entry->kind)) {
      return error(name, entry->id, "invalid fixed file/kind target or missing name");
    }
    const struct config_literal_value *required = config_literal_field(value, "required");
    if (!required || required->kind != CONFIG_LITERAL_BOOLEAN) {
      return error(name, entry->id, "required must be a boolean");
    }
    entry->required = required->as.boolean;
    entry->default_value = config_literal_field(value, "default");
    if (!default_valid(entry, entry->default_value)) {
      return error(name, entry->id, "default must be a complete value matching the target kind/name");
    }
    if (!migration(config_literal_field(value, "migration"), entry)) {
      return error(name, entry->id, "unsupported or malformed declarative migration");
    }
    for (size_t i = 0; i + 1 < catalog->count; ++i) {
      const struct config_catalog_entry *prior = &catalog->entries[i];
      if (!strcmp(prior->id, entry->id) || (prior->file == entry->file && prior->kind == entry->kind &&
          !strcmp(prior->name, entry->name))) {
        return error(name, entry->id, "duplicate ID or target");
      }
    }
  }
  return true;
}

bool config_catalog_read(const char *name, const char *bytes, size_t size,
    struct config_catalog **output)
{
  *output = NULL;
  struct config_catalog *catalog = calloc(1, sizeof(*catalog));
  if (!catalog) return error(name, NULL, "cannot allocate catalog");
  struct config_literal_diagnostic diagnostic;
  if (!config_literal_parse(bytes, size, &catalog->document, &diagnostic)) {
    fprintf(stderr, "catalog: %s:%zu:%zu: %s\n", name, diagnostic.line,
        diagnostic.column, diagnostic.reason);
    config_catalog_free(catalog);
    return false;
  }
  if (!decode(name, catalog)) {
    config_catalog_free(catalog);
    return false;
  }
  *output = catalog;
  return true;
}

const struct config_catalog_entry *config_catalog_find(
    const struct config_catalog *catalog, const char *id)
{
  if (!catalog) return NULL;
  for (size_t i = 0; i < catalog->count; ++i) {
    if (!strcmp(catalog->entries[i].id, id)) return &catalog->entries[i];
  }
  return NULL;
}
