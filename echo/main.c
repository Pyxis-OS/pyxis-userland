#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
  bool newline = true;
  int first = 1;
  if (argc > 1 && !strcmp(argv[1], "-n")) {
    newline = false;
    first = 2;
  }

  for (int i = first; i < argc; ++i) {
    if ((i > first && fputc(' ', stdout) == EOF) ||
        fputs(argv[i], stdout) == EOF) {
      goto output_error;
    }
  }
  if (newline && fputc('\n', stdout) == EOF) {
    goto output_error;
  }
  if (fflush(stdout) == EOF) {
    goto output_error;
  }
  return EXIT_SUCCESS;

output_error:
  fprintf(stderr, "echo: stdout: %s\n", strerror(errno));
  return EXIT_FAILURE;
}
