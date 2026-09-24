#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
  if (argc != 3) {
    fputs("usage: mv source-file destination-file\n", stderr);
    return EXIT_FAILURE;
  }
  if (rename(argv[1], argv[2]) != 0) {
    fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], argv[2], strerror(errno));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
