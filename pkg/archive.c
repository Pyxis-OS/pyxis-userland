#include "archive.h"
#include "../xfer/hash.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 512
#define IO_SIZE 16384
#define INVENTORY_SIZE_MAX ((uint64_t)PKG_MAX_FILES * PKG_INVENTORY_LINE_MAX)
#define DIRECTORY_DEPTH_MAX (PKG_PATH_MAX / 2 + 1)
#define READ_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define WRITE_DIRECTORY_RIGHTS (READ_DIRECTORY_RIGHTS | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)

struct member {
  char name[257];
  uint64_t offset, size;
  bool directory;
};

struct archive_reader {
  handle_t file;
  uint64_t size, position;
};

struct inventory_reader {
  handle_t file;
  uint64_t position, end;
  unsigned char buffer[4096];
  size_t used, available;
};

struct inventory_line {
  char path[PKG_PATH_MAX + 1], sha256[65];
  uint64_t size;
};

struct directory_frame {
  handle_t handle;
  size_t end, children;
};

struct tree_walk {
  struct directory_frame stack[DIRECTORY_DEPTH_MAX];
  size_t depth;
  char previous[PKG_PATH_MAX + 1];
  bool file_prefix[PKG_PATH_MAX + 1];
  bool extract, verify;
};

static bool fail(const char **reason, const char *message)
{
  *reason = message;
  return false;
}

static bool read_exact(handle_t file, uint64_t offset, void *bytes, size_t size,
    const char **reason)
{
  unsigned char *output = bytes;
  while (size) {
    if (pkg_cancelled()) {
      return fail(reason, "cancelled");
    }
    size_t count = 0;
    size_t wanted = size > IO_SIZE ? IO_SIZE : size;
    if (file_read(file, offset, output, wanted, &count) != CALL_OK ||
        !count || count > wanted) {
      return fail(reason, "cannot read package bytes");
    }
    offset += count;
    output += count;
    size -= count;
  }
  return true;
}

static bool write_exact(handle_t file, uint64_t offset, const void *bytes,
    size_t size, const char **reason)
{
  const unsigned char *input = bytes;
  while (size) {
    size_t count = 0;
    if (file_write(file, offset, input, size, &count) != CALL_OK || !count || count > size) {
      return fail(reason, "cannot write staged package file");
    }
    offset += count;
    input += count;
    size -= count;
  }
  return true;
}

static bool zeroes(handle_t file, uint64_t offset, uint64_t size, const char **reason)
{
  unsigned char bytes[IO_SIZE];
  while (size) {
    size_t count = size > sizeof(bytes) ? sizeof(bytes) : (size_t)size;
    if (!read_exact(file, offset, bytes, count, reason)) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      if (bytes[i]) {
        return fail(reason, "nonzero archive padding or trailer");
      }
    }
    offset += count;
    size -= count;
  }
  return true;
}

/* Hash exactly one bounded range, optionally copying to a fresh file. */
static bool hash_range(handle_t file, uint64_t offset, uint64_t size,
    const char *expected, handle_t destination, const char **reason)
{
  unsigned char bytes[IO_SIZE];
  char actual[65];
  if (!digest_begin()) {
    return fail(reason, "cannot initialize SHA-256");
  }
  bool ok = true;
  uint64_t copied = 0;
  while (ok && copied < size) {
    size_t count = size - copied > sizeof(bytes) ? sizeof(bytes) : (size_t)(size - copied);
    ok = read_exact(file, offset + copied, bytes, count, reason);
    if (ok && !digest_update(bytes, count)) {
      ok = fail(reason, "cannot compute SHA-256");
    }
    if (ok && destination != HANDLE_INVALID) {
      ok = write_exact(destination, copied, bytes, count, reason);
    }
    copied += count;
  }
  if (ok && (!digest_finish(actual) || strcmp(expected, actual))) {
    ok = fail(reason, "package SHA-256 mismatch");
  }
  digest_abort();
  return ok;
}

