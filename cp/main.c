#include "cp.h"
#include "../common/directory.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
  fputs("usage: cp [--] source-file... destination\n", stderr);
  return EXIT_FAILURE;
}

static bool valid_leaf(const char *name)
{
  return *name && strcmp(name, ".") && strcmp(name, "..");
}

static enum call_status destination_parent(const char *path, handle_t *parent,
    const char **name)
{
  const char *separator = strrchr(path, '/');
  *name = separator ? separator + 1 : path;
  if (!valid_leaf(*name)) {
    return CALL_BAD_REQUEST;
  }
  if (!separator) {
    return resolve_directory(".", CP_DIRECTORY_RIGHTS, parent);
  }
  size_t length = separator - path + 1;
  char *directory = malloc(length + 1);
  if (!directory) {
    return CALL_NO_MEMORY;
  }
  memcpy(directory, path, length);
  directory[length] = '\0';
  enum call_status status = resolve_directory(directory, CP_DIRECTORY_RIGHTS, parent);
  free(directory);
  return status;
}

int main(int argc, char **argv)
{
  bool options = true;
  int operands = 0;
  for (int i = 1; i < argc; ++i) {
    if (options && !strcmp(argv[i], "--")) {
      options = false;
      continue;
    }
    if (options && argv[i][0] == '-' && argv[i][1]) {
      fprintf(stderr, "cp: unsupported option: %s\n", argv[i]);
      return usage();
    }
    argv[++operands] = argv[i];
  }
  if (operands < 2) {
    return usage();
  }

  const char *destination = argv[operands];
  const char *name = NULL;
  handle_t parent = HANDLE_INVALID;
  enum call_status status = resolve_directory(destination, CP_DIRECTORY_RIGHTS, &parent);
  bool directory = status == CALL_OK;
  if (!directory && operands == 2 &&
      (status == CALL_WRONG_TYPE || status == CALL_NOT_FOUND)) {
    status = destination_parent(destination, &parent, &name);
  }
  if (status != CALL_OK) {
    report_directory_error("cp", destination, status);
    if (operands > 2) {
      fputs("cp: several sources require an existing destination directory\n", stderr);
    }
    return EXIT_FAILURE;
  }

  int result = EXIT_SUCCESS;
  for (int i = 1; i < operands; ++i) {
    const char *source = argv[i];
    if (directory) {
      const char *separator = strrchr(source, '/');
      name = separator ? separator + 1 : source;
      if (!valid_leaf(name)) {
        fprintf(stderr, "cp: %s: No file name for directory destination\n", source);
        result = EXIT_FAILURE;
        if (ferror(stderr)) {
          break;
        }
        continue;
      }
    }
    enum cp_result copied = cp_copy_file(source, parent, name, destination);
    if (copied != CP_SUCCESS) {
      result = EXIT_FAILURE;
    }
    if (copied == CP_UNCERTAIN || ferror(stderr)) {
      break;
    }
  }
  if (handle_close(parent) != 0) {
    report_directory_error("cp", destination, CALL_BAD_HANDLE);
    result = EXIT_FAILURE;
  }
  return result;
}
