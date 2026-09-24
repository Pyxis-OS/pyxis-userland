#ifndef USERSPACE_MOUNT_H
#define USERSPACE_MOUNT_H

#include <abi/mount.h>
#include <abi/syscall.h>

/* Opens the selected export as a read-only directory. Success returns an
 * owned handle; failure clears it. Does not consume mount or bind a URI. */
enum call_status mount_open_root(handle_t mount, handle_t *root);

#endif