static bool copy_range(handle_t file, uint64_t offset, uint64_t size,
    handle_t destination, const char **reason)
{
  unsigned char bytes[IO_SIZE];
  uint64_t copied = 0;
  while (copied < size) {
    size_t count = size - copied > sizeof(bytes) ? sizeof(bytes) : (size_t)(size - copied);
    if (!read_exact(file, offset + copied, bytes, count, reason) ||
        !write_exact(destination, copied, bytes, count, reason)) {
      return false;
    }
    copied += count;
  }
  return true;
}

static bool all_zero(const unsigned char *bytes, size_t size)
{
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i]) {
      return false;
    }
  }
  return true;
}

static bool octal(const unsigned char *bytes, size_t size, uint64_t *value)
{
  if (bytes[size - 1]) {
    return false;
  }
  *value = 0;
  for (size_t i = 0; i + 1 < size; ++i) {
    if (bytes[i] < '0' || bytes[i] > '7') {
      return false;
    }
    *value = *value * 8 + bytes[i] - '0';
  }
  return true;
}

static bool field_text(const unsigned char *bytes, size_t size, size_t *length)
{
  *length = 0;
  while (*length < size && bytes[*length]) {
    ++*length;
  }
  return all_zero(bytes + *length, size - *length);
}

static bool utf8(const char *bytes)
{
  size_t remaining = strlen(bytes);
  while (remaining) {
    int count = mbtowc(NULL, bytes, remaining);
    if (count <= 0) {
      return false;
    }
    bytes += count;
    remaining -= (size_t)count;
  }
  return true;
}

static bool split_name(const char *name, size_t *prefix)
{
  size_t size = strlen(name);
  *prefix = 0;
  if (size && size <= 100) {
    return true;
  }
  for (size_t i = 0; i < size && i <= 155; ++i) {
    if (name[i] == '/' && size - i - 1 >= 1 && size - i - 1 <= 100 &&
        strcmp(name + i + 1, "/")) {
      *prefix = i;
      return true;
    }
  }
  return false;
}

static bool next_member(struct archive_reader *reader, struct member *member,
    const char **reason)
{
  unsigned char bytes[BLOCK_SIZE];
  if (reader->position > reader->size || reader->size - reader->position < BLOCK_SIZE) {
    return fail(reason, "missing archive member");
  }
  if (!read_exact(reader->file, reader->position, bytes, sizeof(bytes), reason)) {
    return false;
  }
  uint64_t stored = 0;
  if (!octal(bytes + 148, 7, &stored) || bytes[155] != ' ') {
    return fail(reason, "invalid USTAR checksum field");
  }
  uint64_t checksum = 8 * ' ';
  for (size_t i = 0; i < BLOCK_SIZE; ++i) {
    if (i < 148 || i >= 156) {
      checksum += bytes[i];
    }
  }
  uint64_t unused;
  if (stored != checksum || memcmp(bytes + 257, "ustar\0" "00", 8) ||
      (bytes[156] != '0' && bytes[156] != '5') ||
      !octal(bytes + 100, 8, &unused) || !octal(bytes + 108, 8, &unused) ||
      !octal(bytes + 116, 8, &unused) || !octal(bytes + 124, 12, &member->size) ||
      !octal(bytes + 136, 12, &unused) || !octal(bytes + 329, 8, &unused) ||
      !octal(bytes + 337, 8, &unused) || !all_zero(bytes + 157, 100) ||
      !all_zero(bytes + 265, 64) || !all_zero(bytes + 500, 12)) {
    return fail(reason, "invalid plain USTAR header");
  }
  size_t name_size, prefix_size;
  if (!field_text(bytes, 100, &name_size) || !name_size ||
      !field_text(bytes + 345, 155, &prefix_size)) {
    return fail(reason, "invalid USTAR name padding");
  }
  size_t full_size = 0;
  if (prefix_size) {
    memcpy(member->name, bytes + 345, prefix_size);
    member->name[prefix_size] = '/';
    full_size = prefix_size + 1;
  }
  memcpy(member->name + full_size, bytes, name_size);
  full_size += name_size;
  member->name[full_size] = 0;
  size_t canonical_prefix;
  member->directory = bytes[156] == '5';
  if (!utf8(member->name) || !split_name(member->name, &canonical_prefix) ||
      canonical_prefix != prefix_size ||
      (member->directory && (member->name[full_size - 1] != '/' || member->size)) ||
      (!member->directory && member->name[full_size - 1] == '/')) {
    return fail(reason, "invalid USTAR member name or directory");
  }
  member->offset = reader->position + BLOCK_SIZE;
  uint64_t padding = (BLOCK_SIZE - member->size % BLOCK_SIZE) % BLOCK_SIZE;
  if (member->size > reader->size - member->offset ||
      padding > reader->size - member->offset - member->size) {
    return fail(reason, "USTAR member exceeds archive size");
  }
  reader->position = member->offset + member->size + padding;
  return zeroes(reader->file, member->offset + member->size, padding, reason);
}

