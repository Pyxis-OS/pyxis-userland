#include "store.h"
#include <abi/package_sdk.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <package.h>
#include <package_commands.h>
#include <package_transition.h>
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
  free(selection->origins);
  *selection = (struct pkg_selection){0};
}

bool pkg_selection_load(handle_t packages,
    struct pkg_selection *selection, const char **reason)
{
  *selection = (struct pkg_selection){0};
  struct package_set *set = NULL;
  enum call_status status = package_set_load(packages, &set, reason);
  if (status != CALL_OK) {
    return false;
  }
  selection->count = package_set_count(set);
  selection->number = package_set_number(set);
  selection->capacity = selection->count ? selection->count : 1;
  selection->entries = calloc(selection->capacity,
      sizeof(*selection->entries));
  selection->origins = calloc(selection->capacity, sizeof(*selection->origins));
  if (!selection->entries || !selection->origins) {
    package_set_free(set);
    pkg_selection_free(selection);
    *reason = "cannot allocate package selection";
    return false;
  }
  for (size_t i = 0; i < selection->count; ++i) {
    const char *id, *version;
    package_set_entry(set, i, &id, &version);
    strcpy(selection->entries[i].id, id);
    strcpy(selection->entries[i].version, version);
    const char *name, *fingerprint;
    package_set_origin(set, i, &name, &fingerprint);
    if (name) {
      strcpy(selection->origins[i].name, name);
      strcpy(selection->origins[i].fingerprint, fingerprint);
    }
  }
  package_set_free(set);
  return true;
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
        (strcmp(tail, ".lua") && strcmp(tail, ".previous") && strcmp(tail, ".commands") &&
         strcmp(tail, ".sdk"))) {
      *reason = "unexpected name in sets directory";
      return false;
    }
    if (value >= *number) {
      *number = value + 1;
    }
  }
}

static void *command_bytes(uint64_t number, const struct pkg_projection *projection,
    size_t *size, const char **reason)
{
  size_t count = projection->commands + projection->roots;
  /* The record counts are 32-bit fields of the header; the kernel's memory
   * budget is checked by pkg_indexes_fit before anything is written. */
  if (projection->commands > UINT32_MAX || projection->roots > UINT32_MAX ||
      count > (SIZE_MAX - sizeof(struct package_command_index_header)) /
          sizeof(*projection->records)) {
    *reason = "the set has more packages or commands than the index format counts";
    return NULL;
  }
  *size = sizeof(struct package_command_index_header) + count * sizeof(*projection->records);
  struct package_command_index_header *header = calloc(1, *size);
  if (!header) {
    *reason = "cannot allocate immutable command index";
    return NULL;
  }
  memcpy(header->magic, PACKAGE_COMMAND_INDEX_MAGIC, sizeof(header->magic));
  header->set = number;
  header->count = projection->commands;
  header->roots = projection->roots;
  header->record_size = sizeof(*projection->records);
  if (count) {
    memcpy(header + 1, projection->records, count * sizeof(*projection->records));
  }
  return header;
}

/* The set file NUMBER followed by SUFFIX must hold exactly EXPECTED. */
static bool compare_index(handle_t sets, uint64_t number, const char *suffix,
    const void *expected, size_t expected_size, const char **reason)
{
  char name[48];
  snprintf(name, sizeof(name), "%llu%s", (unsigned long long)number, suffix);
  char *actual = NULL;
  size_t actual_size = 0;
  bool ok = pkg_text(sets, name, expected_size, &actual, &actual_size, reason);
  if (ok && (actual_size != expected_size || memcmp(actual, expected, expected_size))) {
    *reason = !strcmp(suffix, ".sdk") ? "sdk index differs from the selected dev packages" :
        "command index differs from verified selected exports";
    ok = false;
  }
  free(actual);
  return ok;
}

static bool write_index(handle_t sets, uint64_t number, const char *suffix,
    const void *bytes, size_t size, const char **reason)
{
  char name[48];
  snprintf(name, sizeof(name), "%llu%s", (unsigned long long)number, suffix);
  return pkg_write_text(sets, name, bytes, size, reason) &&
      compare_index(sets, number, suffix, bytes, size, reason);
}

/* SDK's bytes with set NUMBER filled in. */
static void *sdk_bytes(uint64_t number, const struct pkg_sdk *sdk, const char **reason)
{
  struct package_sdk_index_header *header = malloc(sdk->size);
  if (!header) {
    *reason = "cannot allocate the sdk index";
    return NULL;
  }
  memcpy(header, sdk->bytes, sdk->size);
  header->set = number;
  return header;
}

