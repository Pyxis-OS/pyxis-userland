#include "../common/directory.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum call_status create_directory(const char *path)
{
  if (!*path || *path == '/') {
    return CALL_BAD_REQUEST;
  }
  char *copy = strdup(path);
  if (!copy) {
    return CALL_NO_MEMORY;
  }
  /* Strip trailing separators without consuming a scheme's :// prefix. */
  char *start = copy;
  while (*start && *start != '/' && *start != ':') {
    ++start;
  }
  start = *start == ':' && start[1] == '/' && start[2] == '/' ? start + 3 : copy;
  char *end = copy + strlen(copy);
  while (end > start && end[-1] == '/') {
    *--end = '\0';
  }

  enum call_status status;
  handle_t directory = HANDLE_INVALID;
  char *slash = strrchr(copy, '/');
  const char *name = slash ? slash + 1 : copy;
  if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    /* Resolve special components rather than accidentally creating literal
     * dot entries or skipping a missing ancestor in paths such as missing/.. */
    status = resolve_directory(copy, 0, &directory);
    if (status == CALL_OK) {
      status = CALL_ALREADY_EXISTS;
    }
    goto done;
  }

  char *parent = slash ? strndup(copy, name - copy) : strdup(".");
  if (!parent) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  status = resolve_directory(parent, DIRECTORY_RIGHT_CREATE, &directory);
  free(parent);
  if (status == CALL_OK) {
    handle_t child;
    status = directory_create(directory, name, DIRECTORY_KIND_DIRECTORY, 0, &child);
    if (status == CALL_OK && handle_close(child) != 0) {
      status = CALL_BAD_HANDLE;
    }
  }

done:
  if (directory != HANDLE_INVALID && handle_close(directory) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  free(copy);
  return status;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fputs("usage: mkdir path...\n", stderr);
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    enum call_status status = create_directory(argv[i]);
    if (status != CALL_OK) {
      if (report_directory_error("mkdir", argv[i], status) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
    }
  }
  return result;
}
