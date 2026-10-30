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
/* current is one small table; a set record lists at most a few thousand
 * packages, well inside the literal parser's own bounds. */
#define CURRENT_LIMIT 256
#define SET_LIMIT LITERAL_SOURCE_LIMIT
#define SET_NAME_MAX 32

static bool alphanumeric(char byte)
{
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
      (byte >= 'A' && byte <= 'Z');
}

/* IDs that name roots and views a package must not be confused with:
 * today's fixed roots, and the pkg:// and sdk:// views (decision 53 of
 * docs/wip/sdk-view.md). */
static const char *const fixed_root_names[] = {
  "boot", "tmp", "bin", "system", "home", "acpi", "packages", "pkg", "sdk",
};

bool package_id_valid(const char *id)
{
  size_t length = strnlen(id, PACKAGE_ID_MAX + 1);
  if (!length || length > PACKAGE_ID_MAX || !alphanumeric(id[0]) ||
      (id[0] >= 'A' && id[0] <= 'Z')) {
    return false;
  }
  for (size_t i = 0; i < sizeof(fixed_root_names) / sizeof(*fixed_root_names); ++i) {
    if (!strcmp(id, fixed_root_names[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < length; ++i) {
    bool lower = (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9');
    if (!lower && id[i] != '.' && id[i] != '-') {
      return false;
    }
  }
  return true;
}

bool package_abi_valid(const char *abi)
{
  size_t length = strnlen(abi, PACKAGE_ABI_MAX + 1);
  if (!length || length > PACKAGE_ABI_MAX) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    bool lower = (abi[i] >= 'a' && abi[i] <= 'z') || (abi[i] >= '0' && abi[i] <= '9');
    if (!lower && (!i || (abi[i] != '.' && abi[i] != '-' && abi[i] != '+'))) {
      return false;
    }
  }
  return true;
}

static enum call_status read_os_release(char **bytes, size_t *size)
{
  *bytes = NULL;
  *size = 0;
  FILE *file = fopen(PACKAGE_OS_RELEASE, "rb");
  if (!file) {
    return CALL_NOT_FOUND;
  }
  enum call_status status = CALL_OK;
  size_t capacity = 0;
  for (;;) {
    if (*size == capacity) {
      /* One byte past the parser's limit shows the file is too large. */
      if (capacity > LITERAL_SOURCE_LIMIT) {
        status = CALL_FILE_TOO_LARGE;
        break;
      }
      size_t grown = capacity ? 2 * capacity : 512;
      if (grown > LITERAL_SOURCE_LIMIT + 1) {
        grown = LITERAL_SOURCE_LIMIT + 1;
      }
      char *buffer = realloc(*bytes, grown);
      if (!buffer) {
        status = CALL_NO_MEMORY;
        break;
      }
      *bytes = buffer;
      capacity = grown;
    }
    size_t count = fread(*bytes + *size, 1, capacity - *size, file);
    *size += count;
    if (!count) {
      status = ferror(file) ? CALL_IO : CALL_OK;
      break;
    }
  }
  fclose(file);
  if (status != CALL_OK) {
    free(*bytes);
    *bytes = NULL;
  }
  return status;
}

static bool abi_string(const struct literal_value *value)
{
  return value && value->kind == LITERAL_STRING &&
      strlen(value->as.string.bytes) == value->as.string.size &&
      package_abi_valid(value->as.string.bytes);
}

/* Recognize the release and development metadata shapes explicitly. */
static bool os_release_fields(const struct literal_value *root)
{
  if (root->kind != LITERAL_TABLE) {
    return false;
  }
  for (const struct literal_entry *entry = root->as.table.entries; entry; entry = entry->next) {
    const struct literal_value *key = entry->key;
    if (entry->implicit || key->kind != LITERAL_STRING ||
        (strcmp(key->as.string.bytes, "release") && strcmp(key->as.string.bytes, "version") &&
         strcmp(key->as.string.bytes, "upgrade_from") && strcmp(key->as.string.bytes, "revision") &&
         strcmp(key->as.string.bytes, "abi") && strcmp(key->as.string.bytes, "signed_floor"))) {
      return false;
    }
  }
  return true;
}

/* YYYY-MM-DDTHH:MM:SSZ, as scripts/os-release.lua writes it. */
static bool time_text(const char *text, size_t size)
{
  static const char shape[] = "dddd-dd-ddTdd:dd:ddZ";
  if (size != sizeof(shape) - 1 || strlen(text) != size) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    if (shape[i] == 'd' ? text[i] < '0' || text[i] > '9' : text[i] != shape[i]) {
      return false;
    }
  }
  return true;
}

static bool provide(struct package_provided_abi *provided, const char *name)
{
  char *copy = strdup(name);
  if (!copy) {
    return false;
  }
  provided->names[provided->count++] = copy;
  return true;
}

static bool provided_text(struct package_provided_abi *provided)
{
  size_t length = sizeof("no ABI");
  for (size_t i = 0; i < provided->count; ++i) {
    length += strlen(provided->names[i]) + 2;
  }
  provided->text = malloc(length);
  if (!provided->text) {
    return false;
  }
  strcpy(provided->text, provided->count ? "" : "no ABI");
  for (size_t i = 0; i < provided->count; ++i) {
    if (i) {
      strcat(provided->text, ", ");
    }
    strcat(provided->text, provided->names[i]);
  }
  return true;
}

enum call_status package_provided_abi(struct package_provided_abi *provided)
{
  *provided = (struct package_provided_abi){0};
  char *bytes;
  size_t size;
  enum call_status status = read_os_release(&bytes, &size);
  if (status != CALL_OK) {
    return status;
  }
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  bool parsed = literal_parse(bytes, size, &document, &diagnostic);
  free(bytes);
  if (!parsed) {
    return CALL_BAD_REQUEST;
  }
  const struct literal_value *list = literal_field(document->root, "abi");
  const struct literal_value *revision = literal_field(document->root, "revision");
  const struct literal_value *release = literal_field(document->root, "release");
  const struct literal_value *version = literal_field(document->root, "version");
  const struct literal_value *anchor = literal_field(document->root, "upgrade_from");
  const struct literal_value *floor = literal_field(document->root, "signed_floor");
  status = os_release_fields(document->root) && list && list->kind == LITERAL_TABLE &&
      revision && revision->kind == LITERAL_STRING &&
      (!revision->as.string.size || abi_string(revision)) &&
      ((abi_string(release) && !strchr(release->as.string.bytes, '+') && !version && !anchor) ||
       (!release && abi_string(version) && strchr(version->as.string.bytes, '+') &&
        abi_string(anchor) && !strchr(anchor->as.string.bytes, '+'))) ?
      CALL_OK : CALL_BAD_REQUEST;
  size_t count = status == CALL_OK ? list->as.table.count : 0;
  if (status == CALL_OK && count &&
      !(provided->names = calloc(count, sizeof(*provided->names)))) {
    status = CALL_NO_MEMORY;
  }
  for (const struct literal_entry *entry = status == CALL_OK ? list->as.table.entries : NULL;
      entry && status == CALL_OK; entry = entry->next) {
    if (!entry->implicit || !abi_string(entry->value) ||
        package_abi_provided(provided, entry->value->as.string.bytes)) {
      status = CALL_BAD_REQUEST;
    } else if (!provide(provided, entry->value->as.string.bytes)) {
      status = CALL_NO_MEMORY;
    }
  }
  if (status == CALL_OK && release) {
    memcpy(provided->release, release->as.string.bytes, release->as.string.size + 1);
  }
  if (status == CALL_OK) {
    const struct literal_value *identity = release ? release : version;
    memcpy(provided->version, identity->as.string.bytes, identity->as.string.size + 1);
    if (anchor) {
      memcpy(provided->upgrade_from, anchor->as.string.bytes, anchor->as.string.size + 1);
    }
  }
  if (status == CALL_OK && floor) {
    if (floor->kind != LITERAL_STRING || !time_text(floor->as.string.bytes, floor->as.string.size)) {
      status = CALL_BAD_REQUEST;
    } else {
      memcpy(provided->signed_floor, floor->as.string.bytes, floor->as.string.size + 1);
    }
  }
  if (status == CALL_OK && !provided_text(provided)) {
    status = CALL_NO_MEMORY;
  }
  literal_free(document);
  if (status != CALL_OK) {
    package_provided_abi_free(provided);
  }
  return status;
}

void package_provided_abi_free(struct package_provided_abi *provided)
{
  for (size_t i = 0; i < provided->count; ++i) {
    free(provided->names[i]);
  }
  free(provided->names);
  free(provided->text);
  *provided = (struct package_provided_abi){0};
}

bool package_abi_provided(const struct package_provided_abi *provided, const char *abi)
{
  for (size_t i = 0; i < provided->count; ++i) {
    if (!strcmp(provided->names[i], abi)) {
      return true;
    }
  }
  return false;
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

static enum call_status parse(const char *bytes, size_t size, struct literal **document)
{
  struct literal_diagnostic diagnostic;
  return literal_parse(bytes, size, document, &diagnostic) ? CALL_OK : CALL_BAD_REQUEST;
}

static bool string_key(const struct literal_entry *entry, const char *key)
{
  return entry->key->kind == LITERAL_STRING &&
      entry->key->as.string.size == strlen(key) &&
      !memcmp(entry->key->as.string.bytes, key, entry->key->as.string.size);
}

/* TABLE has exactly the named string keys, each once (duplicates already refuse). */
static bool exact_fields(const struct literal_value *table, const char *const *keys,
    size_t count)
{
  if (!table || table->kind != LITERAL_TABLE || table->as.table.count != count) {
    return false;
  }
  for (const struct literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
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

static bool text(const struct literal_value *value)
{
  return value && value->kind == LITERAL_STRING &&
      strlen(value->as.string.bytes) == value->as.string.size;
}

struct package_set {
  struct literal *record; /* NULL for the empty set. */
  uint64_t number;
};

/* Read current and its set record. NULL *set with CALL_OK is the empty set. */
static enum call_status load_record(handle_t packages, int64_t requested,
    struct literal **set, uint64_t *number, const char **reason)
{
  *set = NULL;
  *number = 0;
  *reason = NULL;
  char *bytes;
  size_t size;
  enum call_status status;
  if (requested < 0) {
    status = read_file(packages, CURRENT_NAME, CURRENT_LIMIT, &bytes, &size);
    if (status == CALL_NOT_FOUND) {
      return CALL_OK;
    }
    if (status != CALL_OK) {
      *reason = "cannot read current";
      return status;
    }
    struct literal *current = NULL;
    status = parse(bytes, size, &current);
    free(bytes);
    static const char *const current_keys[] = {"set"};
    if (status != CALL_OK || !exact_fields(current->root, current_keys, 1) ||
        literal_field(current->root, "set")->kind != LITERAL_INTEGER ||
        literal_field(current->root, "set")->as.integer < 1) {
      literal_free(current);
      *reason = "current is not a valid set pointer";
      return CALL_BAD_REQUEST;
    }
    requested = literal_field(current->root, "set")->as.integer;
    literal_free(current);
  }
  if (!requested) {
    return CALL_OK;
  }
  *number = requested;
  char name[SET_NAME_MAX];
  snprintf(name, sizeof(name), "%lld.lua", (long long)requested);

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
  struct literal *record;
  status = parse(bytes, size, &record);
  free(bytes);
  if (status != CALL_OK) {
    *reason = "the current set record is not literal data";
    return status;
  }

  static const char *const set_keys[] = {"packages"};
  static const char *const legacy_keys[] = {"id", "version"};
  static const char *const sourced_keys[] = {"id", "version", "origin", "origin_hash"};
  const struct literal_value *list = literal_field(record->root, "packages");
  bool valid = exact_fields(record->root, set_keys, 1) && list->kind == LITERAL_TABLE;
  int64_t index = 0;
  for (const struct literal_entry *entry = valid ? list->as.table.entries : NULL;
       valid && entry; entry = entry->next) {
    const struct literal_value *id = literal_field(entry->value, "id");
    const struct literal_value *version = literal_field(entry->value, "version");
    const struct literal_value *origin = literal_field(entry->value, "origin");
    const struct literal_value *hash = literal_field(entry->value, "origin_hash");
    valid = entry->key->kind == LITERAL_INTEGER && entry->key->as.integer == ++index &&
        (origin ? exact_fields(entry->value, sourced_keys, 4) :
            exact_fields(entry->value, legacy_keys, 2)) && text(id) && text(version) &&
        package_id_valid(id->as.string.bytes) &&
        version_valid(version->as.string.bytes, version->as.string.size);
    if (valid && origin) {
      valid = text(origin) && package_id_valid(origin->as.string.bytes) && text(hash) &&
          hash->as.string.size == 64;
      for (size_t h = 0; valid && h < 64; ++h) {
        char byte = hash->as.string.bytes[h];
        valid = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
      }
    }
    for (const struct literal_entry *prior = list->as.table.entries;
         valid && prior != entry; prior = prior->next) {
      valid = strcmp(literal_field(prior->value, "id")->as.string.bytes,
          id->as.string.bytes) != 0;
    }
  }
  if (!valid) {
    literal_free(record);
    *reason = "the current set record is invalid";
    return CALL_BAD_REQUEST;
  }
  *set = record;
  return CALL_OK;
}

enum call_status package_set_load(handle_t packages, struct package_set **set,
    const char **reason)
{
  *set = NULL;
  struct literal *record;
  uint64_t number;
  enum call_status status = load_record(packages, -1, &record, &number, reason);
  if (status != CALL_OK) {
    return status;
  }
  *set = calloc(1, sizeof(**set));
  if (!*set) {
    literal_free(record);
    *reason = "cannot allocate the package set";
    return CALL_NO_MEMORY;
  }
  (*set)->record = record;
  (*set)->number = number;
  return CALL_OK;
}

enum call_status package_set_load_number(handle_t packages, uint64_t number,
    struct package_set **set, const char **reason)
{
  *set = NULL;
  if (number > INT64_MAX) {
    *reason = "set number is out of range";
    return CALL_LIMIT;
  }
  struct literal *record;
  uint64_t selected;
  enum call_status status = load_record(packages, number, &record, &selected, reason);
  if (status != CALL_OK) {
    return status;
  }
  *set = calloc(1, sizeof(**set));
  if (!*set) {
    literal_free(record);
    *reason = "cannot allocate the package set";
    return CALL_NO_MEMORY;
  }
  (*set)->record = record;
  (*set)->number = selected;
  return CALL_OK;
}

uint64_t package_set_number(const struct package_set *set)
{
  return set->number;
}

bool package_set_entry(const struct package_set *set, size_t index,
    const char **id, const char **version)
{
  const struct literal_value *list = set->record ?
      literal_field(set->record->root, "packages") : NULL;
  const struct literal_entry *entry = list ? list->as.table.entries : NULL;
  for (size_t i = 0; entry && i < index; ++i) {
    entry = entry->next;
  }
  if (!entry) {
    return false;
  }
  *id = literal_field(entry->value, "id")->as.string.bytes;
  *version = literal_field(entry->value, "version")->as.string.bytes;
  return true;
}

bool package_set_origin(const struct package_set *set, size_t index,
    const char **name, const char **fingerprint)
{
  *name = NULL;
  *fingerprint = NULL;
  const struct literal_value *list = set->record ?
      literal_field(set->record->root, "packages") : NULL;
  const struct literal_entry *entry = list ? list->as.table.entries : NULL;
  for (size_t i = 0; entry && i < index; ++i) entry = entry->next;
  if (!entry) return false;
  const struct literal_value *origin = literal_field(entry->value, "origin");
  if (origin) {
    *name = origin->as.string.bytes;
    *fingerprint = literal_field(entry->value, "origin_hash")->as.string.bytes;
  }
  return true;
}

size_t package_set_count(const struct package_set *set)
{
  return set->record ? literal_field(set->record->root, "packages")->as.table.count : 0;
}

void package_set_free(struct package_set *set)
{
  if (set) {
    literal_free(set->record);
    free(set);
  }
}
