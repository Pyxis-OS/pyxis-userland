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

/* Synchronizes every native pool on disk-scoped authority, or the bound FAT
 * instance on FAT control. FAT sync does not clear its owned dirty mark.
 * Requires mount WRITE, borrows the handle, and supplies no disk selector.
 * Completion covers committed dirty data/metadata; close does not synchronize.
 * Failure may follow successful commits in other pools. Malformed replies are
 * outcome unknown, with no automatic retry. */
enum call_status mount_sync(handle_t mount);

/* Borrow the fixed boot-ESP issuer. No device/partition selector is accepted.
 * Rights are exact LOOKUP/ENUMERATE/READ_FILES bits. Success owns a root/control
 * pair; failure clears both. No URI binding or implicit mount is performed.
 * Malformed replies are outcome unknown; never retry uncertain admission
 * automatically. Closing either handle does not retire the instance. */
enum call_status mount_open_fat32(handle_t issuer, uint64_t rights,
    struct mount_fat32_reply *mount);

/* Reopen only the bound FAT32 instance under its original rights ceiling, with
 * exact DIRECTORY_CONTENT_RIGHTS bits. Mutation rights need control WRITE.
 * Success owns a root; failure clears it. Retired controls cannot reopen. */
enum call_status mount_reopen_fat32(handle_t control, uint64_t rights, handle_t *root);

/* Observe actual bound-instance state through OBSERVE. Failure preserves state.
 * A retired control remains an observable tombstone. IDENTITY_VALID supplies
 * retained admission GPT identity/whole-device geometry, including after failure
 * or retirement; malformed evidence returns OUTCOME_UNKNOWN. GUIDs use GPT/EFI
 * on-disk byte encoding. Evidence does not authenticate hardware, confer authority
 * or attest current media contents. */
enum call_status mount_observe(handle_t control, struct mount_state_reply *state);

/* Requires RETIRE. BUSY leaves the instance active while data objects, admitted
 * work or cleanup remain. OK completes retirement and releases exclusion;
 * repeating on a retired control is safe. Closing controls never retires. */
enum call_status mount_retire(handle_t control);

/* Requires INTERVAL. BEGIN owns a non-extending process-wide lease on success;
 * failure preserves *interval. Children do not share ownership. Other processes'
 * mutation/sync is BUSY while reads remain available. Malformed replies leave
 * admission uncertain; never retry automatically. END requires this owner's
 * nonzero identifier, including acknowledgment after expiry. Neither call syncs,
 * rolls back prior effects or changes data authority. */
enum call_status mount_begin(handle_t control, struct mount_interval_reply *interval);
enum call_status mount_end(handle_t control, uint64_t id);

#endif
