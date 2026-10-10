#include "installer.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_SUFFIX ".pxe"
#define BUNDLE_SUFFIX ".pxb"
#define NEWC_HEADER_BYTES 110u
#define NEWC_TRAILER "TRAILER!!!"
#define BIN_RIGHTS DIRECTORY_CONTENT_RIGHTS
#define SOURCE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
/* Installer settings, not ABI: bounded traversal and native destination names.
 * The fixed-depth heap stack owns at most one directory pair per level. */
#define TREE_MAX_DEPTH 32u
#define TREE_MAX_ENTRIES 65536u
#define TREE_NAME_BYTES (NPFS_NAME_MAX + 1)
/* One file call each way per chunk. */
#define COPY_BYTES (FILE_READ_MAX_BYTES < FILE_WRITE_MAX_BYTES ? FILE_READ_MAX_BYTES : \
    FILE_WRITE_MAX_BYTES)

/* One installer copy stage owns this display state. Observation failure never
 * changes the copy result; no filesystem sync is implied by the counters. */
static struct {
  handle_t clock;
  uint64_t bytes, last_ns;
  size_t files, total;
  bool terminal, active, enabled, total_known;
} progress;

void install_progress_end(void)
{
  if (progress.active) {
    putchar('\n');
    progress.active = false;
  }
}

static void progress_show(bool final)
{
  uint64_t now;
  if (!progress.enabled) {
    return;
  }
  if (clock_now(progress.clock, &now) != CALL_OK) {
    install_progress_end();
    progress.enabled = false;
    return;
  }
  uint64_t interval = progress.terminal ? UINT64_C(250000000) : UINT64_C(1000000000);
  if (!final && now - progress.last_ns < interval) {
    return;
  }
  progress.last_ns = now;
  const char *prefix = progress.terminal ? "\r" : "";
  const char *suffix = progress.terminal ? "" : "\n";
  unsigned long long mib = progress.bytes / INSTALL_MIB;
  int result = progress.total_known ?
      printf("%sbin://: %zu/%zu files, %llu MiB%s", prefix, progress.files,
          progress.total, mib, suffix) :
      printf("%sbin://: %zu files, %llu MiB%s", prefix, progress.files, mib, suffix);
  progress.active = progress.terminal;
  if (result < 0) {
    progress.enabled = false;
    install_progress_end();
  }
}

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

static int compare_programs(const void *left, const void *right)
{
  const struct install_program *a = left, *b = right;
  return strcmp(a->name, b->name);
}

static const struct install_program *find_program(const struct install_programs *programs,
    const char *name)
{
  for (size_t i = 0; i < programs->count; ++i) {
    if (!strcmp(programs->entries[i].name, name)) {
      return &programs->entries[i];
    }
  }
  return NULL;
}

static bool has_suffix(const char *name, const char *suffix)
{
  size_t length = strlen(name), suffix_length = strlen(suffix);
  return length > suffix_length && !strcmp(name + length - suffix_length, suffix);
}

static bool add_program(struct install_programs *programs, const char *name, uint64_t kind)
{
  if (programs->count == TREE_MAX_ENTRIES) {
    return false;
  }
  struct install_program *larger = realloc(programs->entries,
      (programs->count + 1) * sizeof(*larger));
  if (!larger) {
    return false;
  }
  programs->entries = larger;
  char *copy = strdup(name);
  if (!copy) {
    return false;
  }
  larger[programs->count++] = (struct install_program){.name = copy, .kind = kind};
  return true;
}

