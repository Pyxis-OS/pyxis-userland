#ifndef USERSPACE_SYSCALL_H
#define USERSPACE_SYSCALL_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Legacy one-argument calls: RAX is the number/result, RDI is arg1.
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

/* Native CALL: six arguments, with status/reply bytes returned in RAX/RDX. */
static inline struct syscall_result syscall_call(handle_t handle, uint64_t operation,
    const void *request, size_t request_size, void *reply, size_t reply_capacity)
{
  uint64_t status = SYSCALL_CALL;
  uint64_t reply_size = (uintptr_t)request;
  register uint64_t arg4 __asm__("r10") = request_size;
  register uint64_t arg5 __asm__("r8") = (uintptr_t)reply;
  register uint64_t arg6 __asm__("r9") = reply_capacity;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(handle), "S"(operation), "r"(arg4), "r"(arg5), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* CLOSE overwrites RDX, so it cannot use the legacy syscall1 wrapper. */
static inline struct syscall_result syscall_close(handle_t handle)
{
  uint64_t status = SYSCALL_CLOSE;
  uint64_t reply_size;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(handle)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

#endif
