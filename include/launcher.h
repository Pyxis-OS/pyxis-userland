#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <abi/launcher.h>
#include <abi/syscall.h>

/* Explicit pointers/counts in request refer to caller storage, borrowed until
 * return. Native statuses; clears *child on failure. Success returns an owned
 * process-control handle with WAIT. Closing it does not terminate the child.
 * Source handles survive success and failure; no target-space/CPU parameter. */
enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child);

#endif