static bool read_entry(handle_t directory, const struct directory_cursor *cursor,
    char name[TREE_NAME_BYTES], struct directory_enumerate_reply *entry)
{
  enum call_status status = directory_enumerate(directory, cursor, name, TREE_NAME_BYTES, entry);
  if (status != CALL_OK) {
    install_progress_end();
    fprintf(stderr, "installer: cannot enumerate program directory (status %u)\n", status);
    return false;
  }
  if (entry->outcome == DIRECTORY_ENTRY || entry->outcome == DIRECTORY_END) {
    return true;
  }
  install_progress_end();
  if (entry->outcome == DIRECTORY_BUFFER_TOO_SMALL) {
    fprintf(stderr, "installer: program entry name exceeds %u bytes\n", NPFS_NAME_MAX);
  } else if (entry->outcome == DIRECTORY_CHANGED) {
    fputs("installer: program directory changed during traversal\n", stderr);
  } else {
    fputs("installer: invalid program directory enumeration reply\n", stderr);
  }
  return false;
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
  char name[TREE_NAME_BYTES];
  size_t entries = 0;
  struct directory_cursor cursor = {0};
  while (ok) {
    struct directory_enumerate_reply entry;
    ok = read_entry(boot, &cursor, name, &entry);
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    cursor = entry.cursor;
    if (++entries > TREE_MAX_ENTRIES) {
      ok = false;
      break;
    }
    bool program = entry.kind == DIRECTORY_KIND_FILE && has_suffix(name, PROGRAM_SUFFIX) &&
        !listed(rescue, rescue_count, name);
    bool bundle = entry.kind == DIRECTORY_KIND_DIRECTORY && has_suffix(name, BUNDLE_SUFFIX);
    if (!program && !bundle) {
      continue;
    }
    ok = add_program(programs, name, entry.kind);
  }
  free_names(rescue, rescue_count);
  if (!ok) {
    install_programs_destroy(programs);
    return false;
  }
  qsort(programs->entries, programs->count, sizeof(*programs->entries), compare_programs);
  return true;
}

