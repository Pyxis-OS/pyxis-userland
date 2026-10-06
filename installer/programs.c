#include "installer.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_SUFFIX ".pxe"
#define NEWC_HEADER_BYTES 110u
#define NEWC_TRAILER "TRAILER!!!"
#define BIN_RIGHTS DIRECTORY_CONTENT_RIGHTS
/* One file call each way per chunk. */
#define COPY_BYTES (FILE_READ_MAX_BYTES < FILE_WRITE_MAX_BYTES ? FILE_READ_MAX_BYTES : \
    FILE_WRITE_MAX_BYTES)

static bool add_name(char ***names, size_t *count, const char *name)
{
  char **larger = realloc(*names, (*count + 1) * sizeof(**names));
  if (!larger) {
    return false;
  }
  *names = larger;
  larger[*count] = strdup(name);
  if (!larger[*count]) {
    return false;
  }
  ++*count;
  return true;
}

static void free_names(char **names, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    free(names[i]);
  }
  free(names);
}

static bool listed(char *const *names, size_t count, const char *name)
{
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(names[i], name)) {
      return true;
    }
  }
  return false;
}

static int compare_names(const void *left, const void *right)
{
  return strcmp(*(char *const *)left, *(char *const *)right);
}

/* Reads a whole small file into a NUL-terminated buffer. */
static char *read_text(handle_t file)
{
  uint64_t size;
  if (file_size(file, &size) != CALL_OK || size > 64 * 1024) {
    return NULL;
  }
  char *text = malloc(size + 1);
  struct install_source source = {.handle = file, .bytes = size};
  if (!text || !install_source_read(&source, 0, text, size)) {
    free(text);
    return NULL;
  }
  text[size] = '\0';
  return text;
}

static bool read_rescue_list(handle_t boot, char ***names, size_t *count)
{
  handle_t share = HANDLE_INVALID, installer = HANDLE_INVALID, list = HANDLE_INVALID;
  char *text = NULL;
  bool ok = directory_lookup(boot, "share", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &share) == CALL_OK &&
      directory_lookup(share, "installer", DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &installer) == CALL_OK &&
      directory_lookup(installer, "rescue.list", DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &list) == CALL_OK &&
      (text = read_text(list)) != NULL;
  for (char *line = text; ok && line && *line;) {
    char *end = strchr(line, '\n');
    if (end) {
      *end = '\0';
    }
    if (*line) {
      ok = add_name(names, count, line);
    }
    line = end ? end + 1 : NULL;
  }
  free(text);
  const handle_t handles[] = {list, installer, share};
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (handles[i] != HANDLE_INVALID) {
      handle_close(handles[i]);
    }
  }
  return ok;
}

bool install_programs_list(handle_t boot, struct install_programs *programs)
{
  *programs = (struct install_programs){0};
  char **rescue = NULL;
  size_t rescue_count = 0;
  bool ok = read_rescue_list(boot, &rescue, &rescue_count);
  size_t capacity = 128;
  char *name = ok ? malloc(capacity) : NULL;
  ok = name != NULL;
  struct directory_cursor cursor = {0};
  while (ok) {
    struct directory_enumerate_reply entry;
    ok = directory_enumerate(boot, &cursor, name, capacity, &entry) == CALL_OK &&
        entry.outcome != DIRECTORY_CHANGED;
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      ok = larger != NULL;
      name = ok ? larger : name;
      capacity = ok ? entry.name_size : capacity;
      continue;
    }
    cursor = entry.cursor;
    size_t length = strlen(name), suffix = strlen(PROGRAM_SUFFIX);
    if (entry.kind != DIRECTORY_KIND_FILE || length <= suffix ||
        strcmp(name + length - suffix, PROGRAM_SUFFIX) ||
        listed(rescue, rescue_count, name)) {
      continue;
    }
    ok = add_name(&programs->names, &programs->count, name);
  }
  free(name);
  free_names(rescue, rescue_count);
  if (!ok) {
    install_programs_destroy(programs);
    return false;
  }
  qsort(programs->names, programs->count, sizeof(*programs->names), compare_names);
  return true;
}

void install_programs_destroy(struct install_programs *programs)
{
  free_names(programs->names, programs->count);
  *programs = (struct install_programs){0};
}

static bool newc_field(const uint8_t *header, unsigned index, uint32_t *value)
{
  *value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    uint8_t c = header[6 + index * 8 + i];
    unsigned digit = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
    if (digit == 16) {
      return false;
    }
    *value = *value << 4 | digit;
  }
  return true;
}

