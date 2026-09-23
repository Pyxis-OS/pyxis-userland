#include "../common/directory.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>

static int list_directory(const char *path)
{
  handle_t directory;
  enum call_status status = resolve_directory(path, DIRECTORY_RIGHT_ENUMERATE, &directory);
  if (status != CALL_OK) {
    report_directory_error("ls", path, status);
    return -1;
  }

  int result = -1;
  size_t capacity = 128;
  char *name = malloc(capacity);
  if (!name) {
    report_directory_error("ls", path, CALL_NO_MEMORY);
    goto done;
  }
  struct directory_cursor cursor = {0};
  for (;;) {
    struct directory_enumerate_reply entry;
    status = directory_enumerate(directory, &cursor, name, capacity, &entry);
    if (status != CALL_OK) {
      report_directory_error("ls", path, status);
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      result = 0;
      break;
    }
    if (entry.outcome == DIRECTORY_CHANGED) {
      fprintf(stderr, "ls: %s: Directory changed during listing\n", path);
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      if (!larger) {
        report_directory_error("ls", path, CALL_NO_MEMORY);
        break;
      }
      name = larger;
      capacity = entry.name_size;
      continue; /* A short buffer leaves the cursor unchanged. */
    }
    if (printf("%s%s\n", name, entry.kind == DIRECTORY_KIND_DIRECTORY ? "/" : "") < 0) {
      perror("ls: stdout");
      break;
    }
    cursor = entry.cursor;
  }
  free(name);

done:
  if (handle_close(directory) != 0) {
    report_directory_error("ls", path, CALL_BAD_HANDLE);
    result = -1;
  }
  return result;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    return list_directory(".") == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    if (argc > 2 && printf("%s:\n", argv[i]) < 0) {
      perror("ls: stdout");
      return EXIT_FAILURE;
    }
    if (list_directory(argv[i]) != 0) {
      result = EXIT_FAILURE;
    }
    if (ferror(stdout) || ferror(stderr)) {
      return EXIT_FAILURE;
    }
  }
  return result;
}
