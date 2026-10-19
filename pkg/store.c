#include "store.h"
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <package.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool pkg_directory(handle_t parent, const char *name, bool create, uint64_t rights,
    handle_t *directory, const char **reason)
{
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      rights, directory);
  if (status == CALL_NOT_FOUND && create) {
    status = directory_create(parent, name, DIRECTORY_KIND_DIRECTORY, rights, directory);
  }
  if (status != CALL_OK) {
    *reason = "cannot open required package directory";
    return false;
  }
  return true;
}

bool pkg_text(handle_t parent, const char *name, size_t limit, char **bytes,
    size_t *size, const char **reason)
{
  *bytes = NULL;
  *size = 0;
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  uint64_t length = 0;
  if (status == CALL_OK) {
    status = file_size(file, &length);
  }
  if (status == CALL_OK && length > limit) {
    status = CALL_LIMIT;
  }
  char *buffer = status == CALL_OK ? malloc((size_t)length + 1) : NULL;
  if (status == CALL_OK && !buffer) {
    status = CALL_NO_MEMORY;
  }
  size_t offset = 0;
  while (status == CALL_OK && offset < length) {
    size_t count = 0;
    status = file_read(file, offset, buffer + offset, length - offset, &count);
    if (status == CALL_OK && !count) {
      status = CALL_IO;
    }
    offset += count;
  }
  if (file != HANDLE_INVALID) {
    handle_close(file);
  }
  if (status != CALL_OK) {
    free(buffer);
    *reason = "cannot read bounded package metadata";
    return false;
  }
  buffer[length] = 0;
  *bytes = buffer;
  *size = length;
  return true;
}

bool pkg_write_text(handle_t parent, const char *name, const void *bytes, size_t size,
    const char **reason)
{
  handle_t file;
  enum call_status status = directory_create(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_WRITE, &file);
  if (status != CALL_OK) {
    *reason = "cannot exclusively create package record";
    return false;
  }
  for (size_t offset = 0; status == CALL_OK && offset < size;) {
    size_t count = 0;
    status = file_write(file, offset, (const unsigned char *)bytes + offset,
        size - offset, &count);
    if (status == CALL_OK && !count) {
      status = CALL_IO;
    }
    offset += count;
  }
  if (status == CALL_OK) {
    status = file_sync(file);
  }
  handle_close(file);
  if (status != CALL_OK) {
    *reason = "cannot synchronize package record";
    return false;
  }
  return true;
}

void pkg_selection_free(struct pkg_selection *selection)
{
  free(selection->entries);
  *selection = (struct pkg_selection){0};
}

bool pkg_selection_load(handle_t packages, bool current, uint64_t number,
    struct pkg_selection *selection, const char **reason)
{
  *selection = (struct pkg_selection){0};
  struct package_set *set = NULL;
  enum call_status status = current ? package_set_load(packages, &set, reason) :
      package_set_load_number(packages, number, &set, reason);
  if (status != CALL_OK) {
    return false;
  }
  selection->count = package_set_count(set);
  selection->number = package_set_number(set);
  selection->capacity = selection->count ? selection->count : 1;
  selection->entries = calloc(selection->capacity,
      sizeof(*selection->entries));
  if (!selection->entries) {
    package_set_free(set);
    *selection = (struct pkg_selection){0};
    *reason = "cannot allocate package selection";
    return false;
  }
  for (size_t i = 0; i < selection->count; ++i) {
    const char *id, *version;
    package_set_entry(set, i, &id, &version);
    strcpy(selection->entries[i].id, id);
    strcpy(selection->entries[i].version, version);
  }
  package_set_free(set);
  return true;
}

bool pkg_predecessor(handle_t packages, uint64_t number, uint64_t *previous,
    const char **reason)
{
  if (!number) {
    *previous = 0;
    return true;
  }
  handle_t sets;
  if (!pkg_directory(packages, "sets", false, PKG_READ_RIGHTS, &sets, reason)) {
    return false;
  }
  char name[48];
  snprintf(name, sizeof(name), "%llu.previous", (unsigned long long)number);
  char *bytes;
  size_t size;
  bool ok = pkg_text(sets, name, 256, &bytes, &size, reason);
  handle_close(sets);
  if (!ok) {
    return false;
  }
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  ok = literal_parse(bytes, size, &document, &diagnostic);
  free(bytes);
  const struct literal_value *format = ok ? literal_field(document->root, "format") : NULL;
  const struct literal_value *value = ok ? literal_field(document->root, "previous") : NULL;
  ok = ok && document->root->kind == LITERAL_TABLE &&
      document->root->as.table.count == 2 && format && value && format->kind == LITERAL_INTEGER &&
      format->as.integer == 1 && value->kind == LITERAL_INTEGER &&
      value->as.integer >= 0 && (uint64_t)value->as.integer < number;
  if (ok) {
    *previous = value->as.integer;
  } else {
    *reason = "invalid predecessor record; use explicit pkg rollback SET";
  }
  literal_free(document);
  return ok;
}

