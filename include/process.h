#ifndef USERSPACE_PROCESS_H
#define USERSPACE_PROCESS_H

#include <abi/handle.h>
#include <abi/process.h>
#include <abi/syscall.h>

/* Requires WAIT authority. Blocks until execution resources have been reclaimed;
 * later calls return the same result. Does not consume or close the handle.
 * Preserves native statuses and clears *result on failure. The caller supplies
 * writable result storage. wait_many WAIT_COMPLETE observes readiness without
 * blocking other handles; the same WAIT right authorizes result retrieval.
 * EXITED carries the signed exit code; FAULTED and TERMINATED carry zero;
 * START_FAILED carries the validated packed reason. Closing an observer does
 * not terminate its target. */
enum call_status process_wait(handle_t process, struct process_result *result);

/* Requires TERMINATE authority; launch grants it with each child's observer.
 * Requests that the process stop and returns without waiting. Idempotent, and
 * successful after completion, which keeps an already committed exit or fault.
 * Affects only that process. Use process_wait or WAIT_COMPLETE for the result. */
enum call_status process_terminate(handle_t process);

/* Human-readable startup diagnostics; no allocation, TLS or I/O. */
const char *process_start_step_name(uint32_t reason);
const char *process_start_status_name(uint32_t reason);

/* Requires TERMINATE authority; requests cooperative interruption of only this
 * process. OK means accepted or coalesced, NOT_FOUND means no receiver and
 * ENDPOINT_CLOSED means the process is stopping. Does not wait for completion. */
enum call_status process_interrupt(handle_t process);

/* Opt the calling process into cooperative interrupts. BUSY means one is
 * already active; copies share its lifetime. Closing the last grant detaches it.
 * Observe WAIT_READABLE, then TAKE; readiness does not consume notification.
 * Clears *receiver on failure. OUTCOME_UNKNOWN must not be retried blindly. */
enum call_status process_interrupt_receiver_create(handle_t *receiver);

/* Owner-process-only: consume one coalesced notification. WOULD_BLOCK means
 * none is pending; ENDPOINT_CLOSED means detached. Does not acknowledge process
 * completion. */
enum call_status process_interrupt_take(handle_t receiver);

#endif