static bool expect_member(struct archive_reader *reader, const char *name,
    uint64_t size, bool directory, struct member *member, const char **reason)
{
  if (!next_member(reader, member, reason)) {
    return false;
  }
  if (strcmp(member->name, name) || member->directory != directory || member->size != size) {
    return fail(reason, "archive members differ from inventory order");
  }
  return true;
}

static bool line_read(struct inventory_reader *reader, char line[PKG_INVENTORY_LINE_MAX + 1],
    bool *end, const char **reason)
{
  size_t size = 0;
  *end = false;
  for (;;) {
    if (reader->used == reader->available) {
      if (reader->position == reader->end) {
        if (size) {
          return fail(reason, "inventory lacks final LF");
        }
        *end = true;
        return true;
      }
      size_t count = reader->end - reader->position > sizeof(reader->buffer) ?
          sizeof(reader->buffer) : (size_t)(reader->end - reader->position);
      if (!read_exact(reader->file, reader->position, reader->buffer, count, reason)) {
        return false;
      }
      reader->position += count;
      reader->used = 0;
      reader->available = count;
    }
    unsigned char byte = reader->buffer[reader->used++];
    if (!byte || byte == '\r' || size == PKG_INVENTORY_LINE_MAX) {
      return fail(reason, "invalid or excessive inventory line");
    }
    if (byte == '\n') {
      line[size] = 0;
      return true;
    }
    line[size++] = (char)byte;
  }
}

static bool parse_line(const char *text, struct inventory_line *line, const char **reason)
{
  size_t length = strlen(text);
  if (length < 68 || text[64] != ' ') {
    return fail(reason, "invalid inventory fields");
  }
  for (size_t i = 0; i < 64; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
      return fail(reason, "invalid inventory SHA-256");
    }
  }
  memcpy(line->sha256, text, 64);
  line->sha256[64] = 0;
  size_t index = 65, digits = 0;
  line->size = 0;
  while (text[index] >= '0' && text[index] <= '9') {
    unsigned digit = text[index++] - '0';
    if (++digits > 19 || line->size > (PKG_FILE_SIZE_MAX - digit) / 10) {
      return fail(reason, "inventory file size exceeds format limit");
    }
    line->size = line->size * 10 + digit;
  }
  if (!digits || (digits > 1 && text[65] == '0') || text[index++] != ' ') {
    return fail(reason, "invalid inventory decimal size");
  }
  size_t path_size = length - index;
  if (!path_size || path_size > PKG_PATH_MAX || text[index] == '/' ||
      memchr(text + index, '\\', path_size)) {
    return fail(reason, "invalid inventory path");
  }
  memcpy(line->path, text + index, path_size + 1);
  if (!utf8(line->path)) {
    return fail(reason, "inventory path is not UTF-8");
  }
  size_t start = 0;
  for (size_t i = 0; i <= path_size; ++i) {
    if (i != path_size && line->path[i] != '/') {
      continue;
    }
    size_t count = i - start;
    if (!count || count > 255 || (count == 1 && line->path[start] == '.') ||
        (count == 2 && line->path[start] == '.' && line->path[start + 1] == '.')) {
      return fail(reason, "invalid inventory path component");
    }
    start = i + 1;
  }
  char full[257] = "tree/";
  memcpy(full + 5, line->path, path_size + 1);
  size_t prefix;
  if (!split_name(full, &prefix)) {
    return fail(reason, "inventory path cannot be split into USTAR fields");
  }
  return true;
}

