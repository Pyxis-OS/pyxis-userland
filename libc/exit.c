#include <stdlib.h>
#include <syscall.h>
#include "runtime.h"

[[noreturn]] void _Exit(int status)
{
  syscall1(SYSCALL_EXIT, (uint32_t)status);

  /* A returning exit syscall violates the ABI; do not fall through into code
   * compiled on the assumption that this function never returns. */
  __builtin_trap();
}

[[noreturn]] void exit(int status)
{
  stdio_finish();
  _Exit(status);
}

[[noreturn]] void abort(void)
{
  _Exit(EXIT_FAILURE);
}
