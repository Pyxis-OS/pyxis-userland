#ifndef USERSPACE_JIT_H
#define USERSPACE_JIT_H

#include <abi/jit.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Native process-owned JIT regions. Allocation returns eager zeroed RW/NX
 * backing; HINT is advisory and may be ignored. Failure clears REGION.
 * Closing/transferring the service does not release the calling process's
 * regions. A transferred service manages only its receiver's allocations.
 * OUTCOME_UNKNOWN must not be retried; an operation may already have committed. */
enum call_status jit_allocate(handle_t jit, size_t size, uintptr_t hint,
    struct jit_region *region);
/* Protect a nonzero page-aligned contained interval. The caller must exclude
 * logical users before rewriting or retiring code; no code-entry lease exists.
 * BUSY/NO_MEMORY before commit preserves the existing permissions and bytes. */
enum call_status jit_protect(handle_t jit, uint64_t identity, size_t offset,
    size_t size, enum jit_state state);
/* Releases the whole identity, with completion after actual reclamation. */
enum call_status jit_release(handle_t jit, uint64_t identity);

#endif