static bool directory_count(handle_t directory, size_t expected, const char **reason)
{
  struct directory_cursor cursor = {0};
  size_t count = 0;
  for (;;) {
    if (pkg_cancelled()) {
      return fail(reason, "cancelled");
    }
    char name[256];
    struct directory_enumerate_reply reply;
    if (directory_enumerate(directory, &cursor, name, sizeof(name), &reply) != CALL_OK) {
      return fail(reason, "cannot enumerate installed package");
    }
    if (reply.outcome == DIRECTORY_END) {
      return count == expected || fail(reason, "installed package has surplus or missing entries");
    }
    if (reply.outcome != DIRECTORY_ENTRY || ++count > expected ||
        (reply.kind != DIRECTORY_KIND_FILE && reply.kind != DIRECTORY_KIND_DIRECTORY)) {
      return fail(reason, "installed package has unexpected or changing entries");
    }
    cursor = reply.cursor;
  }
}

static bool close_frame(struct tree_walk *walk, const char **reason)
{
  struct directory_frame *frame = &walk->stack[--walk->depth];
  bool ok = true;
  if (frame->handle != HANDLE_INVALID) {
    if (walk->extract && directory_sync(frame->handle) != CALL_OK) {
      ok = fail(reason, "cannot sync staged package directory");
    } else if (walk->verify) {
      ok = directory_count(frame->handle, frame->children, reason);
    }
    handle_close(frame->handle);
    frame->handle = HANDLE_INVALID;
  }
  return ok;
}

static void walk_abort(struct tree_walk *walk)
{
  while (walk->depth) {
    handle_t handle = walk->stack[--walk->depth].handle;
    if (handle != HANDLE_INVALID) {
      handle_close(handle);
    }
  }
}

