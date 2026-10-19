#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ignore trailing separators, but a scheme root has no component to append. */
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
  size_t slash = base && directory[base - 1] != '/';
  if (length > SIZE_MAX - base || base + length > SIZE_MAX - slash - 1) {
    errno = EOVERFLOW;
    return NULL;
  }
  char *path = malloc(base + slash + length + 1);
  if (!path) {
    errno = ENOMEM;
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

static void report_failure(const char *source, const char *target, int error)
{
  struct stat info;
  if (error == ENOENT && stat(source, &info) != 0 && errno == ENOENT) {
    fprintf(stderr, "ln: %s: %s\n", source, strerror(ENOENT));
  } else {
    fprintf(stderr, "ln: %s -> %s: %s\n", source, target, strerror(error));
  }
}

int main(int argc, char **argv)
{
  int first = 1;
  bool literal = argc > 1 && !strcmp(argv[1], "--");
  if (literal) {
    first = 2;
  }
  if (argc - first < 2) {
    fputs("usage: ln [--] source... destination\n", stderr);
    return EXIT_FAILURE;
  }
  if (!literal) {
    for (int i = first; i < argc; ++i) {
      if (argv[i][0] == '-' && argv[i][1]) {
        fprintf(stderr, "ln: %s: Unknown option; use -- before file names beginning with -\n", argv[i]);
        return EXIT_FAILURE;
      }
    }
  }
  const char *destination = argv[argc - 1];
  struct stat info;
  int destination_status = stat(destination, &info);
  int destination_error = errno;
  bool into_directory = destination_status == 0 && S_ISDIR(info.st_mode);
  if (argc - first > 2 && !into_directory) {
    fprintf(stderr, "ln: %s: %s; several sources need a destination directory\n",
        destination, strerror(destination_status != 0 ? destination_error : ENOTDIR));
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
        fprintf(stderr, "ln: %s: A root cannot be hard linked\n", source);
        result = EXIT_FAILURE;
        continue;
      }
      joined = join(destination, name, length);
      if (!joined) {
        fprintf(stderr, "ln: %s -> %s: %s\n", source, destination, strerror(errno));
        return EXIT_FAILURE;
      }
      target = joined;
    }
    if (link(source, target) != 0) {
      report_failure(source, target, errno);
      result = EXIT_FAILURE;
    }
    free(joined);
  }
  return result;
}
