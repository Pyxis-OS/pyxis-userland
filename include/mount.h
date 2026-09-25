#ifndef USERSPACE_MOUNT_H
#define USERSPACE_MOUNT_H

#include <abi/mount.h>
#include <abi/syscall.h>

/* Opens the selected export with MOUNT_ACCESS_READ_ONLY or READ_WRITE grants.
 * Success returns an owned handle; failure clears it. Does not consume mount
 * or bind a URI. Write grants do not certify backend/host writability. */
enum call_status mount_open_root(handle_t mount, uint64_t access, handle_t *root);

#endif
