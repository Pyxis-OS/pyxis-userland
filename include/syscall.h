#ifndef USERSPACE_SYSCALL_H
#define USERSPACE_SYSCALL_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* One-argument calls that preserve RDX: RAX is the number/result, RDI is arg1.
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

/* Native CALL: five arguments, with status/reply bytes returned in RAX/RDX. */
static inline struct syscall_result syscall_call(handle_t handle,
    const void *message, size_t message_size, void *reply, size_t reply_capacity)
{
  uint64_t status = SYSCALL_CALL;
  uint64_t reply_size = message_size;
  register uint64_t arg4 __asm__("r10") = (uintptr_t)reply;
  register uint64_t arg5 __asm__("r8") = reply_capacity;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(handle), "S"(message), "r"(arg4), "r"(arg5)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* CLOSE overwrites RDX, so it cannot use the syscall1 wrapper. */
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

static inline struct syscall_result syscall_copy(handle_t source, uint64_t rights,
    uint64_t flags, handle_t *destination)
{
  uint64_t status = SYSCALL_COPY;
  uint64_t reply_size = flags;
  register uint64_t arg4 __asm__("r10") = (uintptr_t)destination;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(source), "S"(rights), "r"(arg4)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_handle_rights(handle_t handle, uint64_t *rights)
{
  uint64_t status = SYSCALL_HANDLE_RIGHTS;
  uint64_t reply_size;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(handle), "S"(rights)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

#endif
