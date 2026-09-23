#include <errno.h>
#include <stdlib.h>

#include "floatscan.h"

static long double convert_float(const char *text, char **end,
  enum float_precision precision)
{
  struct float_input input = { .start = text, .cursor = text };
  int saved_errno = errno;
  long double value = __floatscan(&input, precision, 1);

  /* musl uses EINVAL for no conversion; Pyxis leaves errno unchanged. */
  if (input.cursor == text) errno = saved_errno;
  if (end) *end = (char *)input.cursor;
  return value;
}

float strtof(const char *restrict text, char **restrict end)
{
  return convert_float(text, end, FLOAT_PRECISION_FLOAT);
}

double strtod(const char *restrict text, char **restrict end)
{
  return convert_float(text, end, FLOAT_PRECISION_DOUBLE);
}

long double strtold(const char *restrict text, char **restrict end)
{
  return convert_float(text, end, FLOAT_PRECISION_LONG_DOUBLE);
}