/* The kernel keeps the command index and the sdk index of the active set in
 * memory, within its package-view budget (one 512th of installed RAM, at least
 * 4 MiB); there is no count or file-size constant. A set whose indexes cannot
 * fit is refused here with the numbers, before the review, never after a
 * confirmed change. COMMANDS and SDK say which indexes would be written. */
static bool indexes_fit(handle_t packages, const struct pkg_projection *projection,
    const struct pkg_sdk *sdk, bool commands, bool sdk_index, const char **reason)
{
  uint64_t command_bytes = commands ? sizeof(struct package_command_index_header) +
      ((uint64_t)projection->commands + projection->roots) * sizeof(*projection->records) : 0;
  uint64_t sdk_bytes = sdk_index ? sdk->size : 0;
  if (!command_bytes && !sdk_bytes) {
    return true;
  }
  handle_t live;
  struct package_command_info info;
  enum call_status status = package_commands_open(packages, &live);
  if (status == CALL_OK) {
    status = package_commands_info(live, &info);
    handle_close(live);
  }
  if (status != CALL_OK) {
    *reason = "cannot read the kernel's package-view budget";
    return false;
  }
  if (command_bytes + sdk_bytes > info.budget) {
    static char text[320];
    snprintf(text, sizeof(text),
        "command and sdk indexes of the new set: %llu bytes (%zu commands, %zu packages with files, "
        "%llu sdk bytes) exceed the limit of %llu (kernel package-view memory, one 512th of "
        "installed RAM)", (unsigned long long)(command_bytes + sdk_bytes), projection->commands,
        projection->roots, (unsigned long long)sdk_bytes, (unsigned long long)info.budget);
    *reason = text;
    return false;
  }
  return true;
}

bool pkg_indexes_fit(handle_t packages, const struct pkg_projection *projection,
    const struct pkg_sdk *sdk, const char **reason)
{
  return indexes_fit(packages, projection, sdk, true, true, reason);
}

/* Write set NUMBER's command index when COMMANDS and its sdk index when
 * SDK_MISSING, reading each back. */
static bool write_indexes(handle_t sets, uint64_t number, bool commands, bool sdk_missing,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk, const char **reason)
{
  size_t size;
  void *bytes = commands ? command_bytes(number, projection, &size, reason) : NULL;
  bool ok = !commands || (bytes && write_index(sets, number, ".commands", bytes, size, reason));
  free(bytes);
  bytes = ok && sdk_missing ? sdk_bytes(number, sdk, reason) : NULL;
  ok = ok && (!sdk_missing || (bytes && write_index(sets, number, ".sdk", bytes, sdk->size, reason)));
  free(bytes);
  return ok;
}

bool pkg_indexes_verify(handle_t packages, uint64_t number,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk, const char **reason)
{
  if (!number) {
    return !projection->commands && !projection->roots;
  }
  handle_t sets;
  if (!pkg_directory(packages, "sets", false, PKG_READ_RIGHTS, &sets, reason)) {
    return false;
  }
  size_t size;
  void *bytes = command_bytes(number, projection, &size, reason);
  bool ok = bytes && compare_index(sets, number, ".commands", bytes, size, reason);
  free(bytes);
  bytes = ok ? sdk_bytes(number, sdk, reason) : NULL;
  ok = ok && bytes && compare_index(sets, number, ".sdk", bytes, sdk->size, reason);
  free(bytes);
  handle_close(sets);
  return ok;
}

static bool index_missing(handle_t sets, uint64_t number, const char *suffix, bool *missing,
    const char **reason)
{
  char name[48];
  snprintf(name, sizeof(name), "%llu%s", (unsigned long long)number, suffix);
  handle_t existing = HANDLE_INVALID;
  enum call_status status = directory_lookup(sets, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &existing);
  if (existing != HANDLE_INVALID) handle_close(existing);
  *missing = status == CALL_NOT_FOUND;
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    *reason = "cannot look up the active set's indexes";
    return false;
  }
  return true;
}

bool pkg_reindex(handle_t packages, uint64_t number,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk, const char **reason)
{
  if (!number) {
    return !projection->commands && !projection->roots;
  }
  handle_t sets;
  if (!pkg_directory(packages, "sets", false, PKG_WRITE_RIGHTS, &sets, reason)) {
    return false;
  }
  bool commands = false, sdk_missing = false;
  bool ok = index_missing(sets, number, ".commands", &commands, reason) &&
      index_missing(sets, number, ".sdk", &sdk_missing, reason);
  if (ok && !commands && !sdk_missing) {
    *reason = "reindex requires a missing active index; existing damage is not repaired";
    ok = false;
  }
  ok = ok && indexes_fit(packages, projection, sdk, commands, sdk_missing, reason);
  ok = ok && write_indexes(sets, number, commands, sdk_missing, projection, sdk, reason);
  if (ok && directory_sync(sets) != CALL_OK) {
    *reason = "cannot synchronize bootstrapped indexes";
    ok = false;
  }
  handle_close(sets);
  return ok;
}

