#include <stdlib.h>
#include <syscall.h>

[[noreturn]] void _Exit(int status)
{
  syscall1(SYSCALL_EXIT, (uint32_t)status);

  /* A returning exit syscall violates the ABI; do not fall through into code
   * compiled on the assumption that this function never returns. */
  __builtin_trap();
}

[[noreturn]] void exit(int status)
{
  _Exit(status);
}
