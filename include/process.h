#ifndef USERSPACE_PROCESS_H
#define USERSPACE_PROCESS_H

#include <abi/handle.h>
#include <abi/process.h>
#include <abi/syscall.h>

/* Requires WAIT authority. Blocks until execution resources have been reclaimed;
 * later calls return the same result. Does not consume or close the handle.
 * Preserves native statuses and clears *result on failure. The caller supplies
 * writable result storage. wait_many WAIT_COMPLETE observes readiness without
 * blocking other handles; the same WAIT right authorizes result retrieval. EXITED carries the signed exit code; FAULTED and
 * TERMINATED carry zero. Closing an observer does not terminate its target. */
enum call_status process_wait(handle_t process, struct process_result *result);

/* Requires TERMINATE authority; launch grants it with each child's observer.
 * Requests that the process stop and returns without waiting. Idempotent, and
 * successful after completion, which keeps an already committed exit or fault.
 * Affects only that process. Use process_wait or WAIT_COMPLETE for the result. */
enum call_status process_terminate(handle_t process);

#endif
