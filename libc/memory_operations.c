#include "memory_operations.h"

enum call_status libc_memory_allocate_guarded(handle_t memory, size_t size,
    struct memory_reservation *reservation)
{
  enum call_status status;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    status = memory_allocate_guarded(memory, size, reservation);
    if (status != CALL_BUSY) {
      break;
    }
  }
  return status;
}

enum call_status libc_memory_allocate(handle_t memory, size_t size,
    struct memory_region *region)
{
  enum call_status status;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    status = memory_allocate(memory, size, region);
    if (status != CALL_BUSY) {
      break;
    }
  }
  return status;
}

enum call_status libc_memory_release(handle_t memory, struct memory_region region)
{
  enum call_status status;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    status = memory_release(memory, region);
    if (status != CALL_BUSY) {
      break;
    }
  }
  return status;
}

enum call_status libc_memory_release_reservation(handle_t memory, uint64_t identity)
{
  enum call_status status;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    status = memory_release_reservation(memory, identity);
    if (status != CALL_BUSY) {
      break;
    }
  }
  return status;
}

enum call_status libc_native_thread_create(handle_t threads,
    const struct thread_create *request, handle_t *completion)
{
  enum call_status status;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    status = thread_create(threads, request, completion);
    if (status != CALL_BUSY) {
      break;
    }
  }
  return status;
}
