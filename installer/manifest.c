#include "manifest.h"
#include "digest.h"
#include "installer.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_TAG "PYXIS-REVISION-MANIFEST 1\n"
#define TREE_PATH_BYTES ((INSTALL_TREE_MAX_DEPTH + 1) * \
    (INSTALL_TREE_NAME_MAX + 1))
#define SOURCE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

static const char hex[] = "0123456789abcdef";

static bool component_valid(const char *name, size_t bytes)
{
  return bytes && bytes <= INSTALL_TREE_NAME_MAX &&
      !(bytes == 1 && name[0] == '.') &&
      !(bytes == 2 && name[0] == '.' && name[1] == '.') &&
      !memchr(name, '/', bytes) && !memchr(name, '\0', bytes);
}

static bool path_valid(const char *path, uint64_t kind, size_t *bytes)
{
  size_t length = strnlen(path, TREE_PATH_BYTES);
  if (!length || length == TREE_PATH_BYTES || !strcmp(path, INSTALL_MANIFEST_NAME)) {
    return false;
  }
  unsigned components = 0;
  for (size_t offset = 0; offset < length;) {
    size_t end = offset;
    while (end < length && path[end] != '/') {
      ++end;
    }
    if (!component_valid(path + offset, end - offset) || ++components >
        INSTALL_TREE_MAX_DEPTH + (kind == DIRECTORY_KIND_FILE)) {
      return false;
    }
    if (end + 1 == length) {
      return false;
    }
    offset = end + 1;
  }
  *bytes = length;
  return true;
}

