#include "ls.h"
#include <pyxis/stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
  fprintf(stderr, "usage: ls [-1] [-l] [--] [directory...]\n");
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  struct ls_output output = {0};
  bool options = true;
  int operands = 0;
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    if (options && !strcmp(argument, "--")) {
      options = false;
      continue;
    }
    if (options && argument[0] == '-' && argument[1]) {
      for (size_t j = 1; argument[j]; ++j) {
        if (argument[j] == '1') {
          output.one_per_line = true;
        } else if (argument[j] == 'l') {
          output.long_listing = true;
        } else {
          fprintf(stderr, "ls: unknown option: %s\n", argument);
          return usage();
        }
      }
      continue;
    }
    argv[++operands] = argv[i];
  }

  struct startup_stream binding;
  if (pyxis_stdio_stream(stdout, &binding) != 0) {
    perror("ls: stdout");
    return EXIT_FAILURE;
  }
  output.terminal = binding.protocol == PROTOCOL_CONSOLE;
  if (output.terminal && !output.one_per_line && !output.long_listing) {
    struct terminal term = {.input = HANDLE_INVALID, .output = binding.handle};
    size_t rows;
    /* Missing geometry still permits a colored, one-per-line listing. */
    if (term_size(&term, &output.columns, &rows) != CALL_OK) {
      output.columns = 0;
    }
  }

  int result = EXIT_SUCCESS;
  int directories = operands ? operands : 1;
  for (int i = 0; i < directories; ++i) {
    const char *path = operands ? argv[i + 1] : ".";
    if (operands > 1 &&
        (ls_print_text(path, output.terminal) != 0 || fputs(":\n", stdout) == EOF)) {
      perror("ls: stdout");
      return EXIT_FAILURE;
    }
    struct ls_listing listing = {0};
    bool loaded;
    if (ls_load_listing(path, &output, &listing, &loaded) != 0) {
      result = EXIT_FAILURE;
    }
    if (loaded && ls_print_listing(&listing, &output) != 0) {
      result = EXIT_FAILURE;
    }
    ls_free_listing(&listing);
    if (ferror(stdout) || ferror(stderr)) {
      return EXIT_FAILURE;
    }
  }
  if (fflush(stdout) == EOF) {
    perror("ls: stdout");
    return EXIT_FAILURE;
  }
  return result;
}
