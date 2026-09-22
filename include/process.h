#ifndef USERSPACE_PROCESS_H
#define USERSPACE_PROCESS_H

#include <abi/handle.h>
#include <abi/process.h>
#include <abi/syscall.h>

/* Requires WAIT authority. Blocks until execution resources have been reclaimed;
 * later calls return the same result. Does not consume or close the handle.
 * Preserves native statuses and clears *result on failure. The caller supplies
 * writable result storage. Closing an observer does not terminate its target. */
enum call_status process_wait(handle_t process, struct process_result *result);

#endif
