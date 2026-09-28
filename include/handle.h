#ifndef USERSPACE_HANDLE_H
#define USERSPACE_HANDLE_H

#include <abi/handle.h>
#include <abi/syscall.h>

/* Releases this process's reference, making the handle stale immediately.
 * Other owners keep the object alive. Returns zero on success, -1 on failure;
 * invalid and already-closed handles are errors. */
int handle_close(handle_t handle);

/* Kernel-authenticated grant authority, interface and native/exported kind.
 * Failure clears the output. Querying a closed export still succeeds; subsequent
 * invocation reports closure. No object identity or extra authority is exposed. */
enum call_status handle_query(handle_t handle, struct handle_info *info);

/* Queries independently granted resource and transport authority. At least one
 * output is required; omitted outputs are ignored. Failure clears both supplied
 * outputs. Possessing the handle is sufficient. */
enum call_status handle_rights(handle_t handle, uint64_t *rights, uint64_t *transport);

/* Both forms install a new owned handle in this process and preserve source.
 * copy keeps both masks; copy_restricted requests exact subsets (zero is
 * allowed). No transfer permission is needed. Failure clears *destination.
 * May block for BSP table growth, like directory lookup. */
enum call_status handle_copy(handle_t source, handle_t *destination);
enum call_status handle_copy_restricted(handle_t source, uint64_t rights,
    uint64_t transport, handle_t *destination);

#endif
