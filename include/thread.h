#ifndef USERSPACE_THREAD_H
#define USERSPACE_THREAD_H

#include <abi/thread.h>
#include <abi/syscall.h>

/* MANAGE applies to the calling process. Regions are distinct guarded MEMORY
 * reservations. CREATE pins them until native completion, starts at entry with
 * argument in RDI and a 16-byte-aligned RSP, and performs no runtime startup.
 * The entry must EXIT explicitly; it cannot return. Clears *completion on
 * failure. OUTCOME_UNKNOWN must not be retried: publication may have occurred. */
enum call_status thread_create(handle_t threads, const struct thread_create *request,
    handle_t *completion);

/* SET_FS or MANAGE binds the current thread's guarded FS context once.
 * Rebinding returns BUSY.
 * This establishes only a raw native FS base, not a libc or language TLS layout. */
enum call_status thread_set_fs(handle_t threads, struct thread_fs_context context);

/* Successful EXIT never returns. A refusal returns its native status. The
 * caller must not release its stack/context; completion follows retirement. */
enum call_status thread_exit(handle_t threads, uint64_t value);

/* MANAGE applies to the calling process's ordinary VM-owned writable user RAM. Compare
 * and park an aligned 4- or 8-byte word; a 32-bit expected value has no high
 * bits. Mismatch returns WOULD_BLOCK, wake OK, expiry TIMED_OUT and mapping
 * withdrawal BAD_BUFFER. The deadline is absolute monotonic nanoseconds;
 * zero polls and THREAD_ADDRESS_NO_DEADLINE waits indefinitely. Wake does not
 * transfer lock ownership: use atomics and recheck the predicate. */
enum call_status thread_address_wait(handle_t threads, uintptr_t address,
    uint64_t expected, uint64_t word_size, uint64_t deadline);

/* Wake at most maximum parked registrations at this process-local address.
 * maximum is nonzero; UINT64_MAX wakes all. Clears *woken on failure. A malformed
 * reply returns OUTCOME_UNKNOWN: wakes may already have committed, so do not
 * retry automatically. Neither helper adds authority or dereferences address. */
enum call_status thread_address_wake(handle_t threads, uintptr_t address,
    uint64_t word_size, uint64_t maximum, uint64_t *woken);

/* WAIT requires the completion grant's WAIT right. Repeatable; closing the
 * observer does not stop the sibling. Completion follows pin release and
 * retirement. STOPPED carries zero, EXITED carries the machine-word EXIT value.
 * WAIT_COMPLETE observes readiness. Clears *result on failure. */
enum call_status thread_wait(handle_t completion, struct thread_result *result);

#endif
