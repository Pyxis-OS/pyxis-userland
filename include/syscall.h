#ifndef USERSPACE_SYSCALL_H
#define USERSPACE_SYSCALL_H

#include <abi/handle.h>
#include <abi/forwarder.h>
#include <abi/describe.h>
#include <abi/process.h>
#include <abi/readiness_set.h>
#include <abi/syscall.h>
#include <abi/limit.h>
#include <abi/wait.h>
#include <stddef.h>

/* One-argument calls that preserve RDX: RAX is the number/result, RDI is arg1.
 * RCX and R11 are destroyed by SYSCALL/SYSRET. The memory clobber keeps C
 * memory accesses ordered around the kernel call, including pointer arguments. */
static inline int64_t syscall1(uint64_t number, uint64_t arg1)
{
  int64_t result;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return result;
}

/* R9 selects optional fixed limit detail. Initialize caller storage before use;
 * only CALL_LIMIT writes it. RAX/RDX remain status and ordinary reply bytes. */
static inline struct syscall_result syscall_call_with_limit(handle_t handle,
    const void *message, size_t message_size, void *reply, size_t reply_capacity,
    struct call_limit *limit)
{
  uint64_t status = SYSCALL_CALL;
  uint64_t reply_size = message_size;
  register uint64_t arg4 __asm__("r10") = (uintptr_t)reply;
  register uint64_t arg5 __asm__("r8") = reply_capacity;
  register uint64_t arg6 __asm__("r9") = (uintptr_t)limit;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(handle), "S"(message), "r"(arg4), "r"(arg5), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* CLOSE overwrites RDX, so it cannot use the syscall1 wrapper. */
static inline struct syscall_result syscall_close(handle_t handle)
{
  uint64_t status = SYSCALL_CLOSE;
  uint64_t reply_size;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(handle), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_copy_with_limit(handle_t source, uint64_t rights,
    uint64_t transport, uint64_t flags, handle_t *destination, struct call_limit *limit)
{
  uint64_t status = SYSCALL_COPY;
  uint64_t reply_size = transport;
  register uint64_t arg4 __asm__("r10") = flags;
  register uint64_t arg5 __asm__("r8") = (uintptr_t)destination;
  register uint64_t arg6 __asm__("r9") = (uintptr_t)limit;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(source), "S"(rights), "r"(arg4), "r"(arg5), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* HANDLE_INFO uses RDX for the reply capacity on entry and reply size on exit. */
static inline struct syscall_result syscall_handle_info(handle_t handle,
    struct handle_info *info, size_t capacity)
{
  uint64_t status = SYSCALL_HANDLE_INFO;
  uint64_t reply_size = capacity;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(handle), "S"(info), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* DESCRIBE uses RDX for the reply capacity on entry and reply size on exit. */
static inline struct syscall_result syscall_describe(handle_t handle,
    struct describe_reply *info, size_t capacity, uint64_t flags)
{
  uint64_t status = SYSCALL_DESCRIBE;
  uint64_t reply_size = capacity;
  register uint64_t arg4 __asm__("r10") = flags;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(handle), "S"(info), "r"(arg4), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_wait_many(const struct wait_interest *interests,
    size_t count, uint64_t deadline_ns, uint64_t *events)
{
  uint64_t status = SYSCALL_WAIT_MANY;
  uint64_t reply_size = deadline_ns;
  register uint64_t arg4 __asm__("r10") = (uintptr_t)events;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(interests), "S"(count), "r"(arg4), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* FORWARDER_CREATE uses RDX for the reply capacity on entry and reply size on
 * exit; its request has a fixed size. */
static inline struct syscall_result syscall_forwarder_create(
    const struct forwarder_create *request, struct forwarder_reply *reply, size_t capacity)
{
  uint64_t status = SYSCALL_FORWARDER_CREATE;
  uint64_t reply_size = capacity;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "+d"(reply_size)
                   : "D"(request), "S"(reply), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_process_interrupt_create(
    struct process_interrupt_create_reply *reply, size_t capacity)
{
  uint64_t status = SYSCALL_PROCESS_INTERRUPT_CREATE;
  uint64_t reply_size;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(reply), "S"(capacity), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

/* REPLY receives an abi/process_view.h listing of the caller's own space. */
static inline struct syscall_result syscall_space_processes(void *reply, size_t capacity)
{
  uint64_t status = SYSCALL_SPACE_PROCESSES;
  uint64_t reply_size;
  register uint64_t arg6 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(reply), "S"(capacity), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_readiness_set_create_with_limit(
    struct readiness_set_create_reply *reply, size_t capacity, struct call_limit *limit)
{
  uint64_t status = SYSCALL_READINESS_SET_CREATE;
  uint64_t reply_size;
  register uint64_t arg6 __asm__("r9") = (uintptr_t)limit;
  __asm__ volatile("syscall"
                   : "+a"(status), "=d"(reply_size)
                   : "D"(reply), "S"(capacity), "r"(arg6)
                   : "rcx", "r11", "cc", "memory");
  return (struct syscall_result){status, reply_size};
}

static inline struct syscall_result syscall_call(handle_t handle,
    const void *message, size_t message_size, void *reply, size_t reply_capacity)
{
  return syscall_call_with_limit(handle, message, message_size, reply, reply_capacity, NULL);
}

static inline struct syscall_result syscall_copy(handle_t source, uint64_t rights,
    uint64_t transport, uint64_t flags, handle_t *destination)
{
  return syscall_copy_with_limit(source, rights, transport, flags, destination, NULL);
}

static inline struct syscall_result syscall_readiness_set_create(
    struct readiness_set_create_reply *reply, size_t capacity)
{
  return syscall_readiness_set_create_with_limit(reply, capacity, NULL);
}

#endif