static bool sha256_valid(const char *sha256)
{
  if (!sha256 || strnlen(sha256, 65) != 64) {
    return false;
  }
  for (size_t index = 0; index < 64; ++index) {
    if (!((sha256[index] >= '0' && sha256[index] <= '9') ||
        (sha256[index] >= 'a' && sha256[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static size_t decimal_bytes(uint64_t value)
{
  size_t bytes = 1;
  while (value >= 10) {
    value /= 10;
    ++bytes;
  }
  return bytes;
}

enum install_manifest_status install_manifest_init(struct install_manifest *manifest,
    const char *revision)
{
  *manifest = (struct install_manifest){0};
  if (!revision) {
    return INSTALL_MANIFEST_INVALID;
  }
  size_t bytes = strnlen(revision, sizeof(manifest->revision));
  if (bytes == sizeof(manifest->revision) || !component_valid(revision, bytes)) {
    return INSTALL_MANIFEST_INVALID;
  }
  memcpy(manifest->revision, revision, bytes + 1);
  manifest->text_bytes = sizeof(MANIFEST_TAG) - 1 + 3 + 2 * bytes;
  return INSTALL_MANIFEST_OK;
}

enum install_manifest_status install_manifest_add(struct install_manifest *manifest,
    const char *path, uint64_t kind, uint64_t size, const char *sha256)
{
  size_t length;
  if (!manifest->revision[0] || manifest->sealed || !path ||
      (kind != DIRECTORY_KIND_FILE && kind != DIRECTORY_KIND_DIRECTORY) ||
      !path_valid(path, kind, &length) ||
      (kind == DIRECTORY_KIND_DIRECTORY && (size || sha256)) ||
      (kind == DIRECTORY_KIND_FILE && !sha256_valid(sha256)) ||
      manifest->count >= INSTALL_TREE_MAX_ENTRIES - 1 ||
      size > UINT64_MAX - manifest->bytes) {
    return INSTALL_MANIFEST_INVALID;
  }
  size_t record_bytes = 2 * length + (kind == DIRECTORY_KIND_FILE ?
      69 + decimal_bytes(size) : 3);
  if (record_bytes > INSTALL_MANIFEST_MAX_BYTES - manifest->text_bytes) {
    return INSTALL_MANIFEST_INVALID;
  }
  if (manifest->count == manifest->capacity) {
    size_t capacity = manifest->capacity ? manifest->capacity * 2 : 64;
    if (capacity > INSTALL_TREE_MAX_ENTRIES - 1) {
      capacity = INSTALL_TREE_MAX_ENTRIES - 1;
    }
    struct install_manifest_entry *entries = realloc(manifest->entries,
        capacity * sizeof(*entries));
    if (!entries) {
      return INSTALL_MANIFEST_NO_MEMORY;
    }
    manifest->entries = entries;
    manifest->capacity = capacity;
  }
  char *copy = malloc(length + 1);
  if (!copy) {
    return INSTALL_MANIFEST_NO_MEMORY;
  }
  memcpy(copy, path, length + 1);
  struct install_manifest_entry *entry = &manifest->entries[manifest->count++];
  *entry = (struct install_manifest_entry){.path = copy, .kind = kind, .size = size};
  if (kind == DIRECTORY_KIND_FILE) {
    memcpy(entry->sha256, sha256, sizeof(entry->sha256));
    ++manifest->files;
  }
  manifest->bytes += size;
  manifest->text_bytes += record_bytes;
  return INSTALL_MANIFEST_OK;
}

static int compare_entries(const void *left, const void *right)
{
  const struct install_manifest_entry *a = left, *b = right;
  return strcmp(a->path, b->path);
}

/* Compare a complete record path against a borrowed prefix without changing
 * the owned record or allocating a temporary ancestor name. */
static int compare_prefix(const char *path, const char *prefix, size_t bytes)
{
  int result = strncmp(path, prefix, bytes);
  if (result) {
    return result;
  }
  return (unsigned char)path[bytes];
}

static const struct install_manifest_entry *find_entry(const struct install_manifest *manifest,
    const char *path, size_t bytes)
{
  size_t first = 0, end = manifest->count;
  while (first < end) {
    size_t middle = first + (end - first) / 2;
    int result = compare_prefix(manifest->entries[middle].path, path, bytes);
    if (!result) {
      return &manifest->entries[middle];
    }
    if (result < 0) {
      first = middle + 1;
    } else {
      end = middle;
    }
  }
  return NULL;
}

enum install_manifest_status install_manifest_seal(struct install_manifest *manifest)
{
  manifest->sealed = false;
  size_t revision_bytes = strnlen(manifest->revision, sizeof(manifest->revision));
  if (revision_bytes == sizeof(manifest->revision) ||
      !component_valid(manifest->revision, revision_bytes) ||
      manifest->count >= INSTALL_TREE_MAX_ENTRIES ||
      manifest->text_bytes > INSTALL_MANIFEST_MAX_BYTES) {
    return INSTALL_MANIFEST_INVALID;
  }
  size_t text_bytes = sizeof(MANIFEST_TAG) - 1 + 3 + 2 * revision_bytes, files = 0;
  uint64_t bytes = 0;
  for (size_t index = 0; index < manifest->count; ++index) {
    const struct install_manifest_entry *entry = &manifest->entries[index];
    size_t length;
    if (!entry->path ||
        (entry->kind != DIRECTORY_KIND_FILE && entry->kind != DIRECTORY_KIND_DIRECTORY) ||
        !path_valid(entry->path, entry->kind, &length) ||
        (entry->kind == DIRECTORY_KIND_DIRECTORY && (entry->size || entry->sha256[0])) ||
        (entry->kind == DIRECTORY_KIND_FILE && !sha256_valid(entry->sha256)) ||
        entry->size > UINT64_MAX - bytes) {
      return INSTALL_MANIFEST_INVALID;
    }
    size_t record_bytes = 2 * length + (entry->kind == DIRECTORY_KIND_FILE ?
        69 + decimal_bytes(entry->size) : 3);
    if (record_bytes > INSTALL_MANIFEST_MAX_BYTES - text_bytes) {
      return INSTALL_MANIFEST_INVALID;
    }
    text_bytes += record_bytes;
    bytes += entry->size;
    files += entry->kind == DIRECTORY_KIND_FILE;
  }
  if (text_bytes != manifest->text_bytes || bytes != manifest->bytes || files != manifest->files) {
    return INSTALL_MANIFEST_INVALID;
  }
  qsort(manifest->entries, manifest->count, sizeof(*manifest->entries), compare_entries);
  for (size_t index = 0; index < manifest->count; ++index) {
    const char *path = manifest->entries[index].path;
    if (index && !strcmp(manifest->entries[index - 1].path, path)) {
      return INSTALL_MANIFEST_INVALID;
    }
    for (const char *slash = strchr(path, '/'); slash; slash = strchr(slash + 1, '/')) {
      const struct install_manifest_entry *ancestor = find_entry(manifest, path, slash - path);
      if (!ancestor || ancestor->kind != DIRECTORY_KIND_DIRECTORY) {
        return INSTALL_MANIFEST_INVALID;
      }
    }
  }
  manifest->sealed = true;
  return INSTALL_MANIFEST_OK;
}

void install_manifest_destroy(struct install_manifest *manifest)
{
  for (size_t index = 0; index < manifest->count; ++index) {
    free(manifest->entries[index].path);
  }
  free(manifest->entries);
  *manifest = (struct install_manifest){0};
}

static unsigned hex_digit(unsigned char byte)
{
  return byte >= '0' && byte <= '9' ? byte - '0' :
      byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : 16;
}

static enum install_manifest_status decode_hex(const char *text, size_t bytes,
    size_t maximum, char **decoded)
{
  *decoded = NULL;
  if (!bytes || bytes % 2 || bytes / 2 > maximum) {
    return INSTALL_MANIFEST_INVALID;
  }
  /* Reject invalid text before allocating, so malformed input does not turn
   * an avoidable allocation failure into a refused legacy comparison. */
  for (size_t index = 0; index < bytes; index += 2) {
    unsigned high = hex_digit(text[index]), low = hex_digit(text[index + 1]);
    if (high == 16 || low == 16 || (!high && !low)) {
      return INSTALL_MANIFEST_INVALID;
    }
  }
  char *output = malloc(bytes / 2 + 1);
  if (!output) {
    return INSTALL_MANIFEST_NO_MEMORY;
  }
  for (size_t index = 0; index < bytes; index += 2) {
    output[index / 2] = (char)(hex_digit(text[index]) << 4 | hex_digit(text[index + 1]));
  }
  output[bytes / 2] = '\0';
  *decoded = output;
  return INSTALL_MANIFEST_OK;
}

static bool decode_decimal(const char *text, size_t bytes, uint64_t *value)
{
  *value = 0;
  if (!bytes || (bytes > 1 && text[0] == '0')) {
    return false;
  }
  for (size_t index = 0; index < bytes; ++index) {
    unsigned byte = (unsigned char)text[index];
    if (byte < '0' || byte > '9' || *value > (UINT64_MAX - (byte - '0')) / 10) {
      return false;
    }
    *value = *value * 10 + byte - '0';
  }
  return true;
}

static enum install_manifest_status parse_record(const char *line, size_t bytes,
    struct install_manifest *manifest)
{
  if (bytes < 3 || line[1] != ' ' || (line[0] != 'D' && line[0] != 'F')) {
    return INSTALL_MANIFEST_INVALID;
  }
  uint64_t size = 0, kind = line[0] == 'D' ? DIRECTORY_KIND_DIRECTORY : DIRECTORY_KIND_FILE;
  char sha256[65] = {0};
  const char *path_hex = line + 2;
  size_t path_bytes = bytes - 2;
  if (kind == DIRECTORY_KIND_FILE) {
    const char *separator = memchr(path_hex, ' ', path_bytes);
    if (!separator || !decode_decimal(path_hex, separator - path_hex, &size)) {
      return INSTALL_MANIFEST_INVALID;
    }
    size_t remaining = bytes - (separator + 1 - line);
    if (remaining < 66 || separator[65] != ' ') {
      return INSTALL_MANIFEST_INVALID;
    }
    memcpy(sha256, separator + 1, 64);
    if (!sha256_valid(sha256)) {
      return INSTALL_MANIFEST_INVALID;
    }
    path_hex = separator + 66;
    path_bytes = remaining - 65;
  }
  char *path = NULL;
  enum install_manifest_status status = decode_hex(path_hex, path_bytes,
      TREE_PATH_BYTES - 1, &path);
  if (status == INSTALL_MANIFEST_OK && manifest->count &&
      strcmp(manifest->entries[manifest->count - 1].path, path) >= 0) {
    status = INSTALL_MANIFEST_INVALID;
  }
  if (status == INSTALL_MANIFEST_OK) {
    status = install_manifest_add(manifest, path, kind, size,
        kind == DIRECTORY_KIND_FILE ? sha256 : NULL);
  }
  free(path);
  return status;
}

enum install_manifest_status install_manifest_parse(const void *text, size_t bytes,
    struct install_manifest *manifest)
{
  *manifest = (struct install_manifest){0};
  const char *input = text;
  size_t offset = sizeof(MANIFEST_TAG) - 1;
  if (!text || bytes > INSTALL_MANIFEST_MAX_BYTES || bytes < offset + 5 ||
      memcmp(input, MANIFEST_TAG, offset) || memchr(input, '\0', bytes) ||
      input[offset] != 'R' || input[offset + 1] != ' ') {
    return INSTALL_MANIFEST_INVALID;
  }
  const char *newline = memchr(input + offset + 2, '\n', bytes - offset - 2);
  if (!newline) {
    return INSTALL_MANIFEST_INVALID;
  }
  char *revision = NULL;
  enum install_manifest_status status = decode_hex(input + offset + 2,
      newline - (input + offset + 2), sizeof(manifest->revision) - 1, &revision);
  if (status == INSTALL_MANIFEST_OK) {
    status = install_manifest_init(manifest, revision);
  }
  free(revision);
  offset = newline + 1 - input;
  while (status == INSTALL_MANIFEST_OK && offset < bytes) {
    newline = memchr(input + offset, '\n', bytes - offset);
    if (!newline) {
      status = INSTALL_MANIFEST_INVALID;
      break;
    }
    status = parse_record(input + offset, newline - (input + offset), manifest);
    offset = newline + 1 - input;
  }
  if (status == INSTALL_MANIFEST_OK) {
    status = install_manifest_seal(manifest);
    if (status == INSTALL_MANIFEST_OK && manifest->text_bytes != bytes) {
      status = INSTALL_MANIFEST_INVALID;
    }
  }
  if (status != INSTALL_MANIFEST_OK) {
    install_manifest_destroy(manifest);
  }
  return status;
}

static char *encode_hex(char *output, const char *bytes)
{
  for (; *bytes; ++bytes) {
    unsigned byte = (unsigned char)*bytes;
    *output++ = hex[byte >> 4];
    *output++ = hex[byte & 15];
  }
  return output;
}

enum install_manifest_status install_manifest_encode(const struct install_manifest *manifest,
    char **text, size_t *bytes)
{
  *text = NULL;
  *bytes = 0;
  if (!manifest->sealed || manifest->text_bytes > INSTALL_MANIFEST_MAX_BYTES) {
    return INSTALL_MANIFEST_INVALID;
  }
  char *output = malloc(manifest->text_bytes + 1);
  if (!output) {
    return INSTALL_MANIFEST_NO_MEMORY;
  }
  memcpy(output, MANIFEST_TAG, sizeof(MANIFEST_TAG) - 1);
  char *cursor = output + sizeof(MANIFEST_TAG) - 1;
  *cursor++ = 'R';
  *cursor++ = ' ';
  cursor = encode_hex(cursor, manifest->revision);
  *cursor++ = '\n';
  for (size_t index = 0; index < manifest->count; ++index) {
    const struct install_manifest_entry *entry = &manifest->entries[index];
    *cursor++ = entry->kind == DIRECTORY_KIND_FILE ? 'F' : 'D';
    *cursor++ = ' ';
    if (entry->kind == DIRECTORY_KIND_FILE) {
      char number[21];
      int length = snprintf(number, sizeof(number), "%llu", (unsigned long long)entry->size);
      if (length < 1 || (size_t)length >= sizeof(number)) {
        free(output);
        return INSTALL_MANIFEST_INVALID;
      }
      memcpy(cursor, number, length);
      cursor += length;
      *cursor++ = ' ';
      memcpy(cursor, entry->sha256, 64);
      cursor += 64;
      *cursor++ = ' ';
    }
    cursor = encode_hex(cursor, entry->path);
    *cursor++ = '\n';
  }
  if ((size_t)(cursor - output) != manifest->text_bytes) {
    free(output);
    return INSTALL_MANIFEST_INVALID;
  }
  *cursor = '\0';
  *text = output;
  *bytes = manifest->text_bytes;
  return INSTALL_MANIFEST_OK;
}

static bool entry_equal(const struct install_manifest_entry *left,
    const struct install_manifest_entry *right)
{
  return left->kind == right->kind && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

bool install_manifest_equal(const struct install_manifest *left,
    const struct install_manifest *right)
{
  if (!left->sealed || !right->sealed || left->count != right->count ||
      strcmp(left->revision, right->revision)) {
    return false;
  }
  for (size_t index = 0; index < left->count; ++index) {
    if (strcmp(left->entries[index].path, right->entries[index].path) ||
        !entry_equal(&left->entries[index], &right->entries[index])) {
      return false;
    }
  }
  return true;
}

static enum install_manifest_status source_file(handle_t directory, const char *name,
    const char *path, uint8_t *buffer, struct install_manifest *manifest)
{
  handle_t file = HANDLE_INVALID;
  uint64_t size = 0;
  struct install_digest digest = {0};
  bool ok = directory_lookup(directory, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &file) == CALL_OK && file_size(file, &size) == CALL_OK &&
      install_digest_begin(&digest);
  struct install_source source = {.handle = file, .bytes = size};
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t length = size - offset < FILE_READ_MAX_BYTES ?
        (size_t)(size - offset) : FILE_READ_MAX_BYTES;
    ok = install_source_read(&source, offset, buffer, length) &&
        install_digest_update(&digest, buffer, length);
    offset += length;
  }
  char sha256[65];
  ok = ok && install_digest_finish(&digest, sha256);
  install_digest_abort(&digest);
  if (file != HANDLE_INVALID) {
    handle_close(file);
  }
  return ok ? install_manifest_add(manifest, path, DIRECTORY_KIND_FILE, size, sha256) :
      INSTALL_MANIFEST_INVALID;
}

struct source_level {
  handle_t directory;
  struct directory_cursor cursor;
  size_t path_bytes;
};

static enum install_manifest_status source_directory(handle_t boot, const char *name,
    char *path, uint8_t *buffer, struct source_level *levels,
    struct install_manifest *manifest)
{
  unsigned depth = 0;
  handle_t root = HANDLE_INVALID;
  if (directory_lookup(boot, name, DIRECTORY_KIND_DIRECTORY, SOURCE_RIGHTS, &root) != CALL_OK) {
    return INSTALL_MANIFEST_INVALID;
  }
  levels[0] = (struct source_level){.directory = root, .path_bytes = strlen(path)};
  enum install_manifest_status status = INSTALL_MANIFEST_OK;
  for (;;) {
    struct source_level *level = &levels[depth];
    char child_name[INSTALL_TREE_NAME_MAX + 1];
    struct directory_enumerate_reply entry;
    if (directory_enumerate(level->directory, &level->cursor, child_name,
        sizeof(child_name), &entry) != CALL_OK) {
      status = INSTALL_MANIFEST_INVALID;
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      handle_close(level->directory);
      if (!depth) {
        return INSTALL_MANIFEST_OK;
      }
      --depth;
      continue;
    }
    if (entry.outcome != DIRECTORY_ENTRY ||
        !component_valid(child_name, strnlen(child_name, sizeof(child_name)))) {
      status = INSTALL_MANIFEST_INVALID;
      break;
    }
    level->cursor = entry.cursor;
    size_t child_bytes = strlen(child_name);
    size_t path_bytes = level->path_bytes + 1 + child_bytes;
    if (path_bytes >= TREE_PATH_BYTES) {
      status = INSTALL_MANIFEST_INVALID;
      break;
    }
    path[level->path_bytes] = '/';
    memcpy(path + level->path_bytes + 1, child_name, child_bytes + 1);
    if (entry.kind == DIRECTORY_KIND_FILE) {
      status = source_file(level->directory, child_name, path, buffer, manifest);
    } else if (entry.kind == DIRECTORY_KIND_DIRECTORY &&
        depth + 1 < INSTALL_TREE_MAX_DEPTH) {
      status = install_manifest_add(manifest, path, DIRECTORY_KIND_DIRECTORY, 0, NULL);
      handle_t child = HANDLE_INVALID;
      if (status == INSTALL_MANIFEST_OK && directory_lookup(level->directory,
          child_name, DIRECTORY_KIND_DIRECTORY, SOURCE_RIGHTS, &child) != CALL_OK) {
        status = INSTALL_MANIFEST_INVALID;
      }
      if (status == INSTALL_MANIFEST_OK) {
        levels[++depth] = (struct source_level){.directory = child, .path_bytes = path_bytes};
      }
    } else {
      status = INSTALL_MANIFEST_INVALID;
    }
    if (status != INSTALL_MANIFEST_OK) {
      break;
    }
    install_progress_advance(0, 1);
  }
  for (;;) {
    handle_close(levels[depth].directory);
    if (!depth) {
      break;
    }
    --depth;
  }
  return status;
}

enum install_manifest_status install_manifest_source(handle_t boot,
    const struct install_programs *programs, const char *revision,
    struct install_manifest *manifest)
{
  enum install_manifest_status status = install_manifest_init(manifest, revision);
  if (status != INSTALL_MANIFEST_OK) {
    return status;
  }
  char *path = malloc(TREE_PATH_BYTES);
  uint8_t *buffer = malloc(FILE_READ_MAX_BYTES);
  struct source_level *levels = calloc(INSTALL_TREE_MAX_DEPTH, sizeof(*levels));
  if (!path || !buffer || !levels) {
    status = INSTALL_MANIFEST_NO_MEMORY;
  }
  install_progress_begin("Inventorying and hashing source programs",
      INSTALL_PROGRESS_SOURCE_READ, "entries");
  if (programs->totals_known) {
    install_progress_bytes_total(programs->bytes);
  }
  for (size_t index = 0; status == INSTALL_MANIFEST_OK && index < programs->count; ++index) {
    const struct install_program *program = &programs->entries[index];
    size_t length = strnlen(program->name, INSTALL_TREE_NAME_MAX + 1);
    if (!component_valid(program->name, length)) {
      status = INSTALL_MANIFEST_INVALID;
      break;
    }
    memcpy(path, program->name, length + 1);
    if (program->kind == DIRECTORY_KIND_FILE) {
      status = source_file(boot, program->name, path, buffer, manifest);
    } else if (program->kind == DIRECTORY_KIND_DIRECTORY) {
      status = install_manifest_add(manifest, path, DIRECTORY_KIND_DIRECTORY, 0, NULL);
      if (status == INSTALL_MANIFEST_OK) {
        install_progress_advance(0, 1);
        status = source_directory(boot, program->name, path, buffer, levels, manifest);
      }
    } else {
      status = INSTALL_MANIFEST_INVALID;
    }
    if (status == INSTALL_MANIFEST_OK && program->kind == DIRECTORY_KIND_FILE) {
      install_progress_advance(0, 1);
    }
  }
  if (status == INSTALL_MANIFEST_OK) {
    status = install_manifest_seal(manifest);
  }
  if (status != INSTALL_MANIFEST_OK) {
    install_progress_pause();
    fputs("installer: cannot inventory source programs within manifest limits\n", stderr);
    install_manifest_destroy(manifest);
  }
  install_progress_finish();
  free(levels);
  free(buffer);
  free(path);
  return status;
}

enum program_change {
  PROGRAM_NEW,
  PROGRAM_UPDATED,
  PROGRAM_REMOVED,
  PROGRAM_UNCHANGED,
};

struct program_report {
  const char *name;
  enum program_change change;
  uint64_t old_bytes, new_bytes;
  size_t old_files, new_files;
};

static struct program_report *find_report(struct program_report *reports, size_t count,
    const char *path)
{
  const char *slash = strchr(path, '/');
  size_t bytes = slash ? (size_t)(slash - path) : strlen(path);
  size_t first = 0, end = count;
  while (first < end) {
    size_t middle = first + (end - first) / 2;
    int result = compare_prefix(reports[middle].name, path, bytes);
    if (!result) {
      return &reports[middle];
    }
    if (result < 0) {
      first = middle + 1;
    } else {
      end = middle;
    }
  }
  return NULL;
}

static size_t next_root(const struct install_manifest *manifest, size_t index)
{
  while (index < manifest->count && strchr(manifest->entries[index].path, '/')) {
    ++index;
  }
  return index;
}

static bool print_name(FILE *output, const char *name)
{
  if (fputc('"', output) == EOF) {
    return false;
  }
  for (; *name; ++name) {
    unsigned byte = (unsigned char)*name;
    if (byte < 0x20 || byte > 0x7e) {
      if (fprintf(output, "\\x%02x", byte) < 0) {
        return false;
      }
    } else {
      if ((byte == '\\' || byte == '"') && fputc('\\', output) == EOF) {
        return false;
      }
      if (fputc(byte, output) == EOF) {
        return false;
      }
    }
  }
  return fputc('"', output) != EOF;
}

bool install_manifest_report(FILE *output, const struct install_manifest *previous,
    const struct install_manifest *source)
{
  static const char *const changes[] = {"new", "updated", "removed", "unchanged"};
  if (!source->sealed || (previous && !previous->sealed)) {
    return false;
  }
  const struct install_manifest empty = {.sealed = true};
  if (!previous) {
    previous = &empty;
  }
  size_t capacity = previous->count + source->count;
  struct program_report *reports = capacity ? calloc(capacity, sizeof(*reports)) : NULL;
  if (capacity && !reports) {
    return false;
  }
  size_t count = 0, old = next_root(previous, 0), new = next_root(source, 0);
  while (old < previous->count || new < source->count) {
    const char *old_name = old < previous->count ? previous->entries[old].path : NULL;
    const char *new_name = new < source->count ? source->entries[new].path : NULL;
    int order = !old_name ? 1 : !new_name ? -1 : strcmp(old_name, new_name);
    struct program_report *report = &reports[count++];
    report->name = order > 0 ? new_name : old_name;
    report->change = order > 0 ? PROGRAM_NEW : order < 0 ? PROGRAM_REMOVED : PROGRAM_UNCHANGED;
    if (order <= 0) {
      old = next_root(previous, old + 1);
    }
    if (order >= 0) {
      new = next_root(source, new + 1);
    }
  }
  for (unsigned side = 0; side < 2; ++side) {
    const struct install_manifest *manifest = side ? source : previous;
    for (size_t index = 0; index < manifest->count; ++index) {
      const struct install_manifest_entry *entry = &manifest->entries[index];
      struct program_report *report = find_report(reports, count, entry->path);
      if (!report) {
        free(reports);
        return false;
      }
      if (entry->kind == DIRECTORY_KIND_FILE) {
        if (side) {
          report->new_bytes += entry->size;
          ++report->new_files;
        } else {
          report->old_bytes += entry->size;
          ++report->old_files;
        }
      }
    }
  }
  old = new = 0;
  while (old < previous->count || new < source->count) {
    const struct install_manifest_entry *a = old < previous->count ? &previous->entries[old] : NULL;
    const struct install_manifest_entry *b = new < source->count ? &source->entries[new] : NULL;
    int order = !a ? 1 : !b ? -1 : strcmp(a->path, b->path);
    if (order || !entry_equal(a, b)) {
      struct program_report *report = find_report(reports, count, order > 0 ? b->path : a->path);
      if (report->change == PROGRAM_UNCHANGED) {
        report->change = PROGRAM_UPDATED;
      }
    }
    if (order <= 0) {
      ++old;
    }
    if (order >= 0) {
      ++new;
    }
  }
  bool ok = true;
  for (size_t index = 0; ok && index < count; ++index) {
    const struct program_report *report = &reports[index];
    ok = fprintf(output, "  %-9s ", changes[report->change]) >= 0 &&
        print_name(output, report->name) &&
        fprintf(output, "  old %zu files/%llu bytes; new %zu files/%llu bytes\n",
          report->old_files, (unsigned long long)report->old_bytes,
          report->new_files, (unsigned long long)report->new_bytes) >= 0;
  }
  free(reports);
  return ok;
}
