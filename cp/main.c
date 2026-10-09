#include "cp.h"
#include "../common/directory.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
  fputs("usage: cp [-r] [--] source... destination\n", stderr);
  return EXIT_FAILURE;
}

static bool valid_leaf(const char *name)
{
  return *name && strcmp(name, ".") && strcmp(name, "..");
}

/* The last component of a directory source, ignoring trailing slashes but
 * never reaching into a scheme's :// prefix. Empty when there is none. */
static size_t directory_leaf(const char *path, const char **leaf)
{
  const char *start = path;
  while (*start && *start != '/' && *start != ':') {
    ++start;
  }
  start = *start == ':' && start[1] == '/' && start[2] == '/' ? start + 3 : path;
  size_t end = strlen(path);
  while (end > (size_t)(start - path) && path[end - 1] == '/') {
    --end;
  }
  size_t begin = end;
  while (begin > (size_t)(start - path) && path[begin - 1] != '/') {
    --begin;
  }
  *leaf = path + begin;
  return end - begin;
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
  bool recursive = false;
  int operands = 0;
  for (int i = 1; i < argc; ++i) {
    if (options && !strcmp(argv[i], "--")) {
      options = false;
      continue;
    }
    if (options && !strcmp(argv[i], "-r")) {
      recursive = true;
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
    if (recursive) {
      handle_t tree;
      status = resolve_directory(source, CP_SOURCE_RIGHTS, &tree);
      if (status == CALL_OK) {
        char leaf[CP_NAME_BYTES];
        char shown[CP_PATH_BYTES];
        const char *root = name;
        if (directory) {
          const char *start;
          size_t length = directory_leaf(source, &start);
          if (!length || (length == 1 && start[0] == '.') ||
              (length == 2 && start[0] == '.' && start[1] == '.')) {
            fprintf(stderr, "cp: %s: No file name for directory destination\n", source);
            handle_close(tree);
            result = EXIT_FAILURE;
            if (ferror(stderr)) {
              break;
            }
            continue;
          }
          if (length >= sizeof(leaf)) {
            fprintf(stderr, "cp: %s: Entry name longer than 255 bytes\n", source);
            handle_close(tree);
            result = EXIT_FAILURE;
            continue;
          }
          memcpy(leaf, start, length);
          leaf[length] = '\0';
          root = leaf;
          size_t used = strlen(destination);
          snprintf(shown, sizeof(shown), "%s%s%s", destination,
              used && destination[used - 1] == '/' ? "" : "/", leaf);
        } else {
          snprintf(shown, sizeof(shown), "%s", destination);
        }
        enum cp_result copied = cp_copy_tree(tree, source, parent, root, shown);
        if (copied != CP_SUCCESS) {
          result = EXIT_FAILURE;
        }
        if (copied == CP_UNCERTAIN || ferror(stderr)) {
          break;
        }
        continue;
      }
      if (status != CALL_WRONG_TYPE) {
        report_directory_error("cp", source, status);
        result = EXIT_FAILURE;
        if (ferror(stderr)) {
          break;
        }
        continue;
      }
    }
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
