#ifndef LIBC_FORMAT_H
#define LIBC_FORMAT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Without a sink, buffer holds the first capacity - 1 bytes and length counts
 * everything. With a sink (format_stream), buffer is a chunk of capacity bytes
 * handed to the sink whenever it fills, and failed latches a sink refusal. */
struct output {
  char *buffer;
  size_t capacity;
  size_t length;
  bool (*sink)(void *context, const char *bytes, size_t count);
  void *context;
  size_t pending;
  bool failed;
};

enum float_format_flags {
  FLOAT_LEFT = 1u << 0,
  FLOAT_PLUS = 1u << 1,
  FLOAT_SPACE = 1u << 2,
  FLOAT_ALTERNATE = 1u << 3,
  FLOAT_ZERO = 1u << 4,
};

/* Shared by the integer formatter and the musl floating-point conversion.
 * Count truncated bytes, saturating length at INT_MAX + 1. */
void format_append(struct output *out, const char *text, size_t count);
void format_pad(struct output *out, char character, size_t count);
/* The vsnprintf conversions, written through sink in bounded chunks instead of
 * into one buffer. Returns 0, an errno value for a bad format (output before
 * the bad conversion may already have been written), or -1 when the sink
 * refused a chunk. Callers that must not write on a bad format validate it
 * first with vsnprintf. */
int format_stream(bool (*sink)(void *context, const char *bytes, size_t count),
                  void *context, const char *format, va_list args);
int format_float(struct output *out, long double value, int width, int precision,
                 unsigned flags, int kind);

#endif
