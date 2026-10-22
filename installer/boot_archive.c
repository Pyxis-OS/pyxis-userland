#include "installer.h"

#include <directory.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The newc boot archive filter: splits program records from the rescue
 * archive. Shared by the installer's program copy and the boot publisher. */
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

bool install_archive_filter(const struct install_source *archive,
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

