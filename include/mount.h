#ifndef USERSPACE_MOUNT_H
#define USERSPACE_MOUNT_H

#include <abi/mount.h>
#include <abi/syscall.h>

/* Opens the selected export with MOUNT_ACCESS_READ_ONLY or READ_WRITE grants.
 * Success returns an owned handle; failure clears it. Does not consume mount
 * or bind a URI. Write grants do not certify backend/host writability. */
enum call_status mount_open_root(handle_t mount, uint64_t access, handle_t *root);

/* Opens a native volume through disk-scoped authority. partition is one-based and
 * fits a GPT entry number (UINT32_MAX);
 * name is NUL-terminated and contains 1–255 bytes. rights is exact and must
 * include LOOKUP. FILESYSTEM_INFO additionally requires mount OBSERVE authority;
 * mutation rights additionally require mount WRITE authority. Success returns
 * an owned root, without consuming authority or binding a URI. Failure clears
 * root; malformed replies are outcome unknown. */
enum call_status mount_open_volume(handle_t mount, uint64_t partition,
    const char *name, uint64_t rights, handle_t *root);

/* Synchronizes every mounted native pool on this authority's configured disk.
 * Requires mount WRITE, borrows the handle, and supplies no disk selector.
 * Completion covers committed dirty data/metadata; close does not synchronize.
 * Failure may follow successful commits in other pools. Malformed replies are
 * outcome unknown, with no automatic retry. */
enum call_status mount_sync(handle_t mount);

#endif
