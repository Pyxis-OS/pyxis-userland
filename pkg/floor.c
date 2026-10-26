#include "floor.h"
#include <literal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOOR_MAX_BYTES 16384

static bool field_text(const struct literal_value *table, const char *key,
    char *target, size_t capacity)
{
  const struct literal_value *value = literal_field(table, key);
  if (!value || value->kind != LITERAL_STRING || !value->as.string.size ||
      value->as.string.size >= capacity ||
      strlen(value->as.string.bytes) != value->as.string.size) {
    return false;
  }
  memcpy(target, value->as.string.bytes, value->as.string.size + 1);
  return true;
}

static bool floor_parse(const struct literal_value *value,
    struct pkg_repository_floor *floor)
{
  if (value->kind != LITERAL_TABLE || value->as.table.count != 6 ||
      !field_text(value, "name", floor->name, sizeof(floor->name)) ||
      !package_id_valid(floor->name) ||
      !field_text(value, "url", floor->url, sizeof(floor->url)) ||
      !pkg_repository_url_valid(floor->url) ||
      !field_text(value, "signed_at", floor->signed_at, sizeof(floor->signed_at)) ||
      !pkg_time_valid(floor->signed_at) ||
      !field_text(value, "sha256", floor->sha256, sizeof(floor->sha256))) {
    return false;
  }
  for (size_t i = 0; i < 64; ++i) {
    if ((floor->sha256[i] < '0' || floor->sha256[i] > '9') &&
        (floor->sha256[i] < 'a' || floor->sha256[i] > 'f')) {
      return false;
    }
  }
  const struct literal_value *signed_index = literal_field(value, "signed");
  const struct literal_value *serial = literal_field(value, "serial");
  if (!signed_index || signed_index->kind != LITERAL_BOOLEAN ||
      !signed_index->as.boolean || !serial || serial->kind != LITERAL_INTEGER ||
      serial->as.integer < 1 || (uint64_t)serial->as.integer > PKG_SERIAL_MAX) {
    return false;
  }
  floor->serial = (uint64_t)serial->as.integer;
  return true;
}

bool pkg_repository_floors_load(struct pkg_repository_floors *floors,
    const char **reason)
{
  *floors = (struct pkg_repository_floors){0};
  *reason = NULL;
  FILE *file = fopen(PKG_REPOSITORY_FLOORS, "rb");
  if (!file) {
    if (errno == ENOENT) {
      return true;
    }
    *reason = "the embedded repository floor cannot be opened";
    return false;
  }
  char bytes[FLOOR_MAX_BYTES + 1];
  size_t size = fread(bytes, 1, sizeof(bytes), file);
  bool read = !ferror(file) && size <= FLOOR_MAX_BYTES;
  fclose(file);
  if (!read) {
    *reason = "the embedded repository floor is unreadable or too large";
    return false;
  }
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  bool valid = literal_parse(bytes, size, &document, &diagnostic);
  const struct literal_value *root = valid ? document->root : NULL;
  const struct literal_value *version = valid ? literal_field(root, "version") : NULL;
  const struct literal_value *list = valid ? literal_field(root, "repositories") : NULL;
  valid = valid && root->kind == LITERAL_TABLE && root->as.table.count == 2 &&
      version && version->kind == LITERAL_INTEGER && version->as.integer == 1 &&
      list && list->kind == LITERAL_TABLE && list->as.table.count <= PKG_REPOSITORIES_MAX;
  for (const struct literal_entry *entry = valid ? list->as.table.entries : NULL;
       valid && entry; entry = entry->next) {
    struct pkg_repository_floor *floor = &floors->entries[floors->count];
    valid = entry->key->kind == LITERAL_INTEGER &&
        entry->key->as.integer == (int64_t)floors->count + 1 &&
        floor_parse(entry->value, floor);
    for (size_t i = 0; valid && i < floors->count; ++i) {
      valid = strcmp(floors->entries[i].name, floor->name) &&
          strcmp(floors->entries[i].url, floor->url);
    }
    if (valid) {
      ++floors->count;
    }
  }
  literal_free(document);
  if (!valid) {
    *floors = (struct pkg_repository_floors){0};
    *reason = "the embedded repository floor is not a bounded version-1 signed table";
  }
  return valid;
}

const struct pkg_repository_floor *pkg_repository_floor_find(
    const struct pkg_repository_floors *floors, const struct pkg_repository *repository)
{
  for (size_t i = 0; i < floors->count; ++i) {
    if (!strcmp(floors->entries[i].name, repository->name) &&
        !strcmp(floors->entries[i].url, repository->url)) {
      return &floors->entries[i];
    }
  }
  return NULL;
}
