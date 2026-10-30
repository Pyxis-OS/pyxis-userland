#ifndef USERSPACE_SHARED_MEMORY_H
#define USERSPACE_SHARED_MEMORY_H

#include <abi/handle.h>
#include <abi/memory.h>
#include <abi/shared_memory.h>
#include <abi/syscall.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow explicit CREATE_SHARED authority. Success returns one owned region
 * handle and its page-rounded extent; failure clears CREATED. Receiving or
 * mapping a region needs no creation authority. */
enum call_status shared_memory_create(handle_t memory, size_t size,
    struct shared_memory_created *created);

/* Borrow a region with READ or READ|WRITE authority. Success returns a full
 * caller-owned R/NX or RW/NX view; failure clears VIEW. Handle close and creator
 * exit alone do not unmap it. Release its identity with MEMORY MANAGE through
 * memory_release_reservation(), even after closing the region handle.
 * Both helpers return OUTCOME_UNKNOWN for an untrustworthy post-effect reply:
 * do not replay the operation or infer that no handle/view was published. */
enum call_status shared_memory_map(handle_t region, uint64_t rights,
    struct memory_reservation *view);

#ifdef __cplusplus
}
#endif

#endif