static bool next_number(handle_t sets, uint64_t *number, const char **reason)
{
  *number = 1;
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply reply;
    if (directory_enumerate(sets, &cursor, name, sizeof(name), &reply) != CALL_OK ||
        (reply.outcome != DIRECTORY_ENTRY && reply.outcome != DIRECTORY_END)) {
      *reason = "cannot enumerate immutable set records";
      return false;
    }
    if (reply.outcome == DIRECTORY_END) {
      return true;
    }
    cursor = reply.cursor;
    const char *tail = name;
    uint64_t value = 0;
    while (*tail >= '0' && *tail <= '9') {
      unsigned digit = *tail++ - '0';
      if (value > ((uint64_t)INT64_MAX - 1 - digit) / 10) {
        *reason = "set number exhausted";
        return false;
      }
      value = value * 10 + digit;
    }
    if (tail == name || name[0] == '0' ||
        (strcmp(tail, ".lua") && strcmp(tail, ".previous") && strcmp(tail, ".commands"))) {
      *reason = "unexpected name in sets directory";
      return false;
    }
    if (value >= *number) {
      *number = value + 1;
    }
  }
}

static void *command_bytes(uint64_t number, const struct package_command_record *commands,
    size_t command_count, size_t *size, const char **reason)
{
  if (command_count > PACKAGE_COMMANDS_LIMIT) {
    *reason = "command projection exceeds its record limit";
    return NULL;
  }
  *size = sizeof(struct package_command_index_header) + command_count * sizeof(*commands);
  struct package_command_index_header *header = calloc(1, *size);
  if (!header) {
    *reason = "cannot allocate immutable command index";
    return NULL;
  }
  memcpy(header->magic, PACKAGE_COMMAND_INDEX_MAGIC, sizeof(header->magic));
  header->set = number;
  header->count = command_count;
  header->record_size = sizeof(*commands);
  if (command_count) {
    memcpy(header + 1, commands, command_count * sizeof(*commands));
  }
  return header;
}

static bool compare_commands(handle_t sets, uint64_t number, const void *expected,
    size_t expected_size, const char **reason)
{
  char name[48];
  snprintf(name, sizeof(name), "%llu.commands", (unsigned long long)number);
  char *actual = NULL;
  size_t actual_size = 0;
  bool ok = pkg_text(sets, name, sizeof(struct package_command_index_header) +
      PACKAGE_COMMANDS_LIMIT * sizeof(struct package_command_record),
      &actual, &actual_size, reason);
  if (ok && (actual_size != expected_size || memcmp(actual, expected, expected_size))) {
    *reason = "command index differs from verified selected exports";
    ok = false;
  }
  free(actual);
  return ok;
}

static bool write_commands(handle_t sets, uint64_t number,
    const struct package_command_record *commands, size_t command_count, const char **reason)
{
  size_t size;
  void *bytes = command_bytes(number, commands, command_count, &size, reason);
  if (!bytes) {
    return false;
  }
  char name[48];
  snprintf(name, sizeof(name), "%llu.commands", (unsigned long long)number);
  bool ok = pkg_write_text(sets, name, bytes, size, reason) &&
      compare_commands(sets, number, bytes, size, reason);
  free(bytes);
  return ok;
}

bool pkg_commands_verify(handle_t packages, uint64_t number,
    const struct package_command_record *commands, size_t command_count, const char **reason)
{
  if (!number) {
    return command_count == 0;
  }
  handle_t sets;
  if (!pkg_directory(packages, "sets", false, PKG_READ_RIGHTS, &sets, reason)) {
    return false;
  }
  size_t size;
  void *bytes = command_bytes(number, commands, command_count, &size, reason);
  bool ok = bytes && compare_commands(sets, number, bytes, size, reason);
  free(bytes);
  handle_close(sets);
  return ok;
}