static bool walk_path(struct tree_walk *walk, struct archive_reader *archive,
    const struct inventory_line *line, handle_t *file, struct member *member,
    const char **reason)
{
  *file = HANDLE_INVALID;
  size_t length = strlen(line->path), common = 0;
  if (walk->previous[0] && strcmp(walk->previous, line->path) >= 0) {
    return fail(reason, "inventory is not sorted without duplicates");
  }
  while (walk->previous[common] && walk->previous[common] == line->path[common]) {
    ++common;
  }
  /* A prior file can still be a prefix after intervening names: a, a-, a/b. */
  for (size_t i = common + 1; i <= PKG_PATH_MAX; ++i) {
    walk->file_prefix[i] = false;
  }
  size_t retained = 1;
  while (retained < walk->depth && walk->stack[retained].end <= common &&
      line->path[walk->stack[retained].end] == '/') {
    ++retained;
  }
  while (walk->depth > retained) {
    if (!close_frame(walk, reason)) {
      return false;
    }
  }
  size_t start = 0;
  for (size_t i = 0; i < length; ++i) {
    if (line->path[i] != '/') {
      continue;
    }
    if (walk->file_prefix[i]) {
      return fail(reason, "inventory path is both a file and directory");
    }
    if (walk->depth > 1 && i <= walk->stack[walk->depth - 1].end) {
      start = i + 1;
      continue;
    }
    char name[257] = "tree/";
    memcpy(name + 5, line->path, i);
    name[5 + i] = '/';
    name[6 + i] = 0;
    size_t prefix;
    if (!split_name(name, &prefix)) {
      return fail(reason, "inventory directory does not fit USTAR");
    }
    if (archive && !expect_member(archive, name, 0, true, member, reason)) {
      return false;
    }
    char component[PKG_PATH_MAX + 1];
    memcpy(component, line->path + start, i - start);
    component[i - start] = 0;
    struct directory_frame *parent = &walk->stack[walk->depth - 1];
    handle_t child = HANDLE_INVALID;
    enum call_status status = CALL_OK;
    if (walk->extract) {
      status = directory_create(parent->handle, component, DIRECTORY_KIND_DIRECTORY,
          WRITE_DIRECTORY_RIGHTS, &child);
    } else if (walk->verify) {
      status = directory_lookup(parent->handle, component, DIRECTORY_KIND_DIRECTORY,
          READ_DIRECTORY_RIGHTS, &child);
    }
    if (status != CALL_OK) {
      return fail(reason, "cannot open expected package directory");
    }
    ++parent->children;
    walk->stack[walk->depth++] = (struct directory_frame){ .handle = child, .end = i };
    start = i + 1;
  }
  char name[257] = "tree/";
  memcpy(name + 5, line->path, length + 1);
  if (archive && !expect_member(archive, name, line->size, false, member, reason)) {
    return false;
  }
  struct directory_frame *parent = &walk->stack[walk->depth - 1];
  enum call_status status = CALL_OK;
  if (walk->extract) {
    status = directory_create(parent->handle, line->path + start,
        DIRECTORY_KIND_FILE, FILE_RIGHT_READ | FILE_RIGHT_WRITE, file);
  } else if (walk->verify) {
    status = directory_lookup(parent->handle, line->path + start,
        DIRECTORY_KIND_FILE, FILE_RIGHT_READ, file);
  }
  if (status != CALL_OK) {
    return fail(reason, "cannot open expected package file");
  }
  ++parent->children;
  memcpy(walk->previous, line->path, length + 1);
  walk->file_prefix[length] = true;
  return true;
}

static bool process_inventory(struct inventory_reader *inventory,
    struct archive_reader *archive, struct tree_walk *walk,
    const struct pkg_manifest *manifest, const char **reason)
{
  uint64_t files = 0, bytes = 0;
  for (;;) {
    char text[PKG_INVENTORY_LINE_MAX + 1];
    bool end;
    struct inventory_line line;
    if (!line_read(inventory, text, &end, reason)) {
      return false;
    }
    if (end) {
      break;
    }
    if (pkg_cancelled()) {
      return fail(reason, "cancelled");
    }
    if (!parse_line(text, &line, reason)) {
      return false;
    }
    if (++files > PKG_MAX_FILES || bytes > UINT64_MAX - line.size) {
      return fail(reason, "inventory totals exceed format bounds");
    }
    bytes += line.size;
    handle_t file = HANDLE_INVALID;
    struct member member;
    if (!walk_path(walk, archive, &line, &file, &member, reason)) {
      return false;
    }
    bool ok;
    if (archive) {
      ok = hash_range(archive->file, member.offset, line.size, line.sha256, file, reason);
      if (ok && file != HANDLE_INVALID && file_sync(file) != CALL_OK) {
        ok = fail(reason, "cannot sync staged package file");
      }
    } else {
      uint64_t size;
      ok = file_size(file, &size) == CALL_OK && size == line.size;
      if (!ok) {
        fail(reason, "installed package file size differs from inventory");
      } else {
        ok = hash_range(file, 0, line.size, line.sha256, HANDLE_INVALID, reason);
      }
    }
    if (file != HANDLE_INVALID) {
      handle_close(file);
    }
    if (!ok) {
      return false;
    }
  }
  if (files != manifest->files || bytes != manifest->bytes) {
    return fail(reason, "inventory totals differ from manifest");
  }
  while (walk->depth) {
    if (!close_frame(walk, reason)) {
      return false;
    }
  }
  return true;
}

