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

/* Bind the current thread's guarded FS context once. Rebinding returns BUSY.
 * This establishes only a raw native FS base, not a libc or language TLS layout. */
enum call_status thread_set_fs(handle_t threads, struct thread_fs_context context);

/* Successful EXIT never returns. A refusal returns its native status. The
 * caller must not release its stack/context; completion follows retirement. */
enum call_status thread_exit(handle_t threads, uint64_t value);

/* WAIT requires the completion grant's WAIT right. Repeatable; closing the
 * observer does not stop the sibling. Completion follows pin release and
 * retirement. STOPPED carries zero, EXITED carries the machine-word EXIT value.
 * WAIT_COMPLETE observes readiness. Clears *result on failure. */
enum call_status thread_wait(handle_t completion, struct thread_result *result);

#endif
