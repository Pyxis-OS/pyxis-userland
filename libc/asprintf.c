#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int vasprintf(char **restrict output, const char *restrict format, va_list args)
{
  *output = NULL;

  va_list copy;
  va_copy(copy, args);
  int length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0) {
    return -1;
  }

  /* The formatter bounds length at INT_MAX; length plus NUL fits size_t. */
  size_t capacity = (size_t)length + 1;
  char *buffer = malloc(capacity);
  if (!buffer) {
    return -1;
  }

  va_copy(copy, args);
  int formatted = vsnprintf(buffer, capacity, format, copy);
  va_end(copy);
  if (formatted != length) {
    int error = formatted < 0 ? errno : EINVAL;
    free(buffer);
    errno = error;
    return -1;
  }

  *output = buffer;
  return length;
}

int asprintf(char **restrict output, const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vasprintf(output, format, args);
  va_end(args);
  return result;
}
