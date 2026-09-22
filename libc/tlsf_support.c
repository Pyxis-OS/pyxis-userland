#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include "tlsf_user.h"

void libc_tlsf_log(const char *format, ...)
{
  char buffer[256];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (length < 0) {
    return;
  }
  size_t count = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1;
  for (size_t i = 0; i < count; ++i) {
    syscall1(SYSCALL_LOG_PUTCHAR, (unsigned char)buffer[i]);
  }
}

[[noreturn]] void libc_tlsf_fail(const char *condition, const char *file, int line)
{
  /* No heap or output capability is needed to report allocator corruption. */
  libc_tlsf_log("libc: TLSF assertion %s (%s:%d)\n", condition, file, line);
  _Exit(EXIT_FAILURE);
}
