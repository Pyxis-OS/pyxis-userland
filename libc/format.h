#ifndef LIBC_FORMAT_H
#define LIBC_FORMAT_H

#include <stddef.h>

struct output {
  char *buffer;
  size_t capacity;
  size_t length;
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
int format_float(struct output *out, long double value, int width, int precision,
                 unsigned flags, int kind);

#endif
