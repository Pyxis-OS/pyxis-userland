#include "../common/directory.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc < 2) {
    fputs("usage: rm path...\n", stderr);
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    enum call_status status = remove_path(argv[i], DIRECTORY_KIND_FILE);
    if (status != CALL_OK) {
      if (report_directory_error("rm", argv[i], status) < 0) {
        return EXIT_FAILURE;
      }
      result = EXIT_FAILURE;
    }
  }
  return result;
}
