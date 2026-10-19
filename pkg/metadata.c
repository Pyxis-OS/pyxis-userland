#include "metadata.h"

#include <literal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool alphanumeric(unsigned char byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
      (byte >= '0' && byte <= '9');
}

bool pkg_version_valid(const char *version)
{
  size_t length = strnlen(version, PACKAGE_VERSION_MAX + 1);
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

static bool fields(const struct literal_value *value, const char *const *names,
    size_t count)
{
  if (!value || value->kind != LITERAL_TABLE || value->as.table.count != count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!literal_field(value, names[i])) {
      return false;
    }
  }
  return true;
}

static bool text(const struct literal_value *table, const char *key,
    char *output, size_t capacity)
{
  const struct literal_value *value = literal_field(table, key);
  if (!value || value->kind != LITERAL_STRING || value->as.string.size >= capacity ||
      memchr(value->as.string.bytes, 0, value->as.string.size)) {
    return false;
  }
  memcpy(output, value->as.string.bytes, value->as.string.size);
  output[value->as.string.size] = 0;
  return true;
}

static bool integer(const struct literal_value *table, const char *key,
    uint64_t minimum, uint64_t maximum, uint64_t *output)
{
  const struct literal_value *value = literal_field(table, key);
  if (!value || value->kind != LITERAL_INTEGER || value->as.integer < 0 ||
      (uint64_t)value->as.integer < minimum || (uint64_t)value->as.integer > maximum) {
    return false;
  }
  *output = (uint64_t)value->as.integer;
  return true;
}

static bool format(const struct literal_value *table)
{
  uint64_t number;
  return integer(table, "format", 1, 1, &number);
}

static bool identity(const struct literal_value *table, struct pkg_identity *output)
{
  return text(table, "id", output->id, sizeof(output->id)) &&
      package_id_valid(output->id) &&
      text(table, "version", output->version, sizeof(output->version)) &&
      pkg_version_valid(output->version);
}

static bool kind(const struct literal_value *table, char output[10])
{
  return text(table, "kind", output, 10) &&
      (!strcmp(output, "data") || !strcmp(output, "toolchain"));
}

static bool hash(const struct literal_value *table, const char *key, char output[65])
{
  if (!text(table, key, output, 65) || strlen(output) != 64) {
    return false;
  }
  for (size_t i = 0; i < 64; ++i) {
    if (!((output[i] >= '0' && output[i] <= '9') ||
        (output[i] >= 'a' && output[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool array(const struct literal_value *value)
{
  if (!value || value->kind != LITERAL_TABLE) {
    return false;
  }
  int64_t index = 1;
  for (const struct literal_entry *entry = value->as.table.entries; entry;
      entry = entry->next, ++index) {
    if (entry->key->kind != LITERAL_INTEGER || entry->key->as.integer != index) {
      return false;
    }
  }
  return true;
}

bool pkg_index_parse(const char *bytes, size_t size, struct pkg_entry **entries,
    size_t *count, const char **reason)
{
  *entries = NULL;
  *count = 0;
  *reason = "invalid repository index";
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  if (!literal_parse(bytes, size, &document, &diagnostic)) {
    return false;
  }
  static const char *const root_fields[] = { "format", "packages" };
  static const char *const entry_fields[] = {
    "id", "version", "kind", "archive", "size", "sha256",
  };
  const struct literal_value *packages = literal_field(document->root, "packages");
  bool ok = fields(document->root, root_fields, 2) && format(document->root) && array(packages);
  size_t length = ok ? packages->as.table.count : 0;
  struct pkg_entry *result = length ? calloc(length, sizeof(*result)) : NULL;
  if (length && !result) {
    *reason = "out of memory reading repository index";
    ok = false;
  }
  size_t index = 0;
  for (const struct literal_entry *item = ok ? packages->as.table.entries : NULL;
      ok && item; item = item->next, ++index) {
    struct pkg_entry *entry = &result[index];
    const struct literal_value *value = item->value;
    ok = fields(value, entry_fields, 6) && identity(value, &entry->identity) &&
        kind(value, entry->kind) && text(value, "archive", entry->archive, sizeof(entry->archive)) &&
        integer(value, "size", 1, INT64_MAX, &entry->size) && hash(value, "sha256", entry->sha256);
    if (!ok) {
      break;
    }
    char expected[PKG_ARCHIVE_PATH_MAX + 1];
    snprintf(expected, sizeof(expected), "packages/%s/%s.tar", entry->identity.id, entry->identity.version);
    ok = !strcmp(expected, entry->archive);
    for (size_t previous = 0; ok && previous < index; ++previous) {
      ok = strcmp(result[previous].identity.id, entry->identity.id) ||
          strcmp(result[previous].identity.version, entry->identity.version);
    }
  }
  literal_free(document);
  if (!ok) {
    free(result);
    return false;
  }
  *entries = result;
  *count = length;
  *reason = NULL;
  return true;
}

void pkg_manifest_free(struct pkg_manifest *manifest)
{
  free(manifest->dependencies);
  memset(manifest, 0, sizeof(*manifest));
}

bool pkg_manifest_parse(const char *bytes, size_t size,
    struct pkg_manifest *manifest, const char **reason)
{
  memset(manifest, 0, sizeof(*manifest));
  *reason = "invalid package manifest";
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  if (!literal_parse(bytes, size, &document, &diagnostic)) {
    return false;
  }
  static const char *const manifest_fields[] = {
    "format", "id", "version", "kind", "depends", "files", "bytes", "inventory_sha256",
  };
  static const char *const dependency_fields[] = { "id", "version" };
  const struct literal_value *root = document->root;
  const struct literal_value *dependencies = literal_field(root, "depends");
  bool ok = fields(root, manifest_fields, 8) && format(root) &&
      identity(root, &manifest->identity) && kind(root, manifest->kind) &&
      integer(root, "files", 1, PKG_MAX_FILES, &manifest->files) &&
      integer(root, "bytes", 0, INT64_MAX, &manifest->bytes) &&
      hash(root, "inventory_sha256", manifest->inventory_sha256) && array(dependencies);
  manifest->dependency_count = ok ? dependencies->as.table.count : 0;
  if (manifest->dependency_count) {
    manifest->dependencies = calloc(manifest->dependency_count, sizeof(*manifest->dependencies));
    if (!manifest->dependencies) {
      *reason = "out of memory reading dependencies";
      ok = false;
    }
  }
  size_t index = 0;
  for (const struct literal_entry *item = ok ? dependencies->as.table.entries : NULL;
      ok && item; item = item->next, ++index) {
    struct pkg_identity *dependency = &manifest->dependencies[index];
    ok = fields(item->value, dependency_fields, 2) && identity(item->value, dependency) &&
        strcmp(dependency->id, manifest->identity.id);
    for (size_t previous = 0; ok && previous < index; ++previous) {
      ok = strcmp(manifest->dependencies[previous].id, dependency->id) ||
          strcmp(manifest->dependencies[previous].version, dependency->version);
    }
  }
  literal_free(document);
  if (!ok) {
    pkg_manifest_free(manifest);
    return false;
  }
  *reason = NULL;
  return true;
}
