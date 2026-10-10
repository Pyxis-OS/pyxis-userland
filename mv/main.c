#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool is_directory(const char *path)
{
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* The last component of SOURCE, without trailing slashes. A scheme root such
 * as home:// has none. */
static const char *base_name(const char *source, size_t *length)
{
  size_t end = strlen(source);
  while (end && source[end - 1] == '/') {
    --end;
  }
  if (!end || source[end - 1] == ':') {
    return NULL;
  }
  size_t start = end;
  while (start && source[start - 1] != '/') {
    --start;
  }
  *length = end - start;
  return source + start;
}

static char *join(const char *directory, const char *name, size_t length)
{
  size_t base = strlen(directory);
  bool slash = base && directory[base - 1] != '/';
  char *path = malloc(base + slash + length + 1);
  if (!path) {
    return NULL;
  }
  memcpy(path, directory, base);
  if (slash) {
    path[base] = '/';
  }
  memcpy(path + base + slash, name, length);
  path[base + slash + length] = '\0';
  return path;
}

/* A rename failure names the problem with the source when it has one. */
static void report_failure(const char *source, const char *target, int error)
{
  struct stat info;
  if (stat(source, &info) == 0 && S_ISDIR(info.st_mode)) {
    fprintf(stderr, "mv: %s: Directories cannot be moved\n", source);
  } else if (error == ENOENT && stat(source, &info) != 0 && errno == ENOENT) {
    fprintf(stderr, "mv: %s: %s\n", source, strerror(ENOENT));
  } else {
    fprintf(stderr, "mv: %s -> %s: %s\n", source, target, strerror(error));
  }
}

int main(int argc, char **argv)
{
  int first = 1;
  if (argc > 1 && !strcmp(argv[1], "--")) {
    first = 2;
  }
  int count = argc - first;
  if (count < 2) {
    fputs("usage: mv [--] source... destination\n", stderr);
    return EXIT_FAILURE;
  }
  const char *destination = argv[argc - 1];
  bool into_directory = is_directory(destination);
  if (count > 2 && !into_directory) {
    struct stat info;
    if (stat(destination, &info) != 0 && errno == ENOENT) {
      fprintf(stderr, "mv: %s: %s\n", destination, strerror(ENOENT));
    } else {
      fprintf(stderr, "mv: %s: %s; several sources need a destination directory\n",
          destination, strerror(ENOTDIR));
    }
    return EXIT_FAILURE;
  }

  int result = EXIT_SUCCESS;
  for (int i = first; i < argc - 1; ++i) {
    const char *source = argv[i];
    char *joined = NULL;
    const char *target = destination;
    if (into_directory) {
      size_t length;
      const char *name = base_name(source, &length);
      if (!name) {
        fprintf(stderr, "mv: %s: A root cannot be moved\n", source);
        result = EXIT_FAILURE;
        continue;
      }
      joined = join(destination, name, length);
      if (!joined) {
        fprintf(stderr, "mv: %s\n", strerror(ENOMEM));
        return EXIT_FAILURE;
      }
      target = joined;
    }
    if (rename(source, target) != 0) {
      report_failure(source, target, errno);
      result = EXIT_FAILURE;
    }
    free(joined);
  }
  return result;
}