bool pkg_stage(handle_t packages, const struct pkg_selection *selection,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk,
    uint64_t *staged, const char **reason)
{
  *staged = 0;
  handle_t sets = HANDLE_INVALID;
  if (!pkg_directory(packages, "sets", true, PKG_WRITE_RIGHTS, &sets, reason)) {
    return false;
  }
  uint64_t number;
  bool ok = next_number(sets, &number, reason) &&
      indexes_fit(packages, projection, sdk, true, true, reason);
  size_t capacity = 64;
  for (size_t i = 0; ok && i < selection->count; ++i) {
    const struct pkg_identity *identity = &selection->entries[i];
    const struct pkg_origin *origin = &selection->origins[i];
    size_t extra = 80 + strlen(identity->id) + strlen(identity->version) +
        strlen(origin->name) + strlen(origin->fingerprint);
    capacity += extra;
  }
  char *bytes = ok ? malloc(capacity) : NULL;
  if (!bytes) {
    if (ok) {
      static char text[160];
      snprintf(text, sizeof(text), "the set record of %zu packages needs %zu bytes: out of memory",
          selection->count, capacity);
      *reason = text;
    }
    handle_close(sets);
    return false;
  }
  size_t used = snprintf(bytes, capacity, "return { packages = {\n");
  for (size_t i = 0; i < selection->count; ++i) {
    const struct pkg_origin *origin = &selection->origins[i];
    int count;
    if (origin->name[0]) {
      count = snprintf(bytes + used, capacity - used,
          "  { id = \"%s\", version = \"%s\", origin = \"%s\", origin_hash = \"%s\" },\n",
          selection->entries[i].id, selection->entries[i].version, origin->name,
          origin->fingerprint);
    } else {
      count = snprintf(bytes + used, capacity - used,
          "  { id = \"%s\", version = \"%s\" },\n", selection->entries[i].id,
          selection->entries[i].version);
    }
    if (count < 0 || (size_t)count >= capacity - used) {
      *reason = "internal error: the set record outgrew its computed size";
      free(bytes);
      handle_close(sets);
      return false;
    }
    used += (size_t)count;
  }
  int tail = snprintf(bytes + used, capacity - used, "} }\n");
  if (tail < 0 || (size_t)tail >= capacity - used) {
    *reason = "internal error: the set record outgrew its computed size";
    free(bytes);
    handle_close(sets);
    return false;
  }
  used += (size_t)tail;
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  ok = literal_parse_sized(bytes, used, &document, &diagnostic);
  literal_free(document);
  if (!ok) {
    static char text[256];
    char described[160];
    snprintf(text, sizeof(text), "internal error: the new set record does not parse: %s",
        literal_describe(&diagnostic, described, sizeof(described)));
    *reason = text;
  }
  char name[48];
  snprintf(name, sizeof(name), "%llu.lua", (unsigned long long)number);
  ok = ok && pkg_write_text(sets, name, bytes, used, reason);
  free(bytes);
  ok = ok && write_indexes(sets, number, true, true, projection, sdk, reason);
  if (ok && directory_sync(sets) != CALL_OK) {
    *reason = "cannot synchronize immutable set names";
    ok = false;
  }
  handle_close(sets);
  if (!ok) {
    return false;
  }
  *staged = number;
  return true;
}

bool pkg_publish(handle_t packages, const struct pkg_selection *selection,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk,
    bool *uncertain, const char **reason)
{
  *uncertain = false;
  bool marked = false, pending = false;
  uint64_t marked_set = 0;
  struct package_boot_identity identity;
  struct package_transition transition;
  enum call_status status = package_transition_load(packages, &pending, &transition);
  if (status == CALL_OK && !pending) {
    status = package_boot_load(packages, &marked, &marked_set, &identity);
  }
  if (status != CALL_OK || pending || (marked && marked_set != selection->number)) {
    *reason = "package boot selection evidence disagrees with current; use rescue";
    return false;
  }
  uint64_t number;
  if (!pkg_stage(packages, selection, projection, sdk, &number, reason)) return false;
  *uncertain = true;
  if (marked) {
    /* Durable intent lets boot-init finish either side of the pointer switch. */
    transition = (struct package_transition){.from_set = selection->number,
      .to_set = number, .target = identity};
    status = package_transition_publish(packages, &transition);
  }
  if (status == CALL_OK) status = package_set_activate(packages, number);
  if (status == CALL_OK && marked) status = package_boot_publish(packages, number, &identity);
  if (status == CALL_OK && marked) status = package_transition_remove(packages);
  if (status != CALL_OK) {
    *reason = "activation uncertain; retain lock and boot evidence until recovery";
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