static size_t align4(size_t value)
{
  return (value + 3) & ~(size_t)3;
}

bool install_archive_filter(const struct install_source *archive,
    const struct install_programs *programs, struct install_source *rescue)
{
  *rescue = (struct install_source){.handle = HANDLE_INVALID};
  if (archive->bytes > SIZE_MAX) {
    return false;
  }
  size_t size = (size_t)archive->bytes;
  uint8_t *input = malloc(size), *output = malloc(size);
  if (!input || !output || !install_source_read(archive, 0, input, size)) {
    goto fail;
  }
  /* newc: each record is a 110-byte header, its name padded to four bytes
   * with the header, then its data padded to four bytes. */
  size_t used = 0;
  for (size_t offset = 0;;) {
    uint32_t file_size, name_size;
    if (size - offset < NEWC_HEADER_BYTES || memcmp(input + offset, "07070", 5) ||
        !newc_field(input + offset, 6, &file_size) ||
        !newc_field(input + offset, 11, &name_size) || !name_size) {
      goto fail;
    }
    size_t data = align4(offset + NEWC_HEADER_BYTES + name_size);
    size_t next = align4(data + file_size);
    if (data > size || next > size || next < data ||
        input[offset + NEWC_HEADER_BYTES + name_size - 1] != '\0') {
      goto fail;
    }
    const char *name = (const char *)input + offset + NEWC_HEADER_BYTES;
    bool trailer = !strcmp(name, NEWC_TRAILER);
    if (trailer || !listed(programs->names, programs->count, name)) {
      memcpy(output + used, input + offset, next - offset);
      used += next - offset;
    }
    offset = next;
    if (trailer) {
      break;
    }
  }
  free(input);
  rescue->memory = output;
  rescue->bytes = used;
  return true;

fail:
  fputs("installer: the boot archive is not a valid newc archive\n", stderr);
  free(input);
  free(output);
  return false;
}

/* Removes every file in DIRECTORY; revision directories hold only files. */
static bool clear_directory(handle_t directory)
{
  char **names = NULL;
  size_t count = 0, capacity = 128;
  char *name = malloc(capacity);
  bool ok = name != NULL;
  struct directory_cursor cursor = {0};
  while (ok) {
    struct directory_enumerate_reply entry;
    ok = directory_enumerate(directory, &cursor, name, capacity, &entry) == CALL_OK &&
        entry.outcome != DIRECTORY_CHANGED;
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      ok = larger != NULL;
      name = ok ? larger : name;
      capacity = ok ? entry.name_size : capacity;
      continue;
    }
    cursor = entry.cursor;
    ok = add_name(&names, &count, name);
  }
  for (size_t i = 0; ok && i < count; ++i) {
    ok = directory_remove(directory, names[i], DIRECTORY_KIND_FILE) == CALL_OK;
  }
  free(name);
  free_names(names, count);
  return ok;
}

static bool copy_program(handle_t boot, handle_t directory, const char *name, uint8_t *buffer)
{
  handle_t source = HANDLE_INVALID, target = HANDLE_INVALID;
  uint64_t size = 0;
  bool ok = directory_lookup(boot, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &source) == CALL_OK && file_size(source, &size) == CALL_OK &&
      directory_create(directory, name, DIRECTORY_KIND_FILE, FILE_RIGHTS, &target) == CALL_OK;
  struct install_source from = {.handle = source, .bytes = size};
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t length = size - offset < COPY_BYTES ? (size_t)(size - offset) : COPY_BYTES;
    size_t written = 0;
    ok = install_source_read(&from, offset, buffer, length) &&
        file_write(target, offset, buffer, length, &written) == CALL_OK && written == length;
    offset += length;
  }
  if (source != HANDLE_INVALID) {
    handle_close(source);
  }
  if (target != HANDLE_INVALID) {
    handle_close(target);
  }
  if (!ok) {
    fprintf(stderr, "installer: cannot copy %s into the bin volume\n", name);
  }
  return ok;
}

static bool verify_program(handle_t boot, handle_t directory, const char *name,
    uint8_t *expected, uint8_t *actual)
{
  handle_t source = HANDLE_INVALID, target = HANDLE_INVALID;
  uint64_t size = 0, copied = 0;
  bool ok = directory_lookup(boot, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &source) == CALL_OK &&
      directory_lookup(directory, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
        &target) == CALL_OK &&
      file_size(source, &size) == CALL_OK && file_size(target, &copied) == CALL_OK &&
      size == copied;
  struct install_source from = {.handle = source, .bytes = size};
  struct install_source to = {.handle = target, .bytes = size};
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t length = size - offset < COPY_BYTES ? (size_t)(size - offset) : COPY_BYTES;
    ok = install_source_read(&from, offset, expected, length) &&
        install_source_read(&to, offset, actual, length) && !memcmp(expected, actual, length);
    offset += length;
  }
  if (source != HANDLE_INVALID) {
    handle_close(source);
  }
  if (target != HANDLE_INVALID) {
    handle_close(target);
  }
  if (!ok) {
    fprintf(stderr, "installer: bin://%s does not match its source\n", name);
  }
  return ok;
}