void install_programs_destroy(struct install_programs *programs)
{
  for (size_t i = 0; i < programs->count; ++i) {
    free(programs->entries[i].name);
  }
  free(programs->entries);
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

static bool program_path(const struct install_programs *programs, const char *path)
{
  for (size_t i = 0; i < programs->count; ++i) {
    const struct install_program *program = &programs->entries[i];
    size_t length = strlen(program->name);
    if (!strncmp(path, program->name, length) &&
        (path[length] == '\0' ||
          (program->kind == DIRECTORY_KIND_DIRECTORY && path[length] == '/'))) {
      return true;
    }
  }
  return false;
}

bool install_archive_filter(const struct install_source *archive,
    struct install_programs *programs, struct install_source *rescue)
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
  size_t used = 0, files = 0;
  bool files_known = true;
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
    /* Boot normalizes leading ./; progress must not claim a raw-path total. */
    if (!strncmp(name, "./", 2)) {
      files_known = false;
    }
    bool selected = !trailer && program_path(programs, name);
    if (selected) {
      uint32_t mode;
      if (!newc_field(input + offset, 1, &mode)) {
        files_known = false;
      } else if ((mode & 0170000) == 0100000) {
        ++files;
      }
    }
    if (!selected) {
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
  programs->files = files;
  programs->files_known = files_known;
  return true;

fail:
  fputs("installer: the boot archive is not a valid newc archive\n", stderr);
  free(input);
  free(output);
  return false;
}

struct tree_level {
  handle_t source, target;
  struct directory_cursor cursor;
  size_t entries;
  char name[TREE_NAME_BYTES];
};

/* Mutations invalidate enumeration cursors. Restart at zero after each child
 * removal; descend before removing directories, including empty ones. */
static bool clear_directory(handle_t directory)
{
  struct tree_level *levels = calloc(TREE_MAX_DEPTH + 1, sizeof(*levels));
  if (!levels) {
    return false;
  }
  levels[0].target = directory;
  unsigned depth = 0;
  size_t entries = 0;
  bool ok = true;
  while (ok) {
    struct tree_level *level = &levels[depth];
    const struct directory_cursor start = {0};
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    ok = read_entry(level->target, &start, name, &entry);
    if (!ok) {
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      if (!depth) {
        break;
      }
      handle_close(level->target);
      --depth;
      ok = directory_remove(levels[depth].target, level->name,
          DIRECTORY_KIND_DIRECTORY) == CALL_OK;
      continue;
    }
    if (++entries > TREE_MAX_ENTRIES) {
      ok = false;
      break;
    }
    if (entry.kind == DIRECTORY_KIND_FILE) {
      ok = directory_remove(level->target, name, DIRECTORY_KIND_FILE) == CALL_OK;
    } else if (entry.kind == DIRECTORY_KIND_DIRECTORY && depth < TREE_MAX_DEPTH) {
      handle_t child = HANDLE_INVALID;
      ok = directory_lookup(level->target, name, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &child) == CALL_OK;
      if (ok) {
        ++depth;
        levels[depth] = (struct tree_level){.target = child};
        strcpy(levels[depth].name, name);
      }
    } else {
      ok = false;
    }
  }
  while (depth) {
    handle_close(levels[depth--].target);
  }
  if (!ok) {
    fputs("installer: cannot clear program tree within traversal limits\n", stderr);
  }
  free(levels);
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
    if (ok) {
      progress.bytes += written;
      progress_show(false);
    }
    offset += length;
  }
  if (ok) {
    ++progress.files;
    progress_show(false);
  }
  if (source != HANDLE_INVALID) {
    handle_close(source);
  }
  if (target != HANDLE_INVALID) {
    handle_close(target);
  }
  if (!ok) {
    install_progress_end();
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

static bool verify_entry_count(handle_t directory, size_t expected)
{
  struct directory_cursor cursor = {0};
  size_t count = 0;
  for (;;) {
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    if (!read_entry(directory, &cursor, name, &entry)) {
      return false;
    }
    if (entry.outcome == DIRECTORY_END) {
      return count == expected;
    }
    if (++count > expected) {
      return false;
    }
    cursor = entry.cursor;
  }
}

/* The boot archive is immutable and the admitted target pool is offline.
 * Root selection comes from PROGRAMS; every descendant belongs to its unit.
 * Verification checks every source name at its exact kind, then independently
 * counts destination entries at each level to detect extras without sorting. */
static bool walk_programs(handle_t boot, handle_t directory,
    const struct install_programs *programs, uint8_t *buffers, bool verify)
{
  struct tree_level *levels = calloc(TREE_MAX_DEPTH + 1, sizeof(*levels));
  if (!levels) {
    return false;
  }
  levels[0] = (struct tree_level){.source = boot, .target = directory};
  unsigned depth = 0;
  size_t entries = 0, root_entries = 0;
  bool ok = true;
  while (ok) {
    struct tree_level *level = &levels[depth];
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    ok = read_entry(level->source, &level->cursor, name, &entry);
    if (!ok) {
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      if (verify && !verify_entry_count(level->target, level->entries)) {
        fputs("installer: installed program directory has missing or extra entries\n", stderr);
        ok = false;
        break;
      }
      if (!depth) {
        ok = root_entries == programs->count;
        break;
      }
      handle_close(level->source);
      handle_close(level->target);
      --depth;
      continue;
    }
    level->cursor = entry.cursor;
    if (!depth) {
      const struct install_program *program = find_program(programs, name);
      if (!program) {
        continue;
      }
      if (program->kind != entry.kind) {
        ok = false;
        break;
      }
      ++root_entries;
    }
    if (++entries > TREE_MAX_ENTRIES) {
      install_progress_end();
      fputs("installer: program trees exceed the entry limit\n", stderr);
      ok = false;
      break;
    }
    ++level->entries;
    if (entry.kind == DIRECTORY_KIND_FILE) {
      ok = verify ? verify_program(level->source, level->target, name,
          buffers, buffers + COPY_BYTES) :
          copy_program(level->source, level->target, name, buffers);
    } else if (entry.kind == DIRECTORY_KIND_DIRECTORY) {
      if (depth == TREE_MAX_DEPTH) {
        install_progress_end();
        fputs("installer: program tree exceeds the directory depth limit\n", stderr);
        ok = false;
        break;
      }
      handle_t source = HANDLE_INVALID, target = HANDLE_INVALID;
      ok = directory_lookup(level->source, name, DIRECTORY_KIND_DIRECTORY, SOURCE_RIGHTS,
          &source) == CALL_OK;
      if (ok) {
        ok = (verify ? directory_lookup(level->target, name, DIRECTORY_KIND_DIRECTORY,
              SOURCE_RIGHTS, &target) :
            directory_create(level->target, name, DIRECTORY_KIND_DIRECTORY,
              BIN_RIGHTS, &target)) == CALL_OK;
      }
      if (!ok) {
        install_progress_end();
        fprintf(stderr, "installer: cannot %s program directory %s\n",
            verify ? "verify" : "copy", name);
        if (source != HANDLE_INVALID) {
          handle_close(source);
        }
        break;
      }
      ++depth;
      levels[depth] = (struct tree_level){.source = source, .target = target};
      strcpy(levels[depth].name, name);
    } else {
      install_progress_end();
      fprintf(stderr, "installer: unsupported program entry kind for %s\n", name);
      ok = false;
    }
  }
  while (depth) {
    handle_close(levels[depth].source);
    handle_close(levels[depth].target);
    --depth;
  }
  free(levels);
  if (!ok) {
    install_progress_end();
    fprintf(stderr, "installer: cannot %s complete program trees in the bin volume\n",
        verify ? "verify" : "copy");
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
    const struct install_programs *programs, const char *revision, handle_t clock)
{
  handle_t root = HANDLE_INVALID, directory = HANDLE_INVALID;
  progress = (typeof(progress)){.clock = clock, .terminal = isatty(STDOUT_FILENO),
    .enabled = true, .total = programs->files, .total_known = programs->files_known};
  progress.enabled = clock_now(clock, &progress.last_ns) == CALL_OK;
  puts("Preparing bin revision directory...");
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
  if (ok) {
    ok = walk_programs(boot, directory, programs, buffers, false);
    if (ok) {
      progress_show(true);
    }
    install_progress_end();
    progress.enabled = false;
  }
  if (ok) {
    puts("Flushing program pool...");
    status = directory_sync(directory);
    ok = status == CALL_OK;
    if (!ok) {
      fprintf(stderr, "installer: bin volume sync failed (status %u)\n", status);
    }
  }
  if (ok) {
    puts("Verifying copied programs...");
    ok = walk_programs(boot, directory, programs, buffers, true);
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
  size_t entries = 0;
  bool changed = false;
  bool ok = disk_open_volume(disk->handle, INSTALL_POOL_PARTITION,
      INSTALL_BIN_VOLUME, BIN_RIGHTS, &root) == CALL_OK;
  struct directory_cursor cursor = {0};
  while (ok) {
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    ok = read_entry(root, &cursor, name, &entry);
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (++entries > TREE_MAX_ENTRIES) {
      ok = false;
      break;
    }
    cursor = entry.cursor;
    if (entry.kind != DIRECTORY_KIND_DIRECTORY || !strcmp(name, keep) ||
        (previous && !strcmp(name, previous))) {
      continue;
    }
    handle_t directory = HANDLE_INVALID;
    ok = directory_lookup(root, name, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &directory) == CALL_OK && clear_directory(directory);
    if (directory != HANDLE_INVALID) {
      handle_close(directory);
    }
    ok = ok && directory_remove(root, name, DIRECTORY_KIND_DIRECTORY) == CALL_OK;
    if (ok) {
      changed = true;
      printf("Removed old programs bin://%s\n", name);
      cursor = (struct directory_cursor){0};
    }
  }
  if (ok && changed) {
    ok = directory_sync(root) == CALL_OK;
  }
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  return ok;
}
