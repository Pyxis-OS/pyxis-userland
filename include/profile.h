#ifndef USERSPACE_PROFILE_H
#define USERSPACE_PROFILE_H

#include <abi/handle.h>
#include <abi/profile.h>
#include <abi/syscall.h>

/* Caller-scoped collection; copying a handle never exposes another process.
 * Snapshot outputs are cleared on failure. See abi/profile.h for boundaries. */
enum call_status profile_begin(handle_t profile);
enum call_status profile_snapshot(handle_t profile, struct profile_snapshot *snapshot);
enum call_status profile_end(handle_t profile, struct profile_snapshot *snapshot);

#endif