bool pkg_reindex(handle_t packages, uint64_t number,
    const struct package_command_record *commands, size_t command_count, const char **reason)
{
  if (!number) {
    return command_count == 0;
  }
  handle_t sets;
  if (!pkg_directory(packages, "sets", false, PKG_WRITE_RIGHTS, &sets, reason)) {
    return false;
  }
  char name[48];
  snprintf(name, sizeof(name), "%llu.commands", (unsigned long long)number);
  handle_t existing = HANDLE_INVALID;
  enum call_status status = directory_lookup(sets, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &existing);
  if (existing != HANDLE_INVALID) handle_close(existing);
  bool ok = status == CALL_NOT_FOUND;
  if (!ok) {
    *reason = "reindex requires a missing active command index; existing damage is not repaired";
  }
  ok = ok && write_commands(sets, number, commands, command_count, reason);
  if (ok && directory_sync(sets) != CALL_OK) {
    *reason = "cannot synchronize bootstrapped command index";
    ok = false;
  }
  handle_close(sets);
  return ok;
}

bool pkg_publish(handle_t packages, const struct pkg_selection *selection,
    uint64_t previous, const struct package_command_record *commands,
    size_t command_count, bool *uncertain, const char **reason)
{
  *uncertain = false;
  handle_t sets = HANDLE_INVALID;
  if (!pkg_directory(packages, "sets", true, PKG_WRITE_RIGHTS, &sets, reason)) {
    return false;
  }
  uint64_t number;
  bool ok = next_number(sets, &number, reason);
  size_t capacity = 64 + selection->count * 180;
  char *bytes = ok && capacity <= LITERAL_SOURCE_LIMIT ? malloc(capacity) : NULL;
  if (!bytes) {
    if (ok) {
      *reason = "set record exceeds metadata budget or available memory";
    }
    handle_close(sets);
    return false;
  }
  size_t used = snprintf(bytes, capacity, "return { format = 1, packages = {\n");
  for (size_t i = 0; i < selection->count; ++i) {
    used += snprintf(bytes + used, capacity - used,
        "  { id = \"%s\", version = \"%s\" },\n", selection->entries[i].id,
        selection->entries[i].version);
  }
  used += snprintf(bytes + used, capacity - used, "} }\n");
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  ok = literal_parse(bytes, used, &document, &diagnostic);
  literal_free(document);
  if (!ok) {
    *reason = "new set exceeds literal parser budget";
  }
  char name[48];
  snprintf(name, sizeof(name), "%llu.lua", (unsigned long long)number);
  ok = ok && pkg_write_text(sets, name, bytes, used, reason);
  free(bytes);
  char predecessor[96];
  int length = snprintf(predecessor, sizeof(predecessor),
      "return { format = 1, previous = %llu }\n", (unsigned long long)previous);
  snprintf(name, sizeof(name), "%llu.previous", (unsigned long long)number);
  ok = ok && pkg_write_text(sets, name, predecessor, length, reason);
  ok = ok && write_commands(sets, number, commands, command_count, reason);
  if (ok && directory_sync(sets) != CALL_OK) {
    *reason = "cannot synchronize immutable set names";
    ok = false;
  }
  handle_close(sets);
  if (!ok) {
    return false;
  }
  enum call_status status = directory_remove(packages, "current.new", DIRECTORY_KIND_FILE);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    *reason = "cannot remove an unpublished current.new";
    return false;
  }
  char pointer[96];
  length = snprintf(pointer, sizeof(pointer), "return { format = 1, set = %llu }\n",
      (unsigned long long)number);
  if (!pkg_write_text(packages, "current.new", pointer, length, reason) ||
      directory_sync(packages) != CALL_OK) {
    *reason = "cannot synchronize current.new";
    return false;
  }
  *uncertain = true;
  status = directory_rename(packages, "current.new", packages, "current",
      DIRECTORY_RENAME_REPLACE, DIRECTORY_KIND_FILE);
  if (status != CALL_OK || directory_sync(packages) != CALL_OK) {
    *reason = "activation uncertain; retain evidence until recovery";
    return false;
  }
  *uncertain = false;
  printf("pkg: active set %llu (%zu packages)\n", (unsigned long long)number,
      selection->count);
  return true;
}

bool pkg_tree_open(handle_t packages, const struct pkg_identity *identity,
    uint64_t rights, handle_t *tree, const char **reason)
{
  *tree = HANDLE_INVALID;
  handle_t store, id;
  if (!pkg_directory(packages, "store", false, rights, &store, reason)) {
    return false;
  }
  bool ok = pkg_directory(store, identity->id, false, rights, &id, reason);
  handle_close(store);
  if (!ok) {
    return false;
  }
  ok = pkg_directory(id, identity->version, false, rights, tree, reason);
  handle_close(id);
  return ok;
}
