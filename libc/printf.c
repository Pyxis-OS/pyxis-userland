#include <errno.h>
#include <stdio.h>
#include "format.h"
#include "stream.h"
#include "signal_scope.h"

static bool stream_sink(void *context, const char *bytes, size_t count)
{
  return fwrite(bytes, 1, count, context) == count;
}

int vfprintf(FILE *restrict stream, const char *restrict format, va_list args)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_byte_ready(stream, true)) {
    return EOF;
  }
  /* The first pass validates the format, counts the result and fits the common
   * short line in local. A longer result is formatted again through a bounded
   * chunk, so output never needs memory in proportion to its size and a bad
   * format still writes nothing. vsnprintf copies args, so the second pass sees
   * the same argument list. */
  char local[256];
  int length = vsnprintf(local, sizeof(local), format, args);
  if (length < 0) {
    return stream_error(stream, errno);
  }
  if ((size_t)length < sizeof(local)) {
    return fwrite(local, 1, length, stream) == (size_t)length ? length : EOF;
  }
  return format_stream(stream_sink, stream, format, args) == 0 ? length : EOF;
}

int fprintf(FILE *restrict stream, const char *restrict format, ...)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  va_list args;
  va_start(args, format);
  int result = vfprintf(stream, format, args);
  va_end(args);
  return result;
}

int vprintf(const char *restrict format, va_list args)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  return vfprintf(stdout, format, args);
}

int printf(const char *restrict format, ...)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  va_list args;
  va_start(args, format);
  int result = vfprintf(stdout, format, args);
  va_end(args);
  return result;
}
