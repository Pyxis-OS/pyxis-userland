#include "cp.h"
#include "../common/directory.h"
#include <file.h>
#include <handle.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TEMPORARY_ATTEMPTS 64
#define TEMPORARY_NAME_BYTES 21
#define COPY_BYTES (FILE_READ_MAX_BYTES < FILE_WRITE_MAX_BYTES ? FILE_READ_MAX_BYTES : \
    FILE_WRITE_MAX_BYTES)

static unsigned char buffer[COPY_BYTES];
static uint64_t next_temporary;

static enum cp_result reserve_temporary(handle_t parent, const char *name,
    const char *destination, char *temporary, handle_t *file)
{
  *file = HANDLE_INVALID;
  for (unsigned attempt = 0; attempt < TEMPORARY_ATTEMPTS; ++attempt) {
    if (next_temporary == UINT64_MAX) {
      break;
    }
    snprintf(temporary, TEMPORARY_NAME_BYTES, ".cp-%016" PRIx64, next_temporary++);
    if (!strcmp(temporary, name)) {
      continue;
    }
    enum call_status status = directory_create(parent, temporary,
        DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, file);
    if (status == CALL_OK) {
      return CP_SUCCESS;
    }
    if (status == CALL_ALREADY_EXISTS) {
      continue;
    }
    report_directory_error("cp", destination, status);
    /* A failed create may have effects without returning an owned handle.
     * Never remove a name whose reservation was not confirmed. */
    fprintf(stderr, "cp: %s: Temporary creation not confirmed; %s may remain\n",
        destination, temporary);
    return CP_UNCERTAIN;
  }
  fprintf(stderr, "cp: %s: Cannot reserve a .cp- temporary name after %u candidates\n",
      destination, TEMPORARY_ATTEMPTS);
  return CP_FAILED;
}

static enum cp_result copy_bytes(handle_t input, handle_t output, uint64_t size,
    const char *source, const char *destination)
{
  uint64_t offset = 0;
  while (offset < size) {
    size_t capacity = size - offset < sizeof(buffer) ?
        (size_t)(size - offset) : sizeof(buffer);
    size_t count;
    enum call_status status = file_read(input, offset, buffer, capacity, &count);
    if (status != CALL_OK) {
      report_directory_error("cp", source, status);
      return CP_FAILED;
    }
    if (count == 0) {
      fprintf(stderr, "cp: %s: Source ended before its initial size\n", source);
      return CP_FAILED;
    }
    size_t written = 0;
    while (written < count) {
      size_t progress;
      status = file_write(output, offset + written, buffer + written, count - written,
          &progress);
      if (status != CALL_OK) {
        report_directory_error("cp", destination, status);
        return status == CALL_OUTCOME_UNKNOWN ? CP_UNCERTAIN : CP_FAILED;
      }
      written += progress;
    }
    offset += count;
  }
  return CP_SUCCESS;
}

static bool close_file(handle_t *file, const char *path)
{
  if (*file == HANDLE_INVALID) {
    return true;
  }
  handle_t owned = *file;
  *file = HANDLE_INVALID;
  if (handle_close(owned) != 0) {
    report_directory_error("cp", path, CALL_BAD_HANDLE);
    return false;
  }
  return true;
}

enum cp_result cp_copy_file(const char *source, handle_t parent, const char *name,
    const char *destination)
{
  handle_t input;
  enum call_status status = resolve_file(source, FILE_RIGHT_READ, &input);
  if (status != CALL_OK) {
    report_directory_error("cp", source, status);
    return CP_FAILED;
  }
  enum cp_result result = CP_FAILED;
  handle_t output = HANDLE_INVALID;
  bool reserved = false;
  char temporary[TEMPORARY_NAME_BYTES];
  uint64_t size;
  status = file_size(input, &size);
  if (status != CALL_OK) {
    report_directory_error("cp", source, status);
    goto done;
  }
  result = reserve_temporary(parent, name, destination, temporary, &output);
  if (result != CP_SUCCESS) {
    goto done;
  }
  reserved = true;
  /* Bound the copy by the initially observed size. Retaining input avoids
   * truncating a source alias, but does not snapshot concurrent writes. */
  result = copy_bytes(input, output, size, source, destination);
  bool input_closed = close_file(&input, source);
  bool output_closed = close_file(&output, destination);
  if (!input_closed || !output_closed) {
    if (result == CP_SUCCESS) {
      result = CP_FAILED;
    }
  }
  if (result != CP_SUCCESS) {
    goto done;
  }

  status = directory_rename(parent, temporary, parent, name, DIRECTORY_RENAME_REPLACE);
  /* Once publication was attempted, a failed reply cannot justify removing
   * either name. Another cp may already have reused a vacated temporary name. */
  reserved = false;
  if (status != CALL_OK) {
    report_directory_error("cp", destination, status);
    fprintf(stderr, "cp: %s: Publication not confirmed; check %s and temporary %s\n",
        destination, name, temporary);
    result = CP_UNCERTAIN;
  }

done:
  bool final_input_closed = close_file(&input, source);
  bool final_output_closed = close_file(&output, destination);
  if (!final_input_closed || !final_output_closed) {
    if (result == CP_SUCCESS) {
      result = CP_FAILED;
    }
  }
  if (reserved) {
    status = directory_remove(parent, temporary, DIRECTORY_KIND_FILE);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      report_directory_error("cp", destination, status);
      fprintf(stderr, "cp: %s: Partial temporary file %s may remain\n", destination, temporary);
      result = CP_UNCERTAIN;
    }
  }
  return result;
}
