#ifndef USERSPACE_STARTUP_H
#define USERSPACE_STARTUP_H

#include <abi/handle.h>
#include <abi/startup.h>
#include <stdbool.h>
#include <stddef.h>

/* Runtime entry only, before main and before argv can be modified. */
bool startup_init(const struct startup_info *info);

/* Borrow the process-owned caller-bound environment service. */
handle_t startup_environment_handle(void);

/* Kernel-prepared logical CPU allowance at process startup, always nonzero.
 * No capability/query; this is neither global inventory nor a live affinity API. */
size_t startup_usable_cpu_count(void);

/* The chain runs from the permitted parent-navigation boundary to the current
 * directory. Out-of-range indexes return HANDLE_INVALID. The optional display
 * path describes the initial context only; changing directory does not update it.
 * The array accessor borrows read-only storage, returning NULL for an empty chain. */
const handle_t *startup_working_directories(void);
size_t startup_working_directory_count(void);
handle_t startup_working_directory(size_t index);
const char *startup_working_path(void);

#endif
