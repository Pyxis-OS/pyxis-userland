#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* Formatting only: no streams yet. Supports s, c, d, i, u, o, x, X, p and %;
 * integer lengths hh/h/l/ll/j/z/t, flags -+ #0, width and precision (also *).
 * Unsupported formats fail with EINVAL; a count above INT_MAX uses EOVERFLOW.
 * Returns the full length excluding NUL even when truncated. NULL is allowed
 * for a zero-capacity destination. */
int snprintf(char *restrict buffer, size_t capacity, const char *restrict format, ...)
  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *restrict buffer, size_t capacity, const char *restrict format, va_list args)
  __attribute__((format(printf, 3, 0)));

#endif
