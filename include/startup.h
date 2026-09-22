#ifndef USERSPACE_STARTUP_H
#define USERSPACE_STARTUP_H

#include <abi/handle.h>
#include <stddef.h>

/* Allocation-free lookups in the immutable startup snapshot. Names are case
 * sensitive; absent handles are HANDLE_INVALID and absent strings are NULL.
 * Handles are borrowed from this process's table, not duplicated. Bindings may
 * alias one handle; close each owned handle once. Closing does not edit the
 * snapshot, so later lookup can return that stale handle. Strings remain valid
 * until exit. These helpers are available when main begins. */
handle_t startup_resource(const char *name);
handle_t startup_root(const char *scheme);
const char *startup_environment(const char *name);

/* The chain runs from the permitted parent-navigation boundary to the current
 * directory. Out-of-range indexes return HANDLE_INVALID. The optional display
 * path is descriptive only. Currently boot programs have no directory context. */
size_t startup_working_directory_count(void);
handle_t startup_working_directory(size_t index);
const char *startup_working_path(void);

#endif
