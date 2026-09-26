#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char buffer[4096];

static int report_error(const char *name, int error)
{
  return fprintf(stderr, "head: %s: %s\n", name, strerror(error));
}

static int usage(void)
{
  fprintf(stderr, "usage: head [-n N | -c N] [file|-]\n");
  return EXIT_FAILURE;
}

static bool parse_count(const char *text, uint64_t *count)
{
  if (!*text) {
    return false;
  }

  uint64_t value = 0;
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    unsigned digit = *cursor - '0';
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *count = value;
  return true;
}

/* Own input until its boundary or an error, including during output waits. */
static int copy_input(FILE *input, const char *name, uint64_t remaining, bool bytes)
{
  size_t buffered = 0;
  int read_error = 0, write_error = 0;
  while (remaining != 0) {
    size_t capacity = bytes ?
        (remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer)) : 1;
    size_t read = fread_some(buffer + buffered, capacity, input);
    read_error = ferror(input) ? errno : 0;
    bool line_ended = false;
    if (read != 0) {
      buffered += read;
      if (bytes) {
        remaining -= read;
      } else if (buffer[buffered - 1] == '\n') {
        --remaining;
        line_ended = true;
      }
    }
    if (remaining == 0 || read == 0 || read_error != 0) {
      break;
    }
    if (bytes || buffered == sizeof(buffer) || line_ended) {
      if (fwrite(buffer, 1, buffered, stdout) != buffered) {
        write_error = errno;
        break;
      }
      buffered = 0;
    }
  }

  /* Release the reader before a final write or diagnostic can block, so the
   * producer can observe closure even while our downstream consumer is slow. */
  int close_error = fclose(input) != 0 ? errno : 0;
  if (buffered != 0 && write_error == 0 &&
      fwrite(buffer, 1, buffered, stdout) != buffered) {
    write_error = errno;
  }
  if (read_error != 0) {
    report_error(name, read_error);
  }
  if (close_error != 0) {
    report_error(name, close_error);
  }
  if (write_error != 0) {
    report_error("stdout", write_error);
  }
  return read_error || close_error || write_error ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
  bool bytes = false;
  bool count_option = false;
  bool options = true;
  uint64_t count = 10;
  const char *path = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    if (!path && options && !strcmp(argument, "--")) {
      options = false;
      continue;
    }
    if (!path && options && (!strcmp(argument, "-n") || !strcmp(argument, "-c"))) {
      if (count_option || i + 1 == argc || !parse_count(argv[i + 1], &count)) {
        return usage();
      }
      bytes = argument[1] == 'c';
      count_option = true;
      ++i;
      continue;
    }
    if (!path && options && argument[0] == '-' && strcmp(argument, "-")) {
      return usage();
    }
    if (path) {
      return usage();
    }
    path = argument;
  }

  bool input_is_stdin = !path || !strcmp(path, "-");
  const char *name = input_is_stdin ? "stdin" : path;
  FILE *input = input_is_stdin ? stdin : fopen(path, "rb");
  int result = EXIT_SUCCESS;
  if (!input) {
    report_error(name, errno);
    result = EXIT_FAILURE;
  } else {
    result = copy_input(input, name, count, bytes);
  }

  if (fclose(stdout) != 0) {
    report_error("stdout", errno);
    result = EXIT_FAILURE;
  }
  return result;
}