static bool read_manifest(handle_t file, uint64_t offset, uint64_t size,
    struct pkg_manifest *manifest, const char **reason)
{
  if (size > LITERAL_SOURCE_LIMIT) {
    return fail(reason, "package manifest exceeds literal source limit");
  }
  char *bytes = malloc(size ? (size_t)size : 1);
  if (!bytes) {
    return fail(reason, "out of memory reading package manifest");
  }
  bool ok = read_exact(file, offset, bytes, (size_t)size, reason) &&
      pkg_manifest_parse(bytes, (size_t)size, manifest, reason);
  free(bytes);
  return ok;
}

static bool copy_metadata(handle_t archive, const struct member *member,
    handle_t destination, const char **reason)
{
  handle_t file;
  if (directory_create(destination, member->name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_WRITE, &file) != CALL_OK) {
    return fail(reason, "cannot create staged metadata");
  }
  bool ok = copy_range(archive, member->offset, member->size, file, reason);
  if (ok && file_sync(file) != CALL_OK) {
    ok = fail(reason, "cannot sync staged package metadata");
  }
  handle_close(file);
  return ok;
}

bool pkg_archive(handle_t snapshot, const struct pkg_entry *entry,
    handle_t destination, struct pkg_manifest *manifest, const char **reason)
{
  memset(manifest, 0, sizeof(*manifest));
  *reason = NULL;
  struct archive_reader reader = { .file = snapshot };
  if (file_size(snapshot, &reader.size) != CALL_OK || reader.size != entry->size) {
    return fail(reason, "archive size differs from repository index");
  }
  if (!hash_range(snapshot, 0, reader.size, entry->sha256, HANDLE_INVALID, reason)) {
    return false;
  }
  if (reader.size % BLOCK_SIZE) {
    return fail(reason, "archive is not a multiple of 512 bytes");
  }
  struct member metadata[2];
  if (!next_member(&reader, &metadata[0], reason) ||
      strcmp(metadata[0].name, "package.lua") || metadata[0].directory ||
      !next_member(&reader, &metadata[1], reason) ||
      strcmp(metadata[1].name, "package.files") || metadata[1].directory) {
    if (!*reason) {
      fail(reason, "archive must begin with package.lua and package.files");
    }
    return false;
  }
  struct tree_walk walk = { .extract = destination != HANDLE_INVALID };
  bool ok = read_manifest(snapshot, metadata[0].offset, metadata[0].size, manifest, reason);
  if (ok && (strcmp(manifest->identity.id, entry->identity.id) ||
      strcmp(manifest->identity.version, entry->identity.version) || strcmp(manifest->kind, entry->kind))) {
    ok = fail(reason, "package manifest differs from repository index");
  }
  if (ok && metadata[1].size > INVENTORY_SIZE_MAX) {
    ok = fail(reason, "package inventory exceeds format limit");
  }
  if (ok) {
    ok = hash_range(snapshot, metadata[1].offset, metadata[1].size,
        manifest->inventory_sha256, HANDLE_INVALID, reason);
  }
  if (ok && destination != HANDLE_INVALID) {
    ok = copy_metadata(snapshot, &metadata[0], destination, reason) &&
        copy_metadata(snapshot, &metadata[1], destination, reason);
  }
  struct member tree;
  if (ok) {
    ok = expect_member(&reader, "tree/", 0, true, &tree, reason);
  }
  handle_t root = HANDLE_INVALID;
  if (ok && destination != HANDLE_INVALID && directory_create(destination, "tree",
      DIRECTORY_KIND_DIRECTORY, WRITE_DIRECTORY_RIGHTS, &root) != CALL_OK) {
    ok = fail(reason, "cannot create staged package tree");
  }
  if (ok) {
    walk.stack[0] = (struct directory_frame){ .handle = root };
    walk.depth = 1;
    struct inventory_reader inventory = {
      .file = snapshot, .position = metadata[1].offset,
      .end = metadata[1].offset + metadata[1].size,
    };
    ok = process_inventory(&inventory, &reader, &walk, manifest, reason);
  }
  if (ok && (reader.size - reader.position < 2 * BLOCK_SIZE ||
      !zeroes(snapshot, reader.position, reader.size - reader.position, reason))) {
    if (!*reason) {
      fail(reason, "archive lacks two zero trailer blocks");
    }
    ok = false;
  }
  if (ok && destination != HANDLE_INVALID && directory_sync(destination) != CALL_OK) {
    ok = fail(reason, "cannot sync staged package namespace");
  }
  walk_abort(&walk);
  if (!ok) {
    pkg_manifest_free(manifest);
  }
  return ok;
}

