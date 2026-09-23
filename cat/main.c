#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the transfer buffer off the initial one-page user stack. */
static unsigned char buffer[4096];

static int report_error(const char *path, int error)
{
  return fprintf(stderr, "cat: %s: %s\n", path, strerror(error));
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fputs("usage: cat path...\n", stderr);
    return EXIT_FAILURE;
  }

  int result = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    FILE *file = fopen(argv[i], "rb");
    if (!file) {
      if (report_error(argv[i], errno) < 0) {
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
        fclose(file);
        return EXIT_FAILURE;
      }
      if (read_error != 0) {
        if (report_error(argv[i], read_error) < 0) {
          fclose(file);
          return EXIT_FAILURE;
        }
        result = EXIT_FAILURE;
        break;
      }
      if (count == 0) {
        break;
      }
    }

    if (fclose(file) != 0) {
      if (report_error(argv[i], errno) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
    }
  }
  return result;
}
