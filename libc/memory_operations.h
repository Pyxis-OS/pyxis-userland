#ifndef LIBC_MEMORY_OPERATIONS_H
#define LIBC_MEMORY_OPERATIONS_H

#include <memory.h>
#include <thread.h>

/* Bounded tuning for definite pre-effect VM admission refusals. No lock or
 * mapping lease spans a retry; native syscall return first reopens the gate.
 * Unknown outcomes and all other failures are never retried. Helpers are
 * TLS-free because initial FS bootstrap uses them too. */
#define LIBC_VM_ATTEMPTS 8

enum call_status libc_memory_allocate_guarded(handle_t memory, size_t size,
    struct memory_reservation *reservation);
enum call_status libc_memory_allocate(handle_t memory, size_t size,
    struct memory_region *region);
enum call_status libc_memory_release(handle_t memory, struct memory_region region);
enum call_status libc_memory_release_reservation(handle_t memory, uint64_t identity);
enum call_status libc_native_thread_create(handle_t threads,
    const struct thread_create *request, handle_t *completion);

#endif
