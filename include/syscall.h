#ifndef USERSPACE_SYSCALL_H
#define USERSPACE_SYSCALL_H

#include <stdint.h>

#define SYSCALL_PUTCHAR UINT64_C(0)
#define SYSCALL_LOG_PUTCHAR UINT64_C(1)
#define SYSCALL_EXIT UINT64_C(-1)

/* Caelum's one-argument syscall ABI: RAX is the number/result, RDI is arg1.
 * RCX and R11 are destroyed by SYSCALL/SYSRET. The memory clobber keeps C
 * memory accesses ordered around the kernel call, including pointer arguments. */
static inline int64_t syscall1(uint64_t number, uint64_t arg1)
{
  int64_t result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1)
                   : "rcx", "r11", "cc", "memory");
  return result;
}

#endif
