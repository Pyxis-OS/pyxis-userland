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

/* RAM FILE buffer replacement, independent of private-memory collection. */
enum call_status profile_file_begin(handle_t profile);
enum call_status profile_file_snapshot(handle_t profile, struct profile_file_snapshot *snapshot);
enum call_status profile_file_end(handle_t profile, struct profile_file_snapshot *snapshot);

/* Host FILE reads/writes, independent of memory and RAM FILE collection. */
enum call_status profile_host_begin(handle_t profile);
enum call_status profile_host_snapshot(handle_t profile, struct profile_host_snapshot *snapshot);
enum call_status profile_host_end(handle_t profile, struct profile_host_snapshot *snapshot);

#endif
