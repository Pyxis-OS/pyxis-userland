#ifndef USERSPACE_HANDLE_H
#define USERSPACE_HANDLE_H

#include <abi/handle.h>
#include <abi/syscall.h>

/* Releases this process's reference, making the handle stale immediately.
 * Other owners keep the object alive. Returns zero on success, -1 on failure;
 * invalid and already-closed handles are errors. */
int handle_close(handle_t handle);

/* Queries this handle's granted, object-specific rights, including zero.
 * Does not expose object identity, allocate or change ownership. Failure clears
 * *rights. Possessing a handle is sufficient; no extra query right is needed. */
enum call_status handle_rights(handle_t handle, uint64_t *rights);

/* Both forms install a new owned handle in this process and preserve source.
 * copy keeps its rights; copy_restricted requests an exact subset (zero is
 * allowed). No transfer permission is needed. Failure clears *destination.
 * May block for BSP table growth, like directory lookup. */
enum call_status handle_copy(handle_t source, handle_t *destination);
enum call_status handle_copy_restricted(handle_t source, uint64_t rights,
                                         handle_t *destination);

#endif
