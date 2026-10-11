#ifndef USERSPACE_MEMORY_H
#define USERSPACE_MEMORY_H

#include <abi/handle.h>
#include <abi/memory.h>
#include <abi/syscall.h>

/* Native status; output is cleared on failure. The caller supplies valid output
 * storage. Returned bytes are zeroed, writable, NX and private to this process.
 * The handle needs MANAGE, discovered through an explicit startup grant. */
enum call_status memory_allocate(handle_t memory, size_t size, struct memory_region *region);

/* Release one whole region using its returned address/size. No user buffer is
 * written on return; pass by value so the descriptor may live inside the region.
 * Closing the service handle does not release backing; process exit does. */
enum call_status memory_release(handle_t memory, struct memory_region region);

/* Eager zeroed RW/NX usable bytes with one unmapped lower guard. The returned
 * descriptor covers the full reservation; usable storage starts one page above
 * address. Its process-private identity survives no release/address reuse.
 * Clears *reservation on failure. No libc stack-size policy is imposed. */
enum call_status memory_allocate_guarded(handle_t memory, size_t usable_size,
    struct memory_reservation *reservation);

/* Releases the whole identity. Live/retiring thread stack or FS context pins
 * return BUSY until native completion. No output writes; backing can contain
 * the caller's descriptor. Closing MEMORY does not release the reservation. */
enum call_status memory_release_reservation(handle_t memory, uint64_t identity);

#endif
