#ifndef USERSPACE_STARTUP_H
#define USERSPACE_STARTUP_H

#include <abi/handle.h>
#include <abi/startup.h>
#include <stdbool.h>
#include <stddef.h>

/* Runtime entry only, before main and before argv can be modified. */
bool startup_init(const struct startup_info *info);

/* Allocation-free lookups in the immutable startup snapshot. Names are case
 * sensitive; absent handles are HANDLE_INVALID and absent strings are NULL.
 * Handles are borrowed from this process's table, not duplicated. Bindings may
 * alias one handle; close each owned handle once. Closing does not edit the
 * snapshot, so later lookup can return that stale handle. Strings remain valid
 * until exit. These helpers are available when main begins. */
/* Borrow the dedicated handle owned by the corresponding libc descriptor.
 * The snapshot retains no reference. Never close it with handle_close; use
 * fclose on that stream. After owner close, do not use or forward the stale
 * handle. Descriptor-number reuse does not refresh this immutable snapshot.
 * An absent/out-of-range stream returns NONE/invalid. */
struct startup_stream startup_stream(enum startup_stream_index index);

handle_t startup_resource(const char *name);
handle_t startup_root(const char *scheme);
/* Borrow the dedicated namespace handle; absent means HANDLE_INVALID. */
handle_t startup_namespace(void);
const char *startup_environment(const char *name);

/* Borrowed immutable array for explicitly forwarding the initial environment. */
const struct startup_variable *startup_environment_variables(void);
size_t startup_environment_count(void);

/* The chain runs from the permitted parent-navigation boundary to the current
 * directory. Out-of-range indexes return HANDLE_INVALID. The optional display
 * path describes the initial context only; changing directory does not update it.
 * The array accessor borrows read-only storage, returning NULL for an empty chain. */
const handle_t *startup_working_directories(void);
size_t startup_working_directory_count(void);
handle_t startup_working_directory(size_t index);
const char *startup_working_path(void);

#endif
