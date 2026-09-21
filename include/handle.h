#ifndef USERSPACE_HANDLE_H
#define USERSPACE_HANDLE_H

#include <abi/handle.h>

/* Releases this process's reference, making the handle stale immediately.
 * Other owners keep the object alive. Returns zero on success, -1 on failure;
 * invalid and already-closed handles are errors. */
int handle_close(handle_t handle);

#endif