static bool manifests_equal(const struct pkg_manifest *a, const struct pkg_manifest *b)
{
  if (strcmp(a->identity.id, b->identity.id) || strcmp(a->identity.version, b->identity.version) ||
      strcmp(a->kind, b->kind) || a->files != b->files || a->bytes != b->bytes ||
      strcmp(a->inventory_sha256, b->inventory_sha256) || a->dependency_count != b->dependency_count) {
    return false;
  }
  for (size_t i = 0; i < a->dependency_count; ++i) {
    bool found = false;
    for (size_t j = 0; j < b->dependency_count; ++j) {
      if (!strcmp(a->dependencies[i].id, b->dependencies[j].id) &&
          !strcmp(a->dependencies[i].version, b->dependencies[j].version)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

bool pkg_tree_verify(handle_t directory, const struct pkg_manifest *expected,
    struct pkg_manifest *manifest, const char **reason)
{
  memset(manifest, 0, sizeof(*manifest));
  *reason = NULL;
  handle_t metadata = HANDLE_INVALID, inventory = HANDLE_INVALID, tree = HANDLE_INVALID;
  struct tree_walk walk = { .verify = true };
  uint64_t size = 0, inventory_size = 0;
  bool ok = directory_lookup(directory, "package.lua", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &metadata) == CALL_OK && file_size(metadata, &size) == CALL_OK;
  if (!ok) {
    fail(reason, "cannot read installed package manifest");
  }
  if (ok) {
    ok = read_manifest(metadata, 0, size, manifest, reason);
  }
  if (ok && expected && !manifests_equal(manifest, expected)) {
    ok = fail(reason, "installed immutable package conflicts with requested metadata");
  }
  if (ok) {
    ok = directory_lookup(directory, "package.files", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &inventory) == CALL_OK &&
        file_size(inventory, &inventory_size) == CALL_OK && inventory_size <= INVENTORY_SIZE_MAX;
    if (!ok) {
      fail(reason, "cannot read bounded installed package inventory");
    }
  }
  if (ok) {
    ok = hash_range(inventory, 0, inventory_size, manifest->inventory_sha256, HANDLE_INVALID, reason);
  }
  if (ok && directory_lookup(directory, "tree", DIRECTORY_KIND_DIRECTORY,
      READ_DIRECTORY_RIGHTS, &tree) != CALL_OK) {
    ok = fail(reason, "cannot open installed package tree");
  }
  if (ok) {
    walk.stack[0] = (struct directory_frame){ .handle = tree };
    walk.depth = 1;
    tree = HANDLE_INVALID;
    struct inventory_reader lines = { .file = inventory, .end = inventory_size };
    ok = process_inventory(&lines, NULL, &walk, manifest, reason);
  }
  if (ok) {
    ok = directory_count(directory, 3, reason);
  }
  walk_abort(&walk);
  if (tree != HANDLE_INVALID) {
    handle_close(tree);
  }
  if (inventory != HANDLE_INVALID) {
    handle_close(inventory);
  }
  if (metadata != HANDLE_INVALID) {
    handle_close(metadata);
  }
  if (!ok) {
    pkg_manifest_free(manifest);
  }
  return ok;
}
