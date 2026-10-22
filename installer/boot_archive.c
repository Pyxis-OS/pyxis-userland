#include "installer.h"
#include "literal.h"
#include <pxe/p1f.h>

#include <directory.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The newc boot archive filter: splits program records from the rescue
 * archive and checks the rescue identity. Shared by the installer's program
 * copy and the boot publisher. */
#define NEWC_HEADER_BYTES 110u
#define NEWC_TRAILER "TRAILER!!!"

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

static bool align4(size_t value, size_t *aligned)
{
  if (value > SIZE_MAX - 3) {
    return false;
  }
  *aligned = (value + 3) & ~(size_t)3;
  return true;
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

static bool archive_filter(const struct install_source *archive,
    struct install_programs *programs, struct install_source *rescue)
{
  install_progress_begin("Reading source boot archive", INSTALL_PROGRESS_SOURCE_READ, NULL);
  install_progress_bytes_total(archive->bytes);
  *rescue = (struct install_source){.handle = HANDLE_INVALID};
  if (archive->bytes > SIZE_MAX) {
    install_progress_finish();
    return false;
  }
  size_t size = (size_t)archive->bytes;
  size_t capacity = size;
  uint8_t *input = malloc(size), *output = malloc(capacity ? capacity : 1);
  if (!input || !output || !install_source_read(archive, 0, input, size)) {
    goto fail;
  }
  install_progress_begin("Filtering rescue boot archive", INSTALL_PROGRESS_WORK, "records");
  /* newc: each record is a 110-byte header, its name padded to four bytes
   * with the header, then its data padded to four bytes. */
  size_t used = 0, files = 0;
  uint64_t program_bytes = 0;
  bool totals_known = true;
  for (size_t offset = 0;;) {
    uint32_t file_size, name_size;
    if (size - offset < NEWC_HEADER_BYTES || memcmp(input + offset, "070701", 6) ||
        !newc_field(input + offset, 6, &file_size) ||
        !newc_field(input + offset, 11, &name_size) || !name_size) {
      goto fail;
    }
    size_t name_start = offset + NEWC_HEADER_BYTES;
    size_t data, next;
    if (name_size > size - name_start ||
        !align4(name_start + name_size, &data) || data > size || file_size > size - data ||
        !align4(data + file_size, &next) || next > size) {
      goto fail;
    }
    const char *name = (const char *)input + name_start;
    size_t name_length = strnlen(name, name_size);
    if (!name_length || name_length == name_size) {
      goto fail;
    }
    for (size_t i = name_length; i < name_size; ++i) {
      if (name[i]) {
        goto fail;
      }
    }
    bool trailer = !strcmp(name, NEWC_TRAILER);
    /* Boot normalizes leading ./; progress must not claim a raw-path total. */
    if (!strncmp(name, "./", 2)) {
      totals_known = false;
    }
    bool selected = !trailer && program_path(programs, name);
    if (selected) {
      uint32_t mode;
      if (!newc_field(input + offset, 1, &mode)) {
        totals_known = false;
      } else if ((mode & 0170000) == 0100000) {
        ++files;
        program_bytes += file_size;
      }
    }
    if (!selected) {
      if (next - offset > capacity - used) {
        goto fail;
      }
      memcpy(output + used, input + offset, next - offset);
      used += next - offset;
    }
    install_progress_advance(next - offset, 1);
    offset = next;
    if (trailer) {
      if (file_size) {
        goto fail;
      }
      for (size_t i = offset; i < size; ++i) {
        if (input[i]) {
          goto fail;
        }
      }
      break;
    }
  }
  free(input);
  rescue->memory = output;
  rescue->bytes = used;
  programs->files = files;
  programs->bytes = program_bytes;
  programs->totals_known = totals_known;
  install_progress_finish();
  return true;

fail:
  install_progress_pause();
  fputs("installer: boot archive is invalid or unsupported\n", stderr);
  free(input);
  free(output);
  install_progress_finish();
  return false;
}


bool install_archive_filter(const struct install_source *archive,
    struct install_programs *programs, struct install_source *rescue)
{
  return archive_filter(archive, programs, rescue);
}

bool install_archive_rescue_identity(const struct install_source *archive)
{
  struct install_programs none = {0};
  struct install_source checked = {.handle = HANDLE_INVALID};
  if (!archive_filter(archive, &none, &checked)) {
    return false;
  }
  bool initializer = false, defaults = false, duplicate = false;
  bool seen_initializer = false, seen_defaults = false;
  for (size_t offset = 0; offset < checked.bytes;) {
    uint32_t size, name_size;
    const uint8_t *header = checked.memory + offset;
    if (!newc_field(header, 6, &size) || !newc_field(header, 11, &name_size)) {
      duplicate = true;
      break;
    }
    const char *name = (char *)header + NEWC_HEADER_BYTES;
    size_t data;
    if (!align4(offset + NEWC_HEADER_BYTES + name_size, &data)) {
      duplicate = true;
      break;
    }
    if (!strcmp(name, NEWC_TRAILER)) {
      break;
    }
    if (!strncmp(name, "./", 2)) {
      name += 2;
    }
    if (!strcmp(name, "boot-init.pxe")) {
      duplicate |= seen_initializer;
      seen_initializer = true;
      struct p1f_header image = {0};
      if (size >= sizeof(image)) {
        memcpy(&image, checked.memory + data, sizeof(image));
      }
      initializer = image.magic == P1F_MAGIC && image.segment_count &&
          image.segment_count <= (size - sizeof(image)) / sizeof(struct p1f_segment);
    } else if (!strcmp(name, "config/installed.lua")) {
      duplicate |= seen_defaults;
      seen_defaults = true;
      struct literal *document = NULL;
      struct literal_diagnostic diagnostic;
      defaults = literal_parse((char *)checked.memory + data, size, &document, &diagnostic);
      literal_free(document);
    }
    if (!align4(data + size, &offset)) {
      duplicate = true;
      break;
    }
  }
  free(checked.memory);
  return initializer && defaults && !duplicate;
}
