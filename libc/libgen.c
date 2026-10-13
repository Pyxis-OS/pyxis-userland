#include <libgen.h>
#include <string.h>
#include "working_path_internal.h"

static char current[] = ".";

/* A scheme root, or a single "/", ends the path and is never removed. */
static size_t root_length(const char *path)
{
  size_t root = libc_path_root_prefix(path);
  return root ? root : path[0] == '/' ? 1 : 0;
}

char *dirname(char *path)
{
  if (!path || !*path) {
    return current;
  }
  size_t root = root_length(path);
  size_t end = strlen(path);
  while (end > root && path[end - 1] == '/') {
    --end;
  }
  size_t separator = end;
  while (separator > root && path[separator - 1] != '/') {
    --separator;
  }
  if (separator == root) {
    if (!root) {
      return current;
    }
    path[root] = '\0';
    return path;
  }
  size_t last = separator - 1;
  while (last > root && path[last - 1] == '/') {
    --last;
  }
  path[last] = '\0';
  return path;
}

char *basename(char *path)
{
  if (!path || !*path) {
    return current;
  }
  size_t root = root_length(path);
  size_t end = strlen(path);
  while (end > root && path[end - 1] == '/') {
    --end;
  }
  if (end == root) {
    path[root] = '\0';
    return path;
  }
  path[end] = '\0';
  size_t start = end;
  while (start > root && path[start - 1] != '/') {
    --start;
  }
  return path + start;
}
