#include "encode.h"
#include "../common/directory.h"
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <inttypes.h>
#include <screen_capture.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define TEMPORARY_ATTEMPTS 64
#define TEMPORARY_NAME_BYTES (sizeof(".screenshot-") + 16)

static enum call_status destination_parent(const char *path, handle_t *parent,
    const char **name)
{
  const char *separator = strrchr(path, '/');
  *name = separator ? separator + 1 : path;
  if (!**name || !strcmp(*name, ".") || !strcmp(*name, "..")) {
    return CALL_BAD_REQUEST;
  }
  if (!separator) {
    return resolve_directory(".", PARENT_RIGHTS, parent);
  }
  size_t length = separator - path + 1;
  char *directory = malloc(length + 1);
  if (!directory) {
    return CALL_NO_MEMORY;
  }
  memcpy(directory, path, length);
  directory[length] = '\0';
  enum call_status status = resolve_directory(directory, PARENT_RIGHTS, parent);
  free(directory);
  return status;
}

static bool reserve_temporary(handle_t parent, const char *name,
    const char *destination, char *temporary, handle_t *file)
{
  for (unsigned attempt = 0; attempt < TEMPORARY_ATTEMPTS; ++attempt) {
    snprintf(temporary, TEMPORARY_NAME_BYTES, ".screenshot-%016" PRIx64,
        (uint64_t)attempt);
    if (!strcmp(temporary, name)) {
      continue;
    }
    enum call_status status = directory_create(parent, temporary,
        DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, file);
    if (status == CALL_OK) {
      return true;
    }
    if (status == CALL_ALREADY_EXISTS) {
      continue;
    }
    report_directory_error("screenshot", destination, status);
    fprintf(stderr, "screenshot: %s: Temporary creation not confirmed; %s may remain\n",
        destination, temporary);
    return false;
  }
  fprintf(stderr, "screenshot: %s: Cannot reserve a temporary name after %u candidates\n",
      destination, TEMPORARY_ATTEMPTS);
  return false;
}

static bool close_owned(handle_t *handle, const char *description)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  if (handle_close(owned) != 0) {
    report_directory_error("screenshot", description, CALL_BAD_HANDLE);
    return false;
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fputs("usage: screenshot PATH\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t capture = startup_resource("screen_capture");
  if (capture == HANDLE_INVALID) {
    fputs("screenshot: screen_capture resource unavailable\n", stderr);
    return EXIT_FAILURE;
  }

  const char *destination = argv[1], *name;
  handle_t parent = HANDLE_INVALID, output = HANDLE_INVALID;
  struct screen_capture_reply snapshot = {0};
  char temporary[TEMPORARY_NAME_BYTES];
  bool reserved = false;
  int result = EXIT_FAILURE;
  enum call_status status = destination_parent(destination, &parent, &name);
  if (status != CALL_OK) {
    report_directory_error("screenshot", destination, status);
    return EXIT_FAILURE;
  }
  status = screen_capture_frame(capture, &snapshot);
  if (status != CALL_OK) {
    report_directory_error("screenshot", "screen capture", status);
    goto done;
  }
  if (!reserve_temporary(parent, name, destination, temporary, &output)) {
    goto done;
  }
  reserved = true;
  bool encoded = screenshot_encode(&snapshot, output, destination);
  bool snapshot_closed = close_owned(&snapshot.file, "capture FILE");
  bool output_closed = close_owned(&output, destination);
  if (!encoded || !snapshot_closed || !output_closed) {
    goto done;
  }

  status = directory_rename(parent, temporary, parent, name, DIRECTORY_RENAME_REPLACE);
  /* A failed publication reply cannot establish either name's ownership. */
  reserved = false;
  if (status != CALL_OK) {
    report_directory_error("screenshot", destination, status);
    fprintf(stderr, "screenshot: %s: Publication not confirmed; check %s and temporary %s\n",
        destination, name, temporary);
    goto done;
  }
  result = EXIT_SUCCESS;

done:
  bool snapshot_closed_final = close_owned(&snapshot.file, "capture FILE");
  bool output_closed_final = close_owned(&output, destination);
  if (reserved) {
    status = directory_remove(parent, temporary, DIRECTORY_KIND_FILE);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      report_directory_error("screenshot", destination, status);
      fprintf(stderr, "screenshot: %s: Partial temporary file %s may remain\n",
          destination, temporary);
    }
  }
  bool parent_closed = close_owned(&parent, destination);
  if (!snapshot_closed_final || !output_closed_final || !parent_closed) {
    result = EXIT_FAILURE;
  }
  return result;
}
