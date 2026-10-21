#include "system_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <syscall.h>

void system_log(const char *format, ...)
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
