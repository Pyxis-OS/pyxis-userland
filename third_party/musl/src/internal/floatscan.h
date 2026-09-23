#ifndef PYXIS_MUSL_FLOATSCAN_H
#define PYXIS_MUSL_FLOATSCAN_H

#include <stddef.h>

enum float_precision {
  FLOAT_PRECISION_FLOAT,
  FLOAT_PRECISION_DOUBLE,
  FLOAT_PRECISION_LONG_DOUBLE,
};

struct float_input {
  const char *start;
  const char *cursor;
};

/* The scanner may consume the terminating NUL, but then stops or pushes it
 * back before another read. Rollback never precedes the original string. */
static inline int string_get(struct float_input *input)
{
  return (unsigned char)*input->cursor++;
}

static inline void string_unget(struct float_input *input)
{
  input->cursor--;
}

static inline void string_reject(struct float_input *input)
{
  input->cursor = input->start;
}

/* Keep musl's internal prefix-permitted mode; strto* always passes 1. */
long double __floatscan(struct float_input *input, int precision, int pok);

#endif