static enum call_status open_bin(const struct install_disk *disk, handle_t *root)
{
  enum call_status status = disk_create_volume(disk->handle, INSTALL_POOL_PARTITION,
      INSTALL_BIN_VOLUME);
  if (status != CALL_OK && status != CALL_ALREADY_EXISTS) {
    fprintf(stderr, "installer: cannot create the bin volume (status %u)\n", status);
    return status;
  }
  status = disk_open_volume(disk->handle, INSTALL_POOL_PARTITION, INSTALL_BIN_VOLUME,
      BIN_RIGHTS, root);
  if (status != CALL_OK) {
    fprintf(stderr, "installer: cannot open the bin volume (status %u)\n", status);
  }
  return status;
}

bool install_programs_write(const struct install_disk *disk, handle_t boot,
    const struct install_programs *programs, const char *revision)
{
  handle_t root = HANDLE_INVALID, directory = HANDLE_INVALID;
  uint8_t *buffers = malloc(2 * COPY_BYTES);
  enum call_status status = buffers ? open_bin(disk, &root) : CALL_NO_MEMORY;
  if (status == CALL_OK) {
    status = directory_create(root, revision, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS, &directory);
    /* A rerun after an interrupted program stage rewrites the directory. */
    if (status == CALL_ALREADY_EXISTS) {
      status = directory_lookup(root, revision, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &directory);
      if (status == CALL_OK && !clear_directory(directory)) {
        status = CALL_IO;
      }
    }
  }
  bool ok = status == CALL_OK;
  if (!ok) {
    fprintf(stderr, "installer: cannot prepare bin://%s (status %u)\n", revision, status);
  }
  for (size_t i = 0; ok && i < programs->count; ++i) {
    ok = copy_program(boot, directory, programs->names[i], buffers);
  }
  if (ok) {
    status = directory_sync(directory);
    ok = status == CALL_OK;
    if (!ok) {
      fprintf(stderr, "installer: bin volume sync failed (status %u)\n", status);
    }
  }
  for (size_t i = 0; ok && i < programs->count; ++i) {
    ok = verify_program(boot, directory, programs->names[i], buffers, buffers + COPY_BYTES);
  }
  if (directory != HANDLE_INVALID) {
    handle_close(directory);
  }
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  free(buffers);
  return ok;
}

bool install_programs_cleanup(const struct install_disk *disk, const char *keep,
    const char *previous)
{
  handle_t root = HANDLE_INVALID;
  char **names = NULL;
  size_t count = 0, capacity = 128;
  char *name = malloc(capacity);
  bool ok = name && disk_open_volume(disk->handle, INSTALL_POOL_PARTITION,
      INSTALL_BIN_VOLUME, BIN_RIGHTS, &root) == CALL_OK;
  struct directory_cursor cursor = {0};
  while (ok) {
    struct directory_enumerate_reply entry;
    ok = directory_enumerate(root, &cursor, name, capacity, &entry) == CALL_OK &&
        entry.outcome != DIRECTORY_CHANGED;
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      ok = larger != NULL;
      name = ok ? larger : name;
      capacity = ok ? entry.name_size : capacity;
      continue;
    }
    cursor = entry.cursor;
    if (entry.kind == DIRECTORY_KIND_DIRECTORY && strcmp(name, keep) &&
        (!previous || strcmp(name, previous))) {
      ok = add_name(&names, &count, name);
    }
  }
  for (size_t i = 0; ok && i < count; ++i) {
    handle_t directory = HANDLE_INVALID;
    ok = directory_lookup(root, names[i], DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &directory) == CALL_OK && clear_directory(directory);
    if (directory != HANDLE_INVALID) {
      handle_close(directory);
    }
    ok = ok && directory_remove(root, names[i], DIRECTORY_KIND_DIRECTORY) == CALL_OK;
    if (ok) {
      printf("Removed old programs bin://%s\n", names[i]);
    }
  }
  if (ok && count) {
    ok = directory_sync(root) == CALL_OK;
  }
  free(name);
  free_names(names, count);
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  return ok;
}
