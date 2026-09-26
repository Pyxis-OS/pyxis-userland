#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bound transfer storage independently of file size. */
static unsigned char buffer[4096];

static int report_error(const char *path, int error)
{
  return fprintf(stderr, "cat: %s: %s\n", path, strerror(error));
}

int main(int argc, char **argv)
{
  int result = EXIT_SUCCESS;
  int operands = argc > 1 ? argc - 1 : 1;
  for (int i = 0; i < operands; ++i) {
    const char *path = argc > 1 ? argv[i + 1] : "-";
    bool input = !strcmp(path, "-");
    const char *name = input ? "stdin" : path;
    FILE *file = input ? stdin : fopen(path, "rb");
    if (!file) {
      if (report_error(name, errno) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
      continue;
    }

    for (;;) {
      size_t count = fread(buffer, 1, sizeof(buffer), file);
      /* A read can return bytes and an error together. Preserve the error
       * across output, but still copy the bytes that were read. */
      int read_error = ferror(file) ? errno : 0;
      if (count != 0 && fwrite(buffer, 1, count, stdout) != count) {
        report_error("stdout", errno);
        if (!input) {
          fclose(file);
        }
        return EXIT_FAILURE;
      }
      if (read_error != 0) {
        if (report_error(name, read_error) < 0) {
          if (!input) {
            fclose(file);
          }
          return EXIT_FAILURE;
        }
        result = EXIT_FAILURE;
        break;
      }
      if (count == 0) {
        break;
      }
    }

    if (!input && fclose(file) != 0) {
      if (report_error(name, errno) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
    }
  }
  return result;
}
