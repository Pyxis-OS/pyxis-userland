#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "stream.h"

int vfprintf(FILE *restrict stream, const char *restrict format, va_list args)
{
  if (!stream_ready(stream, true)) {
    return EOF;
  }
  char local[256];
  char *buffer = local;
  int length = vsnprintf(local, sizeof(local), format, args);
  if (length < 0) {
    return stream_error(stream, errno);
  }
  if ((size_t)length >= sizeof(local)) {
    buffer = malloc((size_t)length + 1);
    if (!buffer) {
      return stream_error(stream, errno);
    }
    /* vsnprintf copies args, so the second pass sees the same argument list.
     * Format completely before output; no fixed formatting-length cutoff. */
    int formatted = vsnprintf(buffer, (size_t)length + 1, format, args);
    if (formatted != length) {
      free(buffer);
      return stream_error(stream, formatted < 0 ? errno : EINVAL);
    }
  }
  bool success = fwrite(buffer, 1, length, stream) == (size_t)length;
  if (buffer != local) {
    free(buffer);
  }
  return success ? length : EOF;
}

int fprintf(FILE *restrict stream, const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vfprintf(stream, format, args);
  va_end(args);
  return result;
}

int vprintf(const char *restrict format, va_list args)
{
  return vfprintf(stdout, format, args);
}

int printf(const char *restrict format, ...)
{
  va_list args;
  va_start(args, format);
  int result = vfprintf(stdout, format, args);
  va_end(args);
  return result;
}
