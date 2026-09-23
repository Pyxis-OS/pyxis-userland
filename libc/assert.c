#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((noreturn)) void __assert_fail(const char *expression,
  const char *file, int line, const char *function)
{
  /* Write variable-length fields separately so the diagnostic needs no heap. */
  fputs(file, stderr);
  fprintf(stderr, ":%d: ", line);
  fputs(function, stderr);
  fputs(": assertion failed: ", stderr);
  fputs(expression, stderr);
  fputc('\n', stderr);

  abort();
}
